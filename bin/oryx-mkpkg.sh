#!/bin/bash
# oryx-mkpkg.sh -- build a pacman package from a staging directory.
#
#   oryx-mkpkg.sh -n NAME -v VERSION -d DESC [-a arch] [-D dep]... STAGEDIR [OUTDIR]
#
# The Dell has no makepkg and the target has no build environment worth
# speaking of, so this is how Oryx's OWN packages get made -- as opposed to
# deb2pkg-dell.sh, which repackages Debian's. oryx-base and oryx-branding
# will be built with this too.
#
# -v VERSION MUST be a full "pkgver-pkgrel" string (e.g. "1.0-1"), NEVER a
# bare pkgver ("1.0") -- this script does not append a pkgrel itself, unlike
# deb2pkg-dell.sh/oryx-wheel2pkg.sh. Skipping it broke `pacman -S oryx-get`
# outright with errors that never even mentioned oryx-get by name: libalpm
# recovers name/version/rel from a sync-db directory name by splitting from
# the RIGHT, and with no real rel segment to anchor on, a HYPHENATED
# package name like "oryx-get" (directory "oryx-get-1.0") gets misparsed as
# name="oryx" version="get" rel="1.0". See FIXES.md, oryx-get section.
#
# STAGEDIR is the package root: what is at STAGEDIR/usr/bin/foo lands at
# /usr/bin/foo. Everything about the archive layout that pacman cares about
# is handled here -- see the comments below, each of which is a bug that
# already bit once.
set -euo pipefail

NAME=; VER=; DESC=; ARCH=i686; DEPS=()
while getopts "n:v:d:a:D:" o; do
    case "$o" in
        n) NAME=$OPTARG ;;
        v) VER=$OPTARG ;;
        d) DESC=$OPTARG ;;
        a) ARCH=$OPTARG ;;
        D) DEPS+=("$OPTARG") ;;
        *) echo "usage: $0 -n NAME -v VERSION -d DESC [-a arch] [-D dep]... STAGEDIR [OUTDIR]" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
STAGE=${1:-}; OUT=${2:-$HOME/Desktop/hurd/repo}
[ -n "$NAME" ] && [ -n "$VER" ] && [ -d "${STAGE:-}" ] || {
    echo "usage: $0 -n NAME -v VERSION -d DESC [-a arch] [-D dep]... STAGEDIR [OUTDIR]" >&2
    exit 2
}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
STAGE=$(cd "$STAGE" && pwd)

# Reproducibility: a fixed timestamp, so rebuilding the same tree gives the
# same bytes. Otherwise every rebuild changes the package's sha256 under an
# unchanged filename and every cached copy on the target fails its checksum.
# See FIXES.md 13.
SDE=${SOURCE_DATE_EPOCH:-$(git -C "$(dirname "$0")" log -1 --format=%ct 2>/dev/null || echo 1700000000)}

# pacman compares directory modes against what is already on the filesystem
# and warns for every mismatch ("directory permissions differ on /usr/").
# A staging tree made under the usual umask gives 775, so normalise.
find "$STAGE" -type d -exec chmod 755 {} +

size=$(du -sb "$STAGE" 2>/dev/null | cut -f1)
{
    echo "pkgname = $NAME"
    echo "pkgbase = $NAME"
    echo "pkgver = $VER"
    echo "pkgdesc = ${DESC:-$NAME}"
    echo "url = https://oryx.invalid/$NAME"
    echo "builddate = $SDE"
    echo "packager = oryx-mkpkg"
    echo "size = ${size:-0}"
    echo "arch = $ARCH"
    echo "license = custom"
    for d in ${DEPS+"${DEPS[@]}"}; do echo "depend = $d"; done
} > "$STAGE/.PKGINFO"

# Member paths must be "usr/bin/foo", NOT "./usr/bin/foo": pacman accepts a
# ./-prefixed archive without complaint and then installs NOTHING, registering
# an empty package. .PKGINFO must come first. Hence the explicit member list
# rather than a plain `tar -cf - .`.
FILE="$OUT/${NAME}-${VER}-${ARCH}.pkg.tar.zst"
( cd "$STAGE" && { printf '.PKGINFO\0'
                   find . -mindepth 1 -maxdepth 1 ! -name .PKGINFO -printf '%P\0' | sort -z
                 } | tar --format=gnu --null -T - -cf - --sort=name \
                       --mtime="@$SDE" --owner=0 --group=0 \
                 | zstd -q -f -o "$FILE" )

rm -f "$STAGE/.PKGINFO"
echo "$FILE"
