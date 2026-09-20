#!/usr/bin/env python3
"""Build an Arch-format package repository database from .pkg.tar.zst files.

A standalone replacement for pacman's `repo-add`, so that the Dell does not
need pacman installed in order to publish packages for Oryx Hurd.

The on-disk format is libalpm's, unchanged:

    <repo>.db.tar.gz        one <pkgname>-<pkgver>/desc entry per package
    <repo>.files.tar.gz     the same, plus a <pkgname>-<pkgver>/files listing
    <repo>.db               symlink -> <repo>.db.tar.gz
    <repo>.files            symlink -> <repo>.files.tar.gz

Usage:
    oryx-repo-add.py REPODIR [-n NAME] [PKG ...]

With no PKG arguments every *.pkg.tar.zst in REPODIR is indexed, which is the
normal way to use it: drop packages in, re-run, done. Passing packages copies
them into REPODIR first.

Dependencies come from each package's own .PKGINFO and from nowhere else.
This is deliberate and not negotiable: pacman compares the database entry
against the package file and rejects any package whose dependencies differ,
reporting it as "invalid or corrupted package" -- which is a thoroughly
misleading way to describe metadata that merely disagrees. So the database
must mirror the packages exactly, and any dependency work happens before
this runs, in oryx-setdeps.py.
"""

import argparse
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
import time

# .PKGINFO key -> desc section. Keys that may appear more than once map to a
# section holding one value per line.
SINGLE = {
    "pkgname": "NAME",
    "pkgbase": "BASE",
    "pkgver": "VERSION",
    "pkgdesc": "DESC",
    "url": "URL",
    "builddate": "BUILDDATE",
    "packager": "PACKAGER",
    "arch": "ARCH",
}
MULTI = {
    "group": "GROUPS",
    "license": "LICENSE",
    "replaces": "REPLACES",
    "conflict": "CONFLICTS",
    "provides": "PROVIDES",
    "depend": "DEPENDS",
    "optdepend": "OPTDEPENDS",
    "makedepend": "MAKEDEPENDS",
    "checkdepend": "CHECKDEPENDS",
}
# The order libalpm's own repo-add writes them in. Not required, but it keeps
# diffs against a pacman-generated database readable.
ORDER = [
    "FILENAME", "NAME", "BASE", "VERSION", "DESC", "GROUPS", "CSIZE", "ISIZE",
    "MD5SUM", "SHA256SUM", "PGPSIG", "URL", "LICENSE", "ARCH", "BUILDDATE",
    "PACKAGER", "REPLACES", "CONFLICTS", "PROVIDES", "DEPENDS", "OPTDEPENDS",
    "MAKEDEPENDS", "CHECKDEPENDS",
]


def read_package(path):
    """Return (.PKGINFO text, sorted file list) from a .pkg.tar.zst.

    Python 3.13 has no built-in zstd, so decompression goes through the zstd
    binary. The whole archive is decompressed once and read from memory; these
    packages are Debian binaries, tens of megabytes at the very worst.
    """
    try:
        raw = subprocess.run(
            ["zstd", "-dc", path], check=True, stdout=subprocess.PIPE
        ).stdout
    except FileNotFoundError:
        sys.exit("error: the `zstd` binary is required but was not found")
    except subprocess.CalledProcessError:
        sys.exit(f"error: {os.path.basename(path)}: not a valid zstd archive")

    pkginfo, files = None, []
    with tarfile.open(fileobj=io.BytesIO(raw), mode="r:") as tf:
        for m in tf:
            if m.name == ".PKGINFO":
                pkginfo = tf.extractfile(m).read().decode("utf-8", "replace")
            elif m.name.startswith("."):
                continue  # .MTREE, .INSTALL and friends are not listed
            else:
                files.append(m.name + ("/" if m.isdir() else ""))
    if pkginfo is None:
        sys.exit(f"error: {os.path.basename(path)}: no .PKGINFO member")
    return pkginfo, sorted(files)


def parse_pkginfo(text):
    """.PKGINFO -> {section: [values]}, discarding comments and unknown keys."""
    out = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, val = line.partition("=")
        key, val = key.strip(), val.strip()
        if not val:
            continue
        if key in SINGLE:
            out[SINGLE[key]] = [val]
        elif key in MULTI:
            out.setdefault(MULTI[key], []).append(val)
        elif key == "size":
            out["ISIZE"] = [val]
    return out


def hashes(path):
    """md5 and sha256 of the package file, in a single pass."""
    md5, sha = hashlib.md5(), hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            md5.update(chunk)
            sha.update(chunk)
    return md5.hexdigest(), sha.hexdigest()


def format_sections(sections, order):
    """Render {section: [values]} in libalpm's `%KEY%\\nvalue\\n\\n` layout."""
    parts = []
    for key in order:
        vals = sections.get(key)
        if vals:
            parts.append("%{}%\n{}\n".format(key, "\n".join(vals)))
    return "\n".join(parts) + "\n"


