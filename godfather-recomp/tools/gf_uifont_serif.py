"""gf_uifont_serif.py -- v1098S: add serif glyphs to assets/ui_font.bin for the store / inventory side sheet.
Printable ASCII of P052 Bold (Palatino-like, the deco titles) as codes 0xF100+c and P052 Italic (the quote-style item
description) as 0xF200+c, appended to the TEXT glyphs in the free space of the 1024x1024 atlas; existing text glyphs and
icons keep their exact pixels. Usage: python3 gf_uifont_serif.py <in ui_font.bin> <out ui_font.bin>"""
import struct, sys, os
from PIL import Image, ImageDraw, ImageFont
src, dst = sys.argv[1], sys.argv[2]
def _pick(*c): return next((f for f in c if f and os.path.isfile(f)), None)
_WF = os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'Fonts')
FONTS = [(0xF100, _pick('/usr/share/fonts/opentype/urw-base35/P052-Bold.otf', os.path.join(_WF, 'palab.ttf'), os.path.join(_WF, 'georgiab.ttf'))),
         (0xF200, _pick('/usr/share/fonts/opentype/urw-base35/P052-Italic.otf', os.path.join(_WF, 'palai.ttf'), os.path.join(_WF, 'georgiai.ttf')))]
if not all(p for _, p in FONTS): sys.exit('no serif fonts (P052 / Palatino Linotype / Georgia) found')
d = open(src, 'rb').read(); assert d[:8] == b'IFUIFNT1'
AW, AH, nt, TLINE, ni, ILINE, wx, wy = struct.unpack('<8I', d[8:40])
recs = [struct.unpack('<I8h', d[40 + 20 * i:60 + 20 * i]) for i in range(nt + ni)]
atlas = Image.frombytes('RGBA', (AW, AH), d[40 + 20 * (nt + ni):40 + 20 * (nt + ni) + AW * AH * 4])
text = [r[:8] for r in recs[:nt] if not (0xF100 <= r[0] < 0xF300)]   # re-running replaces earlier serif glyphs
icons = [r[:8] for r in recs[nt:]]
used = max(r[2] + r[4] for r in text + icons)
x, y, rowh = 4, used + 6, 0
def place(img):
    global x, y, rowh
    w, h = img.size
    if x + w + 2 > AW: x = 4; y += rowh + 2; rowh = 0
    assert y + h < AH, 'atlas full'
    atlas.paste(img, (x, y)); px, py = x, y
    x += w + 2; rowh = max(rowh, h); return px, py
base = int(TLINE * 0.80)
for off, path in FONTS:
    f = ImageFont.truetype(path, 56)
    asc, desc = f.getmetrics()
    for c in list(range(32, 127)) + [ord(ch) for ch in '¡¿ÁÉÍÓÚÜÑáéíóúüñ']:   # + the accents translated text needs (atlas space is tight)
        ch = chr(c); adv = f.getlength(ch); bb = f.getbbox(ch)
        if bb[2] <= bb[0] or bb[3] <= bb[1]: text.append((off + c, 0, 0, 0, 0, 0, 0, int(round(adv)))); continue
        w, h = bb[2] - bb[0] + 2, bb[3] - bb[1] + 2
        im = Image.new('RGBA', (w, h), (255, 255, 255, 0))
        m = Image.new('L', (w, h), 0); ImageDraw.Draw(m).text((1 - bb[0], 1 - bb[1]), ch, font=f, fill=255)
        im.putalpha(m); px, py = place(im)
        text.append((off + c, px, py, w, h, bb[0] - 1, (base - asc) + bb[1] - 1, int(round(adv))))
with open(dst, 'wb') as o:
    o.write(b'IFUIFNT1'); o.write(struct.pack('<8I', AW, AH, len(text), TLINE, len(icons), ILINE, wx, wy))
    for r in text + icons: o.write(struct.pack('<I8h', *r, 0))
    o.write(atlas.tobytes())
print('text glyphs', len(text), 'icons', len(icons), 'atlas used to y', y + rowh, 'of', AH)
