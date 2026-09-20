#!/bin/sh
# Serve the Oryx package repository over HTTP for the T400.
#
#   oryx-serve.sh [REPODIR] [PORT]
#
# Defaults to ~/Desktop/hurd/repo on port 8099, bound to all interfaces so the
# T400 on the LAN can reach it. Foreground; Ctrl-C to stop.
#
# The target fetches from here, so this has to be running for `pacman -Sy` or
# `pacman -S` on Oryx to work. Nothing is signed (SigLevel = Never), which is
# fine for a LAN repo of locally repackaged binaries and would not be fine for
# anything published.
set -eu

REPODIR="${1:-$HOME/Desktop/hurd/repo}"
PORT="${2:-8099}"

[ -d "$REPODIR" ] || { echo "no such repo directory: $REPODIR" >&2; exit 1; }
[ -f "$REPODIR/oryx.db.tar.gz" ] || {
    echo "no oryx.db.tar.gz in $REPODIR -- run oryx-repo-add.py first" >&2
    exit 1
}

# The LAN address, not the VPN one: the T400 has to be able to route to it.
IP=$(ip -4 -o addr show scope global 2>/dev/null \
     | awk '$4 ~ /^192\.168\./ {split($4,a,"/"); print a[1]; exit}')
[ -n "$IP" ] || IP=$(hostname -I 2>/dev/null | awk '{print $1}')

echo "serving $REPODIR on port $PORT"
echo
echo "on the target, in /usr/local/etc/pacman.conf:"
echo
echo "    [oryx]"
echo "    Server = http://${IP:-<dell-ip>}:$PORT"
echo
exec python3 -m http.server "$PORT" --bind 0.0.0.0 --directory "$REPODIR"
