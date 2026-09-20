#!/bin/sh
# run-tests.sh -- what the Linux personality can actually run.
#
# Every case is a real Debian i386 static busybox applet executed under
# oryxlinux on GNU/Hurd. Where a comparison is meaningful the expected value
# is produced by the HOST's own tools, so the test proves the emulated result
# matches what Hurd itself says -- not merely that something was printed.
#
#   ./run-tests.sh [path-to-oryxlinux] [path-to-busybox]
set -u
EMU=${1:-./oryxlinux}
BB=${2:-tests/busybox}
pass=0; fail=0

ok()   { pass=$((pass+1)); printf '  ok    %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  FAIL  %s\n      got: %s\n      want: %s\n' "$1" "$2" "$3"; }

# check <name> <expected> <busybox args...>
check() {
    name=$1; want=$2; shift 2
    got=$("$EMU" "$BB" "$@" 2>&1 | head -5)
    if [ "$got" = "$want" ]; then ok "$name"; else bad "$name" "$got" "$want"; fi
}

echo "== Linux personality test suite =="
echo "emulator: $EMU"
echo "guest:    $BB"
echo

echo "-- basics"
check "echo"        "hello"                "echo" "hello"
check "pwd"         "$(pwd)"               "pwd"
check "uname -s"    "Linux"                "uname" "-s"
check "id -u"       "$(id -u)"             "id" "-u"
check "basename"    "c"                    "basename" "/a/b/c"

echo "-- files (compared against the host's own answer)"
check "cat"         "$(cat /etc/hostname)"          "cat" "/etc/hostname"
check "md5sum"      "$(md5sum /etc/hostname)"       "md5sum" "/etc/hostname"
check "sha256sum"   "$(sha256sum /etc/hostname)"    "sha256sum" "/etc/hostname"
check "wc -c"       "$(wc -c < /etc/hostname) /etc/hostname" "wc" "-c" "/etc/hostname"
check "head"        "$(head -2 /etc/passwd)"        "head" "-2" "/etc/passwd"

echo "-- directories (getdents64 + statx)"
check "ls"          "$(ls /etc/apt)"                "ls" "/etc/apt"
check "find"        "$(find /etc/apt -type f | sort)" "find" "/etc/apt" "-type" "f"

echo "-- text processing"
check "sed"         "$(sed s/o/O/ /etc/hostname)"   "sed" "s/o/O/" "/etc/hostname"
check "sort"        "$(sort /etc/hostname)"         "sort" "/etc/hostname"
check "grep"        "$(grep -c . /etc/hostname)"    "grep" "-c" "." "/etc/hostname"

echo "-- pipes and fds (dup3)"
# The host has no hexdump, so compare against od, which it does have. Both
# render the same bytes; only the framing differs, so compare the byte column.
hex_want=$(od -An -tx1 /etc/hostname | tr -s " " | sed "s/^ //;s/ $//")
hex_got=$("$EMU" "$BB" hexdump -C /etc/hostname 2>&1 | head -1 |
          cut -c11-58 | tr -s " " | sed "s/^ //;s/ $//")
if [ "$hex_got" = "$hex_want" ]; then ok "hexdump"; else bad "hexdump" "$hex_got" "$hex_want"; fi

echo
printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
