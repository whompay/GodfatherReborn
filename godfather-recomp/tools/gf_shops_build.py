"""v1110S: build assets/gf_shops.txt -- the real world box of every business interior (shop) on the disc.
For each interior folder / file under world_terrain whose name classifies as a store (same rules as gf_shop.c classify), every
placed CollisionPart / BuildingCollisionPart box is transformed to world space (gf_seats_build.scan_small placement walk) and
unioned. Parts at the exact origin (unplaced / interior-local) are skipped. One line per interior:
  name x0 y0 z0 x1 y1 z1 parts
usage: python tools/gf_shops_build.py <world_terrain> <out.txt> [i0 i1]   (batches append to out.txt when i0 > 0)"""
import sys, os, glob, re, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '_tmp', 'shop'))
import gf_seats_build as sb
SHOP = re.compile(r'smoke|quack|bakery|florist|butcher|liquor|liqour|bar_|nightclub|^rest|_rest', re.I)
def interiors(wt):
    seen = {}
    for f in sorted(glob.glob(os.path.join(glob.escape(wt), '**', '*.str'), recursive=True)):
        rel = os.path.relpath(f, wt).replace('\\', '/').split('/')
        if len(rel) < 3: continue
        leaf = rel[2][:-4] if rel[2].endswith('.str') else rel[2]
        leaf = re.sub(r'_z0.*$', '', leaf)
        if not SHOP.search(leaf): continue
        seen.setdefault(leaf, []).append(f)
    return sorted(seen.items())
def box_of(files):
    col = {}; hdr = {}; neo = {}; blocks = {}; roots = []
    for f in files:
        c_, h_, n_, b_, r_ = sb.scan_small(f)
        col.update(c_); hdr.update(h_); neo.update(n_); blocks.update(b_); roots += r_
    nested = set()
    for h, inst in roots:
        for nm, tn, attrs in inst:
            if tn == 'MetaModelPart':
                for a, v in attrs:
                    if a == 'fileName' and isinstance(v, int): nested.add(v)
    lo = np.full(3, 1e9); hi = np.full(3, -1e9); n = [0]
    def place(inst, M, d):
        if d > 6: return
        for nm, tn, attrs in inst:
            X = next((v for a, v in attrs if a == 'XFORM'), None); Mi = X @ M if X is not None else M
            refs = [v for a, v in attrs if isinstance(v, int)]
            if tn == 'MetaModelPart':
                for r in refs:
                    if r in blocks: place(blocks[r], Mi, d + 1)
                continue
            if 'Collision' not in tn: continue
            if np.abs(Mi[3, :3]).max() < 1e-3: continue          # unplaced / local
            B = None
            for r in refs:
                B = col.get(r)
                if B is None and r in hdr and hdr[r] in neo: B = neo[hdr[r]]
                if B is not None: break
            if B is None: continue
            mn, mx = B
            if np.any(mx - mn > 80.0): continue
            for cx in (mn[0], mx[0]):
                for cy in (mn[1], mx[1]):
                    for cz in (mn[2], mx[2]):
                        w = np.array([cx, cy, cz, 1.0]) @ Mi
                        lo[:] = np.minimum(lo, w[:3]); hi[:] = np.maximum(hi, w[:3])
            n[0] += 1
    for h, inst in roots:
        if h in nested: continue
        place(inst, np.eye(4), 0)
    return lo, hi, n[0]
if __name__ == '__main__':
    wt, outp = sys.argv[1], sys.argv[2]
    # one world_terrain folder, several separated by ';', or the godfather_v4 folder (all its */world_terrain)
    roots = [r for r in wt.split(';') if r]
    if len(roots) == 1 and os.path.basename(os.path.normpath(roots[0])) != 'world_terrain':
        roots = sorted(glob.glob(os.path.join(glob.escape(roots[0]), '*', 'world_terrain'))) or roots
    merged = {}
    for r in roots:
        for leaf, files in interiors(r): merged.setdefault(leaf, []).extend(files)
    items = sorted(merged.items())
    i0, i1 = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) > 4 else (0, len(items))
    with open(outp, 'a' if i0 > 0 else 'w') as f:
        if i0 == 0: f.write('# v1110S tools/gf_shops_build.py: name x0 y0 z0 x1 y1 z1 parts (world box of the interior collision)\n')
        for leaf, files in items[i0:i1]:
            lo, hi, n = box_of(files)
            if n: f.write('%s %.2f %.2f %.2f %.2f %.2f %.2f %d\n' % (leaf, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], n))
            print(leaf, len(files), 'files', n, 'parts', lo.round(1), hi.round(1), flush=True)
    print(len(items), 'interiors total')
