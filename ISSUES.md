# Oryx Hurd — open issues

Tracked separately from `FIXES.md`. FIXES.md is the catalogue of gaps
already found *and closed* — symptom, cause, fix. This file is for gaps
found but **not** closed: real investigation happened, a wall was hit, and
picking it back up needs context that shouldn't have to be rediscovered.

---

## 1. Yggdrasil — blocked on gccgo's missing Go generics support

**Status:** investigated in depth 2026-09-20, not shipped. Not a quick
retry — read this fully before attempting again.

**What works:** Yggdrasil can run headless (`IfName: none` in its config)
as a pure mesh router with no local network interface, which sidesteps
[issue 2](#2-no-tuntap-translator-on-gnuhurd) below entirely for the
*relay* use case (see that issue for why a local address still needs a
real translator). Go itself has no Hurd port, but Debian ships `gccgo`
(GCC's Go frontend) for hurd-i386, and other Go programs are already built
this way for this architecture — `kcptun` is a real, installed, working
example, proving the toolchain is viable in principle.

**What was fixed along the way (recipe, not just findings):**
- `src/tun/tun_other.go` (yggdrasil's own "unsupported platform" fallback)
  unconditionally calls `wgtun.CreateTUN`, which doesn't exist for any GOOS
  outside the 5 platforms `golang.zx2c4.com/wireguard/tun` explicitly
  implements — so it fails to compile on every genuinely unsupported
  platform, Hurd included. Harmless at runtime (`tun._start()` never calls
  `setup()` when `IfName` is `"none"`), but blocks compilation regardless.
  **Fix:** replace the file's body with a stub returning an error, drop the
  `wgtun` import. Legitimate, upstreamable patch.
- No `go` build tool via the normal Debian path (`golang-any` depends on
  `golang-go`, which does not exist for hurd-i386 — `golang-any` is
  therefore currently uninstallable there). **But `gccgo-14`/`gccgo-16`
  each bundle their own tool** at `/usr/bin/go-14` / `go-16` — an
  undocumented escape hatch, found only by `dpkg -L gccgo-14`. It does real
  GOPATH-mode dependency resolution and correct GOOS-aware file selection
  (`go-14 env GOOS` → `hurd`, genuinely, not a guess).
- `golang.org/x/sys/unix` has real, dedicated Hurd support —
  `syscall_hurd.go`, `syscall_hurd_386.go` — confirming the syscall layer is
  intentional upstream work, not a lucky accident.
- Two gccgo stdlib gaps found and worked around: `crypto/ecdh` (Go 1.20)
  and `"slices"` (Go 1.21), both missing regardless of gccgo 14 vs 16 (GCC's
  Go stdlib mirror lags upstream by several releases, independent of GCC
  version). Fixed by vendoring `x/crypto/curve25519` from v0.17.0 (still has
  the pre-`ecdh` pure-Go path behind a `!go1.20` tag) and a 5-line local
  `slices` shim providing just `SortStableFunc`.
- QUIC and WebSocket transport support deliberately excluded
  (`src/core/link_quic.go`, `link_ws.go`, `link_wss.go`, plus the handful of
  `case "quic":` / `l.ws` references in `link.go`) — `quic-go` is archived
  only under its old, likely API-incompatible identity
  (`github.com/lucas-clemente/quic-go`), and `github.com/coder/websocket`
  isn't packaged at all. TCP/TLS transport (proven fine on Hurd by
  Reticulum) is unaffected — this trims an optional feature, not core
  peering.

**The actual blocker:** gccgo's Go frontend does not implement generics **at
all**, independent of any library or GOOS question. Verified with the
smallest possible isolated test:
```go
func Max[T int | float64](a, b T) T { if a > b { return a }; return b }
```
fails to even parse (`expected '('`, `expected ']'`) on gccgo-14 AND
gccgo-16. This sinks `github.com/Arceliar/phony` — the actor-model
concurrency primitive underneath BOTH `ironwood` (yggdrasil's DHT/routing
engine) and yggdrasil itself — whose lock-free queue uses
`atomic.Pointer[queueElem]` and `atomic.Bool` (generic stdlib types, Go
1.19).

**Gccgo version finding (2026-09-20, ~10 min check):** the hurd-i386
archive currently offers `gccgo-11`, `-12`, `-14`, `-15`, `-16` — 16 is the
newest, already the one tested. A web search confirms this is not a
version-specific gap to wait out: gccgo's Go frontend has been "stuck on
basically Go 1.18 standard library features... due to the lack of generics
support in the frontend" for years, across many GCC releases, with no
indication of active work to close it. **A newer GCC release is not a
credible fix to wait for.**

**Next steps, in order of how likely they are to actually work:**
1. **Patch `Arceliar/phony`'s queue to drop generics** — it's small and
   self-contained (`actor.go`). `atomic.Pointer[T]` is a thin generic
   wrapper the stdlib itself implements with `unsafe.Pointer` +
   `atomic.CompareAndSwapPointer`/`LoadPointer`/`StorePointer` underneath;
   rewriting `phony`'s queue with those directly (pre-generics, Go 1.17
   style) would be a legitimate, bounded patch. **But** every OTHER current
   or future dependency using generics (pervasive in Go code written after
   ~2022) hits the identical wall — this fixes one instance, not the
   standing limitation.
2. Check for a non-gccgo Go compiler alternative for Hurd (there is none
   known as of this writing — Go's own toolchain has no Hurd port at all;
   this was the reason gccgo was reached for in the first place).
3. Revisit if/when gccgo's generics support ever lands upstream — track via
   the GCC Go frontend release notes, not by re-testing every GCC release
   blind.

**Staged build tree:** left in place on the T400 at `/root/ygg-build/` —
**do not delete it.** Contains the full source tree (yggdrasil + all
resolved dependencies) with the `tun_other.go` patch and the
curve25519/`slices` workarounds already applied, i.e. everything up to the
`phony` wall, ready to resume from. Full diagnostic trail: `FIXES.md` §17.

---

## 2. No TUN/TAP translator on GNU/Hurd

**Status:** confirmed blocker, scoped as its own future project, not
started.

GNU/Hurd has no TUN/TAP-equivalent translator — nothing exposes a virtual
network interface that a userspace program can bind to and have the kernel
route real traffic through. Confirmed by the total absence of `openvpn` or
`wireguard-tools` anywhere in the ~96,000-package hurd-i386 archive (both
fundamentally require exactly this to function at all), and independently
by [issue 1](#1-yggdrasil--blocked-on-gccgos-missing-go-generics-support)
above, where the same gap is why Yggdrasil's *local* network interface
(as opposed to its headless relay mode) isn't reachable either.

**What this blocks:**
- **Mullvad VPN** — their app is WireGuard-based; even OpenVPN configs
  (which Mullvad has been deprecating in favour of WireGuard anyway) need
  the same missing primitive.
- **WireGuard and OpenVPN** generally, for any purpose.
- **Yggdrasil with a local address.** Headless relay mode (`IfName: none`,
  see issue 1) sidesteps this — the box can forward mesh traffic for other
  Yggdrasil nodes — but it never gets its own reachable Yggdrasil IPv6
  address, and can't originate or receive traffic ON that network itself.
  This is a real, usable partial mode, just not "Oryx has a Yggdrasil
  address" in the way TCP/I2P give Reticulum and i2pd a real local identity.

**What this does NOT block:** Reticulum (proven, including now over I2P —
FIXES.md §15, §18) and i2pd (FIXES.md §16) both work fully, because neither
needs a system-level virtual network interface — they operate entirely
through ordinary TCP/UDP sockets and, for i2pd, local SOCKS/HTTP proxies.
The pattern that separates "can work on Hurd today" from "needs this
translator first" is exactly that distinction.

**The actual fix** is a new Hurd translator implementing tun/tap semantics
— reading/writing raw packets to/from a userspace program the way
`/dev/net/tun` does on Linux, then wiring that into pfinet or a comparable
routing path. This is a kernel-level systems project on the same scale as
the Linux personality work (`linux-personality/README.md`) — deep Mach/Hurd
translator-writing knowledge, likely weeks not hours, and NOT attempted as
part of any of the packaging work in FIXES.md. Owner chose to scope it as
its own project rather than attempt it inline (2026-09-20).

**Where to start, if picked up:** read an existing simple Hurd translator
for the RPC/io patterns (`/hurd/pflocal` is a reasonable reference — small,
self-contained, not tied to real hardware), then the Linux kernel's own
`drivers/net/tun.c` for the actual character-device semantics
(`TUNSETIFF`, packet framing) that need reimplementing against Hurd's
translator model instead of a Linux char device. `oryx-linux`
(`linux-personality/README.md`) is the closest existing precedent in this
project for "reimplement a Linux kernel-facing interface against Mach/Hurd
primitives," even though it solves a different problem (syscall emulation,
not a translator).

---

## 3. Reticulum-over-I2P: destination address not persisted across restarts

**Status:** low priority, observed 2026-09-20, not investigated further.

RNS's `I2PInterface` generates a fresh I2P b32 destination on every `rnsd`
restart rather than persisting one long-term the way i2pd persists its own
router identity (`router.keys`/`router.info`, see FIXES.md §16). Confirmed
by three restarts of the T400's `rnsd` producing three different b32
addresses, with a new keyfile accumulating under
`~/.reticulum/storage/i2p/*.i2p` each time rather than an existing one
being reused.

**Practical consequence:** a `peers = <b32>` entry pointing at another
node's I2P address only stays valid as long as that OTHER node's `rnsd`
hasn't restarted since the address was recorded. In the tested
configuration (T400's config carries `peers=` pointing at the Dell) this
was a non-issue because the Dell's `rnsd` was never restarted during
testing — but the reverse direction, or any future restart of the Dell's
`rnsd`, would silently go stale and need the T400's config hand-updated
with the Dell's new address.

**Why this is low priority, not blocking:** `peers=` is only needed to
*initiate* a first connection to a peer with no other path to it yet — once
a path exists via the normal announce/DHT mechanism, RNS interfaces don't
depend on it. The Dell and T400 also already share two OTHER interfaces
(TCP, both directions), so the I2P path going stale would not sever
connectivity between them, only remove I2P specifically as one of several
routes until manually re-pinned.

**Not investigated:** whether `I2PInterface` supports persisting its
destination identity via some config option or keyfile path not yet tried,
or whether this is simply how `RNS.vendor.i2plib` is designed to work
(ephemeral-by-default, matching how e.g. a `TCPClientInterface`'s local
port is never meant to be a stable identity either — the actual stable
identity in Reticulum lives at the Destination/announce layer above any
one interface). Worth a source read of `RNS/Interfaces/I2PInterface.py`
(`i2p_dest_hash_of`/`i2p_dest_hash_nf`, around line 231) before assuming
it's a bug rather than intended behaviour.

See FIXES.md §18 for the full test that surfaced this.

---

## Referenced from

`README.md`'s "Where this goes next" roadmap links here for Yggdrasil and
the TUN/TAP translator. Update both places if an issue here gets resolved
or reprioritized.
