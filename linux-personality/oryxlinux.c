/*
 * oryxlinux.c -- a Linux personality for GNU/Hurd.
 *
 * Runs a statically linked 32-bit Linux binary on GNU Mach by loading it into
 * a task of its own and servicing its `int $0x80` syscalls from outside.
 *
 * This is the step after probe.c. probe.c proved one syscall could be trapped
 * and serviced inside our own process; this loads a real ELF into a REAL
 * separate task and runs it.
 *
 *   1. task_create() an empty address space -- no Hurd libc in it at all
 *   2. map the ELF's PT_LOAD segments into it with vm_allocate/vm_write
 *   3. build a Linux initial stack (argc, argv, envp, auxv)
 *   4. take the task's exception port and start a thread at e_entry
 *   5. every `int $0x80` arrives here as EXC_BAD_INSTRUCTION; decode eax,
 *      perform the call against the child's memory, set eax, step eip by 2
 *
 * Why a fresh task rather than fork():
 *   A forked child still has Hurd's glibc, its TLS and its %gs selector
 *   mapped, and a Linux binary wants to lay out that address space itself --
 *   including loading at 0x08048000 and running its own TLS setup. An empty
 *   task sidesteps every one of those collisions. It also means a crash in
 *   the guest cannot corrupt the emulator.
 *
 * WHAT IS PROVEN AND WHAT IS NOT: see README.md. Static binaries only; no
 * ld-linux.so, no threads, no signals.
 *
 * Build on Oryx:  gcc -Wall -O0 -o oryxlinux oryxlinux.c
 * Run:            ./oryxlinux [-v] <static-linux-i386-binary> [args...]
 */
#define _GNU_SOURCE
/* Hurd's off_t is 32 bits by default, which cannot express what _llseek and
   mmap2 pass around. Ask for the 64-bit variants before any header. */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <elf.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <limits.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <termios.h>

#include <mach.h>
#include <mach/exception.h>
#include <mach/message.h>
#include <mach/task_special_ports.h>
#include <mach/i386/thread_status.h>
#include <mach/exc_server.h>
#include <mach/vm_param.h>
#include <mach_error.h>
#include <mach/i386/mach_i386.h>

#ifndef i386_THREAD_STATE_COUNT
#define i386_THREAD_STATE_COUNT \
    (sizeof(struct i386_thread_state) / sizeof(natural_t))
#endif

/* i386_THREAD_STATE silently resets %gs; see probe.c. Always this flavor. */
#define STATE_FLAVOR  i386_REGS_SEGS_STATE
#define STATE_COUNT   i386_THREAD_STATE_COUNT

/* Where the packaged Linux loader and libraries live. */
#ifndef DEFAULT_SYSROOT
#define DEFAULT_SYSROOT "/usr/lib/oryx-linux/sysroot"
#endif

#define PAGE_DOWN(x)  ((x) & ~(vm_address_t)(vm_page_size - 1))
#define PAGE_UP(x)    PAGE_DOWN((x) + vm_page_size - 1)

/* ------------------------------------------------------------------ *
 *  Linux i386 ABI constants. Ours, not the host's -- the host's differ.
 * ------------------------------------------------------------------ */
#define LINUX_EPERM 1
#define LINUX_ENOENT 2
#define LINUX_ESRCH 3
#define LINUX_EINTR 4
#define LINUX_EIO 5
#define LINUX_ENXIO 6
#define LINUX_E2BIG 7
#define LINUX_ENOEXEC 8
#define LINUX_EBADF 9
#define LINUX_ECHILD 10
#define LINUX_EAGAIN 11
#define LINUX_ENOMEM 12
#define LINUX_EACCES 13
#define LINUX_EFAULT 14
#define LINUX_EBUSY 16
#define LINUX_EEXIST 17
#define LINUX_EXDEV 18
#define LINUX_ENODEV 19
#define LINUX_ENOTDIR 20
#define LINUX_EISDIR 21
#define LINUX_EINVAL 22
#define LINUX_ENFILE 23
#define LINUX_EMFILE 24
#define LINUX_ENOTTY 25
#define LINUX_EFBIG 27
#define LINUX_ENOSPC 28
#define LINUX_ESPIPE 29
#define LINUX_EROFS 30
#define LINUX_EMLINK 31
#define LINUX_EPIPE 32
#define LINUX_ERANGE 34
#define LINUX_ENAMETOOLONG 36
#define LINUX_ENOSYS 38
#define LINUX_ENOTEMPTY 39
#define LINUX_ELOOP 40

#define LINUX_AT_NULL 0
#define LINUX_AT_PHDR 3
#define LINUX_AT_PHENT 4
#define LINUX_AT_PHNUM 5
#define LINUX_AT_PAGESZ 6
#define LINUX_AT_BASE 7
#define LINUX_AT_FLAGS 8
#define LINUX_AT_ENTRY 9
#define LINUX_AT_UID 11
#define LINUX_AT_EUID 12
#define LINUX_AT_GID 13
#define LINUX_AT_EGID 14
#define LINUX_AT_PLATFORM 15
#define LINUX_AT_HWCAP 16
#define LINUX_AT_CLKTCK 17
#define LINUX_AT_SECURE 23
#define LINUX_AT_RANDOM 25

/*
 * Hurd's errno values are not Linux's -- they are Mach error codes with a
 * subsystem in the high bits, so EINVAL is 0x40000016, not 22. Returning
 * one raw would have the guest's libc set errno to nonsense. Map by NAME:
 * the host macro gives the Hurd value, the literal is what Linux uses.
 */
static int errno_to_linux(int e)
{
    switch (e) {
    case EPERM: return LINUX_EPERM;
    case ENOENT: return LINUX_ENOENT;
    case ESRCH: return LINUX_ESRCH;
    case EINTR: return LINUX_EINTR;
    case EIO: return LINUX_EIO;
    case ENXIO: return LINUX_ENXIO;
    case E2BIG: return LINUX_E2BIG;
    case ENOEXEC: return LINUX_ENOEXEC;
    case EBADF: return LINUX_EBADF;
    case ECHILD: return LINUX_ECHILD;
    case EAGAIN: return LINUX_EAGAIN;
    case ENOMEM: return LINUX_ENOMEM;
    case EACCES: return LINUX_EACCES;
    case EFAULT: return LINUX_EFAULT;
    case EBUSY: return LINUX_EBUSY;
    case EEXIST: return LINUX_EEXIST;
    case EXDEV: return LINUX_EXDEV;
    case ENODEV: return LINUX_ENODEV;
    case ENOTDIR: return LINUX_ENOTDIR;
    case EISDIR: return LINUX_EISDIR;
    case EINVAL: return LINUX_EINVAL;
    case ENFILE: return LINUX_ENFILE;
    case EMFILE: return LINUX_EMFILE;
    case ENOTTY: return LINUX_ENOTTY;
    case EFBIG: return LINUX_EFBIG;
    case ENOSPC: return LINUX_ENOSPC;
    case ESPIPE: return LINUX_ESPIPE;
    case EROFS: return LINUX_EROFS;
    case EMLINK: return LINUX_EMLINK;
    case EPIPE: return LINUX_EPIPE;
    case ERANGE: return LINUX_ERANGE;
    case ENAMETOOLONG: return LINUX_ENAMETOOLONG;
    case ENOTEMPTY: return LINUX_ENOTEMPTY;
    case ELOOP: return LINUX_ELOOP;
    default: return LINUX_EINVAL;
    }
}

/* ------------------------------------------------------------------ */

static mach_port_t exc_port;
static int         verbose;
static int         guest_status = -1;
static volatile int guest_done;
static unsigned long syscall_count;

/*
 * The process table.
 *
 * Every guest process is a Mach task sharing one exception port, so all of
 * their syscalls arrive on the same port and are served by one loop. The
 * exception message carries the task and thread it came from, so `cur` is
 * set from that on entry and everything below works against the process that
 * actually trapped.
 *
 * child_task/child_thread stay as macros over `cur` so the memory helpers,
 * the loader and the syscall table did not have to change when this stopped
 * being a single-process emulator.
 */
#define MAX_PROC 64
#define MAX_FD   256

struct proc {
    int          used, alive, reaped;
    int          pid, ppid, status;
    task_t       task;
    thread_t     thread;
    vm_address_t brk_lo, brk_hi;     /* named so the macros below cannot
                                        mangle `p->brk_start` at a use site */
    int          tls_sel;            /* GDT selector from set_thread_area */

    /* Guest fd -> host fd. Linux gives each process its OWN descriptor
       table: fork copies it, and a close in the child must not disturb the
       parent. Sharing one table across every guest process breaks command
       substitution with "dup2(4,1): Bad file descriptor", because the
       parent's close takes the descriptor away before the child runs. */
    int          fd[MAX_FD];

    /* A wait4 that could not be answered yet. The reply to the exception is
       withheld -- which is what keeps the caller blocked -- and sent once a
       child exits. The server itself must never block: the child still needs
       its own syscalls serviced by this same loop. */
    int          waiting;
    int          wait_for;           /* pid, or -1 for any */
    mach_port_t  wait_reply;
    vm_address_t wait_status_addr;
};

static struct proc procs[MAX_PROC];
static struct proc *root;            /* the process we were asked to run */
static int next_pid = 100;

/*
 * The server is multi-threaded, and it has to be.
 *
 * A guest pipeline has one process read()ing a pipe the other has not
 * written yet. With a single server thread that read blocks the whole
 * emulator, the writer never gets serviced, and `a | b` deadlocks forever.
 * Several threads receive from the same exception port, so one parked in a
 * blocking host call does not stop the others.
 *
 * `cur` is therefore per-thread: each worker is serving a different guest
 * process. Everything genuinely shared -- the process table, pid allocation,
 * the directory cache -- is covered by one lock, held for the whole of a
 * syscall EXCEPT around host calls that can block. Those are the only places
 * concurrency is needed, and keeping that window small is what makes a
 * coarse lock safe here.
 */
static __thread struct proc *cur;    /* whose exception THIS thread handles */
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&mtx)
#define UNLOCK() pthread_mutex_unlock(&mtx)

/* Run a host call that may block without holding the lock. */
#define UNLOCKED(expr) ({ UNLOCK(); __typeof__(expr) _r = (expr); LOCK(); _r; })

#define child_task   (cur->task)
#define child_thread (cur->thread)
#define brk_start    (cur->brk_lo)
#define brk_cur      (cur->brk_hi)

/* Set in serve() before dispatch so wait4 can withhold it. */
static __thread mach_port_t cur_reply;
static __thread int         defer_reply;

