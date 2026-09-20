#!/bin/sh
# run-gnu.sh -- unmodified Debian i386 GNU coreutils binaries under oryxlinux.
#
# These are not busybox applets written to be small and portable; they are the
# real GNU tools from Debian's coreutils package, dynamically linked against
# glibc and pulled in through ld-linux.so.2, libselinux, libcap, libacl,
# libattr, libgmp and libpcre2.
#
# Needs a sysroot holding the Linux loader and those libraries:
#   ORYX_SYSROOT=/root/lp/sysroot ./run-gnu.sh
set -u
EMU=${1:-./oryxlinux}
DIR=${2:-tests/gnu}
: "${ORYX_SYSROOT:?set ORYX_SYSROOT to the Linux sysroot}"
export ORYX_SYSROOT
pass=0; fail=0

ok()  { pass=$((pass+1)); printf '  ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf '  FAIL  %s\n      got: %s\n      want: %s\n' "$1" "$2" "$3"; }

cmp_host() {   # name, tool, args... -- compare against the host's own tool
    name=$1; tool=$2; shift 2
    want=$("$tool" "$@" 2>&1 | head -3)
    got=$("$EMU" "$DIR/$tool" "$@" 2>&1 | head -3)
    [ "$got" = "$want" ] && ok "$name" || bad "$name" "$got" "$want"
}

echo "== GNU coreutils under the Linux personality =="
echo "sysroot: $ORYX_SYSROOT"
echo

# Does it start at all, i.e. did ld.so resolve every library?
v=$("$EMU" "$DIR/ls" --version 2>&1 | head -1)
case "$v" in
    "ls (GNU coreutils)"*) ok "ld.so resolved all libraries ($v)" ;;
    *) bad "ld.so resolution" "$v" "ls (GNU coreutils) ..." ;;
esac

cmp_host "sha256sum matches host" sha256sum /etc/hostname
cmp_host "wc -c matches host"     wc -c /etc/hostname
cmp_host "ls -a matches host"     ls -a /etc/apt

echo
printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
