/* gf_prompts.c -- v1042G native PC prompts for Godfather's SIXAXIS moves.
 *
 * The motion-only grapple moves are now driven from the keyboard/mouse or a controller (cellPad.c v1042M), but
 * the game still told the player to "MOVE the SIXAXIS(TM) wireless controller FORWARD". Those prompts are plain
 * markup strings in the English LH2 string tables (gf_global.en.LH2, gf_frontend.en.LH2), with the game's own
 * glyph tokens ([LSTop], [b=Rstick], [RightStickDepress], [RightStickClockwise] ...).
 *
 * A low-priority host thread finds every loaded copy of those strings in guest memory (needle "SIXAXIS\xAC
 * wireless"), matches it exactly against gf_prompts_table.h and rewrites it in place -- every replacement is no
 * longer than the original, the rest is NUL padded, so the table's offsets stay valid:
 *   controller:      "... then FLICK [b=Rstick] UP near a wall."   (native stick/R3/L3/rotate glyphs)
 *   keyboard/mouse:  "... HOLD RMB and LMB to grab, then PRESS X+W near a wall."
 * It follows the input device the player last used (g_input_kbm) and the settings menu's prompt choice
 * (g_ui_kbm_glyphs / KBM_GLYPHS), rewriting the already patched strings when that changes.
 * GF_PROMPTS=0 disables. Log: [prompts].
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gf_prompts_table.h"

extern uint8_t* vm_base;
extern volatile int g_input_kbm;      /* cellPad.c: 1 keyboard/mouse, 0 controller, -1 not known yet */
extern volatile int g_ui_kbm_glyphs;  /* rsx_ui_overlay.c: -1 env, 0 controller, 1 auto, 2 keyboard/mouse */

#define GFP_N ((int)(sizeof g_gf_prompts / sizeof g_gf_prompts[0]))
#define GFP_MAX_SITES 512
typedef struct { uint32_t ea; short idx; signed char mode; } GfSite;
static GfSite s_sites[GFP_MAX_SITES];
static int s_nsites = 0;
static int s_cfg = 1;   /* KBM_GLYPHS: 0 controller, 1 auto, 2 keyboard/mouse */

static int gfp_want_kbm(void)
{
    const int c = g_ui_kbm_glyphs >= 0 ? g_ui_kbm_glyphs : s_cfg;
    return c == 2 || (c != 0 && g_input_kbm > 0);
}
/* A language pack's own prompts (<pack>/gf_prompts.txt, tools/gf_prompts_lang.py): indices GFP_N.. */
#define GFP_EXT_MAX 128
static char* s_ext[GFP_EXT_MAX][3]; static size_t s_ext_len[GFP_EXT_MAX]; static int s_next = 0;
extern const char* ps3_fs_lang_pack_dir(void);   /* sys_fs.c */
static char* gfp_unhex(const char* h, size_t n)
{
    char* o = (char*)malloc(n / 2 + 1); if (!o) return NULL;
    for (size_t i = 0; i < n / 2; i++) { unsigned v = 0; sscanf(h + 2 * i, "%2x", &v); o[i] = (char)v; }
    o[n / 2] = 0; return o;
}
static void gfp_load_ext(void)
{
    const char* lp = ps3_fs_lang_pack_dir(); if (!lp) return;
    char path[MAX_PATH + 64]; snprintf(path, sizeof path, "%s\\gf_prompts.txt", lp);
    FILE* f = fopen(path, "rb"); if (!f) return;
    static char l[8192];
    while (s_next < GFP_EXT_MAX && fgets(l, sizeof l, f)) {
        if (l[0] == '#') continue;
        char* t1 = strchr(l, '\t'); char* t2 = t1 ? strchr(t1 + 1, '\t') : NULL; if (!t2) continue;
        char* e = t2 + 1; while (*e && *e != '\r' && *e != '\n') e++;
        char* o = gfp_unhex(l, (size_t)(t1 - l)); char* p = gfp_unhex(t1 + 1, (size_t)(t2 - t1 - 1)); char* k = gfp_unhex(t2 + 1, (size_t)(e - t2 - 1));
        if (!o || !p || !k || strlen(p) > strlen(o) || strlen(k) > strlen(o)) { free(o); free(p); free(k); continue; }
        s_ext[s_next][0] = o; s_ext[s_next][1] = p; s_ext[s_next][2] = k; s_ext_len[s_next] = strlen(o); s_next++;
    }
    fclose(f);
    printf("[prompts] %d prompts from the language pack (%s)\n", s_next, path);
}
static const char* gfp_text(int idx, int kbm)
{
    if (idx >= GFP_N) return s_ext[idx - GFP_N][kbm ? 2 : 1];
    return kbm ? g_gf_prompts[idx].kbm : g_gf_prompts[idx].pad;
}
static size_t gfp_len(int idx) { return idx >= GFP_N ? s_ext_len[idx - GFP_N] : g_gf_prompts[idx].len; }

/* write variant `kbm` of prompt idx at ea, NUL padded to the original length */
static void gfp_write(uint32_t ea, int idx, int kbm)
{
    const char* t = gfp_text(idx, kbm);
    const size_t n = strlen(t), len = gfp_len(idx);
    uint8_t* p = vm_base + ea;
    memcpy(p, t, n);
    memset(p + n, 0, len - n);
}
/* is variant `kbm` of prompt idx still at ea (and nothing else reused that memory)? */
static int gfp_still(uint32_t ea, int idx, int kbm)
{
    const char* t = gfp_text(idx, kbm);
    const size_t n = strlen(t);
    return memcmp(vm_base + ea, t, n + 1) == 0;
}

static int gfp_match(const uint8_t* s, size_t len)
{
    for (int i = 0; i < GFP_N; i++)
        if (g_gf_prompts[i].len == len && !memcmp(g_gf_prompts[i].orig, s, len)) return i;
    for (int i = 0; i < s_next; i++)
        if (s_ext_len[i] == len && !memcmp(s_ext[i][0], s, len)) return GFP_N + i;
    return -1;
}

static void gfp_site_add(uint32_t ea, int idx, int kbm)
{
    for (int i = 0; i < s_nsites; i++) if (s_sites[i].ea == ea) { s_sites[i].idx = (short)idx; s_sites[i].mode = (signed char)kbm; return; }
    if (s_nsites < GFP_MAX_SITES) { s_sites[s_nsites].ea = ea; s_sites[s_nsites].idx = (short)idx; s_sites[s_nsites].mode = (signed char)kbm; s_nsites++; }
}

