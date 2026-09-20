# type = debian. The Debian hurd-i386 (debian-ports) Packages index.
#
# Dependency parsing/alternative-resolution reuses bin/debindex.py UNCHANGED
# (imported, not copied) -- "the two must agree about what 'the dependencies
# of X' means" applies just as much to oryx-get as to closure.py/oryx-repo-
# add.py, which is the whole reason that module exists as shared code
# instead of living inside one script.
#
# TRUST, per the explicit requirement this was built against: verify
# InRelease's GPG signature against the debian-ports-archive-keyring package
# (gpgv, not gnupg -- gnupg has no hurd-i386 build, gpgv does), verify the
# Packages file's sha256 against InRelease, then verify each downloaded
# .deb's sha256 against the Packages file. Every step of that chain was
# tested by hand on the T400 before this was written -- see FIXES.md.
# Never add a fallback path that skips verification; if gpgv or the keyring
# is missing, this refuses to run at all rather than degrade silently.

from __future__ import annotations

import bz2
import gzip
import hashlib
import lzma
import subprocess
import sys
import urllib.request
from pathlib import Path

# debindex.py's canonical home is bin/debindex.py (shared with closure.py and
# oryx-repo-add.py on the Dell) -- the packaging step (bin/oryx-get/build.sh)
# copies it alongside this tool's own files so the SAME file ships here, not
# a fork of it. Falls back to the Dell dev-tree location (one level up from
# bin/oryx-get/) so `python3 oryx-get` also works uninstalled, from a checkout.
_here = Path(__file__).resolve().parent.parent
for _candidate in (_here, _here.parent):
    if (_candidate / "debindex.py").is_file():
        sys.path.insert(0, str(_candidate))
        break
import debindex  # noqa: E402

from .config import SourceConfig
from .sources import Candidate, SourceError
from . import convert


class TrustError(SourceError):
    """Raised when the trust chain cannot be established. oryx-get's caller
    treats this as fatal for the whole 'debian' source -- never caught and
    silently downgraded to an unverified download.
    """


