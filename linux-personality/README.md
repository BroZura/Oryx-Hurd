# Linux personality for Oryx Hurd

A Linux syscall emulation layer for GNU/Hurd — the same category of thing as
FreeBSD's Linuxulator, Solaris `lx` zones, WSL 1 and gVisor, but on the
microkernel that was *designed* for OS personalities in the first place.

**Status: shipped as part of Oryx.** `pacman -S oryx-linux oryx-linux-sysroot`
and Linux binaries run, with no flags and nothing to configure.

It runs them **static and dynamic, with a working process model** — Unmodified
Debian i386 binaries execute on GNU Mach in a task of their own, with their
syscalls serviced from outside — including **GNU coreutils 9.7**, dynamically
linked through a real `ld-linux.so.2` against glibc, libselinux, libcap,
libacl, libattr, libgmp and libpcre2.

```
$ ./oryxlinux --sysroot ./sysroot tests/gnu/ls --version
ls (GNU coreutils) 9.7
Packaged by Debian (9.7-3)
```

```
$ ./oryxlinux tests/busybox uname -a
Linux oryx 4.4.0-oryx #1 Oryx Hurd linux-personality i686 GNU/Linux

$ ./oryxlinux tests/busybox ls /etc | head -3
X11
alternatives
apt
```

Test results, every case compared against the *host's* own answer — so they
prove the emulated result matches what Hurd itself reports, not merely that
something was printed:

| suite | guest | result |
|---|---|---|
| `run-tests.sh` | static busybox | **16 passed, 0 failed** |
| `run-tests.sh` | dynamic PIE busybox | **16 passed, 0 failed** |
| `run-shell.sh` | busybox ash: fork/exec/pipes | **10 passed, 0 failed** |
| `run-gnu.sh` | Debian GNU coreutils | **4 passed, 0 failed** |

**46 tests, all green against the installed binary.**

```
$ oryxlinux busybox sh -c 'busybox cat /etc/hostname | busybox md5sum'
b10f0b7b8b81183dc3b591a18a9045e9  -
```

That one line is two guest processes, a fork, an execve, a host pipe between
them and a wait4 — and the digest matches the host's own `md5sum`.

---

## Layout

```
probe.c         the original proof: trap one int $0x80 inside our own process
oryxlinux.c     the personality: separate task, ELF loader, syscall table
tests/hello.S   a minimal hand-written Linux binary (no libc at all)
tests/run-tests.sh   the suite above, for either busybox
tests/run-shell.sh   fork / execve / wait4 / pipes, through busybox ash
tests/run-gnu.sh     the same idea for Debian's GNU coreutils
```

Installed layout (from the `oryx-linux` and `oryx-linux-sysroot` packages):

```
/usr/bin/oryxlinux                  the emulator
/usr/lib/oryx-linux/sysroot/        the Linux loader and libraries
/usr/share/doc/oryx-linux/          this file
```

Build on Oryx: `gcc -Wall -O0 -o oryxlinux oryxlinux.c`

Get a guest to run:

```sh
# on the Dell -- a static guest needs nothing else
curl -O http://deb.debian.org/debian/pool/main/b/busybox/busybox-static_1.37.0-6+b9_i386.deb
dpkg-deb -x busybox-static_*_i386.deb x && scp x/usr/bin/busybox archhurd:/root/lp/tests/
```

A **dynamic** guest also needs a sysroot with the Linux loader and libraries.
Unpack the i386 `libc6` (plus whatever else the binary wants) and lay it out
so `/lib/ld-linux.so.2` resolves inside it:

```sh
dpkg-deb -x libc6_*_i386.deb root          # also libcap2, libselinux1, ... for coreutils
mkdir -p sysroot/lib && cp -a root/usr/lib/. sysroot/lib/
./oryxlinux --sysroot /root/lp/sysroot tests/busybox-dyn echo hi
```