static struct proc *proc_new(void)
{
    for (int i = 0; i < MAX_PROC; i++)
        if (!procs[i].used) {
            memset(&procs[i], 0, sizeof procs[i]);
            procs[i].used = procs[i].alive = 1;
            procs[i].pid = next_pid++;
            procs[i].tls_sel = -1;
            return &procs[i];
        }
    return NULL;
}

static struct proc *proc_by_task(task_t t)
{
    for (int i = 0; i < MAX_PROC; i++)
        if (procs[i].used && procs[i].task == t)
            return &procs[i];
    return NULL;
}

/* Guest descriptor -> host descriptor, or -1 if the guest has it closed. */
static int hfd(unsigned gfd)
{
    if (gfd >= MAX_FD) return -1;
    return cur->fd[gfd];
}

/* Lowest free guest descriptor, as Linux promises. */
static int gfd_alloc(int host)
{
    if (host < 0) return -1;
    for (int i = 0; i < MAX_FD; i++)
        if (cur->fd[i] < 0) { cur->fd[i] = host; return i; }
    close(host);
    return -1;
}

static void fd_init(struct proc *p)
{
    for (int i = 0; i < MAX_FD; i++) p->fd[i] = -1;
    p->fd[0] = 0; p->fd[1] = 1; p->fd[2] = 2;
}

/* fork: copy the table, dup()ing so the two sides close independently while
   still sharing the underlying file description (and therefore its offset). */
static void fd_fork(struct proc *child, struct proc *parent)
{
    for (int i = 0; i < MAX_FD; i++) {
        if (parent->fd[i] < 0) { child->fd[i] = -1; continue; }
        child->fd[i] = (i <= 2 && parent->fd[i] == i) ? i : dup(parent->fd[i]);
    }
}

/*
 * Close everything a process held, on exit.
 *
 * The guard is on the HOST descriptor, not the guest index. Skipping guest
 * fds 0-2 looks right and is wrong: after `cmd > file` or a pipeline, the
 * child's fd 1 IS the pipe's write end, and leaving it open means the reader
 * never sees EOF. `echo x | cat` then hangs forever with the writer already
 * exited. Host 0/1/2 are the emulator's own stdio, shared by every process,
 * and those are the only ones that must survive.
 */
static void fd_closeall(struct proc *p)
{
    for (int i = 0; i < MAX_FD; i++) {
        if (p->fd[i] > 2) close(p->fd[i]);
        p->fd[i] = -1;
    }
}

/*
 * Guest filesystem root for Linux-only paths.
 *
 * A dynamic Linux binary asks for /lib/ld-linux.so.2 and
 * /lib/i386-linux-gnu/libc.so.6. Hurd has neither -- its own loader is
 * /lib/ld.so and its libraries live in /lib/i386-gnu -- so the paths are
 * disjoint and could simply be created on the host. A sysroot is better:
 * the Linux userland stays in one directory instead of being scattered
 * through the real /lib, and nothing on Hurd can pick it up by accident.
 *
 * Resolution tries the sysroot first and falls back to the real path, so
 * /etc/hostname and friends still work while libraries come from the
 * sysroot.
 */
static const char *sysroot;

static const char *resolve(const char *guest, char *buf, size_t n)
{
    if (!sysroot || guest[0] != '/')
        return guest;
    snprintf(buf, n, "%s%s", sysroot, guest);
    return access(buf, F_OK) == 0 ? buf : guest;
}

#define LOG(...) do { if (verbose) { fprintf(stderr, "[oryx] " __VA_ARGS__); } } while (0)

/* ---------------- child memory access ----------------------------- *
 * The guest lives in another task, so nothing can be dereferenced
 * directly -- every byte goes through vm_read/vm_write.
 * ------------------------------------------------------------------ */

/*
 * GNU Mach has no vm_read_overwrite -- only vm_read, which allocates the
 * result in OUR address space and wants a page-aligned range. So: widen to
 * whole pages, copy the slice out, give the mapping back. Forgetting the
 * vm_deallocate leaks an entire page per syscall, which a busy guest turns
 * into hundreds of megabytes.
 */
static int peek(vm_address_t addr, void *buf, size_t len)
{
    if (!len) return 0;
    vm_address_t p0 = PAGE_DOWN(addr), p1 = PAGE_UP(addr + len);
    vm_offset_t data = 0;
    mach_msg_type_number_t cnt = 0;

    if (vm_read(child_task, p0, p1 - p0, &data, &cnt) != KERN_SUCCESS)
        return -1;
    if (cnt < (addr - p0) + len) { vm_deallocate(mach_task_self(), data, cnt); return -1; }
    memcpy(buf, (const char *) data + (addr - p0), len);
    vm_deallocate(mach_task_self(), data, cnt);
    return 0;
}

/*
 * Write into the guest.
 *
 * vm_write demands that BOTH the destination address and the length be page
 * aligned -- an unaligned call fails outright, which is what "vm_write of
 * segment 0 failed" was: ELF segments start at 0x08048000 + an offset and
 * are never a whole number of pages. So this is a read-modify-write of the
 * enclosing pages, which also transparently handles writing into a mapping
 * that is currently read-only.
 */
static int poke(vm_address_t addr, const void *buf, size_t len)
{
    if (len == 0)
        return 0;
    vm_address_t p0 = PAGE_DOWN(addr), p1 = PAGE_UP(addr + len);
    size_t span = p1 - p0;

    /* The SOURCE buffer must be page-aligned too: vm_write passes it as
       out-of-line memory, and an unaligned source is rejected the same way
       an unaligned destination is. malloc() gives no such guarantee. */
    char *page = NULL;
    if (posix_memalign((void **) &page, vm_page_size, span) != 0 || !page)
        return -1;
    memset(page, 0, span);

    /* Preserve whatever else shares the first and last page. A failed read
       means the range is not mapped yet (a fresh vm_allocate), where zeros
       are the correct starting point. */
    vm_offset_t data = 0;
    mach_msg_type_number_t cnt = 0;
    if (vm_read(child_task, p0, span, &data, &cnt) == KERN_SUCCESS) {
        memcpy(page, (const char *) data, cnt < span ? cnt : span);
        vm_deallocate(mach_task_self(), data, cnt);
    }

    memcpy(page + (addr - p0), buf, len);

    kern_return_t kp = vm_protect(child_task, p0, span, FALSE,
                                  VM_PROT_READ | VM_PROT_WRITE);
    kern_return_t kr = vm_write(child_task, p0, (vm_address_t) page, span);
    if (kr != KERN_SUCCESS)
        LOG("poke %#x+%zu: vm_protect=%d vm_write=%s\n",
            (unsigned) addr, len, (int) kp, mach_error_string(kr));
    free(page);
    return kr == KERN_SUCCESS ? 0 : -1;
}

/*
 * Read a NUL-terminated string out of the guest, a page at a time.
 *
 * Page granularity matters in both directions: reading byte-by-byte would
 * cost a whole vm_read per character, and reading `max` bytes in one go
 * would run off the end of the mapping whenever a string sits near the top
 * of a page and fail a perfectly valid path.
 */
static int peek_str(vm_address_t addr, char *buf, size_t max)
{
    size_t got = 0;
    while (got < max - 1) {
        vm_address_t at = addr + got;
        size_t chunk = vm_page_size - (at & (vm_page_size - 1));
        if (chunk > max - 1 - got)
            chunk = max - 1 - got;
        if (peek(at, buf + got, chunk) < 0)
            return got ? 0 : -1;
        if (memchr(buf + got, '\0', chunk))
            return 0;
        got += chunk;
    }
    buf[max - 1] = '\0';
    return 0;
}

/* ---------------- ELF loading ------------------------------------- */

struct loaded {
    Elf32_Addr   entry;        /* absolute, bias already applied */
    Elf32_Addr   phdr;         /* absolute address of the program headers */
    Elf32_Half   phentsize, phnum;
    vm_address_t brk;          /* end of the highest segment */
    vm_address_t base;         /* load bias (0 for ET_EXEC) */
    vm_address_t interp_base;  /* where ld.so went, for AT_BASE */
    char         interp[256];  /* PT_INTERP path, "" if static */
};

/*
 * Load bias for position-independent objects.
 *
 * Linux puts an i386 PIE around 0x56555000 and the interpreter elsewhere;
 * the exact values do not matter, only that the two do not overlap each
 * other, the 8 MB stack below STACK_TOP, or the heap that grows up from the
 * executable. These two are chosen to leave a wide gap for all three.
 */
#define PIE_BASE     0x56555000u
#define INTERP_BASE  0x40000000u

/*
 * Load one ELF object at `bias`. Used for both the executable and the
 * interpreter, which is the whole reason it takes a bias at all: ld.so is
 * itself an ET_DYN and has to land somewhere the executable is not.
 */
