# Oryx Hurd — the catalogue of gaps

Every item here was found the hard way on the ThinkPad T400 between
2026-09-17 and 2026-09-19. The scripts encode the fixes; this file records the
**symptoms and causes**, which is the part that is expensive to rediscover.

Almost all of it traces to one root cause:

> `deb2pkg` deliberately drops Debian maintainer scripts, because they assume
> dpkg, debconf and Debian semantics pacman does not provide. Everything a
> `postinst` would have done therefore never happens.

That is not a marginal set of tasks. It removes commands entirely, breaks TLS,
and leaves the system with no pseudo-terminals.

---

## 0. The structural fix: pacman hooks

Items 1, 3 and most of 9 below are *symptoms* of the dropped maintainer
scripts. Repairing them by hand after each install does not scale — a desktop
would bury you in stale caches. `/etc/pacman.d/hooks/` is the right place:

| hook | trigger | repairs |
|---|---|---|
| `10-ldconfig` | `usr/lib/*.so*` | shared library cache |
| `20-ca-certificates` | `usr/share/ca-certificates/*` | conf + CA bundle (§3) |
| `30-alternatives` | `usr/bin/*-hurd`, `*.procps` | the missing commands (§1) |
| `40-glib-schemas` | `glib-2.0/schemas/*.xml` | `gschemas.compiled` |
| `50-mime` | `mime/packages/*.xml` | MIME database |
| `60-desktop` | `applications/*.desktop` | desktop database |
| `70-icon-cache` | `icons/*/index.theme` | GTK icon caches |
| `80-fontconfig` | `usr/share/fonts/*` | `fc-cache` |

Handlers live in `/usr/local/sbin/oryx-*`.

> **Every handler must `exit 0` even on failure.** A failing pacman hook
> aborts the entire transaction, which is far worse than a stale cache. They
> also no-op silently when their tool is not installed, so the desktop hooks
> sit harmless on a console-only system.

**Verified** by deleting `/etc/ssl/certs/ca-certificates.crt` and
`/etc/ca-certificates.conf`, then reinstalling an unrelated package: the hooks
rebuilt the 121-CA bundle unprompted and HTTPS to PyPI returned 200 again.

This is what lets *any* Debian hurd-i386 package be installed and just work —
the premise of the whole distribution, and the thing the original source-built
Arch Hurd could never have.

## 1. Commands that simply do not exist

**Symptom.** `ps`, `uptime`, `vmstat`, `reboot`, `halt`, `poweroff`, `kill`,
`fakeroot` — not broken, *absent*. `uptime: command not found` on a system
where procps is installed.

**Cause.** On hurd-i386 these are all `update-alternatives` links. The `hurd`
package ships `ps-hurd`, `uptime-hurd`, `vmstat-hurd`, `reboot-hurd`,
`halt-hurd`, `poweroff-hurd`; procps ships `ps.procps`, `kill.procps`, … The
plain names are created by postinst, which never ran.

**Fix.** `oryx-postinst.sh alternatives`. Prefers the `-hurd` variants, which
is Debian's intent on this architecture — Hurd's own `ps` understands Mach
tasks properly. **Expect this for every future converted package.**

**Related trap.** `ps-hurd` does not list every process — it showed no gettys
while `ps.procps` showed three, and it omits PID 1. Cross-check with
`ps.procps` before concluding something is dead.

---

## 2. No pseudo-terminals → no interactive login

**Symptom.** `ssh root@host` fails with `PTY allocation request failed on
channel 0`. Authentication succeeds; the session cannot start.

**Cause.** MAKEDEV was never run for ptys, so `/dev` held only the physical
`tty1`–`tty6`. **Non-interactive `ssh host 'cmd'` needs no pty and always
worked**, which is why this hid through an entire build and only surfaced when
a human first tried to log in.

**Fix.** `cd /dev && ./MAKEDEV ptyp ptyq` — a bank each, 64 pairs. Naming a
single node (`ptyp0`) makes just that one. Passive translators on disk, so
they survive reboots.

---

## 3. TLS is entirely broken

**Symptom.** Every HTTPS client fails. pip dies with a bare
`FileNotFoundError: [Errno 1073741826]` out of `certifi.where()`.

**Cause.** `ca-certificates`' postinst generates **both**
`/etc/ca-certificates.conf` and the concatenated bundle
`/etc/ssl/certs/ca-certificates.crt`. Neither exists without it. Debian
patches `certifi` to point at the system bundle, so the failure surfaces far
from its cause. Running `update-ca-certificates` alone does nothing — it reads
the `.conf` that is also missing.

**Fix.**
```sh
cd /usr/share/ca-certificates && find . -name '*.crt' | sed 's|^\./||' | sort \
    > /etc/ca-certificates.conf
update-ca-certificates
```

---

## 4. pacman reports zero packages

**Symptom.** `pacman -Q` prints nothing, exit code 0, while ~160 packages are
installed and `/var/lib/pacman/local` is full.

**Cause.** pacman is built `--prefix=/usr/local`, so its compiled-in `DBPath`
is `/var/local/lib/pacman/`. The real database is `/var/lib/pacman/`, because
that is where it was installed from the build host with `--root`/`--dbpath`.
The stock `pacman.conf` has every path commented out, so the compiled defaults
win. Check `pacman -v` if `-Q` ever comes back empty.

**Also:** `CheckSpace` needs `/etc/mtab`, which GNU/Hurd does not have —
`could not determine filesystem mount points` then `not enough free disk
space`. And pacman fails hard if `GPGDir` does not exist, even with
`SigLevel = Never`; it will not create it.

---

## 5. Read-only root and manual networking every boot

**Symptom.** `/` is read-only after login; networking has to be rebuilt by
hand with `MAKEDEV netdde` + `settrans` after every boot.

**Cause.** `/etc/rcS.d` was **empty**. Nothing remounted the root. And
`/servers/socket/2` still carried Debian's stock passive translator
`/hurd/pfinet -6 /servers/socket/26` — no address at all, and IPv6 on.

**Fix.** `rcS.d/S05hurd-rw` for the remount. For networking, set the passive
translator to the real static config with **`settrans -cp`** (passive only) —
then it starts on demand with no init script at all. `/dev/netdde` already had
its translator; the `MAKEDEV netdde` step was never needed.

**IPv6 must stay off.** If both `socket/2` and `socket/26` have translators,
two pfinets start, fight over `socket/2`, and one dies with
`socket: Translator died`. sshd needs `AddressFamily inet`.

