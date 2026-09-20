/*
 * probe.c -- proof that a Linux syscall can be emulated on GNU Mach.
 *
 * THE EXPERIMENT THE WHOLE IDEA RESTS ON.
 *
 * Background: GNU Mach's own syscalls use `lcall $0x7` (a call gate) --
 * verified by disassembling libc: 43 call sites, zero `int $0x80`. So
 * interrupt 0x80, which is how 32-bit Linux binaries enter the kernel, is
 * completely unclaimed on Hurd and faults instead.
 *
 * This program:
 *   1. installs a Mach exception port on its own task
 *   2. executes `int $0x80` with Linux's write(2) convention in the registers
 *      (eax=4, ebx=fd, ecx=buf, edx=len)
 *   3. catches the resulting exception
 *   4. reads the faulting thread's registers with thread_get_state()
 *   5. performs the requested operation
 *   6. writes the return value into EAX, steps EIP past the 2-byte int $0x80
 *   7. resumes the thread
 *
 * If the message appears and execution continues, that is a complete Linux
 * syscall emulation cycle, and a real personality server is then a matter of
 * implementing more syscalls and an ELF loader.
 *
 * GNU Mach uses the ORIGINAL Mach 3 exception interface -- task_set_exception_port,
 * no EXC_MASK_*, no behaviors. Exceptions arrive as exception_raise (msgh_id
 * 2401) and are demuxed by the MIG-generated glue in <mach/exc_server.h>,
 * which calls our _S_catch_exception_raise.
 *
 * Build:  gcc -Wall -o probe probe.c -lpthread
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include <mach.h>
#include <mach/mach_types.h>
#include <mach/exception.h>
#include <mach/message.h>
#include <mach/task_special_ports.h>
#include <mach/i386/thread_status.h>
#include <mach/exc_server.h>

#ifndef i386_THREAD_STATE_COUNT
#define i386_THREAD_STATE_COUNT \
    (sizeof(struct i386_thread_state) / sizeof(natural_t))
#endif

/*
 * CRITICAL: use i386_REGS_SEGS_STATE, not i386_THREAD_STATE.
 *
 * Both flavors use `struct i386_thread_state`, but thread_set_state() with
 * i386_THREAD_STATE does NOT honour the segment fields -- GNU Mach resets
 * %gs from 0x4b (Hurd's LDT selector for thread-local storage) to the plain
 * data selector 0x1f. The resumed thread then dies on its next libc call,
 * because stack-protector code reads the canary from %gs:0x14.
 *
 * Measured: gs=0x004b before the call, gs=0x001f after.
 */
#define ORYX_STATE_FLAVOR  i386_REGS_SEGS_STATE
#define ORYX_STATE_COUNT   i386_THREAD_STATE_COUNT

static mach_port_t exc_port;
static volatile int handled = 0;

static const char *exc_name(int e)
{
    switch (e) {
    case EXC_BAD_ACCESS:      return "EXC_BAD_ACCESS";
    case EXC_BAD_INSTRUCTION: return "EXC_BAD_INSTRUCTION";
    case EXC_ARITHMETIC:      return "EXC_ARITHMETIC";
    case EXC_EMULATION:       return "EXC_EMULATION";
    case EXC_SOFTWARE:        return "EXC_SOFTWARE";
    case EXC_BREAKPOINT:      return "EXC_BREAKPOINT";
    default:                  return "unknown";
    }
}

/*
 * Called by the MIG glue for each exception_raise. Returning KERN_SUCCESS
 * tells Mach the exception was handled and the thread should continue.
 */
