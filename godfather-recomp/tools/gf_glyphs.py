"""gf_glyphs.py -- build Godfather's keyboard/mouse (and new-controller) button-glyph atlas.

Input : the game's 512x256 DXT3 prompt atlas dumped by TEX_DUMP (GFTEX1 file).
Output: gf_kbm_glyphs.bin ("IFKBMIC3": u32 n, n x {w,h,fmt,nb,csum, orig, kbm, pad}) for rsx_d3d12_backend.c, and
        preview PNGs. Every glyph is redrawn inside its original rectangle, so every UV the game uses still lands
        on the matching picture. Untouched DXT blocks are copied bit-exact.
"""
import struct, sys, numpy as np
from PIL import Image, ImageDraw, ImageFont
sys.path.insert(0, '.')
from gf_tex import decode

SS = 4  # supersampling
FONT = '/usr/share/fonts/truetype/freefont/FreeSansBold.ttf'
FILL = (33, 35, 33, 255); EDGE = (0, 0, 0, 255); HI = (88, 86, 88, 255); TXT = (196, 194, 196, 255)
GOLD = (210, 170, 55, 255); GOLD_TXT = (24, 22, 18, 255)

def font(px): return ImageFont.truetype(FONT, max(6, int(px)))

def canvas(w, h): return Image.new('RGBA', (w * SS, h * SS), (0, 0, 0, 0))
def down(im, w, h): return im.resize((w, h), Image.LANCZOS)

def fit_text(d, box, text, maxh, fill, outline=None, ow=0):
    x0, y0, x1, y1 = box
    sz = maxh
    while sz > 6:
        f = font(sz); bb = d.textbbox((0, 0), text, font=f, stroke_width=ow)
        if bb[2] - bb[0] <= (x1 - x0) and bb[3] - bb[1] <= (y1 - y0): break
        sz -= 1
    f = font(sz); bb = d.textbbox((0, 0), text, font=f, stroke_width=ow)
    tx = (x0 + x1) / 2 - (bb[0] + bb[2]) / 2; ty = (y0 + y1) / 2 - (bb[1] + bb[3]) / 2
    d.text((tx, ty), text, font=f, fill=fill, stroke_width=ow, stroke_fill=outline)

def key(d, box, label, hl=False, r=None, txt_scale=0.62):
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    r = r if r is not None else min(w, h) * 0.22
    e = max(SS, int(min(w, h) * 0.07))
    d.rounded_rectangle(box, r, fill=EDGE)
    d.rounded_rectangle((x0 + e, y0 + e, x1 - e, y1 - e), max(1, r - e), fill=GOLD if hl else FILL)
    if not hl:
        d.rounded_rectangle((x0 + e, y0 + e, x1 - e, y1 - e), max(1, r - e), outline=HI, width=max(1, SS))
    if label:
        pad = e * 2.2
        fit_text(d, (x0 + pad, y0 + pad, x1 - pad, y1 - pad), label, (h - 2 * pad) * txt_scale / 0.62 * 0.62,
                 GOLD_TXT if hl else TXT)

def g_key(w, h, label, square=True, scale=0.9):
    im = canvas(w, h); d = ImageDraw.Draw(im)
    W, H = w * SS, h * SS
    if square:
        s = min(W, H) * scale; cx, cy = W / 2, H / 2
        box = (cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2)
    else:
        mx, my = W * (1 - scale) / 2, H * (1 - scale) / 2
        box = (mx, my, W - mx, H - my)
    key(d, box, label)
    return down(im, w, h)

def g_text(w, h, text):
    im = canvas(w, h); d = ImageDraw.Draw(im)
    fit_text(d, (SS, SS, w * SS - SS, h * SS - SS), text, h * SS, (184, 182, 184, 255), EDGE, max(2, int(h * SS * 0.09)))
    return down(im, w, h)

