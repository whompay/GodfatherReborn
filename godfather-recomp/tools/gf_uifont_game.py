"""gf_uifont_game.py -- build assets/ui_font.bin ("IFUIFNT1") for the runtime overlay from the GAME'S OWN fonts.

The disc's frontend/frontend.str holds EA bitmap fonts: a FFN metrics block ("FONT", 191 Windows-1252 glyphs, 24-byte
records: code u16 LE, then big-endian w h adv xoff yoff u0 v0 u1 v1, texture coordinates in 1/16 texel) and a DXT3
texture of the same name. The overlay's sets are filled as:
  text glyphs     <- Trade Gothic LT Std 14 bold (the game's HUD / help text)
  0xF100 + c      <- Magnesium MVB Std 16 bold   (the game's menu typeface; store / inventory titles)
  0xF200 + c      <- Trade Gothic LT Std 14 bold (item descriptions)
Glyphs are scaled to the overlay's line (TLINE). The controller / key icons are made as in gf_uifont.py.
usage: python gf_uifont_game.py <USRDIR> <out ui_font.bin>"""
import os, re, struct, sys, tempfile
import numpy as np
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gf_str, gf_txd, gf_tex

AW = AH = 1024; TLINE = 32; ILINE = 64
TEXT, TITLE, DESC = 'tradegothicltstd14b', 'magnesium_mvb_std16b', 'tradegothicltstd14b'

