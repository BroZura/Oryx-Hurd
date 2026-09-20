#!/bin/bash
#
#  oryx-postinst.sh -- bring a freshly-built Oryx Hurd system up to scratch.
#
#  WHY THIS EXISTS
#  ---------------
#  Oryx Hurd is bootstrapped by repacking Debian hurd-i386 .deb binaries into
#  pacman packages (see bin/deb2pkg-dell.sh). That conversion deliberately
#  DROPS Debian maintainer scripts, because they assume dpkg, debconf and
#  Debian semantics that pacman does not provide.
#
#  The consequence is that everything a postinst would normally do never
#  happens. That is not a small set. Commands vanish entirely, TLS does not
#  work, and there are no pseudo-terminals to log in on. Every fix below was
#  found the hard way on the ThinkPad T400, and each one is annotated with the
#  symptom that led to it so the next person does not have to rediscover it.
#
#  This script is the seed of what should eventually be a real `oryx-base`
#  package. It is idempotent: safe to run repeatedly.
#
#  USAGE
#     ./oryx-postinst.sh                 run every fix
#     ./oryx-postinst.sh --list          show the fixes
#     ./oryx-postinst.sh ptys cacerts    run only the named ones
#     ./oryx-postinst.sh --net           also (re)write the static network
#                                        config -- OFF by default, it is
#                                        host-specific
#
#  Run it ON the target, as root. Never against a mounted target from a host:
#  touching a node with a passive translator starts that translator in the
#  RUNNING system, which is how the build host lost its network three times.
#
set -uo pipefail

# ---------------------------------------------------------------- settings --
OS_NAME="Oryx Hurd"
OS_ID="oryxhurd"
HOSTNAME_WANT="${ORYX_HOSTNAME:-archhurd}"   # not renamed yet; set to taste
LOGO_COLOR=135                               # 256-color purple

# Only used with --net. Host-specific: change before reusing.
NET_IFACE="/dev/eth0"
NET_ADDR="192.168.2.182"
NET_MASK="255.255.255.0"
NET_GW="192.168.2.1"

DO_NET=0
SELF_DIR=$(cd "$(dirname "$0")" && pwd)

# ----------------------------------------------------------------- helpers --
ok()   { printf '  \033[32mok\033[0m    %s\n' "$*"; }
skip() { printf '  \033[90mskip\033[0m  %s\n' "$*"; }
warn() { printf '  \033[33mwarn\033[0m  %s\n' "$*"; }
head_() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

need_root() {
    [ "$(id -u)" = 0 ] || { echo "must run as root" >&2; exit 1; }
}

writable_root() {
    case "$(fsysopts / 2>/dev/null)" in
        *--writable*) return 0 ;;
        *) fsysopts / --writable 2>/dev/null && return 0 || return 1 ;;
    esac
}

# =========================================================== the fixes ======

