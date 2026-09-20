# Pre-reboot checklist — oryx-rnsd, oryx-i2pd, oryx-swap

**Not run.** This file exists so the actual reboot — whenever it happens,
with someone physically at the T400 — is a checklist, not an improvisation.
See the T400 remote-access rule: SSH is the only way in, and a bad boot
config is only found out AFTER access is already gone. All three of these
are additive (new `rc2.d`/`rcS.d` entries; nothing existing was changed to
add them), which is why the risk is believed to be low — but "believed" is
exactly the word a checklist exists to stop trusting blindly.

---

## Before rebooting

Run all of these from the current session, while SSH still works, and note
the results here (or just eyeball them) before pulling the trigger.

1. **Confirm someone is physically at the machine**, or has out-of-band
   access to its console. There is no serial console, no IPMI — if the
   boot doesn't come back on the network, a keyboard at the machine is the
   only recovery path (GRUB entry picked by hand, `wd0s3`, 3rd top-level
   item).

2. **Record current state of all three services**, so "did it come back
   the same way" has something concrete to compare against:
   ```sh
   ssh archhurd '
     rnstatus
     /etc/init.d/i2pd status; ps.procps aux | grep i2pd | grep -v grep
     swapon -s 2>&1 || cat /proc/swaps 2>&1
     ls /etc/rc2.d/ /etc/rcS.d/
   '
   ```
   Expect: `oryx-rnsd` at `rc2.d/S25`, `i2pd` at `rc2.d/S22` (BEFORE rnsd —
   see FIXES.md §18, this ordering was wrong once already), `oryx-swap` at
   `rcS.d/S06`. rnstatus should show the Dell TCP link, the community-hub
   TCP link, and (once it's had a minute) the I2P interface, all `Up`.

3. **Check `/etc/rc2.d/` has no leftover stale symlinks** from before the
   i2pd reordering (FIXES.md §18) — there should be exactly one `i2pd`
   entry (`S22i2pd`), not also a leftover `S30i2pd`:
   ```sh
   ssh archhurd 'ls /etc/rc2.d/'
   ```

4. **Confirm `/etc/default/i2pd` still pins `--datadir`** (FIXES.md §16 —
   without this, i2pd silently reads/writes the wrong directory under
   `start-stop-daemon --chuid` and never reseeds, with nothing logged about
   why):
   ```sh
   ssh archhurd 'cat /etc/default/i2pd'
   ```
   Expect: `DAEMON_OPTS="$DAEMON_OPTS --datadir=/var/lib/i2pd"`.

5. **Note the current I2P b32 addresses on both boxes** (T400 and Dell) —
   they are NOT persisted across `rnsd` restarts (ISSUES.md §3), so a
   reboot is expected to change the T400's. This is informational, not a
   blocker: it means don't be surprised when it's different afterward, and
   don't treat a changed address as evidence something broke.
   ```sh
   ssh archhurd 'rnstatus | grep "I2P B32"'
   /home/lain/rnsvenv/bin/rnstatus | grep "I2P B32"
   ```

6. **Sync and flush**, since Hurd cannot remount `/` read-only at shutdown
   and an unclean halt after heavy writes has produced multiply-claimed
   blocks before (FIXES.md §10):
   ```sh
   ssh archhurd 'sync; sync; sleep 2; sync'
   ```

7. **Use `safe-reboot`, never a bare `reboot`** (FIXES.md §10):
   ```sh
   ssh archhurd 'safe-reboot'
   ```

---

## After rebooting

1. **Expect a forced fsck.** Normal — Hurd never gets to set the clean
   flag on this machine. Not a sign of damage on its own.

2. **Wait for the GRUB entry to be picked by hand at the keyboard**, then
   for boot to reach `login>`. This is the step that needs someone
   physically present.

3. **Confirm SSH comes back**, before checking anything else:
   ```sh
   ssh -o ConnectTimeout=15 archhurd 'echo alive'
   ```
   If this fails, STOP and go to Recovery below rather than trying more
   things over a connection that may be the last one that will work.

4. **`/` writable, network up unattended** (the original boot-verification
   bar this system was already held to, before any of this session's work):
   ```sh
   ssh archhurd 'fsysopts / 2>&1; ip addr 2>&1 || ifconfig 2>&1'
   ```

5. **Check all three services came up WITHOUT manual intervention**:
   ```sh
   ssh archhurd '
     /etc/init.d/oryx-rnsd status
     /etc/init.d/i2pd status
     ps.procps aux | grep i2pd | grep -v grep
     swapon -s 2>&1 || cat /proc/swaps 2>&1
   '
   ```
   `i2pd`'s own `status` action has a known cosmetic false-negative on this
   port even when the daemon is genuinely healthy (FIXES.md §16's ending
   note) — cross-check with the `ps.procps` line and, once i2pd has had a
   minute, its web console (`curl`-less: use `python3 -c "import
   urllib.request; ..."` against `http://127.0.0.1:7070/`, same technique
   used throughout this session) before concluding it failed.

6. **Wait several minutes, then check `rnstatus` on both interfaces
   including I2P.** Tunnels take 1–3 minutes even under good conditions
   (FIXES.md §18) — do not judge the I2P interface before then.
   ```sh
   ssh archhurd 'rnstatus'
   ```

7. **If the T400's I2P b32 address changed** (expected, see step 5 above),
   update the Dell's `peers=` entry if it was ever pointed at the T400's
   OLD address specifically — check `/root/.reticulum/config` on the Dell.
   (As configured during this session, `peers=` points the OTHER direction
   — T400 → Dell — so this is unlikely to bite, but the Dell's `rnsd` was
   also never restarted during testing; a first restart is untested.)

8. **Only once 3–7 all check out**, consider swap/rnsd/i2pd's boot
   persistence PROVEN rather than merely "additive and believed safe."
   Update FIXES.md §4/§15/§16 (drop the "not yet proven across a real
   boot" caveats) and ISSUES.md if anything above didn't match.

---

## Recovery, if SSH does not come back

- **GRUB is still the way in** — nothing about this session's work touches
  the boot path (GRUB, `/etc/init.d/rc`, `inittab`) or `sshd`/`pfinet`
  directly. If SSH is down, the most likely causes are: (a) still booting
  / stuck at the fsck, (b) a genuinely new problem unrelated to
  rnsd/i2pd/swap, or (c) — least likely, but check first since it's fastest
  to rule out — the GRUB entry wasn't picked within its timeout and it
  booted Debian instead of Oryx.
- **At the console**: `login root` (empty password on this box, per
  existing setup), then work through steps 3–6 above locally instead of
  over SSH. If `sshd` itself is the problem, check
  `/etc/init.d/ssh status` and the FIXES.md §14 sysusers fix
  (`/usr/bin/systemd-sysusers; mkdir -p /run/sshd; /etc/init.d/ssh
  restart`) — that exact failure mode (accepts TCP, no banner) has
  happened before on this machine for an unrelated reason, and is worth
  ruling out even though nothing in this session touched openssh.
- **If i2pd or rnsd are the problem** and NOT SSH itself: this is not an
  access emergency. Stop them (`/etc/init.d/i2pd stop`,
  `/etc/init.d/oryx-rnsd stop`), fix at leisure over the now-working SSH
  session, re-run `oryx-postinst.sh i2pd rnsd` if in doubt about config
  drift.
- **If genuinely stuck**: the machine boots to a `login>` prompt worst
  case, which is itself full recovery access — nothing here is as
  dangerous as the sshd/glibc/pfinet/netdde class of change the remote-
  access rule exists for.
