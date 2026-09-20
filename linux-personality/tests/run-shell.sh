#!/bin/sh
# run-shell.sh -- fork, execve, wait4 and pipes, via a real Linux shell.
#
# These are the cases that need a process model rather than a single task:
# busybox ash forks, execs another Linux binary inside the emulator, and
# waits for it; pipelines need two live guest processes exchanging data
# through a host pipe while BOTH are being serviced by the same emulator.
#
#   ./run-shell.sh [path-to-oryxlinux] [path-to-busybox]
set -u
EMU=${1:-./oryxlinux}
BB=${2:-tests/busybox}
BBA=$(cd "$(dirname "$BB")" && pwd)/$(basename "$BB")   # absolute, for the guest
pass=0; fail=0

ok()  { pass=$((pass+1)); printf '  ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf '  FAIL  %s\n      got: %s\n      want: %s\n' "$1" "$2" "$3"; }

sh_is() {   # name, expected, script
    name=$1; want=$2; shift 2
    got=$("$EMU" "$BB" sh -c "$1" 2>&1 | head -5)
    [ "$got" = "$want" ] && ok "$name" || bad "$name" "$got" "$want"
}

echo "== shell: fork / execve / wait4 / pipes =="
echo

echo "-- builtins only (no fork)"
sh_is "loop"        "$(printf '1\n2\n3')"  'for i in 1 2 3; do echo $i; done'
sh_is "conditional" "yes"                  'if true; then echo yes; fi'
sh_is "variable"    "hello-var"            'X=hello; echo $X-var'

echo "-- fork + execve + wait4"
sh_is "exec a Linux binary" "forked"       "$BBA echo forked"
sh_is "sequence waits"      "$(printf 'a\nb')" "$BBA echo a; $BBA echo b"
sh_is "exit status"         "7"            "$BBA false; echo 7"

echo "-- pipes between two guest processes"
sh_is "simple pipe"    "piped"             "echo piped | $BBA cat"
sh_is "two-stage pipe" "a"                 "echo a | $BBA cat | $BBA cat"
sh_is "cmd substitution" "sub: inner"      "echo sub: \$($BBA echo inner)"

# The emulated pipeline must agree with the host's own answer.
want=$(cat /etc/hostname | md5sum)
got=$("$EMU" "$BB" sh -c "$BBA cat /etc/hostname | $BBA md5sum" 2>&1 | head -1)
[ "$got" = "$want" ] && ok "piped md5sum matches host" \
                     || bad "piped md5sum matches host" "$got" "$want"

echo
printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