# --- alternatives ----------------------------------------------------------
# SYMPTOM: `ps`, `uptime`, `vmstat`, `reboot`, `halt`, `poweroff`, `kill` and
# `fakeroot` do not exist at all -- not broken, absent.
# CAUSE: on hurd-i386 these are update-alternatives links. The hurd package
# ships ps-hurd/uptime-hurd/vmstat-hurd/reboot-hurd/halt-hurd/poweroff-hurd,
# procps ships ps.procps/kill.procps/..., and the plain names are created by
# postinst. Debian's own `login` ignores login.defs too, so PATH is minimal.
# NOTE: the -hurd variants win where both exist; that is Debian's intent on
# this architecture, and Hurd's ps understands Mach tasks properly.
fix_alternatives() {
    head_ "update-alternatives links"
    mkdir -p /etc/alternatives
    local spec name dir providers target p
    for spec in \
        "ps:/usr/bin:ps-hurd ps.procps" \
        "uptime:/usr/bin:uptime-hurd uptime.procps" \
        "vmstat:/usr/bin:vmstat-hurd vmstat.procps" \
        "w:/usr/bin:w-hurd" \
        "kill:/usr/bin:kill.procps" \
        "fakeroot:/usr/bin:fakeroot-hurd" \
        "reboot:/usr/sbin:reboot-hurd" \
        "halt:/usr/sbin:halt-hurd" \
        "poweroff:/usr/sbin:poweroff-hurd"
    do
        name=${spec%%:*}; rest=${spec#*:}; dir=${rest%%:*}; providers=${rest#*:}
        target=""
        for p in $providers; do [ -x "$dir/$p" ] && { target="$dir/$p"; break; }; done
        [ -n "$target" ] || { skip "$name (no provider installed)"; continue; }
        # never clobber a real file -- only fill a gap or repoint our own link
        if [ -e "$dir/$name" ] && [ ! -L "$dir/$name" ]; then
            skip "$name (real file present)"; continue
        fi
        ln -sfn "$target" "/etc/alternatives/$name"
        ln -sfn "/etc/alternatives/$name" "$dir/$name"
        ok "$dir/$name -> $target"
    done
}

# --- ptys ------------------------------------------------------------------
# SYMPTOM: `ssh root@host` fails with "PTY allocation request failed on
# channel 0". Non-interactive `ssh host 'cmd'` works fine, which is why this
# can hide through an entire build.
# CAUSE: MAKEDEV was never run for pseudo-terminals, so /dev held only the
# physical tty1..tty6. Nothing interactive can work without these.
fix_ptys() {
    head_ "pseudo-terminals"
    if [ -e /dev/ptyp0 ]; then
        ok "ptys already present ($(ls -1 /dev | grep -c '^pty') nodes)"
        return
    fi
    [ -x /dev/MAKEDEV ] || { warn "/dev/MAKEDEV missing"; return; }
    ( cd /dev && ./MAKEDEV ptyp ptyq ) && ok "created 64 pty pairs (ptyp, ptyq)"
}

# --- CA certificates -------------------------------------------------------
# SYMPTOM: every HTTPS client fails. pip dies with a bare
# "FileNotFoundError" out of certifi.where().
# CAUSE: ca-certificates' postinst generates BOTH /etc/ca-certificates.conf
# and the concatenated bundle. Neither exists without it, and Debian patches
# certifi to point at the system bundle.
fix_cacerts() {
    head_ "CA certificate bundle"
    [ -d /usr/share/ca-certificates ] || { skip "ca-certificates not installed"; return; }
    if [ -s /etc/ssl/certs/ca-certificates.crt ]; then
        ok "bundle present ($(grep -c 'BEGIN CERTIFICATE' /etc/ssl/certs/ca-certificates.crt) CAs)"
        return
    fi
    ( cd /usr/share/ca-certificates && find . -name '*.crt' | sed 's|^\./||' | sort ) \
        > /etc/ca-certificates.conf
    update-ca-certificates >/dev/null 2>&1
    if [ -s /etc/ssl/certs/ca-certificates.crt ]; then
        ok "generated ($(grep -c 'BEGIN CERTIFICATE' /etc/ssl/certs/ca-certificates.crt) CAs)"
    else
        warn "update-ca-certificates produced nothing"
    fi
}

# --- pacman ----------------------------------------------------------------
# SYMPTOM: `pacman -Q` prints NOTHING while ~160 packages are installed.
# CAUSE: pacman here is built --prefix=/usr/local, so its compiled-in DBPath
# is /var/local/lib/pacman -- but the database is at /var/lib/pacman, because
# that is where it was installed from the build host with --root/--dbpath.
# The stock config has every path commented out.
# ALSO: CheckSpace needs /etc/mtab, which GNU/Hurd does not have; it fails
# with "could not determine filesystem mount points" then "not enough free
# disk space". GPGDir must exist even with SigLevel = Never.
fix_pacman() {
    head_ "pacman configuration"
    local conf=/usr/local/etc/pacman.conf
    [ -d /usr/local/etc ] || { skip "pacman not installed"; return; }
    mkdir -p /var/cache/pacman/pkg /etc/pacman.d/gnupg /etc/pacman.d/hooks /var/log
    cat > "$conf" <<'PACMAN'
#
# /usr/local/etc/pacman.conf -- Oryx Hurd
#
# pacman is built --prefix=/usr/local, so its compiled defaults point at
# /var/local/lib/pacman. The real database is /var/lib/pacman. Without these
# explicit paths `pacman -Q` silently reports nothing.
#
[options]
RootDir      = /
DBPath       = /var/lib/pacman/
CacheDir     = /var/cache/pacman/pkg/
LogFile      = /var/log/pacman.log
GPGDir       = /etc/pacman.d/gnupg/
HookDir      = /etc/pacman.d/hooks/

HoldPkg      = pacman glibc
Architecture = i686

# procps here is the local +hurd.1 rebuild. Stock sid's procps needs
# libproc2.so.1 and does not work on Hurd, so it must never be pulled in as
# an upgrade -- doing so removes ps, uptime and vmstat. See FIXES.md 8 and 13.
IgnorePkg    = procps libproc2-0

# Nothing is signed: this tree is repackaged from Debian binaries.
SigLevel           = Never
LocalFileSigLevel  = Never
RemoteFileSigLevel = Never

# CheckSpace needs /etc/mtab, which GNU/Hurd does not have. Leave it off.
#CheckSpace
VerbosePkgLists
ParallelDownloads = 2

PACMAN
    # The repository, appended rather than embedded: the server address is
    # host-specific, the same reason --net is opt-in. Override with
    # ORYX_REPO_URL=http://host:port before running.
    cat >> "$conf" <<PACMANREPO

# Packages are repackaged Debian hurd-i386 binaries and are not signed.
# Served from the build host by oryx-serve.sh; if that is not running,
# pacman -Sy fails and only pacman -U works.
[oryx]
Server = ${ORYX_REPO_URL:-http://192.168.2.210:8099}
PACMANREPO
    ok "wrote $conf"
    # pacman lives in /usr/local/bin, which is not in every PATH.
    [ -x /usr/local/bin/pacman ] && ln -sfn /usr/local/bin/pacman /usr/bin/pacman \
        && ok "/usr/bin/pacman -> /usr/local/bin/pacman"
    local n; n=$(pacman -Q 2>/dev/null | wc -l)
    ok "pacman reports $n packages"
}

# --- boot scripts ----------------------------------------------------------
# SYMPTOM 1: / is read-only after every boot; you must run
#            `fsysopts / --writable` by hand after logging in.
# SYMPTOM 2: the console keyboard layout setting never applies.
# CAUSE: /etc/rcS.d was EMPTY. Nothing remounted the root, and hurd-console
# was wired into no runlevel at all.
# NOTE: hurd-console goes in rcS.d, NOT rc2.d as its LSB header says. init
# spawns the gettys on entering runlevel 2 and they need /dev/vcs/N/console to
# exist already; starting the console at sysinit wins that race. It must come
# after the read-write remount because it writes /var/run/console.pid.
fix_boot() {
    head_ "boot scripts"
    cat > /etc/init.d/hurd-rw <<'RWEOF'
#! /bin/sh
### BEGIN INIT INFO
# Provides:          hurd-rw
# Required-Start:
# Required-Stop:
# Default-Start:     S
# Default-Stop:
# Short-Description: Remount / writable and set the hostname
# Description:       Hurd boots the root filesystem read-only and nothing in
#                    this system remounted it, so every boot needed a manual
#                    "fsysopts / --writable" after login.
### END INIT INFO
PATH=/sbin:/usr/sbin:/bin:/usr/bin
case "$1" in
    start|"")
        echo "Remounting / writable..."
        fsysopts / --writable || echo "fsysopts / --writable FAILED" >&2
        [ -r /etc/hostname ] && hostname "$(cat /etc/hostname)"
        ;;
    stop|restart|force-reload|status) ;;
    *) echo "usage: $0 start" >&2; exit 2 ;;