/* Horspool scan of one committed range [b, b+n) for the needle; returns strings patched */
/* v1042X: second needle -- the melee / grab prompts that name the right stick (gf_prompts_table.h MELEE entries) */
static const uint8_t k_nd_sixaxis[] = { 'S','I','X','A','X','I','S',0xAC,' ','w','i','r','e','l','e','s','s' };
static const uint8_t k_nd_sixaxis_any[] = { 'S','I','X','A','X','I','S',0xAC };   /* language-pack prompts */
static const uint8_t k_nd_rstick[] = { '[','b','=','R','s','t','i','c','k',']' };
static int gfp_scan_range(uint8_t* b, size_t n, int kbm, int* unknown, const uint8_t* nd, size_t m)
{
    size_t skip[256];
    for (int i = 0; i < 256; i++) skip[i] = m;
    for (size_t i = 0; i + 1 < m; i++) skip[nd[i]] = m - 1 - i;
    int got = 0;
    size_t i = 0;
    while (i + m <= n) {
        const uint8_t last = b[i + m - 1];
        if (last == nd[m - 1] && b[i] == nd[0] && !memcmp(b + i, nd, m)) {
            /* string bounds: back to the previous NUL, forward to the next one */
            size_t s = i, e = i + m;
            while (s > 0 && i - s < 600 && b[s - 1] != 0) s--;
            while (e < n && e - i < 600 && b[e] != 0) e++;
            const int idx = e < n ? gfp_match(b + s, e - s) : -1;
            if (idx >= 0) {
                const uint32_t ea = (uint32_t)((b + s) - vm_base);
                gfp_write(ea, idx, kbm);
                gfp_site_add(ea, idx, kbm);
                got++;
            } else (*unknown)++;
            i = e;
            continue;
        }
        i += skip[last];
    }
    return got;
}

static int gfp_scan_all(int kbm, int* unknown, size_t* scanned)
{
    int got = 0;
    uint64_t ea = 0x00010000u;
    const uint64_t end = 0xC0000000ull;
    while (ea < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vm_base + ea, &mi, sizeof mi)) break;
        uint64_t rs = (uint64_t)((uint8_t*)mi.BaseAddress - vm_base), re = rs + mi.RegionSize;
        if (re <= ea) break;
        if (rs < ea) rs = ea;
        if (re > end) re = end;
        const DWORD pr = mi.Protect & 0xFF;
        const int readable = mi.State == MEM_COMMIT && !(mi.Protect & PAGE_GUARD) &&
                             (pr == PAGE_READWRITE || pr == PAGE_READONLY || pr == PAGE_WRITECOPY ||
                              pr == PAGE_EXECUTE_READWRITE || pr == PAGE_EXECUTE_READ);
        const int rsx = rs >= 0x10000000ull && rs < 0x20000000ull;   /* RSX local memory: textures, no text */
        if (readable && !rsx) {
            __try { got += gfp_scan_range(vm_base + rs, (size_t)(re - rs), kbm, unknown, k_nd_sixaxis, sizeof k_nd_sixaxis);
                    got += gfp_scan_range(vm_base + rs, (size_t)(re - rs), kbm, unknown, k_nd_rstick, sizeof k_nd_rstick);
                    if (s_next) got += gfp_scan_range(vm_base + rs, (size_t)(re - rs), kbm, unknown, k_nd_sixaxis_any, sizeof k_nd_sixaxis_any);
                    *scanned += (size_t)(re - rs); }
            __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
        ea = re;
    }
    return got;
}

