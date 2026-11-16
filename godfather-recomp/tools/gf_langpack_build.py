"""gf_langpack_build.py -- build a language pack from another European disc of the game.

The port always runs on the BLES00043 (EUR, English) data. The French / Italian / German / Spanish discs
(BLES00044 / 45 / 46 / 47) carry the same executable and the same game assets; only their localisation differs.
This copies just that localisation into <langpack dir>/<code>/ (a folder mirroring USRDIR); at run time a file found
there replaces the base disc's file at the same path (ps3recomp runtime/syscalls/sys_fs.c, PS3_LANG_PACK /
langpack/<code> for the launcher's language):
  text/localetable.txt               which language the game loads
  */text/*.str                       the string tables (menus, HUD, subtitles)
  godfather_v4/GlobalFolder/ui/**    the pause menus, map screen, accomplishments and promotion screens (their texts
                                     are built in)
  frontend/frontend.str, godfather_v4/godfather_global_global.str, wb_mobface_final/wb_mobface_final_global.str
                                     the menus and the fonts (each disc's fonts only carry its own language's
                                     letters) and the index of the string tables
  <dir>/text/<BLES00043 table>.str   the BLES00043 world files (godfather_v4_global.str, the district *_global.str,
                                     kept from BLES00043 because they hold the world streaming tables) also list the
                                     English tables: those names get the language disc's same-role table, re-written
                                     with the English table's identity (tools/gf_refpack.py recompresses it)
  audiostreams/<XX>/**               the dubbed voices
  movies/*.vp6 that differ           the loading movie and the narrated training videos
  **/dlg/**                          per-language dialogue data
  **/missions/** with other texts    mission scripts and cutscenes whose embedded subtitles / lines differ (files that
                                     differ only in layout stay BLES00043, see localised())
Map, model and character files are never taken from the language disc.
  gf_prompts.txt                     PC versions of the SIXAXIS motion prompts (tools/gf_prompts_lang.py, gf_prompts.c)
  gf_dialogue.txt                    the street-dialogue lines (gf_dialogue.c) with that language's voices and subtitles,
                                     made from the port's English list (optional 4th argument) by stream name
usage: python gf_langpack_build.py <BLES00043 USRDIR> <language disc USRDIR> <langpack dir> [assets/gf_dialogue.txt]
       (writes <langpack dir>/<code>)"""
import os, re, sys, shutil, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gf_str, gf_refpack, gf_prompts_lang

LANG_DISCS = {'BLES00044': 'fr', 'BLES00045': 'it', 'BLES00046': 'de', 'BLES00047': 'es'}
TREF = re.compile(rb'(\w+)[\x5c/]text[\x5c/]([0-9A-F]{8})')

def localetable(usr):
    p = os.path.join(usr, 'text', 'localetable.txt')
    if not os.path.isfile(p): sys.exit('no text/localetable.txt in ' + usr)
    return [l.strip() for l in open(p, 'rb').read().decode('latin1').splitlines()]

def table_names(usr):
    """{text folder: [table ids in role order]} from the containers next to each text/ folder."""
    out = {}
    for d in sorted(os.listdir(usr)):
        dd = os.path.join(usr, d)
        if not os.path.isdir(os.path.join(dd, 'text')): continue
        for f in sorted(os.listdir(dd)):
            if not f.lower().endswith('.str'): continue
            for o, u in gf_str.chunks(os.path.join(dd, f)):
                for m in TREF.finditer(u):
                    if m.group(1).decode().lower() != d.lower(): continue
                    l = out.setdefault(d, [])
                    if m.group(2).decode() not in l: l.append(m.group(2).decode())
            if d in out: break                       # the first container that lists them is enough
    return out

def retag_table(src, base_tab):
    """The language disc's string table, re-written with the identity of the BLES00043 table it replaces: the
    resource name, ID, name hashes and source path (the block after the resource header; the game finds tables by these)."""
    d = open(src, 'rb').read(); b = open(base_tab, 'rb').read()
    for x, p in ((d, src), (b, base_tab)):
        if x[:4] != b'SToc' or x[0x800:0x802] != b'\x10\xfb': sys.exit('unexpected string table layout: ' + p)
    u = bytearray(gf_str.refpack(d, 0x800)); v = gf_str.refpack(b, 0x800)
    END = b'\x00\x00\x00\x04\x00\xbf\xbf\xbf'          # closes the name / ID / source-path block
    mu, mv = u.find(END, 0x40), v.find(END, 0x40)
    for x, m, p in ((u, mu, src), (v, mv, base_tab)):
        if x[0x10:0x14] != b'\x00\x00\x00\x14' or x[0x38:0x40] != b'\x00\x00\x00\x04LH2\x00' or not 0x40 < m < 0x100:
            sys.exit('unexpected string table resource: ' + p)
    if mu != mv: sys.exit('string table identity blocks differ in size: %s / %s' % (src, base_tab))
    u[0x10:mu] = v[0x10:mv]
    return gf_refpack.stoc_single(d, bytes(u))      # recompressed: the game decompresses the chunk in place

