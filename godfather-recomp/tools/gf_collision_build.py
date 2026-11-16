"""v1057C: build assets/gf_collision.bin (world collision for the ragdolls) from the level files.
Sources (2026-11-04 RE, see HANDOFF): Havok 4.1 packfiles (EAStorageMeshShape / hkSimpleMeshShape meshes, hkBoxShape,
hkConvexVerticesShape -> its AABB) and EA MMdl scene blocks (tools/gf_mmdl.py): CollisionPart / BuildingCollisionPart
(CollisionModel ref), Destructible (CMD_SetModel ref -> the prop's collision), MetaModelPart (nested block) -- each with
an optional XFORM (row-vector 4x4). Assets are matched by the hash triple in the header just before them.
Output "GFCOL1\\0\\0", f32 cell, f32 x0, f32 z0, i32 nx, i32 nz, i32 ntri, i32 nidx, then ntri*9 f32 (world tris),
nx*nz*(i32 start, i32 count) into the index list, nidx i32. Cells are XZ squares; a tri is listed in every cell its
XZ bounds touch.
usage: python tools/gf_collision_build.py <USRDIR/godfather_v4> <out.bin>"""
import sys, os, re, struct, glob, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import gf_str, gf_mmdl, gf_coll
CELL = 16.0
def box_tris(c, h):
    x0, y0, z0 = c - h; x1, y1, z1 = c + h
    P = np.array([[x0,y0,z0],[x1,y0,z0],[x1,y1,z0],[x0,y1,z0],[x0,y0,z1],[x1,y0,z1],[x1,y1,z1],[x0,y1,z1]])
    F = [(0,1,2),(0,2,3),(4,6,5),(4,7,6),(0,4,5),(0,5,1),(3,2,6),(3,6,7),(0,3,7),(0,7,4),(1,5,6),(1,6,2)]
    return P[np.array(F)]
def shapes_of(data, loc, objs):
    """triangles (N,3,3) of one packfile in its asset frame"""
    out = []
    rbs = [o for o, c in objs if c == 'hkRigidBody']
    for off, cls in objs:
        T = None
        if cls in ('EAStorageMeshShape', 'hkSimpleMeshShape'):
            fx = sorted(k for k in loc if off <= k < off + 0x40)
            if len(fx) >= 2:
                vp, nv = loc[fx[0]], gf_mmdl.be(data, fx[0] + 4); tp, nt = loc[fx[1]], gf_mmdl.be(data, fx[1] + 4)
                if 0 < nv < 300000 and 0 < nt < 600000 and vp + nv * 16 <= len(data) and tp + nt * 12 <= len(data):
                    V = np.frombuffer(data, '>f4', nv * 4, vp).reshape(nv, 4)[:, :3].astype(np.float64)
                    I = np.frombuffer(data, '>i4', nt * 3, tp).reshape(nt, 3)
                    if I.min() >= 0 and I.max() < nv: T = V[I]
        elif cls == 'hkBoxShape':
            h = np.frombuffer(data, '>f4', 3, off + 0x10).astype(np.float64)
            if np.all(h > 0.005) and np.all(h < 100): T = box_tris(np.zeros(3), h)
        elif cls == 'hkConvexVerticesShape':
            h = np.frombuffer(data, '>f4', 3, off + 0x10).astype(np.float64); c = np.frombuffer(data, '>f4', 3, off + 0x20).astype(np.float64)
            if np.all(h > 0.005) and np.all(h < 100) and np.all(np.abs(c) < 100): T = box_tris(c, h)
        if T is None or not len(T): continue
        rb = max([r for r in rbs if r < off], default=None)   # the body that owns this shape
        if rb is not None and rb + 0xF0 <= len(data):
            R = np.frombuffer(data, '>f4', 12, rb + 0xb0).reshape(3, 4)[:, :3].astype(np.float64)   # columns
            t = np.frombuffer(data, '>f4', 3, rb + 0xe0).astype(np.float64)
            if np.all(np.isfinite(R)) and abs(abs(np.linalg.det(R)) - 1) < 0.05 and np.all(np.abs(t) < 5000):
                T = T @ R + t   # p' = R p + t with R stored as columns -> row form p @ R^T^T
        out.append(T)
    return np.concatenate(out) if out else None