static int load_one(const char *path, vm_address_t bias, struct loaded *out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return -1; }

    struct stat stbuf;
    if (fstat(fd, &stbuf) < 0) { perror("fstat"); close(fd); return -1; }
    size_t fsize = stbuf.st_size;
    unsigned char *img = malloc(fsize);
    if (!img || (size_t) read(fd, img, fsize) != fsize) {
        fprintf(stderr, "cannot read %s\n", path); close(fd); free(img);
        return -1;
    }
    close(fd);

    Elf32_Ehdr *eh = (Elf32_Ehdr *) img;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
        eh->e_ident[EI_CLASS] != ELFCLASS32 ||
        eh->e_ident[EI_DATA] != ELFDATA2LSB ||
        eh->e_machine != EM_386) {
        fprintf(stderr, "%s: not a 32-bit little-endian i386 ELF\n", path);
        free(img); return -1;
    }
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) {
        fprintf(stderr, "%s: e_type %d is neither ET_EXEC nor ET_DYN\n",
                path, eh->e_type);
        free(img); return -1;
    }
    /* An ET_EXEC states its own addresses; only ET_DYN may be moved. */
    if (eh->e_type == ET_EXEC)
        bias = 0;

    Elf32_Phdr *ph = (Elf32_Phdr *) (img + eh->e_phoff);
    vm_address_t highest = 0;
    int mapped = 0;
    memset(out->interp, 0, sizeof out->interp);

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_INTERP) {
            size_t n = ph[i].p_filesz;
            if (n >= sizeof out->interp) n = sizeof out->interp - 1;
            memcpy(out->interp, img + ph[i].p_offset, n);
            out->interp[n] = '\0';
            continue;
        }
        if (ph[i].p_type != PT_LOAD)
            continue;

        vm_address_t vaddr  = bias + ph[i].p_vaddr;
        vm_address_t vstart = PAGE_DOWN(vaddr);
        vm_address_t vend   = PAGE_UP(vaddr + ph[i].p_memsz);

        vm_address_t a = vstart;
        kern_return_t kr = vm_allocate(child_task, &a, vend - vstart, FALSE);
        if (kr != KERN_SUCCESS && kr != KERN_NO_SPACE) {
            fprintf(stderr, "vm_allocate %#x+%#x: %s\n", (unsigned) vstart,
                    (unsigned)(vend - vstart), mach_error_string(kr));
            free(img); return -1;
        }
        /* KERN_NO_SPACE just means an earlier segment already claimed the
           page they share at the boundary, which is normal. */

        if (ph[i].p_filesz &&
            poke(vaddr, img + ph[i].p_offset, ph[i].p_filesz) < 0) {
            fprintf(stderr, "writing segment %d of %s failed\n", i, path);
            free(img); return -1;
        }

        vm_prot_t prot = 0;
        if (ph[i].p_flags & PF_R) prot |= VM_PROT_READ;
        if (ph[i].p_flags & PF_W) prot |= VM_PROT_WRITE;
        if (ph[i].p_flags & PF_X) prot |= VM_PROT_EXECUTE;
        vm_protect(child_task, vstart, vend - vstart, FALSE, prot);

        LOG("  %s seg %d: %#08x-%#08x %c%c%c\n", path, i, (unsigned) vaddr,
            (unsigned)(vaddr + ph[i].p_memsz),
            (prot & VM_PROT_READ) ? 'r' : '-',
            (prot & VM_PROT_WRITE) ? 'w' : '-',
            (prot & VM_PROT_EXECUTE) ? 'x' : '-');
        if (vend > highest) highest = vend;
        mapped++;
    }
    if (!mapped) {
        fprintf(stderr, "%s: no PT_LOAD segments\n", path);
        free(img); return -1;
    }

    out->phdr = 0;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD &&
            eh->e_phoff >= ph[i].p_offset &&
            eh->e_phoff < ph[i].p_offset + ph[i].p_filesz) {
            out->phdr = bias + ph[i].p_vaddr + (eh->e_phoff - ph[i].p_offset);
            break;
        }
    out->entry = bias + eh->e_entry;
    out->phentsize = eh->e_phentsize;
    out->phnum = eh->e_phnum;
    out->brk = highest;
    out->base = bias;
    free(img);
    return 0;
}

/* ---------------- the initial stack ------------------------------- *
 * Linux hands a fresh process:
 *   esp -> argc, argv[], NULL, envp[], NULL, auxv[], AT_NULL, then strings.
 * glibc's static startup reads all of it, and will crash without AT_RANDOM
 * (it seeds the stack guard from those 16 bytes).
 * ------------------------------------------------------------------ */

#define STACK_TOP   0xbf000000u
#define STACK_SIZE  (8u * 1024 * 1024)

static int build_stack(int argc, char **argv, char **envp,
                       const struct loaded *ld, vm_address_t *esp_out)
{
    vm_address_t base = STACK_TOP - STACK_SIZE;
    kern_return_t kr = vm_allocate(child_task, &base, STACK_SIZE, FALSE);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "stack vm_allocate: %s\n", mach_error_string(kr));
        return -1;
    }
    vm_protect(child_task, base, STACK_SIZE, FALSE,
               VM_PROT_READ | VM_PROT_WRITE);

    int envc = 0;
    while (envp[envc]) envc++;

    /* Build the whole block locally, then write it once. Much easier to get
       right than poking a live stack pointer around. */
    size_t bytes = 64 * 1024;
    char *blk = calloc(1, bytes);
    if (!blk) return -1;

    /* Strings live at the top; pointers below them. Lay strings out first so
       their final addresses are known when the pointer array is filled. */
    size_t soff = bytes;
    vm_address_t saddr[128];
    if (argc + envc > 120) { fprintf(stderr, "too many args\n"); free(blk); return -1; }

    unsigned char rnd[16];
    for (int i = 0; i < 16; i++) rnd[i] = (unsigned char)(rand() ^ (i * 37));
    soff -= 16; memcpy(blk + soff, rnd, 16);
    vm_address_t at_random = STACK_TOP - (bytes - soff);

    static const char plat[] = "i686";
    soff -= sizeof plat; memcpy(blk + soff, plat, sizeof plat);
    vm_address_t at_platform = STACK_TOP - (bytes - soff);

    for (int i = argc - 1; i >= 0; i--) {
        size_t n = strlen(argv[i]) + 1;
        soff -= n; memcpy(blk + soff, argv[i], n);
        saddr[i] = STACK_TOP - (bytes - soff);
    }
    for (int i = envc - 1; i >= 0; i--) {
        size_t n = strlen(envp[i]) + 1;
        soff -= n; memcpy(blk + soff, envp[i], n);
        saddr[argc + i] = STACK_TOP - (bytes - soff);
    }

    /* Vector: argc + argv + NULL + envp + NULL + auxv + AT_NULL */
    size_t nwords = 1 + (argc + 1) + (envc + 1) + (14 * 2);
    size_t voff = (soff - nwords * 4) & ~15u;     /* 16-byte aligned esp */
    uint32_t *w = (uint32_t *)(blk + voff);
    size_t k = 0;

    w[k++] = (uint32_t) argc;
    for (int i = 0; i < argc; i++) w[k++] = saddr[i];
    w[k++] = 0;
    for (int i = 0; i < envc; i++) w[k++] = saddr[argc + i];
    w[k++] = 0;

#define AUX(t, v) do { w[k++] = (uint32_t)(t); w[k++] = (uint32_t)(v); } while (0)
    AUX(LINUX_AT_PHDR,     ld->phdr);
    AUX(LINUX_AT_PHENT,    ld->phentsize);
    AUX(LINUX_AT_PHNUM,    ld->phnum);
    AUX(LINUX_AT_PAGESZ,   vm_page_size);
    AUX(LINUX_AT_BASE,     ld->interp_base);
    AUX(LINUX_AT_FLAGS,    0);
    AUX(LINUX_AT_ENTRY,    ld->entry);
    AUX(LINUX_AT_UID,      getuid());
    AUX(LINUX_AT_EUID,     geteuid());
    AUX(LINUX_AT_GID,      getgid());
    AUX(LINUX_AT_EGID,     getegid());
    AUX(LINUX_AT_SECURE,   0);
    AUX(LINUX_AT_RANDOM,   at_random);     /* glibc dies without this */
    AUX(LINUX_AT_PLATFORM, at_platform);
    AUX(LINUX_AT_CLKTCK,   100);
    AUX(LINUX_AT_HWCAP,    0);
    AUX(LINUX_AT_NULL,     0);
#undef AUX

    vm_address_t esp = STACK_TOP - (bytes - voff);
    if (poke(esp, blk + voff, bytes - voff) < 0) {
        fprintf(stderr, "stack write failed\n"); free(blk); return -1;
    }
    free(blk);
    *esp_out = esp;
    LOG("stack: base=%#x esp=%#x argc=%d envc=%d\n",
        (unsigned) base, (unsigned) esp, argc, envc);
    return 0;
}

/*
 * Load an executable into the current process and work out where to start.
 *
 * Shared by startup and execve so that the two cannot drift apart -- an
 * exec'd program must get the same treatment as the first one, including its
 * interpreter and a properly built Linux stack.
 */
static int load_program(const char *hostpath, char **argv, char **envp,
                        vm_address_t *entry_out, vm_address_t *esp_out)
{
    struct loaded ld, interp;
    if (load_one(hostpath, PIE_BASE, &ld) < 0)
        return -1;
    brk_start = brk_cur = ld.brk;
    ld.interp_base = 0;

    vm_address_t start_at = ld.entry;
    if (ld.interp[0]) {
        char ibuf[1200];
        const char *ipath = resolve(ld.interp, ibuf, sizeof ibuf);
        LOG("interpreter %s -> %s\n", ld.interp, ipath);
        if (load_one(ipath, INTERP_BASE, &interp) < 0) {
            fprintf(stderr,
                "cannot load interpreter %s\n"
                "  A dynamic binary needs a Linux ld.so and libc. Point\n"
                "  --sysroot at a tree containing them, e.g. unpacked from\n"
                "  Debian's libc6 i386 package.\n", ld.interp);
            return -1;
        }
        ld.interp_base = interp.base;
        start_at = interp.entry;          /* ld.so runs first and jumps to
                                             AT_ENTRY once it has relocated */
        if (interp.brk > brk_cur)
            brk_start = brk_cur = interp.brk;
    }

    int ac = 0;
    while (argv[ac]) ac++;
    if (build_stack(ac, argv, envp, &ld, esp_out) < 0)
        return -1;
    *entry_out = start_at;
    return 0;
}

/* ---------------- syscalls ---------------------------------------- */

struct linux_stat64 {           /* what i386 fstat64 fills in */
    uint64_t st_dev;
    uint32_t __pad0;
    uint32_t __st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint32_t __pad3;
    int64_t  st_size;
    uint32_t st_blksize;
    uint64_t st_blocks;
    uint32_t st_atime_, st_atime_ns;
    uint32_t st_mtime_, st_mtime_ns;
    uint32_t st_ctime_, st_ctime_ns;
    uint64_t st_ino;
} __attribute__((packed));

static void fill_stat64(struct linux_stat64 *d, const struct stat *s)
{
    memset(d, 0, sizeof *d);
    d->st_dev = s->st_dev;
    d->__st_ino = (uint32_t) s->st_ino;
    d->st_ino = s->st_ino;
    d->st_mode = s->st_mode;
    d->st_nlink = s->st_nlink;
    d->st_uid = s->st_uid;
    d->st_gid = s->st_gid;
    d->st_rdev = s->st_rdev;
    d->st_size = s->st_size;
    d->st_blksize = s->st_blksize;
    d->st_blocks = s->st_blocks;
    d->st_atime_ = s->st_atime;
    d->st_mtime_ = s->st_mtime;
    d->st_ctime_ = s->st_ctime;
}

/* Linux open() flag bits, which are not the host's. */
static int open_flags_from_linux(int lf)
{
    int f = 0;
    switch (lf & 3) {
    case 0: f = O_RDONLY; break;
    case 1: f = O_WRONLY; break;
    default: f = O_RDWR; break;
    }
    if (lf & 0100)   f |= O_CREAT;
    if (lf & 0200)   f |= O_EXCL;
    if (lf & 01000)  f |= O_TRUNC;
    if (lf & 02000)  f |= O_APPEND;
    if (lf & 04000)  f |= O_NONBLOCK;
    if (lf & 0200000) f |= O_DIRECTORY;
    return f;
}

