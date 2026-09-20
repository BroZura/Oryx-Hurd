#!/usr/bin/env python3
"""Write resolved dependencies into the packages in a repository.

pacman requires a package's own .PKGINFO and the repository database entry
for it to agree. When they differ it rejects the package during `pacman -S`
with

    File ... is corrupted (invalid or corrupted package)

and only `--debug` reveals the real reason: "internal package depends
mismatch". The file is not corrupt at all, and `pacman -U` on the very same
file installs it happily, which makes this a genuinely misleading failure to
chase. Hence one tool, run before indexing: it puts the dependencies into the
packages, and oryx-repo-add.py then copies them out of the packages into the
database, so the two cannot disagree.

Why this cannot be done by the converter, one .deb at a time:

    Debian's tmux depends on
        systemd | systemd-standalone-tmpfiles | systemd-tmpfiles
    and taking the first alternative -- what apt does -- gives `systemd` on a
    system that will never have one. Choosing correctly means knowing what
    the repository holds, which is only knowable here, with every package in
    front of us. The right answer is systemd-tmpfiles, which seedfiles
    provides.

Version constraints are dropped: deb2pkg mangles Debian versions into pacman
ones (`1:2.4-3~bpo` -> `1.2.4.3.bpo-1`) in a way that does not preserve
ordering, so a constraint cannot be evaluated correctly and a wrong answer is
worse than none. The repository is a coherent snapshot of one archive state,
so unversioned dependencies resolve to the right packages regardless.

Usage:
    oryx-setdeps.py REPODIR --index PACKAGES [-n]
"""

import argparse
import io
import os
import subprocess
import sys
import tarfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import debindex  # noqa: E402


def read_pkg(path):
    """Return (list of (TarInfo, bytes|None), pkginfo text)."""
    raw = subprocess.run(["zstd", "-dc", path], check=True,
                         stdout=subprocess.PIPE).stdout
    members, pkginfo = [], None
    with tarfile.open(fileobj=io.BytesIO(raw), mode="r:") as tf:
        for m in tf:
            data = tf.extractfile(m).read() if m.isreg() else None
            if m.name == ".PKGINFO":
                pkginfo = data.decode("utf-8", "replace")
            members.append((m, data))
    return members, pkginfo


def mangle_version(dver):
    """Debian version -> the pacman version deb2pkg produces from it.

    Must stay in step with deb2pkg-dell.sh: `:`, `-`, `+` and `~` all become
    `.`, and pkgrel is always 1.
    """
    if not dver:
        return None
    for ch in ":-+~":
        dver = dver.replace(ch, ".")
    return f"{dver}-1"


def pkginfo_value(text, key):
    for line in text.splitlines():
        k, _, v = line.partition("=")
        if k.strip() == key:
            return v.strip()
    return None


def pkginfo_values(text, key):
    out = []
    for line in text.splitlines():
        k, _, v = line.partition("=")
        if k.strip() == key and v.strip():
            out.append(v.strip())
    return out


def set_relations(text, deps, provides):
    """Replace depend/provides lines, keeping every other line in order."""
    kept = [ln for ln in text.splitlines()
            if ln.partition("=")[0].strip() not in ("depend", "provides")]
    kept += [f"depend = {d}" for d in deps]
    kept += [f"provides = {p}" for p in provides]
    return "\n".join(kept) + "\n"


def write_pkg(path, members, pkginfo):
    """Rewrite the package with a new .PKGINFO, preserving everything else.

    Deterministic: same input, same bytes out, so re-running does not churn
    checksums and invalidate every cached copy on the target.
    """
    buf = io.BytesIO()
    data = pkginfo.encode()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.GNU_FORMAT) as tf:
        for m, payload in members:
            if m.name == ".PKGINFO":
                m = tarfile.TarInfo(".PKGINFO")
                m.size, m.mtime, m.mode, m.uid, m.gid = len(data), 0, 0o644, 0, 0
                tf.addfile(m, io.BytesIO(data))
            elif payload is None:
                tf.addfile(m)
            else:
                tf.addfile(m, io.BytesIO(payload))

    tmp = path + ".new"
    with open(tmp, "wb") as fh:
        subprocess.run(["zstd", "-q", "-f", "-"], input=buf.getvalue(),
                       stdout=fh, check=True)
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("repodir")
    ap.add_argument("--index", required=True, metavar="PACKAGES",
                    help="Debian Packages index to take relationships from")
    ap.add_argument("-n", "--dry-run", action="store_true")
    args = ap.parse_args()

    repodir = os.path.abspath(args.repodir)
    files = sorted(f for f in os.listdir(repodir) if f.endswith(".pkg.tar.zst"))
    if not files:
        sys.exit(f"error: no packages in {repodir}")

    excluded = set()
    exclude_path = os.path.join(repodir, "oryx-exclude.txt")
    if os.path.exists(exclude_path):
        with open(exclude_path) as fh:
            for line in fh:
                line = line.split("#")[0].strip()
                if line:
                    excluded.add(line)

    print(f":: reading {len(files)} packages")
    pkgs = []
    for fn in files:
        path = os.path.join(repodir, fn)
        members, text = read_pkg(path)
        if text is None:
            print(f"   !! {fn}: no .PKGINFO, skipped")
            continue
        name = pkginfo_value(text, "pkgname")
        version = pkginfo_value(text, "pkgver")
        if not name:
            continue
        # Version-qualified too: a local rebuild and the archive's build of
        # the same package can both be present, and only one is excluded.
        if name in excluded or f"{name}={version}" in excluded:
            continue
        pkgs.append((name, version, path, members, text))

    index = debindex.Index(args.index)

    # Provides first: a dependency may name a virtual package that something
    # else in this repository supplies, and that has to be known before any
    # dependency can be resolved against it.
    provides = {name: index.provides_of(name) for name, *_ in pkgs}

    avail = {name for name, *_ in pkgs}
    for virt in provides.values():
        avail.update(virt)

    changed = unchanged = unknown = local = 0
    dropped = {}
    for name, version, path, members, text in sorted(pkgs):
        if name not in index.pkgs:
            unknown += 1
            continue

        # A package whose version does not match the archive's is a local
        # rebuild, and the index describes a DIFFERENT build of it. Copying
        # those dependencies in would be actively wrong: the `+hurd.1` procps
        # links libproc2-0, while the archive's build of the same name wants
        # libproc2-1. Leave local rebuilds exactly as they are.
        if version != mangle_version(index.pkgs[name].get("Version", "")):
            local += 1
            continue

        deps, missing = index.depends(name, avail)
        for m in missing:
            dropped.setdefault(m, []).append(name)

        if (pkginfo_values(text, "depend") == deps
                and pkginfo_values(text, "provides") == provides[name]):
            unchanged += 1
            continue

        changed += 1
        if args.dry_run:
            print(f"   would update {name}: {len(deps)} deps")
            continue
        write_pkg(path, members, set_relations(text, deps, provides[name]))

    print(f"   {changed} updated, {unchanged} already correct, "
          f"{unknown} not in the index, {local} local rebuilds left alone")
    if dropped:
        print(f"\n:: {len(dropped)} dependency names nothing here provides:")
        for name in sorted(dropped):
            users = sorted(dropped[name])
            shown = ", ".join(users[:3]) + ("..." if len(users) > 3 else "")
            print(f"     {name}  (wanted by {shown})")
        print("   Left out of the packages, so the database matches them.")
        print("   To honour one instead:  oryx-install.sh -P <name>")


if __name__ == "__main__":
    main()
