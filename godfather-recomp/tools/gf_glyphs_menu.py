"""gf_glyphs_menu.py -- v1107Z: add a 4th variant to gf_kbm_glyphs.bin: the keyboard/mouse atlas with L1 / R1 drawn as
Q / E keys (in menus Q / E switch tabs). Input: the IFKBMIC3 file; output: IFKBMIC4 (n x {w,h,fmt,nb,csum, orig, kbm,
pad, kbm_menu}). rsx_d3d12_backend.c shows kbm_menu while cellPad.c's g_gf_menu_glyphs is set.
Usage: python3 gf_glyphs_menu.py ../assets/gf_kbm_glyphs.bin ../assets/gf_kbm_glyphs.bin [font.ttf]"""
import os, struct, sys, tempfile
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gf_glyphs as G
from gf_tex import decode

def main(src, dst, font=None):
    if font: G.FONT = font
    d = open(src, 'rb').read()
    assert d[:8] in (b'IFKBMIC3', b'IFKBMIC4'), d[:8]
    has4 = d[:8] == b'IFKBMIC4'
    n = struct.unpack('<I', d[8:12])[0]; p = 12; ents = []
    for _ in range(n):
        w, h, fmt, nb, cs = struct.unpack('<5I', d[p:p + 20]); p += 20
        orig = d[p:p + nb]; kbm = d[p + nb:p + 2 * nb]; pad = d[p + 2 * nb:p + 3 * nb]; p += 3 * nb
        if has4: p += nb
        tmp = os.path.join(tempfile.gettempdir(), 'gfkbm.tex')
        open(tmp, 'wb').write(b'GFTEX1\0\0' + struct.pack('<5I', w, h, fmt, nb, cs) + kbm)
        _, _, _, _, img = decode(tmp)
        img = np.clip(img, 0, 255).astype(np.uint8); menu = img.copy()
        for name, label in (('L1', 'Q'), ('R1', 'E')) + ((('CROSS', 'ENTER'),) if G.SCHEME == 'classic' else ()):   # v1130E: PC Classic menus confirm with Enter
            x0, y0, x1, y1 = G.CELLS[name]
            G.paste_cell(menu, G.CELLS[name], G.g_key(x1 - x0, y1 - y0, label))
        if G.SCHEME == 'classic':   # v1130Q: no d-pad buttons in PC Classic menus / the weapon dealer's buy screen (mouse)
            x0, y0, x1, y1 = G.CELLS['DPAD']; menu[y0:y1, x0:x1] = 0
        mb = G.enc_dxt3(menu, img, kbm)
        assert len(mb) == nb
        ents.append((w, h, fmt, nb, cs, orig, kbm, pad, mb))
        open(tmp, 'wb').write(b'GFTEX1\0\0' + struct.pack('<5I', w, h, fmt, nb, cs) + mb)
        _, _, _, _, im = decode(tmp)
        from PIL import Image
        bg = Image.new('RGBA', (w, h), (95, 95, 95, 255)); bg.alpha_composite(Image.fromarray(np.clip(im, 0, 255).astype(np.uint8), 'RGBA'))
        bg.convert('RGB').resize((w * 2, h * 2), Image.NEAREST).save(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gf_glyphs_preview_kbm_menu.png'))
    with open(dst, 'wb') as f:
        f.write(b'IFKBMIC4'); f.write(struct.pack('<I', len(ents)))
        for e in ents:
            f.write(struct.pack('<5I', *e[:5])); f.write(e[5]); f.write(e[6]); f.write(e[7]); f.write(e[8])
    print('wrote', dst, len(ents), 'entries')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
