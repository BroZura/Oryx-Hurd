# oryx-get's shared converter core.
#
# Ported from bin/deb2pkg-dell.sh (Debian .deb -> pacman) and
# bin/oryx-wheel2pkg.sh (PyPI wheel -> pacman), which both call
# bin/oryx-mkpkg.sh for the actual packaging step. This module is that same
# packaging step plus both extraction paths, in one place, so a future
# server-side tool can import it unchanged -- "don't fork it" per the
# original request.
#
# DELIBERATE DIFFERENCE from deb2pkg-dell.sh: that script omits `depend =`
# lines on purpose, because packages headed for the [oryx] REPOSITORY must
# match the repo database's own dependency metadata exactly, or pacman -S
# refuses them as "invalid or corrupted package" (FIXES.md 13). oryx-get's
# conversions are installed straight off disk with `pacman -U`, which never
# consults a sync database (see the same FIXES.md entry: "-U on that very
# same file installs it without complaint"), so there is no database to
# disagree with -- real `depend =` lines belong in .PKGINFO here, matching
# oryx-mkpkg.sh's convention for Oryx's own packages, not deb2pkg-dell.sh's.
# --export (oryx_get/export.py) reuses the exact same dependency list for
# repo-add, so the two can never disagree either.

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
from dataclasses import dataclass, field
from pathlib import Path


class ConvertError(RuntimeError):
    pass


def _run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)


def mangle_version(deb_version: str) -> str:
    """Debian version -> pacman-safe pkgver. Same mangling as deb2pkg-dell.sh:
    ':', '-', '+', '~' all become '.'. Ordering is not preserved -- pacman
    version comparison on a mangled string is not meaningful, which is why
    oryx-setdeps.py drops version CONSTRAINTS entirely elsewhere. Fine here:
    oryx-get installs a specific resolved version, it does not compare them.
    """
    out = deb_version
    for ch in ":-+~":
        out = out.replace(ch, ".")
    return out


def read_ar_mtime(deb_path: Path) -> int:
    """The .deb's own build time, from its ar archive header -- NOT date.now().
    Converting the same .deb twice must produce identical bytes, or a stale
    cached copy on the target fails its checksum ("invalid or corrupted
    package (checksum)", FIXES.md 13). 8-byte global magic, then a 60-byte
    per-member header whose mtime is 12 decimal bytes at offset 16.
    """
    try:
        with open(deb_path, "rb") as fh:
            fh.seek(8 + 16)
            raw = fh.read(12).split()[0]
            val = int(raw)
            if val > 0:
                return val
    except (OSError, ValueError, IndexError):
        pass
    return int(deb_path.stat().st_mtime)


def normalize_dir_modes(root: Path) -> None:
    """pacman warns 'directory permissions differ on /usr/' for every
    directory not at 755 -- staging trees made under a normal umask are 775.
    """
    for dirpath, dirnames, _ in os.walk(root):
        os.chmod(dirpath, 0o755)


def usr_merge(root: Path) -> list[str]:
    """The target has /bin -> usr/bin etc; pacman refuses to write a real
    directory over a symlink. Move real top-level bin/sbin/lib/lib64 into
    usr/. Returns what was merged, for logging.
    """
    merged = []
    for name in ("bin", "sbin", "lib", "lib64"):
        src = root / name
        if src.is_dir() and not src.is_symlink():
            dst = root / "usr" / name
            dst.mkdir(parents=True, exist_ok=True)
            for item in src.iterdir():
                target = dst / item.name
                if target.exists():
                    if target.is_dir() and item.is_dir():
                        shutil.copytree(item, target, dirs_exist_ok=True)
                        continue
                    target.unlink()
                shutil.move(str(item), str(target))
            shutil.rmtree(src)
            merged.append(name)
    return merged


@dataclass
class PkgMeta:
    name: str
    version: str          # already pacman-mangled
    arch: str              # "any" or "i686"
    desc: str = ""
    depends: list[str] = field(default_factory=list)
    builddate: int = 0
    url: str = ""
    packager: str = "oryx-get"
    license: str = "custom"
    source: str = ""       # provenance: "debian:<pkg>=<version>" / "pypi:<name>==<ver>", + hash
    pkgrel: str = "1"