#define RET_ERR() (-(long) errno_to_linux(errno))

static long sys_write(int fd, vm_address_t buf, size_t len)
{
    if (fd < 0) return -LINUX_EBADF;
    if (len > (1u << 20)) len = 1u << 20;
    char *tmp = malloc(len ? len : 1);
    if (!tmp) return -LINUX_ENOMEM;
    if (len && peek(buf, tmp, len) < 0) { free(tmp); return -LINUX_EFAULT; }
    ssize_t n = UNLOCKED(write(fd, tmp, len));
    free(tmp);
    return n < 0 ? RET_ERR() : n;
}

static long sys_read(int fd, vm_address_t buf, size_t len)
{
    if (fd < 0) return -LINUX_EBADF;
    if (len > (1u << 20)) len = 1u << 20;
    char *tmp = malloc(len ? len : 1);
    if (!tmp) return -LINUX_ENOMEM;
    ssize_t n = UNLOCKED(read(fd, tmp, len));
    if (n > 0 && poke(buf, tmp, n) < 0) { free(tmp); return -LINUX_EFAULT; }
    free(tmp);
    return n < 0 ? RET_ERR() : n;
}

/* Linux writev: an array of 32-bit {base,len} pairs in the guest. */
static long sys_writev(int fd, vm_address_t vec, unsigned cnt)
{
    long total = 0;
    if (cnt > 1024) return -LINUX_EINVAL;
    for (unsigned i = 0; i < cnt; i++) {
        uint32_t iov[2];
        if (peek(vec + i * 8, iov, 8) < 0) return -LINUX_EFAULT;
        if (!iov[1]) continue;
        long n = sys_write(fd, iov[0], iov[1]);
        if (n < 0) return total ? total : n;
        total += n;
        if ((uint32_t) n != iov[1]) break;
    }
    return total;
}

static long sys_brk(vm_address_t want)
{
    /* Linux brk() returns the new break, or the current one if the request
       cannot be met -- it never returns an error. */
    if (want == 0 || want < brk_start)
        return (long) brk_cur;
    vm_address_t old = PAGE_UP(brk_cur), new = PAGE_UP(want);
    if (new > old) {
        vm_address_t a = old;
        if (vm_allocate(child_task, &a, new - old, FALSE) != KERN_SUCCESS)
            return (long) brk_cur;
        vm_protect(child_task, old, new - old, FALSE,
                   VM_PROT_READ | VM_PROT_WRITE);
    } else if (new < old) {
        vm_deallocate(child_task, new, old - new);
    }
    brk_cur = want;
    return (long) brk_cur;
}

/*
 * mmap -- the call ld.so lives on.
 *
 * The dynamic loader's pattern is: one big PROT_NONE anonymous reservation
 * for the whole library, then a MAP_FIXED file-backed mapping over each of
 * its segments. So MAP_FIXED landing on top of memory that is ALREADY mapped
 * is the normal case, not an error, and must overwrite rather than fail --
 * getting that wrong makes every shared library load silently wrong.
 *
 * Mach has no file-backed mapping we can reach from here, so a file mapping
 * is anonymous memory with the contents read into it. That is fine for
 * MAP_PRIVATE, which is all the loader uses; MAP_SHARED would not be
 * coherent with other writers, and nothing here needs it yet.
 */
#define LINUX_MAP_SHARED    0x01
#define LINUX_MAP_FIXED     0x10
#define LINUX_MAP_ANONYMOUS 0x20

static long do_mmap(vm_address_t addr, size_t len, int prot, int flags,
                    int fd, off_t off)
{
    if (!len)
        return -LINUX_EINVAL;
    size_t span = PAGE_UP(len);
    vm_address_t a;

    if (flags & LINUX_MAP_FIXED) {
        a = PAGE_DOWN(addr);
        /* Drop whatever is there and take the range unconditionally. */
        vm_deallocate(child_task, a, span);
        vm_address_t want = a;
        if (vm_allocate(child_task, &want, span, FALSE) != KERN_SUCCESS
                || want != a)
            return -LINUX_ENOMEM;
    } else {
        a = addr ? PAGE_DOWN(addr) : 0;
        if (a && vm_allocate(child_task, &a, span, FALSE) != KERN_SUCCESS)
            a = 0;                       /* hint refused: fall back to anywhere */
        if (!a && vm_allocate(child_task, &a, span, TRUE) != KERN_SUCCESS)
            return -LINUX_ENOMEM;
    }

    if (!(flags & LINUX_MAP_ANONYMOUS) && fd >= 0) {
        char *tmp = malloc(span);
        if (!tmp)
            return -LINUX_ENOMEM;
        ssize_t n = pread(fd, tmp, len, off);
        if (n > 0 && poke(a, tmp, n) < 0) { free(tmp); return -LINUX_EFAULT; }
        free(tmp);
    }

    vm_prot_t vp = 0;
    if (prot & 1) vp |= VM_PROT_READ;
    if (prot & 2) vp |= VM_PROT_WRITE;
    if (prot & 4) vp |= VM_PROT_EXECUTE;
    /* PROT_NONE reservations must stay readable to us: poke() re-protects
       before writing, but a later MAP_FIXED over the range needs to read the
       surrounding page. VM_PROT_NONE on Mach would also make the guest fault
       on a page ld.so intends to map over, so keep at least READ. */
    vm_protect(child_task, a, span, FALSE, vp ? vp : VM_PROT_READ);
    LOG("mmap %#x len=%#zx prot=%d flags=%#x fd=%d -> %#x\n",
        (unsigned) addr, len, prot, flags, fd, (unsigned) a);
    return (long) a;
}

static long sys_uname(vm_address_t buf)
{
    /* Linux struct utsname: six 65-byte fields. Claiming to be Linux is the
       whole point -- software checks this and changes behaviour. */
    char u[6][65];
    memset(u, 0, sizeof u);
    strcpy(u[0], "Linux");
    strcpy(u[1], "oryx");
    strcpy(u[2], "4.4.0-oryx");       /* plausible to glibc's version check */
    strcpy(u[3], "#1 Oryx Hurd linux-personality");
    strcpy(u[4], "i686");
    strcpy(u[5], "(none)");
    return poke(buf, u, sizeof u) < 0 ? -LINUX_EFAULT : 0;
}

/*
 * set_thread_area(2) -- the call that decides whether glibc runs at all.
 *
 * Static glibc sets up thread-local storage before main() and aborts with
 * "Fatal glibc error: Cannot allocate TLS block" if it cannot. On Linux the
 * call installs a segment descriptor whose base is the thread pointer, then
 * glibc loads %gs with (entry_number << 3) | 3 and reads TLS off %gs:0.
 *
 * GNU Mach can do exactly this: i386_set_gdt() modifies "thread-specific
 * segment descriptor slots ... copied into the CPU on each thread switch".
 * It is the same mechanism Hurd's own glibc uses -- Hurd's %gs is 0x4b,
 * which is GDT index 9, RPL 3.
 *
 * Passing selector -1 asks Mach to allocate a slot and hand back the full
 * selector. Linux wants an ENTRY NUMBER, so we return selector >> 3; glibc
 * recomputes (entry << 3) | 3, which is the same selector again as long as
 * Mach's slots are GDT with RPL 3.
 */
struct linux_user_desc {
    uint32_t entry_number;
    uint32_t base_addr;
    uint32_t limit;
    uint32_t flags;      /* bitfields, read as a word */
};

#define UD_SEG_32BIT(f)       ((f) & 1)
#define UD_CONTENTS(f)        (((f) >> 1) & 3)
#define UD_READ_EXEC_ONLY(f)  (((f) >> 3) & 1)
#define UD_LIMIT_IN_PAGES(f)  (((f) >> 4) & 1)
#define UD_SEG_NOT_PRESENT(f) (((f) >> 5) & 1)
#define UD_USEABLE(f)         (((f) >> 6) & 1)

static long sys_set_thread_area(vm_address_t uaddr)
{
    struct linux_user_desc ud;
    if (peek(uaddr, &ud, sizeof ud) < 0)
        return -LINUX_EFAULT;

    unsigned f = ud.flags;
    /* Access byte: present, DPL 3, non-system, then the segment type.
       Data segments are type 0b0001cw a -- expand-down and writable from
       the request, accessed set so the CPU need not write it back. */
    unsigned type;
    if (UD_CONTENTS(f) >= 2)                     /* code */
        type = 0x8 | (UD_READ_EXEC_ONLY(f) ? 0 : 0x2) | 1;
    else                                         /* data */
        type = ((UD_CONTENTS(f) & 1) << 2) | (UD_READ_EXEC_ONLY(f) ? 0 : 0x2) | 1;
    unsigned access = (UD_SEG_NOT_PRESENT(f) ? 0 : 0x80) | (3 << 5) | 0x10 | type;

    struct descriptor d;
    d.low_word  = (ud.limit & 0xffff) | ((ud.base_addr & 0xffff) << 16);
    d.high_word = ((ud.base_addr >> 16) & 0xff)
                | (access << 8)
                | (((ud.limit >> 16) & 0xf) << 16)
                | (UD_USEABLE(f) << 20)
                | (UD_SEG_32BIT(f) << 22)
                | (UD_LIMIT_IN_PAGES(f) << 23)
                | (((ud.base_addr >> 24) & 0xff) << 24);

    /* Mach has only "a few" of these slots. glibc asks for a fresh one on
       every exec, so remember ours and hand back the same slot rather than
       leaking one per execve until allocation fails. */
    int sel;
    if (ud.entry_number != 0xffffffffu)
        sel = (int)((ud.entry_number << 3) | 3);   /* caller named one */
    else if (cur->tls_sel >= 0)
        sel = cur->tls_sel;                        /* reuse ours */
    else
        sel = -1;                                  /* allocate a slot */

    kern_return_t kr = i386_set_gdt(child_thread, &sel, d);
    if (kr != KERN_SUCCESS) {
        LOG("i386_set_gdt: %s\n", mach_error_string(kr));
        return -LINUX_ENOSYS;
    }

    cur->tls_sel = sel;
    ud.entry_number = (unsigned) sel >> 3;
    if (poke(uaddr, &ud, sizeof ud) < 0)
        return -LINUX_EFAULT;
    LOG("set_thread_area: base=%#x -> selector %#x (entry %u)\n",
        ud.base_addr, (unsigned) sel, ud.entry_number);
    return 0;
}

