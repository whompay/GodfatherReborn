"""Build assets/gf_kbm_glyphs.bin (keyboard/mouse + new-controller button prompts, rsx_d3d12_backend.c) straight from
the game files. All of Godfather's button prompts come from one 512x256 DXT3 texture, "buttons" in
godfather_v4/godfather_global_global.str (guest checksum 0x6DAE69FE). This finds it, then runs tools/gf_glyphs.py
(IFKBMIC3: original + keyboard/mouse + controller) and tools/gf_glyphs_menu.py (IFKBMIC4: + the menu variant).
Key caps are drawn with a bold sans font: KBM_FONT=<file.ttf>, else FreeSans Bold / DejaVu Sans Bold / Arial Bold.
usage: python tools/gf_kbm_build.py <game USRDIR> <out.bin>"""
import os, struct, sys, tempfile
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, here)
import gf_str, gf_txd

CSUM = 0x6DAE69FE
FONTS = ['/usr/share/fonts/truetype/freefont/FreeSansBold.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf',
         os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'Fonts', 'arialbd.ttf'),
         '/System/Library/Fonts/Supplemental/Arial Bold.ttf']

def find_font():
    f = os.environ.get('KBM_FONT')
    if f:
        if not os.path.isfile(f): sys.exit(f"KBM_FONT={f} does not exist")
        return f
    for f in FONTS:
        if os.path.isfile(f): return f
    sys.exit("no bold sans font found; set KBM_FONT=<file.ttf>")

def find_atlas(usr):
    src = os.path.join(usr, 'godfather_v4', 'godfather_global_global.str')
    for o, u in gf_str.chunks(src):
        for name, w, h, fmt, hd, data in gf_txd.textures(u):
            if (w, h) != (512, 256): continue
            s = u.find(data[:64]) if len(data) >= 64 else -1
            raw = u[s:s + w * h] if s >= 0 else b''     # DXT3 level 0 = one byte per pixel
            if len(raw) == w * h and gf_txd.csum(raw) == CSUM:
                return w, h, raw
    sys.exit(f"the button prompt atlas (512x256 DXT3, checksum {CSUM:08X}) was not found in {src}")

def main(usr, out):
    out = os.path.abspath(out)
    w, h, raw = find_atlas(usr)
    font = find_font()
    work = tempfile.mkdtemp(prefix='gf_kbm_'); cwd = os.getcwd(); os.chdir(work)   # the glyph tools write previews to cwd
    try:
        with open('buttons.tex', 'wb') as f: f.write(b'GFTEX1\0\0' + struct.pack('<5I', w, h, 0x87, len(raw), CSUM) + raw)
        import gf_glyphs as G, gf_glyphs_menu as M
        G.FONT = font
        os.makedirs(os.path.dirname(out), exist_ok=True)
        # v1130P one atlas per keyboard scheme: <out> = "Reborn", <out>_classic.bin = "PC Classic"
        root, ext = os.path.splitext(out)
        for scheme, path in (('reborn', out), ('classic', root + '_classic' + ext)):
            G.SCHEME = scheme
            G.main('buttons.tex', path)
            M.main(path, path, font)
    finally:
        os.chdir(cwd)
    print(f"button prompts ({os.path.basename(font)}) -> {out} (+ _classic)")

if __name__ == '__main__':
    if len(sys.argv) != 3: sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
