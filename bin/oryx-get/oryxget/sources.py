# Pluggable package sources for oryx-get. Adding a new source TYPE means
# adding a class here and registering it in SOURCE_TYPES; adding a new
# source INSTANCE of an existing type means only editing /etc/oryx-get.conf.

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

from . import convert
from .config import SourceConfig


class SourceError(RuntimeError):
    pass


@dataclass
class Candidate:
    """What a source found for a requested package name."""
    source_name: str
    source_type: str
    pkgname: str          # the oryx/pacman package name this will become
    version: str
    already_in_repo: bool = False   # type=repo: pacman -S handles it directly
    # For conversion-needed sources (debian, pypi):
    fetch_fn: Optional[object] = None   # callable() -> Path (downloaded artifact)
    convert_fn: Optional[object] = None  # callable(artifact, outdir, depends, source) -> Path
    depends: list[str] | None = None
    unresolved_deps: list[str] | None = None  # declared but not auto-resolved (warn only)
    provenance: str = ""
    needs: list["Candidate"] = field(default_factory=list)  # deps, install BEFORE this one


class RepoSource:
    """type = repo. An existing prebuilt pacman repo, [oryx] today. If the
    name resolves there, no conversion is needed at all -- just `pacman -S`.
    """
    def __init__(self, cfg: SourceConfig):
        self.cfg = cfg

    def find(self, pkgname: str) -> Optional[Candidate]:
        try:
            out = subprocess.run(
                ["pacman", "-Si", pkgname],
                capture_output=True, text=True,
            )
        except FileNotFoundError:
            return None
        if out.returncode != 0:
            return None
        version = ""
        for line in out.stdout.splitlines():
            if line.startswith("Version"):
                version = line.split(":", 1)[1].strip()
                break
        return Candidate(
            source_name=self.cfg.name, source_type="repo",
            pkgname=pkgname, version=version, already_in_repo=True,
        )

    def search(self, term: str) -> list[str]:
        out = subprocess.run(["pacman", "-Ss", term], capture_output=True, text=True)
        if out.returncode != 0:
            return []
        return [l for l in out.stdout.splitlines() if l.startswith("oryx/")]

    def info(self, pkgname: str) -> Optional[dict]:
        out = subprocess.run(["pacman", "-Si", pkgname], capture_output=True, text=True)
        if out.returncode != 0:
            return None
        fields = {}
        for line in out.stdout.splitlines():
            if ":" in line and not line.startswith(" "):
                k, _, v = line.partition(":")
                fields[k.strip()] = v.strip()
        return {
            "name": pkgname, "version": fields.get("Version", "?"),
            "description": fields.get("Description", ""),
            "depends": fields.get("Depends On", "").split(), "source": "repo",
        }