def scan_file(f):
    import bisect
    col = {}; hdr_name = {}; neo = {}; direct = []; blocks = {}; roots = []
    for o, u in gf_str.chunks(f):
        ao = gf_mmdl.asset_offsets(u)
        heads = sorted((off, h) for h, off in ao.items()); hoffs = [x[0] for x in heads]
        def owner(pos):
            i = bisect.bisect_right(hoffs, pos) - 1
            return heads[i][1] if i >= 0 and pos - heads[i][0] < 0x600 else None
        for h, off in ao.items():
            m = re.search(rb'([\w\-\.]{3,})\.meta\.xml', u[off:off + 0x200])
            if m: hdr_name.setdefault(h, m.group(1).decode())
        for m in re.finditer(re.escape(gf_coll.MAG), u):
            try: data, loc, objs = gf_coll.packfile(u, m.start())
            except Exception: continue
            T = shapes_of(data, loc, objs)
            if T is None: continue
            h = owner(m.start())
            if h is not None and h not in col: col[h] = T
            nm = re.findall(rb'Neo\|([\w\-]+)\|', data)
            if nm: neo.setdefault(nm[0].decode(), []).append(T)
            full = re.findall(rb'Neo\|[\x20-\x7e]{6,}', data); full = full[0].decode() if full else ''
            ctr = np.abs((T.reshape(-1, 3).max(0) + T.reshape(-1, 3).min(0)) / 2)
            if 'Animated' not in full and (('|terrain|' in full) or ('|Zone' in full) or ('|st_' in full) or ctr.max() > 120.0):
                direct.append(T)
        for B, inst in gf_mmdl.mmdls(u):
            h = owner(B)
            if h is not None: blocks.setdefault(h, inst)
            roots.append((f, h, inst))
    return col, hdr_name, neo, direct, blocks, roots
