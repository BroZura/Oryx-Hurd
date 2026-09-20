# --export: write converted package(s) + a repo-add'd database into a
# directory, in the exact layout a static HTTP server can serve as a
# pacman repo -- so a future HQ machine (or a Reticulum-served mirror) can
# take over with NO changes to this tool, just pointing pacman.conf's
# Server= at wherever the directory ends up. Reuses bin/oryx-repo-add.py
# AS A SUBPROCESS -- same "don't fork it" rule as debindex.py, and the
# same tool that already builds the real [oryx] repo, so an exported
# directory and the real repo are the same format by construction, not by
# two implementations happening to agree.

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

from . import sources as srcmod


def _find_repo_cached(pkgname: str) -> Path | None:
    """For an already_in_repo candidate, the file pacman already downloaded
    into its own cache -- included in an export so the result is genuinely
    self-contained, not "everything except what oryx-get considered
    already handled."
    """
    cache = Path("/var/cache/pacman/pkg")
    if not cache.is_dir():
        return None
    matches = sorted(cache.glob(f"{pkgname}-*.pkg.tar.zst"))
    return matches[-1] if matches else None


def collect(cand: srcmod.Candidate, cfg, dry_run: bool, seen: set[str] | None = None) -> list[Path]:
    """Convert (or locate) every package in cand's tree, WITHOUT installing
    any of them locally -- export is a side output, not tied to a local
    pacman -S/-U the way normal install_candidate() is.
    """
    if seen is None:
        seen = set()
    if cand.pkgname in seen:
        return []
    seen.add(cand.pkgname)

    out: list[Path] = []
    for dep in cand.needs:
        out.extend(collect(dep, cfg, dry_run, seen))

    if cand.already_in_repo:
        cached = _find_repo_cached(cand.pkgname)
        if cached:
            out.append(cached)
        else:
            print(f"[oryx-get] --export: {cand.pkgname} is in [oryx] but not found in "
                  f"pacman's cache (never downloaded here) -- not included. Install it "
                  f"once first if the export needs to be self-contained.", file=sys.stderr)
        return out

    if dry_run:
        print(f"[dry-run] would fetch+convert {cand.pkgname} for export")
        return out

    outdir = cfg.cache_dir / "built"
    artifact = cand.fetch_fn()
    pkg_path = cand.convert_fn(artifact, outdir, cand.depends or [], cand.provenance)
    out.append(pkg_path)
    return out


def export(cand: srcmod.Candidate, cfg, target_dir: Path, dry_run: bool, repo_name: str = "oryx") -> int:
    target_dir.mkdir(parents=True, exist_ok=True)
    files = collect(cand, cfg, dry_run)
    if dry_run:
        print(f"[dry-run] would export {len(files)} package(s) to {target_dir} "
              f"and run oryx-repo-add.py")
        return 0
    if not files:
        print("[oryx-get] --export: nothing to export", file=sys.stderr)
        return 1

    for f in files:
        dest = target_dir / f.name
        if f.resolve() != dest.resolve():
            shutil.copy2(f, dest)
        print(f"[oryx-get] exported {f.name}")

    # /usr/lib/oryx-get/oryx-repo-add.py in a proper install; the dev-checkout
    # fallback matches build.sh's staging layout (bin/oryx-get/oryx-repo-add.py).
    repo_add = Path("/usr/lib/oryx-get/oryx-repo-add.py")
    if not repo_add.is_file():
        repo_add = Path(__file__).resolve().parent.parent / "oryx-repo-add.py"
    r = subprocess.run([sys.executable, str(repo_add), str(target_dir), "-n", repo_name])
    if r.returncode != 0:
        return r.returncode

    print(f"\n[oryx-get] {target_dir} is now a servable pacman repo (name={repo_name}).")
    print(f"           A client adds it with, e.g.:")
    print(f"             [{repo_name}]")
    print(f"             Server = http://<this-host>:<port>")
    print(f"           serving {target_dir} over plain HTTP is enough (python3 -m http.server).")
    return 0