/*
 * getdents64 -- what `ls` needs, and the reason it said
 * "Function not implemented" until now.
 *
 * The guest hands us a file descriptor it got from open(); the host side
 * needs a DIR* to iterate. fdopendir() takes ownership of the descriptor, so
 * it gets a dup() -- otherwise closing the DIR* would close the guest's fd
 * out from under it. The DIR* is cached per guest fd because the guest calls
 * getdents64 repeatedly and expects to continue where it left off.
 */
struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
} __attribute__((packed));

#define MAX_DIRCACHE 32
static struct { int fd; DIR *dir; } dircache[MAX_DIRCACHE];

static DIR *dir_for(int fd)
{
    for (int i = 0; i < MAX_DIRCACHE; i++)
        if (dircache[i].dir && dircache[i].fd == fd)
            return dircache[i].dir;
    for (int i = 0; i < MAX_DIRCACHE; i++) {
        if (!dircache[i].dir) {
            int d = dup(fd);
            if (d < 0) return NULL;
            DIR *dp = fdopendir(d);
            if (!dp) { close(d); return NULL; }
            dircache[i].fd = fd;
            dircache[i].dir = dp;
            return dp;
        }
    }
    return NULL;
}

static void dir_forget(int fd)
{
    for (int i = 0; i < MAX_DIRCACHE; i++)
        if (dircache[i].dir && dircache[i].fd == fd) {
            closedir(dircache[i].dir);
            dircache[i].dir = NULL;
        }
}

static long sys_getdents64(int fd, vm_address_t buf, unsigned bufsz)
{
    DIR *dp = dir_for(fd);
    if (!dp) return -LINUX_ENOTDIR;

    char *out = calloc(1, bufsz);
    if (!out) return -LINUX_ENOMEM;
    unsigned used = 0;

    for (;;) {
        long save = telldir(dp);
        errno = 0;
        struct dirent *de = readdir(dp);
        if (!de) break;

        size_t nlen = strlen(de->d_name);
        size_t rec = (sizeof(struct linux_dirent64) + nlen + 1 + 7) & ~7u;
        if (used + rec > bufsz) {
            /* Does not fit: rewind so the next call returns it again. */
            seekdir(dp, save);
            break;
        }
        struct linux_dirent64 *d = (struct linux_dirent64 *)(out + used);
        d->d_ino = de->d_ino;
        d->d_off = telldir(dp);
        d->d_reclen = rec;
#ifdef _DIRENT_HAVE_D_TYPE
        d->d_type = de->d_type;
#else
        d->d_type = 0;      /* DT_UNKNOWN; the guest will stat() instead */
#endif
        memcpy(d->d_name, de->d_name, nlen + 1);
        used += rec;
    }

    long r = used;
    if (used && poke(buf, out, used) < 0)
        r = -LINUX_EFAULT;
    free(out);
    return r;
}

/*
 * statx(2) -- syscall 383, and the real reason `ls` failed.
 *
 * A current glibc does not call stat64/fstatat64 any more; it calls statx and
 * falls back only on -ENOSYS from an old kernel. Since we claim to be Linux
 * 4.4 the fallback should have triggered, but busybox calls statx directly,
 * so it has to exist. Same story for clock_gettime64 (403): the 32-bit time_t
 * calls are being retired ahead of 2038.
 */
struct statx_ts { int64_t tv_sec; uint32_t tv_nsec; int32_t pad; };

struct linux_statx {
    uint32_t stx_mask, stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink, stx_uid, stx_gid;
    uint16_t stx_mode, __spare0;
    uint64_t stx_ino, stx_size, stx_blocks, stx_attributes_mask;
    struct statx_ts stx_atime, stx_btime, stx_ctime, stx_mtime;
    uint32_t stx_rdev_major, stx_rdev_minor;
    uint32_t stx_dev_major, stx_dev_minor;
    uint64_t stx_mnt_id;
    uint32_t stx_dio_mem_align, stx_dio_offset_align;
    uint64_t __spare3[12];
} __attribute__((packed));

#define STATX_BASIC_STATS 0x000007ffU

static void fill_statx(struct linux_statx *d, const struct stat *s)
{
    memset(d, 0, sizeof *d);
    d->stx_mask = STATX_BASIC_STATS;
    d->stx_blksize = s->st_blksize;
    d->stx_nlink = s->st_nlink;
    d->stx_uid = s->st_uid;
    d->stx_gid = s->st_gid;
    d->stx_mode = s->st_mode;
    d->stx_ino = s->st_ino;
    d->stx_size = s->st_size;
    d->stx_blocks = s->st_blocks;
    d->stx_atime.tv_sec = s->st_atime;
    d->stx_mtime.tv_sec = s->st_mtime;
    d->stx_ctime.tv_sec = s->st_ctime;
    d->stx_rdev_major = (s->st_rdev >> 8) & 0xfff;
    d->stx_rdev_minor = s->st_rdev & 0xff;
    d->stx_dev_major = (s->st_dev >> 8) & 0xfff;
    d->stx_dev_minor = s->st_dev & 0xff;
}

/* The *at() family. Only AT_FDCWD is honoured for the directory argument --
   busybox and friends use it for essentially everything, and a real dirfd
   would need openat-relative resolution the host does not expose simply. */
#define LINUX_AT_FDCWD (-100)
#define LINUX_AT_SYMLINK_NOFOLLOW 0x100

/* ---------------- processes: fork, execve, wait4 ------------------- */

/* Pull a NULL-terminated char*[] (argv or envp) out of the guest. */
static char **peek_vector(vm_address_t addr, int *count)
{
    int n = 0, cap = 64;
    char **v = calloc(cap + 1, sizeof *v);
    if (!v) return NULL;
    while (addr) {
        uint32_t p;
        if (peek(addr + n * 4, &p, 4) < 0 || !p) break;
        char buf[4096];
        if (peek_str(p, buf, sizeof buf) < 0) break;
        if (n == cap) {
            cap *= 2;
            char **nv = realloc(v, (cap + 1) * sizeof *v);
            if (!nv) break;
            v = nv;
        }
        v[n++] = strdup(buf);
    }
    v[n] = NULL;
    if (count) *count = n;
    return v;
}

static void free_vector(char **v)
{
    if (!v) return;
    for (int i = 0; v[i]; i++) free(v[i]);
    free(v);
}

/* Throw away every mapping in a task -- execve replacing the image. */
static void wipe_address_space(task_t t)
{
    vm_address_t addr = 0;
    for (;;) {
        vm_size_t size = 0;
        vm_prot_t prot, maxprot;
        vm_inherit_t inh;
        boolean_t shared;
        memory_object_name_t obj;
        vm_offset_t off;
        if (vm_region(t, &addr, &size, &prot, &maxprot, &inh, &shared,
                      &obj, &off) != KERN_SUCCESS)
            break;
        if (obj != MACH_PORT_NULL)
            mach_port_deallocate(mach_task_self(), obj);
        if (vm_deallocate(t, addr, size) != KERN_SUCCESS)
            addr += size;            /* could not drop it; step over it */
    }
}

static int load_program(const char *hostpath, char **argv, char **envp,
                        vm_address_t *entry_out, vm_address_t *esp_out);

/*
 * execve -- replace this process's image, keeping the task and thread.
 *
 * Linux keeps the pid and the file descriptors across exec, so re-using the
 * same task and thread is not just convenient, it is the correct semantics.
 * The thread is stopped (it is waiting for our exception reply), so pulling
 * the address space out from under it is safe as long as its registers are
 * pointed at the new entry before it resumes.
 */
static long sys_execve(vm_address_t pathp, vm_address_t argvp, vm_address_t envp,
                       struct i386_thread_state *st)
{
    char path[1024], rbuf[1200];
    if (peek_str(pathp, path, sizeof path) < 0)
        return -LINUX_EFAULT;

    const char *hp = resolve(path, rbuf, sizeof rbuf);
    if (access(hp, X_OK) != 0)
        return -(long) errno_to_linux(errno);

    char **av = peek_vector(argvp, NULL);
    char **ev = peek_vector(envp, NULL);
    if (!av || !ev) { free_vector(av); free_vector(ev); return -LINUX_ENOMEM; }

    wipe_address_space(cur->task);

    vm_address_t entry = 0, esp = 0;
    int rc = load_program(hp, av, ev, &entry, &esp);
    free_vector(av); free_vector(ev);
    if (rc < 0) {
        /* The old image is already gone, so there is nothing to return to.
           Linux cannot fail here either once it has committed. */
        LOG("execve %s failed after teardown -- killing pid %d\n", path, cur->pid);
        cur->alive = 0;
        cur->status = 127;
        guest_done = (cur == root);
        return 0;
    }

    /* A fresh image means fresh TLS; the old GDT slot is reused rather than
       leaking one per exec, since Mach offers only a few. */
    memset(st, 0, sizeof *st);
    mach_msg_type_number_t cnt = STATE_COUNT;
    thread_get_state(cur->thread, STATE_FLAVOR, (thread_state_t) st, &cnt);
    st->eip = entry;
    st->uesp = esp;
    st->eax = st->ebx = st->ecx = st->edx = st->esi = st->edi = st->ebp = 0;
    thread_set_state(cur->thread, STATE_FLAVOR, (thread_state_t) st, STATE_COUNT);

    LOG("execve %s -> entry %#x esp %#x (pid %d)\n", path,
        (unsigned) entry, (unsigned) esp, cur->pid);
    /* Tell the caller not to touch eip/eax: the new image is already set up. */
    return LONG_MIN;
}

/*
 * fork -- Mach gives this away almost for free.
 *
 * task_create(parent, inherit_memory=TRUE) produces a task whose address
 * space is inherited copy-on-write from the parent, which is exactly fork's
 * contract. The child gets a thread with the parent's registers, eax forced
 * to 0 and eip stepped past the int $0x80, so both sides return from the
 * same instruction with the values Linux promises.
 */