---

## 6. Console keyboard layout never applied

**Symptom.** `/etc/default/keyboard` says `de`; the console stays US.

**Cause.** `hurd-console` was wired into **no runlevel at all**, so it never
ran. Two things were needed:

1. `cd /dev && ./MAKEDEV vcs` — `/dev/vcs` did not exist, so the daemon timed
   out with `Could not receive return value from daemon process` and the
   `/dev/ttyN` translators pointed at a path that was not there.
2. Wire it into **`rcS.d`, NOT `rc2.d`** despite its LSB header saying
   `Default-Start: 2 3 4 5`. init spawns the gettys on entering runlevel 2 and
   they need `/dev/vcs/N/console` to exist already; starting the console at
   sysinit wins that race. It goes *after* the read-write remount because it
   writes `/var/run/console.pid`.

---

## 7. Console font — and a genuine one-way trap

The vga driver takes **no font options**. It only opens
`/usr/share/hurd/vga-system.bdf`, which ships as a **dangling symlink** to
`/usr/src/unifont.bdf` that no installed package provides — so it falls back
to its built-in 8x16 font.

VGA text mode fixes the cell width at 8px, so font **height** sets the rows:
**8 → 50, 14 → 28, 16 → 25.**

> ⚠️ **Shrinking applies live. Enlarging needs a REBOOT.** Going to 8x8 worked
> instantly and `/dev/vcs/1/display` confirmed 80x50. Going back did *not*
> restore 80x25: the driver reads the VGA mode already programmed instead of
> reprogramming the row count, and the console server keeps the geometry its
> virtual consoles were created with. Restarting the client, and even
> `settrans -fgap /dev/vcs /hurd/console` to restart the server, both left it
> at 80x50. VGA text mode only resets at POST.

Verify geometry by reading the `cons_display` header:
`od -A d -t u4 -N 24 /dev/vcs/1/display` — fields 4 and 5 are width and height.

Debian ships `bdf2psf` but not the reverse, and the Hurd console reads BDF
only, so `bin/psf2bdf.py` converts Linux console PSF fonts (handles PSF1 and
PSF2 including their Unicode tables).

---

## 8. Packaging traps

- **`tar -cf - .` produces `./usr/bin/foo` member paths, and pacman installs
  such a package as EMPTY** — no error, no files, and it still registers in
  the database. Members must be `usr/bin/foo`, `.PKGINFO` first. Always check
  `pacman -Ql <pkg> | wc -l` after installing something new.
- **pacman will not write a directory over a symlink.** A `.deb` shipping a
  real `/bin` (psmisc) or `/sbin` (libc-bin) aborts with `/bin exists in
  filesystem (owned by base-files)`. The converters usr-merge payloads first.
- **Some installed packages are local Hurd rebuilds (`+hurd.1`) whose `.deb`
  is no longer downloadable.** `procps` is one: sid's 4.0.7 needs
  `libproc2.so.1`, the working local build 4.0.4 provides `libproc2.so.0`.
  Use `bin/installed2pkg.sh` to repack from a Debian host's installed files.
- **`iputils-ping` has no hurd-i386 build** — `ping` comes from
  `inetutils-ping`. Asking apt for an unavailable package aborts the whole
  `apt-get download` batch.
- **`ldd` on a mounted target lies.** It runs the *host's* loader and resolves
  against the host's `/lib`, so everything looks present. Read `DT_NEEDED`
  statically — `bin/depsweep.sh`.

---

## 9. fastfetch

- **A config without a `modules` key prints the logo and NOTHING else.** It
  does not fall back to the default module set.
- **Logo type must be `file-raw`, not `file`,** for art containing literal
  `$`. `file` treats `$1`–`$9` as colour placeholders and eats them, shifting
  the art out of alignment.
- **fastfetch's package detection finds nothing on GNU/Hurd.** The count is
  read from the pacman database directly via a `command` module.
- Colours are auto-disabled when stdout is not a TTY, which makes testing over
  a pipe misleading.

---

## 10. Shutdown

Hurd cannot remount `/` read-only at shutdown, so the clean flag is never set
and **every boot forces an fsck**. Unavoidable, and not a sign of damage.

The real danger is an unclean halt *after heavy writes*: one large install
followed by a bare `reboot` produced multiply-claimed blocks and ~160
unattached inodes. Always use `safe-reboot` / `safe-halt` / `safe-poweroff`.

**Power-off does not complete on the T400.** It runs correctly all the way —
every server acknowledges, including `ext2fs part:3:device:wd0 of halt...done`
— then hangs at `hwsleep-0079 hw_legacy_sleep : Entering sleep state [S5]`.
Holding the power button there **is safe**: ext2fs has already halted and the
disk is idle. Wait ~15 s after the S5 line; if it has not powered off by then
it will not.

---

## 11. Hardware, T400-specific

- **Must boot `gnumach-1.8-486-smp.gz`.** ACPI routes SATA and Ethernet to the
  same legacy IRQ 11; `rumpdisk` and `netdde` are userspace and cannot share
  it, so the disk dies with `wd0d: device timeout` the moment networking
  starts. The SMP kernel brings up the IOAPIC. **If that error reappears,
  check `uname -a` FIRST.**
- **Must pass `noide noahci`** so rumpdisk takes over; Mach's own IDE/AHCI
  drivers do not work on this machine.
- WiFi is not usable: `netdde` is a DDE port of Linux 2.6-era *wired* drivers
  with no mac80211/cfg80211.

---

## 12. The translator hazard

**This took the build host's network down three times.**

On a *mounted* target, creating or merely *touching* a node with a passive
translator **starts that translator in the RUNNING system**. Creating
`/dev/netdde` on a mounted target started a second netdde that grabbed the NIC.

- `settrans -cp` — passive only. Safe.
- `settrans -c` — active **and** passive. **Starts it.**
- `chmod`, `: > file`, `ls -l <node>`, a stat-ing glob — all start it.
- `showtrans` uses `O_NOTRANS` and is safe. `settrans -fg` clears passive too.

**Rule: do device-node work from INSIDE the target, not from the host.**

Two related quirks: `find /mnt` does not descend across the mountpoint (use
`find /mnt/.`), and `settrans -a /mnt /hurd/ext2fs -w <dev>` **silently
ignores `-w`** — run `fsysopts /mnt --writable` after mounting or pacman
cannot lock its database.

