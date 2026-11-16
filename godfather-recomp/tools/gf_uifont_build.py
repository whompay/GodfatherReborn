"""Build assets/ui_font.bin (the runtime overlay's font: menus, store, dialogue, trainer, GPS hint) from the game files
on any OS: finds the button prompt atlas like gf_kbm_build.py, then runs gf_uifont.py (base atlas + prompt icons),
gf_uifont_text.py (condensed text glyphs) and gf_uifont_serif.py (store serif glyphs) with fonts found on this machine.
Fonts: the URW/FreeSans ones on Linux; on Windows Arial Bold, Bahnschrift Bold Condensed and Palatino Linotype.
usage: python tools/gf_uifont_build.py <game USRDIR> <out ui_font.bin>"""
import os, runpy, struct, sys, tempfile
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, here)
from PIL import ImageFont
import gf_kbm_build

WIN = os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'Fonts')
def pick(*cands):
    for c in cands:
        if c and os.path.isfile(c.split('|')[0]): return c
    return None
BASE = pick('/usr/share/fonts/truetype/freefont/FreeSansBold.ttf', os.path.join(WIN, 'arialbd.ttf'),
            '/System/Library/Fonts/Supplemental/Arial Bold.ttf')
TEXT = pick('/usr/share/fonts/opentype/urw-base35/NimbusSansNarrow-Bold.otf',
            os.path.join(WIN, 'bahnschrift.ttf') + '|Bold Condensed', os.path.join(WIN, 'arialbd.ttf'))
SERIF_B = pick('/usr/share/fonts/opentype/urw-base35/P052-Bold.otf', os.path.join(WIN, 'palab.ttf'))
SERIF_I = pick('/usr/share/fonts/opentype/urw-base35/P052-Italic.otf', os.path.join(WIN, 'palai.ttf'))

_truetype = ImageFont.truetype
def _tt(path, size=10, *a, **k):
    """ImageFont.truetype that maps the tools' Linux font paths to the fonts picked above."""
    p = str(path)
    if 'FreeSansBold' in p: p = BASE
    elif 'NimbusSansNarrow' in p: p = TEXT
    elif 'P052-Bold' in p: p = SERIF_B
    elif 'P052-Italic' in p: p = SERIF_I
    file, _, var = p.partition('|')
    f = _truetype(file, size, *a, **k)
    if var:
        f.set_variation_by_name(var)
    return f

def run(script, *args):
    sys.argv = [script] + list(args)
    runpy.run_path(os.path.join(here, script), run_name='__main__')