class PyPISource:
    """type = pypi. Pure-Python (py3-none-any) wheels only -- see the
    comment in config.py's DEFAULT_CONF for why compiled extensions are out
    of scope (no cross toolchain for hurd-i386).
    """
    def __init__(self, cfg: SourceConfig):
        self.cfg = cfg
        self.base = cfg.options.get("url", "https://pypi.org").rstrip("/")

    def find(self, pkgname: str) -> Optional[Candidate]:
        url = f"{self.base}/pypi/{pkgname}/json"
        try:
            with urllib.request.urlopen(url, timeout=20) as resp:
                data = json.load(resp)
        except Exception as e:
            raise SourceError(f"PyPI lookup for {pkgname!r} failed: {e}")

        version = data["info"]["version"]
        releases = data["urls"]  # latest release's files
        wheel = None
        for f in releases:
            if f["packagetype"] == "bdist_wheel" and f["filename"].endswith("-py3-none-any.whl"):
                wheel = f
                break
        if wheel is None:
            raise SourceError(
                f"{pkgname} {version}: no py3-none-any wheel available -- "
                "needs a compiled extension, which oryx-get cannot build "
                "for hurd-i386 (no cross toolchain). Refusing rather than "
                "installing something that will not import."
            )

        sha256 = wheel["digests"]["sha256"]
        download_url = wheel["url"]
        filename = wheel["filename"]

        # Non-stdlib runtime deps this wheel declares -- Stage A does not
        # resolve these transitively (deliberately out of scope for now,
        # see FIXES.md). Warn rather than silently produce a broken install.
        requires = data["info"].get("requires_dist") or []
        extra_deps = [r for r in requires if "extra ==" not in r]

        def _fetch() -> Path:
            cache = Path("/var/cache/oryx-get/pypi")
            cache.mkdir(parents=True, exist_ok=True)
            dest = cache / filename
            if not dest.exists():
                urllib.request.urlretrieve(download_url, dest)
            actual = hashlib.sha256(dest.read_bytes()).hexdigest()
            if actual != sha256:
                dest.unlink(missing_ok=True)
                raise SourceError(
                    f"{filename}: sha256 mismatch (expected {sha256}, got {actual})"
                )
            return dest

        return Candidate(
            source_name=self.cfg.name, source_type="pypi",
            pkgname=f"python3-{pkgname.lower().replace('_', '-')}",
            version=version,
            fetch_fn=_fetch,
            convert_fn=convert.convert_wheel,
            depends=[],  # transitive pypi deps: not resolved in this stage
            unresolved_deps=extra_deps,
            provenance=f"pypi:{pkgname}=={version} sha256:{sha256}",
        )

    def search(self, term: str) -> list[str]:
        raise SourceError("PyPI has no simple search API here -- use https://pypi.org/search/?q=... by hand")

    def info(self, pkgname: str) -> Optional[dict]:
        url = f"{self.base}/pypi/{pkgname}/json"
        try:
            with urllib.request.urlopen(url, timeout=20) as resp:
                data = json.load(resp)
        except Exception:
            return None
        return {
            "name": f"python3-{pkgname.lower().replace('_', '-')}",
            "version": data["info"]["version"],
            "description": (data["info"].get("summary") or "").splitlines()[:1] and data["info"].get("summary", ""),
            "depends": [r.split()[0] for r in (data["info"].get("requires_dist") or []) if "extra ==" not in r],
            "source": "pypi",
        }


SOURCE_TYPES = {
    "repo": RepoSource,
    "pypi": PyPISource,
}

try:
    # debian_source.py locates debindex.py via a runtime sys.path search
    # (see that module) -- this import fails cleanly in a context where
    # debindex.py hasn't been copied alongside oryx-get yet, in which case
    # [source:debian] is skipped with a warning by build_sources() below
    # rather than crashing the whole tool.
    from . import debian_source
    SOURCE_TYPES["debian"] = debian_source.DebianSource
except Exception:
    pass


def build_sources(configs: list[SourceConfig]) -> list:
    """Unknown source TYPES are skipped with a warning, not a hard error --
    /etc/oryx-get.conf ships a [source:debian] entry ahead of the "debian"
    type actually being implemented (staged rollout), and a config should
    never have to be edited back down just because this binary is older
    than the file. A source instance with a genuinely broken type is a
    config mistake worth surfacing, but not one that should block every
    OTHER configured source from working.
    """
    sources = []
    for c in configs:
        cls = SOURCE_TYPES.get(c.type)
        if cls is None:
            print(f"[oryx-get] [source:{c.name}]: type {c.type!r} not implemented "
                  f"in this version -- skipping", file=sys.stderr)
            continue
        try:
            sources.append(cls(c))
        except SourceError as e:
            # Construction-time failures (e.g. the Debian source's trust
            # prerequisites, gpgv/keyring, being missing) disable just that
            # ONE source, never the whole tool -- repo/pypi should keep
            # working even if Debian's trust chain is temporarily broken.
            print(f"[oryx-get] [source:{c.name}] unavailable: {e}", file=sys.stderr)
    return sources