---

## 13. The repository

Added 2026-09-20. `[oryx]` in `/usr/local/etc/pacman.conf`, served from the
Dell by `bin/oryx-serve.sh`. Dependencies are written into the packages by
`bin/oryx-setdeps.py`, then indexed by `bin/oryx-repo-add.py`.

### The one that cost the most: "invalid or corrupted package"

**pacman requires a package's `.PKGINFO` dependencies to match its database
entry exactly.** When they differ it says:

```
:: File /var/cache/pacman/pkg/tree-...pkg.tar.zst is corrupted
   (invalid or corrupted package).
```

The file is **not corrupt**. Everything about it checks out: the sha256
matches the database, `zstd -t` passes, `tar -t` lists it, and `pacman -U` on
that very same file installs it without complaint. Only `pacman -S` fails,
because only `-S` consults the sync database and compares.

The real reason appears **only under `--debug`**, one line, immediately
before the misleading message:

```
debug: internal package depends mismatch
```

**When a package is reported corrupt, run `pacman -S --debug <pkg>` and read
the line before the error.** Note also that `pacman -Sw` (download only)
*succeeds*, because it validates checksums but never loads the packages —
so a working `-Sw` proves nothing about `-S`.

Consequences that follow from that equality check:

- Dependencies cannot be pruned in the database to make them satisfiable.
  Dropping an unsatisfiable dependency from the database while the package
  still declares it produces precisely this failure. Drop it in **both**, or
  in neither.
- There is exactly one source of truth: the package. `oryx-setdeps.py` writes
  dependencies into `.PKGINFO`; `oryx-repo-add.py` copies them out into the
  database. Running them in the other order reintroduces the mismatch.

### Choosing dependencies needs to know what the repo holds

The converter cannot do it from one `.deb`. Debian's tmux depends on

```
systemd | systemd-standalone-tmpfiles | systemd-tmpfiles, libc0.3, ...
```

and taking the **first** alternative — which is what apt does, and what a
naive `sed 's/|.*//'` does — yields `systemd` on a system that will never
have one. `oryx-setdeps.py` sees the whole repository and picks the
alternative something in it actually provides: `systemd-tmpfiles`, supplied
by `seedfiles`. Version constraints are dropped, because `deb2pkg` mangles
Debian versions (`1:2.4-3~bpo` -> `1.2.4.3.bpo-1`) in a way that does not
preserve ordering, so a constraint cannot be evaluated correctly.

### Conversion must be reproducible

`deb2pkg` originally stamped `builddate = $(date +%s)`, so converting the
same `.deb` twice produced **different bytes under the same filename and
version**. Every stale copy — the target's `/var/cache/pacman/pkg` above all
— then failed its checksum, reported as `invalid or corrupted package
(checksum)`. This masqueraded as several different bugs during the repo work
and sent the investigation down two wrong paths.

`builddate` now comes from the ar header of the `.deb`'s first member (8-byte
global magic, then a 60-byte header whose mtime is 12 decimal bytes at offset
16), and the tar is built with `--sort=name --mtime --owner=0 --group=0` over
a sorted member list. Same `.deb` in, identical package out. **After changing
package bytes, clear the target's cache (`pacman -Scc`)** or it will keep
validating the old ones.

### Smaller things

- **`libalpm` does the downloading, not the `pacman` binary.** `readelf -d`
  on `/usr/local/bin/pacman` shows no libcurl and looks like a dead end. The
  HTTP support is in `/usr/local/lib/i386-gnu/libalpm.so.16`, which needs
  `libcurl-gnutls.so.4` — Debian's renamed soname, and it is present. Check
  the library, not the executable.
- **The Dell has no pacman**, so no `repo-add`. Debian's
  `pacman-package-manager` would supply one, but the database format is small
  and stable, so `oryx-repo-add.py` writes it directly instead of adding a
  package manager to the build host for the sake of one shell script.
- ⚠️ **A package the target has a local rebuild of must never be indexed.**
  Seeding from the Debian index pulled in stock `procps` 4.0.7, which showed
  up in `pacman -Qu` as an upgrade over the working `+hurd.1` build (§8);
  taking it would have removed `ps`, `uptime` and `vmstat`. Deleting the file
  by hand does **not** hold — its `.deb` is in `debs/`, so the next converter
  run puts it straight back. It is listed in `repo/oryx-exclude.txt`, and
  `IgnorePkg = procps libproc2-0` on the target is the second line of
  defence. **After any reseed, check `pacman -Qu` before `-Syu`.**
- **`$OUT` used after `cd` needs to be absolute.** `deb2pkg-dell.sh` builds
  the tar from inside the work directory, so a relative outdir resolved
  against *that*, wrote the package into the temp tree, and deleted it —
  reporting success and exit status 0 the whole time. 119 packages
  "converted", none written.
- **`repo-add` semantics that matter:** one database entry per package name,
  so two versions in one directory means picking (newest `builddate` wins).
  `%CSIZE%` is the compressed file, `%ISIZE%` is `.PKGINFO`'s `size`.
- Two installed packages are **not** in the repo and cannot be: `procps`
  (excluded on purpose) and `libproc2-0`, whose soname is gone from sid.
  That is the `installed2pkg.sh` case and needs a Debian host.

---

## 14. System users — and how this locked the box out

Added 2026-09-20, after it cost remote access to the machine.

**Symptom.** After upgrading `openssh-server` 10.2 → 10.5, sshd accepts the
TCP connection and its child dies before sending a banner:

```
kex_exchange_identification: read: Connection reset by peer
Connection reset by 192.168.2.182 port 22
```

No banner, no log the client can see, port still open. Only console access
recovers it.

**Cause.** openssh-server 10.5 needs a privilege-separation user, declared in
`/usr/lib/sysusers.d/openssh-server.conf`:

```
u sshd -:nogroup "sshd user" /run/sshd
```

Debian creates that user from its maintainer script / sysusers fragment. The
converter drops maintainer scripts, nothing on Oryx ran `sysusers.d`, and
`base-passwd`'s masters do not contain `sshd` either — they carry only the
base set. So the user never existed. The previous openssh was hand-installed
and did not need it.

**Fix at the console:**

```sh
/usr/bin/systemd-sysusers          # from opensysusers; reads sysusers.d
mkdir -p /run/sshd && chmod 0755 /run/sshd
/etc/init.d/ssh restart
```