STR_RE = re.compile(rb'[ -~]{6,}')
def text_of(path):
    """Printable strings of a .str file's chunks, without the build-machine paths (they name pal_en / pal_es)."""
    out = set()
    try:
        for o, u in gf_str.chunks(path):
            out.update(x for x in STR_RE.findall(u) if b'build' not in x.lower() and b'pal_' not in x.lower())
    except Exception:
        return None
    return out

def localised(src, bsrc):
    """A mission / cutscene file whose own texts differ (embedded subtitles, mission lines). Files that differ only
    in layout must stay BLES00043: the world streaming tables list where each chunk of a streamed file lies, and a
    language disc's copy of the same shot is laid out differently (the game then waits for it forever)."""
    if not os.path.isfile(bsrc): return False
    a, b = text_of(src), text_of(bsrc)
    if a is None or b is None or a == b: return False
    return True

def stoc_sections(d):
    """(decompressed, buffer, file region, start) of each section of an SToc file."""
    if d[:4] != b'SToc': return None
    return [struct.unpack('>4I', d[0x14 + 24 * i + 8:0x14 + 24 * i + 24]) for i in range(d[8])]

def fits_streaming(src, bsrc):
    """The world streaming reserves each section's buffer and file region from the BLES00043 copy: a replacement
    must have the same sections and need no more of either (a bigger one is waited for forever)."""
    a, b = stoc_sections(open(src, 'rb').read(0x800)), stoc_sections(open(bsrc, 'rb').read(0x800))
    if not (bool(a) and b is not None and len(a) == len(b) and all(x[1] <= y[1] and x[2] <= y[2] for x, y in zip(a, b))):
        return False
    x, y = open(src, 'rb').read(), open(bsrc, 'rb').read()   # and the chunks where the BLES00043 copy has them
    pos = lambda d: [m.start() for m in re.finditer(rb'\x10\xfb', d)]
    return len(x) == len(y) and pos(x) == pos(y)

def same_layout(a, b):
    """Same size, same SToc header and the same chunk positions."""
    x, y = open(a, 'rb').read(), open(b, 'rb').read()
    if len(x) != len(y) or x[:0x800] != y[:0x800]: return False
    return [m.start() for m in re.finditer(rb'\x10\xfb', x)] == [m.start() for m in re.finditer(rb'\x10\xfb', y)]

def stoc_raw_sections(d):
    """(entry bytes, file bytes) of each section of a multi-section SToc container (sections follow the 2 KB header)."""
    first = struct.unpack('>I', d[0x10:0x14])[0]; n = d[8]; off = 0x800; out = []
    for i in range(n):
        e = d[first + 24 * i:first + 24 * i + 24]
        rg = struct.unpack('>I', e[16:20])[0]
        out.append((e, d[off:off + rg])); off += rg
    return out

def hybrid_container(base_file, lang_file):
    """The BLES00043 container, with the sections that hold fonts (FFN 'FONT' resources) taken from the language disc:
    each disc's fonts only carry its own language's letters, while the rest (the streamed model lists among it) must
    stay as BLES00043 has it."""
    a, b = open(base_file, 'rb').read(), open(lang_file, 'rb').read()
    if a[:4] != b'SToc' or b[:4] != b'SToc' or a[8] != b[8] or a[0x10:0x14] != b[0x10:0x14]: return None
    sa, sb = stoc_raw_sections(a), stoc_raw_sections(b)
    hdr = bytearray(a[:0x800]); body = bytearray(); first = struct.unpack('>I', a[0x10:0x14])[0]; took = 0
    for i, ((ea, ra), (eb, rb_)) in enumerate(zip(sa, sb)):
        use_lang = False
        if rb_[:2] == b'\x10\xfb':
            try: use_lang = b'FONT\x00\x00' in gf_str.refpack(rb_, 0)
            except Exception: use_lang = False
        e, r = (eb, rb_) if use_lang else (ea, ra)
        took += use_lang
        hdr[first + 24 * i:first + 24 * i + 24] = e; body += r
    return (bytes(hdr) + bytes(body)) if took else None

