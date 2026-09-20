#!/bin/bash
# oryx-get-mkpkg.sh -- build the REAL oryx-get pacman package.
#
# Unlike oryx-linux, oryx-get is pure Python: no target compilation needed,
# so this runs entirely on the Dell and calls oryx-mkpkg.sh directly, the
# same tool oryx-base/oryx-branding are meant to use.
#
# Layout mirrors python3-rns/python3-lxmf (oryx-wheel2pkg.sh's own
# convention): the importable package goes in /usr/lib/python3/dist-
# packages, already on every install's sys.path with no PYTHONPATH or
# sys.path hack needed. debindex.py ships ALONGSIDE oryxget/ there too --
# not a copy that can drift, but the same file bin/debindex.py already is,
# placed where oryxget/debian_source.py's own path search already expects
# it (see that file). oryx-repo-add.py -- a script, not a module -- goes to
# /usr/lib/oryx-get/ instead, so it never LOOKS like part of the importable
# package.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HOME/Desktop/hurd/repo}

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/usr/bin" \
         "$STAGE/usr/lib/python3/dist-packages" \
         "$STAGE/usr/lib/oryx-get" \
         "$STAGE/usr/share/doc/oryx-get"

install -m 755 "$HERE/oryx-get" "$STAGE/usr/bin/oryx-get"
cp -a "$HERE/oryxget" "$STAGE/usr/lib/python3/dist-packages/oryxget"
find "$STAGE/usr/lib/python3/dist-packages/oryxget" -name "__pycache__" -type d -exec rm -rf {} + 2>/dev/null || true
install -m 644 "$HERE/../debindex.py" "$STAGE/usr/lib/python3/dist-packages/debindex.py"
install -m 755 "$HERE/../oryx-repo-add.py" "$STAGE/usr/lib/oryx-get/oryx-repo-add.py"
[ -f "$HERE/README.md" ] && install -m 644 "$HERE/README.md" "$STAGE/usr/share/doc/oryx-get/README.md"

# -v MUST be a full pkgver-pkgrel string, e.g. "1.0-1", not bare "1.0" --
# oryx-mkpkg.sh does not append a pkgrel itself. Skipping it broke
# `pacman -S oryx-get` outright: libalpm splits a sync-db directory name
# ("oryx-get-1.0") from the RIGHT to recover name/version/rel, and with no
# real pkgrel component to anchor that split, a HYPHENATED package name
# ("oryx-get") gets misparsed as name="oryx" version="get" rel="1.0" --
# surfacing as "oryx database is inconsistent: name/version mismatch on
# package oryx" and "target not found: oryx-get", neither of which
# mentions oryx-get by name (see FIXES.md, oryx-get section).
"$HERE/../oryx-mkpkg.sh" \
    -n oryx-get -v 1.0-1 \
    -d "Fetch a package and make it installable -- no Oryx server involved" \
    -a any \
    -D python3 -D zstd -D gpgv -D debian-ports-archive-keyring \
    "$STAGE" "$OUT"
