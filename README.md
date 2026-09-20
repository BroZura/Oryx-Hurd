# Oryx Hurd

A GNU/Hurd system managed with pacman, running on bare metal on a ThinkPad
T400. Bootstrapped by repacking Debian hurd-i386 binaries rather than
compiling from source.

**Status (2026-09-20):** boots unattended from `wd0s3`, brings up its own
networking, accepts SSH, 262 packages under pacman, Python 3.14 + pip, git and
perl working. 953 MB of swap is live. Reachable at `192.168.2.182`
(`ssh archhurd` from the Dell). It is also on the **Reticulum mesh** — `rnsd`
runs as `oryx-rnsd` (rc2.d), leaf-connected over TCP to the Dell's shared
instance — and on the real **I2P network** — `i2pd` (rc2.d), reseeded and
building tunnels within seconds of boot, local proxies on 127.0.0.1. As of
2026-09-20, **Reticulum also runs over I2P**: `rnsd`'s `[[I2P]]` interface
rides i2pd's SAM bridge, verified with a real LXMF message delivered to the
Dell over I2P alone (TCP interfaces disabled to prove it). See FIXES.md 18.
It can also **fetch and convert its own packages with no Dell/server
involved** — `pacman -S oryx-get`, verified end to end on real Debian and
PyPI packages. See the "oryx-get" section below and FIXES.md 19.

**It runs Linux binaries.** `pacman -S oryx-linux oryx-linux-sysroot` installs
a Linux personality that executes unmodified 32-bit Debian i386 binaries —
static and dynamic, including GNU coreutils through a real `ld-linux.so.2`,
and `busybox sh` with fork, exec and pipelines. 46 tests, all green. This is
the thing no other Hurd distribution has; see `linux-personality/`.

It has **its own binary repository with working dependency resolution**.
`pacman -S <pkg>` against `[oryx]` pulls a package's dependencies, in the
right order, with no `--nodeps` and no help from this machine. 223 packages,
served from the Dell over HTTP. `git` and `perl` were installed that way —
ten packages, resolved and ordered entirely by pacman on the target.

This directory is everything needed to reproduce the configuration, and is
intended to become the `oryx-base` package.

---

## Layout

```
oryx-postinst.sh   THE script. Applies every fix to a running Oryx system.
                   Self-contained (config files embedded), idempotent.
FIXES.md           The catalogue: symptom, cause and fix for every gap found.
                   The expensive part to rediscover -- read this first.

bin/               Tools that run on the DELL (or any Debian-derived host)
  oryx-install.sh    install Debian hurd-i386 packages onto the target:
                     resolve closure, convert, publish to the repo, install
  oryx-setdeps.py    resolve dependencies INTO the repo's packages; run
                     before indexing, never after (see FIXES.md 13)
  oryx-repo-add.py   build the repository database (a standalone `repo-add`,
                     so the Dell does not need pacman installed)
  oryx-serve.sh      serve the repository over HTTP for the T400
  oryx-repo.service  systemd --user unit so that server survives a reboot
  oryx-mkpkg.sh      build one of ORYX'S OWN packages from a staging dir
                     (deb2pkg-dell.sh repackages Debian's; this makes ours)
  oryx-wheel2pkg.sh  package a pure-Python PyPI wheel (RNS, LXMF, NomadNet --
                     not in Debian at all, so deb2pkg has nothing to convert)
  debindex.py        shared Debian Packages parser and relationship
                     translation, used by closure.py and oryx-setdeps.py
  deb2pkg-dell.sh    convert one .deb -> pacman package
  installed2pkg.sh   package something already installed on a Debian host
                     (for local +hurd.1 rebuilds whose .deb is gone)
  closure.py         dependency closure from a Debian Packages index
  depsweep.sh        static DT_NEEDED sweep over a mounted target
  psf2bdf.py         Linux console PSF font -> BDF for the Hurd console
  oryx-get/          a package fetcher that runs ENTIRELY ON THE TARGET --
                     no Dell involved. See its own section below and
                     FIXES.md 19. oryx-get-mkpkg.sh builds the real pacman
                     package (copies in debindex.py + oryx-repo-add.py from
                     here, reused as-is, never forked); build.sh is the
                     older dev-loop tarball, kept for fast iteration.

linux-personality/ The Linux personality -- run 32-bit Linux binaries on
                   Hurd. Ships as the oryx-linux package. Read its README:
                   the Mach findings in it are the deepest part of Oryx.

target/            Files installed onto the Oryx system
  hooks/             pacman hooks -- postinst emulation, the structural fix
  sbin/              their handlers (oryx-alternatives, oryx-update-cacerts,
                     oryx-sysusers, ...) plus oryx-debian, which mounts the
                     Debian partition on demand
  logo-plain.txt     the oryx ASCII art, uncoloured
  vga-8x{8,14,16}.bdf console fonts
```

---

## Usage

**Configure a system** (run on the target, as root):

```sh
scp -r oryx-postinst.sh target/ archhurd:/tmp/oryx/
ssh archhurd '/tmp/oryx/oryx-postinst.sh'
```