kern_return_t _S_catch_exception_raise(mach_port_t port,
                                       mach_port_t thread,
                                       mach_port_t task,
                                       integer_t exception,
                                       integer_t code,
                                       rpc_long_integer_t subcode)
{
    struct i386_thread_state st;
    mach_msg_type_number_t count = ORYX_STATE_COUNT;
    kern_return_t kr;

    (void) port; (void) task;

    printf("\n[!] exception %d (%s)  code=%d subcode=%ld\n",
           (int) exception, exc_name((int) exception),
           (int) code, (long) subcode);

    kr = thread_get_state(thread, ORYX_STATE_FLAVOR,
                          (thread_state_t) &st, &count);
    if (kr != KERN_SUCCESS) {
        printf("    thread_get_state failed: %d\n", (int) kr);
        return kr;
    }

    /* Confirm EIP really points at the faulting `cd 80`. */
    unsigned char *insn = (unsigned char *) st.eip;
    printf("    eip=0x%08x  bytes at eip: %02x %02x  %s\n",
           st.eip, insn[0], insn[1],
           (insn[0] == 0xcd && insn[1] == 0x80) ? "<- int $0x80, as expected"
                                                : "<- NOT int $0x80");
    printf("    linux syscall regs: eax=%u ebx=%u ecx=0x%08x edx=%u\n",
           st.eax, st.ebx, st.ecx, st.edx);

    /*
     * Only emulate if the faulting instruction really is `int $0x80`.
     * Anything else is a genuine fault -- decline it. Advancing EIP blindly
     * turns one unrelated fault into an endless walk through garbage, which
     * is exactly what happened before this check existed. Note that taking
     * the task exception port also intercepts the faults glibc would
     * normally turn into POSIX signals, so real bugs arrive here too.
     */
    if (!(insn[0] == 0xcd && insn[1] == 0x80)) {
        printf("    NOT a linux syscall -- declining (genuine fault)\n");
        return KERN_FAILURE;
    }

    long ret = -38;   /* -ENOSYS */

    switch (st.eax) {
    case 4:   /* Linux i386: write(fd, buf, count) */
        printf("    -> emulating write(%u, 0x%08x, %u)\n",
               st.ebx, st.ecx, st.edx);
        fflush(stdout);
        ret = write((int) st.ebx, (const void *) st.ecx, (size_t) st.edx);
        break;
    case 1:   /* Linux i386: exit(status) */
        printf("    -> emulating exit(%u)\n", st.ebx);
        exit((int) st.ebx);
    default:
        printf("    -> unimplemented linux syscall %u, returning -ENOSYS\n",
               st.eax);
        break;
    }

    /* Return value in EAX, and step over the two-byte int $0x80. */
    st.eax = (unsigned int) ret;
    st.eip += 2;

    kr = thread_set_state(thread, ORYX_STATE_FLAVOR,
                          (thread_state_t) &st, ORYX_STATE_COUNT);
    if (kr != KERN_SUCCESS) {
        printf("    thread_set_state failed: %d\n", (int) kr);
        return kr;
    }

    /* Did the segment registers survive the round trip? Hurd keeps thread-local
       storage behind %gs, and the resumed code reads the stack canary from
       %gs:0x14 -- if %gs changes, the very next libc call dies. */
    struct i386_thread_state chk;
    mach_msg_type_number_t ccount = ORYX_STATE_COUNT;
    if (thread_get_state(thread, ORYX_STATE_FLAVOR,
                         (thread_state_t) &chk, &ccount) == KERN_SUCCESS) {
        printf("    segs before: gs=0x%04x fs=0x%04x ds=0x%04x es=0x%04x cs=0x%04x ss? efl=0x%08x\n",
               st.gs, st.fs, st.ds, st.es, st.cs, st.efl);
        printf("    segs after : gs=0x%04x fs=0x%04x ds=0x%04x es=0x%04x cs=0x%04x efl=0x%08x\n",
               chk.gs, chk.fs, chk.ds, chk.es, chk.cs, chk.efl);
        printf("    eip after=0x%08x eax after=%u  %s\n", chk.eip, chk.eax,
               chk.gs == st.gs ? "(gs preserved)" : "(!!! gs CHANGED !!!)");
    }
    printf("    thread_set_state OK; resuming at eip=0x%08x with eax=%ld\n",
           st.eip, ret);
    handled = 1;
    return KERN_SUCCESS;
}