A plain reboot does **not** fix it: `/etc/init.d/ssh` creates `/run/sshd`,
but nothing creates the user.

**Structural fix.** `90-sysusers.hook` + `oryx-sysusers` now run
`systemd-sysusers` after any package that ships a `usr/lib/sysusers.d/*`
fragment. This was the last known gap in the postinst emulation of §0, and
the one the README used to list as not covered. dbus's `messagebus` user is
covered by the same mechanism.

### ⚠️ The procedural lesson, which matters more than the cause

**Never upgrade sshd over ssh on this machine.** The reasoning that made it
look safe was wrong:

> "Upgrade the package but do not restart sshd. The running master process
> keeps serving from the old binary in memory, so the new one can be tested
> on a spare port first."

**sshd re-execs its own binary for every new connection.** The upgrade takes
effect on the very next connection, before any test can run. A running daemon
is not protection when the binary underneath it changes.

SSH is the only route into the T400 — port 22 is the only thing listening, and
booting needs a GRUB entry picked by hand at the keyboard. Anything touching
sshd, glibc, pfinet, netdde, `/servers/socket/*` or the boot path should be
done with someone at the machine, or arranged to take effect only on the next
boot so a bad change is found while access still works.

---

## 15. Reticulum on the mesh — rnsd has no daemon mode

Added 2026-09-20. `python3-rns`, `python3-lxmf`, `nomadnet` and their
dependencies (`python3-urwid`, `python3-wcwidth`, `python3-qrcode`,
`python3-cryptography`, `python3-bcrypt`, `python3-cffi-backend`,
`python3-serial`, `python3-typing-extensions`) are packaged and installed.
`/root/.reticulum/config` runs it as a leaf (`enable_transport = No`) with a
single `TCPClientInterface` to the Dell's shared instance at
`192.168.2.210:4242`. `rnstatus` on the T400 shows that interface `Up`.

**None of this is in Debian hurd-i386** — RNS, LXMF and NomadNet are pure-Python
PyPI wheels, so `deb2pkg-dell.sh` has nothing to convert. `bin/oryx-wheel2pkg.sh`
exists for exactly this: it unzips a `py3-none-any` wheel, moves any
`.data/scripts/` into `usr/bin`, and — because pip is what normally generates
console-script wrappers from `entry_points.txt`, and nothing runs pip on the
target — writes those wrappers itself, or `rnsd`/`nomadnet` would not exist
after `pacman -S`. Byte-compiling is deliberately skipped: the Dell (3.13) and
target (3.14) CPython versions do not share a `.pyc` magic number, and Python
falls back to source silently, so leaving `__pycache__` out is correct, not a
gap.

**Symptom.** `rnsd` was started by hand for the first test and stayed in the
foreground even with `-s` (`--service`, meant to send it to a log file). It
has no `--daemonize` and writes no pidfile.

**Cause.** rnsd simply does not implement backgrounding — `-s` only changes
*where* it logs, not whether it forks. This is normal for a PyPI console
script; Debian packages that need a real daemon supply their own init
integration, which a wheel obviously does not.

**Fix.** `oryx-postinst.sh rnsd` installs `/etc/init.d/oryx-rnsd`, using
`start-stop-daemon --background --make-pidfile` to supply both, and wires it
into `rc2.d` (`S25oryx-rnsd`, after `S20ssh` — this box only populates
`rc2.d`, not `rc3.d`–`rc5.d`, see §6's boot-scripts note). Verified live: kill
the hand-started process, `/etc/init.d/oryx-rnsd start`, `rnstatus` shows the
TCP interface back `Up` within 3 seconds — RNS's `TCPClientInterface` retries
every 5s with no retry limit, so the Dell side needs nothing.

**Related trap: neither `pidof rnsd` nor `ps.procps -C rnsd` can find it.**
`/usr/bin/rnsd` is a `#!/usr/bin/python3` script. The kernel execs *python3*
as the real image, so every process-table view of it — comm, `/proc/*/exe`,
whatever `-C` matches against — says `python3`, never `rnsd`. Track it by
PIDFILE only. This will bite the same way for `nomadnet` if it is ever run as
a background node.

**Not exploited, but worth knowing: a second `rnsd` cannot collide.** RNS's
shared-instance design means a process that loses the bind race on
`shared_instance_port` just becomes a *client* of the one that won
(`Reticulum.py`, `is_connected_to_shared_instance`), instead of erroring. The
init script does not rely on this — `fix_rnsd` never starts or stops a process
it did not start itself, checking `rnstatus` for an existing shared instance
first, precisely so applying it can never drop a live interface.

**Not yet proven across a real boot** — same caveat as `oryx-swap` (§4). The
box has not been rebooted since this was installed.

### The `pkill -f` self-match footgun (this cost the SSH session mid-test)

`pkill -f "/usr/bin/rnsd"` was run **over one `ssh host '...'` invocation**
that also *contained the string `/usr/bin/rnsd` in its own command line* (the
heredoc being executed had that pattern in it, e.g. in an echoed message).
`pkill -f` matches the full command line of every process, including the
remote `bash -c '...'` process running the ssh command itself — which killed
its own shell mid-script, dropping the SSH connection (exit 255) before the
rest of the script ran. `sshd` and the target were both fine; only that one
remote subshell died.

**Rule: when killing a process by pattern over SSH, never let the pattern
string also appear in the command you send.** Use a narrower pattern (a PID,
or `pkill -f` against something the wrapper command does not itself mention),
or put the kill in its own bare `ssh host pkill ...` with nothing else in the
same invocation.

---

## 16. i2pd — a real Debian hurd-i386 build, two postinst gaps

Added 2026-09-20. `i2pd` is a genuine Debian hurd-i386 build (v2.59.0, pure
C++, no TUN/TAP needed -- unlike Yggdrasil it works purely through local
proxies and TCP/UDP transports). `oryx-install.sh i2pd` converts and installs
it like any other package. Two gaps, both the usual dropped-maintainer-script
root cause, but two DIFFERENT postinst mechanisms:

**1. No system user.** i2pd depends on plain `adduser`, not a
`usr/lib/sysusers.d/*` fragment, so the `90-sysusers.hook` (§14) cannot catch
it -- there is no declarative fragment to read, only the deleted postinst
script's `adduser --system i2pd` call. `/etc/init.d/i2pd` chuids to a user
`i2pd` that silently does not exist, so `start-stop-daemon --chuid` fails to
even test-start. Fixed in `oryx-postinst.sh i2pd` with an explicit
`useradd --system --home-dir /var/lib/i2pd ...` -- this home directory choice
matters, see below. Expect this for any future package whose postinst
creates a user the classic `adduser` way rather than via sysusers.d.

**2. `--chuid` does not export `$HOME` -- i2pd reseeds as root, never as
its own user, with NOTHING logged about why.** Started by hand as root with
an explicit `--datadir=/var/lib/i2pd`, i2pd reseeds within a few seconds:
"Reseed: 14 certificates loaded", dozens of `NetDb: RouterInfo added`,
tunnels built. Started via the real init script (`start-stop-daemon --chuid
i2pd`, no `--datadir` -- exactly how Debian ships it), i2pd starts, opens its
proxies, looks completely healthy, and then NEVER reseeds: no `Reseed:` log
line at ANY log level, netDb stays at 0 files, every tunnel build fails
"no peers available" forever. **This is not a crash and logs nothing** --
easy to conclude the daemon just needs more time, when actually it is reading
and writing an entirely different, unintended directory than the one being
inspected.

**Cause:** the shipped init script's `DAEMON_OPTS` never passes `--datadir`,
so i2pd resolves it from `$HOME`. `start-stop-daemon --chuid` changes
uid/gid but does **not** reliably export a new `$HOME` for the target user
the way a real login would -- so i2pd silently keeps whatever `$HOME` the
init script's own environment had (root's, or unset), not `/var/lib/i2pd`.