```
./oryx-postinst.sh                 every fix
./oryx-postinst.sh --list          show them
./oryx-postinst.sh ptys cacerts    only those
./oryx-postinst.sh --net           also write the static network config
                                   (off by default, it is host-specific)
```

**Serve the repository** (run on the Dell, leave running):

```sh
bin/oryx-serve.sh                  # ~/Desktop/hurd/repo on :8099
```

The target's `[oryx]` section points at `http://192.168.2.210:8099`.

**This is already running as a systemd user service** (`oryx-repo.service`,
enabled, with lingering on) so it comes back after a reboot of the Dell. The
foreground script above is for debugging. To check or stop it:

```sh
systemctl --user status oryx-repo.service
systemctl --user disable --now oryx-repo.service   # to undo
```

Note it listens on 0.0.0.0:8099 with no authentication and serves unsigned
packages — deliberate for a LAN repo of repackaged binaries, not something to
expose further.

**Install packages** (run on the Dell):

```sh
bin/oryx-install.sh -n git tmux      # dry run: resolve and report
bin/oryx-install.sh git tmux         # do it
bin/oryx-install.sh -P git           # publish to the repo, install nothing
bin/oryx-install.sh -p git           # old way: scp + pacman -U, no repo
```

It resolves the hurd-i386 dependency closure, skips what the target already
has, converts each `.deb`, publishes them into the repository and has the
target pull them over HTTP. **The T400 never needs to boot Debian to gain
software.**

Because the packages stay in the repo, the target can reinstall anything
without the Dell converting it a second time, and `pacman -Ss` finally has
something to search.

**Rebuild the repository by hand**, after adding packages to it directly:

```sh
bin/oryx-setdeps.py  ~/Desktop/hurd/repo --index ~/Desktop/hurd/Packages
bin/oryx-repo-add.py ~/Desktop/hurd/repo -n oryx
```

**That order is not optional.** pacman rejects any package whose dependencies
disagree with its database entry, reporting it as `invalid or corrupted
package` — see FIXES.md §13, which is the single most expensive thing in this
project to rediscover. `oryx-setdeps.py` writes dependencies into the
packages; `oryx-repo-add.py` copies them out into the database. Reversing the
order guarantees the mismatch.

`repo/oryx-exclude.txt` lists packages that must never be indexed (`procps`,
because the target runs a local `+hurd.1` rebuild). Deleting such a package
from the repo by hand does not hold — the next converter run recreates it.

Packages already installed are skipped, never upgraded — deliberately.
Silently bumping glibc underneath a running Hurd is not something to do as a
side effect of installing something unrelated.

> Maintainer scripts are dropped by the converter, but **the pacman hooks now
> handle that automatically** — library cache, CA bundle, alternatives, glib
> schemas, mime/desktop/icon caches, fontconfig, and **system users** all
> regenerate after any install. See FIXES.md §0 and §14. Still not covered:
> `/etc/machine-id`.

---

## oryx-get — fetch a package with no Dell/server involved

Added 2026-09-20. Runs entirely ON the T400 (or any Oryx system): no Dell,
no HQ machine, nothing else in the loop. A real pacman package --
`pacman -S oryx-get` -- not a hand-deployed script; build it with
`bin/oryx-get/oryx-get-mkpkg.sh`. Config: `/etc/oryx-get.conf`, generated
by `oryx-postinst.sh oryx_get` (or on first run if missing).

```sh
oryx-get w3m                        # resolves + converts + installs, deps included
oryx-get --pypi cowsay              # pure-Python PyPI wheel, same as oryx-wheel2pkg.sh
oryx-get --search lynx              # across every configured source
oryx-get --info w3m                 # version, description, dependencies
oryx-get --dry-run some-package     # show the plan, touch nothing
oryx-get libc0.3                    # refused -- protected, see below
oryx-get --allow-protected libc0.3  # still refused without an interactive
                                     # confirmation (never bypassable from a
                                     # script or a non-interactive session)
oryx-get --export /path/to/dir --export-name myrepo lynx
                                     # converts (does not install locally) and
                                     # writes a servable pacman repo to the dir
```

