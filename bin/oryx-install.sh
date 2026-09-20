#!/bin/bash
# oryx-install.sh [-n] <package> [package...]
#
# Install Debian hurd-i386 packages onto the Oryx Hurd box, driven from HERE
# (the Dell). Resolves the dependency closure, converts each .deb into a
# pacman package, copies them over and installs them in ONE transaction.
#
# The T400 never has to boot Debian for this: repacking a .deb is
# architecture-independent and this machine is Debian-derived, so it has
# dpkg-deb. Oryx Hurd itself has no apt, no curl and no bsdtar.
#
#   -n   dry run: resolve and report, download and install nothing
#   -p   push mode: scp the packages over and `pacman -U` them, the old way,
#        for when the repo is not being served
#
# By default the converted packages are published into the repository
# (~/Desktop/hurd/repo) and the target pulls them over HTTP with `pacman -S`.
# That needs oryx-serve.sh running on this machine. The packages stay in the
# repo afterwards, so the target can reinstall them without the Dell doing
# any conversion work again.
#
# Packages already installed on the target are SKIPPED, never upgraded. That
# is deliberate: the target's glibc came from the same archive at a known
# version and silently bumping it underneath a running Hurd is not something
# to do as a side effect of installing an unrelated package.
#
# Remember: maintainer scripts are dropped by the converter, so after
# installing anything that relies on them, run fix-alternatives.sh on the
# target, and check for other postinst work (glib schemas, mime/icon caches,
# fc-cache, system users).
set -euo pipefail

HOST=${ORYX_HOST:-archhurd}
BASE=${ORYX_BASE:-$HOME/Desktop/hurd}
MIRROR=http://deb.debian.org/debian-ports
IDXURL=$MIRROR/dists/sid/main/binary-hurd-i386/Packages.xz
IDX=$BASE/Packages
DEBS=$BASE/debs
PKGS=$BASE/pkgs
HERE=$(cd "$(dirname "$0")" && pwd)

REPODIR=${ORYX_REPO:-$BASE/repo}

DRY=0
REPO=1
PUBONLY=0
while [ $# -gt 0 ]; do
    case "$1" in
        -n) DRY=1; shift ;;
        -p) REPO=0; shift ;;     # push over scp instead of publishing
        -P) PUBONLY=1; shift ;;  # publish to the repo, install nothing
        --) shift; break ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *)  break ;;
    esac
done
[ $# -gt 0 ] || { echo "usage: $0 [-n] [-p] [-P] <package>..." >&2; exit 2; }

mkdir -p "$DEBS" "$PKGS"

# Refresh the package index if missing or older than a day.
if [ ! -s "$IDX" ] || [ -n "$(find "$IDX" -mtime +1 2>/dev/null)" ]; then
    echo ":: fetching package index"
    curl -sfL -o "$IDX.xz" "$IDXURL"
    xz -df "$IDX.xz"
fi

echo ":: resolving closure for: $*"
# Declared up front: the trap fires on any early exit, and under `set -u` an
# unset variable in it masks the real error with "unbound variable".
INST=; TODO=; STAGE=
CLOSURE=$(mktemp); trap 'rm -f "$CLOSURE" "$INST" "$TODO"; rm -rf "$STAGE"' EXIT
python3 "$HERE/closure.py" "$IDX" --list "$@" > "$CLOSURE"

INST=$(mktemp)
TODO=$(mktemp)
if [ "$PUBONLY" = 1 ]; then
    # Seeding the repo: take the closure as-is. Skipping what the target
    # already has is exactly wrong here, since the usual reason to publish is
    # that an installed package is missing from the repo.
    cp "$CLOSURE" "$TODO"
else
    echo ":: querying $HOST for what is already installed"
    ssh "$HOST" 'pacman -Q 2>/dev/null' | cut -d" " -f1 | sort -u > "$INST"
    while IFS=$'\t' read -r name ver file; do
        grep -qxF "$name" "$INST" || printf '%s\t%s\t%s\n' "$name" "$ver" "$file"
    done < "$CLOSURE" > "$TODO"
fi

total=$(wc -l < "$CLOSURE"); todo=$(wc -l < "$TODO")
echo ":: $total in closure, $((total - todo)) already installed, $todo to install"
[ "$todo" -eq 0 ] && { echo ":: nothing to do"; exit 0; }
cut -f1 "$TODO" | paste -sd' ' | fmt -w 76 | sed 's/^/   /'

if [ "$DRY" = 1 ]; then echo ":: dry run, stopping here"; exit 0; fi

echo ":: downloading"
while IFS=$'\t' read -r name ver file; do
    out="$DEBS/${file##*/}"
    [ -s "$out" ] || curl -sfL -o "$out" "$MIRROR/$file" || { echo "download failed: $name" >&2; exit 1; }
done < "$TODO"

echo ":: converting to pacman packages"
STAGE=$(mktemp -d)
while IFS=$'\t' read -r name ver file; do
    "$HERE/deb2pkg-dell.sh" "$DEBS/${file##*/}" "$STAGE" >/dev/null
done < "$TODO"
ls "$STAGE" | wc -l | xargs printf '   %s packages built\n'

cp -a "$STAGE"/*.pkg.tar.zst "$PKGS"/ 2>/dev/null || true

if [ "$REPO" = 1 ]; then
    # Publish into the repository and let the target pull over HTTP. This is
    # the path that makes Oryx a distribution rather than a push target, so it
    # is the default; oryx-serve.sh has to be running.
    echo ":: publishing into $REPODIR"
    cp -a "$STAGE"/*.pkg.tar.zst "$REPODIR"/

    # Resolve dependencies INTO the packages before indexing. Order matters:
    # the database is built from the packages, and pacman rejects any package
    # whose dependencies disagree with its database entry. Doing this after
    # repo-add would produce exactly that mismatch.
    python3 "$HERE/oryx-setdeps.py" "$REPODIR" --index "$IDX" \
        | sed 's/^/   /' | tail -n 12
    python3 "$HERE/oryx-repo-add.py" "$REPODIR" | sed 's/^/   /' | tail -n 4

    if [ "$PUBONLY" = 1 ]; then
        echo ":: published only, nothing installed"
        exit 0
    fi

    echo ":: installing from the repo"
    names=$(cut -f1 "$TODO" | paste -sd' ')
    # No --nodeps: the repository carries real dependencies now, so pacman
    # resolves the transaction itself. The closure above still decides what
    # gets converted and published, but it is no longer load-bearing for
    # whether an install succeeds.
    ssh "$HOST" "pacman -Sy --noconfirm $names 2>&1 | tail -n 20"
else
    echo ":: copying to $HOST"
    ssh "$HOST" 'rm -rf /tmp/oryx-pkgs && mkdir -p /tmp/oryx-pkgs'
    scp -q "$STAGE"/*.pkg.tar.zst "$HOST":/tmp/oryx-pkgs/

    echo ":: installing"
    ssh "$HOST" 'pacman -U --noconfirm --nodeps --nodeps /tmp/oryx-pkgs/*.pkg.tar.zst 2>&1 | tail -n 20; rm -rf /tmp/oryx-pkgs'
fi

echo ":: done. Verify a package actually delivered files:  pacman -Ql <pkg> | wc -l"
