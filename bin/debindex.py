#!/usr/bin/env python3
"""Parsing a Debian Packages index, and translating its relationship fields
into pacman ones.

Shared by closure.py (which resolves what to download) and oryx-repo-add.py
(which writes those relationships into the repository database). Keeping one
copy matters: the two must agree about what "the dependencies of X" means, or
the closure the Dell downloads and the closure pacman resolves on the target
drift apart.
"""

import re


class Index:
    """A parsed Debian Packages file."""

    def __init__(self, path):
        self.pkgs = {}      # name -> {field: value}
        self.provides = {}  # virtual name -> [real names]
        self._parse(path)

    def _parse(self, path):
        cur, field = {}, None

        def flush():
            name = cur.get("Package")
            if not name:
                return
            # First stanza wins: the index lists the highest version first.
            self.pkgs.setdefault(name, dict(cur))
            for pv in split_relations(cur.get("Provides", "")):
                bare = strip_constraint(pv)
                if bare:
                    self.provides.setdefault(bare, []).append(name)

        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if line == "\n":
                    flush()
                    cur, field = {}, None
                elif line[0] in " \t":
                    if field:
                        cur[field] = cur.get(field, "") + " " + line.strip()
                elif ":" in line:
                    field, _, val = line.partition(":")
                    cur[field] = val.strip()
                else:
                    field = None
        flush()  # a final stanza with no trailing blank line

    def resolve(self, name):
        """A real package name for `name`, following Provides. None if unknown."""
        if name in self.pkgs:
            return name
        alts = self.provides.get(name)
        return alts[0] if alts else None

    def depends(self, name, available=None):
        """Dependency names of `name`, as pacman would want them.

        `available` limits which names may be chosen -- pass the set of
        packages that actually exist in the repository, so that an alternative
        which is present is preferred over one that is not, and a clause that
        nothing can satisfy is dropped rather than left to fail the install.

        Returns (deps, dropped). Version constraints are discarded; see
        oryx-repo-add.py for why.
        """
        pkg = self.pkgs.get(name)
        if not pkg:
            return [], []

        deps, dropped, seen = [], [], set()
        for fld in ("Pre-Depends", "Depends"):
            for clause in split_relations(pkg.get(fld, "")):
                alts = [strip_constraint(a) for a in re.split(r"\s*\|\s*", clause)]
                alts = [a for a in alts if a]
                if not alts:
                    continue

                chosen = self._pick(alts, available)
                if chosen is None:
                    dropped.append(alts[0])
                elif chosen != name and chosen not in seen:
                    seen.add(chosen)
                    deps.append(chosen)
        return deps, dropped

    def _pick(self, alts, available):
        """First alternative that is usable, preferring real packages."""
        if available is None:
            for a in alts:
                if a in self.pkgs:
                    return a
            for a in alts:
                if a in self.provides:
                    return self.provides[a][0]
            return None

        # Restricted to what the repository holds: a real name first, then a
        # provider of a virtual one that is itself in the repository.
        for a in alts:
            if a in available:
                return a
        for a in alts:
            for real in self.provides.get(a, ()):
                if real in available:
                    return real
        return None

    def provides_of(self, name):
        """Virtual names `name` declares, for %PROVIDES%."""
        pkg = self.pkgs.get(name)
        if not pkg:
            return []
        out = []
        for pv in split_relations(pkg.get("Provides", "")):
            bare = strip_constraint(pv)
            if bare and bare != name and bare not in out:
                out.append(bare)
        return out


def split_relations(raw):
    """Split a comma-separated relationship field into clauses."""
    return [c.strip() for c in re.split(r"\s*,\s*", raw or "") if c.strip()]


def strip_constraint(rel):
    """`libfoo (>= 1.2) [linux-any]` -> `libfoo`, and `pkg:any` -> `pkg`."""
    rel = rel.split("(")[0].split("[")[0].split("<")[0].strip()
    return rel.split(":")[0].strip()
