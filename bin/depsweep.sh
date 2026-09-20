#!/bin/bash
# Static DT_NEEDED sweep over a mounted Hurd target.
# Does NOT run ldd: that would resolve against the HOST's libraries and lie.
# Prunes dev/servers/proc/mnt -- stat-ing a passive translator node starts it
# in the RUNNING system (handoff section 6), and the target's /mnt is dead.
# Note: find must be given "/mnt/." -- bare "/mnt" does not descend.
T=/mnt
OUT=/root/depsweep
rm -rf "$OUT"; mkdir -p "$OUT"

find "$T/." \( -name dev -o -name servers -o -name proc -o -name mnt \) -prune -o \
     -type f -print 2>/dev/null | sed 's#/\./#/#' | sort > "$OUT/allfiles"
echo "files scanned: $(wc -l < "$OUT/allfiles")"

# One readelf per file. Non-ELF files simply produce no output.
: > "$OUT/needed"; : > "$OUT/soname"
while IFS= read -r f; do
  readelf -d "$f" 2>/dev/null | sed -n \
    -e "s|.*(NEEDED).*\[\(.*\)\]|N\t\1\t$f|p" \
    -e "s|.*(SONAME).*\[\(.*\)\]|S\t\1\t$f|p"
done < "$OUT/allfiles" > "$OUT/dyn"

grep -P '^N\t' "$OUT/dyn" | cut -f2,3 > "$OUT/needed"
{ grep -P '^S\t' "$OUT/dyn" | cut -f2
  # a library is also findable by its on-disk basename
  grep -E '\.so($|\.)' "$OUT/allfiles" | sed 's#.*/##'
} | sort -u > "$OUT/present"

echo "ELF objects with a dynamic section: $(cut -f2 "$OUT/dyn" | wc -l)"
echo "sonames present in target: $(wc -l < "$OUT/present")"

cut -f1 "$OUT/needed" | sort -u > "$OUT/needed.uniq"
comm -23 "$OUT/needed.uniq" "$OUT/present" > "$OUT/missing"
echo
echo "===== MISSING SONAMES: $(wc -l < "$OUT/missing") ====="
while IFS= read -r m; do
  n=$(awk -F'\t' -v m="$m" '$1==m' "$OUT/needed" | wc -l)
  echo "--- $m  (needed by $n)"
  awk -F'\t' -v m="$m" '$1==m{print "      " $2}' "$OUT/needed" | sort -u | head -15
done < "$OUT/missing"