def write_pkginfo(stage: Path, meta: PkgMeta) -> None:
    size = sum(f.stat().st_size for f in stage.rglob("*") if f.is_file())
    lines = [
        f"pkgname = {meta.name}",
        f"pkgbase = {meta.name}",
        f"pkgver = {meta.version}-{meta.pkgrel}",
        f"pkgdesc = {meta.desc or meta.name}",
        f"url = {meta.url or f'https://oryx.invalid/{meta.name}'}",
        f"builddate = {meta.builddate}",
        f"packager = {meta.packager}",
        f"size = {size}",
        f"arch = {meta.arch}",
        f"license = {meta.license}",
    ]
    for d in meta.depends:
        lines.append(f"depend = {d}")
    if meta.source:
        # Not a real pacman field -- an oryx-get-specific extension line so
        # a package's provenance (what it was converted FROM, and its
        # original hash) can be re-verified later. pacman ignores unknown
        # keys in .PKGINFO; `oryx-get --info` reads this one back out.
        lines.append(f"# oryx-get-source = {meta.source}")
    (stage / ".PKGINFO").write_text("\n".join(lines) + "\n")


def build_pkg_tarball(stage: Path, meta: PkgMeta, outdir: Path) -> Path:
    """The exact packaging step oryx-mkpkg.sh performs: .PKGINFO first,
    member paths WITHOUT a "./" prefix (pacman silently installs a
    ./-prefixed archive as empty -- FIXES.md 8/15), reproducible tar
    (--sort=name, fixed mtime/owner/group).
    """
    outdir.mkdir(parents=True, exist_ok=True)
    normalize_dir_modes(stage)
    write_pkginfo(stage, meta)
    filename = f"{meta.name}-{meta.version}-{meta.pkgrel}-{meta.arch}.pkg.tar.zst"
    out_path = outdir / filename

    members = sorted(
        p.relative_to(stage).as_posix()
        for p in stage.iterdir()
        if p.name != ".PKGINFO"
    )
    file_list = "\0".join([".PKGINFO"] + members) + "\0"

    tar = subprocess.Popen(
        ["tar", "--format=gnu", "--null", "-T", "-", "-cf", "-",
         "--sort=name", f"--mtime=@{meta.builddate}", "--owner=0", "--group=0"],
        cwd=stage, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    )
    zstd = subprocess.Popen(
        ["zstd", "-q", "-f", "-o", str(out_path)],
        stdin=tar.stdout,
    )
    tar.stdout.close()
    tar.communicate(input=file_list.encode())
    zstd.wait()
    (stage / ".PKGINFO").unlink(missing_ok=True)
    if tar.returncode != 0 or zstd.returncode != 0:
        raise ConvertError(f"packaging failed for {meta.name} (tar={tar.returncode}, zstd={zstd.returncode})")
    return out_path


# --------------------------------------------------------------- .deb path --

def deb_fields(deb_path: Path) -> dict[str, str]:
    out = _run(["dpkg-deb", "-f", str(deb_path)]).stdout
    fields: dict[str, str] = {}
    cur = None
    for line in out.splitlines():
        if line and line[0] not in " \t" and ":" in line:
            cur, val = line.split(":", 1)
            fields[cur.strip()] = val.strip()
        elif cur and line.startswith((" ", "\t")):
            fields[cur] += "\n" + line
    return fields


