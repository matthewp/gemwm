"""Turn EmuTOS's Atari ST 8x16 system font (fnt_st_8x16.c) into a TrueType
font whose glyphs are the original pixels as square outlines, so it stays
pixel-sharp at any whole-number scale.

usage: st_font.py fnt_st_8x16.c out.ttf
"""
import re
import sys

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

W, H = 8, 16          # pixels per glyph
ROW_BYTES = 256       # the font form: 256 glyphs side by side, 16 rows
PX = 64               # font units per pixel
ASCENT, DESCENT = 14, 2  # glyphs sit on row 13; g and y reach row 15

src = open(sys.argv[1]).read()
table = src[src.index("dat_table[]"):]
table = table[table.index("{") + 1:table.index("};")]
words = [int(w, 16) for w in re.findall(r"0x([0-9a-fA-F]{4})", table)]
data = bytearray()
for w in words:            # 68000 words: high byte first
    data += bytes([w >> 8, w & 0xff])
assert len(data) == ROW_BYTES * H, len(data)


def rows(ch):
    return [data[r * ROW_BYTES + ch] for r in range(H)]


# Atari ST code -> Unicode. ASCII, and 0x80-0xAF where the ST's accented
# letters and symbols match code page 437.
codes = {c: chr(c) for c in range(0x20, 0x7f)}
codes.update({c: bytes([c]).decode("cp437") for c in range(0x80, 0xb0)})

fb = FontBuilder(unitsPerEm=H * PX, isTTF=True)
names = [".notdef"] + [f"uni{ord(u):04X}" for u in codes.values()]
fb.setupGlyphOrder(names)
fb.setupCharacterMap({ord(u): f"uni{ord(u):04X}" for u in codes.values()})

glyphs = {}
pen = TTGlyphPen(None)
glyphs[".notdef"] = pen.glyph()
for code, u in codes.items():
    pen = TTGlyphPen(None)
    for r, bits in enumerate(rows(code)):
        top = (ASCENT - r) * PX
        bottom = top - PX
        x = 0
        while x < W:       # one rectangle per run of set pixels
            if bits & (0x80 >> x):
                start = x
                while x < W and bits & (0x80 >> x):
                    x += 1
                left, right = start * PX, x * PX
                pen.moveTo((left, bottom))   # clockwise, as TrueType wants
                pen.lineTo((left, top))
                pen.lineTo((right, top))
                pen.lineTo((right, bottom))
                pen.closePath()
            else:
                x += 1
    glyphs[f"uni{ord(u):04X}"] = pen.glyph()
fb.setupGlyf(glyphs)
fb.setupHorizontalMetrics({n: (W * PX, 0) for n in names})
fb.setupHorizontalHeader(ascent=ASCENT * PX, descent=-DESCENT * PX)
fb.setupNameTable({
    "familyName": "Atari ST 8x16",
    "styleName": "Regular",
    "copyright": "Glyphs from EmuTOS (fnt_st_8x16.c), GPL-2.0-or-later",
})
fb.setupOS2(sTypoAscender=ASCENT * PX, sTypoDescender=-DESCENT * PX,
            sTypoLineGap=0, usWinAscent=ASCENT * PX,
            usWinDescent=DESCENT * PX, xAvgCharWidth=W * PX,
            panose={"bFamilyType": 2, "bSerifStyle": 0, "bWeight": 5,
                    "bProportion": 9,  # monospaced
                    "bContrast": 0, "bStrokeVariation": 0, "bArmStyle": 0,
                    "bLetterForm": 0, "bMidline": 0, "bXHeight": 0})
fb.setupPost(isFixedPitch=1)
fb.save(sys.argv[2])
print(f"wrote {len(codes)} glyphs to {sys.argv[2]}")
