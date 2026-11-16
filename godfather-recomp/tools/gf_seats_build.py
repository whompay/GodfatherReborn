"""v1109C: build assets/gf_seats.txt (sittable furniture) from the level files, reusing gf_collision_build.scan_file.
Every MMdl instance (nested MetaModelPart blocks walked like the collision builder) whose INSTANCE name or referenced ASSET
name (asset .meta.xml header / Havok Neo| name) looks like seating -> one line:
  kind x y z ax az hx hz top name
kind = bench | chair | stool | sofa; x y z = world position of the instance origin; (ax, az) = the instance's local +X axis in
world XZ (unit); hx hz = half extents of its collision box along local X / Z (0 = unknown); top = collision top above the origin
(0 = unknown); name = instance / asset name. World coords = gf_collision.bin / ragdoll world.
usage: python tools/gf_seats_build.py scan <USRDIR/godfather_v4> <i0> <i1> <part.pkl>   (batches, e.g. 0 600 / 600 1200 / 1200 9999)
       python tools/gf_seats_build.py <USRDIR/godfather_v4> <out.txt> <part.pkl>..."""
import sys, os, re, glob, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gf_collision_build as cb
PAT = re.compile(r'(park)?bench|chair|stool|sofa|couch|settee|\bpew|^pew|_pew|booth_seat|armchair', re.I)
NOT = re.compile(r'spawn|reactivate|wheelchair_ramp|phonebooth|des_.*crate', re.I)
def kind_of(n):
    n = n.lower()
    if 'bench' in n or 'pew' in n: return 'bench'
    if 'stool' in n: return 'stool'
    if 'sofa' in n or 'couch' in n or 'settee' in n: return 'sofa'
    return 'chair'
def box(T):
    P = T.reshape(-1, 3); return (P.min(0), P.max(0))
MODEL = re.compile(rb'\\export\\([\w\-]+)\\lodmodel', re.I)
def scan_small(f):
    c_, h_, n_, d_, b_, r_ = cb.scan_file(f)
    import gf_str, gf_mmdl
    for o, u in gf_str.chunks(f):   # render models: EARS_MESH header path ...\export\<model>\lodmodelN.rws
        for h, off in gf_mmdl.asset_offsets(u).items():
            if h in h_: continue
            m = MODEL.search(u[off:off + 0x400])
            if m: h_[h] = m.group(1).decode()
    return ({k: box(v) for k, v in c_.items()}, h_, {k: box(v[0]) for k, v in n_.items()}, b_, [(h, inst) for ff, h, inst in r_])
def files_of(root):
    files = []
    for r in root.split(';'): files += sorted(glob.glob(os.path.join(glob.escape(r), '**', '*.str'), recursive=True))
    return [f for f in files if 'world_terrain' in f.replace('\\', '/') or os.path.basename(f).endswith('_global.str')]
def scan_batch(root, i0, i1, outp):
    import pickle
    from multiprocessing import Pool
    files = files_of(root)[i0:i1]
    with Pool(max(1, os.cpu_count() or 2)) as pool: res = pool.map(scan_small, files, chunksize=4)
    pickle.dump(res, open(outp, 'wb')); print(len(files), 'files ->', outp)
def main(root, outp, parts):
    import pickle
    col = {}; neo = {}; blocks = {}; roots = []; hdr = {}
    for pp in parts:
        for c_, h_, n_, b_, r_ in pickle.load(open(pp, 'rb')):
            for k, v in c_.items(): col.setdefault(k, v)
            for k, v in h_.items(): hdr.setdefault(k, v)
            for k, v in n_.items(): neo.setdefault(k, v)
            for k, v in b_.items(): blocks.setdefault(k, v)
            roots += r_
    nested = set()
    for h, inst in roots:
        for nm, tn, attrs in inst:
            if tn == 'MetaModelPart':
                for a, v in attrs:
                    if a == 'fileName' and isinstance(v, int): nested.add(v)
    out = []; seen = set()
    def place(inst, M, depth, parent):
        if depth > 6: return
        for nm, tn, attrs in inst:
            X = next((v for a, v in attrs if a == 'XFORM'), None)
            Mi = X @ M if X is not None else M
            refs = [v for a, v in attrs if isinstance(v, int)]
            if tn == 'MetaModelPart':
                for r in refs:
                    if r in blocks: place(blocks[r], Mi, depth + 1, nm or parent)
                continue
            names = [nm] + [hdr.get(r, '') for r in refs]
            hit = next((n for n in names if n and PAT.search(n) and not NOT.search(n)), None)
            if not hit: continue
            B = None
            for r in refs:
                B = col.get(r)
                if B is None and r in hdr and hdr[r] in neo: B = neo[hdr[r]]
                if B is not None: break
            hx = hz = top = 0.0
            if B is not None:
                mn, mx = B
                if np.all(mx - mn < 8.0): hx, hz, top = (mx[0] - mn[0]) / 2, (mx[2] - mn[2]) / 2, mx[1]
            p = Mi[3, :3]; ax = Mi[0, :3]; l = np.hypot(ax[0], ax[2])
            if not np.all(np.isfinite(p)) or l < 1e-6 or np.abs(p).max() > 20000: continue
            key = (round(p[0], 1), round(p[1], 1), round(p[2], 1))
            if key in seen: continue
            seen.add(key)
            out.append((kind_of(hit), p[0], p[1], p[2], ax[0] / l, ax[2] / l, hx, hz, top, hit))
    for h, inst in roots:
        if h in nested: continue
        place(inst, np.eye(4), 0, '')
    with open(outp, 'w') as f:
        f.write('# v1109C gf_seats_build.py: kind x y z ax az hx hz top name\n')
        for o in out: f.write('%s %.3f %.3f %.3f %.4f %.4f %.3f %.3f %.3f %s\n' % o)
    from collections import Counter
    print(len(out), 'seats', Counter(o[0] for o in out).most_common(), Counter(o[9] for o in out).most_common(25))
if __name__ == '__main__':
    if sys.argv[1] == 'scan': scan_batch(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5])
    else: main(sys.argv[1], sys.argv[2], sys.argv[3:])