static long sys_fork(struct i386_thread_state *st)
{
    struct proc *c = proc_new();
    if (!c) return -LINUX_EAGAIN;

    if (task_create(cur->task, TRUE, &c->task) != KERN_SUCCESS) {
        c->used = 0;
        return -LINUX_EAGAIN;
    }
    c->ppid = cur->pid;
    fd_fork(c, cur);
    c->brk_lo = cur->brk_lo;
    c->brk_hi = cur->brk_hi;
    c->tls_sel = -1;             /* selectors are per-thread, not inherited */

    if (task_set_exception_port(c->task, exc_port) != KERN_SUCCESS ||
        thread_create(c->task, &c->thread) != KERN_SUCCESS) {
        task_terminate(c->task);
        c->used = 0;
        return -LINUX_EAGAIN;
    }

    /*
     * Carry the TLS descriptor across.
     *
     * Mach's i386_set_gdt slots are per-THREAD, not per-task, so the child's
     * brand new thread has no descriptor behind the %gs it inherits in the
     * register state. The first instruction touching TLS then takes a #GP
     * with the selector as its error code -- observed as
     *   exception 2 code 13 subcode 72
     * on a plain `ret`, which is a thoroughly unhelpful place to land.
     * i386_get_gdt reads the parent's descriptor back so it can be installed
     * on the child at the same selector.
     */
    if (cur->tls_sel >= 0) {
        struct descriptor d;
        if (i386_get_gdt(cur->thread, cur->tls_sel, &d) == KERN_SUCCESS) {
            int sel = cur->tls_sel;
            if (i386_set_gdt(c->thread, &sel, d) == KERN_SUCCESS)
                c->tls_sel = sel;
            else
                LOG("fork: could not give child pid %d a TLS slot\n", c->pid);
        }
    }

    struct i386_thread_state cs = *st;
    cs.eax = 0;                  /* the child sees 0 */
    cs.eip += 2;                 /* both sides step over the int $0x80 */
    if (thread_set_state(c->thread, STATE_FLAVOR, (thread_state_t) &cs,
                         STATE_COUNT) != KERN_SUCCESS) {
        task_terminate(c->task);
        c->used = 0;
        return -LINUX_EAGAIN;
    }
    thread_resume(c->thread);
    LOG("fork: pid %d -> child pid %d\n", cur->pid, c->pid);
    return c->pid;
}

/* Finish a wait4 that was parked, and release the parent's thread. */
static void wait_complete(struct proc *parent, struct proc *child)
{
    struct i386_thread_state st;
    mach_msg_type_number_t cnt = STATE_COUNT;

    struct proc *save = cur;
    cur = parent;                /* peek/poke act on the parent's memory */
    if (parent->wait_status_addr) {
        int32_t wstatus = (child->status & 0xff) << 8;   /* exited, code */
        poke(parent->wait_status_addr, &wstatus, 4);
    }
    cur = save;

    if (thread_get_state(parent->thread, STATE_FLAVOR,
                         (thread_state_t) &st, &cnt) == KERN_SUCCESS) {
        st.eax = (unsigned) child->pid;
        st.eip += 2;
        thread_set_state(parent->thread, STATE_FLAVOR,
                         (thread_state_t) &st, STATE_COUNT);
    }

    mig_reply_header_t r;
    memset(&r, 0, sizeof r);
    r.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    r.Head.msgh_size = sizeof r;
    r.Head.msgh_remote_port = parent->wait_reply;
    r.Head.msgh_local_port = MACH_PORT_NULL;
    r.Head.msgh_id = 2500;
    r.RetCodeType.msgt_name = MACH_MSG_TYPE_INTEGER_32;
    r.RetCodeType.msgt_size = 32;
    r.RetCodeType.msgt_number = 1;
    r.RetCodeType.msgt_inline = TRUE;
    r.RetCode = KERN_SUCCESS;
    mach_msg(&r.Head, MACH_SEND_MSG, sizeof r, 0, MACH_PORT_NULL,
             MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);

    parent->waiting = 0;
    child->reaped = 1;
    child->used = 0;
    LOG("wait4: pid %d reaped child %d (status %d)\n",
        parent->pid, child->pid, child->status);
}

/* A process has exited: tidy it up and wake a parent parked in wait4. */
static void proc_exited(struct proc *p, int status)
{
    p->alive = 0;
    p->status = status;
    fd_closeall(p);
    thread_terminate(p->thread);
    task_terminate(p->task);

    if (p == root) { guest_status = status; guest_done = 1; return; }

    for (int i = 0; i < MAX_PROC; i++) {
        struct proc *q = &procs[i];
        if (q->used && q->waiting && q->pid == p->ppid &&
            (q->wait_for == -1 || q->wait_for == p->pid)) {
            wait_complete(q, p);
            return;
        }
    }
}

static long sys_wait4(int pid, vm_address_t status_addr, int options)
{
    int have_children = 0;

    for (int i = 0; i < MAX_PROC; i++) {
        struct proc *p = &procs[i];
        if (!p->used || p->ppid != cur->pid) continue;
        if (pid > 0 && p->pid != pid) continue;
        have_children = 1;
        if (!p->alive && !p->reaped) {          /* already dead: reap now */
            if (status_addr) {
                int32_t w = (p->status & 0xff) << 8;
                if (poke(status_addr, &w, 4) < 0) return -LINUX_EFAULT;
            }
            int r = p->pid;
            p->reaped = 1; p->used = 0;
            return r;
        }
    }
    if (!have_children)
        return -LINUX_ECHILD;
    if (options & 1)                            /* WNOHANG */
        return 0;

    /* Park. The reply is withheld, so the caller's thread stays stopped,
       while this loop carries on serving the child that has to exit first. */
    cur->waiting = 1;
    cur->wait_for = pid > 0 ? pid : -1;
    cur->wait_reply = cur_reply;
    cur->wait_status_addr = status_addr;
    defer_reply = 1;
    LOG("wait4: pid %d parked\n", cur->pid);
    return 0;
}

static const char *scname(unsigned n)
{
    switch (n) {
    case 1: return "exit"; case 3: return "read"; case 4: return "write";
    case 5: return "open"; case 6: return "close"; case 13: return "time";
    case 19: return "lseek"; case 20: return "getpid"; case 33: return "access";
    case 45: return "brk"; case 54: return "ioctl"; case 78: return "gettimeofday";
    case 85: return "readlink"; case 90: return "old_mmap"; case 91: return "munmap";
    case 122: return "uname"; case 125: return "mprotect"; case 140: return "_llseek";
    case 145: return "readv"; case 146: return "writev"; case 174: return "rt_sigaction";
    case 175: return "rt_sigprocmask"; case 183: return "getcwd";
    case 191: return "ugetrlimit"; case 192: return "mmap2"; case 197: return "fstat64";
    case 195: return "stat64"; case 196: return "lstat64"; case 199: return "getuid32";
    case 200: return "getgid32"; case 201: return "geteuid32"; case 202: return "getegid32";
    case 220: return "getdents64"; case 221: return "fcntl64";
    case 300: return "fstatat64"; case 305: return "readlinkat";
    case 307: return "faccessat"; case 386: return "rseq";
    case 295: return "openat"; case 383: return "statx";
    case 403: return "clock_gettime64"; case 172: return "prctl";
    case 330: return "dup3"; case 331: return "pipe2"; case 42: return "pipe";
    case 162: return "nanosleep"; case 11: return "execve"; case 2: return "fork"; case 224: return "gettid"; case 240: return "futex";
    case 243: return "set_thread_area"; case 252: return "exit_group";
    case 258: return "set_tid_address"; case 265: return "clock_gettime";
    case 311: return "set_robust_list"; case 355: return "getrandom";
    default: return "?";
    }
}