def g_wasd(w, h, hl=None):
    im = canvas(w, h); d = ImageDraw.Draw(im)
    W, H = w * SS, h * SS
    k = min(W / 3.15, H / 2.1); g = k * 0.07
    ox = (W - 3 * k - 2 * g) / 2; oy = (H - 2 * k - g) / 2
    pos = {'W': (1, 0), 'A': (0, 1), 'S': (1, 1), 'D': (2, 1)}
    for L, (cx, cy) in pos.items():
        x = ox + cx * (k + g); y = oy + cy * (k + g)
        key(d, (x, y, x + k, y + k), L, hl == L, txt_scale=0.7)
    return down(im, w, h)

def g_arrows(w, h):
    im = canvas(w, h); d = ImageDraw.Draw(im)
    W, H = w * SS, h * SS
    k = min(W / 3.15, H / 2.1); g = k * 0.07
    ox = (W - 3 * k - 2 * g) / 2; oy = (H - 2 * k - g) / 2
    for cx, cy, dirn in ((1, 0, 'u'), (0, 1, 'l'), (1, 1, 'd'), (2, 1, 'r')):
        x = ox + cx * (k + g); y = oy + cy * (k + g)
        key(d, (x, y, x + k, y + k), None)
        c = (x + k / 2, y + k / 2); s = k * 0.2
        tri = {'u': [(c[0], c[1] - s), (c[0] - s, c[1] + s * .7), (c[0] + s, c[1] + s * .7)],
               'd': [(c[0], c[1] + s), (c[0] - s, c[1] - s * .7), (c[0] + s, c[1] - s * .7)],
               'l': [(c[0] - s, c[1]), (c[0] + s * .7, c[1] - s), (c[0] + s * .7, c[1] + s)],
               'r': [(c[0] + s, c[1]), (c[0] - s * .7, c[1] - s), (c[0] - s * .7, c[1] + s)]}[dirn]
        d.polygon(tri, fill=TXT)
    return down(im, w, h)