def load_fonts(usr, names):
    src = os.path.join(usr, 'frontend', 'frontend.str')
    metrics, tex = {}, {}
    for o, u in gf_str.chunks(src):
        for m in re.finditer(rb'FONT\x00\x00', u):
            F = m.start(); nm = re.findall(rb'(\w+)\.inf', u[max(0, F - 0x60):F])
            if not nm or nm[-1].decode() not in names or nm[-1].decode() in metrics: continue
            cnt = struct.unpack('>H', u[F + 10:F + 12])[0]; t = F + struct.unpack('>I', u[F + 12:F + 16])[0]
            g = []
            for i in range(cnt):
                r = u[t + 24 * i:t + 24 * i + 20]
                c = struct.unpack('<H', r[:2])[0]
                w, h, adv, xo, yo, u0, v0, u1, v1 = struct.unpack('>HHHhhHHHH', r[2:20])
                g.append((c, w, h, adv, xo, yo, u0 // 16, v0 // 16))
            metrics[nm[-1].decode()] = g
        for m in gf_txd.PAT.finditer(u):
            s = m.end(); nm = u[s + 8:s + 40].split(b'\0')[0].decode('latin1')
            if nm not in names or nm in tex: continue
            w, h = struct.unpack_from('>HH', u[s + 72:s + 92], 8)
            raw = u[s + 92:s + 92 + w * h]                          # DXT3: one byte per texel
            p = os.path.join(tempfile.gettempdir(), 'gf_uifont_%s.tex' % nm)
            with open(p, 'wb') as f: f.write(b'GFTEX1\0\0' + struct.pack('<5I', w, h, 0x87, len(raw), 0) + raw)
            img = gf_tex.decode(p); img = img[-1] if isinstance(img, tuple) else img
            tex[nm] = np.clip(np.array(img), 0, 255).astype(np.uint8)
    miss = [n for n in names if n not in metrics or n not in tex]
    if miss: sys.exit('game fonts not found in %s: %s' % (src, ', '.join(miss)))
    return metrics, tex

atlas = Image.new('RGBA', (AW, AH), (0, 0, 0, 0))
x = y = 4; rowh = 0
def place(img):
    global x, y, rowh
    w, h = img.size
    if x + w + 2 > AW: x = 4; y += rowh + 2; rowh = 0
    if y + h >= AH: sys.exit('atlas full')
    atlas.paste(img, (x, y)); px, py = x, y
    x += w + 2; rowh = max(rowh, h); return px, py

def add_font(glyphs, texarr, off):
    a_ = next(g for g in glyphs if g[0] == ord('A'))
    k = (0.80 * TLINE) / float(a_[5] + a_[2])            # cap baseline at 80% of the line, like gf_uifont.py
    out = []
    for c, w, h, adv, xo, yo, u0, v0 in glyphs:
        uc = ord(bytes([c]).decode('cp1252', 'replace')) if c < 256 else c
        if uc == 0xFFFD: continue
        if w == 0 or h == 0: out.append((off + uc, 0, 0, 0, 0, 0, 0, int(round(adv * k)))); continue
        cell = texarr[v0:v0 + h, u0:u0 + w]
        im = Image.new('RGBA', (w, h), (255, 255, 255, 0)); im.putalpha(Image.fromarray(np.ascontiguousarray(cell[:, :, 3])))
        sw, sh = max(1, int(round(w * k))), max(1, int(round(h * k)))
        im = im.resize((sw, sh), Image.LANCZOS)
        px, py = place(im)
        out.append((off + uc, px, py, sw, sh, int(round(xo * k)), int(round(yo * k)), int(round(adv * k))))
    return out

def main(usr, outp):
    metrics, tex = load_fonts(usr, {TEXT, TITLE, DESC})
    white = Image.new('RGBA', (4, 4), (255, 255, 255, 255)); wx, wy = place(white); wx += 1; wy += 1
    text = add_font(metrics[TEXT], tex[TEXT], 0)
    text += add_font(metrics[TITLE], tex[TITLE], 0xF100)
    text += add_font(metrics[DESC], tex[DESC], 0xF200)
    # icons, as gf_uifont.py: the controller shapes from the game's prompt atlas, key caps drawn
    import gf_kbm_build as K, gf_glyphs as G
    G.FONT = K.find_font()
    w, h, raw = K.find_atlas(usr)
    p = os.path.join(tempfile.gettempdir(), 'gf_uifont_buttons.tex')
    with open(p, 'wb') as f: f.write(b'GFTEX1\0\0' + struct.pack('<5I', w, h, 0x87, len(raw), K.CSUM) + raw)
    ga = gf_tex.decode(p); ga = ga[-1] if isinstance(ga, tuple) else ga
    ga = np.clip(np.array(ga), 0, 255).astype(np.uint8)
    icons = []
    def add_icon(code, img):
        s = ILINE / img.size[1]
        img = img.resize((max(1, int(img.size[0] * s)), ILINE), Image.LANCZOS)
        px, py = place(img); icons.append((code, px, py, img.size[0], img.size[1], 0, 0, img.size[0] + 4))
    def cell(r): x0, y0, x1, y1 = r; return Image.fromarray(np.ascontiguousarray(ga[y0:y1, x0:x1]), 'RGBA')
    C = G.CELLS
    add_icon(0xF080, cell(C['CIRCLE'])); add_icon(0xF081, cell(C['SQUARE'])); add_icon(0xF082, cell(C['TRIANGLE'])); add_icon(0xF083, cell(C['CROSS']))
    add_icon(0xF100, G.g_key(150, 64, 'SPACE', square=False, scale=0.97))
    add_icon(0xF101, G.g_key(130, 64, 'ENTER', square=False, scale=0.97))
    add_icon(0xF102, G.g_key(110, 64, 'ESC', square=False, scale=0.97))
    os.makedirs(os.path.dirname(os.path.abspath(outp)), exist_ok=True)
    with open(outp, 'wb') as o:
        o.write(b'IFUIFNT1'); o.write(struct.pack('<8I', AW, AH, len(text), TLINE, len(icons), ILINE, wx, wy))
        for r in text + icons: o.write(struct.pack('<I8h', r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], 0))
        o.write(atlas.tobytes())
    if os.environ.get('UI_FONT_PREVIEW'): atlas.save(os.environ['UI_FONT_PREVIEW'])
    print('game fonts -> %s: %d text glyphs, %d icons, atlas used to y %d' % (outp, len(text), len(icons), y + rowh))

if __name__ == '__main__':
    if len(sys.argv) != 3: sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