static DWORD WINAPI gfp_thread(LPVOID arg)
{
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    Sleep(4000);
    int last_kbm = -1, logs = 0;
    unsigned tick = 0;
    for (;;) {
        const int kbm = gfp_want_kbm();
        if (kbm != last_kbm && s_nsites) {          /* device switched: rewrite what is already patched */
            int moved = 0, lost = 0;
            __try {
                for (int i = 0; i < s_nsites; ) {
                    GfSite* st = &s_sites[i];
                    if (gfp_still(st->ea, st->idx, st->mode)) {
                        if (st->mode != kbm) { gfp_write(st->ea, st->idx, kbm); st->mode = (signed char)kbm; moved++; }
                        i++;
                    } else { s_sites[i] = s_sites[--s_nsites]; lost++; }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { s_nsites = 0; }
            printf("[prompts] v1042G prompts -> %s (%d strings rewritten, %d stale dropped)\n",
                   kbm ? "keyboard/mouse" : "controller", moved, lost);
            fflush(stdout);
        }
        last_kbm = kbm;
        /* new copies appear when a string table (re)loads: full scan every 2 s */
        if ((tick++ % 4) == 0) {
            int unknown = 0; size_t scanned = 0;
            LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
            const int got = gfp_scan_all(kbm, &unknown, &scanned);
            QueryPerformanceCounter(&t1);
            if (got || (logs < 2)) {
                if (logs < 60) {
                    logs++;
                    printf("[prompts] v1042G scan: %d SIXAXIS prompts -> %s, %d unmatched, %zu MB in %.0f ms (%d sites)\n",
                           got, kbm ? "keyboard/mouse" : "controller", unknown, scanned >> 20,
                           (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart, s_nsites);
                    fflush(stdout);
                }
            }
        }
        Sleep(500);
    }
    return 0;
}

/* v1043N GF_MEMFIND=<text> (diagnostic, default off): every 5 s, list guest addresses holding <text> as ASCII and as
 * UTF-16BE (up to 24 hits, with 48 bytes around each). Used to locate the name-entry buffer for keyboard typing. */
static char s_mf_pat[64];
static void gfm_scan(const uint8_t* pat, size_t n, const char* kind)
{
    int hits = 0;
    uint64_t ea = 0x00010000u; const uint64_t end = 0xC0000000ull;
    while (ea < end && hits < 24) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vm_base + ea, &mi, sizeof mi)) break;
        uint64_t rs = (uint64_t)((uint8_t*)mi.BaseAddress - vm_base), re = rs + mi.RegionSize;
        if (re <= ea) break;
        if (rs < ea) rs = ea;
        if (re > end) re = end;
        const DWORD pr = mi.Protect & 0xFF;
        const int readable = mi.State == MEM_COMMIT && !(mi.Protect & PAGE_GUARD) &&
                             (pr == PAGE_READWRITE || pr == PAGE_READONLY || pr == PAGE_WRITECOPY || pr == PAGE_EXECUTE_READWRITE);
        if (readable && !(rs >= 0x10000000ull && rs < 0x20000000ull) && re - rs >= n) {
            __try {
                const uint8_t* b = vm_base + rs; const size_t len = (size_t)(re - rs);
                for (size_t i = 0; i + n <= len && hits < 24; i++) {
                    if (b[i] != pat[0] || memcmp(b + i, pat, n)) continue;
                    const uint32_t a = (uint32_t)(rs + i);
                    char hex[200]; int o = 0;
                    const size_t c0 = i >= 16 ? i - 16 : 0;
                    for (size_t k = c0; k < c0 + 48 && k < len; k++) o += snprintf(hex + o, sizeof hex - o, "%02X", b[k]);
                    printf("[memfind] v1043N %s \"%s\" at 0x%08X (context from 0x%08X: %s)\n", kind, s_mf_pat, a, (uint32_t)(rs + c0), hex);
                    hits++; i += n - 1;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
        ea = re;
    }
    printf("[memfind] v1043N %s \"%s\": %d hit(s)\n", kind, s_mf_pat, hits); fflush(stdout);
}
static DWORD WINAPI gfm_thread(LPVOID arg)
{
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    const size_t n = strlen(s_mf_pat);
    uint8_t w[128];
    for (size_t i = 0; i < n; i++) { w[2 * i] = 0; w[2 * i + 1] = (uint8_t)s_mf_pat[i]; }
    for (;;) {
        Sleep(5000);
        gfm_scan((const uint8_t*)s_mf_pat, n, "ascii");
        gfm_scan(w, 2 * n, "utf16be");
    }
    return 0;
}

/* v1043T name-entry screen detector for keyboard typing (cellPad.c pad_namekb_poll, g_pad_namekb_seq_fn).
 * Opening Godfather's ENTER NAME screen creates ~20 APT text objects holding the default name "Aldo" in the UI heap
 * (soak T03: 0 hits in 0x40000000-0x48000000 in the creator, 20+ right after the screen opens; the static string-pool
 * copies live above 0x48F00000). While no player exists (menus), this counts them every 250 ms; a rise of >= 4 is
 * one screen opening. GF_NAMEKB_DETECT=0 = off. */
extern int (*volatile g_pad_weapon_fn)(void);
extern int (*volatile g_pad_namekb_seq_fn)(void);
static volatile LONG s_nk_seq = 0;
static int gfn_seq(void) { return (int)s_nk_seq; }
static int gfn_count(void)
{
    int hits = 0;
    uint64_t ea = 0x40000000u; const uint64_t end = 0x48000000ull;
    while (ea < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vm_base + ea, &mi, sizeof mi)) break;
        uint64_t rs = (uint64_t)((uint8_t*)mi.BaseAddress - vm_base), re = rs + mi.RegionSize;
        if (re <= ea) break;
        if (rs < ea) rs = ea;
        if (re > end) re = end;
        const DWORD pr = mi.Protect & 0xFF;
        if (mi.State == MEM_COMMIT && !(mi.Protect & PAGE_GUARD) && (pr == PAGE_READWRITE || pr == PAGE_READONLY) && re - rs >= 4) {
            __try {
                const uint8_t* b = vm_base + rs; const size_t len = (size_t)(re - rs);
                const uint8_t* p = b; const uint8_t* e = b + len - 4;
                while (p <= e && (p = (const uint8_t*)memchr(p, 'A', (size_t)(e - p) + 1)) != NULL) {
                    if (p[1] == 'l' && p[2] == 'd' && p[3] == 'o') hits++;
                    p++;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
        ea = re;
    }
    return hits;
}
static DWORD WINAPI gfn_thread(LPVOID arg)
{
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    Sleep(3000);
    /* v1043T-2 (soak T05): while the screen is up the count keeps RISING every scan (the APT text is rebuilt), so a rise
     * is a new opening only after the count was flat for >= GF_NAMEKB_QUIET_MS (default 1500). GF_NAMEKB_TRACE=1 prints
     * every scan. */
    const char* tr = getenv("GF_NAMEKB_TRACE"); const int trace = tr && *tr == '1';
    const char* qe = getenv("GF_NAMEKB_QUIET_MS"); const unsigned long long quiet_ms = (qe && *qe) ? (unsigned long long)atoi(qe) : 1500ull;
    int last = -1; unsigned long long last_rise = 0;
    const unsigned long long t0 = GetTickCount64();
    for (;;) {
        Sleep(250);
        if (g_pad_weapon_fn && g_pad_weapon_fn() >= 0) { last = -1; last_rise = 0; Sleep(750); continue; }   /* in the game */
        const int n = gfn_count();
        const unsigned long long now = GetTickCount64();
        if (trace) { printf("[namekb] trace t=%.2f s count %d\n", (double)(now - t0) / 1000.0, n); fflush(stdout); }
        if (last >= 0 && n >= last + 4) {
            if (!last_rise || now - last_rise >= quiet_ms) {
                const LONG q = InterlockedIncrement(&s_nk_seq);
                printf("[namekb] v1043T name-entry screen detected (#%ld, %d -> %d text objects)\n", (long)q, last, n); fflush(stdout);
            }
            last_rise = now;
        }
        last = n;
    }
    return 0;
}

/* v1043N GF_MEMDIFF=1 (diagnostic): PAD_SCRIPT events with mask bit 16 (0x10000) snapshot the UI heap 0x40000000-0x48000000;
 * after the 4th snapshot, values that step like a grid cursor are listed ([memdiff]): snapshots taken with the name-entry
 * cursor on a, b, c, then m (one row down). */
extern void (*volatile g_pad_snapshot_fn)(void);
enum { GFS_BASE = 0x40000000u, GFS_LEN = 0x08000000u, GFS_N = 4 };
static uint8_t* s_gfs[GFS_N]; static int s_gfs_n = 0;
static uint32_t gfs_be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static float gfs_f32(const uint8_t* p) { uint32_t u = gfs_be32(p); float f; memcpy(&f, &u, 4); return f; }
static double gfs_f64(const uint8_t* p) { uint64_t u = ((uint64_t)gfs_be32(p) << 32) | gfs_be32(p + 4); double d; memcpy(&d, &u, 8); return d; }
static void gfs_analyze(void)
{
    int ni = 0, nf = 0, nd = 0, np = 0;
    for (uint32_t o = 0; o + 8 <= GFS_LEN; o += 4) {
        const uint8_t *a = s_gfs[0] + o, *b = s_gfs[1] + o, *c = s_gfs[2] + o, *d = s_gfs[3] + o;
        const uint32_t i0 = gfs_be32(a), i1 = gfs_be32(b), i2 = gfs_be32(c), i3 = gfs_be32(d);
        if (i0 < 64 && i1 == i0 + 1 && i2 == i0 + 2 && (i3 == i0 + 12 || i3 == i0 + 2) && ni < 40) {
            ni++; printf("[memdiff] int   0x%08X: %u %u %u %u\n", GFS_BASE + o, i0, i1, i2, i3); }
        const float f0 = gfs_f32(a), f1 = gfs_f32(b), f2 = gfs_f32(c), f3 = gfs_f32(d);
        if (f0 > -64.0f && f0 < 64.0f && f0 == (float)(int)f0 && f1 == f0 + 1.0f && f2 == f0 + 2.0f &&
            (f3 == f0 + 12.0f || f3 == f0 + 2.0f) && nf < 40) {
            nf++; printf("[memdiff] f32   0x%08X: %g %g %g %g\n", GFS_BASE + o, f0, f1, f2, f3); }
        if ((o & 7) == 0) {
            const double d0 = gfs_f64(a), d1 = gfs_f64(b), d2 = gfs_f64(c), d3 = gfs_f64(d);
            if (d0 > -64.0 && d0 < 64.0 && d0 == (double)(int)d0 && d1 == d0 + 1.0 && d2 == d0 + 2.0 &&
                (d3 == d0 + 12.0 || d3 == d0 + 2.0) && nd < 40) {
                nd++; printf("[memdiff] f64   0x%08X: %g %g %g %g\n", GFS_BASE + o, d0, d1, d2, d3); }
        }
        /* highlight position: x steps evenly a->b->c, m below c (same x); y constant then changes */
        const float x0 = f0, x1 = f1, x2 = f2, x3 = f3;
        const float y0 = gfs_f32(a + 4), y1 = gfs_f32(b + 4), y2 = gfs_f32(c + 4), y3 = gfs_f32(d + 4);
        const float dx = x1 - x0;
        if (dx > 5.0f && dx < 400.0f && x2 - x1 == dx && x3 == x2 && y0 == y1 && y1 == y2 && y3 != y2 &&
            y3 - y2 > -400.0f && y3 - y2 < 400.0f && x0 > -2000.0f && x0 < 4000.0f && np < 40) {
            np++; printf("[memdiff] xy    0x%08X: (%g,%g) (%g,%g) (%g,%g) (%g,%g)\n", GFS_BASE + o, x0, y0, x1, y1, x2, y2, x3, y3); }
    }
    printf("[memdiff] v1043N done: %d int, %d f32, %d f64, %d xy candidates\n", ni, nf, nd, np); fflush(stdout);
    /* v1043N-2: around the first int candidate -- hex of every snapshot, and row-like values (v, v, v, v+1) */
    uint32_t first = 0xFFFFFFFFu;
    for (uint32_t o = 0; o + 4 <= GFS_LEN && first == 0xFFFFFFFFu; o += 4) {
        const uint32_t i0 = gfs_be32(s_gfs[0] + o);
        if (i0 < 64 && gfs_be32(s_gfs[1] + o) == i0 + 1 && gfs_be32(s_gfs[2] + o) == i0 + 2 && gfs_be32(s_gfs[3] + o) == i0 + 2) first = o;
    }
    if (first != 0xFFFFFFFFu) {
        const uint32_t lo = first >= 0x200 ? first - 0x200 : 0, hi = first + 0x600 < GFS_LEN ? first + 0x600 : GFS_LEN;
        for (int k = 0; k < GFS_N; k++)
            for (uint32_t o = lo; o < hi; o += 32) {
                char line[160]; int q = 0;
                for (int j = 0; j < 32; j += 4) q += snprintf(line + q, sizeof line - q, " %08X", gfs_be32(s_gfs[k] + o + j));
                printf("[memdiff] snap%d 0x%08X:%s\n", k, GFS_BASE + o, line);
            }
        const uint32_t rlo = first >= 0x10000 ? first - 0x10000 : 0, rhi = first + 0x10000 < GFS_LEN ? first + 0x10000 : GFS_LEN;
        int nr = 0;
        for (uint32_t o = rlo; o < rhi && nr < 40; o += 4) {
            const uint32_t r0 = gfs_be32(s_gfs[0] + o), r3 = gfs_be32(s_gfs[3] + o);
            if (r0 < 8 && gfs_be32(s_gfs[1] + o) == r0 && gfs_be32(s_gfs[2] + o) == r0 && r3 == r0 + 1) {
                nr++; printf("[memdiff] row?  0x%08X: %u %u %u %u\n", GFS_BASE + o, r0, r0, r0, r3); }
        }
        fflush(stdout);
    }
}
static void gfs_snap(void)
{
    if (s_gfs_n >= GFS_N) return;
    uint8_t* buf = (uint8_t*)calloc(1, GFS_LEN);
    if (!buf) { printf("[memdiff] out of memory\n"); return; }
    uint64_t ea = GFS_BASE; const uint64_t end = (uint64_t)GFS_BASE + GFS_LEN;
    while (ea < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vm_base + ea, &mi, sizeof mi)) break;
        uint64_t rs = (uint64_t)((uint8_t*)mi.BaseAddress - vm_base), re = rs + mi.RegionSize;
        if (re <= ea) break;
        if (rs < ea) rs = ea;
        if (re > end) re = end;
        const DWORD pr = mi.Protect & 0xFF;
        if (mi.State == MEM_COMMIT && !(mi.Protect & PAGE_GUARD) && (pr == PAGE_READWRITE || pr == PAGE_READONLY)) {
            __try { memcpy(buf + (rs - GFS_BASE), vm_base + rs, (size_t)(re - rs)); } __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
        ea = re;
    }
    s_gfs[s_gfs_n++] = buf;
    printf("[memdiff] snapshot %d taken\n", s_gfs_n); fflush(stdout);
    if (s_gfs_n == GFS_N) gfs_analyze();
}

/* v1043Y GF_PAUSEDIFF=1 (diagnostic, user-driven): F7 (game window focused) = memory snapshot, with a beep. Take 4:
 * playing, PAUSED, playing, PAUSED. Words of guest RAM (not RSX local 0x10000000-0x1FFFFFFF) that hold one value whenever
 * playing and another whenever paused are listed ([pausediff]); small values (flags) first. Used to find Godfather's pause
 * flag for g_pad_menu_fn (WASD = d-pad in the pause menu). */
typedef struct { uint32_t ea, len; uint8_t* buf; } gpd_reg;
typedef struct { uint32_t ea, v0, v1; } gpd_cand;
static gpd_reg s_gpd_r[512]; static int s_gpd_nr = 0, s_gpd_n = 0;
static gpd_cand* s_gpd_c = NULL; static size_t s_gpd_nc = 0;
static int gpd_copy(uint8_t* dst, uint32_t ea, uint32_t len)
{
    __try { memcpy(dst, vm_base + ea, len); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void gpd_snap(void)
{
    const int k = s_gpd_n;
    /* v1053C: log the player object with each snapshot (player-relative flags, e.g. "in a car" for the car controls) */
    { uint8_t b[4]; uint32_t a = 0;
      if (gpd_copy(b, 0x10181290u, 4)) a = gfs_be32(b);
      if (a >= 0x10000u && a < 0xF0000000u && gpd_copy(b, a + 4, 4)) a = gfs_be32(b); else a = 0;
      if (a >= 0x10000u && a < 0xF0000000u && gpd_copy(b, a, 4)) a = gfs_be32(b); else a = 0;
      printf("[pausediff] v1053C snapshot %d: player object 0x%08X\n", k + 1, a >= 0x17FCu ? a - 0x17FCu : 0); fflush(stdout); }
    if (k >= 4) return;
    if (k == 0) {
        size_t total = 0;
        const uint64_t ranges[2][2] = { { 0x00010000ull, 0x10000000ull }, { 0x20000000ull, 0xF0000000ull } };
        for (int r = 0; r < 2; r++) {
            uint64_t ea = ranges[r][0];
            while (ea < ranges[r][1] && s_gpd_nr < 512) {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery(vm_base + ea, &mi, sizeof mi)) break;
                uint64_t rs = (uint64_t)((uint8_t*)mi.BaseAddress - vm_base), re = rs + mi.RegionSize;
                if (re <= ea) break;
                if (rs < ea) rs = ea;
                if (re > ranges[r][1]) re = ranges[r][1];
                const DWORD pr = mi.Protect & 0xFF;
                if (mi.State == MEM_COMMIT && !(mi.Protect & PAGE_GUARD) && (pr == PAGE_READWRITE || pr == PAGE_EXECUTE_READWRITE) &&
                    total + (re - rs) <= (1024ull << 20)) {
                    uint8_t* b = (uint8_t*)malloc((size_t)(re - rs));
                    if (b && gpd_copy(b, (uint32_t)rs, (uint32_t)(re - rs))) {
                        s_gpd_r[s_gpd_nr].ea = (uint32_t)rs; s_gpd_r[s_gpd_nr].len = (uint32_t)(re - rs); s_gpd_r[s_gpd_nr].buf = b;
                        s_gpd_nr++; total += (size_t)(re - rs);
                    } else free(b);
                }
                ea = re;
            }
        }
        printf("[pausediff] snapshot 1 (PLAYING): %d regions, %zu MB\n", s_gpd_nr, total >> 20);
    } else if (k == 1) {
        const size_t cap = 16u << 20;
        s_gpd_c = (gpd_cand*)malloc(cap * sizeof(gpd_cand)); s_gpd_nc = 0;
        for (int r = 0; r < s_gpd_nr && s_gpd_c; r++) {
            uint8_t* live = (uint8_t*)malloc(s_gpd_r[r].len);
            if (live && gpd_copy(live, s_gpd_r[r].ea, s_gpd_r[r].len))
                for (uint32_t o = 0; o + 4 <= s_gpd_r[r].len && s_gpd_nc < cap; o += 4) {
                    const uint32_t v0 = gfs_be32(s_gpd_r[r].buf + o), v1 = gfs_be32(live + o);
                    if (v0 != v1) { s_gpd_c[s_gpd_nc].ea = s_gpd_r[r].ea + o; s_gpd_c[s_gpd_nc].v0 = v0; s_gpd_c[s_gpd_nc].v1 = v1; s_gpd_nc++; }
                }
            free(live); free(s_gpd_r[r].buf); s_gpd_r[r].buf = NULL;
        }
        printf("[pausediff] snapshot 2 (PAUSED): %zu changed words\n", s_gpd_nc);
    } else {
        size_t w = 0;
        for (size_t i = 0; i < s_gpd_nc; i++) {
            uint8_t v[4];
            if (!gpd_copy(v, s_gpd_c[i].ea, 4)) continue;
            const uint32_t now = gfs_be32(v);
            if (now == (k == 2 ? s_gpd_c[i].v0 : s_gpd_c[i].v1)) s_gpd_c[w++] = s_gpd_c[i];
        }
        s_gpd_nc = w;
        printf("[pausediff] snapshot %d (%s): %zu candidates left\n", k + 1, k == 2 ? "PLAYING" : "PAUSED", s_gpd_nc);
        if (k == 3) {
            int shown = 0;
            for (int pass = 0; pass < 3; pass++)
                for (size_t i = 0; i < s_gpd_nc && shown < 300; i++) {
                    const uint32_t a = s_gpd_c[i].v0, b = s_gpd_c[i].v1;
                    const int cls = (a <= 1 && b <= 1) ? 0 : (a < 256 && b < 256) ? 1 : 2;
                    if (cls != pass) continue;
                    shown++;
                    printf("[pausediff] %s 0x%08X: playing 0x%08X  paused 0x%08X\n", cls == 0 ? "FLAG " : cls == 1 ? "small" : "word ",
                           s_gpd_c[i].ea, a, b);
                }
            printf("[pausediff] v1043Y done: %zu candidates (%d shown)\n", s_gpd_nc, shown);
            /* v1043Y-2: full list to pausediff_<tick>.txt (compare runs: only addresses present every run are stable), and
             * for each flag: static (ELF data, < 0x02000000) words pointing at most 0x4000 below it = pointer to its object */
            char fn[64]; snprintf(fn, sizeof fn, "pausediff_%llu.txt", (unsigned long long)GetTickCount64());
            FILE* f = fopen(fn, "w");
            if (f) { for (size_t i = 0; i < s_gpd_nc; i++) fprintf(f, "%08X %08X %08X\n", s_gpd_c[i].ea, s_gpd_c[i].v0, s_gpd_c[i].v1);
                     fclose(f); printf("[pausediff] full list -> %s\n", fn); }
            for (size_t i = 0; i < s_gpd_nc; i++) {
                const uint32_t a = s_gpd_c[i].v0, b = s_gpd_c[i].v1, x = s_gpd_c[i].ea;
                if (!(a <= 1 && b <= 1) || x < 0x20000000u) continue;
                int np = 0;
                for (uint32_t sa = 0x00010000u; sa < 0x02000000u && np < 6; sa += 0x10000u) {
                    uint8_t* blk = (uint8_t*)malloc(0x10000u);
                    if (blk && gpd_copy(blk, sa, 0x10000u))
                        for (uint32_t o2 = 0; o2 < 0x10000u && np < 6; o2 += 4) {
                            const uint32_t v = gfs_be32(blk + o2);
                            if (v <= x && x - v < 0x4000u && v >= 0x20000000u) {
                                np++; printf("[pausediff] ptr  flag 0x%08X <- static 0x%08X = 0x%08X (+0x%X)\n", x, sa + o2, v, x - v); }
                        }
                    free(blk);
                }
            }
            fflush(stdout);
        }
    }
    s_gpd_n++;
    fflush(stdout);
    MessageBeep(MB_OK);
}
static DWORD WINAPI gpd_thread(LPVOID arg)
{
    (void)arg; int prev = 0;
    printf("[pausediff] v1043Y ON: press F7 4x -- playing, PAUSED, playing, PAUSED\n"); fflush(stdout);
    for (;;) {
        Sleep(30);
        HWND fg = GetForegroundWindow(); DWORD pid = 0; if (fg) GetWindowThreadProcessId(fg, &pid);
        const int down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F7) & 0x8000);
        if (down && !prev) gpd_snap();
        prev = down;
        if (s_gpd_n >= 4) return 0;
    }
}

/* v1044C GF_AIMDIFF=1 (diagnostic, user-driven; user: free aim turns EXTREMELY slowly -> drive the camera angles from
 * the mouse directly). F7 = snapshot, with a beep. Keep free aim on (hold RMB) the whole time:
 *   1 start   2 turned RIGHT ~45 deg   3 turned RIGHT ~45 deg more   4 looked UP (no turning)   5 looked DOWN (no turning)
 * Yaw candidates: big-endian floats that step the same way at 2 and 3 and stay put at 4 and 5. Pitch candidates: floats
 * that did not move at 2->3 (snapshot 3 kept in full) but move one way at 4 and back the other way at 5. Pairs of a yaw and
 * a pitch candidate within 0x40 bytes are printed first ([aimdiff]); full lists -> aimdiff_yaw.txt / aimdiff_pitch.txt. */
typedef struct { uint32_t ea; float v0, v1, v2, v3, v4; } gad_cand;
static gpd_reg s_gad_r[512]; static int s_gad_nr = 0, s_gad_n = 0;
static gad_cand *s_gad_y = NULL, *s_gad_p = NULL; static size_t s_gad_ny = 0, s_gad_np = 0;
static float gad_f(const uint8_t* p) { uint32_t u = gfs_be32(p); float f; memcpy(&f, &u, 4); return f; }
static int gad_ok(float f) { return f == f && f > -1e5f && f < 1e5f; }
static float gad_live(uint32_t ea, int* ok) { uint8_t v[4]; *ok = gpd_copy(v, ea, 4); return *ok ? gad_f(v) : 0.0f; }
static void gad_full(void)   /* replace the full copy with the current memory */
{
    for (int r = 0; r < s_gad_nr; r++) free(s_gad_r[r].buf);
    s_gad_nr = 0; size_t total = 0;
    const uint64_t ranges[2][2] = { { 0x00010000ull, 0x10000000ull }, { 0x20000000ull, 0xF0000000ull } };
    for (int r = 0; r < 2; r++) {
        uint64_t ea = ranges[r][0];
        while (ea < ranges[r][1] && s_gad_nr < 512) {
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery(vm_base + ea, &mi, sizeof mi)) break;
            uint64_t rs = (uint64_t)((uint8_t*)mi.BaseAddress - vm_base), re = rs + mi.RegionSize;
            if (re <= ea) break;
            if (rs < ea) rs = ea;
            if (re > ranges[r][1]) re = ranges[r][1];
            const DWORD pr = mi.Protect & 0xFF;
            if (mi.State == MEM_COMMIT && !(mi.Protect & PAGE_GUARD) && (pr == PAGE_READWRITE || pr == PAGE_EXECUTE_READWRITE) &&
                total + (re - rs) <= (1024ull << 20)) {
                uint8_t* b = (uint8_t*)malloc((size_t)(re - rs));
                if (b && gpd_copy(b, (uint32_t)rs, (uint32_t)(re - rs))) {
                    s_gad_r[s_gad_nr].ea = (uint32_t)rs; s_gad_r[s_gad_nr].len = (uint32_t)(re - rs); s_gad_r[s_gad_nr].buf = b;
                    s_gad_nr++; total += (size_t)(re - rs);
                } else free(b);
            }
            ea = re;
        }
    }
    printf("[aimdiff] full copy: %d regions, %zu MB\n", s_gad_nr, total >> 20);
}
static void gad_snap(void)
{
    const int k = s_gad_n;
    if (k >= 5) return;
    const size_t cap = 8u << 20;
    if (k == 0) gad_full();
    else if (k == 1) {   /* yaw: changed since 1 */
        s_gad_y = (gad_cand*)malloc(cap * sizeof(gad_cand)); s_gad_ny = 0;
        for (int r = 0; r < s_gad_nr && s_gad_y; r++) {
            uint8_t* live = (uint8_t*)malloc(s_gad_r[r].len);
            if (live && gpd_copy(live, s_gad_r[r].ea, s_gad_r[r].len))
                for (uint32_t o = 0; o + 4 <= s_gad_r[r].len && s_gad_ny < cap; o += 4) {
                    const float a = gad_f(s_gad_r[r].buf + o), b = gad_f(live + o);
                    if (!gad_ok(a) || !gad_ok(b) || a == b) continue;
                    const float d = b - a; if (d > -0.02f && d < 0.02f) continue;
                    gad_cand* c = &s_gad_y[s_gad_ny++]; c->ea = s_gad_r[r].ea + o; c->v0 = a; c->v1 = b;
                }
            free(live);
        }
        printf("[aimdiff] snapshot 2 (turned right): %zu yaw candidates\n", s_gad_ny);
    } else if (k == 2) {   /* yaw keeps stepping the same way; full copy for pitch */
        size_t w = 0;
        for (size_t i = 0; i < s_gad_ny; i++) {
            int ok; const float v = gad_live(s_gad_y[i].ea, &ok); if (!ok || !gad_ok(v)) continue;
            const float d1 = s_gad_y[i].v1 - s_gad_y[i].v0, d2 = v - s_gad_y[i].v1;
            const float r = d2 / d1;   /* same direction, comparable size (wrap-around at +-pi / 360 tolerated by |d2| check) */
            if (!(r > 0.25f && r < 4.0f)) continue;
            s_gad_y[i].v2 = v; s_gad_y[w++] = s_gad_y[i];
        }
        s_gad_ny = w;
        gad_full();
        printf("[aimdiff] snapshot 3 (turned right more): %zu yaw candidates\n", s_gad_ny);
    } else if (k == 3) {   /* looked up: yaw stays, pitch changes */
        size_t w = 0;
        for (size_t i = 0; i < s_gad_ny; i++) {
            int ok; const float v = gad_live(s_gad_y[i].ea, &ok); if (!ok) continue;
            const float d1 = s_gad_y[i].v1 - s_gad_y[i].v0, dd = v - s_gad_y[i].v2;
            if ((dd < 0 ? -dd : dd) > 0.15f * (d1 < 0 ? -d1 : d1)) continue;
            s_gad_y[i].v3 = v; s_gad_y[w++] = s_gad_y[i];
        }
        s_gad_ny = w;
        s_gad_p = (gad_cand*)malloc(cap * sizeof(gad_cand)); s_gad_np = 0;
        for (int r = 0; r < s_gad_nr && s_gad_p; r++) {
            uint8_t* live = (uint8_t*)malloc(s_gad_r[r].len);
            if (live && gpd_copy(live, s_gad_r[r].ea, s_gad_r[r].len))
                for (uint32_t o = 0; o + 4 <= s_gad_r[r].len && s_gad_np < cap; o += 4) {
                    const float a = gad_f(s_gad_r[r].buf + o), b = gad_f(live + o);
                    if (!gad_ok(a) || !gad_ok(b)) continue;
                    const float d = b - a; if (d > -0.02f && d < 0.02f) continue;
                    gad_cand* c = &s_gad_p[s_gad_np++]; c->ea = s_gad_r[r].ea + o; c->v2 = a; c->v3 = b;
                }
            free(live);
        }
        for (int r = 0; r < s_gad_nr; r++) { free(s_gad_r[r].buf); s_gad_r[r].buf = NULL; }
        printf("[aimdiff] snapshot 4 (looked up): %zu yaw, %zu pitch candidates\n", s_gad_ny, s_gad_np);
    } else {   /* looked down: yaw stays, pitch moves the other way past where it was */
        size_t w = 0;
        for (size_t i = 0; i < s_gad_ny; i++) {
            int ok; const float v = gad_live(s_gad_y[i].ea, &ok); if (!ok) continue;
            const float d1 = s_gad_y[i].v1 - s_gad_y[i].v0, dd = v - s_gad_y[i].v3;
            if ((dd < 0 ? -dd : dd) > 0.15f * (d1 < 0 ? -d1 : d1)) continue;
            s_gad_y[i].v4 = v; s_gad_y[w++] = s_gad_y[i];
        }
        s_gad_ny = w; w = 0;
        for (size_t i = 0; i < s_gad_np; i++) {
            int ok; const float v = gad_live(s_gad_p[i].ea, &ok); if (!ok || !gad_ok(v)) continue;
            const float up = s_gad_p[i].v3 - s_gad_p[i].v2, dn = v - s_gad_p[i].v3;
            if (!((up > 0 && dn < -up) || (up < 0 && dn > -up))) continue;   /* back past the start */
            s_gad_p[i].v4 = v; s_gad_p[w++] = s_gad_p[i];
        }
        s_gad_np = w;
        printf("[aimdiff] snapshot 5 (looked down): %zu yaw, %zu pitch candidates\n", s_gad_ny, s_gad_np);
        FILE* f = fopen("aimdiff_yaw.txt", "w");
        if (f) { for (size_t i = 0; i < s_gad_ny; i++) fprintf(f, "%08X %g %g %g %g %g\n", s_gad_y[i].ea, s_gad_y[i].v0, s_gad_y[i].v1, s_gad_y[i].v2, s_gad_y[i].v3, s_gad_y[i].v4); fclose(f); }
        f = fopen("aimdiff_pitch.txt", "w");
        if (f) { for (size_t i = 0; i < s_gad_np; i++) fprintf(f, "%08X %g %g %g\n", s_gad_p[i].ea, s_gad_p[i].v2, s_gad_p[i].v3, s_gad_p[i].v4); fclose(f); }
        int shown = 0;
        for (size_t i = 0; i < s_gad_ny && shown < 120; i++)
            for (size_t j = 0; j < s_gad_np && shown < 120; j++) {
                const int64_t dist = (int64_t)s_gad_p[j].ea - (int64_t)s_gad_y[i].ea;
                if (dist < -0x40 || dist > 0x40) continue;
                shown++;
                printf("[aimdiff] PAIR yaw 0x%08X (%g %g %g | %g %g)  pitch 0x%08X (%+d) (%g -> up %g -> down %g)\n",
                       s_gad_y[i].ea, s_gad_y[i].v0, s_gad_y[i].v1, s_gad_y[i].v2, s_gad_y[i].v3, s_gad_y[i].v4,
                       s_gad_p[j].ea, (int)dist, s_gad_p[j].v2, s_gad_p[j].v3, s_gad_p[j].v4);
            }
        for (size_t i = 0; i < s_gad_ny && i < 60; i++)
            printf("[aimdiff] yaw   0x%08X: %g %g %g | %g %g\n", s_gad_y[i].ea, s_gad_y[i].v0, s_gad_y[i].v1, s_gad_y[i].v2, s_gad_y[i].v3, s_gad_y[i].v4);
        printf("[aimdiff] v1044C done: %zu yaw, %zu pitch candidates, %d pairs shown (aimdiff_yaw.txt / aimdiff_pitch.txt)\n",
               s_gad_ny, s_gad_np, shown);
    }
    s_gad_n++;
    fflush(stdout);
    MessageBeep(MB_OK);
}
static DWORD WINAPI gad_thread(LPVOID arg)
{
    (void)arg; int prev = 0;
    printf("[aimdiff] v1044C ON: hold RMB (free aim), F7 at: start, right, right more, up, down\n"); fflush(stdout);
    for (;;) {
        Sleep(30);
        HWND fg = GetForegroundWindow(); DWORD pid = 0; if (fg) GetWindowThreadProcessId(fg, &pid);
        const int down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F7) & 0x8000);
        if (down && !prev) gad_snap();
        prev = down;
        if (s_gad_n >= 5) return 0;
    }
}

