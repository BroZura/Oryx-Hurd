#!/bin/bash
# deb2pkg-dell.sh <file.deb> [outdir]
#
# Convert a Debian hurd-i386 .deb into a pacman .pkg.tar.zst ON THE DELL.
#
# Same logic as the T400's packages/deb2pkg.sh, with two changes:
#   - GNU tar + zstd instead of bsdtar (Parrot has no libarchive-tools);
#     pacman reads this fine, it uses libarchive on the reading side.
#   - no /srv paths.
#
# Why here and not on the T400: repacking a .deb is architecture-independent,
# and the Dell has the internet, a fast CPU and dpkg-deb. Doing it here means
# the Arch Hurd target no longer needs a Debian GNU/Hurd boot to gain
# packages -- convert here, scp the .pkg.tar.zst over, `pacman -U` there.
#
# Maintainer scripts are dropped, exactly as on the T400 -- so remember that
# update-alternatives, glib-compile-schemas, fc-cache, mime/desktop/icon
# caches and system-user creation do NOT happen. See fix-alternatives.sh.

set -u
DEB="${1:-}"
OUT="${2:-$HOME/Desktop/hurd/pkgs}"
[ -f "$DEB" ] || { echo "usage: $0 <file.deb> [outdir]" >&2; exit 2; }
mkdir -p "$OUT"
# Absolute, because the tar below runs after `cd` into the work directory: a
# relative outdir would resolve against THAT and the package would be written
# into the temp tree and deleted with it, silently and with exit status 0.
OUT=$(cd "$OUT" && pwd)
DEB=$(readlink -f "$DEB")

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/root"

name=$(dpkg-deb -f "$DEB" Package)
dver=$(dpkg-deb -f "$DEB" Version)
darch=$(dpkg-deb -f "$DEB" Architecture)
desc=$(dpkg-deb -f "$DEB" Description | head -1)
[ -n "$name" ] || { echo "no Package field in $DEB" >&2; exit 1; }

dpkg-deb -x "$DEB" "$WORK/root" || { echo "extract failed: $DEB" >&2; exit 1; }

# usr-merge: the target has /bin -> usr/bin, and pacman refuses to write a
# real directory over a symlink.
for d in bin sbin lib lib64; do
    src="$WORK/root/$d"
    [ -d "$src" ] && [ ! -L "$src" ] || continue
    mkdir -p "$WORK/root/usr/$d"
    cp -a "$src/." "$WORK/root/usr/$d/" || { echo "usr-merge failed: $d" >&2; exit 1; }
    rm -rf "$src"
    echo "  usr-merged /$d -> /usr/$d" >&2
done

pkgver=$(printf '%s' "$dver" | sed -e 's/:/./g' -e 's/-/./g' -e 's/+/./g' -e 's/~/./g')
pkgrel=1
case "$darch" in all) arch=any ;; *) arch=i686 ;; esac
size=$(du -sb "$WORK/root" 2>/dev/null | cut -f1); [ -z "$size" ] && size=0

# Reproducibility: builddate comes from the .deb, never from `date`.
#
# Converting the same .deb twice must produce the same bytes. If it does not,
# a re-convert yields a different sha256 under the SAME filename and version,
# and every stale copy -- the target's /var/cache/pacman/pkg above all --
# then fails `pacman`'s integrity check with "invalid or corrupted package".
# That is a genuinely confusing failure, because the file is not corrupt.
#
# The ar header of the first member holds the build time Debian stamped in:
# 8-byte global magic, then a 60-byte header whose mtime is 12 decimal bytes
# at offset 16.
builddate=$(python3 -c '
import sys
with open(sys.argv[1], "rb") as fh:
    fh.seek(8 + 16)
    try:
        print(int(fh.read(12).split()[0]))
    except (ValueError, IndexError):
        print(0)
' "$DEB" 2>/dev/null) || builddate=0
# Fall back to the file mtime rather than `date`: still stable for a given
# downloaded .deb, where now() is stable for nothing.
[ "${builddate:-0}" -gt 0 ] 2>/dev/null || builddate=$(stat -c %Y "$DEB")

{
    echo "pkgname = $name"
    echo "pkgbase = $name"
    echo "pkgver = $pkgver-$pkgrel"
    echo "pkgdesc = $desc"
    echo "url = https://packages.debian.org/$name"
    echo "builddate = $builddate"
    echo "packager = deb2pkg-dell (repackaged from Debian hurd-i386)"
    echo "size = $size"
    echo "arch = $arch"
    echo "license = custom:see-debian-copyright"
    # NO `depend` lines here, deliberately. Dependencies are written into the
    # repository database instead, by oryx-repo-add.py --depends-from.
    #
    # This was tried the other way and it is wrong twice over.
    #
    # Choosing correctly needs to know what the repository holds, which a
    # converter looking at one .deb cannot. Debian's tmux depends on
    #   systemd | systemd-standalone-tmpfiles | systemd-tmpfiles
    # and taking the first alternative -- what apt does -- yields `systemd`
    # on a system that will never have one. The indexer picks the
    # alternative that something in the repository actually provides
    # (seedfiles, here), which is the answer that works.
    #
    # And because the indexer drops dependencies nothing can satisfy, a
    # package declaring its own would then disagree with the database entry
    # for it, which pacman reports as
    #   "File ... is corrupted (invalid or corrupted package)"
    # -- a thoroughly misleading message for metadata that merely disagrees.
    #
    # The cost is that `pacman -U` on a bare file sees no dependencies. That
    # is the price of a single source of truth, and it is the cheaper side.
} > "$WORK/root/.PKGINFO"

# Member paths must be "usr/bin/foo", NOT "./usr/bin/foo": pacman accepts an
# archive of ./-prefixed entries without complaint and then installs NOTHING
# -- the package registers in the database with an empty file list. Hence the
# explicit top-level list rather than a plain "tar -cf - .".
# .PKGINFO must come first in the archive.
( cd "$WORK/root" && { printf '.PKGINFO\0'
                       find . -mindepth 1 -maxdepth 1 ! -name .PKGINFO -printf '%P\0' \
                           | sort -z
                     } | tar --format=gnu --null -T - -cf - --sort=name \
                           --mtime="@$builddate" --owner=0 --group=0 \
                     | zstd -q -f -o "$OUT/${name}-${pkgver}-${pkgrel}-${arch}.pkg.tar.zst" )

echo "${name}-${pkgver}-${pkgrel}-${arch}.pkg.tar.zst"