class DebianSource:
    def __init__(self, cfg: SourceConfig):
        self.cfg = cfg
        self.url = cfg.options.get("url", "http://deb.debian.org/debian-ports").rstrip("/")
        self.suite = cfg.options.get("suite", "unstable")
        self.component = cfg.options.get("component", "main")
        self.arch = cfg.options.get("arch", "hurd-i386")
        self.keyring = Path(cfg.options.get("keyring", "/usr/share/keyrings/debian-ports-archive-keyring.gpg"))
        self.cache = Path("/var/cache/oryx-get/debian")
        self.cache.mkdir(parents=True, exist_ok=True)
        self._index: debindex.Index | None = None
        self._installed: set[str] | None = None
        self._check_trust_prereqs()

    # ---------------------------------------------------------------- trust --

    def _check_trust_prereqs(self) -> None:
        gpgv = subprocess.run(["sh", "-c", "command -v gpgv"], capture_output=True, text=True)
        if gpgv.returncode != 0 or not gpgv.stdout.strip():
            raise TrustError(
                "gpgv is not installed -- the Debian source cannot be trusted "
                "without it. Install with: pacman -S gpgv debian-ports-archive-keyring "
                "(both exist for hurd-i386/as arch=all in the archive, verified "
                "2026-09-20). Refusing to fall back to an unverified download."
            )
        if not self.keyring.is_file():
            raise TrustError(
                f"{self.keyring} not found -- install debian-ports-archive-keyring "
                "first. Refusing to fall back to an unverified download."
            )

    def _verify_inrelease(self, path: Path) -> None:
        r = subprocess.run(
            ["gpgv", "--keyring", str(self.keyring), str(path)],
            capture_output=True, text=True,
        )
        if r.returncode != 0:
            raise TrustError(f"InRelease signature verification FAILED:\n{r.stderr}")

    # -------------------------------------------------------------- fetching --

    def _fetch(self, url: str, dest: Path) -> Path:
        if not dest.exists():
            tmp = dest.with_suffix(dest.suffix + ".part")
            urllib.request.urlretrieve(url, tmp)
            tmp.rename(dest)
        return dest

    def _get_index(self) -> debindex.Index:
        if self._index is not None:
            return self._index

        inrelease = self.cache / f"InRelease-{self.suite}"
        self._fetch(f"{self.url}/dists/{self.suite}/InRelease", inrelease)
        self._verify_inrelease(inrelease)

        # Parse InRelease for the sha256 of the Packages file we want, so the
        # download below can be checked against a value that was itself part
        # of the GPG-signed document -- not trusted on its own.
        wanted_path = f"{self.component}/binary-{self.arch}/Packages"
        sha256s = self._parse_release_hashes(inrelease, "SHA256")
        pkgs_entry = None
        pkgs_ext = None
        for ext in (".xz", ".gz", ".bz2", ""):
            key = wanted_path + ext
            if key in sha256s:
                pkgs_entry, pkgs_ext = sha256s[key], ext
                break
        if pkgs_entry is None:
            raise TrustError(f"InRelease has no entry for {wanted_path}(.xz/.gz/.bz2)")

        compressed = self.cache / f"Packages-{self.suite}-{self.arch}{pkgs_ext}"
        self._fetch(f"{self.url}/dists/{self.suite}/{wanted_path}{pkgs_ext}", compressed)
        actual = hashlib.sha256(compressed.read_bytes()).hexdigest()
        if actual != pkgs_entry["sha256"]:
            compressed.unlink(missing_ok=True)
            raise TrustError(
                f"Packages{pkgs_ext} sha256 mismatch against InRelease "
                f"(expected {pkgs_entry['sha256']}, got {actual}) -- refusing to use it"
            )

        packages_path = self.cache / f"Packages-{self.suite}-{self.arch}"
        raw = compressed.read_bytes()
        if pkgs_ext == ".xz":
            data = lzma.decompress(raw)
        elif pkgs_ext == ".gz":
            data = gzip.decompress(raw)
        elif pkgs_ext == ".bz2":
            data = bz2.decompress(raw)
        else:
            data = raw
        packages_path.write_bytes(data)

        self._index = debindex.Index(str(packages_path))
        return self._index

    @staticmethod
    def _parse_release_hashes(inrelease: Path, field_name: str) -> dict:
        """Minimal Release-file hash-list parser: lines under 'SHA256:' of
        the form '  <hash> <size> <path>'. InRelease is a clearsigned
        Release file -- the hash block's format is identical either way.
        """
        out = {}
        in_block = False
        for line in inrelease.read_text(errors="replace").splitlines():
            if line.strip() == f"{field_name}:":
                in_block = True
                continue
            if in_block:
                if line.startswith((" ", "\t")):
                    parts = line.split()
                    if len(parts) == 3:
                        h, size, path = parts
                        out[path] = {"sha256": h, "size": int(size)}
                    continue
                else:
                    in_block = False
        return out

    def _installed_set(self) -> set[str]:
        if self._installed is None:
            r = subprocess.run(["pacman", "-Q"], capture_output=True, text=True)
            self._installed = {line.split()[0] for line in r.stdout.splitlines() if line.strip()}
        return self._installed

    def _repo_available_set(self) -> set[str]:
        """Names pacman can resolve from [oryx] right now, installed or not
        -- "skip what pacman already provides" means this too, not just
        already-installed packages: a dependency already converted and
        sitting in the repo should be `pacman -S`'d, never re-converted from
        Debian a second time.
        """
        if not hasattr(self, "_repo_avail"):
            r = subprocess.run(["pacman", "-Sl", "oryx"], capture_output=True, text=True)
            self._repo_avail = {line.split()[1] for line in r.stdout.splitlines() if len(line.split()) >= 2}
        return self._repo_avail

    # ------------------------------------------------------------------ API --

    def search(self, term: str) -> list[str]:
        idx = self._get_index()
        term_lower = term.lower()
        hits = []
        for name, pkg in idx.pkgs.items():
            desc = pkg.get("Description", "")
            if term_lower in name.lower() or term_lower in desc.lower():
                hits.append(f"debian/{name} {pkg.get('Version', '?')} -- {desc.splitlines()[0] if desc else ''}")
        return sorted(hits)

    def info(self, pkgname: str) -> dict | None:
        idx = self._get_index()
        real = idx.resolve(pkgname)
        if real is None:
            return None
        pkg = idx.pkgs[real]
        deps, _ = idx.depends(real, available=self._installed_set() | self._repo_available_set())
        return {
            "name": real, "version": pkg.get("Version", "?"),
            "description": pkg.get("Description", "").splitlines()[0] if pkg.get("Description") else "",
            "depends": deps, "source": "debian",
            "filename": pkg.get("Filename", ""), "sha256": pkg.get("SHA256", ""),
        }

    def find(self, pkgname: str) -> Candidate | None:
        idx = self._get_index()
        real = idx.resolve(pkgname)
        if real is None:
            return None
        return self._build_candidate(real, idx, set())

    def _build_candidate(self, name: str, idx: debindex.Index, seen: set[str]) -> Candidate | None:
        if name in seen:
            return None  # cycle guard
        seen = seen | {name}

        pkg = idx.pkgs.get(name)
        if pkg is None:
            return None

        # "available" biases debindex's OR-alternative picking (e.g. Debian's
        # tmux: systemd | systemd-standalone-tmpfiles | systemd-tmpfiles --
        # pick whichever this box can actually satisfy) toward what is
        # already installed OR already sitting in the [oryx] repo, same
        # rule oryx-setdeps.py uses for the same reason (FIXES.md 13).
        available = self._installed_set() | self._repo_available_set()
        deps, dropped = idx.depends(name, available=available)
        # idx.depends(available=...) is built for the repo-INDEXING use case
        # (oryx-setdeps.py/oryx-repo-add.py): drop anything not in a curated
        # final set, on purpose, so the shipped repo never promises a
        # dependency it cannot supply (FIXES.md 13). For oryx-get's recursive
        # fetch-and-convert, "not already available" must mean "go convert
        # it," not "drop it" -- otherwise a perfectly ordinary sole
        # dependency like w3m's libgc1 (no OR-alternatives, just not yet
        # installed or repo-published) gets silently dropped instead of
        # pulled in. Re-resolve each dropped name with Index.resolve()
        # (existing public API, not a fork of the alternative-picking logic)
        # to find any real package for it before giving up on it entirely.
        for dropped_name in dropped:
            real = idx.resolve(dropped_name)
            if real is not None and real not in deps:
                deps.append(real)
            else:
                print(f"[oryx-get] {name}: dependency {dropped_name!r} does not "
                      f"exist in the Debian index either -- proceeding without it",
                      file=sys.stderr)

        needs = []
        dep_names_for_pkginfo = []
        for dep in deps:
            if dep in self._installed_set():
                dep_names_for_pkginfo.append(dep)
            elif dep in self._repo_available_set():
                needs.append(Candidate(
                    source_name="oryx", source_type="repo",
                    pkgname=dep, version="", already_in_repo=True,
                ))
                dep_names_for_pkginfo.append(dep)
            else:
                dep_cand = self._build_candidate(dep, idx, seen)
                if dep_cand is not None:
                    needs.append(dep_cand)
                    dep_names_for_pkginfo.append(dep_cand.pkgname)
                else:
                    print(f"[oryx-get] {name}: dependency {dep!r} not found in the "
                          f"Debian index either -- proceeding without it", file=sys.stderr)

        version = pkg.get("Version", "0")
        sha256 = pkg.get("SHA256", "")
        filename = pkg.get("Filename", "")
        if not filename or not sha256:
            raise SourceError(f"{name}: Packages entry missing Filename/SHA256 -- cannot verify")

        def _fetch(name=name, filename=filename, sha256=sha256) -> Path:
            deb_path = self.cache / Path(filename).name
            if not deb_path.exists():
                urllib.request.urlretrieve(f"{self.url}/{filename}", deb_path)
            actual = hashlib.sha256(deb_path.read_bytes()).hexdigest()
            if actual != sha256:
                deb_path.unlink(missing_ok=True)
                raise TrustError(
                    f"{filename}: sha256 mismatch against the (GPG-verified) "
                    f"Packages index (expected {sha256}, got {actual}) -- refusing to install it"
                )
            return deb_path

        def _convert(artifact, outdir, depends, source, _dep_names=dep_names_for_pkginfo):
            # `depends` from the caller is ignored in favour of the names
            # resolved here, which reflect this exact recursive resolution
            # (already-installed deps included) rather than whatever the
            # generic CLI plumbing passed through.
            return convert.convert_deb(artifact, outdir, _dep_names, source)

        return Candidate(
            source_name=self.cfg.name, source_type="debian",
            pkgname=name, version=convert.mangle_version(version),
            fetch_fn=_fetch, convert_fn=_convert,
            depends=dep_names_for_pkginfo,
            needs=needs,
            provenance=f"debian:{name}={version} sha256:{sha256}",
        )