static void *handler(void *arg)
{
    (void) arg;
    union { mach_msg_header_t hdr; char buf[4096]; } in, out;

    for (;;) {
        kern_return_t kr = mach_msg(&in.hdr, MACH_RCV_MSG, 0, sizeof in,
                                    exc_port, MACH_MSG_TIMEOUT_NONE,
                                    MACH_PORT_NULL);
        if (kr != KERN_SUCCESS) {
            printf("[!] receive failed: %d\n", (int) kr);
            return NULL;
        }

        printf("[*] msg in: id=%d bits=0x%08x remote=%u local=%u size=%u\n",
               (int) in.hdr.msgh_id, (unsigned) in.hdr.msgh_bits,
               (unsigned) in.hdr.msgh_remote_port,
               (unsigned) in.hdr.msgh_local_port,
               (unsigned) in.hdr.msgh_size);
        mach_port_t reply_to = in.hdr.msgh_remote_port;

        mig_routine_t routine = _S_exc_server_routine(&in.hdr);
        if (!routine) {
            printf("[!] unexpected message id %d\n", (int) in.hdr.msgh_id);
            continue;
        }
        memset(&out, 0, sizeof out);
        routine(&in.hdr, &out.hdr);

        /*
         * GNU Mach's MIG glue here does not fill in the reply header, so we
         * build it by hand. A reply is mandatory: the kernel is blocked
         * waiting for it, and without one the faulting thread never resumes.
         *
         * Layout is mig_reply_header_t -- header, a type descriptor for the
         * return code, then the code itself. The descriptor must be filled
         * in; a zeroed one is rejected and the send fails.
         */
        mig_reply_header_t *r = (mig_reply_header_t *) &out.hdr;
        memset(r, 0, sizeof *r);
        r->Head.msgh_bits        = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
        r->Head.msgh_size        = sizeof *r;
        r->Head.msgh_remote_port = reply_to;
        r->Head.msgh_local_port  = MACH_PORT_NULL;
        r->Head.msgh_seqno       = 0;
        r->Head.msgh_id          = in.hdr.msgh_id + 100;   /* 2400 -> 2500 */
        r->RetCodeType.msgt_name       = MACH_MSG_TYPE_INTEGER_32;
        r->RetCodeType.msgt_size       = 32;
        r->RetCodeType.msgt_number     = 1;
        r->RetCodeType.msgt_inline     = TRUE;
        r->RetCodeType.msgt_longform   = FALSE;
        r->RetCodeType.msgt_deallocate = FALSE;
        r->RetCode = KERN_SUCCESS;   /* handled -- resume the thread */

        kr = mach_msg(&r->Head, MACH_SEND_MSG, sizeof *r, 0,
                      MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        printf("[*] handler: reply sent to port %u, kr=%d %s\n",
               (unsigned) reply_to, (int) kr,
               kr == KERN_SUCCESS ? "(thread should resume)" : "(FAILED)");
    }
    return NULL;
}

int main(void)
{
    kern_return_t kr;
    pthread_t th;

    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: stdout may be a pipe */
    printf("Oryx -- Linux syscall emulation probe\n");
    printf("=====================================\n");
    printf("GNU Mach uses `lcall $0x7`; int $0x80 is unclaimed and faults.\n\n");

    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &exc_port);
    if (kr) { printf("port_allocate: %d\n", (int) kr); return 1; }
    kr = mach_port_insert_right(mach_task_self(), exc_port, exc_port,
                                MACH_MSG_TYPE_MAKE_SEND);
    if (kr) { printf("insert_right: %d\n", (int) kr); return 1; }

    if (pthread_create(&th, NULL, handler, NULL) != 0) {
        printf("pthread_create failed\n"); return 1;
    }
    sleep(1);

    kr = task_set_exception_port(mach_task_self(), exc_port);
    if (kr) { printf("[!] task_set_exception_port: %d\n", (int) kr); return 1; }
    printf("[*] exception port installed on our task\n");
    printf("[*] issuing int $0x80 with eax=4 (linux write)\n");
    fflush(stdout);

    const char msg[] = "    >>> HELLO FROM A LINUX SYSCALL ON GNU/HURD <<<\n";
    long result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a" (result)
        : "a" (4), "b" (1), "c" (msg), "d" (sizeof msg - 1)
        : "memory");

    printf("\n[+] execution resumed. syscall returned %ld\n", result);
    printf("[+] handled=%d\n", handled);
    if (handled && result == (long)(sizeof msg - 1))
        printf("\n*** COMPLETE LINUX SYSCALL EMULATION CYCLE ON GNU MACH ***\n");
    return 0;
}