def add_key_icons(src, tex, dst):
    """Append prompt icons for the mods' own hints: 0xF084 = the game's D-pad picture, 0xF300 + c = a key cap with the
    letter / digit c (A-Z, 0-9) in the keyboard prompt style of gf_glyphs.py."""
    from PIL import Image
    import gf_glyphs as G
    from gf_tex import decode
    d = open(src, 'rb').read(); assert d[:8] == b'IFUIFNT1'
    AW, AH, nt, TLINE, ni, ILINE, wx, wy = struct.unpack('<8I', d[8:40])
    recs = [struct.unpack('<I8h', d[40 + 20 * i:60 + 20 * i]) for i in range(nt + ni)]
    atlas = Image.frombytes('RGBA', (AW, AH), d[40 + 20 * (nt + ni):40 + 20 * (nt + ni) + AW * AH * 4])
    text, icons = [r[:8] for r in recs[:nt]], [r[:8] for r in recs[nt:] if not (r[0] == 0xF084 or 0xF300 <= r[0] < 0xF380)]
    x, y, rowh = 4, max(r[2] + r[4] for r in text + icons) + 6, 0
    def add(code, img):
        nonlocal x, y, rowh
        s = ILINE / img.size[1]
        img = img.resize((max(1, int(img.size[0] * s)), ILINE), Image.LANCZOS)
        w, h = img.size
        if x + w + 2 > AW: x = 4; y += rowh + 2; rowh = 0
        assert y + h < AH, 'atlas full'
        atlas.paste(img, (x, y)); icons.append((code, x, y, w, h, 0, 0, w + 4))
        x += w + 2; rowh = max(rowh, h)
    _, _, _, _, ga = decode(tex)
    x0, y0, x1, y1 = G.CELLS['DPAD']
    dp = Image.fromarray(ga[y0:y1, x0:x1].copy(), 'RGBA').copy(); px = dp.load()
    W, H = dp.size
    # the right arm in red ("D-pad right"): the four arms are separate shapes, so take the solid shape whose centre
    # lies furthest right, grown by one pixel into its own anti-aliased edge
    comp, best, best_cx = {}, None, -1.0
    for j in range(H):
        for i in range(W):
            if px[i, j][3] < 128 or (i, j) in comp: continue
            todo, cells = [(i, j)], []
            comp[(i, j)] = len(cells)
            while todo:
                a, b = todo.pop(); cells.append((a, b))
                for n in ((a + 1, b), (a - 1, b), (a, b + 1), (a, b - 1)):
                    if 0 <= n[0] < W and 0 <= n[1] < H and n not in comp and px[n][3] >= 128:
                        comp[n] = 1; todo.append(n)
            cx = sum(c[0] for c in cells) / len(cells)
            if len(cells) > 8 and cx > best_cx: best, best_cx = cells, cx
    arm = set(best or [])
    edge = {(a + da, b + db) for a, b in arm for da in (-1, 0, 1) for db in (-1, 0, 1)}
    for (i, j) in edge:
        if 0 <= i < W and 0 <= j < H and px[i, j][3] > 0 and ((i, j) in arm or px[i, j][3] < 128):
            r, g, b, a = px[i, j]
            v = (r + g + b) / 765.0                   # keep the arm's shading
            px[i, j] = (int(150 + 105 * v), int(20 + 40 * v), int(20 + 40 * v), a)
    add(0xF084, dp)
    for c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789':
        add(0xF300 + ord(c), G.g_key(64, 64, c))
    with open(dst, 'wb') as o:
        o.write(b'IFUIFNT1'); o.write(struct.pack('<8I', AW, AH, len(text), TLINE, len(icons), ILINE, wx, wy))
        for r in text + icons: o.write(struct.pack('<I8h', *r, 0))
        o.write(atlas.tobytes())

def main(usr, out):
    if os.environ.get('UI_FONT_GAME', '') not in ('', '0'):       # opt-in: the game's own fonts (gf_uifont_game.py)
        try:
            import gf_uifont_game; gf_uifont_game.main(usr, os.path.abspath(out)); return
        except (Exception, SystemExit) as e:
            print('game fonts not usable (%s) -- using installed fonts' % e)
    if not (BASE and TEXT and SERIF_B and SERIF_I):
        sys.exit("ui font: a needed font was not found (bold sans, condensed sans, serif bold and italic)")
    out = os.path.abspath(out)
    w, h, raw = gf_kbm_build.find_atlas(usr)
    ImageFont.truetype = _tt
    work = tempfile.mkdtemp(prefix='gf_uifont_'); cwd = os.getcwd(); os.chdir(work)
    try:
        with open('buttons.tex', 'wb') as f:
            f.write(b'GFTEX1\0\0' + struct.pack('<5I', w, h, 0x87, len(raw), gf_kbm_build.CSUM) + raw)
        run('gf_uifont.py', 'buttons.tex', 'base.bin')
        run('gf_uifont_text.py', 'base.bin', 'text.bin', TEXT)
        run('gf_uifont_serif.py', 'text.bin', 'serif.bin')
        os.makedirs(os.path.dirname(out), exist_ok=True)
        add_key_icons('serif.bin', 'buttons.tex', out)
    finally:
        os.chdir(cwd); ImageFont.truetype = _truetype
    print("ui font (%s / %s / %s) -> %s" % (tuple(os.path.basename(x.split("|")[0]) for x in (BASE, TEXT, SERIF_B)) + (out,)))

if __name__ == '__main__':
    if len(sys.argv) != 3: sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