static long dispatch(struct i386_thread_state *st)
{
    unsigned nr = st->eax;
    unsigned a1 = st->ebx, a2 = st->ecx, a3 = st->edx;
    unsigned a4 = st->esi, a5 = st->edi;
    char path[1024], path2[1024], rbuf[1200];
#define RESOLVE(p) resolve((p), rbuf, sizeof rbuf)

    syscall_count++;
    LOG("syscall %u (%s) a=%#x,%#x,%#x\n", nr, scname(nr), a1, a2, a3);

    switch (nr) {
    case 1:                                     /* exit */
    case 252:                                   /* exit_group */
        proc_exited(cur, (int)(a1 & 0xff));
        return LONG_MIN;                        /* nothing to resume */

    case 2:   return sys_fork(st);              /* fork */
    case 190: return sys_fork(st);              /* vfork -- fork is a legal
                                                   implementation of it */
    case 120:                                   /* clone */
        /* Only the fork-equivalent shape. Threads share an address space,
           which needs a second thread in ONE task and a futex; that is the
           next piece of work, not something to fake here. */
        if ((a1 & 0x00000100) == 0)             /* !CLONE_VM */
            return sys_fork(st);
        return -LINUX_ENOSYS;

    case 11:  return sys_execve(a1, a2, a3, st);
    case 7:                                     /* waitpid */
        return sys_wait4((int) a1, a2, a3);
    case 114:                                   /* wait4 */
        return sys_wait4((int) a1, a2, a3);

    case 3:  return sys_read(hfd(a1), a2, a3);
    case 4:  return sys_write(hfd(a1), a2, a3);
    case 146: return sys_writev(hfd(a1), a2, a3);
    case 145: {                                 /* readv */
        long total = 0;
        for (unsigned i = 0; i < a3; i++) {
            uint32_t iov[2];
            if (peek(a2 + i * 8, iov, 8) < 0) return -LINUX_EFAULT;
            long n = sys_read(hfd(a1), iov[0], iov[1]);
            if (n < 0) return total ? total : n;
            total += n;
            if ((uint32_t) n != iov[1]) break;
        }
        return total;
    }

    case 5: {                                   /* open */
        if (peek_str(a1, path, sizeof path) < 0) return -LINUX_EFAULT;
        int fd = open(RESOLVE(path), open_flags_from_linux(a2), a3);
        return fd < 0 ? RET_ERR() : gfd_alloc(fd);
    }
    case 295: {                                 /* openat */
        if (peek_str(a2, path, sizeof path) < 0) return -LINUX_EFAULT;
        int fd = open(RESOLVE(path), open_flags_from_linux(a3), a4);
        return fd < 0 ? RET_ERR() : gfd_alloc(fd);
    }
    case 6: {                                   /* close */
        int h = hfd(a1);
        if (h < 0) return -LINUX_EBADF;
        dir_forget(h);
        cur->fd[a1] = -1;
        return (h > 2 && close(h) < 0) ? RET_ERR() : 0;
    }
    case 220: return sys_getdents64(hfd(a1), a2, a3);   /* getdents64 */

    case 383: {                                 /* statx */
        struct stat st2;
        int r;
        if (peek_str(a2, path, sizeof path) < 0) return -LINUX_EFAULT;
        if (path[0] == '\0')                     /* empty path = the dirfd */
            r = fstat(hfd(a1), &st2);
        else if ((int) a1 != LINUX_AT_FDCWD && path[0] != '/')
            return -LINUX_ENOSYS;
        else
            r = (a3 & LINUX_AT_SYMLINK_NOFOLLOW) ? lstat(RESOLVE(path), &st2)
                                                 : stat(RESOLVE(path), &st2);
        if (r < 0) return RET_ERR();
        struct linux_statx sx; fill_statx(&sx, &st2);
        return poke(a5, &sx, sizeof sx) < 0 ? -LINUX_EFAULT : 0;
    }
    case 403: {                                 /* clock_gettime64 */
        struct timeval tv; gettimeofday(&tv, NULL);
        struct { int64_t sec; int64_t nsec; } ts = { tv.tv_sec, tv.tv_usec * 1000 };
        if (a2 && poke(a2, &ts, sizeof ts) < 0) return -LINUX_EFAULT;
        return 0;
    }

    case 300: {                                 /* fstatat64 */
        if (peek_str(a2, path, sizeof path) < 0) return -LINUX_EFAULT;
        if ((int) a1 != LINUX_AT_FDCWD && path[0] != '/') return -LINUX_ENOSYS;
        struct stat s;
        const char *rp = RESOLVE(path);
        int r = (a4 & LINUX_AT_SYMLINK_NOFOLLOW) ? lstat(rp, &s) : stat(rp, &s);
        if (r < 0) return RET_ERR();
        struct linux_stat64 d; fill_stat64(&d, &s);
        return poke(a3, &d, sizeof d) < 0 ? -LINUX_EFAULT : 0;
    }
    case 305: {                                 /* readlinkat */
        if (peek_str(a2, path, sizeof path) < 0) return -LINUX_EFAULT;
        ssize_t n = readlink(RESOLVE(path), path2, sizeof path2);
        if (n < 0) return RET_ERR();
        if ((unsigned) n > a4) n = a4;
        return poke(a3, path2, n) < 0 ? -LINUX_EFAULT : n;
    }
    case 307: {                                 /* faccessat */
        if (peek_str(a2, path, sizeof path) < 0) return -LINUX_EFAULT;
        return access(RESOLVE(path), a3) < 0 ? RET_ERR() : 0;
    }

    case 10: {                                  /* unlink */
        if (peek_str(a1, path, sizeof path) < 0) return -LINUX_EFAULT;
        return unlink(path) < 0 ? RET_ERR() : 0;
    }
    case 39: {                                  /* mkdir */
        if (peek_str(a1, path, sizeof path) < 0) return -LINUX_EFAULT;
        return mkdir(path, a2) < 0 ? RET_ERR() : 0;
    }
    case 40: {                                  /* rmdir */
        if (peek_str(a1, path, sizeof path) < 0) return -LINUX_EFAULT;
        return rmdir(path) < 0 ? RET_ERR() : 0;
    }
    case 38: {                                  /* rename */
        if (peek_str(a1, path, sizeof path) < 0) return -LINUX_EFAULT;
        if (peek_str(a2, path2, sizeof path2) < 0) return -LINUX_EFAULT;
        return rename(path, path2) < 0 ? RET_ERR() : 0;
    }
    case 15: {                                  /* chmod */
        if (peek_str(a1, path, sizeof path) < 0) return -LINUX_EFAULT;
        return chmod(path, a2) < 0 ? RET_ERR() : 0;
    }
    case 12: {                                  /* chdir */
        if (peek_str(a1, path, sizeof path) < 0) return -LINUX_EFAULT;
        return chdir(path) < 0 ? RET_ERR() : 0;
    }
    case 41: { int h = hfd(a1); if (h < 0) return -LINUX_EBADF;
               return gfd_alloc(dup(h)); }
    case 63: case 330: {                        /* dup2, dup3 */
        int h = hfd(a1);
        if (h < 0 || a2 >= MAX_FD) return -LINUX_EBADF;
        if (a1 == a2) return (nr == 330) ? -LINUX_EINVAL : (long) a2;
        /* dup2 onto a live descriptor closes it first, silently. */
        if (cur->fd[a2] >= 0 && cur->fd[a2] > 2) close(cur->fd[a2]);
        int n2 = dup(h);
        if (n2 < 0) return RET_ERR();
        if (nr == 330 && (a3 & 02000000)) fcntl(n2, F_SETFD, FD_CLOEXEC);
        cur->fd[a2] = n2;
        return (long) a2;
    }
    case 42: case 331: {                        /* pipe, pipe2 */
        int pf[2];
        if (pipe(pf) < 0) return RET_ERR();
        if (nr == 331 && (a2 & 02000000)) {
            fcntl(pf[0], F_SETFD, FD_CLOEXEC);
            fcntl(pf[1], F_SETFD, FD_CLOEXEC);
        }
        int g0 = gfd_alloc(pf[0]), g1 = gfd_alloc(pf[1]);
        if (g0 < 0 || g1 < 0) return -LINUX_EMFILE;
        uint32_t v[2] = { g0, g1 };
        return poke(a1, v, 8) < 0 ? -LINUX_EFAULT : 0;
    }
    case 162: {                                 /* nanosleep */
        uint32_t ts[2];
        if (peek(a1, ts, 8) < 0) return -LINUX_EFAULT;
        struct timespec t = { ts[0], ts[1] };
        return UNLOCKED(nanosleep(&t, NULL)) < 0 ? RET_ERR() : 0;
    }
    case 33: {                                  /* access */
        if (peek_str(a1, path, sizeof path) < 0) return -LINUX_EFAULT;
        return access(RESOLVE(path), a2) < 0 ? RET_ERR() : 0;
    }
    case 85: {                                  /* readlink */
        if (peek_str(a1, path, sizeof path) < 0) return -LINUX_EFAULT;
        ssize_t n = readlink(path, path2, sizeof path2);
        if (n < 0) return RET_ERR();
        if ((unsigned) n > a3) n = a3;
        return poke(a2, path2, n) < 0 ? -LINUX_EFAULT : n;
    }
    case 183: {                                 /* getcwd */
        if (!getcwd(path, sizeof path)) return RET_ERR();
        size_t n = strlen(path) + 1;
        if (n > a2) return -LINUX_ERANGE;
        return poke(a1, path, n) < 0 ? -LINUX_EFAULT : (long) n;
    }

    case 195: case 196: {                       /* stat64 / lstat64 */
        if (peek_str(a1, path, sizeof path) < 0) return -LINUX_EFAULT;
        struct stat s;
        const char *rp = RESOLVE(path);
        int r = (nr == 195) ? stat(rp, &s) : lstat(rp, &s);
        if (r < 0) return RET_ERR();
        struct linux_stat64 d; fill_stat64(&d, &s);
        return poke(a2, &d, sizeof d) < 0 ? -LINUX_EFAULT : 0;
    }
    case 197: {                                 /* fstat64 */
        struct stat s;
        if (fstat(hfd(a1), &s) < 0) return RET_ERR();
        struct linux_stat64 d; fill_stat64(&d, &s);
        return poke(a2, &d, sizeof d) < 0 ? -LINUX_EFAULT : 0;
    }

    case 19:                                    /* lseek */
        return lseek(hfd(a1), (int32_t) a2, a3) < 0 ? RET_ERR() : 0;
    case 140: {                                 /* _llseek */
        off_t r = lseek(hfd(a1), ((off_t) a2 << 32) | (off_t) a3, a5);
        if (r < 0) return RET_ERR();
        uint64_t v = r;
        return poke(a4, &v, 8) < 0 ? -LINUX_EFAULT : 0;
    }

    case 45:  return sys_brk(a1);
    case 90: {                                  /* old_mmap: args in a struct */
        uint32_t m[6];
        if (peek(a1, m, sizeof m) < 0) return -LINUX_EFAULT;
        return do_mmap(m[0], m[1], m[2], m[3], hfd(m[4]), m[5]);
    }
    case 192: return do_mmap(a1, a2, a3, a4, hfd(a5),
                             (off_t) st->ebp * (off_t) 4096);
    case 91:                                    /* munmap */
        vm_deallocate(child_task, PAGE_DOWN(a1), PAGE_UP(a2));
        return 0;
    case 125: {                                 /* mprotect */
        vm_prot_t vp = 0;
        if (a3 & 1) vp |= VM_PROT_READ;
        if (a3 & 2) vp |= VM_PROT_WRITE;
        if (a3 & 4) vp |= VM_PROT_EXECUTE;
        vm_protect(child_task, PAGE_DOWN(a1), PAGE_UP(a2), FALSE, vp);
        return 0;
    }

    case 122: return sys_uname(a1);
    case 20: case 224: return cur->pid;
    case 64: return cur->ppid;   /* getppid */
    case 199: case 24: return getuid();
    case 200: case 47: return getgid();
    case 201: case 49: return geteuid();
    case 202: case 50: return getegid();

    case 13: {                                  /* time */
        time_t t = time(NULL);
        if (a1) { uint32_t v = t; if (poke(a1, &v, 4) < 0) return -LINUX_EFAULT; }
        return (long) t;
    }
    case 78: {                                  /* gettimeofday */
        struct timeval tv; gettimeofday(&tv, NULL);
        uint32_t v[2] = { tv.tv_sec, tv.tv_usec };
        if (a1 && poke(a1, v, 8) < 0) return -LINUX_EFAULT;
        return 0;
    }
    case 265: {                                 /* clock_gettime */
        struct timeval tv; gettimeofday(&tv, NULL);
        uint32_t v[2] = { tv.tv_sec, tv.tv_usec * 1000 };
        if (a2 && poke(a2, v, 8) < 0) return -LINUX_EFAULT;
        return 0;
    }
    case 355: {                                 /* getrandom */
        char *tmp = malloc(a2 ? a2 : 1);
        if (!tmp) return -LINUX_ENOMEM;
        for (unsigned i = 0; i < a2; i++) tmp[i] = (char)(rand() & 0xff);
        long r = poke(a1, tmp, a2) < 0 ? -LINUX_EFAULT : (long) a2;
        free(tmp);
        return r;
    }

    case 54: {                                  /* ioctl */
        if (a2 == 0x5413) {                     /* TIOCGWINSZ */
            struct winsize ws;
            if (ioctl(hfd(a1), TIOCGWINSZ, &ws) < 0) return RET_ERR();
            uint16_t v[4] = { ws.ws_row, ws.ws_col, ws.ws_xpixel, ws.ws_ypixel };
            return poke(a3, v, sizeof v) < 0 ? -LINUX_EFAULT : 0;
        }
        /* Enough for isatty(); general terminal control is not wired up. */
        return isatty(hfd(a1)) ? 0 : -LINUX_ENOTTY;
    }
    case 221: {                                 /* fcntl64 */
        int h = hfd(a1);
        if (h < 0) return -LINUX_EBADF;
        if (a2 == 0)                            /* F_DUPFD */
            return gfd_alloc(dup(h));
        int r = fcntl(h, a2, a3);
        return r < 0 ? RET_ERR() : r;
    }
    case 191: {                                 /* ugetrlimit */
        uint32_t rl[2] = { 0xffffffffu, 0xffffffffu };
        if (a2 && poke(a2, rl, 8) < 0) return -LINUX_EFAULT;
        return 0;
    }

    /* Accepted and ignored: a single-threaded static binary sets these up
       during startup and never depends on the effect. Returning -ENOSYS
       instead makes glibc abort before main(). */
    case 174: case 175: case 258: case 311: case 172:
        return 0;

    case 243: return sys_set_thread_area(a1);   /* set_thread_area */

    default:
        LOG("unimplemented syscall %u (%s)\n", nr, scname(nr));
        return -LINUX_ENOSYS;
    }
}