/* v1044D GF_AIMTRACE=1 (diagnostic, needs a slow-memory build with PPU_WWATCH armed: play-aimwatch.cmd). The free-aim
 * yaw copies start at exactly 0.0 when free aim is entered and grow while turning (GF_AIMDIFF), but heap addresses move
 * between sessions, so they are found in the SAME session: on entering free aim (gun + RMB) every 0.0 float in
 * 0x40000000-0x50000000 is marked; 1.5 s later (player turning) the marked floats now in 3..720 are grouped by value;
 * the live write watch (g_ww_live, ppu_loader.cpp) is aimed at a member of the biggest group, so the following [ww] lines
 * name the guest code that writes the yaw. Each later free-aim entry watches the next group (5 max). Log [aimtrace]. */
extern volatile uint32_t g_ww_live, g_ww_live_len; extern volatile long g_ww_live_gen;
enum { GAT_BASE = 0x40000000u, GAT_LEN = 0x10000000u };
static DWORD WINAPI gat_thread(LPVOID arg)
{
    (void)arg;
    uint8_t* zbits = (uint8_t*)malloc(GAT_LEN / 32u);
    typedef struct { uint32_t ea; float v; } gat_c;
    gat_c* c = (gat_c*)malloc(sizeof(gat_c) * 8192);
    if (!zbits || !c) { printf("[aimtrace] out of memory\n"); return 0; }
    printf("[aimtrace] v1044D ON: draw a gun, hold RMB and keep turning RIGHT for ~5 s; repeat for the next candidate\n"); fflush(stdout);
    int prev = 0, round = 0;
    for (;;) {
        Sleep(20);
        HWND fg = GetForegroundWindow(); DWORD pid = 0; if (fg) GetWindowThreadProcessId(fg, &pid);
        const int gun = g_pad_weapon_fn && g_pad_weapon_fn() == 1;
        const int down = pid == GetCurrentProcessId() && gun && (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
        if (!(down && !prev)) { prev = down; continue; }
        prev = down;
        if (round >= 5) continue;
        Sleep(300);   /* the R3 toggle lands */
        memset(zbits, 0, GAT_LEN / 32u);
        size_t nz = 0;
        for (uint64_t ea = GAT_BASE; ea < (uint64_t)GAT_BASE + GAT_LEN; ) {
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery(vm_base + ea, &mi, sizeof mi)) break;
            uint64_t rs = (uint64_t)((uint8_t*)mi.BaseAddress - vm_base), re = rs + mi.RegionSize;
            if (re <= ea) break;
            if (rs < ea) rs = ea;
            if (re > (uint64_t)GAT_BASE + GAT_LEN) re = (uint64_t)GAT_BASE + GAT_LEN;
            const DWORD pr = mi.Protect & 0xFF;
            if (mi.State == MEM_COMMIT && !(mi.Protect & PAGE_GUARD) && (pr == PAGE_READWRITE || pr == PAGE_EXECUTE_READWRITE)) {
                __try {
                    for (uint64_t a = rs; a + 4 <= re; a += 4) {
                        const uint32_t u = *(const uint32_t*)(vm_base + a);
                        if ((u & 0xFFFFFF7Fu) == 0) {   /* +0.0 or -0.0 (host-order view of a BE word) */
                            const uint32_t i = (uint32_t)((a - GAT_BASE) >> 2); zbits[i >> 3] |= (uint8_t)(1u << (i & 7)); nz++; }
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) { }
            }
            ea = re;
        }
        printf("[aimtrace] round %d: free aim entered, %zu zero floats marked -- keep turning\n", round + 1, nz); fflush(stdout);
        Sleep(1500);
        int n = 0;
        for (uint32_t i = 0; i < GAT_LEN / 4u && n < 8192; i++) {
            if (!(zbits[i >> 3] & (1u << (i & 7)))) { if ((i & 7) == 0 && !zbits[i >> 3]) i += 7; continue; }
            const uint32_t ea = GAT_BASE + i * 4u; uint8_t b[4];
            if (!gpd_copy(b, ea, 4)) continue;
            const float v = gfs_f32(b), av = v < 0 ? -v : v;
            if (v == v && av >= 3.0f && av <= 720.0f) { c[n].ea = ea; c[n].v = v; n++; }
        }
        /* groups of identical values, biggest first; skip groups used in earlier rounds */
        float used[8]; int nused = 0; (void)used;
        int best_cnt[5] = {0}; float best_v[5]; uint32_t best_ea[5];
        for (int k = 0; k < 5; k++) { best_cnt[k] = 0; best_v[k] = 0; best_ea[k] = 0; }
        for (int i = 0; i < n; i++) {
            int dup = 0; for (int j = 0; j < i; j++) if (c[j].v == c[i].v) { dup = 1; break; }
            if (dup) continue;
            int cnt = 0; for (int j = i; j < n; j++) if (c[j].v == c[i].v) cnt++;
            for (int k = 0; k < 5; k++) if (cnt > best_cnt[k]) {
                for (int m = 4; m > k; m--) { best_cnt[m] = best_cnt[m-1]; best_v[m] = best_v[m-1]; best_ea[m] = best_ea[m-1]; }
                best_cnt[k] = cnt; best_v[k] = c[i].v; best_ea[k] = c[i].ea; break; }
        }
        printf("[aimtrace] %d candidates (were 0.0, now 3..720)\n", n);
        for (int k = 0; k < 5 && best_cnt[k]; k++) {
            printf("[aimtrace]   group %d: value %g x%d, e.g.", k + 1, best_v[k], best_cnt[k]);
            int shown = 0; for (int i = 0; i < n && shown < 8; i++) if (c[i].v == best_v[k]) { printf(" 0x%08X", c[i].ea); shown++; }
            printf("\n");
        }
        (void)nused;
        const int pick = round < 5 ? round : 4;
        if (best_cnt[pick]) {
            g_ww_live_len = 0x10; g_ww_live = best_ea[pick] & ~3u; g_ww_live_gen++;
            printf("[aimtrace] round %d: write watch -> 0x%08X (group %d, value %g) -- [ww] lines follow in the err log\n",
                   round + 1, g_ww_live, pick + 1, best_v[pick]);
        }
        fflush(stdout);
        round++;
    }
}

void gf_prompts_start(void)
{
    { const char* at = getenv("GF_AIMTRACE");
      if (at && *at == '1') { HANDLE ht = CreateThread(NULL, 256u << 10, gat_thread, NULL, 0, NULL); if (ht) CloseHandle(ht); } }
    { const char* ad = getenv("GF_AIMDIFF");
      if (ad && *ad == '1') { HANDLE ha = CreateThread(NULL, 256u << 10, gad_thread, NULL, 0, NULL); if (ha) CloseHandle(ha); } }
    { const char* pd = getenv("GF_PAUSEDIFF");
      if (pd && *pd == '1') { HANDLE hp = CreateThread(NULL, 256u << 10, gpd_thread, NULL, 0, NULL); if (hp) CloseHandle(hp); } }
    { const char* md = getenv("GF_MEMDIFF"); if (md && *md == '1') g_pad_snapshot_fn = gfs_snap; }
    { const char* d = getenv("GF_NAMEKB_DETECT");
      if (d && *d == '1') { g_pad_namekb_seq_fn = gfn_seq;   /* v1043G: replaced by the renderer's grid probe; opt-in */
          HANDLE hn = CreateThread(NULL, 256u << 10, gfn_thread, NULL, 0, NULL); if (hn) CloseHandle(hn); } }
    { const char* m = getenv("GF_MEMFIND");
      if (m && *m) { snprintf(s_mf_pat, sizeof s_mf_pat, "%s", m);
                     HANDLE hm = CreateThread(NULL, 256u << 10, gfm_thread, NULL, 0, NULL); if (hm) CloseHandle(hm); } }
    const char* e = getenv("GF_PROMPTS");
    if (e && *e == '0') { printf("[prompts] v1042G SIXAXIS prompt rewrite off (GF_PROMPTS=0)\n"); return; }
    e = getenv("KBM_GLYPHS"); if (e && *e) s_cfg = atoi(e);
    gfp_load_ext();
    printf("[prompts] v1042G SIXAXIS prompt rewrite ON (%d prompts; KBM_GLYPHS=%d)\n", GFP_N, s_cfg);
    fflush(stdout);
    HANDLE h = CreateThread(NULL, 256u << 10, gfp_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}
#else
void gf_prompts_start(void) {}
#endif