esac
exit 0
RWEOF
    chmod 755 /etc/init.d/hurd-rw
    ln -sfn ../init.d/hurd-rw /etc/rcS.d/S05hurd-rw
    ok "rcS.d/S05hurd-rw (remount / writable)"

    if [ -x /etc/init.d/hurd-console ]; then
        # the console daemon needs /dev/vcs or it times out with
        # "Could not receive return value from daemon process"
        [ -e /dev/vcs ] || ( cd /dev && ./MAKEDEV vcs ) && :
        ln -sfn ../init.d/hurd-console /etc/rcS.d/S10hurd-console
        ok "rcS.d/S10hurd-console (console + keyboard layout)"
    else
        skip "hurd-console not installed"
    fi
}

# --- shutdown --------------------------------------------------------------
# Hurd cannot remount / read-only at shutdown, so the clean flag is never set
# and every boot forces an fsck. That is survivable. What is NOT survivable is
# an unclean halt after heavy writes: one large install followed by a bare
# reboot produced multiply-claimed blocks and ~160 unattached inodes.
# NOTE: power-off does not complete on the T400 -- it runs correctly all the
# way to "Entering sleep state [S5]" and hangs. Holding the power button there
# is safe: ext2fs has already acknowledged halt and the disk is idle.
fix_shutdown() {
    head_ "safe shutdown tools"
    mkdir -p /usr/local/sbin
    cat > /usr/local/sbin/safe-reboot <<'SHEOF'
#!/bin/sh
# Flush everything to disk before going down. Hurd cannot remount / read-only
# at shutdown, so syncing first is what keeps an interrupted write from
# turning into multiply-claimed blocks.
# Invoked as safe-halt or safe-poweroff, it halts / powers off instead.
set -e
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
echo "syncing..."
sync; sync; sleep 2; sync
echo "attempting read-only remount of / (may fail; the sync above is what matters)"
fsysopts / --readonly || echo "  -> remount refused, continuing"
case "$(basename "$0")" in
    safe-halt)     echo "halting";      exec halt ;;
    safe-poweroff) echo "powering off"; exec poweroff ;;
    *)             echo "rebooting";    exec reboot ;;
esac
SHEOF
    chmod 755 /usr/local/sbin/safe-reboot
    local n
    for n in safe-halt safe-poweroff; do
        ln -sfn safe-reboot "/usr/local/sbin/$n"
    done
    # Hurd's own login ignores /etc/login.defs, so root's console PATH does
    # not include /usr/local/sbin and the bare names fail there.
    for n in safe-reboot safe-halt safe-poweroff; do
        ln -sfn /usr/local/sbin/safe-reboot "/usr/sbin/$n"
    done
    ok "safe-reboot / safe-halt / safe-poweroff (also in /usr/sbin for PATH)"
}