def g_mouse(w, h, hl=None, arrow=None):
    im = canvas(w, h); d = ImageDraw.Draw(im)
    W, H = w * SS, h * SS
    mh = H * 0.92; mw = min(W * 0.62, mh * 0.66)
    if arrow in ('l', 'r'): mw = min(mw, W * 0.5)
    cx, cy = W / 2, H / 2
    if arrow == 'l': cx = W * 0.6
    if arrow == 'r': cx = W * 0.4
    if arrow == 'u': cy = H * 0.56; mh = H * 0.8
    if arrow == 'd': cy = H * 0.44; mh = H * 0.8
    x0, y0, x1, y1 = cx - mw / 2, cy - mh / 2, cx + mw / 2, cy + mh / 2
    e = max(SS, int(mw * 0.08)); r = mw / 2
    d.rounded_rectangle((x0, y0, x1, y1), r, fill=EDGE)
    d.rounded_rectangle((x0 + e, y0 + e, x1 - e, y1 - e), r - e, fill=FILL, outline=HI, width=SS)
    split = y0 + mh * 0.42
    # buttons
    if hl in ('L', 'R'):
        im2 = canvas(w, h); d2 = ImageDraw.Draw(im2)
        d2.rounded_rectangle((x0 + e, y0 + e, x1 - e, y1 - e), r - e, fill=GOLD)
        mask = Image.new('L', im.size, 0); dm = ImageDraw.Draw(mask)
        if hl == 'L': dm.rectangle((0, 0, cx, split), fill=255)
        else: dm.rectangle((cx, 0, W, split), fill=255)
        im.paste(im2, (0, 0), Image.composite(im2.getchannel('A'), Image.new('L', im.size, 0), mask))
    d.line((x0 + e, split, x1 - e, split), fill=EDGE, width=max(SS, e // 2))
    d.line((cx, y0 + e, cx, split), fill=EDGE, width=max(SS, e // 2))
    ww = mw * 0.16; wy0 = y0 + mh * 0.12; wy1 = y0 + mh * 0.32
    d.rounded_rectangle((cx - ww / 2, wy0, cx + ww / 2, wy1), ww / 2, fill=GOLD if hl == 'M' else (120, 118, 120, 255), outline=EDGE, width=max(1, SS // 2))
    if arrow:
        s = min(W, H) * 0.16
        if arrow == 'u': p = (cx, y0 - s * 0.15); tri = [(p[0], p[1] - s * .9), (p[0] - s, p[1] + s * .1), (p[0] + s, p[1] + s * .1)]
        if arrow == 'd': p = (cx, y1 + s * 0.15); tri = [(p[0], p[1] + s * .9), (p[0] - s, p[1] - s * .1), (p[0] + s, p[1] - s * .1)]
        if arrow == 'l': p = (x0 - s * 0.2, cy); tri = [(p[0] - s * .9, p[1]), (p[0] + s * .1, p[1] - s), (p[0] + s * .1, p[1] + s)]
        if arrow == 'r': p = (x1 + s * 0.2, cy); tri = [(p[0] + s * .9, p[1]), (p[0] - s * .1, p[1] - s), (p[0] - s * .1, p[1] + s)]
        d.polygon(tri, fill=GOLD, outline=EDGE)
    return down(im, w, h)

# ---------------------------------------------------------------- cells (x0, y0, x1, y1) in atlas pixels
CELLS = {
    'CROSS': (2, 2, 41, 41), 'CIRCLE': (42, 2, 81, 41), 'SQUARE': (82, 2, 121, 41), 'TRIANGLE': (122, 2, 161, 41),
    'SELECT_TXT': (0, 44, 96, 68), 'SELECT_BTN': (27, 68, 69, 92),
    'START_TXT': (256, 208, 325, 231), 'START_BTN': (266, 231, 317, 254),
    'R1': (267, 131, 316, 163), 'L1': (267, 168, 316, 200), 'R2': (318, 130, 367, 176), 'L2': (368, 130, 417, 176),
    'RS': (117, 52, 161, 96), 'LS': (117, 97, 161, 141), 'R3_A': (115, 142, 160, 185), 'RS_U': (114, 185, 161, 232),
    'LS_N': (166, 2, 211, 49), 'LS_U': (211, 2, 256, 49),
    'LS_L': (165, 53, 211, 98), 'LS_R': (211, 53, 256, 98), 'L3': (164, 116, 211, 161), 'LS_D': (211, 116, 256, 161),
    'RS_L': (162, 164, 210, 208), 'RS_R': (210, 164, 256, 208), 'R3_B': (164, 210, 209, 256), 'RS_D': (210, 211, 256, 256),
    'DPAD': (16, 186, 49, 220),
}
PICTO = {  # SIXAXIS pictograms: (rect, controller-mode replacement)
    'SX_LIFT': ((17, 109, 71, 168), 'R3'), 'SX_ROT': ((266, 29, 320, 81), 'RS'), 'SX_TILT': ((340, 54, 395, 97), 'R3'),
    'SX_RIGHT': ((428, 12, 494, 43), 'RS'), 'SX_LEFT': ((416, 67, 482, 98), 'RS'), 'SX_A': ((433, 115, 477, 140), 'RS'),
    'SX_B': ((331, 187, 385, 218), 'RS'), 'SX_C': ((426, 187, 480, 218), 'RS'),
}
KBM_CLASSIC = {  # v1130P "PC Classic" scheme (the original EA PC port's keys)
    'CROSS': lambda w, h: g_key(w, h, 'SHIFT', scale=0.86), 'CIRCLE': lambda w, h: g_key(w, h, 'C'),   # v1130E EA PC scheme
    'SQUARE': lambda w, h: g_key(w, h, 'CAPS', scale=0.86), 'TRIANGLE': lambda w, h: g_key(w, h, 'SPACE', scale=0.86),
    'SELECT_TXT': lambda w, h: g_text(w, h, 'N'), 'SELECT_BTN': lambda w, h: g_key(w, h, 'N', square=False, scale=0.95),
    'START_TXT': lambda w, h: g_text(w, h, 'ESC'), 'START_BTN': lambda w, h: g_key(w, h, 'ESC', square=False, scale=0.95),
    'R1': lambda w, h: g_mouse(w, h, 'L'), 'L1': lambda w, h: g_mouse(w, h, 'R'),
    'R2': lambda w, h: g_key(w, h, 'V/F', scale=0.86), 'L2': lambda w, h: g_key(w, h, 'ALT', scale=0.86),
    'RS': lambda w, h: g_mouse(w, h), 'LS': lambda w, h: g_wasd(w, h), 'R3_A': lambda w, h: g_key(w, h, 'F/V', scale=0.86),
    'RS_U': lambda w, h: g_mouse(w, h, arrow='u'), 'LS_N': lambda w, h: g_wasd(w, h), 'LS_U': lambda w, h: g_wasd(w, h, 'W'),
    'LS_L': lambda w, h: g_wasd(w, h, 'A'), 'LS_R': lambda w, h: g_wasd(w, h, 'D'), 'L3': lambda w, h: g_key(w, h, 'X'),
    'LS_D': lambda w, h: g_wasd(w, h, 'S'), 'RS_L': lambda w, h: g_mouse(w, h, arrow='l'), 'RS_R': lambda w, h: g_mouse(w, h, arrow='r'),
    'R3_B': lambda w, h: g_key(w, h, 'F/V', scale=0.86), 'RS_D': lambda w, h: g_mouse(w, h, arrow='d'), 'DPAD': lambda w, h: g_arrows(w, h),
}

def is_yellow(p): return p[3] > 40 and p[0] > 150 and p[1] > 110 and p[2] < 110 and p[0] - p[2] > 80

def paste_cell(out, rect, glyph):
    x0, y0, x1, y1 = rect
    out[y0:y1, x0:x1] = 0
    g = np.array(glyph, dtype=np.uint8)
    out[y0:y1, x0:x1] = g

SPLIT = {  # pictograms whose arrow is not plain yellow / overlaps the body: (arrow rows, body rows) in atlas y
    (17, 109, 71, 168): ((109, 139), (139, 168)),
    (266, 29, 320, 81): ((57, 81), (29, 59)),
}
def picto(out, orig, rect, body_img_fn):
    x0, y0, x1, y1 = rect
    cell = orig[y0:y1, x0:x1].copy()
    if rect in SPLIT:
        (ay0, ay1), (by0, by1) = SPLIT[rect]
        newcell = np.zeros_like(cell)
        arr = cell[ay0 - y0:ay1 - y0].copy()
        grey = (arr[:, :, 0] > 130) & (arr[:, :, 2] > 130)          # controller outline (white/grey), not arrow
        arr[grey] = 0
        newcell[ay0 - y0:ay1 - y0] = arr
        bh = by1 - by0; s = min(bh, x1 - x0)
        gx0 = (x1 - x0 - s) // 2; gy0 = by0 - y0 + (bh - s) // 2
        g = np.array(body_img_fn(s, s), dtype=np.uint8)
        region = newcell[gy0:gy0 + s, gx0:gx0 + s]
        a = g[:, :, 3:4].astype(np.float32) / 255.0
        region[:] = (g * a + region * (1 - a)).astype(np.uint8)
        region[:, :, 3] = np.maximum(region[:, :, 3], g[:, :, 3])
        out[y0:y1, x0:x1] = newcell
        return
    yel = np.zeros(cell.shape[:2], bool)
    for yy in range(cell.shape[0]):
        for xx in range(cell.shape[1]):
            yel[yy, xx] = is_yellow(cell[yy, xx])
    # one step of 4-neighbour dilation (was scipy.ndimage.binary_dilation; numpy only, so the installer needs no scipy)
    dil = yel.copy(); dil[1:, :] |= yel[:-1, :]; dil[:-1, :] |= yel[1:, :]; dil[:, 1:] |= yel[:, :-1]; dil[:, :-1] |= yel[:, 1:]
    yel = dil & (cell[:, :, 3] > 10)
    body = (cell[:, :, 3] > 40) & ~yel
    ys, xs = np.nonzero(body)
    if not len(xs): return
    bx0, bx1, by0, by1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    # square-ish area for the replacement, centred on the controller body
    s = min(bx1 - bx0, by1 - by0, max(bx1 - bx0, by1 - by0))
    s = int(min(max(by1 - by0, s) * 1.05, bx1 - bx0, y1 - y0))
    cx, cy = (bx0 + bx1) // 2, (by0 + by1) // 2
    gx0 = max(0, min(cell.shape[1] - s, cx - s // 2)); gy0 = max(0, min(cell.shape[0] - s, cy - s // 2))
    newcell = np.zeros_like(cell)
    newcell[yel] = cell[yel]
    g = np.array(body_img_fn(s, s), dtype=np.uint8)
    region = newcell[gy0:gy0 + s, gx0:gx0 + s]
    ga = g[:, :, 3:4].astype(np.float32) / 255.0
    keep = region[:, :, 3:4] > 0  # yellow arrow stays on top
    comp = (g.astype(np.float32) * 1.0)
    region[:] = np.where(keep, region, comp.astype(np.uint8))
    out[y0:y1, x0:x1] = newcell

# ---------------------------------------------------------------- DXT3
def enc_color_block(px):  # px: 16x4 uint8 (RGBA)
    rgb = px[:, :3].astype(np.float32); a = px[:, 3].astype(np.float32)
    wgt = (a > 8).astype(np.float32) + 1e-3
    mean = (rgb * wgt[:, None]).sum(0) / wgt.sum()
    cov = ((rgb - mean) * wgt[:, None]).T @ (rgb - mean)
    try:
        ev, evec = np.linalg.eigh(cov); axis = evec[:, -1]
    except Exception: axis = np.array([1, 1, 1.0]) / 3 ** .5
    proj = (rgb - mean) @ axis
    sel = wgt > 0.5
    if sel.any(): lo, hi = proj[sel].min(), proj[sel].max()
    else: lo, hi = proj.min(), proj.max()
    c0 = np.clip(mean + axis * hi, 0, 255); c1 = np.clip(mean + axis * lo, 0, 255)
    def q(c): return (int(round(c[0] * 31 / 255)) << 11) | (int(round(c[1] * 63 / 255)) << 5) | int(round(c[2] * 31 / 255))
    def dq(v): return np.array([((v >> 11) & 31) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31])
    q0, q1 = q(c0), q(c1)
    if q0 < q1: q0, q1 = q1, q0
    if q0 == q1:
        if q0 > 0: q1 = q0 - 1
        else: q0 = 1
    p0, p1 = dq(q0), dq(q1)
    pal = np.array([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3])
    idx = np.argmin(((rgb[:, None, :] - pal[None]) ** 2).sum(2), 1)
    bits = 0
    for i in range(16): bits |= int(idx[i]) << (2 * i)
    return struct.pack('<HHI', q0, q1, bits)

def enc_dxt3(img, orig_img, orig_bytes):
    h, w = img.shape[:2]; out = bytearray(orig_bytes); p = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            blk = img[by:by + 4, bx:bx + 4].reshape(16, 4)
            if not np.array_equal(blk, orig_img[by:by + 4, bx:bx + 4].reshape(16, 4)):
                al = bytearray(8)
                for i in range(16):
                    v = (int(blk[i, 3]) * 15 + 127) // 255
                    al[i // 2] |= v << (4 * (i & 1))
                out[p:p + 16] = bytes(al) + enc_color_block(blk)
            p += 16
    return bytes(out)

KBM = dict(KBM_CLASSIC)   # v1130P "Reborn" scheme (the port's own keys; the default)
KBM.update({
    'CROSS': lambda w, h: g_key(w, h, 'SPACE', scale=0.86), 'SQUARE': lambda w, h: g_key(w, h, 'F'), 'TRIANGLE': lambda w, h: g_key(w, h, 'E'),
    'SELECT_TXT': lambda w, h: g_text(w, h, 'TAB'), 'SELECT_BTN': lambda w, h: g_key(w, h, 'TAB', square=False, scale=0.95),
    'R2': lambda w, h: g_key(w, h, 'R'), 'L2': lambda w, h: g_key(w, h, 'Q'), 'R3_A': lambda w, h: g_mouse(w, h, 'M'),
    'L3': lambda w, h: g_key(w, h, 'SHIFT', scale=0.86), 'R3_B': lambda w, h: g_mouse(w, h, 'M'),
})
SCHEME = 'reborn'   # 'classic' -> KBM_CLASSIC + PICTO_KBM (gf_kbm_build.py builds both)

PICTO_KBM = {  # v1130J: the key each SIXAXIS pictogram stands for in the EA-style KBM scheme
    'SX_LIFT': lambda W, H: g_key(W, H, 'W', scale=0.92), 'SX_ROT': lambda W, H: g_key(W, H, 'Y', scale=0.92),
    'SX_TILT': lambda W, H: g_key(W, H, 'SPACE', scale=0.92), 'SX_RIGHT': lambda W, H: g_key(W, H, 'D', scale=0.92),
    'SX_LEFT': lambda W, H: g_key(W, H, 'A', scale=0.92), 'SX_A': lambda W, H: g_key(W, H, 'S', scale=0.92),
    'SX_B': lambda W, H: g_key(W, H, 'CTRL', scale=0.92), 'SX_C': lambda W, H: g_mouse(W, H, arrow='d'),
}

def main(src, outbin):
    d = open(src, 'rb').read(); w, h, fmt, nb, cs = struct.unpack('<5I', d[8:28]); raw = d[28:28 + nb]
    _, _, _, _, orig = decode(src)
    kbm = orig.copy(); pad = orig.copy()
    for name, rect in CELLS.items():
        x0, y0, x1, y1 = rect
        paste_cell(kbm, rect, (KBM_CLASSIC if SCHEME == 'classic' else KBM)[name](x1 - x0, y1 - y0))
    rs_rect, r3_rect = CELLS['RS'], CELLS['R3_B']
    def crop_fn(rect):
        x0, y0, x1, y1 = rect; im = Image.fromarray(orig[y0:y1, x0:x1], 'RGBA')
        return lambda W, H: im.resize((W, H), Image.LANCZOS)
    for name, (rect, padg) in PICTO.items():
        picto(kbm, orig, rect, PICTO_KBM[name] if SCHEME == 'classic' else (lambda W, H: g_key(W, H, 'X', scale=0.92)))   # v1130J / v1130P
        picto(pad, orig, rect, crop_fn(rs_rect if padg == 'RS' else r3_rect))
    kb = enc_dxt3(kbm, orig, raw); pb = enc_dxt3(pad, orig, raw)
    assert len(kb) == nb and len(pb) == nb
    with open(outbin, 'wb') as f:
        f.write(b'IFKBMIC3'); f.write(struct.pack('<I', 1))
        f.write(struct.pack('<5I', w, h, fmt, nb, cs)); f.write(raw); f.write(kb); f.write(pb)
    # previews (decoded from the encoded bytes, i.e. what the game will show)
    for tag, data in (('orig', raw), ('kbm', kb), ('pad', pb)):
        tmp = 'prev_%s.tex' % tag
        open(tmp, 'wb').write(d[:28] + data)
        _, _, _, _, im = decode(tmp)
        bg = Image.new('RGBA', (w, h), (95, 95, 95, 255)); bg.alpha_composite(Image.fromarray(im, 'RGBA'))
        bg.convert('RGB').resize((w * 2, h * 2), Image.NEAREST).save('preview_%s.png' % tag)
    print('wrote', outbin, 'csum %08X' % cs)

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
