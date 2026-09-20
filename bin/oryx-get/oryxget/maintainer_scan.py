# Scan a .deb's dropped maintainer scripts (postinst/preinst/postrm/prerm)
# for things our pacman hooks do NOT cover, so oryx-get can label a result
# "may not work" instead of failing silently later -- exactly the class of
# gap that cost real debugging time for i2pd's missing user (FIXES.md 16)
# and rnsd's $HOME/datadir surprise, found by hand each time because
# nothing warned up front. This is a heuristic grep, not a shell
# interpreter -- it will miss anything hidden behind a variable or a
# sourced file, and that is a stated limitation, not a bug to chase.

from __future__ import annotations

import re
import subprocess
import tempfile
from pathlib import Path

# Patterns already handled by our own pacman hooks (target/hooks/*.hook) --
# see FIXES.md 0 and 14. A maintainer script doing ONLY these is fine.
COVERED = [
    (r"\bldconfig\b", "shared library cache (10-ldconfig.hook)"),
    (r"\bupdate-ca-certificates\b", "CA bundle (20-ca-certificates.hook)"),
    (r"\bupdate-alternatives\b", "alternatives (30-alternatives.hook)"),
    (r"\bglib-compile-schemas\b", "glib schemas (40-glib-schemas.hook)"),
    (r"\bupdate-mime-database\b", "MIME database (50-mime.hook)"),
    (r"\bupdate-desktop-database\b", "desktop database (60-desktop.hook)"),
    (r"\bgtk-update-icon-cache\b", "icon cache (70-icon-cache.hook)"),
    (r"\bfc-cache\b", "fontconfig cache (80-fontconfig.hook)"),
    (r"\bsystemd-sysusers\b", "sysusers.d fragments (90-sysusers.hook)"),
]

# Patterns known NOT to be covered by anything oryx runs automatically --
# each maps to a one-line explanation of the actual gap, in FIXES.md's own
# voice, so the warning is immediately actionable rather than just ominous.
UNCOVERED = [
    (r"\badduser\b|\buseradd\b",
     "creates a system user by shell command, not a sysusers.d fragment -- "
     "the 90-sysusers hook cannot catch this (FIXES.md 16, i2pd's exact gap). "
     "The user will not exist after conversion; oryx-get does not create it."),
    (r"\bupdate-rc\.d\b|\binvoke-rc\.d\b",
     "registers or starts a sysvinit service -- Hurd DOES use sysvinit here, "
     "but this registration is dropped along with the rest of the postinst. "
     "The service's init script will exist but is not wired into any "
     "rcN.d/ runlevel; it will not start at boot without a manual symlink "
     "(same pattern as oryx-rnsd/oryx-i2pd in oryx-postinst.sh)."),
    (r"\bdeb-systemd-helper\b|\bsystemctl\s+(enable|start|daemon-reload)\b",
     "manages a systemd unit -- irrelevant on this sysvinit system, but a "
     "sign the package expects a running service oryx-get will not start."),
    (r"\bdebconf\b|\bdb_(get|set|input|go)\b",
     "reads or writes debconf answers -- nothing runs debconf here, so any "
     "configuration this would have prompted for keeps its compiled-in "
     "default instead."),
    (r"\bchown\b|\bchmod\b",
     "changes ownership/permissions on a path (often a runtime state "
     "directory it also creates) -- oryx-get does not replay this; a "
     "service expecting a specific owner may need it set by hand, the "
     "same class of gap as i2pd's /run and /var/log ownership."),
    (r"\bmigrate\b|\bupgrade-migration\b",
     "appears to run a data migration -- this only ever matters when "
     "upgrading an existing install, which oryx-get does not orchestrate."),
]


def _extract_scripts(deb_path: Path) -> dict[str, str]:
    scripts = {}
    with tempfile.TemporaryDirectory(prefix="oryx-get-scan-") as tmp:
        r = subprocess.run(["dpkg-deb", "-e", str(deb_path), tmp],
                            capture_output=True, text=True)
        if r.returncode != 0:
            return scripts
        for name in ("preinst", "postinst", "prerm", "postrm"):
            p = Path(tmp) / name
            if p.is_file():
                scripts[name] = p.read_text(errors="replace")
    return scripts


def scan(deb_path: Path) -> dict:
    """Returns {"has_scripts": bool, "covered": [...], "uncovered": [(pattern_desc, script)...]}."""
    scripts = _extract_scripts(deb_path)
    result = {"has_scripts": bool(scripts), "covered": [], "uncovered": []}
    for script_name, text in scripts.items():
        for pattern, desc in COVERED:
            if re.search(pattern, text):
                result["covered"].append(f"{script_name}: {desc}")
        for pattern, desc in UNCOVERED:
            if re.search(pattern, text):
                result["uncovered"].append(f"{script_name}: {desc}")
    return result


def warn(pkgname: str, scan_result: dict) -> bool:
    """Print warnings for a scan result. Returns True if the package should
    be labelled "may not work" (i.e. an uncovered pattern was found).
    """
    if not scan_result["has_scripts"]:
        return False
    if scan_result["uncovered"]:
        print(f"[oryx-get] {pkgname}: MAY NOT WORK -- dropped maintainer script(s) "
              f"do something our pacman hooks don't cover:")
        for u in scan_result["uncovered"]:
            print(f"    - {u}")
        return True
    if scan_result["covered"]:
        print(f"[oryx-get] {pkgname}: dropped maintainer script(s) only do things "
              f"already covered by pacman hooks ({'; '.join(scan_result['covered'])}) -- should be fine.")
    return False