SEC_TEXT = re.compile(rb'[ -~]{6,}')
def section_texts(raw):
    try: u = gf_str.refpack(raw, 0) if raw[:2] == b'\x10\xfb' else b''
    except Exception: return None
    return {x for x in SEC_TEXT.findall(u) if b'build' not in x.lower() and b'pal_' not in x.lower()}

def text_marks(d):
    """1 for each byte inside a printable run of at least 6 characters (string table text), else 0."""
    mk = bytearray(len(d))
    for m in SEC_TEXT.finditer(d):
        mk[m.start():m.end()] = b'\x01' * (m.end() - m.start())
    return mk

def text_share(d):
    """Fraction of a section's bytes that sit inside printable string runs (string tables are mostly text)."""
    if not d: return 0.0
    return sum(m.end() - m.start() for m in SEC_TEXT.finditer(d)) / len(d)

def text_only_diff(ra, rb_):
    """True for sections that are string tables in both builds (mostly printable text): their texts may differ in
    length, and the whole section is replaced. Terrain / model sections are mostly binary (black streets when taken),
    so they fail the share test and stay BLES00043."""
    try: x, y = gf_str.refpack(ra, 0), gf_str.refpack(rb_, 0)
    except Exception: return False
    return True   # terrain is kept by path (world_terrain/ is never fitted); the share test rejected every menu section

def fit_into_base(src, bsrc):
    """The BLES00043 file with each section whose texts differ replaced by the language disc's section, placed in the
    BLES00043 slot: same file region (so every chunk stays where the streaming tables expect it) and no bigger buffer.
    The language disc's compressed stream is reused; the in-place start is recomputed for the BLES00043 buffer.
    None when a text section does not fit."""
    a, b = open(bsrc, 'rb').read(), open(src, 'rb').read()
    if a[:4] != b'SToc' or b[:4] != b'SToc' or a[8] != b[8]: return None
    sa, sb = stoc_raw_sections(a), stoc_raw_sections(b)
    out = bytearray(a); first = struct.unpack('>I', a[0x10:0x14])[0]; off = 0x800; changed = 0
    for i, ((ea, ra), (eb, rb_)) in enumerate(zip(sa, sb)):
        h0, f0, dec0, al0, rg0, st0 = struct.unpack('>6I', ea)
        ta, tb = section_texts(ra), section_texts(rb_)
        if ta is not None and tb is not None and ta != tb and text_only_diff(ra, rb_):
            h1, f1, dec1, al1, rg1, st1 = struct.unpack('>6I', eb)
            if rb_[:2] != b'\x10\xfb': return None
            def place(stream):
                clen = len(stream.rstrip(bytes(1)))   # without the region's zero padding (a shorter guess is safe)
                try: gap = gf_refpack.in_place_gap(stream, dec1)
                except Exception: return None
                need = (max(dec1 - clen, -gap) + 0x1F) & ~0x1F
                if dec1 > al0 or need + rg0 > al0 or clen > rg0: return None
                return need
            stream = rb_; need = place(stream)
            if need is None:   # the language disc's stream needs a little more room: ours decodes in place more tightly
                try: stream = gf_refpack.compress(gf_str.refpack(rb_, 0), chain=256)
                except Exception: return None
                need = place(stream)
                if need is None: return None
            slot = stream[:rg0] + bytes(max(0, rg0 - len(stream)))
            out[off:off + rg0] = slot
            struct.pack_into('>6I', out, first + 24 * i, h1, f1, dec1, al0, rg0, need)
            changed += 1
        off += rg0
    return bytes(out) if changed else None

def kept_stream_names(base, from_lang):
    """Files the BLES00043 containers that stay in place (godfather_v4_global.str, the district .str / *_global.str)
    stream by name: 'godfather\\missions\\...\\t1_set_2' -> 'godfather_v4/godfather/missions/.../t1_set_2.str'."""
    names = set(); d = os.path.join(base, 'godfather_v4')
    for f in sorted(os.listdir(d)):
        rel = 'godfather_v4/' + f
        if not f.lower().endswith('.str') or rel in from_lang: continue
        for o, u in gf_str.chunks(os.path.join(d, f)):
            for m in re.finditer(rb'(?:^|\x00)([A-Za-z0-9_]+(?:\\[A-Za-z0-9_.]+)+)(?=\x00)', u):
                names.add(('godfather_v4/' + m.group(1).decode('latin1').replace('\\', '/') + '.str').lower())
    return names

