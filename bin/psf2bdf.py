#!/usr/bin/env python3
"""psf2bdf.py <font.psf[.gz]> <out.bdf>

Convert a Linux console PSF font to BDF, for use as the GNU/Hurd console's
/usr/share/hurd/vga-system.bdf.

Debian has bdf2psf but not the reverse, and the Hurd console's vga driver
reads BDF only -- it takes no font options at all, it just opens that fixed
path. VGA text mode fixes the cell width at 8 pixels, so the font height is
what sets the row count: 16 -> 25 rows, 14 -> 28, 8 -> 50.

Handles PSF1 and PSF2, including their Unicode tables, so glyphs come out
with real ENCODING values rather than a guessed codepage.
"""
import sys, gzip, struct

src, dst = sys.argv[1], sys.argv[2]
raw = (gzip.open if src.endswith(".gz") else open)(src, "rb").read()

if raw[:2] == b"\x36\x04":                       # PSF1
    mode, charsize = raw[2], raw[3]
    width, height = 8, charsize
    nglyphs = 512 if (mode & 0x01) else 256
    has_uni = bool(mode & 0x06)
    data = raw[4:4 + nglyphs * charsize]
    tail = raw[4 + nglyphs * charsize:]
    psf1 = True
elif raw[:4] == b"\x72\xb5\x4a\x86":             # PSF2
    (ver, hdrsize, flags, nglyphs,
     charsize, height, width) = struct.unpack_from("<7I", raw, 4)
    has_uni = bool(flags & 0x01)
    data = raw[hdrsize:hdrsize + nglyphs * charsize]
    tail = raw[hdrsize + nglyphs * charsize:]
    psf1 = False
else:
    sys.exit("not a PSF font")

if width != 8:
    sys.exit(f"width {width} != 8; VGA text mode needs an 8-pixel-wide font")

bytes_per_row = (width + 7) // 8
rowbytes = charsize // height if height else 1

# --- Unicode table: glyph index -> list of codepoints -----------------------
enc = {}
if has_uni and tail:
    if psf1:
        vals = struct.unpack("<%dH" % (len(tail) // 2), tail[: len(tail) // 2 * 2])
        gi, cur = 0, []
        for v in vals:
            if v == 0xFFFF:
                if cur:
                    enc[gi] = cur
                gi += 1; cur = []
            elif v == 0xFFFE:
                pass                      # start of a sequence; ignore combos
            else:
                if gi not in enc:
                    cur.append(v)
    else:
        gi = 0
        for chunk in tail.split(b"\xff"):
            if gi >= nglyphs:
                break
            first = chunk.split(b"\xfe")[0]
            if first:
                try:
                    enc[gi] = [ord(c) for c in first.decode("utf-8")]
                except UnicodeDecodeError:
                    pass
            gi += 1

# One glyph per codepoint; lowest glyph index wins for a duplicate codepoint.
cps = {}
for gi in range(nglyphs):
    for cp in (enc.get(gi) or ([gi] if not has_uni else [])):
        cps.setdefault(cp, gi)
if not cps:
    cps = {i: i for i in range(nglyphs)}

asc = height - (1 if height > 6 else 0)
out = [
    "STARTFONT 2.1",
    f"FONT -misc-console-medium-r-normal--{height}-{height*10}-75-75-c-{width*10}-iso10646-1",
    f"SIZE {height} 75 75",
    f"FONTBOUNDINGBOX {width} {height} 0 {-(height-asc)}",
    "STARTPROPERTIES 5",
    f"FONT_ASCENT {asc}",
    f"FONT_DESCENT {height - asc}",
    f"DEFAULT_CHAR 32",
    f'FAMILY_NAME "console"',
    f'WEIGHT_NAME "medium"',
    "ENDPROPERTIES",
    f"CHARS {len(cps)}",
]
for cp in sorted(cps):
    gi = cps[cp]
    glyph = data[gi * charsize:(gi + 1) * charsize]
    out += [
        f"STARTCHAR U+{cp:04X}",
        f"ENCODING {cp}",
        "SWIDTH 480 0",
        f"DWIDTH {width} 0",
        f"BBX {width} {height} 0 {-(height-asc)}",
        "BITMAP",
    ]
    for r in range(height):
        row = glyph[r * rowbytes:(r + 1) * rowbytes]
        out.append(row.hex().upper().ljust(bytes_per_row * 2, "0"))
    out.append("ENDCHAR")
out.append("ENDFONT")

open(dst, "w").write("\n".join(out) + "\n")
print(f"{dst}: {width}x{height}, {len(cps)} glyphs  (~{400//height} text rows)")