def describe(path):
    """Build the desc and files section-maps for one package file."""
    pkginfo, files = read_package(path)
    sections = parse_pkginfo(pkginfo)

    name = sections.get("NAME", [None])[0]
    version = sections.get("VERSION", [None])[0]
    if not name or not version:
        sys.exit(f"error: {os.path.basename(path)}: .PKGINFO lacks pkgname/pkgver")

    md5, sha = hashes(path)
    sections["FILENAME"] = [os.path.basename(path)]
    sections["CSIZE"] = [str(os.path.getsize(path))]
    sections["MD5SUM"] = [md5]
    sections["SHA256SUM"] = [sha]

    return f"{name}-{version}", sections, files


def add_entry(tf, path, text, mtime):
    data = text.encode()
    info = tarfile.TarInfo(path)
    info.size = len(data)
    info.mtime = mtime
    info.mode = 0o644
    tf.addfile(info, io.BytesIO(data))


def add_dir(tf, path, mtime):
    info = tarfile.TarInfo(path)
    info.type = tarfile.DIRTYPE
    info.mode = 0o755
    info.mtime = mtime
    tf.addfile(info)


def write_db(repodir, name, entries, kind):
    """Write <name>.<kind>.tar.gz plus its bare <name>.<kind> symlink."""
    archive = os.path.join(repodir, f"{name}.{kind}.tar.gz")
    tmp = archive + ".new"
    mtime = int(time.time())
    with tarfile.open(tmp, "w:gz") as tf:
        for key, sections, files in entries:
            add_dir(tf, key, mtime)
            add_entry(tf, f"{key}/desc", format_sections(sections, ORDER), mtime)
            if kind == "files":
                add_entry(tf, f"{key}/files",
                          "%FILES%\n" + "".join(f + "\n" for f in files), mtime)
    os.replace(tmp, archive)

    link = os.path.join(repodir, f"{name}.{kind}")
    if os.path.islink(link) or os.path.exists(link):
        os.remove(link)
    os.symlink(os.path.basename(archive), link)
    return archive


def load_exclusions(repodir, extra):
    """Packages that must never enter the repository.

    Kept in a file beside the packages rather than in anyone's memory: the
    reason a package is excluded (see procps in FIXES.md 13) long outlives
    whoever deleted the file by hand, and re-running the converter over a
    directory of .debs silently puts it straight back.

    Two forms, because excluding by name alone is too blunt once a local
    rebuild and the archive's build of the SAME package both exist:

        procps              every version
        procps=2.4.0.7.1-1  that version only

    The second is what keeps stock procps out while the working `+hurd.1`
    rebuild of the same name stays in.
    """
    rules = set(extra or [])
    path = os.path.join(repodir, "oryx-exclude.txt")
    if os.path.exists(path):
        with open(path) as fh:
            for line in fh:
                line = line.split("#")[0].strip()
                if line:
                    rules.add(line)
    return rules


def is_excluded(name, version, rules):
    return name in rules or f"{name}={version}" in rules


def main():
    ap = argparse.ArgumentParser(
        description="Build an Arch repository database without pacman.")
    ap.add_argument("repodir", help="directory holding the packages")
    ap.add_argument("packages", nargs="*",
                    help="packages to copy in first (default: index what is there)")
    ap.add_argument("-n", "--name", default="oryx", help="repository name")
    ap.add_argument("--exclude", action="append", metavar="NAME",
                    help="never index this package (adds to oryx-exclude.txt)")
    args = ap.parse_args()

    repodir = os.path.abspath(args.repodir)
    os.makedirs(repodir, exist_ok=True)

    for src in args.packages:
        if not os.path.isfile(src):
            sys.exit(f"error: no such package: {src}")
        dst = os.path.join(repodir, os.path.basename(src))
        if os.path.abspath(src) != dst:
            shutil.copy2(src, dst)
            print(f"  + {os.path.basename(src)}")

    found = sorted(f for f in os.listdir(repodir) if f.endswith(".pkg.tar.zst"))
    if not found:
        sys.exit(f"error: no .pkg.tar.zst files in {repodir}")

    # Keep only the newest file per package name. Two versions of the same
    # package in one directory is normal after a rebuild; libalpm's database
    # holds one entry per name, so picking is not optional.
    excluded = load_exclusions(repodir, args.exclude)
    best = {}
    skipped = []
    for fn in found:
        key, sections, files = describe(os.path.join(repodir, fn))
        pkgname = sections["NAME"][0]
        if is_excluded(pkgname, sections["VERSION"][0], excluded):
            skipped.append(fn)
            continue
        prev = best.get(pkgname)
        if prev and int(prev[1]["BUILDDATE"][0]) > int(sections["BUILDDATE"][0]):
            print(f"  - {fn} (superseded by {prev[1]['FILENAME'][0]})")
            continue
        if prev:
            print(f"  - {prev[1]['FILENAME'][0]} (superseded by {fn})")
        best[pkgname] = (key, sections, files)
        print(f"  > {key}")

    entries = [best[k] for k in sorted(best)]

    if skipped:
        print(f"\n:: {len(skipped)} excluded by oryx-exclude.txt, not indexed")
        for fn in skipped:
            print(f"     {fn}")

    db = write_db(repodir, args.name, entries, "db")
    fdb = write_db(repodir, args.name, entries, "files")

    print(f"\n{len(entries)} packages indexed into repository '{args.name}'")
    for p in (db, fdb):
        print(f"  {p}  ({os.path.getsize(p):,} bytes)")


if __name__ == "__main__":
    main()