Three sources, tried in the order they appear in `/etc/oryx-get.conf`:
`[oryx]` (already-built repo, just `pacman -S`), Debian hurd-i386
(debian-ports, GPG-verified end to end -- `InRelease` → `Packages` sha256 →
each `.deb`'s sha256), and PyPI (pure-Python wheels only). Adding a new
source later is a config entry, not a code change.

Protected packages (glibc, openssh, pfinet, netdde, the boot path, …) are
refused by default — this exists because of a real incident (FIXES.md §14:
an unattended sshd upgrade locked the T400 out over its only access route)
— and Debian conversions get scanned for dropped maintainer-script actions
our pacman hooks don't cover (system users, init registration, …), so a
result that "may not work" says so up front instead of failing silently
later. Full detail, the trust-chain verification, and the bug found and
fixed in the shared dependency-resolution code: **FIXES.md §19**.

---

## Where this goes next

Ranked by value per effort:

1. ~~**Stand up a repo.**~~ ~~**Teach it dependencies.**~~ **Done
   2026-09-20.** 225 packages, `[oryx]`, real `%DEPENDS%`, `--nodeps` gone,
   and every package installed on the target is in it. The server runs as a
   systemd unit. Nothing outstanding here.
2. **Turn these fixes into real packages** — `oryx-base` (boot/init) and
   `oryx-branding` (os-release, logo, fastfetch). This script is the seed,
   and `bin/oryx-mkpkg.sh` is now the tool: `oryx-linux` and
   `oryx-linux-sysroot` were built with it and install cleanly from the repo.
3. **A reproducible bootstrap**: empty partition → booting Oryx, unattended.
   The expensive part is already paid — the non-obvious steps are all in
   FIXES.md.
4. ~~**Swap.**~~ **Done 2026-09-20** — 953 MB live, via `S06oryx-swap`.
   Needed `/dev/wd0s1` repointed from the nonexistent `@/dev/disk:` store to
   `device:wd0`. **Not yet proven across a real boot.**
5. ~~**Reticulum mesh.**~~ **Done 2026-09-20** — `python3-rns`, `python3-lxmf`,
   `nomadnet` packaged from PyPI wheels (`bin/oryx-wheel2pkg.sh`, new for
   this), installed, and leaf-connected over TCP to the Dell's shared
   instance. `oryx-rnsd` (rc2.d) daemonizes `rnsd`, which has no daemon mode
   of its own — see FIXES.md §15. **Not yet proven across a real boot**,
   same as swap above. This is now also the practical answer to "no WiFi"
   below: a mesh presence that does not need netdde at all.
6. **Prove items 4 and 5 across a real reboot together**, since neither has
   been. Both are additive (new rc2.d/rcS.d entries, nothing existing
   changed), so the risk is low, but "not yet proven" should not linger
   indefinitely.
7. **Upstream contributions.** Port packages with no hurd-i386 build
   (`iputils-ping`), write a translator, report the IRQ 11 finding to
   bug-hurd — real-hardware Hurd data is rare and wanted.

**WiFi itself is still not worth attempting** (netdde has no
mac80211/cfg80211; it would mean porting the Linux wireless stack into DDE) —
Reticulum and i2pd over the existing wired/TCP path are the practical
workaround, not a fix for that gap.

**i2pd — done 2026-09-20.** Real Debian hurd-i386 build (pure C++, no
TUN/TAP needed), installed, reseeded and building tunnels on the real I2P
network within seconds of boot. Two postinst gaps fixed — see FIXES.md 16.

**Mullvad / WireGuard / OpenVPN / Yggdrasil (with a local interface) — all
blocked on the same missing kernel primitive: GNU/Hurd has no TUN/TAP
translator at all.** Not a packaging gap; nothing currently makes it
possible to expose a virtual network interface to route real traffic
through. The actual fix is a new Hurd translator implementing tun/tap
semantics — a kernel-level systems project on the scale of the Linux
personality work, not attempted here. Full scope: **ISSUES.md 2**.
**Yggdrasil specifically** can still run *headless* (no local interface,
pure mesh relay) without needing that — investigated in depth, got past
several real blockers (see FIXES.md 17), but stopped at gccgo's Go frontend
not implementing generics at all, which `github.com/Arceliar/phony`
(unavoidable, underneath both `ironwood` and yggdrasil itself) needs. Not a
"wait for a newer GCC" situation — confirmed this is a longstanding,
version-independent gccgo limitation. Full scope, staged build tree
location, and next steps: **ISSUES.md 1**.

**USB** was tried on 2026-09-20 and is not there yet. `/hurd/rumpusbdisk`
exists and `MAKEDEV rumpusbdisk` creates the nodes, but starting the
translator hangs in initialisation with nothing attached, and leaves a
process that has to be cleared with `settrans -fg`. The nodes were removed
again, because any `ls -l` of them restarts it. Worth retrying with a stick
actually plugged in and someone at the machine.

---

## Related

- `ISSUES.md` — gaps found but NOT closed (Yggdrasil, the missing TUN/TAP
  translator, the stale I2P peer address). `FIXES.md` is closed gaps only.
- `PRE-REBOOT-CHECKLIST.md` — oryx-rnsd/oryx-i2pd/oryx-swap are all additive
  and believed safe across a reboot, but none has actually been proven
  across one yet. Work through this WITH someone at the machine before the
  next reboot, then update FIXES.md/ISSUES.md with what actually happened.
- `../HURD-T400-HANDOFF.md` — the machine: disk layout, boot fixes, recovery
- `/root/archhurd/PROGRESS.md` on the **Debian** partition — the running build
  log. **Reachable from Oryx since 2026-09-20**: `oryx-debian mount` (or `rw`
  to commit). It is not mounted automatically on purpose — see the warning in
  that script about Debian's `/dev` starting translators in the running
  system.