/* ---------------- exception handling ------------------------------ */

kern_return_t _S_catch_exception_raise(mach_port_t port, mach_port_t thread,
                                       mach_port_t task, integer_t exception,
                                       integer_t code, rpc_long_integer_t subcode)
{
    struct i386_thread_state st;
    mach_msg_type_number_t count = STATE_COUNT;
    (void) port;

    /* Every guest task shares this port, so the message decides whose
       registers and whose memory the rest of this refers to. */
    LOCK();
    struct proc *p = proc_by_task(task);
    if (!p) {
        UNLOCK();
        LOG("exception from unknown task %u -- ignoring\n", (unsigned) task);
        return KERN_FAILURE;
    }
    cur = p;
    cur->thread = thread;

    if (thread_get_state(thread, STATE_FLAVOR, (thread_state_t) &st, &count)
            != KERN_SUCCESS) {
        UNLOCK();
        return KERN_FAILURE;
    }

    unsigned char insn[2] = { 0, 0 };
    peek(st.eip, insn, 2);

    if (exception != EXC_BAD_INSTRUCTION || insn[0] != 0xcd || insn[1] != 0x80) {
        fprintf(stderr,
                "\n[oryx] guest fault: exception %d code %d subcode %ld at eip=%#x"
                " (bytes %02x %02x)\n", (int) exception, (int) code,
                (long) subcode, (unsigned) st.eip, insn[0], insn[1]);
        fprintf(stderr, "[oryx] eax=%#x ebx=%#x ecx=%#x edx=%#x esp=%#x\n",
                st.eax, st.ebx, st.ecx, st.edx, st.uesp);
        proc_exited(cur, 128 + 11);
        UNLOCK();
        return KERN_SUCCESS;     /* stop cleanly rather than loop on the fault */
    }

    long ret = dispatch(&st);

    /* LONG_MIN means the handler already decided where the thread goes --
       it exited, or execve pointed it at a new image -- so leave its
       registers alone. defer_reply means wait4 parked it. */
    if (ret == LONG_MIN || defer_reply || !cur->alive) {
        UNLOCK();
        return KERN_SUCCESS;
    }

    st.eax = (unsigned int) ret;
    st.eip += 2;                 /* step over the int $0x80 */
    kern_return_t kr2 = thread_set_state(thread, STATE_FLAVOR,
                                         (thread_state_t) &st, STATE_COUNT);
    UNLOCK();
    return kr2 == KERN_SUCCESS ? KERN_SUCCESS : KERN_FAILURE;
}

static void finish(int status) __attribute__((noreturn));

/*
 * Tear down and leave, from whichever worker thread saw the root process
 * exit. Joining the others is not possible: they are blocked in mach_msg
 * waiting for guest syscalls that will never come, so the process simply
 * ends here.
 */
static void finish(int status)
{
    for (int i = 0; i < MAX_PROC; i++)
        if (procs[i].used && procs[i].alive) {
            thread_terminate(procs[i].thread);
            task_terminate(procs[i].task);
        }
    if (verbose)
        fprintf(stderr, "[oryx] guest exited with status %d after %lu syscalls\n",
                status, syscall_count);
    fflush(NULL);
    _exit(status < 0 ? 1 : status);
}

static void *serve(void *arg)
{
    (void) arg;
    union { mach_msg_header_t hdr; char buf[4096]; } in, out;

    for (;;) {
        if (mach_msg(&in.hdr, MACH_RCV_MSG, 0, sizeof in, exc_port,
                     MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) != KERN_SUCCESS)
            return NULL;

        mach_port_t reply_to = in.hdr.msgh_remote_port;
        mig_routine_t routine = _S_exc_server_routine(&in.hdr);
        if (!routine)
            continue;
        memset(&out, 0, sizeof out);
        cur_reply = reply_to;
        defer_reply = 0;
        routine(&in.hdr, &out.hdr);

        /* wait4 withheld the reply on purpose: the caller stays stopped
           until a child exits and wait_complete() sends it. */
        if (defer_reply)
            continue;

        /*
         * The MIG glue parses the request but leaves the reply header empty,
         * so it is built by hand -- including the type descriptor, which a
         * zeroed one is not: the send fails silently and the guest hangs
         * forever waiting to be resumed. (probe.c, the hard way.)
         */
        mig_reply_header_t *r = (mig_reply_header_t *) &out.hdr;
        memset(r, 0, sizeof *r);
        r->Head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
        r->Head.msgh_size = sizeof *r;
        r->Head.msgh_remote_port = reply_to;
        r->Head.msgh_local_port = MACH_PORT_NULL;
        r->Head.msgh_id = in.hdr.msgh_id + 100;
        r->RetCodeType.msgt_name = MACH_MSG_TYPE_INTEGER_32;
        r->RetCodeType.msgt_size = 32;
        r->RetCodeType.msgt_number = 1;
        r->RetCodeType.msgt_inline = TRUE;
        r->RetCodeType.msgt_longform = FALSE;
        r->RetCodeType.msgt_deallocate = FALSE;
        r->RetCode = KERN_SUCCESS;

        mach_msg(&r->Head, MACH_SEND_MSG, sizeof *r, 0, MACH_PORT_NULL,
                 MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);

        if (guest_done)
            finish(guest_status);
    }
    return NULL;
}

#define NWORKERS 8

/* ------------------------------------------------------------------ */

int main(int argc, char **argv, char **envp)
{
    kern_return_t kr;
    int ai = 1;

    while (ai < argc && argv[ai][0] == '-') {
        if (!strcmp(argv[ai], "-v")) { verbose = 1; ai++; }
        else if (!strcmp(argv[ai], "--sysroot") && ai + 1 < argc) {
            sysroot = argv[ai + 1]; ai += 2;
        } else break;
    }
    if (!sysroot) sysroot = getenv("ORYX_SYSROOT");
    /* Installed default, so a packaged oryxlinux runs dynamic binaries with
       no flags and no environment set up by hand. */
    if (!sysroot && access(DEFAULT_SYSROOT "/lib", F_OK) == 0)
        sysroot = DEFAULT_SYSROOT;
    if (ai >= argc) {
        fprintf(stderr,
            "oryxlinux -- run a 32-bit Linux binary on GNU/Hurd\n"
            "usage: %s [-v] [--sysroot DIR] <binary> [args...]\n"
            "\n"
            "  --sysroot DIR   where the Linux ld.so and libraries live;\n"
            "                  needed for dynamic binaries. Also ORYX_SYSROOT.\n"
            "                  Defaults to %s\n",
            argv[0], DEFAULT_SYSROOT);
        return 2;
    }

    setvbuf(stdout, NULL, _IONBF, 0);

    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &exc_port);
    if (kr) { fprintf(stderr, "port_allocate: %s\n", mach_error_string(kr)); return 1; }
    kr = mach_port_insert_right(mach_task_self(), exc_port, exc_port,
                                MACH_MSG_TYPE_MAKE_SEND);
    if (kr) { fprintf(stderr, "insert_right: %s\n", mach_error_string(kr)); return 1; }

    cur = root = proc_new();
    if (cur) fd_init(cur);
    if (!cur) { fprintf(stderr, "no process slots\n"); return 1; }

    /* An EMPTY address space: inherit_memory = FALSE. Nothing of Hurd's libc
       is in here, which is exactly what the guest needs. */
    kr = task_create(mach_task_self(), FALSE, &child_task);
    if (kr) { fprintf(stderr, "task_create: %s\n", mach_error_string(kr)); return 1; }

    vm_address_t entry = 0, esp = 0;
    if (load_program(argv[ai], argv + ai, envp, &entry, &esp) < 0) return 1;

    kr = task_set_exception_port(child_task, exc_port);
    if (kr) { fprintf(stderr, "set_exception_port: %s\n", mach_error_string(kr)); return 1; }

    kr = thread_create(child_task, &child_thread);
    if (kr) { fprintf(stderr, "thread_create: %s\n", mach_error_string(kr)); return 1; }

    /* Take the default register state and change only what we must: a
       hand-built state would need valid cs/ds/ss selectors, and the ones the
       kernel puts in a fresh thread are already correct. */
    struct i386_thread_state st;
    mach_msg_type_number_t count = STATE_COUNT;
    kr = thread_get_state(child_thread, STATE_FLAVOR, (thread_state_t) &st, &count);
    if (kr) { fprintf(stderr, "thread_get_state: %s\n", mach_error_string(kr)); return 1; }
    st.eip = entry;
    st.uesp = esp;
    st.eax = st.ebx = st.ecx = st.edx = st.esi = st.edi = st.ebp = 0;

    kr = thread_set_state(child_thread, STATE_FLAVOR, (thread_state_t) &st, STATE_COUNT);
    if (kr) { fprintf(stderr, "thread_set_state: %s\n", mach_error_string(kr)); return 1; }

    LOG("pid %d: entry=%#x esp=%#x brk=%#x -- starting guest\n",
        cur->pid, (unsigned) entry, (unsigned) esp, (unsigned) brk_cur);

    kr = thread_resume(child_thread);
    if (kr) { fprintf(stderr, "thread_resume: %s\n", mach_error_string(kr)); return 1; }

    /* Extra workers so a guest blocked in read() cannot stall the rest. */
    for (int i = 1; i < NWORKERS; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, serve, NULL) == 0)
            pthread_detach(t);
    }
    serve(NULL);
    finish(guest_status);
}
