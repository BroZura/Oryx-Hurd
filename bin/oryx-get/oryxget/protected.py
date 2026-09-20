# Protected-package refusal.
#
# Refuse by default to install or upgrade anything on /etc/oryx-get.conf's
# [protected] list, or anything that would pull one in as a dependency.
# --allow-protected plus an interactive confirmation are both required to
# override. This exists because of a real incident (FIXES.md 14: an
# unattended openssh upgrade locked the T400 out over SSH, the only way in)
# -- not caution for its own sake. See t400-remote-access-rule.

from __future__ import annotations

import sys


def find_protected(cand, protected: set[str], _seen=None) -> list[str]:
    """Every protected package name that `cand` or any of its transitive
    `needs` would install or touch. Empty list if none.
    """
    if _seen is None:
        _seen = set()
    if cand.pkgname in _seen:
        return []
    _seen.add(cand.pkgname)

    hits = []
    if cand.pkgname in protected:
        hits.append(cand.pkgname)
    for dep in cand.needs:
        hits.extend(find_protected(dep, protected, _seen))
    return hits


def confirm_protected(hits: list[str]) -> bool:
    """Interactive confirmation. Never bypassed programmatically -- if
    stdin isn't a TTY, this refuses rather than assuming yes, so a script
    or cron job can never silently touch a protected package.
    """
    print(f"[oryx-get] REFUSING by default: this would install/upgrade "
          f"protected package(s): {', '.join(hits)}", file=sys.stderr)
    print("[oryx-get] These are protected because touching them wrong has "
          "already broken this exact machine once (FIXES.md 14) -- an "
          "unattended openssh upgrade locked it out over SSH, the ONLY way "
          "in. --allow-protected was passed; confirming interactively is "
          "still required.", file=sys.stderr)
    if not sys.stdin.isatty():
        print("[oryx-get] stdin is not a TTY -- refusing rather than assuming "
              "an answer. Run this from an interactive shell to confirm.",
              file=sys.stderr)
        return False
    try:
        answer = input(f"Type the package name to confirm ({hits[0]}): ")
    except (EOFError, KeyboardInterrupt):
        print()
        return False
    return answer.strip() == hits[0]