# --- pacman hooks ----------------------------------------------------------
# THE structural fix, rather than the one-shot repairs above.
#
# Everything a Debian postinst would have done is missing, because deb2pkg
# drops maintainer scripts. Repairing that by hand after each install does not
# scale -- a desktop would bury you in stale caches. pacman's hook mechanism
# is the right place: declare a path trigger, run a regenerator afterwards.
#
# Each handler MUST exit 0 even on failure: a failing hook aborts the whole
# pacman transaction, which would be far worse than a stale cache.
#
# This is what lets ANY Debian hurd-i386 package be installed and just work,
# which is the whole premise of the distribution.
fix_hooks() {
    head_ "pacman hooks (postinst emulation)"
    mkdir -p /etc/pacman.d/hooks /usr/local/sbin
    local n=0 f
    if [ -d "$SELF_DIR/target/hooks" ]; then
        for f in "$SELF_DIR"/target/hooks/*.hook; do
            [ -e "$f" ] || continue
            install -m 644 "$f" /etc/pacman.d/hooks/ && n=$((n+1))
        done
        ok "$n hooks in /etc/pacman.d/hooks"
    else
        warn "no target/hooks alongside this script"
    fi
    n=0
    if [ -d "$SELF_DIR/target/sbin" ]; then
        for f in "$SELF_DIR"/target/sbin/oryx-*; do
            [ -e "$f" ] || continue
            install -m 755 "$f" /usr/local/sbin/ && n=$((n+1))
        done
        ok "$n handlers in /usr/local/sbin"
    else
        warn "no target/sbin alongside this script"
    fi
}

# --- branding --------------------------------------------------------------
fix_branding() {
    head_ "branding"
    cat > /etc/os-release <<EOSR
PRETTY_NAME="$OS_NAME (GNU/Hurd)"
NAME="$OS_NAME"
ID=$OS_ID
ID_LIKE="arch debian"
VERSION_CODENAME=forky
SUPPORT_URL="https://www.gnu.org/software/hurd/"
# A GNU/Hurd system managed with pacman, bootstrapped from Debian hurd-i386
# binaries. ID_LIKE keeps both lineages visible.
EOSR
    ok "/etc/os-release -> $OS_NAME"
    printf '%s (GNU/Hurd) \\n \\l\n\n' "$OS_NAME" > /etc/issue
    ok "/etc/issue"
    if [ -n "${HOSTNAME_WANT:-}" ] && [ "$(cat /etc/hostname 2>/dev/null)" != "$HOSTNAME_WANT" ]; then
        echo "$HOSTNAME_WANT" > /etc/hostname
        ok "/etc/hostname -> $HOSTNAME_WANT"
    fi
}

# --- fastfetch -------------------------------------------------------------
# NOTE 1: a config without a "modules" key prints the logo and NOTHING else.
#         It does not fall back to the default module set.
# NOTE 2: logo type must be "file-raw", not "file". The oryx art contains
#         literal '$' characters, which "file" treats as color placeholders
#         and eats, shifting the art out of alignment.
# NOTE 3: fastfetch's package detection finds nothing on GNU/Hurd, so the
#         count is read from the pacman database directly.
fix_fastfetch() {
    head_ "fastfetch"
    command -v fastfetch >/dev/null 2>&1 || { skip "fastfetch not installed"; return; }
    mkdir -p /etc/xdg/fastfetch
    cat > /etc/xdg/fastfetch/config.jsonc <<FFEOF
{
  "\$schema": "https://github.com/fastfetch-cli/fastfetch/raw/dev/doc/json_schema.json",

  // System-wide: fastfetch searches /etc/xdg via XDG_CONFIG_DIRS.
  // "file-raw" NOT "file" -- the art contains literal '\$' characters which
  // "file" would treat as color placeholders and swallow.
  "logo": {
    "type": "file-raw",
    "source": "/etc/xdg/fastfetch/logo.txt",
    "padding": { "top": 1, "right": 2 }
  },

  "display": {
    "color": {
      "title": "38;5;$LOGO_COLOR",
      "keys": "38;5;$LOGO_COLOR",
      "separator": "38;5;$LOGO_COLOR",
      "output": "38;5;$LOGO_COLOR"
    }
  },

  // "modules" MUST be explicit. Omitting it prints the logo and nothing else.
  "modules": [
    "title", "separator", "os", "kernel", "uptime",
    {
      "type": "command",
      "key": "Packages",
      "text": "echo \\"\$(ls /var/lib/pacman/local | grep -c -- - ) (pacman)\\""
    },
    "shell", "terminal", "cpu", "memory", "swap", "disk",
    "localip", "locale", "break", "colors"
  ]
}
FFEOF
    ok "config.jsonc"

    if [ ! -s /etc/xdg/fastfetch/logo-plain.txt ]; then
        if [ -s "$SELF_DIR/target/logo-plain.txt" ]; then
            install -m 644 "$SELF_DIR/target/logo-plain.txt" /etc/xdg/fastfetch/logo-plain.txt
            ok "logo art installed from $SELF_DIR/target/"
        else
            warn "no logo-plain.txt (expected alongside this script in target/)"
        fi
    else
        ok "logo art already present"
    fi

    cat > /usr/local/sbin/logo-color <<'LCEOF'
#!/bin/sh
# logo-color [256-color-number] -- recolor the fastfetch logo.
# The pristine art is logo-plain.txt; this bakes an ANSI color into logo.txt.
# Colors cannot come from fastfetch's $1..$9 placeholders because the art
# contains literal '$' characters. 93 violet, 99 slate, 135 purple, 141 lavender.
set -e
N="${1:-135}"
D=/etc/xdg/fastfetch
[ -f "$D/logo-plain.txt" ] || { echo "missing $D/logo-plain.txt" >&2; exit 1; }
ESC=$(printf '\033')
sed -e "s/^/${ESC}[38;5;${N}m/" -e "s/\$/${ESC}[0m/" "$D/logo-plain.txt" > "$D/logo.txt"
echo "logo recolored to 256-color $N"
LCEOF
    chmod 755 /usr/local/sbin/logo-color
    [ -s /etc/xdg/fastfetch/logo-plain.txt ] && /usr/local/sbin/logo-color "$LOGO_COLOR" >/dev/null \
        && ok "logo colored (256-color $LOGO_COLOR)"
}

# --- console font ----------------------------------------------------------
# The vga driver takes NO font options -- it only opens
# /usr/share/hurd/vga-system.bdf, which ships as a DANGLING symlink pointing
# at /usr/src/unifont.bdf that no package provides, so the driver falls back
# to its built-in 8x16 font. VGA text mode fixes the cell width at 8px, so the
# font HEIGHT sets the row count: 8 -> 50 rows, 14 -> 28, 16 -> 25.
# WARNING: shrinking applies live, ENLARGING NEEDS A REBOOT. The driver reads
# the VGA mode already programmed rather than reprogramming the row count, and
# restarting the console server does not help either.
fix_consolefont() {
    head_ "console font"
    mkdir -p /usr/share/hurd/fonts
    local f found=0
    for f in 8x8 8x14 8x16; do
        if [ -s "$SELF_DIR/target/vga-$f.bdf" ]; then
            install -m 644 "$SELF_DIR/target/vga-$f.bdf" "/usr/share/hurd/fonts/vga-$f.bdf"
            found=1
        fi
    done
    [ "$found" = 1 ] && ok "fonts installed in /usr/share/hurd/fonts" \
                     || skip "no vga-*.bdf alongside this script (see bin/psf2bdf.py)"
    cat > /usr/local/sbin/console-font <<'CFEOF'
#!/bin/sh
# console-font [8|14|16|default] -- set the Hurd console text size.
# 8 -> 50 rows, 14 -> 28, 16 -> 25 (VGA fixes the cell width at 8px).
# ONE-WAY WITHOUT A REBOOT: shrinking applies immediately, enlarging does not.
set -e
LINK=/usr/share/hurd/vga-system.bdf
DIR=/usr/share/hurd/fonts
case "${1:-}" in
    "") [ -e "$LINK" ] && echo "current: $(readlink -f "$LINK")" \
                       || echo "current: built-in 8x16 VGA font (25 rows)"; exit 0 ;;
    8|14|16) ln -sfn "$DIR/vga-8x$1.bdf" "$LINK" ;;
    default) rm -f "$LINK" ;;
    *) echo "usage: $0 [8|14|16|default]" >&2; exit 2 ;;
esac
echo "restarting the console (the screen will blink)..."
/etc/init.d/hurd-console restart >/dev/null 2>&1 || {
    echo "console restart failed -- start it by hand" >&2; exit 1; }
echo "NOTE: a LARGER font only takes effect after a reboot."
CFEOF
    chmod 755 /usr/local/sbin/console-font
    ok "console-font installed"
}

# --- networking (opt-in) ---------------------------------------------------
# SYMPTOM: networking must be reconfigured by hand after every boot.
# CAUSE: /servers/socket/2 still carried Debian's stock passive translator
# "/hurd/pfinet -6 /servers/socket/26" -- no address at all, and IPv6 on.
# IPv6 must stay OFF: if both socket/2 and socket/26 have translators, two
# pfinets start, fight over socket/2, and one dies ("Translator died").
# Setting the passive translator means networking starts on demand with no
# init script at all. Use settrans -cp (passive only).
fix_net() {
    head_ "static networking (opt-in)"
    [ "$DO_NET" = 1 ] || { skip "not requested (pass --net)"; return; }
    settrans -cp /servers/socket/2 /hurd/pfinet \
        -i "$NET_IFACE" -a "$NET_ADDR" -m "$NET_MASK" -g "$NET_GW" \
        && ok "socket/2 -> pfinet $NET_ADDR" || warn "settrans failed"
    showtrans /servers/socket/26 2>/dev/null | grep -q . \
        && warn "socket/26 has a translator -- IPv6 must stay off, clear it" \
        || ok "socket/26 clear (IPv6 off)"
}

# --- reticulum daemon --------------------------------------------------------
# SYMPTOM: rnsd was started by hand on 2026-09-20 to bring Oryx onto the
# Reticulum mesh over TCP to the Dell (192.168.2.210:4242) -- it works
# (rnstatus shows the interface Up) but has no init script, so it will not
# come back after a reboot. The box has not rebooted since, so this is
# untested across one -- same caveat as oryx-swap.
# CAUSE: rnsd has no daemonize/pidfile support of its own, even with -s
# (--service): it stays in the foreground and just changes where it logs.
# start-stop-daemon --background --make-pidfile supplies both.
# ALSO: neither `pidof rnsd` nor `ps.procps -C rnsd` can find it. rnsd is a
# script with a #!/usr/bin/python3 shebang; the kernel execs python3 as the
# real image, so /proc's comm/exe is "python3", never "rnsd". Track it by
# pidfile, not by name.
# SAFE TO RE-RUN WHILE ONE IS ALREADY UP: RNS's shared-instance design means a
# second rnsd that loses the bind race on shared_instance_port just becomes a
# client of the first instead of erroring (Reticulum.py, is_connected_to_
# shared_instance) -- but this fix does not exploit that. It only installs the
# init script and wires it into rc2.d; it never starts or stops the process
# that is already running by hand, precisely so applying this cannot drop the
# live TCP interface to the Dell. Confirm persistence with a supervised
# reboot, not by touching the running instance from here.
fix_rnsd() {
    head_ "reticulum daemon (rnsd)"
    command -v rnsd >/dev/null 2>&1 || { skip "rnsd not installed (python3-rns)"; return; }

    # Config is managed here too, not just the init script -- otherwise
    # "reproduce this system from the script" would still need a manually
    # hand-written /root/.reticulum/config. Rewritten wholesale (like
    # fix_pacman does for pacman.conf): small, fully ours, safe to overwrite.
    # Two interfaces: the Dell (LAN, always reachable) and a live public
    # community hub, verified by real TCP connect on 2026-09-20 (see the
    # comment in the config below -- reticulum.network's own testnet is
    # decommissioned, do not re-add it), so this box is on the actual
    # internet-wide network, not only the LAN. panic_on_interface_error = No
    # so a boot with the WAN down still comes up on the Dell link instead of
    # refusing to start.
    mkdir -p /root/.reticulum
    cat > /root/.reticulum/config <<'RNSCONFEOF'
# --- oryx: written by oryx-postinst.sh rnsd -------------------------------
[reticulum]
  enable_transport = No
  share_instance = Yes
  shared_instance_port = 37428
  instance_control_port = 37429
  panic_on_interface_error = No

[logging]
  loglevel = 4

[interfaces]

  # AutoInterface needs IPv6 link-local multicast; pfinet on this box runs
  # IPv4-only on purpose (two-pfinets hazard, FIXES.md 5). Must stay disabled.
  [[Default Interface]]
    type = AutoInterface
    enabled = No

  [[Dell]]
    type = TCPClientInterface
    enabled = Yes
    target_host = 192.168.2.210
    target_port = 4242

  # A real outside-network entry point, not just the LAN link above.
  # reticulum.network's own testnet (the "amsterdam.connect.reticulum.
  # network" hub some docs still cite) was DECOMMISSIONED -- it no longer
  # resolves. There is no official replacement: Reticulum is deliberately
  # not a single network you "join" via one hardcoded hub, and current
  # entry points are community-run and listed at https://directory.rns.
  # recipes/ (also https://rmap.world/). This one was verified reachable
  # (real TCP connect, not just DNS) on 2026-09-20 -- if it ever goes dark,
  # check that directory for a live replacement rather than assuming the
  # network itself is down.
  [[Reticulum Community Hub]]
    type = TCPClientInterface
    enabled = Yes
    target_host = rns.stoppedcold.com
    target_port = 4242

  # Reticulum-over-I2P, added 2026-09-20. Needs i2pd's SAM bridge (oryx-
  # postinst.sh i2pd, [sam] enabled = true) up on 127.0.0.1:7656 -- RNS's
  # bundled RNS.vendor.i2plib talks SAM v3 and finds that address by default
  # with no config needed here. connectable = yes publishes this box's own
  # I2P b32 destination so other RNS nodes can peer TO it, not just from it.
  # panic_on_interface_error = No (set above) means a slow/absent SAM bridge
  # only takes this ONE interface down, not the whole daemon -- verified
  # 2026-09-20, see FIXES.md.
  [[I2P]]
    type = I2PInterface
    enabled = yes
    connectable = yes
    peers = ytcqa2srhvydtcnqldui2sftnmfttlflhqng4lkfp2z6uu3y3pna.b32.i2p
RNSCONFEOF
    ok "/root/.reticulum/config (Dell LAN + public community hub + I2P)"

    cat > /etc/init.d/oryx-rnsd <<'RNSEOF'
#! /bin/sh
### BEGIN INIT INFO
# Provides:          oryx-rnsd
# Required-Start:    hurd-rw $remote_fs i2pd
# Required-Stop:
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: Reticulum Network Stack daemon
# Description:       rnsd has no daemonize/pidfile support of its own -- it
#                    stays in the foreground even with -s. start-stop-daemon
#                    --background supplies both. Tracked by PIDFILE only:
#                    rnsd's comm/exe is always "python3" (shebang script), so
#                    pidof/ps -C cannot find it by name.
#                    Required-Start lists i2pd because rnsd's I2PInterface
#                    needs its SAM bridge -- but this is documentation only:
#                    this box has no insserv, ordering is by the rc2.d S-NN
#                    prefix alone (i2pd is S22, this is S25). A missing SAM
#                    bridge does not stop rnsd starting either way -- only
#                    that one interface retries and stays down (FIXES.md).
### END INIT INFO
PATH=/sbin:/usr/sbin:/bin:/usr/bin
PIDFILE=/var/run/oryx-rnsd.pid
DAEMON=/usr/bin/rnsd

running() { [ -s "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; }

case "$1" in
    start|"")
        if running; then
            echo "oryx-rnsd already running ($(cat "$PIDFILE"))"; exit 0
        fi
        # A shared instance already up (started outside this script, e.g. by
        # hand) is left alone -- starting a managed one on top would just add
        # a redundant client, and this script has no business tearing down a
        # process it does not own.
        if rnstatus 2>/dev/null | grep -A1 'Shared Instance' | grep -q Up; then
            echo "a Reticulum shared instance is already up (not started by this script)"
            exit 0
        fi
        echo "Starting Reticulum daemon..."
        start-stop-daemon --start --background --make-pidfile --pidfile "$PIDFILE" \
            --exec "$DAEMON" -- -s \
            || echo "rnsd FAILED to start" >&2
        ;;
    stop)
        if ! running; then
            echo "oryx-rnsd not running (or was started outside this script)"; exit 0
        fi
        start-stop-daemon --stop --pidfile "$PIDFILE" --retry 5
        rm -f "$PIDFILE"
        ;;
    restart|force-reload) "$0" stop; sleep 1; "$0" start ;;
    status)
        if running; then echo "running ($(cat "$PIDFILE"))"; else echo "not running"; fi
        ;;
    *) echo "usage: $0 {start|stop|restart|status}" >&2; exit 2 ;;
esac
exit 0
RNSEOF
    chmod 755 /etc/init.d/oryx-rnsd
    ok "/etc/init.d/oryx-rnsd"
    ln -sfn ../init.d/oryx-rnsd /etc/rc2.d/S25oryx-rnsd
    ok "rc2.d/S25oryx-rnsd (after S20ssh and S22i2pd; this box only populates rc2.d, see fix_boot)"
    if rnstatus 2>/dev/null | grep -A1 'Shared Instance' | grep -q Up; then
        ok "a Reticulum instance is already up -- left running, untouched"
        warn "it has no pidfile (started outside this script) -- not proven to"
        warn "survive a reboot yet; confirm with a supervised reboot"
    else
        skip "no instance running -- will start on next boot via rc2.d"
    fi
}

# --- i2pd ---------------------------------------------------------------
# SYMPTOM: i2pd is installed (a real Debian hurd-i386 build -- pure C++,
# proxy-based, no TUN/TAP needed, unlike Yggdrasil) but its init script,
# /etc/init.d/i2pd, chuids to a user "i2pd" that does not exist, so it cannot
# even test-start.
# CAUSE: same root cause as everything else in this file -- deb2pkg drops
# maintainer scripts. But this is a DIFFERENT postinst mechanism than #14's
# sysusers.d: older-style Debian packaging (i2pd depends on plain `adduser`,
# not a `usr/lib/sysusers.d/*` fragment), so the 90-sysusers.hook does not and
# cannot catch it -- there is no declarative fragment to read, only the
# deleted postinst script's `adduser --system i2pd` call. Expect this for any
# future package whose postinst creates a user the adduser-classic way.
fix_i2pd() {
    head_ "i2pd (I2P router)"
    command -v i2pd >/dev/null 2>&1 || { skip "i2pd not installed"; return; }
    if getent passwd i2pd >/dev/null 2>&1; then
        ok "user i2pd already present"
    else
        groupadd --system i2pd 2>/dev/null
        useradd --system --gid i2pd --home-dir /var/lib/i2pd --no-create-home \
            --shell /bin/false i2pd \
            && ok "created system user/group i2pd (its adduser postinst never ran)" \
            || warn "useradd i2pd failed"
    fi
    mkdir -p /var/lib/i2pd /run/i2pd /var/log/i2pd
    chown -R i2pd:i2pd /var/lib/i2pd /run/i2pd /var/log/i2pd /etc/i2pd 2>/dev/null
    ok "/var/lib/i2pd, /run/i2pd, /var/log/i2pd owned by i2pd"

    # S22, BEFORE S25oryx-rnsd -- added 2026-09-20 when rnsd grew an
    # I2PInterface (needs i2pd's SAM bridge). Was S30 (after rnsd) until
    # then, which happened to be harmless only because rnsd's I2P interface
    # fails independently and retries rather than blocking the daemon (see
    # FIXES.md) -- fixing the ordering removes the guaranteed-to-fail first
    # attempt at every boot instead of relying on that resilience.
    rm -f /etc/rc2.d/S30i2pd
    ln -sfn ../init.d/i2pd /etc/rc2.d/S22i2pd
    ok "rc2.d/S22i2pd (before S25oryx-rnsd; this box only populates rc2.d)"

    # SYMPTOM: started via the init script (start-stop-daemon --chuid i2pd),
    # i2pd comes up, opens its proxies, but NEVER reseeds -- no "Reseed:" log
    # line at all, netDb stays empty, every tunnel build fails "no peers
    # available" indefinitely. Started as root with an explicit --datadir it
    # reseeds and builds real tunnels within a minute.
    # CAUSE: the shipped init script's DAEMON_OPTS never passes --datadir, so
    # i2pd falls back to resolving it from $HOME -- and start-stop-daemon
    # --chuid changes uid/gid but does not reliably export a new $HOME for
    # the target user, unlike a real login. i2pd silently ends up reading/
    # writing an unintended (and on this box, uninitialised) directory
    # instead of /var/lib/i2pd, with nothing logged about it at any level.
    # FIX: pin --datadir explicitly via /etc/default/i2pd, the init script's
    # own sanctioned override point (sourced AFTER DAEMON_OPTS is built, so it
    # must append rather than assign) -- not by hand-editing the shipped init
    # script, which a reinstall would silently revert.
    if ! grep -q '^DAEMON_OPTS="\$DAEMON_OPTS --datadir' /etc/default/i2pd 2>/dev/null; then
        printf 'DAEMON_OPTS="$DAEMON_OPTS --datadir=/var/lib/i2pd"\n' >> /etc/default/i2pd
        ok "/etc/default/i2pd pins --datadir=/var/lib/i2pd (chuid does not export \$HOME)"
    else
        skip "/etc/default/i2pd already pins --datadir"
    fi

    # Debian's own shipped i2pd.conf already has ipv6 = false -- consistent
    # with FIXES.md 5 (two-pfinet hazard) with nothing to do. What it does NOT
    # do out of the box: enable the local proxies, or set a bandwidth class
    # sane for a ThinkPad T400 rather than i2pd's default assumption of a
    # dedicated server. Same "modest citizen, not a heavy relay" call already
    # made for rnsd's leaf (non-transport) config.
    # TRAP: global (non-sectioned) keys MUST appear before the first
    # [section] header -- i2pd's ini parser attributes anything after a
    # header to that section. A plain `cat >>` lands after the file's last
    # section ([persist]), which turned "bandwidth = L" into the nonexistent
    # option "persist.bandwidth" and i2pd refused to start. Use python3 to
    # insert before the first "[" rather than appending blindly.
    #
    # Rewritten wholesale each run (strip-then-regenerate by known exact
    # markers) rather than "apply once, skip forever" -- so adding a new
    # oryx-managed setting later (SAM, added 2026-09-20 for Reticulum-over-
    # I2P) is just editing this script and re-running it, matching how
    # fix_rnsd treats /root/.reticulum/config. Safe: everything between the
    # two markers below is entirely ours.
    python3 - <<'PYEOF'
p = "/etc/i2pd/i2pd.conf"
s = open(p).read()
GSTART, GEND = "# --- oryx globals start ---\n", "# --- oryx globals end ---\n"
TSTART = "# --- oryx sections start ---\n"
for start, end in ((GSTART, GEND), (TSTART, None)):
    i = s.find(start)
    if i == -1:
        continue
    j = s.find(end, i) + len(end) if end else len(s)
    s = s[:i] + (s[j:] if end else "")
globals_block = (
    GSTART +
    "# These MUST stay above the first [section] below -- i2pd attributes\n"
    "# trailing keys to whatever section precedes them otherwise (see\n"
    "# FIXES.md, i2pd section).\n"
    "bandwidth = L\n"
    "share = 50\n" +
    GEND
)
i = s.index("\n[")
s = s[:i] + "\n" + globals_block + s[i:]
s = s.rstrip("\n") + "\n\n" + TSTART + (
    "[http]\nenabled = true\naddress = 127.0.0.1\nport = 7070\n"
    "\n[httpproxy]\nenabled = true\naddress = 127.0.0.1\nport = 4444\n"
    "\n[socksproxy]\nenabled = true\naddress = 127.0.0.1\nport = 4447\n"
    # SAM bridge, for Reticulum's I2PInterface (RNS.vendor.i2plib speaks SAM
    # v3 over this). Loopback only -- LXMF/RNS traffic on the Dell reaches
    # it over the Dell's own i2pd SAM bridge on ITS OWN loopback, never this
    # one directly; the two i2pd instances talk to each other over the real
    # I2P network, not to each other's SAM ports.
    "\n[sam]\nenabled = true\naddress = 127.0.0.1\nport = 7656\n"
)
open(p, "w").write(s)
PYEOF
    ok "oryx defaults applied to i2pd.conf (globals before first section; proxies + SAM on 127.0.0.1; bandwidth L)"
}

# --- oryx-get ----------------------------------------------------------
# Config is generated, not shipped in the package -- oryx-get.conf is
# entirely Oryx's own creation (no upstream default to preserve, unlike
# i2pd.conf), so this follows the same pattern as fix_pacman/fix_rnsd
# rather than the package carrying a real /etc file. Reuses the package's
# OWN default-config logic (oryxget.config.ensure_default) instead of
# duplicating that text into this script, which would drift the moment
# either copy changed.
fix_oryx_get() {
    head_ "oryx-get"
    command -v oryx-get >/dev/null 2>&1 || { skip "oryx-get not installed"; return; }
    python3 -c "from oryxget import config; config.ensure_default()" \
        && ok "/etc/oryx-get.conf (written if missing; existing config left alone)" \
        || warn "could not write /etc/oryx-get.conf"
    mkdir -p /var/cache/oryx-get
    ok "/var/cache/oryx-get"
}

# ================================================================== main ====
ALL="alternatives ptys cacerts pacman hooks boot shutdown branding fastfetch consolefont net rnsd i2pd oryx_get"

case "${1:-}" in
    --list) echo "$ALL" | tr ' ' '\n'; exit 0 ;;
esac

args=()
for a in "$@"; do
    case "$a" in
        --net) DO_NET=1 ;;
        -*) echo "unknown option: $a" >&2; exit 2 ;;
        *) args+=("$a") ;;
    esac
done
[ ${#args[@]} -gt 0 ] || args=($ALL)

need_root
writable_root || { echo "cannot make / writable" >&2; exit 1; }

printf '\033[1mOryx Hurd post-install\033[0m  (%s)\n' "$(date)"
for f in "${args[@]}"; do
    if declare -f "fix_$f" >/dev/null; then
        "fix_$f"
    else
        echo "no such fix: $f" >&2
    fi
done

head_ "done"
echo "  Reboot to confirm: / writable at login, network up unattended,"
echo "  sshd accepting connections. Expect a forced fsck -- that is normal,"
echo "  Hurd never gets to set the clean flag."