**Fix:** pin `--datadir=/var/lib/i2pd` explicitly, via `/etc/default/i2pd`
-- the init script's own sanctioned override point, sourced *after*
`DAEMON_OPTS` is first assigned, so the override must **append**
(`DAEMON_OPTS="$DAEMON_OPTS --datadir=..."`) rather than reassign. Editing
the shipped `/etc/init.d/i2pd` directly would work too, but a reinstall would
silently revert it -- `/etc/default/<name>` is the durable place. Verified:
after this fix, the same `start-stop-daemon --chuid` startup path reseeds
within 12 seconds -- 95 routers, 57 floodfills, 10 client tunnels built.

**Diagnostic technique worth keeping:** when a daemon looks healthy but
inert, and normal logs show nothing, run it BY HAND with the exact same
flags plus `--loglevel=debug --log=file --logfile=<scratch>` and compare.
The difference between "reseeds in seconds" (manual, as root, explicit
`--datadir`) and "silent forever" (init script, chuid, no `--datadir`)
pinpointed this in two comparisons, with zero source-reading required.

**Custom Oryx config** (`oryx-postinst.sh i2pd`, appended to `i2pd.conf`):
local HTTP/SOCKS proxies enabled on `127.0.0.1` (4444/4447) and the web
console on `127.0.0.1:7070`, `bandwidth = L` + `share = 50` so a T400 stays a
light participant rather than a heavy transit relay -- same call already
made for `rnsd`'s leaf config. **TRAP:** i2pd's ini parser attributes any
global (non-sectioned) key to whichever `[section]` precedes it in the file.
Appending `bandwidth = L` at end-of-file landed it under the file's last
section, `[persist]`, producing the nonexistent option `persist.bandwidth`
and a refusal to start. Global keys must be inserted before the FIRST
`[section]` header -- `oryx-postinst.sh` does this with a small python3
script rather than a blind `cat >>`. Also tried and reverted: `nickname` is
not a real i2pd.conf option (checked against `i2pd --help`) -- verify option
names against `--help` before writing config, `i2pd.conf.d`'s commented-out
stub sections in the shipped default do not enumerate all valid keys.

## 17. Yggdrasil — a real attempt, blocked by gccgo's missing generics support

Investigated 2026-09-20. Not shipped. Recorded in full because getting to the
real blocker took genuine work, and the path there rules out several
plausible-looking dead ends worth not re-walking.

**The premise looked sound.** Yggdrasil can run headless (`IfName: none` in
its config) as a pure mesh router/relay with no local network presence --
confirmed against upstream docs -- which sidesteps the fact that GNU/Hurd has
no TUN/TAP translator at all (same root blocker as Mullvad/WireGuard/OpenVPN,
see the README roadmap). Go itself has no Hurd port, but Debian's Go team
already builds OTHER Go programs for hurd-i386 via **gccgo** (GCC's Go
frontend) instead of upstream Go -- `kcptun` is a real, installed, working
example. So: buildable in principle, just not via the normal path.