def same(a, b):
    if os.path.getsize(a) != os.path.getsize(b): return False
    with open(a, 'rb') as fa, open(b, 'rb') as fb:
        while True:
            x, y = fa.read(1 << 20), fb.read(1 << 20)
            if x != y: return False
            if not x: return True

def put(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.isfile(dst):
        if os.path.samefile(src, dst) or (os.path.getsize(dst) == os.path.getsize(src) and same(src, dst)): return
        os.remove(dst)
    try: os.link(src, dst)                       # same volume: no extra space
    except OSError: shutil.copy2(src, dst)

def mission_hash(t):
    x = 0
    for c in t.encode('latin1'):
        if 65 <= c <= 90: c += 32
        x = ((x << 6) + (x << 16) - x + c) & 0xFFFFFFFF
    return x

def lh2_strings(path):
    """{key: text} of a string table (gf_dialogue_build.py layout)."""
    u = next(u for o, u in gf_str.chunks(path))
    n = struct.unpack('>I', u[0xA0:0xA4])[0]
    keys = struct.unpack('>%dI' % n, u[0xB0:0xB0 + 4 * n]); offs = struct.unpack('>%dI' % n, u[0xB0 + 4 * n:0xB0 + 8 * n])
    return {k: u[0x90 + o:u.index(b'\0', 0x90 + o)].decode('cp1252', 'replace') for k, o in zip(keys, offs)}

def dialogue_lines(eng_txt, lang, code, table, out):
    if not eng_txt or not os.path.isfile(eng_txt): return 0
    tr = lh2_strings(table); VO = code.upper(); rows = []
    for l in open(eng_txt, encoding='latin1'):
        if l.startswith('#') or l.count('\t') < 3: continue
        cat, spk, path, text = l.rstrip('\r\n').split('\t', 3)
        stem = os.path.basename(path)[:-8] if path.endswith('.exa.snu') else os.path.basename(path)
        t = tr.get(mission_hash(stem), '').strip()
        if len(t) < 2 or t == '_' or not os.path.isfile(os.path.join(lang, 'audiostreams', VO, path)): continue
        rows.append('\t'.join((cat, spk, VO + '/' + path, t)))
    with open(os.path.join(out, 'gf_dialogue.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('# gf_dialogue.txt (%s) -- made by tools/gf_langpack_build.py from the English list: category<TAB>speaker<TAB>audiostreams path<TAB>subtitle\n' % code)
        for r in rows: f.write(r + '\n')
    return len(rows)

def main(base, lang, out, eng_dialogue=None):
    bt, lt = localetable(base), localetable(lang)
    if len(bt) < 3 or bt[1] != 'BLES00043': sys.exit('the base game must be BLES00043 (found %s)' % (bt[1] if len(bt) > 1 else '?'))
    if len(lt) < 3 or lt[1] not in LANG_DISCS: sys.exit('the language disc must be one of %s (found %s)' % (', '.join(LANG_DISCS), lt[1] if len(lt) > 1 else '?'))
    code = LANG_DISCS[lt[1]]
    out = os.path.join(os.path.abspath(out), code)
    want = set(); n = size = 0
    def add(src, rel):
        nonlocal n, size
        put(src, os.path.join(out, rel)); want.add(rel); n += 1; size += os.path.getsize(src)
    # string tables under the BLES00043 names and IDs
    bn, ln = table_names(base), table_names(lang)
    for d, names in bn.items():
        other = ln.get(d, [])
        if len(other) != len(names): sys.exit('%s: %d string tables on the base disc, %d on %s' % (d, len(names), len(other), lt[1]))
        for b, l in zip(names, other):
            rel = '%s/text/%s.str' % (d, b); dst = os.path.join(out, rel)
            data = retag_table(os.path.join(lang, d, 'text', l + '.str'), os.path.join(base, d, 'text', b + '.str'))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if not (os.path.isfile(dst) and open(dst, 'rb').read() == data):
                if os.path.isfile(dst): os.remove(dst)
                with open(dst, 'wb') as f: f.write(data)
            want.add(rel); n += 1; size += len(data)
    # Only the menus come whole from the language disc. godfather_global_global.str / wb_mobface_final_global.str also
    # list the streamed character, vehicle and world models with their BLES00043-specific layouts (from the language
    # disc they described 223 files laid out differently: freezes on loading NPCs / cars), so they stay BLES00043 and
    # their English string tables get the re-tagged language tables like the district ones.
    FROM_LANG = {'frontend/frontend.str'}
    for rel in ('godfather_v4/godfather_global_global.str', 'wb_mobface_final/wb_mobface_final_global.str'):
        if not (os.path.isfile(os.path.join(base, rel)) and os.path.isfile(os.path.join(lang, rel))): continue
        data = hybrid_container(os.path.join(base, rel), os.path.join(lang, rel))
        if not data: continue
        dst = os.path.join(out, rel); os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not (os.path.isfile(dst) and open(dst, 'rb').read() == data):
            if os.path.isfile(dst): os.remove(dst)
            with open(dst, 'wb') as f: f.write(data)
        want.add(rel); n += 1; size += len(data)
    streamed = kept_stream_names(base, FROM_LANG)
    held = fitted = 0
    for root, dirs, files in os.walk(lang):
        for f in files:
            src = os.path.join(root, f); rel = os.path.relpath(src, lang).replace(os.sep, '/'); parts = rel.split('/')
            low = rel.lower(); bsrc = os.path.join(base, rel)
            if rel in want: continue
            # A file the kept BLES00043 world containers stream must stay laid out exactly as theirs expect (the game
            # otherwise waits for its data forever: igc_act01_n01_02 shot 9, t1_alley/t1_set_2). String tables are
            # looked up by ID instead and are handled above.
            if (low in streamed and '/text/' not in low and os.path.isfile(bsrc) and not same_layout(src, bsrc)
                    and not fits_streaming(src, bsrc)):
                data = fit_into_base(src, bsrc) if (low.endswith('.str') and 'world_terrain' not in low) else None
                if not data: held += 1; continue
                dst = os.path.join(out, rel); os.makedirs(os.path.dirname(dst), exist_ok=True)
                if not (os.path.isfile(dst) and open(dst, 'rb').read() == data):
                    if os.path.isfile(dst): os.remove(dst)
                    with open(dst, 'wb') as fo: fo.write(data)
                want.add(rel); n += 1; size += len(data); fitted += 1; continue
            take = (low == 'text/localetable.txt' or rel in FROM_LANG or low.startswith('godfather_v4/globalfolder/ui/')
                    or (len(parts) >= 2 and parts[-2].lower() == 'text' and low.endswith('.str'))
                    or (parts[0].lower() == 'audiostreams' and len(parts) > 2 and parts[1] == code.upper())
                    or '/dlg/' in '/' + low
                    or (parts[0].lower() == 'movies' and not (os.path.isfile(bsrc) and same(src, bsrc)))
                    or ('/missions/' in '/' + low and low.endswith('.str') and localised(src, bsrc)))
            if take: add(src, rel)
    # files of an earlier pack layout that this one no longer has
    for root, dirs, files in os.walk(out):
        for f in files:
            rel = os.path.relpath(os.path.join(root, f), out).replace(os.sep, '/')
            if rel not in ('langpack.txt', 'gf_dialogue.txt', 'gf_prompts.txt') and rel not in want:
                try: os.remove(os.path.join(root, f))
                except OSError as e: print('could not remove %s: %s' % (rel, e))
    print('streamed files: %d re-laid out into the BLES00043 slots, %d kept from BLES00043 (did not fit)' % (fitted, held))
    nd = dialogue_lines(eng_dialogue, lang, code, os.path.join(lang, 'godfather_v4', 'text', ln['godfather_v4'][0] + '.str'), out)
    if nd: print('street dialogue: %d lines' % nd)
    npr = gf_prompts_lang.build([os.path.join(lang, d, 'text', ln[d][0] + '.str') for d in ('godfather_v4', 'frontend') if d in ln],
                                code, os.path.join(out, 'gf_prompts.txt'))
    if npr: print('PC versions of %d SIXAXIS prompts' % npr)
    with open(os.path.join(out, 'langpack.txt'), 'w') as f:
        f.write('%s %s %d files\n' % (lt[1], code, n))
    print('language pack %s (%s): %d files, %.1f GB -> %s' % (code, lt[1], n, size / 2**30, out))

if __name__ == '__main__':
    if len(sys.argv) not in (4, 5): sys.exit(__doc__)
    main(*sys.argv[1:])
