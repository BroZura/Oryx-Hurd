/*
 * hello.S -- the smallest possible Linux i386 binary, for oryxlinux.
 *
 * No libc, no startup code: the kernel (here, the emulator) jumps straight
 * to _start with the initial stack in place. Every kernel entry is
 * `int $0x80`, which is what makes this a *Linux* binary rather than a Hurd
 * one even though the assembler is Hurd's.
 *
 * It exercises the pieces in order of how likely they are to be wrong:
 *   write(2)   -- the syscall path itself
 *   the stack  -- reads argc, and prints argv[0], proving the initial stack
 *                 layout the emulator built is the one Linux promises
 *   uname(2)   -- a syscall that writes a struct back into guest memory
 *   exit(2)
 *
 * Build:  gcc -m32 -nostdlib -static -o hello hello.S
 */
    .text
    .globl _start
_start:
    /* esp -> argc, argv[0], argv[1], ..., NULL, envp... */
    movl    (%esp), %ebp            /* ebp = argc */

    /* write(1, msg, msglen) */
    movl    $4, %eax
    movl    $1, %ebx
    movl    $msg, %ecx
    movl    $msglen, %edx
    int     $0x80

    /* write(1, argv0_label, ...) then argv[0] itself */
    movl    $4, %eax
    movl    $1, %ebx
    movl    $a0lbl, %ecx
    movl    $a0lbllen, %edx
    int     $0x80

    movl    4(%esp), %esi           /* esi = argv[0] */
    movl    %esi, %edi
    xorl    %ecx, %ecx
1:  cmpb    $0, (%edi)              /* strlen(argv[0]) */
    je      2f
    incl    %edi
    incl    %ecx
    jmp     1b
2:  movl    %ecx, %edx
    movl    $4, %eax
    movl    $1, %ebx
    movl    %esi, %ecx
    int     $0x80

    movl    $4, %eax
    movl    $1, %ebx
    movl    $nl, %ecx
    movl    $1, %edx
    int     $0x80

    /* uname(&buf) -- writes 6 x 65 bytes back into our memory */
    movl    $122, %eax
    movl    $unamebuf, %ebx
    int     $0x80
    testl   %eax, %eax
    js      3f

    movl    $4, %eax                /* sysname */
    movl    $1, %ebx
    movl    $unbl, %ecx
    movl    $unbllen, %edx
    int     $0x80

    movl    $4, %eax
    movl    $1, %ebx
    movl    $unamebuf, %ecx
    movl    $5, %edx                /* "Linux" */
    int     $0x80

    movl    $4, %eax
    movl    $1, %ebx
    movl    $nl, %ecx
    movl    $1, %edx
    int     $0x80

3:  /* exit(0) */
    movl    $1, %eax
    xorl    %ebx, %ebx
    int     $0x80
    hlt

    .data
msg:    .ascii  "hello from a LINUX binary running on GNU/Hurd\n"
    .set msglen, . - msg
a0lbl:  .ascii  "argv[0] = "
    .set a0lbllen, . - a0lbl
unbl:   .ascii  "uname.sysname = "
    .set unbllen, . - unbl
nl:     .ascii  "\n"

    .bss
    .lcomm unamebuf, 390