**Bug found and fixed: yggdrasil's own "unsupported platform" fallback does
not compile on unsupported platforms.** `src/tun/tun_other.go` (build tag
`!linux && !darwin && !ios && !android && !windows && !openbsd && !freebsd
&& !mobile` -- exactly GNU/Hurd) unconditionally calls
`wgtun.CreateTUN(ifname, mtu)` from `golang.zx2c4.com/wireguard/tun`, whose
`CreateTUN` is defined only in that package's 5 platform-specific files
(`tun_linux.go`, `_darwin.go`, `_freebsd.go`, `_openbsd.go`, `_windows.go`,
verified against the exact upstream commit yggdrasil's `go.mod` pins). So the
catch-all for "unsupported" platforms fails to compile on every genuinely
unsupported platform. Harmless at runtime: `tun._start()` (`src/tun/tun.go`)
already skips calling `setup()` entirely when `IfName` is `"none"`/`"dummy"`,
so a real implementation is never reached in headless mode -- only the
compiler needs something. **Fix:** replace the file's body with a stub that
returns an error and drops the `wgtun` import entirely (`tun.go` itself only
needs `wgtun.Device`, the platform-independent interface type, which lives in
wireguard's own generic `tun.go` and needs none of the OS-specific files).
This is a legitimate small patch, upstreamable as-is.

**No `go` build tool exists for hurd-i386 via the normal package** --
`golang-any` depends on `golang-go`, which Debian has never built for this
architecture (`golang-any` is therefore currently uninstallable there) -- but
`gccgo-14`/`gccgo-16` each ship their own bundled tool as `/usr/bin/go-14` /
`go-16`, discovered only by checking `dpkg -L gccgo-14` directly (the
Debian Go Packaging Team's normal `dh-golang`+`golang-any` pipeline is a dead
end here; this bundled tool is a separate, undocumented escape hatch). It
does real GOPATH-mode dependency resolution and GOOS-aware file selection --
`go-14 env GOOS` correctly reports `hurd`, and `golang.org/x/sys/unix`
genuinely has dedicated `syscall_hurd.go` / `syscall_hurd_386.go` files, so
the syscall layer is real, intentional upstream support, not a lucky
accident.

**Two stdlib gaps found via real build errors, both fixed narrowly:**
- `crypto/ecdh` (added Go 1.20) missing from BOTH gccgo-14 and gccgo-16's
  bundled stdlib -- GCC's Go standard library mirror lags upstream by
  several Go releases regardless of GCC version; a newer GCC major version
  is not a fix. Blocks `golang.org/x/crypto/curve25519`, which since ~Go
  1.20 is purely a wrapper around `crypto/ecdh`. Fixed by vendoring
  `curve25519` from x/crypto v0.17.0 instead, which still carries the
  pre-ecdh pure-Go implementation behind a `!go1.20` build tag
  (`curve25519_compat.go` vs `curve25519_go120.go`) -- gccgo's reported
  compatibility level (`go1.18`) selects the compat file automatically.
- `"slices"` (added Go 1.21) also missing -- needed by
  `src/admin/getpaths.go`'s one call to `slices.SortStableFunc`. A one-off
  local package at `$GOPATH/src/slices` providing just that function (via
  `sort.SliceStable`) would have closed this -- except:

**The actual, fatal blocker: gccgo's Go frontend does not implement
generics, full stop -- independent of any library or GOOS question.**
Verified with the smallest possible isolated test, unrelated to yggdrasil:
```go
func Max[T int | float64](a, b T) T { if a > b { return a }; return b }
```
fails to even PARSE on both gccgo-14 and gccgo-16 (`expected '('`, `expected
']'`) -- not a missing stdlib symbol, a language-level gap. This sank the
attempt at `github.com/Arceliar/phony`, the actor-model concurrency
primitive underneath BOTH `ironwood` (yggdrasil's DHT/routing engine) and
yggdrasil itself: its lock-free queue uses `atomic.Pointer[queueElem]` and
`atomic.Bool` (generic stdlib types, Go 1.19). `phony` is small and
self-contained enough that rewriting its queue with pre-generics primitives
(an `unsafe.Pointer` + `atomic.CompareAndSwapPointer` loop, roughly how
`sync/atomic.Pointer[T]` is implemented under the hood) is plausible as a
follow-up, but every other current or future dependency using generics
(pervasive in Go code written after ~2022) would hit the identical wall --
this is a standing limitation of the toolchain, not a per-package bug to
patch around one at a time.

**Also excluded, deliberately, before reaching the generics wall:** QUIC and
WebSocket transport support (`src/core/link_quic.go`, `link_ws.go`,
`link_wss.go`, moved to `_excluded/`, with the handful of `case "quic":` /
`l.quic` references in `link.go` removed to match). `quic-go` is only
available in the archive under its old pre-rename identity
(`github.com/lucas-clemente/quic-go`, a much older, likely
API-incompatible version) and `github.com/coder/websocket` is not packaged
at all (only its old `nhooyr.io/websocket` predecessor is, a different
import path). TCP/TLS transport -- the same kind of link Reticulum already
proved works fine on Hurd -- is unaffected; this trims an optional feature,
not the daemon's ability to peer.

**If this is revisited:** check whether a newer GCC (17+, whenever it lands)
adds generics to gccgo's frontend before attempting the `phony` patch --
that would remove the standing limitation instead of working around one
instance of it. The staged source tree, the `tun_other.go` patch, and the
curve25519/slices workarounds are otherwise a complete, working recipe up to
that point.

## 18. Reticulum over I2P — works, two gotchas

Added 2026-09-20. `rnsd`'s `[[I2P]]` interface (`type = I2PInterface`) rides
on i2pd's SAM bridge (`RNS.vendor.i2plib` speaks SAM v3, bundled with
`python3-rns` — nothing extra to install). Verified end-to-end between the
T400 and the Dell:

**Timeline, second attempt (first attempt's SAM wasn't up yet — see below):**
- `oryx-postinst.sh i2pd` (enables `[sam]`) + `oryx-postinst.sh rnsd` (adds
  `[[I2P]]`) + `/etc/init.d/oryx-rnsd restart`: instant, both sides.
- SAM bridge listening: instant (plain TCP accept, verified by raw connect).
- Both sides generate a real I2P b32 destination immediately on startup —
  visible in `rnstatus` right away — but the actual tunnel takes **1–3
  minutes** to go from `Down`/"Creating Tunnel" to `Up`/"Tunnel Active". The
  Dell's i2pd had over a week of uptime (warm netDb); a cold i2pd (like the
  T400's first-ever start, see FIXES.md 16) will likely take longer. **Do
  not judge failure before at least 3–5 minutes.**
- End-to-end LXMF delivery, T400 → Dell, **with the T400's TCP interfaces
  disabled** (proving the I2P path alone, not a fallback): delivered well
  within a 200-second test window once the tunnel was already `Up`.

**Gotcha 1: a missing/slow SAM bridge does not stop rnsd, and does not need
to.** Confirmed by stopping i2pd entirely and restarting `oryx-rnsd`: the
daemon started normally, the TCP interfaces stayed `Up`, and the log showed
a clear, self-explanatory retry loop —
```
Error while while configuring I2PInterface[I2P]: [Errno ...] Connect call failed ('127.0.0.1', 7656)
Check that I2P is installed and running, and that SAM is enabled. Retrying tunnel setup later.
```
Restarting i2pd afterward brought the interface back up **without restarting
rnsd** — no manual intervention needed either way. `panic_on_interface_error
= No` (already set, see FIXES.md 15) is what makes this graceful rather than
fatal.

**Gotcha 2: init ordering was backwards, and nothing caught it until now.**
`oryx-i2pd` was `rc2.d/S30`, *after* `oryx-rnsd`'s `S25` — meaning every boot
guaranteed rnsd's first I2P connection attempt would fail (SAM not up yet)
before the retry loop above eventually recovered it. Harmless only because
of gotcha 1's resilience; still wrong, and now fixed: **i2pd is `S22`,
before rnsd's `S25`.** `oryx-rnsd`'s own `Required-Start` LSB header now
lists `i2pd` too, though it is documentation only — this box has no
`insserv`, ordering is purely the `S-NN` prefix (see FIXES.md 6).

**Gotcha 3 (observed, not fixed): the I2P destination address is NOT
persisted across `rnsd` restarts.** Three restarts of the T400's `rnsd`
during testing produced three different b32 addresses
(`feb2oll2...`, `c3k6k3i...`, `w63xz7e...`), confirmed by new keyfiles
accumulating under `~/.reticulum/storage/i2p/*.i2p` each time, unlike
i2pd's own `router.keys`/`router.info` which persist fine (FIXES.md 16).
**Practical consequence:** `peers = <b32>` in one node's config only stays
valid as long as the OTHER node's `rnsd` hasn't restarted since. In this
test the Dell was never restarted, so the T400's `peers=` entry (pointing at
the Dell) stayed valid throughout; had the roles been reversed, every T400
restart would need the Dell's config updated with a fresh address. Not
investigated further — worth revisiting if a stable pinned peer matters more
than it did for this test (the interface still connects and is reachable
without `peers=` once a path exists via announce/DHT, same as any other RNS
interface; `peers=` is only needed to *initiate* the first connection to a
peer with no other path to it yet).

**Config mechanics, both boxes:** i2pd's ini parser attributes global keys
to whichever section precedes them (see FIXES.md 16) — `oryx-postinst.sh`
already inserts `bandwidth`/`share` before the first `[section]`; SAM is a
plain appended `[sam]` section, no such trap. `oryx-postinst.sh`'s i2pd.conf
management was changed from "apply once, skip forever" to "strip by exact
marker, then regenerate" specifically so adding SAM didn't require a special
case for boxes that already had the old-style single-marker block — restored
the live file from the cached `.pkg.tar.zst`'s pristine copy once, by hand,
to migrate cleanly (see the technique in FIXES.md 16's "diagnostic
technique" note; same trick, reused).

## 19. oryx-get — a package fetcher with no Oryx server involved

Added 2026-09-20. `oryx-get <pkg>` fetches a package from whichever
configured source has it and makes it installable, running entirely ON the
T400 -- no Dell, no HQ machine, nothing else in the loop. Lives at
`/opt/oryx-get/` (packaged as `bin/oryx-get/` in this repo), symlinked as
`/usr/local/bin/oryx-get`. Config: `/etc/oryx-get.conf`.

**Architecture: one shared converter core, pluggable sources.**
`oryxget/convert.py` is `bin/deb2pkg-dell.sh` and `bin/oryx-wheel2pkg.sh`
ported to Python, both funneling into the same packaging step
(`build_pkg_tarball`, ported from `bin/oryx-mkpkg.sh`). `oryxget/sources.py`
+ `oryxget/debian_source.py` implement `type = repo | pypi | debian` against
a common `Candidate` interface; adding a new source type later is a new
class + a registry entry, never a change to the CLI. `debindex.py` and
`oryx-repo-add.py` are reused as-is (imported / invoked as a subprocess),
not forked -- `bin/oryx-get/build.sh` copies them into the deployable
bundle from their one canonical location.

**Deliberate difference from deb2pkg-dell.sh: real `depend =` lines.**
deb2pkg-dell.sh omits dependencies on purpose, because packages headed for
the `[oryx]` REPOSITORY must match the repo database's own metadata exactly
or `pacman -S` calls them corrupted (§13). oryx-get installs straight off
disk with `pacman -U`, which never consults a sync database at all -- so
there's nothing to disagree with, and real dependencies belong in
`.PKGINFO` here (matching `oryx-mkpkg.sh`'s own convention for Oryx's own
packages). Confirmed by `pacman -Qi`: `w3m`/`libgc1` show a correct
`Depends On`/`Required By` relationship after an oryx-get install, not just
files dumped on disk.

**Trust chain, verified on real hardware before writing a line of the
resolver:** `gpgv` (hurd-i386) + `debian-ports-archive-keyring` (arch=all)
are BOTH real, installable packages -- confirmed by downloading the actual
`debian-ports` `InRelease` and getting a genuine `gpgv: Good signature`
against the installed keyring, exit 0, on this exact port. The chain oryx-get
implements is exactly that: `InRelease` (GPG-verified) → `Packages`
(sha256, from the signed `InRelease`) → each `.deb` (sha256, from the
verified `Packages`). `gnupg` itself is NOT available for hurd-i386 (only
`gpgv` is) -- irrelevant, `gpgv` is all verification needs. If either
`gpgv` or the keyring is ever missing, the Debian source refuses to
construct at all (a clear error, `[source:debian] unavailable: ...`)
rather than silently degrading to an unverified download; every OTHER
configured source keeps working regardless.

**Bug found and fixed: `debindex.py`'s `available=` mode is the wrong tool
for recursive fetching, not just a different one.** `Index.depends(name,
available=X)` is built for repo-INDEXING (oryx-setdeps.py/oryx-repo-add.py):
drop any dependency not in a curated final set, on purpose, so the shipped
repo never promises something it can't supply. Calling it the same way for
oryx-get's recursive "go convert whatever is missing" resolution silently
DROPPED perfectly ordinary sole dependencies (no OR-alternatives at all) --
`w3m`'s `libgc1` vanished from the resolved list entirely instead of being
queued for conversion, because it wasn't yet "available" and the function
has no fallback to "any real package" when `available` is given. **Fixed
by reusing `Index.resolve()`** (existing public API, exactly "a real name
for a virtual or unknown one") on anything `depends()` drops, rather than
re-deriving the alternative-picking logic -- which would have been forking
debindex.py in spirit even while technically only living in oryx-get.

**Maintainer-script scanning validated against a known case, not just
written and hoped for.** Run against the real i2pd `.deb`, the scanner
independently found the exact two gaps this project already discovered by
hand and fixed manually earlier the same day (§16): the `adduser` system-
user creation, and the dropped `update-rc.d` init-script registration.
Heuristic pattern matching only (greps postinst/preinst/prerm/postrm for
known command names) -- it will miss anything hidden behind a shell
variable or a sourced file, which is a stated limitation, not a bug to
chase further.

**Protected packages: refuses correctly, including the non-interactive
case that matters most.** `--allow-protected` alone is not enough --
confirmation is required, and confirmation over a non-TTY (a script, a
cron job, a non-interactive SSH command) is refused outright rather than
silently assumed. This is the direct, deliberate answer to §14's actual
lesson: the sshd lockout happened because a *procedure* looked safe, not
because anyone was reckless -- the fix belongs in what the tool refuses to
do unattended, not in a comment telling a future operator to be careful.

**Acceptance test, all four passing, on the T400 with no Dell involved in
any conversion:**
1. `oryx-get w3m` -- resolves `libgc1` as a real (non-alternative) Debian
   dependency, converts and installs both in the correct order, `w3m -dump`
   renders real HTML.
2. `oryx-get --pypi cowsay` -- fetched, sha256-verified, converted,
   installed, `import cowsay` succeeds, the generated console script runs.
3. `oryx-get libc0.3` -- refused outright; `--allow-protected` over a
   non-interactive session still refused (no TTY to confirm against).
4. `oryx-get --export DIR --export-name oryx-export-test lynx` -- real
   multi-package export (`lynx` + `lynx-common`), served with
   `python3 -m http.server`, added as a second, independent pacman repo via
   a separate config, installed from there, `lynx -version` runs. Confirms
   the export format needs no changes to become a real second repo.

**Known limitations, by design, not oversights:**
- PyPI dependencies are NOT resolved transitively in this version --
  `oryx-get --pypi X` installs only X, warning about any `requires_dist`
  it declares rather than silently either ignoring or chasing them. Install
  those with oryx-get first if an import fails.
- Compiled-extension wheels are refused outright (no cross toolchain for
  hurd-i386 to build one, see `linux-personality/README.md` for why the
  Linux emulator itself had to be compiled ON the target for the same
  reason) -- oryx-get will not install something that cannot import.
- `--search` has no PyPI backend (no simple search API without an API key);
  repo and Debian search both work.

**Packaged properly 2026-09-20, after first shipping as a hand-deployed
tarball at `/opt/oryx-get`** -- inconsistent with every other piece of this
project, which installs through pacman. Real package now: `oryx-get` at
`/usr/bin/`, the `oryxget` Python package at
`/usr/lib/python3/dist-packages/` (already on every install's `sys.path`,
same convention as `python3-rns`/`python3-lxmf` -- no `sys.path` hack
needed at the call site), `debindex.py` shipped alongside it there too
(the exact same file `bin/debindex.py` already is, copied in at packaging
time by `bin/oryx-get/oryx-get-mkpkg.sh` -- not a fork), and
`oryx-repo-add.py` at `/usr/lib/oryx-get/` (a script `--export` invokes by
subprocess, deliberately kept out of the importable-package directory).
`/etc/oryx-get.conf` is generated by `oryx-postinst.sh oryx_get`, not
shipped in the package -- it is entirely Oryx's own creation with no
upstream default to preserve, unlike `i2pd.conf`, so it follows
`fix_pacman`/`fix_rnsd`'s pattern instead. Depends on `gpgv` and
`debian-ports-archive-keyring` directly, so the Debian source is available
out of the box on any system that installs `oryx-get`, not silently
degraded.

**Bug hit immediately on the first real install: `pacman -S oryx-get`
failed with `oryx database is inconsistent: name/version mismatch on
package oryx` and `target not found: oryx-get` -- neither message
mentioning oryx-get by name, which is what made this genuinely confusing.**
Root cause: `oryx-mkpkg.sh -v 1.0` (bare, no pkgrel) produces a `.PKGINFO`
whose `pkgver` is literally `1.0` -- but `oryx-mkpkg.sh` does NOT append a
pkgrel itself, unlike `deb2pkg-dell.sh`/`oryx-wheel2pkg.sh`, which always
build `pkgver-pkgrel` explicitly. **Every other package in this repository
already has an embedded pkgrel in its version string for exactly this
reason** (confirmed: `oryx-linux`, the one existing precedent for calling
`oryx-mkpkg.sh` directly, was built with `-v 1.0-1`, not bare `1.0` -- this
is the tool's real, undocumented calling convention).

Without it, libalpm's own sync-db loader -- which recovers name/version/
rel from a bare directory name like `oryx-get-1.0` by splitting from the
RIGHT (rightmost hyphen-segment = rel, next = version, everything left
over = name) -- has nothing to anchor on. With a package name that ITSELF
contains a hyphen (`oryx-get`) and no real rel segment, the split
misparses `oryx-get-1.0` as name=`oryx`, version=`get`, rel=`1.0`: hence
every error message about a nonexistent package called plain "oryx"
instead of oryx-get. `oryx-linux` -- also hyphenated -- only ever escaped
this because whoever built it already passed a proper `-v 1.0-1`, whether
by convention-following or by luck.

**Fix:** always pass a complete `pkgver-pkgrel` string to `oryx-mkpkg.sh`'s
`-v`, never a bare version. Worth a comment directly in `oryx-mkpkg.sh`
itself for the next package that uses it -- this is now noted in
`bin/oryx-get/oryx-get-mkpkg.sh`, but the tool being called incorrectly
with no warning is the part likely to bite again elsewhere.

## 20. The Linux personality as a package

`oryx-linux` + `oryx-linux-sysroot`, built with `bin/oryx-mkpkg.sh` and
installed from the repo like anything else. The emulator's own findings live
in `linux-personality/README.md` — that file is the valuable one. Only the
packaging notes belong here:

- **`oryx-mkpkg.sh` is for ORYX's packages**; `deb2pkg-dell.sh` is for
  Debian's. Both hit the same two traps, so both handle them: member paths
  must be `usr/bin/foo` and not `./usr/bin/foo` (pacman installs a
  `./`-prefixed archive as EMPTY, with no error), and `.PKGINFO` must come
  first.
- **Normalise directory modes to 755 when staging.** A tree built under the
  usual umask is 775, and pacman then warns "directory permissions differ on
  /usr/" for every one. Worse, it does not correct an existing directory, so
  a first install at 775 keeps warning even after the package is fixed —
  remove the package and the directory, then reinstall.
- **The emulator has to be compiled on the target** (it is i686 hurd-gnu and
  the Dell is amd64), so the build is: compile on the T400, `scp` the binary
  to the Dell, stage it, `oryx-mkpkg.sh`, `oryx-repo-add.py`, then
  `pacman -S` on the target. There is no cross toolchain.
- **The sysroot is a separate package** and deliberately so: it is 5.5 MB of
  Debian's i386 glibc and friends, it has its own version, and the emulator
  is useful without it for static binaries.
- The emulator defaults to `/usr/lib/oryx-linux/sysroot` when no `--sysroot`
  and no `ORYX_SYSROOT` is given, so an installed system needs no
  configuration at all.
