#!/bin/bash
# installed2pkg.sh <installed-dpkg-package> [outdir]
#
# Build a pacman package from a package ALREADY INSTALLED on this Debian host,
# using dpkg's own file list.
#
# Why this exists alongside deb2pkg.sh: some hurd-i386 packages on this machine
# are local Hurd rebuilds (version suffix "+hurd.1", e.g. procps and
# libproc2-0) whose .deb is no longer downloadable from debian-ports. The sid
# replacement is not a drop-in -- procps 4.0.7 wants libproc2.so.1 while the
# working Hurd build provides libproc2.so.0. The binaries on disk are the ones
# known to run here, so package those.
#
# Same conventions as deb2pkg.sh: version flattened for pacman, pkgrel pinned,
# maintainer scripts deliberately dropped, payload usr-merged.

set -u
name="${1:-}"
OUT="${2:-/srv/archrepo}"
[ -n "$name" ] || { echo "usage: $0 <installed-dpkg-package> [outdir]" >&2; exit 2; }

status=$(dpkg-query -W -f='${Status}' "$name" 2>/dev/null) || true
case "$status" in *" installed") ;; *) echo "$name is not installed" >&2; exit 1 ;; esac

dver=$(dpkg-query -W -f='${Version}' "$name")
darch=$(dpkg-query -W -f='${Architecture}' "$name")
desc=$(dpkg-query -W -f='${binary:Summary}' "$name")
mkdir -p "$OUT"

WORK=$(mktemp -d /srv/archbuild/installed2pkg.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/root"

# Copy every regular file and symlink dpkg attributes to this package.
# Directories are skipped: pacman creates them, and claiming shared dirs
# such as /usr/lib would collide with other packages.
while IFS= read -r p; do
    [ -e "$p" ] || [ -L "$p" ] || continue
    [ -d "$p" ] && [ ! -L "$p" ] && continue
    mkdir -p "$WORK/root$(dirname "$p")"
    cp -a "$p" "$WORK/root$p"
done < <(dpkg -L "$name")

for d in bin sbin lib lib64; do
    src="$WORK/root/$d"
    [ -d "$src" ] && [ ! -L "$src" ] || continue
    mkdir -p "$WORK/root/usr/$d"
    cp -a "$src/." "$WORK/root/usr/$d/" || { echo "usr-merge failed: $d" >&2; exit 1; }
    rm -rf "$src"
    echo "usr-merged /$d -> /usr/$d" >&2
done

pkgver=$(printf '%s' "$dver" | sed -e 's/:/./g' -e 's/-/./g' -e 's/+/./g' -e 's/~/./g')
pkgrel=1
case "$darch" in all) arch=any ;; *) arch=i686 ;; esac
size=$(du -sb "$WORK/root" 2>/dev/null | cut -f1); [ -z "$size" ] && size=0

{
    echo "pkgname = $name"
    echo "pkgbase = $name"
    echo "pkgver = $pkgver-$pkgrel"
    echo "pkgdesc = $desc"
    echo "url = https://packages.debian.org/$name"
    echo "builddate = $(date +%s)"
    echo "packager = installed2pkg (repacked from this host's installed files)"
    echo "size = $size"
    echo "arch = $arch"
    echo "license = custom:see-debian-copyright"
} > "$WORK/root/.PKGINFO"

( cd "$WORK/root" && bsdtar -c --zstd -f "$OUT/${name}-${pkgver}-${pkgrel}-${arch}.pkg.tar.zst" .PKGINFO * ) 2>/dev/null \
  || ( cd "$WORK/root" && bsdtar -c --zstd -f "$OUT/${name}-${pkgver}-${pkgrel}-${arch}.pkg.tar.zst" . )

echo "${name}-${pkgver}-${pkgrel}-${arch}.pkg.tar.zst"
