"""Crew driver road map: the game's main road graphs (as gf_gps_build.py) WITH heights, so a bridge and the street under it
are never joined (gf_gps_build.py joins any nodes of two graphs closer than 4 m on the ground plan).
Writes assets/gf_crewdrive_roads.bin (little-endian), read by gf_crewdrive.c:
  "CDR1", u32 nodes, u32 edges, u32 0, float x[n], y[n], z[n], incoming-edge CSR (u16 start[n+1], u16 src[e]).
usage: python gf_crewdrive_build.py <USRDIR> <out.bin>"""
import collections, math, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gf_gps_build as G

STITCH = 4.0         # nodes of different graphs closer than this on the ground plan ...
STITCH_DY = 2.0      # ... and closer than this in height are joined
MAX_NBR_LINK = 30.0
MAX_NBR_DY = 3.0


def build(gs):
    nodes, edges, base = [], set(), []
    for k, g in enumerate(gs):
        base.append(len(nodes))
        nodes += [(x, y, z, k) for x, y, z in g["nodes"]]
        edges |= {(base[k] + src, base[k] + dst) for dst, src in g["conns"]}
    byguid = {g["guid"]: k for k, g in enumerate(gs)}
    for k, g in enumerate(gs):
        for _, dst, src, _, _, inb, _ in g["nconns"]:
            gi = g["nbr_guid_idx"][inb] if inb < len(g["nbr_guid_idx"]) else None
            k2 = byguid.get(g["guids"][gi]) if gi is not None and gi < len(g["guids"]) else None
            if k2 is not None and dst < gs[k2]["nNode"]:
                a, b = base[k] + src, base[k2] + dst
                if math.hypot(nodes[a][0] - nodes[b][0], nodes[a][2] - nodes[b][2]) < MAX_NBR_LINK and abs(nodes[a][1] - nodes[b][1]) < MAX_NBR_DY:
                    edges.add((a, b))
    grid = collections.defaultdict(list)
    for i, (x, y, z, _) in enumerate(nodes):
        grid[(int(x // 8), int(z // 8))].append(i)
    stitched = skipped = 0
    for i, (x, y, z, k) in enumerate(nodes):
        for dx in (-1, 0, 1):
            for dz in (-1, 0, 1):
                for j in grid[(int(x // 8) + dx, int(z // 8) + dz)]:
                    if j > i and nodes[j][3] != k and math.hypot(nodes[j][0] - x, nodes[j][2] - z) < STITCH:
                        if abs(nodes[j][1] - y) < STITCH_DY:
                            edges.add((i, j)); edges.add((j, i)); stitched += 1
                        else:
                            skipped += 1
    print("crewdrive roads: %d stitches, %d skipped (different height)" % (stitched, skipped))
    return nodes, edges


def write(nodes, edges, out):
    n = len(nodes)
    inc = [[] for _ in range(n)]
    for a, b in edges:
        if a != b:
            inc[b].append(a)
    rstart, rsrc = [0], []
    for v in range(n):
        rsrc += sorted(set(inc[v])); rstart.append(len(rsrc))
    e = len(rsrc); assert n < 65535 and e < 65536
    blob = struct.pack("<4sIII", b"CDR1", n, e, 0)
    blob += struct.pack("<%df" % n, *[p[0] for p in nodes]) + struct.pack("<%df" % n, *[p[1] for p in nodes]) + struct.pack("<%df" % n, *[p[2] for p in nodes])
    blob += struct.pack("<%dH" % (n + 1), *rstart) + struct.pack("<%dH" % e, *rsrc)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    open(out, "wb").write(blob)
    ys = [p[1] for p in nodes]
    print("crewdrive roads: %d nodes, %d edges, height %.1f .. %.1f m, %d bytes -> %s" % (n, e, min(ys), max(ys), len(blob), out))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    gs = G.load_roads(sys.argv[1])
    if not gs:
        sys.exit("no road graphs under %s" % sys.argv[1])
    write(*build(gs), sys.argv[2])