def convert_deb(deb_path: Path, outdir: Path, depends: list[str], source: str) -> Path:
    """Convert one Debian hurd-i386 .deb into a pacman package with REAL
    dependencies (see the module docstring for why that differs from
    deb2pkg-dell.sh). `depends` is supplied by the caller (the resolver has
    already decided which alternative of each `Depends:` OR-group applies --
    see oryx_get/debian_source.py), not recomputed here.
    """
    fields = deb_fields(deb_path)
    name = fields.get("Package")
    if not name:
        raise ConvertError(f"{deb_path}: no Package field")
    darch = fields.get("Architecture", "hurd-i386")
    arch = "any" if darch == "all" else "i686"

    with tempfile.TemporaryDirectory(prefix="oryx-get-deb-") as tmp:
        stage = Path(tmp) / "root"
        stage.mkdir()
        _run(["dpkg-deb", "-x", str(deb_path), str(stage)])
        usr_merge(stage)
        meta = PkgMeta(
            name=name,
            version=mangle_version(fields.get("Version", "0")),
            arch=arch,
            desc=(fields.get("Description", "") or "").splitlines()[0] if fields.get("Description") else "",
            depends=depends,
            builddate=read_ar_mtime(deb_path),
            url=f"https://packages.debian.org/{name}",
            packager="oryx-get (converted from Debian hurd-i386)",
            license="custom:see-debian-copyright",
            source=source,
        )
        return build_pkg_tarball(stage, meta, outdir)


# ------------------------------------------------------------- wheel path --

def wheel_name_version(wheel_path: Path) -> tuple[str, str]:
    base = wheel_path.stem  # <dist>-<version>-<pyver>-<abi>-<platform>
    parts = base.split("-")
    return parts[0], parts[1]


def convert_wheel(wheel_path: Path, outdir: Path, depends: list[str], source: str,
                   name: str | None = None) -> Path:
    """Convert a pure-Python (py3-none-any) PyPI wheel. Same trick as
    oryx-wheel2pkg.sh: files land in /usr/lib/python3/dist-packages (Debian's
    layout, already on the target's sys.path -- NOT sysconfig's purelib).
    Console scripts are generated from entry_points.txt by hand, since
    nothing runs pip on the target to do it at install time.
    """
    import configparser
    import zipfile

    dist, version = wheel_name_version(wheel_path)
    pkgname = name or f"python3-{dist.lower().replace('_', '-')}"

    with tempfile.TemporaryDirectory(prefix="oryx-get-wheel-") as tmp:
        stage = Path(tmp) / "root"
        site = stage / "usr" / "lib" / "python3" / "dist-packages"
        binfile = stage / "usr" / "bin"
        site.mkdir(parents=True)

        with zipfile.ZipFile(wheel_path) as zf:
            zf.extractall(site)

        for data_dir in site.glob("*.data"):
            scripts = data_dir / "scripts"
            if scripts.is_dir():
                binfile.mkdir(parents=True, exist_ok=True)
                for f in scripts.iterdir():
                    shutil.move(str(f), str(binfile / f.name))
            shutil.rmtree(data_dir)

        for ep_file in site.glob("*.dist-info/entry_points.txt"):
            cp = configparser.ConfigParser()
            cp.optionxform = str
            cp.read(ep_file)
            if not cp.has_section("console_scripts"):
                continue
            binfile.mkdir(parents=True, exist_ok=True)
            for scriptname, target in cp.items("console_scripts"):
                mod, _, attr = target.partition(":")
                attr = (attr.strip() or "main").split(".")[0]
                path = binfile / scriptname
                path.write_text(
                    "#!/usr/bin/python3\n"
                    "# Generated by oryx-get (pip does not run on the target).\n"
                    "import re, sys\n"
                    f"from {mod} import {attr}\n"
                    "if __name__ == '__main__':\n"
                    "    sys.argv[0] = re.sub(r'(-script\\.pyw|\\.exe)?$', '', sys.argv[0])\n"
                    f"    sys.exit({attr}())\n"
                )
                path.chmod(0o755)

        for pycache in site.rglob("__pycache__"):
            shutil.rmtree(pycache, ignore_errors=True)

        meta = PkgMeta(
            name=pkgname,
            version=mangle_version(version),
            arch="any",
            desc=f"{dist} {version} (PyPI wheel, packaged by oryx-get)",
            depends=depends,
            builddate=int(wheel_path.stat().st_mtime),
            packager="oryx-get (PyPI wheel)",
            source=source,
        )
        return build_pkg_tarball(stage, meta, outdir)