def main(root, outp):
    files = []
    for r in root.split(';'): files += sorted(glob.glob(os.path.join(glob.escape(r), '**', '*.str'), recursive=True)) if os.path.isdir(r) else [r]
    files = [f for f in files if 'world_terrain' in f.replace('\\', '/') or os.path.basename(f).endswith('_global.str') or os.path.basename(f) == 'godfather_global_global.str']
    print(len(files), 'files'); sys.stdout.flush()
    from multiprocessing import Pool
    col = {}; desname = {}; neo = {}; blocks = {}; roots = []; hdr_name = {}; direct = []
    with Pool(max(1, (os.cpu_count() or 2))) as pool:
        for fi, (c_, h_, n_, d_, b_, r_) in enumerate(pool.imap(scan_file, files, chunksize=4)):
            for k, v in c_.items(): col.setdefault(k, v)
            for k, v in h_.items(): hdr_name.setdefault(k, v)
            for k, v in n_.items(): neo.setdefault(k, []).extend(v)
            direct += d_
            for k, v in b_.items(): blocks.setdefault(k, v)
            roots += r_
            if fi % 300 == 0: print(f'  {fi}/{len(files)} files, {len(col)} collision assets, {len(blocks)} blocks'); sys.stdout.flush()
    nested = set()
    for f, h, inst in roots:
        for nm, tn, attrs in inst:
            if tn == 'MetaModelPart':
                for a, v in attrs:
                    if a == 'fileName' and isinstance(v, int): nested.add(v)
    tris = []; stats = {}
    def place(inst, M, depth):
        if depth > 6: return
        for nm, tn, attrs in inst:
            X = next((v for a, v in attrs if a == 'XFORM'), None)
            Mi = X @ M if X is not None else M
            refs = [v for a, v in attrs if a in ('CollisionModel', 'CMD_SetModel', 'fileName') and isinstance(v, int)]
            for r in refs:
                if tn in ('CollisionPart', 'BuildingCollisionPart'):
                    T = col.get(r)
                elif tn == 'Destructible':
                    T = col.get(r)
                    if T is None and r in hdr_name and hdr_name[r] in neo: T = neo[hdr_name[r]][0]
                    if T is not None and T[:, :, 1].min() < -0.05 and T[:, :, 1].min() > -3: T = T - np.array([0, T[:, :, 1].min(), 0])   # sit on the ground
                elif tn == 'MetaModelPart':
                    if r in blocks: place(blocks[r], Mi, depth + 1); stats['MetaModelPart'] = stats.get('MetaModelPart', 0) + 1
                    else: stats['MetaModelPart unresolved'] = stats.get('MetaModelPart unresolved', 0) + 1
                    continue
                else: continue
                if T is None: stats[tn + ' unresolved'] = stats.get(tn + ' unresolved', 0) + 1; continue
                P = np.concatenate([T.reshape(-1, 3), np.ones((T.shape[0] * 3, 1))], 1) @ Mi
                tris.append(P[:, :3].reshape(-1, 3, 3)); stats[tn] = stats.get(tn, 0) + 1
    for f, h, inst in roots:
        if h in nested: continue
        place(inst, np.eye(4), 0)
    print('placed:', stats, '+', len(direct), 'world-space meshes')
    T = np.concatenate(tris + direct).astype(np.float32)
    ok = np.all(np.isfinite(T), axis=(1, 2)) & (np.abs(T).max(axis=(1, 2)) < 20000)
    T = T[ok]
    n = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0]); T = T[np.linalg.norm(n, axis=1) > 1e-6]
    mn = T.min(axis=(0, 1)); mx = T.max(axis=(0, 1))
    x0, z0 = np.floor(mn[0] / CELL) * CELL, np.floor(mn[2] / CELL) * CELL
    nx, nz = int((mx[0] - x0) // CELL) + 1, int((mx[2] - z0) // CELL) + 1
    tx0 = ((T[:, :, 0].min(1) - x0) // CELL).astype(int); tx1 = ((T[:, :, 0].max(1) - x0) // CELL).astype(int)
    tz0 = ((T[:, :, 2].min(1) - z0) // CELL).astype(int); tz1 = ((T[:, :, 2].max(1) - z0) // CELL).astype(int)
    cells = [[] for _ in range(nx * nz)]
    for i in range(len(T)):
        if (tx1[i] - tx0[i] + 1) * (tz1[i] - tz0[i] + 1) > 400: continue   # absurd triangles
        for cx in range(tx0[i], tx1[i] + 1):
            for cz in range(tz0[i], tz1[i] + 1): cells[cz * nx + cx].append(i)
    starts = []; idx = []
    for c in cells: starts.append((len(idx), len(c))); idx += c
    os.makedirs(os.path.dirname(os.path.abspath(outp)), exist_ok=True)   # a fresh install has no assets/ yet
    with open(outp, 'wb') as f:
        f.write(b'GFCOL1\0\0' + struct.pack('<3f3i', CELL, x0, z0, nx, nz, len(T)) + struct.pack('<i', len(idx)))
        f.write(T.astype('<f4').tobytes()); f.write(np.array(starts, '<i4').tobytes()); f.write(np.array(idx, '<i4').tobytes())
    print(f'{len(T)} triangles, grid {nx}x{nz} cells of {CELL} m, {len(idx)} cell entries -> {outp} ({os.path.getsize(outp) >> 20} MB)')
    np.savez_compressed(outp + '.npz', T=T)
if __name__ == '__main__': main(sys.argv[1], sys.argv[2])
