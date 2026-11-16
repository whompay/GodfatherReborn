"""gf_uifont.py -- build assets/ui_font.bin ("IFUIFNT1") for the runtime overlay (rsx_ui_overlay.c) in Godfather.
Text glyphs from a bold sans TTF; icon glyphs: 0xF080 circle, 0xF081 square, 0xF082 triangle, 0xF083 cross (cut from
the game's own prompt atlas), 0xF100 SPACE keycap, 0xF101 ENTER keycap, 0xF102 ESC keycap (gf_glyphs.py style)."""
import struct, sys, os, numpy as np
from PIL import Image, ImageDraw, ImageFont
sys.path.insert(0, '.'); sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gf_tex import decode
import gf_glyphs as G

AW = AH = 1024; TLINE = 64; ILINE = 64
FONT = next((f for f in [os.environ.get('UI_FONT_TTF', ''), '/usr/share/fonts/truetype/freefont/FreeSansBold.ttf',
             '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'Fonts', 'arialbd.ttf')] if f and os.path.isfile(f)), None)
if not FONT: sys.exit('no bold sans font found; set UI_FONT_TTF=<file.ttf>')
G.FONT = FONT   # the key caps use the same font
atlas = Image.new('RGBA', (AW, AH), (0, 0, 0, 0))
recs = []
x, y, rowh = 4, 4, 0
def place(img):
    global x, y, rowh
    w, h = img.size
    if x + w + 2 > AW: x = 4; y += rowh + 2; rowh = 0
    atlas.paste(img, (x, y)); px, py = x, y
    x += w + 2; rowh = max(rowh, h)
    return px, py
# white texel for rects
white = Image.new('RGBA', (4, 4), (255, 255, 255, 255)); wx, wy = place(white); wx += 1; wy += 1
f = ImageFont.truetype(FONT, 52)
asc, desc = f.getmetrics()
base = int(TLINE * 0.80)
chars = list(range(32, 127)) + list(range(0xA0, 0x100)) + [0x2026, 0x2122]
text = []
for c in chars:
    ch = chr(c)
    adv = f.getlength(ch)
    bb = f.getbbox(ch)  # relative to origin at ascender top
    if bb[2] <= bb[0] or bb[3] <= bb[1]:
        text.append((c, 0, 0, 0, 0, 0, 0, int(round(adv)))); continue
    w, h = bb[2] - bb[0] + 2, bb[3] - bb[1] + 2
    im = Image.new('RGBA', (w, h), (255, 255, 255, 0))
    m = Image.new('L', (w, h), 0); ImageDraw.Draw(m).text((1 - bb[0], 1 - bb[1]), ch, font=f, fill=255)
    im.putalpha(m)
    px, py = place(im)
    xo = bb[0] - 1; yo = (base - asc) + bb[1] - 1
    text.append((c, px, py, w, h, xo, yo, int(round(adv))))
# icons
_, _, _, _, ga = decode(sys.argv[1])
def cell(r):
    x0, y0, x1, y1 = r; return Image.fromarray(ga[y0:y1, x0:x1], 'RGBA')
icons = []
def add_icon(code, img):
    s = ILINE / img.size[1]
    img = img.resize((max(1, int(img.size[0] * s)), ILINE), Image.LANCZOS)
    px, py = place(img)
    icons.append((code, px, py, img.size[0], img.size[1], 0, 0, img.size[0] + 4))
C = G.CELLS
add_icon(0xF080, cell(C['CIRCLE'])); add_icon(0xF081, cell(C['SQUARE'])); add_icon(0xF082, cell(C['TRIANGLE'])); add_icon(0xF083, cell(C['CROSS']))
add_icon(0xF100, G.g_key(150, 64, 'SPACE', square=False, scale=0.97))
add_icon(0xF101, G.g_key(130, 64, 'ENTER', square=False, scale=0.97))
add_icon(0xF102, G.g_key(110, 64, 'ESC', square=False, scale=0.97))
with open(sys.argv[2], 'wb') as o:
    o.write(b'IFUIFNT1'); o.write(struct.pack('<8I', AW, AH, len(text), TLINE, len(icons), ILINE, wx, wy))
    for r in text + icons: o.write(struct.pack('<I8h', r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], 0))
    o.write(atlas.tobytes())
atlas.save('uifont_preview.png')
print('glyphs', len(text), 'icons', len(icons), 'rows used to y', y + rowh)
