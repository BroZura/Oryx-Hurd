#!/bin/bash
# build.sh -- assemble the oryx-get deployment tarball.
#
# Copies bin/debindex.py in alongside oryx-get's own files rather than
# forking it -- bin/debindex.py stays the one canonical copy, shared with
# closure.py and oryx-repo-add.py on the Dell (see oryxget/debian_source.py
# for the import-path side of this).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-/tmp/oryx-get.tar.gz}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cp -a "$HERE/oryx-get" "$HERE/oryxget" "$WORK/"
cp "$HERE/../debindex.py" "$WORK/debindex.py"
cp "$HERE/../oryx-repo-add.py" "$WORK/oryx-repo-add.py"
find "$WORK" -name "__pycache__" -type d -exec rm -rf {} + 2>/dev/null || true
chmod +x "$WORK/oryx-get"

tar -czf "$OUT" -C "$WORK" .
echo "$OUT"