Hurd's own loader is `/lib/ld.so` and its libraries live in `/lib/i386-gnu`,
so the Linux paths (`/lib/ld-linux.so.2`, `/lib/i386-linux-gnu`) do not
collide with anything — the sysroot is about keeping the Linux userland in
one place, not about avoiding a clash.

---

## How it works

1. `task_create(…, FALSE, …)` — an **empty** address space. Not a fork: a
   forked child still has Hurd's glibc, its TLS and its `%gs` mapped, and the
   guest wants to lay out that address space itself, starting at 0x08048000.
   An empty task avoids every one of those collisions, and a guest crash
   cannot corrupt the emulator.
2. Map each `PT_LOAD` with `vm_allocate` + `vm_write`, apply the segment's
   protection.
3. Build a Linux initial stack: `argc`, `argv[]`, `envp[]`, then the auxiliary
   vector.
4. `task_set_exception_port()`, `thread_create()`, set `eip`/`esp`, resume.
5. Every `int $0x80` arrives as `EXC_BAD_INSTRUCTION`. Decode `eax`, perform
   the call against the guest's memory, write the result to `eax`, step `eip`
   by 2, resume.

### The process model

`fork` is nearly free on Mach: `task_create(parent, inherit_memory=TRUE)`
gives a task whose address space is inherited copy-on-write, which is exactly
fork's contract. The child gets a thread carrying the parent's registers with
`eax` forced to 0 and `eip` stepped past the trap, so both sides return from
the same instruction with the values Linux promises.

`execve` keeps the task and thread and replaces what is inside them — which
is also the correct semantics, since Linux preserves the pid and the open
descriptors across exec.

`wait4` cannot block the server: the child still needs *this* loop to service
its syscalls. Instead the reply to the parent's exception is **withheld**,
which is what keeps the parent's thread stopped, and sent once a child exits.

Every guest process shares one exception port, so all their syscalls arrive on
the same port; the message says which task trapped, and that selects the
process the rest of the handler works on.

---

## What was learned doing it

### 1. `int $0x80` is unclaimed on Hurd

