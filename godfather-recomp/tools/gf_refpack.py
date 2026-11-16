"""gf_refpack.py -- RefPack (EA 0x10FB) compressor for the language-pack string tables, and the SToc container header
the game needs to decompress a chunk in place. Inverse of gf_str.refpack()."""
import struct

def compress(data, chain=48):
    n = len(data); out = bytearray()
    out += (b'\x10\xfb' + n.to_bytes(3, 'big')) if n < (1 << 24) else (b'\x90\xfb' + n.to_bytes(4, 'big'))
    head = {}; prev = [-1] * n
    lit0 = 0; i = 0
    def flush_lits(upto, keep):
        nonlocal lit0
        while upto - lit0 > keep:                 # runs are multiples of 4 (max 112); 0-3 stay for the next code
            k = min(112, (upto - lit0) & ~3)
            out.append(0xE0 | ((k >> 2) - 1)); out.extend(data[lit0:lit0 + k]); lit0 += k
    def insert(p):
        if p + 3 <= n:
            h = data[p] | (data[p + 1] << 8) | (data[p + 2] << 16)
            prev[p] = head.get(h, -1); head[h] = p
    while i < n:
        best_len = 0; best_off = 0
        if i + 3 <= n:
            h = data[i] | (data[i + 1] << 8) | (data[i + 2] << 16)
            c = head.get(h, -1); tries = chain
            maxl = min(1028, n - i)
            while c >= 0 and tries and i - c <= 131072:
                if data[c + best_len] == data[i + best_len] if best_len < maxl else False:
                    l = 0
                    while l < maxl and data[c + l] == data[i + l]: l += 1
                    off = i - c
                    ok = (l >= 3 and off <= 1024) or (l >= 4 and off <= 16384) or (l >= 5)
                    if ok and l > best_len: best_len, best_off = l, off
                    if l == maxl: break
                c = prev[c]; tries -= 1
        if best_len:
            flush_lits(i, 3)
            lit = i - lit0; off = best_off - 1; L = best_len
            if L <= 10 and best_off <= 1024:
                out += bytes([((off >> 3) & 0x60) | ((L - 3) << 2) | lit, off & 0xFF])
            elif L <= 67 and best_off <= 16384:
                out += bytes([0x80 | (L - 4), (lit << 6) | (off >> 8), off & 0xFF])
            else:
                out += bytes([0xC0 | (((off >> 16) & 1) << 4) | (((L - 5) >> 8) << 2) | lit, (off >> 8) & 0xFF, off & 0xFF, (L - 5) & 0xFF])
            out.extend(data[lit0:i]); 
            for p in range(i, i + L): insert(p)
            i += L; lit0 = i
        else:
            insert(i); i += 1
    flush_lits(n, 3)
    out.append(0xFC | (n - lit0)); out.extend(data[lit0:n])
    return bytes(out)

def in_place_gap(stream, n):
    """Smallest (input position - output position) while decoding, for placing the stream at an offset of the
    output buffer (the game decompresses a chunk in place)."""
    fl = stream[0]; k = 4 if fl & 0x80 else 3; p = 2 + k + (k if fl & 1 else 0); w = 0; worst = p
    while True:
        b0 = stream[p]
        if b0 < 0x80: lit = b0 & 3; ln = ((b0 >> 2) & 7) + 3; p += 2
        elif b0 < 0xC0: lit = stream[p + 1] >> 6; ln = (b0 & 0x3F) + 4; p += 3
        elif b0 < 0xE0: lit = b0 & 3; ln = ((b0 & 0x0C) << 6) + stream[p + 3] + 5; p += 4
        elif b0 < 0xFC: lit = ((b0 & 0x1F) + 1) << 2; ln = 0; p += 1
        else: lit = b0 & 3; ln = 0; p += 1
        p += lit; w += lit + ln
        worst = min(worst, p - w)
        if b0 >= 0xFC: return worst

def stoc_single(template, data):
    """A one-chunk SToc string-table file holding `data`, laid out like the game's own (header from `template`)."""
    c = compress(data)
    region = (len(c) + 0x7FF) & ~0x7FF
    gap = in_place_gap(c, len(data))
    start = (max(len(data) - len(c), -gap) + 0x1F) & ~0x1F   # the stream sits at the end of the output buffer
    alloc = (start + region + 0xFF) & ~0xFF
    hdr = bytearray(template[:0x800])
    struct.pack_into('>4I', hdr, 0x1C, len(data), alloc, region, start)
    return bytes(hdr) + c + b'\0' * (region - len(c))
