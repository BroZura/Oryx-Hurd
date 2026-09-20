#!/usr/bin/env python3
"""Compute the Depends+PreDepends closure of a package set from a Debian
Packages file, resolving Provides and taking the first alternative of an
"a | b" dependency (which is what apt does absent other constraints).

Used to size the Xfce job for the Arch Hurd target before committing to it.
"""
import sys, re

args = sys.argv[1:]
LIST = "--list" in args
if LIST:
    args.remove("--list")
path = args[0]
roots = args[1:]

pkgs = {}      # name -> dict
provides = {}  # virtual -> [real names]

cur = {}
field = None
with open(path, encoding="utf-8", errors="replace") as fh:
    for line in fh:
        if line == "\n":
            if cur.get("Package"):
                name = cur["Package"]
                # first stanza wins (highest version listed first in practice)
                pkgs.setdefault(name, cur)
                for pv in re.split(r"\s*,\s*", cur.get("Provides", "")):
                    pv = pv.split("(")[0].strip()
                    if pv:
                        provides.setdefault(pv, []).append(name)
            cur, field = {}, None
            continue
        if line[0] in " \t":
            if field:
                cur[field] = cur.get(field, "") + " " + line.strip()
            continue
        if ":" in line:
            field, _, val = line.partition(":")
            cur[field] = val.strip()
        else:
            field = None

def deps_of(name):
    p = pkgs.get(name)
    if not p:
        return []
    out = []
    for f in ("Pre-Depends", "Depends"):
        raw = p.get(f, "")
        if not raw:
            continue
        for clause in re.split(r"\s*,\s*", raw):
            clause = clause.strip()
            if not clause:
                continue
            alts = [a.split("(")[0].split("[")[0].strip()
                    for a in re.split(r"\s*\|\s*", clause)]
            chosen = None
            for a in alts:                      # prefer a real package
                if a in pkgs:
                    chosen = a; break
            if chosen is None:
                for a in alts:                  # else a provider
                    if a in provides:
                        chosen = provides[a][0]; break
            if chosen:
                out.append(chosen)
    return out

seen, missing, stack = set(), set(), list(roots)
while stack:
    n = stack.pop()
    if n in seen:
        continue
    if n not in pkgs:
        if n in provides:
            n = provides[n][0]
            if n in seen:
                continue
        else:
            missing.add(n)
            continue
    seen.add(n)
    stack.extend(deps_of(n))

if LIST:
    for n in sorted(seen):
        p_ = pkgs[n]
        print(f"{n}\t{p_.get('Version')}\t{p_.get('Filename')}")
    sys.exit(0)

size = sum(int(pkgs[n].get("Size", 0) or 0) for n in seen)
inst = sum(int(pkgs[n].get("Installed-Size", 0) or 0) for n in seen)
print(f"roots:            {' '.join(roots)}")
print(f"packages:         {len(seen)}")
print(f"download size:    {size/1e6:.0f} MB")
print(f"installed size:   {inst/1024:.0f} MB")
if missing:
    print(f"NOT AVAILABLE for hurd-i386 ({len(missing)}): {' '.join(sorted(missing))}")