GNU Mach enters the kernel through a call gate, `lcall $0x7` — 43 call sites
in libc, **zero** uses of `int $0x80`. So the instruction 32-bit Linux
binaries use is free to take. It faults as `EXC_BAD_INSTRUCTION`, code 13
(i386 trap 13, #GP), subcode 1026 = 0x402, the IDT selector error for vector
0x80. EIP points **at** the instruction, so it must be advanced by 2 by hand.

### 2. GNU Mach uses the original Mach 3 exception interface

Not the macOS/OSF one. No `EXC_MASK_*`, no behaviors, no
`task_set_exception_ports`. It is `task_set_exception_port()`, and exceptions
arrive as `exception_raise` with `msgh_id = 2400` and a send-once reply right
in `msgh_remote_port`.

### 3. The MIG glue parses but does not reply

`_S_exc_server_routine()` demuxes correctly and calls your handler, then
leaves the reply header **empty**. A reply is mandatory — the kernel blocks
waiting for it and the faulting thread never resumes without one. Build a
`mig_reply_header_t` by hand and **fill in the type descriptor**; a zeroed one
is rejected and the send fails silently.

### 4. ⚠️ `i386_THREAD_STATE` silently destroys `%gs`

`thread_set_state()` with flavor `i386_THREAD_STATE` does not honour the
segment fields: GNU Mach resets `%gs` from `0x4b` (Hurd's TLS selector) to
`0x1f`. The thread then dies on its next libc call, reading the stack canary
from `%gs:0x14`, at an instruction beginning `65 a1` far from the real cause.
**Use `i386_REGS_SEGS_STATE` (flavor 5).**

### 5. Mach's VM calls are page-aligned, on BOTH sides

`vm_write` requires the destination address *and* the length to be whole
pages — ELF segments are neither, so loading fails outright with
"vm_write of segment 0 failed". Less obviously, **the source buffer must be
page-aligned too**: a `malloc()`ed buffer is rejected, `posix_memalign()` is
not. Everything therefore goes through a read-modify-write of the enclosing
pages, which conveniently also handles writing into a read-only mapping.

There is also **no `vm_read_overwrite`** in GNU Mach — only `vm_read`, which
allocates the result in *your* address space. Every read must
`vm_deallocate()` afterwards or it leaks a page per syscall.

### 6. TLS is the make-or-break syscall, and Mach can do it

Static glibc aborts before `main()` with

```
Fatal glibc error: Cannot allocate TLS block
```

unless `set_thread_area` (243) works. On Linux it installs a segment
descriptor whose base is the thread pointer, then glibc loads `%gs` with
`(entry_number << 3) | 3`.

GNU Mach has exactly the right primitive: **`i386_set_gdt()`**, documented as
modifying "thread-specific segment descriptor slots … copied into the CPU on
each thread switch". Pass selector `-1` to allocate one. It hands back a full
selector; Linux wants an *entry number*, so return `selector >> 3` and glibc
recomputes the same selector.

The pleasing part: the slot it allocates is **0x4b** — the very selector
Hurd's own glibc uses for TLS.

### 7. Modern glibc does not call the syscalls you expect

`ls` failed with `ENOSYS` long after `stat64`, `lstat64` and `fstatat64` were
all implemented, because current glibc and busybox call **`statx` (383)** and
**`clock_gettime64` (403)** instead. The 32-bit `time_t` calls are being
retired ahead of 2038, and `statx` has superseded the stat family. Announcing
kernel 4.4 in `uname` does not help — busybox calls `statx` directly.

Implement, in this order, and most things work: `write`, `exit_group`, `brk`,
`set_thread_area`, `openat`, `read`, `close`, `statx`, `getdents64`,
`writev`, `mmap2`, `mprotect`, `uname`.

### 8. Two Linux ABI details that are easy to get wrong

- **`AT_RANDOM` is not optional.** glibc seeds its stack guard from those 16
  bytes; omit the auxv entry and it dereferences NULL before `main()`.
- **Errno numbers are not shared.** Hurd's are Mach error codes with a
  subsystem in the high bits — `EINVAL` is `0x40000016`, not 22. Returning one
  raw sets nonsense in the guest's `errno`. Translate by name.

### 9. Going dynamic is mostly about `mmap` and a bias

Three things, and none of them was the syscall table:

- **ET_DYN everywhere.** Debian's i386 binaries are PIE, and `ld.so` is an
  ET_DYN too, so the loader has to place objects at a *bias* and add it to
  every `p_vaddr`, `e_entry` and `e_phoff`. The executable and the
  interpreter need different biases (here 0x56555000 and 0x40000000) so they
  cannot overlap each other, the stack, or the heap.
- **Execution starts at the INTERPRETER's entry, not the program's.** ld.so
  relocates everything and jumps to `AT_ENTRY` itself. `AT_BASE` must be the
  interpreter's load bias or it cannot relocate itself.
- **`MAP_FIXED` over already-mapped memory is the normal case.** The loader
  reserves one big PROT_NONE anonymous region per library, then maps each
  segment over it with `MAP_FIXED`. An implementation that treats "already
  mapped" as an error makes every shared library load silently wrong. On
  Mach: `vm_deallocate` the range first, then `vm_allocate` it back.

Mach has no file-backed mapping reachable from here, so a file `mmap` is
anonymous memory with the contents read in. That is correct for `MAP_PRIVATE`
— all ld.so uses — and would not be for `MAP_SHARED`.

A missing library reports itself exactly as Linux would, which is a good sign
the loader is doing real work rather than being humoured:

```
tests/gnu/ls: error while loading shared libraries: libcap.so.2:
cannot open shared object file: No such file or directory
```

### 10. Three bugs the process model produced, all worth knowing

**Mach's GDT slots are per-THREAD, not per-task.** A forked child inherits
`%gs` in its register state but *not* the descriptor behind it, so the first
instruction touching TLS takes a `#GP` with the selector as its error code —
surfacing as `exception 2 code 13 subcode 72` on a plain `ret`, nowhere near
the real cause. `i386_get_gdt` reads the parent's descriptor back so it can
be installed on the child at the same selector.

**Each process needs its own descriptor table.** Sharing one across every
guest breaks command substitution with `sh: dup2(4,1): Bad file descriptor`,
because the parent's `close` takes the descriptor away before the child runs.
`fork` copies the table, `dup()`ing each entry so the two sides close
independently while still sharing the file description and its offset.

**On exit, close by HOST descriptor, not guest index.** Skipping guest fds
0–2 looks obviously right and is wrong: after a pipeline the child's fd 1 *is*
the pipe's write end, so leaving it open means the reader never sees EOF and
`echo x | cat` hangs with the writer already exited. Only host 0/1/2 — the
emulator's own stdio — must survive.

### 11. A single-threaded server cannot run a pipeline

`a | b` has one guest process reading a pipe the other has not written yet.
That `read` blocks the server, the writer never gets serviced, and the whole
thing deadlocks. The fix is several worker threads receiving from the same
exception port, so one parked in a blocking host call does not stop the rest.

`cur` — which process this thread is serving — therefore has to be
`__thread`. Everything genuinely shared (the process table, pid allocation,
the directory cache) sits under one lock, held for the whole of a syscall
**except** around host calls that can block. Keeping that window to just
`read`, `write` and `nanosleep` is what makes a coarse lock safe.

---

## Where this goes next

1. ~~**`execve` and `fork`**~~ — **done**, along with `wait4`, pipes and a
   multi-threaded server. `busybox sh` runs pipelines and command
   substitution.
2. **Threads.** `clone` with `CLONE_VM` (a second thread in one task), and
   `futex` mapped onto Mach's `gsync` — the piece most personalities have to
   fake, and Hurd already has it. Currently `clone` without `CLONE_VM` is
   treated as `fork` and anything else returns `ENOSYS`.
3. **Signals.** `rt_sigaction` is currently accepted and ignored, which is
   fine until something actually needs a handler run.
4. **`/proc`.** Hurd's procfs has a `--compatible` flag aimed at Linux procps.
5. ~~**Dynamic binaries**~~ — **done.** ET_DYN/PIE loading, `PT_INTERP`,
   `AT_BASE` and a sysroot are all in.

## The honest limits

- **32-bit only.** This traps `int $0x80`. Modern distro packages are amd64
  and use `syscall`; running those needs CPU emulation, which is a different
  and far larger project — and a user-mode CPU emulator would still need
  everything here, plus a translator.
- **No threads.** Processes yes, threads no: `clone(CLONE_VM)` returns
  `ENOSYS`, so anything genuinely multi-threaded will not run.
- **Signals are accepted and ignored.** `rt_sigaction` succeeds but no
  handler is ever delivered, so Ctrl-C and `kill` do not reach the guest.
- **File mappings are copies, not mappings.** Mach has no file-backed VM we
  can reach from here, so `mmap` of a file reads the contents into anonymous
  memory. Correct for `MAP_PRIVATE`, which is all ld.so uses; `MAP_SHARED`
  would not be coherent between processes.
- **If you can rebuild it, you do not need this.** A package that builds for
  32-bit Linux almost certainly builds for hurd-i386 directly, and porting
  natively is cheaper than porting to an emulation layer. The real niche is
  software welded to Linux-only kernel interfaces — `epoll`, `inotify`,
  netlink — where implementing the interface once beats patching twenty
  packages.

So: a showcase, and the most interesting thing in the project — but the
repository and package porting are what make Oryx *useful*.
