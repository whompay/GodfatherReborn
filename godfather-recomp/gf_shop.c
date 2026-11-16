/* gf_shop.c -- v1092S inventory + stores (user: "an inventory that looks like it could be native to the game, then a store").
 *
 * Pages: rsx_ui_overlay.c draws them in the game's pause-menu look (rsx_ui_shop.h); I = inventory, B = store (only when a
 * store is here), Tab switches, Enter / click = use / buy. This file owns the items, the money and what use / buy do.
 *
 * Stores = the game's own business interiors. Every interior streams in from its own folder, named after the business:
 *   godfather/world_terrain/little_italy/st_bowery/smbiz_018_smoke_292/Zone1.str   -> Tobacconist
 * (2026-11-07 survey of the disc, 214 interiors): smoke = tobacconist, quack = doctor (Dr. Valachi), liquor / bar / nightclub
 * = bar, rest = restaurant, bakery, florist, butcher. The runtime passes every cellFsOpen path (ppu_fs.cpp g_fs_open_note);
 * a business interior that streamed in near the player is "here" while the player stays within GF_SHOP_RADIUS (20 m) of
 * where it streamed in AND is indoors (level collision above the head, gf_rag_ray). GF_SHOP_ANY=1: a general store
 * everywhere (testing).
 *
 * Items: cigarettes (smoking with J / Use takes one -- gf_smoke.c asks gf_inv_smoke_take), cigars, drinks / food / medicine
 * restore a share of the health bar (player+0x14C8, max +0x14CC), roses (a gift). Money = the game's own cash:
 * *(0x101D985C)+0x24, set with func_002EB6F8(mgr, value) (updates the HUD). Inventory saved in gf_inventory.txt next to
 * the exe. Everything that touches the game runs in gf_shop_tick (main PPU thread, called from gf_trainer.c main_tick);
 * the render thread only reads and queues requests.
 * GF_SHOP=0 off. GF_SHOP_TEST=<s>: open the inventory s seconds after the player exists, the store 6 s later, close 6 s
 * after that (soak screenshots). Log [shop]. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
extern const char* ui_tr(const char* en);   /* ui_i18n.c: the port's text in the game language */
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "recompiled/ppu_recomp.h"
#include "../ps3recomp/libs/video/rsx_ui_shop.h"

extern uint8_t* vm_base;
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern int gf_cutscene_active(void);
extern int gf_rag_ray(const float* o, const float* d, float maxd, float* t, float* n);
extern void (*volatile g_fs_open_note)(const char* gpath);
extern volatile int g_smoke_req;
extern volatile int g_cig_mouth_req, g_cig_mouth_on;   /* gf_smoke.c v1093M cigarette in the mouth */   /* gf_smoke.c: 1 = start a smoke on the next player update */
typedef void (*gfn)(ppu_context*);
#define TOC 0x00B4BF60u

static int ok(uint32_t ea) { return ea >= 0x10000u && ea < 0xF0000000u; }
static uint32_t rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void wr32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static float rdf(uint32_t ea) { uint32_t u = rd32(ea); float f; memcpy(&f, &u, 4); return f; }
static void wrf(uint32_t ea, float f) { uint32_t u; memcpy(&u, &f, 4); wr32(ea, u); }
static int fok(float f) { return f == f && f > -1e7f && f < 1e7f; }
static void gf_tailor_tick(void);   /* v1111T, below */
/* v1112A tailor (defined below) */
static void tl_buy(int i); static void gf_tailor_tick_v1112(uint32_t pl);
static const char* tl_title(void); static int tl_count(void); static int tl_row(int k, uiov_shop_row* o); static void tl_activate(int k);
static int tl_selected(int k); static int tl_back(void);
static int s_tc_pend[8]; static int s_tc_npend;   /* v1116B tentative definitions (colour / pattern steps), below */
static void gf_tailor_edtest_tick(uint32_t pl);   /* v1111V, below */
static void tv_blob_diff(uint32_t bp, uint32_t bs);
static double now_s(void) { return (double)GetTickCount64() / 1000.0; }
#define SLOG(...) do { static int n_ = 0; if (n_ < 400) { n_++; printf(__VA_ARGS__); fflush(stdout); } } while (0)

static uint32_t player(void)
{
    __try {
        uint32_t a = rd32(0x10181290u); if (!ok(a)) return 0; a = rd32(a + 4); if (!ok(a)) return 0; a = rd32(a);
        return (ok(a) && a >= 0x17FCu) ? a - 0x17FCu : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int player_pos(uint32_t pl, float* p)
{
    const uint32_t f = rd32(pl + 0x68u); if (!ok(f)) return 0;
    p[0] = rdf(f + 0x40); p[1] = rdf(f + 0x44); p[2] = rdf(f + 0x48);
    return fok(p[0]) && fok(p[1]) && fok(p[2]);
}
/* v1129RR: player position for cellPad's PAD_RECORD / PAD_REPLAY route drift report (host poll thread) */
static int gf_rr_pos(float* p) { int r = 0; __try { const uint32_t pl = player(); if (pl) r = player_pos(pl, p); } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; } return r; }
static int player_in_car(uint32_t pl) { const uint32_t car = rd32(pl + 0x1718u); return ok(car) && car == rd32(pl + 0x4Cu) && (rd32(pl + 0x5C4u) & 0x8u); }

/* ------------------------------------------------------------------ guest calls (main PPU thread only) */
static gfn lookup(uint32_t addr)
{
    for (uint64_t k = 0; k < function_table_count; k++) if ((uint32_t)function_table[k].addr == addr) return function_table[k].func;
    return 0;
}
static uint64_t gcall2(uint32_t addr, uint64_t a0, uint64_t a1)
{
    ppu_context* cur = g_active_ctx; if (!cur) return 0;
    static gfn f; static uint32_t fa; if (fa != addr) { f = lookup(addr); fa = addr; } if (!f) return 0;
    static ppu_context c;
    __try {
        memcpy(&c, cur, sizeof c);
        c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1000u) & ~0xFull; c.gpr[2] = TOC; c.gpr[3] = a0; c.gpr[4] = a1; c.lr = 0;
        g_active_ctx = &c; f(&c);
        while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
        g_active_ctx = cur; return c.gpr[3];
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_active_ctx = cur; g_trampoline_fn = 0; return 0; }
}
static int money_get(void) { const uint32_t m = rd32(0x101D985Cu); return ok(m) ? (int)(int32_t)rd32(m + 0x24) : -1; }
static int money_set(int v)   /* main thread */
{
    const uint32_t m = rd32(0x101D985Cu); if (!ok(m)) return 0;
    if (v < 0) v = 0;
    gcall2(0x002EB6F8u, m, (uint64_t)(int64_t)v);
    return money_get() == v;
}

/* ------------------------------------------------------------------ stores */
enum { ST_NONE, ST_TOBACCO, ST_DOCTOR, ST_BAR, ST_REST, ST_BAKERY, ST_FLORIST, ST_BUTCHER, ST_GENERAL, ST_TAILOR, ST_N };   /* v1112A ST_TAILOR */
static const char* const k_st_name[ST_N] = { "", "Tobacconist", "Doctor's Office", "Bar", "Restaurant", "Bakery", "Florist", "Butcher", "General Store", "Tailor" };
static int classify(const char* nm)
{
    char s[96]; size_t i = 0; for (; nm[i] && i + 1 < sizeof s; i++) s[i] = (char)tolower((unsigned char)nm[i]); s[i] = 0;
    if (strstr(s, "smoke")) return ST_TOBACCO;
    if (strstr(s, "quack")) return ST_DOCTOR;
    if (strstr(s, "bakery")) return ST_BAKERY;
    if (strstr(s, "florist")) return ST_FLORIST;
    if (strstr(s, "butcher")) return ST_BUTCHER;
    if (strstr(s, "liquor") || strstr(s, "liqour") || strstr(s, "bar_") || strstr(s, "nightclub")) return ST_BAR;
    if (!strncmp(s, "rest", 4) || strstr(s, "_rest")) return ST_REST;
    return ST_NONE;
}
typedef struct { char name[64]; int type; double t; float p[3]; int have_p; } seen_t;
#define NSEEN 16
static seen_t s_seen[NSEEN]; static volatile LONG s_seen_lock;
static volatile int s_here = ST_NONE; static char s_here_name[64];
static float s_radius = 20.0f; static int s_any = 0;
/* any thread: every cellFsOpen */
static void fs_note(const char* g)
{
    { extern int (*volatile g_pad_player_pos_fn)(float* p); if (!g_pad_player_pos_fn) g_pad_player_pos_fn = gf_rr_pos; }   /* v1129RR */
    {   /* v1111T: every folder streamed under godfather_v4 (first 300 unique) -> find the tailor interiors outside world_terrain */
        const char* v = strstr(g, "godfather_v4"); if (v) { char d[160]; snprintf(d, sizeof d, "%s", v); char* sl = strrchr(d, '/'); char* bs = strrchr(d, '\\'); if (bs > sl) sl = bs; if (sl) *sl = 0;
            static uint32_t seen[300]; static int ns; uint32_t h = 2166136261u; for (const char* q = d; *q; q++) h = (h ^ (uint8_t)*q) * 16777619u;
            int k = 0; for (int i = 0; i < ns; i++) if (seen[i] == h) { k = 1; break; }
            if (!k && ns < 300) { seen[ns++] = h; printf("[tailor] streams from %s\n", d); fflush(stdout); } } }
    const char* w = strstr(g, "world_terrain/"); if (!w) w = strstr(g, "world_terrain\\"); if (!w) return;
    const char* q = w + 14; char seg[3][64]; int ns = 0;
    while (*q && ns < 3) { int k = 0; while (*q && *q != '/' && *q != '\\' && k < 63) seg[ns][k++] = *q++; seg[ns][k] = 0; ns++; if (*q) q++; else break; }
    if (ns < 3) return;
    char nm[64]; snprintf(nm, sizeof nm, "%s", seg[2]); { char* d = strstr(nm, ".str"); if (d) *d = 0; }
    { char* z = strstr(nm, "_z0"); if (z) *z = 0; }
    { static char seen[64][48]; static int ns; int k = 0; for (int i = 0; i < ns; i++) if (!strcmp(seen[i], nm)) { k = 1; break; }   /* v1111T: every interior that streams in (find the tailors) */
      if (!k && ns < 64) { snprintf(seen[ns++], 48, "%s", nm); float pp[3] = { 0, 0, 0 }; const uint32_t pl0 = player(); if (pl0) { __try { player_pos(pl0, pp); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
          printf("[tailor] interior streamed in: %s (player at %.1f %.1f %.1f)\n", nm, (double)pp[0], (double)pp[1], (double)pp[2]); fflush(stdout); } }
    const int ty = classify(nm); if (ty == ST_NONE) return;
    float p[3] = { 0, 0, 0 }; int hp = 0; const uint32_t pl = player();
    if (pl) { __try { hp = player_pos(pl, p); } __except (EXCEPTION_EXECUTE_HANDLER) { hp = 0; } }
    while (InterlockedExchange(&s_seen_lock, 1)) Sleep(0);
    int k = -1; for (int i = 0; i < NSEEN; i++) if (!strcmp(s_seen[i].name, nm)) { k = i; break; }
    if (k < 0) { k = 0; for (int i = 1; i < NSEEN; i++) if (s_seen[i].t < s_seen[k].t) k = i; }
    const int fresh = strcmp(s_seen[k].name, nm) != 0 || now_s() - s_seen[k].t > 30.0;
    snprintf(s_seen[k].name, sizeof s_seen[k].name, "%s", nm); s_seen[k].type = ty; s_seen[k].t = now_s();
    if (hp) { memcpy(s_seen[k].p, p, sizeof p); s_seen[k].have_p = 1; }
    InterlockedExchange(&s_seen_lock, 0);
    if (fresh) SLOG("[shop] business interior streamed in: %s -> %s (player at %.1f %.1f %.1f)\n", nm, k_st_name[ty], (double)p[0], (double)p[1], (double)p[2]);
}
static int indoors(const float* p)
{
    const float o[3] = { p[0], p[1] + 1.9f, p[2] }, d[3] = { 0, 1, 0 }; float t, n[3];
    return gf_rag_ray(o, d, 6.0f, &t, n);   /* a room's ceiling (b2_t1: 14 m caught a store from the street) */
}
/* v1110S (user: "the game always says press B for butcher", "make shopping more restrictive, the player has to get closer to the
 * counter", "there's a tobacco shop in the safehouse?"). The store used to be "where the PLAYER stood when its interior streamed
 * in" + 20 m + indoors: interiors stream in from the street or a neighbouring building (the user's log: the butcher streamed in at
 * the start point, the tobacconist when he entered the safehouse building), so the wrong rooms became stores. Now:
 *  - the store = the REAL world box of the interior (assets/gf_shops.txt, tools/gf_shops_build.py: union of its placed collision
 *    boxes; 17 business interiors), ground floor only (feet <= base + 3.5 m), indoors (ceiling);
 *  - buying (B, the prompt, the page) only AT THE COUNTER: a flat surface 0.75..1.3 m above the feet within arm's reach (rays down
 *    around the player at 0.5 / 0.8 / 1.1 m, 12 directions; level collision gf_rag_ray), kept while within 1.6 m of that spot.
 *  No gf_shops.txt -> no stores (never the old guess). GF_SHOP_COUNTER=0: anywhere inside the store box. Log [shop] v1110S. */
typedef struct { char name[64]; int type; float lo[3], hi[3]; } shopbox_t;
static shopbox_t s_box[48]; static int s_nbox = -1, s_counter = -1;
static void boxes_load(void)
{
    s_nbox = 0;
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH); { char* sl = strrchr(exe, '\\'); if (sl) *sl = 0; }
    static const char* rel[] = { "\\assets\\gf_shops.txt", "\\..\\assets\\gf_shops.txt", "\\..\\..\\assets\\gf_shops.txt" };
    FILE* f = NULL; char path[MAX_PATH + 64];
    for (int i = 0; i < 3 && !f; i++) { snprintf(path, sizeof path, "%s%s", exe, rel[i]); f = fopen(path, "r"); }
    if (!f) { SLOG("[shop] v1110S no assets\\gf_shops.txt (tools/gf_shops_build.py) -> no stores\n"); return; }
    char ln[256];
    while (fgets(ln, sizeof ln, f) && s_nbox < 48) {
        if (ln[0] == '#') continue;
        shopbox_t b; memset(&b, 0, sizeof b);
        if (sscanf(ln, "%63s %f %f %f %f %f %f", b.name, &b.lo[0], &b.lo[1], &b.lo[2], &b.hi[0], &b.hi[1], &b.hi[2]) != 7) continue;
        b.type = classify(b.name); if (b.type == ST_NONE) continue;
        s_box[s_nbox++] = b;
    }
    fclose(f);
    if (s_nbox < 48) {   /* v1112A Gabriel's Tailoring (little_italy/st_mulberry/shop028_barber_tattoo_369): prop bboxes of its interior */
        shopbox_t b; memset(&b, 0, sizeof b); snprintf(b.name, sizeof b.name, "shop028_barber_tattoo_369");
        b.type = ST_TAILOR; b.lo[0] = -66.3f; b.lo[1] = -1.0f; b.lo[2] = -202.4f; b.hi[0] = -46.8f; b.hi[1] = 4.5f; b.hi[2] = -190.9f; s_box[s_nbox++] = b; }
    { const char* e = getenv("GF_SHOP_COUNTER"); s_counter = (e && *e) ? atoi(e) : 1; }
    SLOG("[shop] v1110S %d stores from %s (real interior boxes, ground floor); buying: %s (GF_SHOP_COUNTER 1 shopkeeper / 2 counter surface / 0 anywhere)\n", s_nbox, path,
         s_counter == 1 ? "next to the shopkeeper" : s_counter == 2 ? "at a counter-height surface" : "anywhere in the store");
}
static int box_at(const float* p)
{
    for (int i = 0; i < s_nbox; i++) {
        const shopbox_t* b = &s_box[i]; const float base = b->lo[1] < -1.0f ? 0.0f : b->lo[1];
        if (p[0] > b->lo[0] + 0.3f && p[0] < b->hi[0] - 0.3f && p[2] > b->lo[2] + 0.3f && p[2] < b->hi[2] - 0.3f &&
            p[1] > b->lo[1] - 0.5f && p[1] < base + 3.5f) return i;
    }
    return -1;
}
static float s_ctr_p[3]; static int s_ctr_ok;
static int at_counter(const float* p)
{
    if (s_ctr_ok) { const float dx = p[0] - s_ctr_p[0], dz = p[2] - s_ctr_p[2];
        if (dx * dx + dz * dz < 1.6f * 1.6f && fabsf(p[1] + 1.0f - s_ctr_p[1]) < 0.6f) return 1; s_ctr_ok = 0; }
    static const float rr[3] = { 0.5f, 0.8f, 1.1f };
    for (int r = 0; r < 3; r++) for (int k = 0; k < 12; k++) {
        const float a = (float)k * 0.5235988f, x = p[0] + sinf(a) * rr[r], z = p[2] + cosf(a) * rr[r];
        const float o[3] = { x, p[1] + 1.6f, z }, d[3] = { 0, -1, 0 }; float t, n[3];
        if (!gf_rag_ray(o, d, 1.2f, &t, n)) continue;
        const float h = 1.6f - t;   /* above the feet */
        if (h >= 0.75f && h <= 1.3f && n[1] > 0.85f) { s_ctr_p[0] = x; s_ctr_p[1] = p[1] + h; s_ctr_p[2] = z; s_ctr_ok = 1; return 1; }
    }
    return 0;
}
/* v1110T (user on v1110S: "couldn't buy from the tobacco shop, and at a bar even the tables count as a counter"). Collision has
 * no "counter": in the opium bar the tables are 0.8 m blocks at the bar's height; the tobacconist (bomb-mission layout) has no clear
 * counter at all. A store's counter is where its SHOPKEEPER stands: buy when an NPC is within GF_SHOP_KEEPER_R (2.6 m), inside the
 * same store box, has stood still >= 2 s, and was not spawned by the street population (requester vt 0x1004B5E8 = passers-by who
 * walked in; gf_world_spawn_info, always-on registry in gf_coop_worldre.c). While inside a store the people in it are logged every
 * 3 s (class, owner, requester vt, still / moving, distance) -- "[shop] v1110T in the store: ..." -- to tune this from a real visit. */
extern int gf_npcs_within(float maxd, uint32_t* out, int max);
extern int gf_world_spawn_info(uint32_t ent, uint32_t* hash, uint32_t* owner, uint32_t* req_vt);
static void keeper_note(uint32_t ent, uint32_t owner, int box);   /* v1110X, below */
typedef struct { uint32_t ent; float p[3]; double still_since, seen; } still_t;
static still_t s_still[32];
static double still_for(uint32_t e, const float* p, double now)
{
    int k = -1, fr = 0; double old = 1e30;
    for (int i = 0; i < 32; i++) { if (s_still[i].ent == e) { k = i; break; } if (s_still[i].seen < old) { old = s_still[i].seen; fr = i; } }
    if (k < 0) { k = fr; s_still[k].ent = e; memcpy(s_still[k].p, p, 12); s_still[k].still_since = now; }
    const float dx = p[0] - s_still[k].p[0], dz = p[2] - s_still[k].p[2];
    if (dx * dx + dz * dz > 0.6f * 0.6f || now - s_still[k].seen > 2.0) { memcpy(s_still[k].p, p, 12); s_still[k].still_since = now; }
    s_still[k].seen = now;
    return now - s_still[k].still_since;
}
static int in_box(int bi, const float* p) { const shopbox_t* b = &s_box[bi]; return p[0] > b->lo[0] && p[0] < b->hi[0] && p[2] > b->lo[2] && p[2] < b->hi[2] && p[1] > b->lo[1] - 0.5f && p[1] < b->hi[1]; }
static uint32_t near_keeper(int bi, const float* pp)
{
    static float r = -1.0f; if (r < 0.0f) { const char* e = getenv("GF_SHOP_KEEPER_R"); r = (e && atof(e) > 0.5) ? (float)atof(e) : 2.6f; }
    uint32_t L[48]; const int n = gf_npcs_within(9.0f, L, 48); const double now = now_s(); uint32_t got = 0; float gd = 1e9f;
    static double t_log; const int log = now - t_log > 3.0; char buf[900]; int o = 0; buf[0] = 0;
    for (int i = 0; i < n; i++) {
        float p[3]; if (!player_pos(L[i], p) || !in_box(bi, p)) continue;
        const double st = still_for(L[i], p, now);
        uint32_t h = 0, ow = 0, rv = 0; const int known = gf_world_spawn_info(L[i], &h, &ow, &rv);
        const float dx = p[0] - pp[0], dz = p[2] - pp[2], d = sqrtf(dx * dx + dz * dz);
        const int street = known && rv == 0x1004B5E8u;
        if (known && ow && !rv && fok(rdf(L[i] + 0x14C8u)) && rdf(L[i] + 0x14C8u) > 0.0f) keeper_note(L[i], ow, bi);   /* v1110X */
        if (d < r && d < gd && st >= 2.0 && !street && fabsf(p[1] - pp[1]) < 1.2f) { got = L[i]; gd = d; }
        if (log && o < (int)sizeof buf - 120) o += snprintf(buf + o, sizeof buf - (size_t)o, " [%08X %.1f m %s%.0fs class %08X owner %08X req %08X%s]",
            L[i], d, st >= 2.0 ? "still " : "moving ", st, h, ow, rv, known ? (street ? " street" : "") : " unknown");
    }
    if (log) { t_log = now; static int nl; if (nl++ < 60) { printf("[shop] v1110T in the store: %s%s\n", buf[0] ? buf : " nobody", got ? "  -> SHOPKEEPER next to the player" : ""); fflush(stdout); } }
    return got;
}
/* v1110U (user: "make them refuse to sell / not show the prompt when they're scared or beaten the shit out of, and do the camera
 * thing like with a dialogue"). Refuses (no prompt, page closed, one toast per approach): health < GF_SHOP_HURT (0.9) of max = beaten;
 * intimidated in a street dialogue within GF_SHOP_SCARED_S (300 s); a gun pointed at him (free-aim target) -> scared for 120 s.
 * Camera: while the store page is drawn (a_title(2) stamps s_page_t on the render thread) the dialogue two-shot of gf_footcam.c
 * (g_gfd_cam_eye / _tgt / _on, same framing as gf_dialogue.c mode_tick: GF_DLG_CAM_*) frames the player and the shopkeeper; off
 * when the page closes (only if this file turned it on). GF_SHOP_CAM=0 off. */
static void toast(const char* fmt, ...); static volatile int s_req_page;   /* defined below */
extern long long gf_dialogue_threat_age_ms(uint32_t ent);
extern uint32_t gf_aim_target_now(void);
extern int gf_npc_name(uint32_t ent, char* out, int cap);
extern volatile int g_gfd_cam_on; extern float g_gfd_cam_eye[3], g_gfd_cam_tgt[3];
static volatile double s_page_t; static uint32_t s_keeper; static int s_cam_mine;
static struct { uint32_t ent; double t; } s_gunned[16]; static unsigned s_gunned_i;
static const char* keeper_refuses(uint32_t k)
{
    static float hurt = -1.0f, scared_s; if (hurt < 0.0f) { const char* e = getenv("GF_SHOP_HURT"); hurt = e && atof(e) > 0 ? (float)atof(e) : 0.9f;
        e = getenv("GF_SHOP_SCARED_S"); scared_s = e && atof(e) > 0 ? (float)atof(e) : 300.0f; }
    const double now = now_s();
    if (gf_aim_target_now() == k) { int j = -1; for (int i = 0; i < 16; i++) if (s_gunned[i].ent == k) j = i; if (j < 0) j = (int)(s_gunned_i++ % 16u); s_gunned[j].ent = k; s_gunned[j].t = now; return "he's staring down your gun"; }
    for (int i = 0; i < 16; i++) if (s_gunned[i].ent == k && now - s_gunned[i].t < 120.0) return "he's too scared after you pulled a gun on him";
    const float hp = rdf(k + 0x14C8u), hm = rdf(k + 0x14CCu);
    if (fok(hp) && fok(hm) && hm > 0.0f && hp < hurt * hm) return "not after the beating you gave him";
    const long long ta = gf_dialogue_threat_age_ms(k);
    if (ta >= 0 && ta < (long long)(scared_s * 1000.0f)) return "he's too scared of you";
    return NULL;
}
static void shop_cam(uint32_t pl)
{
    static int on = -1; if (on < 0) { const char* e = getenv("GF_SHOP_CAM"); on = !(e && *e == '0'); }
    const int open = s_here && s_keeper && now_s() - s_page_t < 0.2;
    float P[3], N[3];
    if (on && open && player_pos(pl, P) && player_pos(s_keeper, N)) {
        float dx = N[0] - P[0], dz = N[2] - P[2]; const float L = sqrtf(dx * dx + dz * dz);
        if (L > 0.05f) { dx /= L; dz /= L; const float rx = dz, rz = -dx;
            static float back = -1.0f, side, up, look; if (back < 0.0f) { const char* e;
                back = (e = getenv("GF_DLG_CAM_BACK")) && atof(e) > 0 ? (float)atof(e) : 1.35f; side = (e = getenv("GF_DLG_CAM_SIDE")) && *e ? (float)atof(e) : 0.75f;
                up = (e = getenv("GF_DLG_CAM_UP")) && *e ? (float)atof(e) : 1.72f; look = (e = getenv("GF_DLG_CAM_LOOK")) && *e ? (float)atof(e) : 0.62f; }
            g_gfd_cam_eye[0] = P[0] - dx * back + rx * side; g_gfd_cam_eye[1] = P[1] + up; g_gfd_cam_eye[2] = P[2] - dz * back + rz * side;
            g_gfd_cam_tgt[0] = P[0] + (N[0] - P[0]) * look; g_gfd_cam_tgt[1] = (P[1] + N[1]) * 0.5f + 1.52f; g_gfd_cam_tgt[2] = P[2] + (N[2] - P[2]) * look;
            if (!s_cam_mine) SLOG("[shop] v1110U store camera on (shopkeeper %08X, %.1f m)\n", s_keeper, (double)L);
            g_gfd_cam_on = 1; s_cam_mine = 1; return; }
    }
    if (s_cam_mine) { g_gfd_cam_on = 0; s_cam_mine = 0; }
}
/* v1110X (user: "when I kill one of the NPCs in a store or make them leave, and turn my back, a new one spawns"). Not the street
 * population (v1101P only scales requester vt 0x1004B5E8): store people are spawned by the store with an OWNER id and requester 0
 * (doctor's office log: class BA1F9E7B owner 0x128 / 0x129 req 0), and the store refills an empty slot as soon as it is unseen.
 * Each owner-id NPC seen inside a store box is remembered; when it dies (health <= 0), leaves the box (> 1 m outside) or is gone,
 * that owner is "lost": the factory (gf_world_factory_pre, v1060W1 wrapper -> r3 = 0) refuses new spawns with that owner while the
 * player is within GF_SHOP_RESPAWN_R (70 m) of the store, for GF_SHOP_RESPAWN_S (600 s). Leaving the area allows it again ("come back
 * later"). GF_SHOP_NORESPAWN=0 = old. Log "[shop] v1110X ...". */
typedef struct { uint32_t owner, ent; int box, lost; double t_lost, t_seen; } skeep_t;
static skeep_t s_sk[24]; static int s_nr_on = -1;
static void keeper_note(uint32_t ent, uint32_t owner, int box)
{
    int k = -1, fr = -1; for (int i = 0; i < 24; i++) { if (s_sk[i].owner == owner && s_sk[i].box == box) { k = i; break; } if (fr < 0 && !s_sk[i].owner) fr = i; }
    if (k < 0) { if (fr < 0) { fr = 0; for (int i = 1; i < 24; i++) if (s_sk[i].t_seen < s_sk[fr].t_seen) fr = i; } k = fr; }
    if (s_sk[k].lost && s_sk[k].ent != ent) SLOG("[shop] v1110X owner %08X is back in %s as %08X (not refused: player left / timed out)\n", owner, s_box[box].name, ent);
    s_sk[k].owner = owner; s_sk[k].ent = ent; s_sk[k].box = box; s_sk[k].lost = 0; s_sk[k].t_seen = now_s();
}
static void keepers_check(void)
{
    const double now = now_s();
    for (int i = 0; i < 24; i++) { skeep_t* k = &s_sk[i]; if (!k->owner || k->lost) continue;
        uint32_t h, ow, rv; float p[3]; const char* why = NULL;
        if (!gf_world_spawn_info(k->ent, &h, &ow, &rv) || ow != k->owner) why = "gone";
        else { const float hp = rdf(k->ent + 0x14C8u);
            if (!(fok(hp) && hp > 0.0f)) why = "died";
            else if (player_pos(k->ent, p)) { const shopbox_t* b = &s_box[k->box];
                if (p[0] < b->lo[0] - 1.0f || p[0] > b->hi[0] + 1.0f || p[2] < b->lo[2] - 1.0f || p[2] > b->hi[2] + 1.0f) why = "left the store"; } }
        if (why) { k->lost = 1; k->t_lost = now; SLOG("[shop] v1110X %s person %08X (owner %08X) %s -- no replacement while you are nearby\n", s_box[k->box].name, k->ent, k->owner, why); } }
}
int gf_shop_refuse_spawn(uint32_t hash, uint32_t owner)   /* main PPU thread (factory); 1 = refuse */
{
    if (s_nr_on < 0) { const char* e = getenv("GF_SHOP_NORESPAWN"); s_nr_on = !(e && *e == '0'); }
    if (!s_nr_on || s_nbox <= 0) return 0;
    static float rr = -1.0f, rs; if (rr < 0.0f) { const char* e = getenv("GF_SHOP_RESPAWN_R"); rr = e && atof(e) > 1 ? (float)atof(e) : 70.0f;
        e = getenv("GF_SHOP_RESPAWN_S"); rs = e && atof(e) > 1 ? (float)atof(e) : 600.0f; }
    for (int i = 0; i < 24; i++) { skeep_t* k = &s_sk[i]; if (!k->lost || k->owner != owner) continue;
        const double age = now_s() - k->t_lost; if (age > rs) { k->owner = 0; return 0; }
        float pp[3]; const uint32_t pl = player(); if (!pl || !player_pos(pl, pp)) return 0;
        const shopbox_t* b = &s_box[k->box]; const float cx = (b->lo[0] + b->hi[0]) * 0.5f, cz = (b->lo[2] + b->hi[2]) * 0.5f;
        if ((pp[0] - cx) * (pp[0] - cx) + (pp[2] - cz) * (pp[2] - cz) > rr * rr) { k->owner = 0; return 0; }   /* left the area: allowed again */
        { static int nl; if (nl++ < 30) SLOG("[shop] v1110X refused a replacement for owner %08X (class %08X) in %s (%.0f s after)\n", owner, hash, b->name, age); }
        return 1; }
    return 0;
}
static void update_here(uint32_t pl)   /* main thread */
{
    int ty = ST_NONE; char nm[64] = ""; static int s_in = -1;
    if (s_nbox < 0) boxes_load();
    keepers_check();   /* v1110X */
    if (s_any) { ty = ST_GENERAL; snprintf(nm, sizeof nm, "test"); }
    else if (pl && !player_in_car(pl)) {
        float p[3];
        if (player_pos(pl, p)) {
            const int bi = box_at(p);
            if (bi >= 0 && s_box[bi].type != ST_TAILOR && !indoors(p)) { }   /* inside the box but under the open sky: not in the store (v1112B: the tailor has no ceiling collision) */
            else if (bi >= 0) {
                if (bi != s_in) { s_in = bi; SLOG("[shop] v1110S inside %s (%s)%s\n", k_st_name[s_box[bi].type], s_box[bi].name, s_counter ? " -- buy from the shopkeeper" : ""); }
                uint32_t kp = 0; static uint32_t s_refused;
                if (s_box[bi].type == ST_TAILOR) { ty = ST_TAILOR; snprintf(nm, sizeof nm, "%s", s_box[bi].name); }   /* v1112A: anywhere in the shop */
                else if (s_counter == 1) {
                    kp = near_keeper(bi, p);
                    const char* why = kp ? keeper_refuses(kp) : NULL;
                    if (why) { if (s_refused != kp) { s_refused = kp; char who[64] = ""; gf_npc_name(kp, who, sizeof who);
                                   toast("%s won't serve you - %s", who[0] ? who : ui_tr("The shopkeeper"), why); SLOG("[shop] v1110U %08X refuses: %s\n", kp, why); }
                               kp = 0; if (now_s() - s_page_t < 0.2) s_req_page = -1; }
                    else if (kp) s_refused = 0;
                }
                if (s_box[bi].type != ST_TAILOR && (s_counter == 0 || (s_counter == 2 ? at_counter(p) : kp != 0))) { ty = s_box[bi].type; snprintf(nm, sizeof nm, "%s", s_box[bi].name); }
                if (kp) s_keeper = kp;
            }
            if (bi < 0 && s_in >= 0) { s_in = -1; s_ctr_ok = 0; }
        }
    }
    if (ty != s_here || strcmp(nm, s_here_name)) {
        if (ty) SLOG("[shop] at the counter: %s (%s)\n", k_st_name[ty], nm); else if (s_here) SLOG("[shop] left the counter\n");
        snprintf(s_here_name, sizeof s_here_name, "%s", nm); s_here = ty;
    }
}

/* ------------------------------------------------------------------ items */
enum { EF_NONE, EF_SMOKE, EF_HEAL };
typedef struct { const char* id; const char* name; const char* desc; int price, per_buy, effect; float heal; unsigned stores; int start; } item_t;
#define SB(x) (1u << (x))
static const item_t k_items[] = {
    { "cigarettes", "Cigarettes", "A pack of twenty. Light one up with J, or use it here. Sprinting, fighting or getting hurt puts it out.",
      15, 20, EF_SMOKE, 0, SB(ST_TOBACCO) | SB(ST_BAR) | SB(ST_GENERAL), 20 },
    { "cigar", "Havana Cigar", "Hand-rolled, from the old country by way of Havana. Smoked like a cigarette, savoured like a reward.",
      40, 1, EF_SMOKE, 0, SB(ST_TOBACCO) | SB(ST_GENERAL), 0 },
    { "whiskey", "Whiskey", "A stiff drink to steady the nerves. Restores some health.", 25, 1, EF_HEAL, 0.30f,
      SB(ST_BAR) | SB(ST_REST) | SB(ST_GENERAL), 0 },
    { "chianti", "Bottle of Chianti", "Red wine from Tuscany. Restores a little health.", 30, 1, EF_HEAL, 0.20f, SB(ST_REST) | SB(ST_BAR), 0 },
    { "pasta", "Plate of Pasta", "Mama's recipe. A full meal restores a good share of health.", 20, 1, EF_HEAL, 0.40f, SB(ST_REST) | SB(ST_GENERAL), 0 },
    { "cannoli", "Cannoli", "Leave the gun, take the cannoli. Restores a little health.", 6, 1, EF_HEAL, 0.12f, SB(ST_BAKERY) | SB(ST_GENERAL), 0 },
    { "bread", "Loaf of Bread", "Fresh this morning. Restores a little health.", 4, 1, EF_HEAL, 0.08f, SB(ST_BAKERY), 0 },
    { "salami", "Salami", "Cured, sliced thick. Restores a little health.", 8, 1, EF_HEAL, 0.15f, SB(ST_BUTCHER) | SB(ST_GENERAL), 0 },
    { "bandages", "Bandages", "Stops the bleeding. Restores a good share of health.", 50, 1, EF_HEAL, 0.50f, SB(ST_DOCTOR) | SB(ST_GENERAL), 0 },
    { "morphine", "Morphine", "The doctor asks no questions. Restores all health.", 150, 1, EF_HEAL, 1.00f, SB(ST_DOCTOR), 0 },
    { "roses", "Bouquet of Roses", "Red roses. A gift for someone. Not much use in a gunfight.", 12, 1, EF_NONE, 0, SB(ST_FLORIST) | SB(ST_GENERAL), 0 },
};
#define NITEMS ((int)(sizeof k_items / sizeof k_items[0]))
static volatile int s_cnt[NITEMS];
static int s_loaded = 0, s_dirty = 0;
static void inv_path(char* out, size_t n)
{
    char exe[MAX_PATH]; DWORD k = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (k && k < MAX_PATH) { char* sl = strrchr(exe, '\\'); if (sl) *sl = 0; snprintf(out, n, "%s\\gf_inventory.txt", exe); }
    else snprintf(out, n, "gf_inventory.txt");
}
static void inv_load(void)
{
    s_loaded = 1;
    char pth[MAX_PATH + 32]; inv_path(pth, sizeof pth);
    FILE* f = fopen(pth, "r");
    if (!f) { for (int i = 0; i < NITEMS; i++) s_cnt[i] = k_items[i].start; SLOG("[shop] new inventory (%s)\n", pth); return; }
    for (int i = 0; i < NITEMS; i++) s_cnt[i] = 0;
    char id[48]; int c;
    while (fscanf(f, "%47s %d", id, &c) == 2) for (int i = 0; i < NITEMS; i++) if (!strcmp(id, k_items[i].id)) s_cnt[i] = c < 0 ? 0 : c > 9999 ? 9999 : c;
    fclose(f);
    SLOG("[shop] inventory loaded from %s\n", pth);
}
static void inv_save(void)
{
    char pth[MAX_PATH + 32]; inv_path(pth, sizeof pth);
    FILE* f = fopen(pth, "w"); if (!f) return;
    fprintf(f, "# Godfather recomp inventory (gf_shop.c): item count\n");
    for (int i = 0; i < NITEMS; i++) fprintf(f, "%s %d\n", k_items[i].id, s_cnt[i]);
    fclose(f); s_dirty = 0;
}

/* ------------------------------------------------------------------ toasts / requests */
static char s_toast[128]; static volatile LONG s_toast_seq, s_toast_read;
static void toast(const char* fmt, ...)
{
    extern const char* ui_tr(const char* en);   /* ui_i18n.c: game language */
    va_list a; va_start(a, fmt); vsnprintf(s_toast, sizeof s_toast, ui_tr(fmt), a); va_end(a);
    InterlockedIncrement(&s_toast_seq);
    SLOG("[shop] %s\n", s_toast);
}
void gf_shop_toast(const char* msg) { extern const char* ui_tr(const char* en); toast("%s", ui_tr(msg)); }
static volatile LONG s_rq_head, s_rq_tail; static volatile int s_rq[16];   /* (page << 8) | item index */
static void push_rq(int v) { const LONG h = s_rq_head; if (h - s_rq_tail >= 16) return; s_rq[h & 15] = v; InterlockedExchange(&s_rq_head, h + 1); }

/* gf_smoke.c: a smoke is starting -- take a cigarette (or the cigar that was used). 0 = none left */
static volatile int s_smoke_item = -1;
int gf_inv_smoke_take(void)
{
    static int on = -1; if (on < 0) { const char* e = getenv("GF_SHOP"); on = !(e && *e == '0'); }
    if (!on) return 1;
    if (!s_loaded) inv_load();
    int it = s_smoke_item; s_smoke_item = -1;
    if (it < 0 || s_cnt[it] <= 0) it = s_cnt[0] > 0 ? 0 : (s_cnt[1] > 0 ? 1 : -1);
    if (it < 0) { toast("Out of cigarettes - buy a pack at a tobacconist or a bar"); return 0; }
    s_cnt[it]--; s_dirty = 1;
    if (s_cnt[it] == 3 && it == 0) toast("Three cigarettes left");
    return 1;
}
static void use_item(uint32_t pl, int i)
{
    if (i < 0 || i >= NITEMS || s_cnt[i] <= 0) return;
    const item_t* it = &k_items[i];
    if (it->effect == EF_SMOKE) {
        if (player_in_car(pl) || gf_cutscene_active()) { toast("Not now"); return; }
        s_smoke_item = i; g_smoke_req = 1;   /* gf_smoke.c starts it (and takes the item) */
        return;
    }
    if (it->effect == EF_HEAL) {
        const float hp = rdf(pl + 0x14C8u), mx = rdf(pl + 0x14CCu);
        if (!fok(hp) || !fok(mx) || mx <= 0.0f || hp <= 0.0f) { toast("Not now"); return; }
        if (hp >= mx - 0.5f) { toast("Already at full health"); return; }
        float nh = hp + it->heal * mx; if (nh > mx) nh = mx;
        wrf(pl + 0x14C8u, nh);
        s_cnt[i]--; s_dirty = 1;
        toast("%s  +%d health", ui_tr(it->name), (int)(nh - hp + 0.5f));
        return;
    }
    toast("%s - keep it for someone special", ui_tr(it->name));
}
static void buy_item(int i)
{
    if (i < 0 || i >= NITEMS) return;
    const item_t* it = &k_items[i];
    if (!s_here || !(it->stores & SB(s_here))) { toast("Not sold here"); return; }
    const int m = money_get(); if (m < 0) { toast("Not now"); return; }
    if (m < it->price) { toast("Not enough cash - %s costs $%d", ui_tr(it->name), it->price); return; }
    if (s_cnt[i] + it->per_buy > 9999) { toast("You can't carry any more"); return; }
    if (!money_set(m - it->price)) { toast("The till is stuck - try again"); return; }
    s_cnt[i] += it->per_buy; s_dirty = 1;
    toast("Bought %s  -$%d", ui_tr(it->name), it->price);
}

/* ------------------------------------------------------------------ main-thread tick (gf_trainer.c main_tick) */
static int s_on = -1; static float s_test = 0.0f; static volatile int s_req_page = 0;
void gf_shop_tick(void)
{
    if (s_on < 0) {
        const char* e = getenv("GF_SHOP"); s_on = !(e && *e == '0');
        e = getenv("GF_SHOP_ANY"); s_any = e && *e == '1';
        e = getenv("GF_SHOP_RADIUS"); if (e && *e) { s_radius = (float)atof(e); if (!(s_radius > 3.0f)) s_radius = 20.0f; }
        e = getenv("GF_SHOP_TEST"); if (e) s_test = (float)atof(e);
        printf("[shop] v1092S inventory (I) + stores (B): %s%s\n", s_on ? "ON" : "off (GF_SHOP=0)", s_any ? ", a general store everywhere (GF_SHOP_ANY)" : ""); fflush(stdout);
    }
    if (!s_on) return;
    if (!s_loaded) inv_load();
    __try {
        const uint32_t pl = player();
        static double t_first, t_here;
        if (pl && !t_first) t_first = now_s();
        if (pl && now_s() - t_here > 0.25) { t_here = now_s(); update_here(pl); }
        if (pl) shop_cam(pl);   /* v1110U every frame */
        gf_tailor_tick();   /* v1111T */
        gf_tailor_edtest_tick(pl);   /* v1111V */
        gf_tailor_tick_v1112(pl);   /* v1112A in-world tailor */
        while (s_rq_tail != s_rq_head) {
            const int v = s_rq[s_rq_tail & 15]; InterlockedIncrement(&s_rq_tail);
            if (!pl) continue;
            if ((v >> 8) == 4) { if (s_tc_npend < 8) s_tc_pend[s_tc_npend++] = v & 0xFF; continue; }   /* v1116B colour / pattern step */
            if ((v >> 8) == 3) tl_buy(v & 0xFF); else if ((v >> 8) == 2) buy_item(v & 0xFF); else use_item(pl, v & 0xFF);   /* v1112A 3 = tailor */
        }
        if (s_test > 0 && t_first) {   /* soak screenshots */
            static int st = 0; const double el = now_s() - t_first;
            if (st == 0 && el > s_test) { st = 1; s_any = 1; s_req_page = 1; SLOG("[shop] test: inventory\n"); }
            else if (st == 1 && el > s_test + 6) { st = 2; s_req_page = 2; SLOG("[shop] test: store\n"); }
            else if (st == 2 && el > s_test + 9) { st = 3; push_rq((2 << 8) | 2); SLOG("[shop] test: buy whiskey\n"); }
            else if (st == 3 && el > s_test + 12) { st = 4; s_req_page = -1; s_any = 0; SLOG("[shop] test: closed\n"); }
        }
        static double t_save; if (s_dirty && now_s() - t_save > 2.0) { t_save = now_s(); inv_save(); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { s_on = 0; printf("[shop] fault -> off\n"); fflush(stdout); }
}

/* ------------------------------------------------------------------ page api (render thread: read + queue only) */
static int store_idx(int k) { int n = 0; for (int i = 0; i < NITEMS; i++) if (s_here && (k_items[i].stores & SB(s_here))) { if (n == k) return i; n++; } return -1; }
static int mouth_row(void) { return s_cnt[0] > 0 || s_cnt[1] > 0 || g_cig_mouth_on; }   /* v1093M: inventory row 0 */
static int inv_idx(int k) { int n = 0; for (int i = 0; i < NITEMS; i++) if (s_cnt[i] > 0) { if (n == k) return i; n++; } return -1; }
static const char* a_title(int page)
{
    static char t[64];
    if (page == 2) { s_page_t = now_s(); if (s_here == ST_TAILOR) return tl_title(); snprintf(t, sizeof t, "%s", k_st_name[s_here] && s_here ? k_st_name[s_here] : "Store"); return t; }   /* v1110U: page drawn */
    return "Inventory";
}
static int a_count(int page)
{
    if (page == 2 && s_here == ST_TAILOR) return tl_count();   /* v1112A */
    int n = page == 1 ? mouth_row() : 0;
    for (int i = 0; i < NITEMS; i++) if (page == 2 ? (s_here && (k_items[i].stores & SB(s_here))) : s_cnt[i] > 0) n++;
    return n;
}
static int a_row(int page, int k, uiov_shop_row* o)
{
    if (page == 2 && s_here == ST_TAILOR) return tl_row(k, o);   /* v1112A */
    if (page == 1 && mouth_row()) {   /* v1093M */
        if (k == 0) { snprintf(o->name, sizeof o->name, "Cigarette in Mouth"); snprintf(o->right, sizeof o->right, "%s", g_cig_mouth_on ? "On" : "Off");
            snprintf(o->desc, sizeof o->desc, "Keep one lit between your lips - it stays there through a gunfight. K toggles it, J takes a drag."); o->enabled = 1; return 1; }
        k--;
    }
    const int i = page == 2 ? store_idx(k) : inv_idx(k); if (i < 0) return 0;
    const item_t* it = &k_items[i];
    snprintf(o->name, sizeof o->name, "%s", it->name);
    snprintf(o->desc, sizeof o->desc, "%s", it->desc);
    if (page == 2) {
        const int m = money_get();
        if (it->per_buy > 1) snprintf(o->name, sizeof o->name, "%s (%d)", it->name, it->per_buy);
        snprintf(o->right, sizeof o->right, "$%d", it->price);
        if (s_cnt[i] > 0) snprintf(o->sub, sizeof o->sub, "have %d", s_cnt[i]);
        o->enabled = m >= it->price;
    } else {
        snprintf(o->right, sizeof o->right, "x %d", s_cnt[i]);
        o->enabled = it->effect != EF_NONE;
    }
    return 1;
}
static void a_activate(int page, int k)
{
    if (page == 2 && s_here == ST_TAILOR) { tl_activate(k); return; }   /* v1112A */
    if (page == 1 && mouth_row()) { if (k == 0) { g_cig_mouth_req = 1; return; } k--; }   /* v1093M */
    const int i = page == 2 ? store_idx(k) : inv_idx(k); if (i < 0) return;
    push_rq((page << 8) | i);
}
static int a_status(int page, char* t, size_t n)
{
    (void)page; const int m = money_get(); if (m < 0) return 0;
    char d[24]; snprintf(d, sizeof d, "%d", m); char o[32]; int L = (int)strlen(d), j = 0;
    for (int i = 0; i < L; i++) { o[j++] = d[i]; if ((L - i - 1) % 3 == 0 && i < L - 1) o[j++] = ','; } o[j] = 0;
    snprintf(t, n, "Cash  $%s", o); return 1;
}
static int a_prompt(char* t, size_t n) { if (!s_here) return 0; if (s_here == ST_TAILOR) return 0;   /* v1112K: no B prompt -- the tailor opens from Gabriel's own SHOP choice only */ snprintf(t, n, "Shop - %s", k_st_name[s_here]); return 1; }
static int a_toast(char* t, size_t n)
{
    const LONG q = s_toast_seq; if (q == s_toast_read) return 0;
    s_toast_read = q; snprintf(t, n, "%s", s_toast); return 1;
}
static int a_can_open(void) { return player() != 0 && !gf_cutscene_active(); }
static int a_request(void) { const int r = s_req_page; if (r) s_req_page = 0; return r; }
static int a_selected(int page, int i) { return page == 2 && s_here == ST_TAILOR ? tl_selected(i) : -1; }   /* v1112A */
static int a_back(int page) { return page == 2 && s_here == ST_TAILOR ? tl_back() : 0; }
static void tl_adjust(int k, int dir);
static void a_adjust(int page, int i, int dir) { if (page == 2 && s_here == ST_TAILOR) tl_adjust(i, dir); }   /* v1116B */
static const uiov_shop_api k_shop_api = { a_title, a_count, a_row, a_activate, a_status, a_prompt, a_toast, a_can_open, a_request, a_selected, a_back, a_adjust };


/* ================================================================== v1111T TAILOR PROBE (in-world tailor, RE stage 2)
 * One real tailor visit tells us what the apparel menu does: (1) every string the clothing natives hash (func_003FE410 at their
 * 14 call sites, apply-pending.ps1 block v1111T) = item / set / category names; (2) SetClothingItemStatus -> func_0038B79C args;
 * (3) the clothing map *(0x10181064)+0x2C (Dinkumware std::map hash -> status) diffed every second; (4) the player's MobFace holder
 * P = *(0x10181060) blob (+0x20 ptr / +0x24 size) checksum, the editor E = *(0x10180F88) state E+0x38, and every func_001BC370
 * (put the model on the player) with its duration. GF_TAILOR_PROBE=0 off. Log "[tailor]". */
static int s_tp = -1;
static int tp_on(void) { if (s_tp < 0) { const char* e = getenv("GF_TAILOR_PROBE"); s_tp = !(e && *e == '0'); printf("[tailor] v1111T probe %s (GF_TAILOR_PROBE)\n", s_tp ? "ON" : "off"); fflush(stdout); } return s_tp; }
static int tp_ok(uint32_t a, uint32_t n) { return a >= 0x10000u && a + n < 0xF0000000u; }
static uint32_t tp_rd32(uint32_t a) { uint32_t v = 0; if (!tp_ok(a, 4)) return 0; __try { v = rd32(a); } __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; } return v; }
static int tp_str(uint32_t a, char* out, int cap)
{
    int n = 0; out[0] = 0; if (!tp_ok(a, 1)) return 0;
    __try { for (; n < cap - 1; n++) { const uint8_t c = vm_base[a + (uint32_t)n]; if (!c) break; if (c < 0x20 || c > 0x7E) { n = 0; break; } out[n] = (char)c; } }
    __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    out[n] = 0; return n;
}
void gf_tailor_hash_note(uint32_t str, uint32_t lr)
{
    if (!tp_on()) return;
    char b[96]; if (!tp_str(str, b, sizeof b)) return;
    static uint32_t seen[512]; static int ns; uint32_t h = lr * 2654435761u; for (const char* q = b; *q; q++) h = h * 31u + (uint8_t)*q;
    for (int i = 0; i < ns; i++) if (seen[i] == h) return;
    if (ns < 512) seen[ns++] = h; else return;
    printf("[tailor] native @%06X hashes \"%s\"\n", lr, b); fflush(stdout);
}
void gf_tailor_set_note(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, double f1, double f31)
{
    if (!tp_on()) return;
    char b[96]; tp_str(r4, b, sizeof b);
    static int n; if (n++ < 300) { printf("[tailor] SetClothingItemStatus -> func_0038B79C(r3 %08X, \"%s\", r5 %08X, r6 %08X, f1 %g, f31 %g)\n", r3, b, r5, r6, f1, f31); fflush(stdout); }
}
static double tp_qpc(void) { static LARGE_INTEGER f; LARGE_INTEGER t; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)f.QuadPart; }
void gf_tailor_apply_mark(uint32_t pl, uint32_t mode, int phase)
{
    if (!tp_on()) return;
    static double t0; static uint32_t p0, m0;
    if (phase == 0) { t0 = tp_qpc(); p0 = pl; m0 = mode; return; }
    static int n; if (n++ < 100) { printf("[tailor] func_001BC370(player %08X, mode %u) put a model on the character in %.2f ms\n", p0, m0, (tp_qpc() - t0) * 1000.0); fflush(stdout); }
}
typedef struct { uint32_t h, v; } tp_kv;
static int tp_map(tp_kv* out, int max)
{
    const uint32_t mgr = tp_rd32(0x10181064u); if (!tp_ok(mgr, 0x40)) return -1;
    const uint32_t m = mgr + 0x2Cu; uint32_t head = 0, size = 0;
    for (int lay = 0; lay < 2 && !head; lay++) {   /* {alloc, _Myhead, _Mysize} or {_Myhead, _Mysize} */
        const uint32_t hh = tp_rd32(m + (lay ? 0u : 4u)), sz = tp_rd32(m + (lay ? 4u : 8u));
        if (tp_ok(hh, 0x18) && sz < 4096) { uint8_t nil = 0; __try { nil = vm_base[hh + 0x15]; } __except (EXCEPTION_EXECUTE_HANDLER) { nil = 0; } if (nil == 1) { head = hh; size = sz; } } }
    if (!head) return -2;
    /* in-order walk: start at head->left (smallest), successor via right subtree / parents */
    uint32_t nd = tp_rd32(head + 0x0); int n = 0, guard = 0;
    while (nd && nd != head && n < max && guard++ < 8192) {
        uint8_t nil = 1; __try { nil = vm_base[nd + 0x15]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; } if (nil) break;
        out[n].h = tp_rd32(nd + 0xC); out[n].v = tp_rd32(nd + 0x10); n++;
        uint32_t r = tp_rd32(nd + 0x8); uint8_t rn = 1; if (tp_ok(r, 0x18)) { __try { rn = vm_base[r + 0x15]; } __except (EXCEPTION_EXECUTE_HANDLER) { rn = 1; } }
        if (!rn) { nd = r; for (int k = 0; k < 64; k++) { const uint32_t l = tp_rd32(nd + 0x0); uint8_t ln = 1; if (tp_ok(l, 0x18)) { __try { ln = vm_base[l + 0x15]; } __except (EXCEPTION_EXECUTE_HANDLER) { ln = 1; } } if (ln) break; nd = l; } }
        else { uint32_t pa = tp_rd32(nd + 0x4); int k = 0; while (pa && pa != head && nd == tp_rd32(pa + 0x8) && k++ < 64) { nd = pa; pa = tp_rd32(pa + 0x4); } nd = pa; }
    }
    (void)size; return n;
}
/* v1111U: where does the apparel menu's 3D preview live? func_00394300 (MobFace native helper) looks the "mobface_player"
 * entity up by name and gives it the editor's live model E+0x24 through vt+0x108 -- apply-pending block v1111U calls this just
 * before that call. The menu watch below logs (1 s, while the editor exists) the preview entity, the player and the last world
 * camera object so we know whether the preview is a world entity we can stand in front of the shop mirror. */
static uint32_t s_pv_ent, s_pv_model; static int s_pv_n;
static int tp_pos(uint32_t ent, float* p)
{
    const uint32_t m = tp_rd32(ent + 0x68); if (!tp_ok(m, 0x50)) return 0;
    __try { p[0] = rdf(m + 0x40); p[1] = rdf(m + 0x44); p[2] = rdf(m + 0x48); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return fok(p[0]) && fok(p[1]) && fok(p[2]);
}
void gf_tailor_preview_note(uint32_t ent, uint32_t model)
{
    if (!tp_on()) return;
    s_pv_n++;
    if (ent == s_pv_ent && model == s_pv_model) return;
    float e[3] = { 0 }, pp[3] = { 0 }; const uint32_t pl = player();
    const int he = tp_pos(ent, e), hp = pl && tp_pos(pl, pp);
    static int n; if (n++ < 40) {
        printf("[tailor] v1111U preview entity %08X (vtable %08X, is the player %d) gets editor model %08X (call #%d) | at %s%.2f %.2f %.2f | player %08X at %.2f %.2f %.2f\n",
               ent, tp_rd32(ent), ent == pl, model, s_pv_n, he ? "" : "(no matrix) ", (double)e[0], (double)e[1], (double)e[2], pl, (double)pp[0], (double)pp[1], (double)pp[2]);
        fflush(stdout); }
    s_pv_ent = ent; s_pv_model = model;
}
extern uint32_t gf_footcam_cam_obj(void);
static void tp_menu_watch(uint32_t E)
{
    static int lines; static uint32_t lastE;
    if (E != lastE) { lines = 0; lastE = E; }
    if (!E || lines >= 15) return;
    lines++;
    const uint32_t pl = player(), cam = gf_footcam_cam_obj();
    float pp[3] = { 0 }, e[3] = { 0 }; const int hp = pl && tp_pos(pl, pp), he = s_pv_ent && tp_pos(s_pv_ent, e);
    printf("[tailor] v1111U menu: E %08X state %u head model E+24 %08X | preview %08X %s(%.2f %.2f %.2f) | player %08X %s(%.2f %.2f %.2f)",
           E, tp_rd32(E + 0x38), tp_rd32(E + 0x24), s_pv_ent, he ? "" : "no pos ", (double)e[0], (double)e[1], (double)e[2], pl, hp ? "" : "no pos ", (double)pp[0], (double)pp[1], (double)pp[2]);
    if (tp_ok(cam, 0x40)) { __try { printf(" | world cam %08X eye (%.2f %.2f %.2f) pitch %.3f yaw %.3f", cam, (double)rdf(cam + 0x8), (double)rdf(cam + 0xC), (double)rdf(cam + 0x10), (double)rdf(cam + 0x18), (double)rdf(cam + 0x1C)); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
    if (s_pv_ent) { const uint32_t m = tp_rd32(s_pv_ent + 0x68); if (tp_ok(m, 0x50)) { __try { printf(" | preview matrix"); for (int r = 0; r < 5; r++) printf(" [%.2f %.2f %.2f %.2f]", (double)rdf(m + r * 16), (double)rdf(m + r * 16 + 4), (double)rdf(m + r * 16 + 8), (double)rdf(m + r * 16 + 12)); } __except (EXCEPTION_EXECUTE_HANDLER) { } } }
    printf("\n"); fflush(stdout);
}
static void gf_tailor_tick(void)
{
    if (!tp_on()) return;
    static double t; const double n = now_s(); if (n - t < 1.0) return; t = n;
    static tp_kv prev[1024]; static int np = -1; tp_kv cur[1024];
    const int nc = tp_map(cur, 1024);
    if (nc >= 0) {
        if (np < 0) { printf("[tailor] clothing map: %d items at start:", nc); for (int i = 0; i < nc && i < 200; i++) printf(" %08X=%u", cur[i].h, cur[i].v); printf("\n"); fflush(stdout); }
        else {
            for (int i = 0; i < nc; i++) { int j; for (j = 0; j < np; j++) if (prev[j].h == cur[i].h) break;
                if (j == np) { static int k; if (k++ < 300) printf("[tailor] clothing map: NEW %08X = %u\n", cur[i].h, cur[i].v); }
                else if (prev[j].v != cur[i].v) { static int k2; if (k2++ < 300) printf("[tailor] clothing map: %08X %u -> %u\n", cur[i].h, prev[j].v, cur[i].v); } }
            fflush(stdout);
        }
        memcpy(prev, cur, sizeof(tp_kv) * (size_t)nc); np = nc;
    } else { static int k3; if (k3++ < 3) { printf("[tailor] clothing map not readable (%d)\n", nc); fflush(stdout); } }
    {   /* MobFace blob + editor state */
        const uint32_t P = tp_rd32(0x10181060u), E = tp_rd32(0x10180F88u);
        const uint32_t face = P ? tp_rd32(P + 0x1C) : 0, bp = face ? tp_rd32(face + 0x4) : 0, bs = face ? tp_rd32(face + 0x8) : 0;   /* v1111U: face object {model, blob, size} */
        uint32_t ck = 0; if (tp_ok(bp, bs) && bs && bs < 0x100000u) { __try { for (uint32_t i = 0; i < bs; i++) ck = ck * 131u + vm_base[bp + i]; } __except (EXCEPTION_EXECUTE_HANDLER) { ck = 0; } }
        const uint32_t es = E ? tp_rd32(E + 0x38) : 0xFFFFFFFFu;
        tp_menu_watch(E);   /* v1111U */
        static uint32_t pck = 1, pes = 0xEEEEEEEEu, psz, pface;
        if (ck != pck || es != pes || bs != psz || face != pface) { static int k4; if (k4++ < 200) { printf("[tailor] MobFace: P %08X blob %08X size %u checksum %08X face %08X | editor E %08X state %u\n", P, bp, bs, ck, face, E, es); fflush(stdout); } }
        if (ck != pck && ck) tv_blob_diff(bp, bs);   /* v1111V */
        pck = ck; pes = es; psz = bs; pface = face;
    }
}

/* ================================================================== v1111V TAILOR STAGE 3: the MobFace editor in GAMEPLAY
 * RE (2026-11-08, static, EBOOT.elf): the apparel / MobFace menu creates the editor E itself -- func_00394754 (screen enter)
 * -> new 0x20 controller (singleton *(0x10180F8C)) func_00394678 -> new 0x88 + ctor thunk func_00013ADC (= lib func_005E1BF4,
 * stores this into *(0x10180F88)) with r4 = *(0x101EC778) allocator, r5 = 0x10012630, r6 = 0; then E+0x38 = 1 if 0.
 * v1111W CORRECTION: thunk func_00010EAC = func_005E0830 is the editor SHUTDOWN (state := 5, releases E+0x34). The update is
 * thunk func_00011C3C = func_005E8484 (writes states 2/3/4/6); the game's load loop in func_00015304 calls it until state 4.
 * Tailor (screen+0x98 == 0): func_00010C9C(P) (= func_005E9340, parse the player's blob into E + generate; func_005E9170
 * returns 0 while E is null or E+0x38 != 4). Save (func_00393AFC): func_000141AC(P, 0) / face = func_000132AC(E, *(P+0x18)) /
 * func_000141AC(P, face). E vtable 0x10108DC8: +0 dtor func_005E230C, +4 deleting dtor func_005E0368.
 * TEST (F12 in gameplay, GF_TAILOR_EDTEST=0 off): create E, tick it to 4, parse the player's look, wait for the generate,
 * bake a new face, put it on the player (func_001BC370(player, 1)), delete E. Every step logged "[tailor] v1111V".
 * If this round trip works, live clothes = change the part between parse and save (next: the slider/part API).
 * Also: the face blob is diffed byte by byte on change (buy / equip at the tailor -> how clothing is encoded). */
static int tv_call(uint32_t addr, uint32_t toc, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t* ret)
{
    ppu_context* cur = g_active_ctx; gfn f = lookup(addr); static ppu_context c;
    if (!cur || !f) { printf("[tailor] v1111V func_%08X %s\n", addr, cur ? "not in the function table" : ": no PPU context"); fflush(stdout); return 0; }
    __try {
        memcpy(&c, cur, sizeof c);
        c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1000u) & ~0xFull; c.gpr[2] = toc;
        c.gpr[3] = a0; c.gpr[4] = a1; c.gpr[5] = a2; c.gpr[6] = a3; c.lr = 0;
        g_active_ctx = &c; f(&c);
        while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
        g_active_ctx = cur; if (ret) *ret = c.gpr[3]; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_active_ctx = cur; g_trampoline_fn = 0;
        printf("[tailor] v1111V guest call func_%08X FAULTED\n", addr); fflush(stdout); return 0;
    }
}
static int tv_g(uint32_t addr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t* ret) { return tv_call(addr, TOC, a0, a1, a2, a3, ret); }
enum { TV_EGLOB = 0x10180F88u, TV_CGLOB = 0x10180F8Cu, TV_PGLOB = 0x10181060u, TV_ALLOC = 0x101EC778u, TV_ARG5 = 0x10012630u, TV_EVT = 0x10108DC8u };
static int s_tv_on = -1, s_tv_st, s_tv_frames; static uint32_t s_tv_E, s_tv_lastst = 0xFFFFFFFFu; static double s_tv_t0, s_tv_ts;
static double tv_ms(void) { return (tp_qpc() - s_tv_t0) * 1000.0; }
static void tv_flags(const char* why)
{
    const uint32_t P = tp_rd32(TV_PGLOB), E = tp_rd32(TV_EGLOB);
    uint8_t f0 = 0, f1 = 0, f2 = 0; if (tp_ok(P, 0x40)) { __try { f0 = vm_base[P + 0x30]; f1 = vm_base[P + 0x31]; f2 = vm_base[P + 0x32]; } __except (EXCEPTION_EXECUTE_HANDLER) { } }
    printf("[tailor] v1111V %s: t %.0f ms | E %08X state %u head model E+24 %08X | P %08X flags +30 %u +31 %u +32 %u blob P+20 %08X\n", why, tv_ms(),
           E, E ? tp_rd32(E + 0x38) : 0xFFFFFFFFu, E ? tp_rd32(E + 0x24) : 0, P, f0, f1, f2, P ? tp_rd32(P + 0x20) : 0); fflush(stdout);
}
static void tv_destroy(const char* why)
{
    const uint32_t E = tp_rd32(TV_EGLOB);
    if (E && E == s_tv_E) {
        const uint32_t vt = tp_rd32(E);
        if (vt == TV_EVT) {
            const uint32_t opd = tp_rd32(vt + 4), fn = tp_rd32(opd), toc = tp_rd32(opd + 4);
            const int r = tv_call(fn, toc, E, 1, 0, 0, NULL);
            printf("[tailor] v1111V editor %08X deleted (%s, func_%08X %s) -> global now %08X\n", E, why, fn, r ? "ok" : "FAILED", tp_rd32(TV_EGLOB));
        } else printf("[tailor] v1111V editor %08X NOT deleted (%s): vtable %08X != %08X (left alive)\n", E, why, vt, TV_EVT);
        if (tp_rd32(TV_EGLOB) == E && vt == TV_EVT) wr32(TV_EGLOB, 0);
    } else printf("[tailor] v1111V destroy (%s): editor global %08X is not ours (%08X) -- left alone\n", why, E, s_tv_E);
    fflush(stdout); s_tv_E = 0; s_tv_st = 0;
}
/* v1111ZB: the user's 16:3x purchase (pants_05 + upper_db_48 + hats_03, equipped at the menu close) changed these big-endian
 * u32 fields of the 940-byte face blob (+0x58..+0x5B also changed but changes on EVERY re-serialisation = checksum/seed, untouched).
 * Shift+F12 = write the BEFORE values, Ctrl+F12 = the AFTER values, then the normal F12 round trip -> clothes change in gameplay. */
static const struct { uint16_t off; uint32_t before, after; } k_tv_outfit[] = {
    { 0x044, 0, 3 }, { 0x050, 0x0E, 0x21 }, { 0x054, 0, 3 }, { 0x0F0, 0, 1 }, { 0x140, 0, 2 }, { 0x144, 0, 4 }, { 0x148, 0, 2 },
    { 0x14C, 0, 3 }, { 0x1A4, 0, 4 }, { 0x1F4, 0, 9 }, { 0x1F8, 0, 6 }, { 0x1FC, 0x0B, 7 }, { 0x200, 0, 4 } };
static int s_tv_poke;   /* 0 plain round trip, 1 BEFORE outfit, 2 AFTER outfit, 3 slot request below */
/* v1111ZC: the five clothing slots of the face blob (BE u32 = the UIC partIndex; tools/gf_uic_apparel.py tables) */
enum { TV_SLOT_HATS, TV_SLOT_EYEWEAR, TV_SLOT_SHOES, TV_SLOT_UPPER, TV_SLOT_LOWER, TV_SLOT_N };
static const uint16_t k_tv_slot_off[TV_SLOT_N] = { 0x44, 0x48, 0x4C, 0x50, 0x54 };
static const char* const k_tv_slot_name[TV_SLOT_N] = { "Hats", "Eyewear", "Shoes", "UpperBody", "LowerBody" };
static const uint8_t k_tv_slot_count[TV_SLOT_N] = { 9, 6, 5, 46, 7 };   /* partIndex range seen in the tables (+1) */
static int s_tv_req_slot = -1, s_tv_req_idx = -1;   /* -1 idx = cycle to the next index */
static uint32_t tv_blob(uint32_t* size)
{
    const uint32_t P = tp_rd32(TV_PGLOB), face = P ? tp_rd32(P + 0x1C) : 0, bp = face ? tp_rd32(face + 4) : 0, bs = face ? tp_rd32(face + 8) : 0;
    if (size) *size = bs; return tp_ok(bp, bs) && bs >= 0x60 ? bp : 0;
}
static void tv_poke_slot(void)
{
    uint32_t bs = 0; const uint32_t bp = tv_blob(&bs); const int sl = s_tv_req_slot;
    if (!bp || sl < 0 || sl >= TV_SLOT_N) return;
    const uint32_t a = bp + k_tv_slot_off[sl], o = tp_rd32(a);
    const uint32_t v = s_tv_req_idx >= 0 ? (uint32_t)s_tv_req_idx : (o + 1) % k_tv_slot_count[sl];
    wr32(a, v); printf("[tailor] v1111ZC wear: %s partIndex %u -> %u\n", k_tv_slot_name[sl], o, v); fflush(stdout);
}
static void tv_poke_outfit(void)
{
    if (s_tv_poke == 3) { tv_poke_slot(); return; }   /* v1111ZC */
    if (!s_tv_poke) return;
    const uint32_t P = tp_rd32(TV_PGLOB), face = P ? tp_rd32(P + 0x1C) : 0, bp = face ? tp_rd32(face + 4) : 0, bs = face ? tp_rd32(face + 8) : 0;
    if (!tp_ok(bp, bs) || bs < 0x210) { printf("[tailor] v1111ZB outfit poke: no blob (face %08X blob %08X size %u)\n", face, bp, bs); fflush(stdout); return; }
    int n = 0; printf("[tailor] v1111ZB writing the %s outfit into the blob %08X:", s_tv_poke == 1 ? "BEFORE-purchase" : "AFTER-purchase", bp);
    for (size_t i = 0; i < sizeof k_tv_outfit / sizeof k_tv_outfit[0]; i++) {
        const uint32_t a = bp + k_tv_outfit[i].off, v = s_tv_poke == 1 ? k_tv_outfit[i].before : k_tv_outfit[i].after, o = tp_rd32(a);
        if (o != v) { wr32(a, v); n++; printf(" +%03X %u->%u", k_tv_outfit[i].off, o, v); } }
    printf(" (%d fields changed)\n", n); fflush(stdout);
}
static void tv_shutdown(const char* why)   /* v1111Z: EVERY exit goes through the editor shutdown (a raw delete leaked the
                                            * resource handle -> the next editor, incl. the game's tailor menu, stuck in state 2) */
{
    if (!s_tv_E || tp_rd32(TV_EGLOB) != s_tv_E) { printf("[tailor] v1111Z shutdown (%s): editor not ours any more (%08X)\n", why, tp_rd32(TV_EGLOB)); fflush(stdout); s_tv_E = 0; s_tv_st = 0; return; }
    printf("[tailor] v1111Z shutting the editor down (%s)\n", why); fflush(stdout);
    if (!tv_g(0x00010EACu, s_tv_E, 0, 0, 0, NULL)) { printf("[tailor] v1111Z shutdown faulted -- editor %08X left alive\n", s_tv_E); fflush(stdout); s_tv_E = 0; s_tv_st = 0; return; }
    tv_flags("shutdown called"); s_tv_st = 5; s_tv_frames = 0; s_tv_ts = tp_qpc(); s_tv_lastst = 5;
}
static void gf_tailor_edtest_tick(uint32_t pl)
{
    if (s_tv_on < 0) { const char* e = getenv("GF_TAILOR_EDTEST"); s_tv_on = !(e && *e == '0');
        printf("[tailor] v1111ZB gameplay MobFace editor test on F12 (Shift = before outfit, Ctrl = after outfit, Alt = next hat): %s (GF_TAILOR_EDTEST)\n", s_tv_on ? "ON" : "off"); fflush(stdout); }
    if (!s_tv_on) return;
    static int down0; DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const int down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F12) & 0x8000); const int press = down && !down0; down0 = down;
    switch (s_tv_st) {
    case 0: {
        if (!press) return;
        const uint32_t P = tp_rd32(TV_PGLOB), face = P ? tp_rd32(P + 0x1C) : 0;
        if (!pl || player_in_car(pl) || gf_cutscene_active()) { printf("[tailor] v1111V F12: not now (on foot, no cutscene)\n"); fflush(stdout); return; }
        if (tp_rd32(TV_EGLOB) || tp_rd32(TV_CGLOB)) { printf("[tailor] v1111V F12: an editor / MobFace screen already exists (E %08X, controller %08X)\n", tp_rd32(TV_EGLOB), tp_rd32(TV_CGLOB)); fflush(stdout); return; }
        if (!tp_ok(P, 0x40) || !tp_ok(face, 12)) { printf("[tailor] v1111V F12: no player MobFace (P %08X face %08X)\n", P, face); fflush(stdout); return; }
        s_tv_poke = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 1 : (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 2 : 0;   /* v1111ZB */
        if (GetAsyncKeyState(VK_MENU) & 0x8000) { s_tv_poke = 3; s_tv_req_slot = TV_SLOT_HATS; s_tv_req_idx = -1; }   /* v1111ZC Alt+F12: next hat */
        printf("[tailor] v1111ZB F12%s\n", s_tv_poke == 1 ? " + Shift: BEFORE-purchase outfit" : s_tv_poke == 2 ? " + Ctrl: AFTER-purchase outfit" : ": plain round trip"); fflush(stdout);
        s_tv_t0 = tp_qpc(); uint64_t mem = 0;
        if (!tv_g(0x000283D4u, 0x88, 0, 0, 0, &mem) || !tp_ok((uint32_t)mem, 0x88)) { printf("[tailor] v1111V new(0x88) failed (%08X)\n", (uint32_t)mem); fflush(stdout); return; }
        const uint32_t alloc = tp_rd32(TV_ALLOC);
        printf("[tailor] v1111V F12: creating the MobFace editor at %08X (allocator %08X) | player %08X, P %08X, face %08X model %08X blob %08X size %u\n",
               (uint32_t)mem, alloc, pl, P, face, tp_rd32(face), tp_rd32(face + 4), tp_rd32(face + 8)); fflush(stdout);
        if (!tv_g(0x00013ADCu, mem, alloc, TV_ARG5, 0, NULL)) return;
        s_tv_E = tp_rd32(TV_EGLOB);
        if (s_tv_E != (uint32_t)mem) { printf("[tailor] v1111V ctor did not publish the editor (global %08X) -- abort, %08X leaked\n", s_tv_E, (uint32_t)mem); fflush(stdout); s_tv_E = 0; return; }
        if (tp_rd32(s_tv_E + 0x38) == 0) wr32(s_tv_E + 0x38, 1);   /* as func_00394754 */
        tv_flags("editor created"); s_tv_st = 1; s_tv_ts = tp_qpc(); s_tv_lastst = 0xFFFFFFFFu; return; }
    case 1: case 3: {
        if (tp_rd32(TV_EGLOB) != s_tv_E) { printf("[tailor] v1111V editor global changed under us (%08X) -- test aborted\n", tp_rd32(TV_EGLOB)); fflush(stdout); s_tv_E = 0; s_tv_st = 0; return; }
        if (!tv_g(0x00011C3Cu, s_tv_E, 0, 0, 0, NULL)) { tv_shutdown("update faulted"); return; }   /* v1111W: 11C3C = func_005E8484 update (10EAC = func_005E0830 is the SHUTDOWN -> state 5) */
        s_tv_frames++;
        const uint32_t es = tp_rd32(s_tv_E + 0x38);
        if (es != s_tv_lastst) { char w[48]; snprintf(w, sizeof w, "state -> %u (frame %d)", es, s_tv_frames); tv_flags(w); s_tv_lastst = es; }
        if (tp_qpc() - s_tv_ts > 20.0) { tv_flags("TIMEOUT"); tv_shutdown("timeout"); return; }
        if (s_tv_st == 1 && es == 4) {
            uint64_t r = 0; tv_poke_outfit(); const double t = tp_qpc();   /* v1111ZB */
            const int okc = tv_g(0x00010C9Cu, tp_rd32(TV_PGLOB), 0, 0, 0, &r);
            printf("[tailor] v1111V parse+generate func_005E9340(P) %s -> %u in %.2f ms\n", okc ? "ok" : "FAILED", (uint32_t)r, (tp_qpc() - t) * 1000.0); fflush(stdout);
            if (!okc) { tv_shutdown("parse faulted"); return; }
            tv_flags("after parse"); s_tv_st = 3; s_tv_frames = 0; s_tv_ts = tp_qpc(); return;
        }
        if (s_tv_st == 3) {
            const uint32_t P = tp_rd32(TV_PGLOB); uint8_t busy = 0; __try { busy = vm_base[P + 0x31] | vm_base[P + 0x32]; } __except (EXCEPTION_EXECUTE_HANDLER) { busy = 1; }
            if (es == 4 && s_tv_frames >= 10) { if (busy) { printf("[tailor] v1111Z P busy flag set after the parse (ignored)\n"); fflush(stdout); } s_tv_st = 4; }   /* v1111Z: +0x32 came back on and blocked the bake */
        }
        return; }
    case 4: {
        const uint32_t P = tp_rd32(TV_PGLOB); uint64_t face = 0; double t = tp_qpc();
        const uint32_t oldface = tp_rd32(P + 0x1C);
        /* v1111ZA: detach the player's model FIRST, like the tailor screen enter (func_00394754 -> func_001B4340(player) =
         * player->vt+0x108(player, 0)). 141AC(P, 0) destroys the old baked model; destroying it while still attached hung
         * the main thread (v1111X / v1111Z). Re-attached by func_001BC370 below in the same tick. */
        if (!pl || !tv_g(0x001B4340u, pl, 0, 0, 0, NULL)) { printf("[tailor] v1111ZA could not detach the player's model -- no bake\n"); fflush(stdout); tv_shutdown("detach failed"); return; }
        int okc = tv_g(0x000141ACu, P, 0, 0, 0, NULL)
               && tv_g(0x000132ACu, s_tv_E, tp_rd32(P + 0x18), 0, 0, &face)
               && tp_ok((uint32_t)face, 12)
               && tv_g(0x000141ACu, P, face, 0, 0, NULL)
               && tv_g(0x00012B7Cu, s_tv_E, face, 0, 0, NULL);   /* v1111X: func_005E40D0(E, face) BUILDS face->model (func_005E3D70) */
        if (okc && !tp_ok(tp_rd32((uint32_t)face), 4)) { printf("[tailor] v1111X face %08X has NO model after func_005E40D0 -- not applied to the player\n", (uint32_t)face); okc = 0; }
        printf("[tailor] v1111X bake %s in %.2f ms: old face %08X -> new face %08X model %08X blob %08X size %u\n", okc ? "ok" : "FAILED", (tp_qpc() - t) * 1000.0,
               oldface, (uint32_t)face, tp_rd32((uint32_t)face), tp_rd32((uint32_t)face + 4), tp_rd32((uint32_t)face + 8)); fflush(stdout);
        if (okc && pl) { t = tp_qpc(); okc = tv_g(0x001BC370u, pl, 1, 0, 0, NULL);
            printf("[tailor] v1111V func_001BC370(player %08X, 1) %s in %.2f ms -- the player should look the same (round trip)\n", pl, okc ? "ok" : "FAILED", (tp_qpc() - t) * 1000.0); fflush(stdout); }
        tv_flags("done"); printf("[tailor] v1111V total %.0f ms\n", tv_ms()); fflush(stdout);
        /* v1111Y: tear down like the menu close func_00393EE4: shutdown thunk 10EAC (state 5, releases E+0x34), then keep
         * updating until the release finishes (state 6 / 0), THEN delete. v1111X deleted at once -> main thread hung. */
        tv_shutdown(okc ? "round trip done" : "bake failed"); return; }
    case 5: {
        if (tp_rd32(TV_EGLOB) != s_tv_E) { printf("[tailor] v1111Y editor global changed during shutdown (%08X) -- someone else deleted it\n", tp_rd32(TV_EGLOB)); fflush(stdout); s_tv_E = 0; s_tv_st = 0; return; }
        const uint32_t es = tp_rd32(s_tv_E + 0x38);
        if (es == 5) { if (!tv_g(0x00011C3Cu, s_tv_E, 0, 0, 0, NULL)) { printf("[tailor] v1111Y update during shutdown faulted -- editor left alive\n"); fflush(stdout); s_tv_E = 0; s_tv_st = 0; return; } }
        s_tv_frames++;
        const uint32_t es2 = tp_rd32(s_tv_E + 0x38);
        if (es2 != s_tv_lastst) { char w[48]; snprintf(w, sizeof w, "shutdown state -> %u (frame %d)", es2, s_tv_frames); tv_flags(w); s_tv_lastst = es2; }
        if (es2 != 5 || (s_tv_frames >= 30 && !getenv("GF_TAILOR_WAIT6"))) {
            if (es2 == 6 || es2 == 0) tv_destroy("released");
            else if (tp_qpc() - s_tv_ts > 10.0) { printf("[tailor] v1111Y editor %08X stuck in state %u -- left alive (leak, no delete), global cleared\n", s_tv_E, es2); fflush(stdout); if (tp_rd32(TV_EGLOB) == s_tv_E) wr32(TV_EGLOB, 0); s_tv_E = 0; s_tv_st = 0; }
        }
        return; }
    default: s_tv_st = 0; return;
    }
}
static void tv_blob_diff(uint32_t bp, uint32_t bs)   /* called on a face blob checksum change */
{
    static uint8_t prev[4096]; static uint32_t psz;
    uint8_t cur[4096]; if (!tp_ok(bp, bs) || !bs || bs > sizeof cur) return;
    __try { memcpy(cur, vm_base + bp, bs); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (psz == bs) {
        int n = 0; printf("[tailor] v1111V blob diff (%u bytes):", bs);
        for (uint32_t i = 0; i < bs; i++) if (cur[i] != prev[i]) { if (n < 64) printf(" +%03X %02X->%02X", i, prev[i], cur[i]); n++; }
        printf(n > 64 ? " ... (%d bytes changed)\n" : " (%d bytes changed)\n", n); fflush(stdout);
    } else if (psz) { printf("[tailor] v1111V blob size %u -> %u (no diff)\n", psz, bs); fflush(stdout); }
    memcpy(prev, cur, bs); psz = bs;
}

/* ================================================================== v1112A IN-WORLD TAILOR (Gabriel's Tailoring)
 * User: replace the game's apparel menu with a GTA-style one -- change clothes in the shop, see them live on the player.
 * Inside the tailor (shop028_barber_tattoo_369 box) B opens the store side sheet as "Tailor": categories -> items (the game's
 * UIC tables, tools/gf_uic_apparel.py; price / respect / unlock / partIndex). The camera swings in front of the player.
 * While the sheet is open ONE MobFace editor is alive (created like the F12 test: 1.7 s); the highlighted item is put on
 * after 0.35 s of no movement: poke the slot (+0x44 hat / +0x48 glasses / +0x4C shoes / +0x50 upper / +0x54 lower) into the
 * face blob, func_005E9340(P) parse, 3 updates, detach func_001B4340, bake 141AC/132AC/141AC/12B7C, func_001BC370(pl, 1).
 * Enter: owned -> wear it; else pay the price (money_set) + func_0038B79C(*(0x10181064), name, 3) = owned (the game's own
 * SetClothingItemStatus core; respect not added yet). Closing: whatever was only tried on reverts to what is worn; editor
 * shutdown 10EAC -> update until 6 -> delete. Locked (needUnlock) items show only when the clothing map knows them.
 * GF_TAILOR=0 off. GF_TAILOR_CAM_DIST (2.4) / _UP (1.45) / _SHIFT (0.55, + = the player sits right of centre). Log [tailor] v1112A. */
typedef struct { const char* id; uint8_t cat, slot, idx; int price, respect; uint8_t need_unlock, state; const char* name; } tl_item_t;
static const tl_item_t k_tl[] = {
    { "apparel_hats_06", 0, 0, 0, 0, 0, 0, 3, "No Hat" },
    { "apparel_hats_01", 0, 0, 1, 2000, 150, 0, 2, "Snap-Brim Hat" },
    { "apparel_hats_04", 0, 0, 4, 2500, 190, 0, 2, "Homburg" },
    { "apparel_hats_02", 0, 0, 2, 3500, 265, 0, 2, "Narrow-Brim Fedora" },
    { "apparel_hats_07", 0, 0, 6, 3700, 265, 1, 2, "Classic Fedora" },
    { "apparel_hats_08", 0, 0, 5, 3700, 290, 1, 2, "Porkpie Hat" },
    { "apparel_hats_03", 0, 0, 3, 3750, 285, 0, 2, "Wide-Brim Fedora" },
    { "apparel_hats_09", 0, 0, 7, 3800, 290, 1, 2, "Curled-Brim Hat" },
    { "apparel_hats_10", 0, 0, 8, 3900, 290, 1, 2, "Zoot Hat" },
    { "apparel_glasses_04", 1, 1, 0, 0, 0, 0, 3, "No Glasses" },
    { "apparel_glasses_05", 1, 1, 4, 0, 200, 1, 2, "Aviators" },
    { "apparel_glasses_06", 1, 1, 5, 0, 210, 1, 2, "Classic Frames" },
    { "apparel_glasses_01", 1, 1, 2, 1250, 95, 0, 2, "Wire Frames" },
    { "apparel_glasses_02", 1, 1, 1, 1250, 95, 0, 2, "Horn Rims" },
    { "apparel_glasses_03", 1, 1, 3, 2500, 190, 0, 2, "Sunglasses" },
    { "apparel_upper_shirt_01", 2, 3, 7, 0, 0, 0, 3, "Dress Shirt No. 1" },
    { "apparel_upper_shirt_02", 2, 3, 0, 0, 0, 0, 3, "Dress Shirt No. 2" },
    { "apparel_upper_shirt_03", 2, 3, 4, 0, 0, 0, 3, "Dress Shirt No. 3" },
    { "apparel_upper_shirt_11", 2, 3, 2, 1200, 375, 0, 2, "Dress Shirt No. 7" },
    { "apparel_upper_shirt_13", 2, 3, 9, 1500, 470, 0, 2, "Dress Shirt with Tie No. 3" },
    { "apparel_upper_shirt_12", 2, 3, 1, 1800, 565, 0, 2, "Dress Shirt No. 8" },
    { "apparel_upper_shirt_05", 2, 3, 6, 2100, 660, 0, 2, "Dress Shirt No. 5" },
    { "apparel_upper_shirt_07", 2, 3, 11, 2100, 660, 0, 2, "Dress Shirt with Tie No. 1" },
    { "apparel_upper_shirt_04", 2, 3, 3, 2400, 750, 0, 2, "Dress Shirt No. 4" },
    { "apparel_upper_shirt_06", 2, 3, 5, 2400, 750, 0, 2, "Dress Shirt No. 6" },
    { "apparel_upper_shirt_08", 2, 3, 10, 3000, 940, 0, 2, "Dress Shirt with Tie No. 2" },
    { "apparel_upper_shirt_14", 2, 3, 8, 3000, 940, 0, 2, "Dress Shirt with Tie No. 4" },
    { "apparel_upper_shirt_15", 2, 3, 42, 3000, 940, 1, 2, "Undershirt" },
    { "apparel_upper_shirt_17", 3, 3, 13, 3600, 1125, 0, 2, "Shirt & Vest No. 1" },
    { "apparel_upper_shirt_18", 3, 3, 12, 3600, 1125, 0, 2, "Shirt & Vest No. 2" },
    { "apparel_upper_vest_19", 3, 3, 15, 3600, 1125, 0, 2, "Shirt & Vest No. 3" },
    { "apparel_upper_vest_20", 3, 3, 14, 3600, 1125, 0, 2, "Shirt & Vest No. 4" },
    { "apparel_upper_vest_21", 3, 3, 17, 4200, 1315, 0, 2, "Shirt & Vest with Tie No. 1" },
    { "apparel_upper_vest_22", 3, 3, 16, 4200, 1315, 0, 2, "Shirt & Vest with Tie No. 2" },
    { "apparel_upper_vest_23", 3, 3, 19, 4200, 1315, 0, 2, "Shirt & Vest with Tie No. 3" },
    { "apparel_upper_vest_24", 3, 3, 18, 4200, 1315, 0, 2, "Shirt & Vest with Tie No. 4" },
    { "apparel_upper_75", 3, 3, 34, 9000, 2815, 0, 2, "Shirt & Vest with Tie No. 5" },
    { "apparel_upper_sport_38", 4, 3, 26, 6000, 1875, 0, 2, "Sport Coat No. 1" },
    { "apparel_upper_sport_39", 4, 3, 27, 6000, 1875, 0, 2, "Sport Coat No. 2" },
    { "apparel_upper_sport_40", 4, 3, 28, 7200, 2250, 0, 2, "Sport Coat with Tie No. 1" },
    { "apparel_upper_sport_41", 4, 3, 29, 7200, 2250, 0, 2, "Sport Coat with Tie No. 2" },
    { "apparel_upper_sport_44", 4, 3, 30, 9600, 3000, 0, 2, "Sport Coat with Vest" },
    { "apparel_upper_sport_45", 4, 3, 31, 10200, 3190, 0, 2, "Sport Coat with Vest & Tie" },
    { "apparel_upper_leisure_29", 5, 3, 20, 22500, 1690, 0, 2, "Leisure Coat No. 1" },
    { "apparel_upper_leisure_30", 5, 3, 21, 22500, 1690, 0, 2, "Leisure Coat No. 2" },
    { "apparel_upper_leisure_31", 5, 3, 22, 27500, 2065, 0, 2, "Leisure Coat with Tie No. 1" },
    { "apparel_upper_leisure_32", 5, 3, 23, 27500, 2065, 0, 2, "Leisure Coat with Tie No. 2" },
    { "apparel_upper_leisure_35", 5, 3, 24, 35000, 2625, 0, 2, "Leisure Coat with Vest" },
    { "apparel_upper_leisure_36", 5, 3, 25, 37500, 2815, 0, 2, "Leisure Coat with Vest & Tie" },
    { "apparel_upper_leisure_38", 5, 3, 36, 37500, 3000, 1, 2, "Leather Coat with Tie" },
    { "apparel_upper_leisure_39", 5, 3, 38, 37500, 2815, 1, 2, "Bomber Jacket" },
    { "apparel_upper_leisure_40", 5, 3, 39, 37500, 2815, 1, 2, "Zoot Suit" },
    { "apparel_upper_leisure_41", 5, 3, 40, 37500, 2815, 1, 2, "Zoot Suit with Tie" },
    { "apparel_upper_leisure_42", 5, 3, 41, 37500, 2815, 1, 2, "Zoot Suit with Vest & Tie" },
    { "apparel_upper_leisure_43", 5, 3, 43, 37500, 2815, 1, 2, "Long Coat" },
    { "apparel_upper_leisure_44", 5, 3, 44, 37500, 2815, 1, 2, "Long Coat with Tie" },
    { "apparel_upper_leisure_45", 5, 3, 45, 37500, 2815, 1, 2, "Long Coat with Vest & Tie" },
    { "apparel_upper_db_47", 6, 3, 32, 11400, 3565, 0, 2, "Double-Breasted Suit" },
    { "apparel_upper_db_48", 6, 3, 33, 12000, 3750, 0, 2, "Double-Breasted Suit with Tie" },
    { "apparel_upper_db_49", 6, 3, 35, 12000, 3750, 1, 2, "Tuxedo" },
    { "apparel_pants_01", 7, 4, 0, 0, 0, 0, 3, "Plain Slacks" },
    { "apparel_pants_02", 7, 4, 1, 720, 225, 0, 2, "Belted Slacks" },
    { "apparel_pants_03", 7, 4, 2, 1080, 340, 0, 2, "Dress Slacks" },
    { "apparel_pants_05", 7, 4, 3, 1200, 375, 0, 2, "Belted Dress Slacks" },
    { "apparel_pants_06", 7, 4, 4, 1400, 375, 1, 2, "Tuxedo Trousers" },
    { "apparel_pants_07", 7, 4, 5, 1400, 375, 1, 2, "Zoot Trousers" },
    { "apparel_pants_15", 7, 4, 6, 1400, 375, 1, 2, "Tapered Trousers" },
    { "apparel_shoes_01", 8, 2, 0, 0, 0, 0, 3, "Dress Shoes" },
    { "apparel_shoes_02", 8, 2, 1, 420, 135, 0, 2, "Oxfords" },
    { "apparel_shoes_04", 8, 2, 2, 540, 170, 0, 2, "Loafers" },
    { "apparel_shoes_03", 8, 2, 3, 720, 225, 0, 2, "Wingtips" },
    { "apparel_shoes_05", 8, 2, 4, 720, 225, 1, 2, "Spectators" },
};
#define TL_N ((int)(sizeof k_tl / sizeof k_tl[0]))
#define TL_NCAT 9
static const char* const k_tl_cat[TL_NCAT] = { "Hats", "Eyewear", "Dress Shirts", "Vests", "Sport Coats", "Overcoats & Jackets", "Double-Breasted", "Trousers", "Shoes" };
enum { TL_IDLE, TL_START, TL_READY, TL_PARSED, TL_CLOSING, TL_DETACHED, TL_CDETACH };
static const char* const k_tl_sets[] = {   /* v1116B: MobFace material sets per k_tl row (UIC set0..set3) */
    "",   /* apparel_hats_06 */
    "SnapBrim",   /* apparel_hats_01 */
    "homburg",   /* apparel_hats_04 */
    "FedoraNarr",   /* apparel_hats_02 */
    "FedoraClassic",   /* apparel_hats_07 */
    "Porkpie",   /* apparel_hats_08 */
    "FedoraWide",   /* apparel_hats_03 */
    "CurledBrim",   /* apparel_hats_09 */
    "ZootHat",   /* apparel_hats_10 */
    "",   /* apparel_glasses_04 */
    "aviators",   /* apparel_glasses_05 */
    "classics",   /* apparel_glasses_06 */
    "WireFrames",   /* apparel_glasses_01 */
    "HornRims",   /* apparel_glasses_02 */
    "sunglasses",   /* apparel_glasses_03 */
    "DressShirts",   /* apparel_upper_shirt_01 */
    "DressShirts",   /* apparel_upper_shirt_02 */
    "DressShirts",   /* apparel_upper_shirt_03 */
    "DressShirts",   /* apparel_upper_shirt_11 */
    "DressShirts,Ties",   /* apparel_upper_shirt_13 */
    "DressShirts",   /* apparel_upper_shirt_12 */
    "DressShirts",   /* apparel_upper_shirt_05 */
    "DressShirts,Ties",   /* apparel_upper_shirt_07 */
    "DressShirts",   /* apparel_upper_shirt_04 */
    "DressShirts",   /* apparel_upper_shirt_06 */
    "DressShirts,Ties",   /* apparel_upper_shirt_08 */
    "DressShirts,Ties",   /* apparel_upper_shirt_14 */
    "undershirt",   /* apparel_upper_shirt_15 */
    "DressShirts,Vest",   /* apparel_upper_shirt_17 */
    "DressShirts,Vest",   /* apparel_upper_shirt_18 */
    "DressShirts,Vest",   /* apparel_upper_vest_19 */
    "DressShirts,Vest",   /* apparel_upper_vest_20 */
    "DressShirts,Vest,Ties",   /* apparel_upper_vest_21 */
    "DressShirts,Vest,Ties",   /* apparel_upper_vest_22 */
    "DressShirts,Vest,Ties",   /* apparel_upper_vest_23 */
    "DressShirts,Vest,Ties",   /* apparel_upper_vest_24 */
    "DressShirts,Vest,Ties",   /* apparel_upper_75 */
    "DressShirts,suitcoat",   /* apparel_upper_sport_38 */
    "DressShirts,suitcoat",   /* apparel_upper_sport_39 */
    "DressShirts,suitcoat,Ties",   /* apparel_upper_sport_40 */
    "DressShirts,suitcoat,Ties",   /* apparel_upper_sport_41 */
    "DressShirts,suitcoat,Vest",   /* apparel_upper_sport_44 */
    "DressShirts,suitcoat,Ties,Vest",   /* apparel_upper_sport_45 */
    "DressShirts,leisurlof",   /* apparel_upper_leisure_29 */
    "DressShirts,leisurlof",   /* apparel_upper_leisure_30 */
    "DressShirts,leisurlof,Ties",   /* apparel_upper_leisure_31 */
    "DressShirts,leisurlof,Ties",   /* apparel_upper_leisure_32 */
    "DressShirts,leisurlof,Vest",   /* apparel_upper_leisure_35 */
    "DressShirts,leisurlof,Ties,Vest",   /* apparel_upper_leisure_36 */
    "DressShirts,leatherc,Ties",   /* apparel_upper_leisure_38 */
    "DressShirts,bombercoat",   /* apparel_upper_leisure_39 */
    "zootcoat,DressShirts",   /* apparel_upper_leisure_40 */
    "zootcoat,DressShirts,Ties",   /* apparel_upper_leisure_41 */
    "zootcoat,DressShirts,Ties,Vest",   /* apparel_upper_leisure_42 */
    "longcoat,DressShirts",   /* apparel_upper_leisure_43 */
    "longcoat,DressShirts,Ties",   /* apparel_upper_leisure_44 */
    "longcoat,DressShirts,Ties,Vest",   /* apparel_upper_leisure_45 */
    "DressShirts,DoubBreast",   /* apparel_upper_db_47 */
    "DressShirts,DoubBreast,Ties",   /* apparel_upper_db_48 */
    "DressShirts,Tuxedo",   /* apparel_upper_db_49 */
    "SlacksCNoBelt",   /* apparel_pants_01 */
    "SlacksCBelt",   /* apparel_pants_02 */
    "SlacksDNoBelt",   /* apparel_pants_03 */
    "SlacksDBelt",   /* apparel_pants_05 */
    "TuxedoBottom",   /* apparel_pants_06 */
    "ZootPants",   /* apparel_pants_07 */
    "TaperedPants",   /* apparel_pants_15 */
    "DressShoe",   /* apparel_shoes_01 */
    "oxfordshoe",   /* apparel_shoes_02 */
    "loafershoe",   /* apparel_shoes_04 */
    "wingtipshoe",   /* apparel_shoes_03 */
    "SpectatorShoes",   /* apparel_shoes_05 */
};
typedef char tl_sets_match[(sizeof k_tl_sets / sizeof k_tl_sets[0]) == TL_N ? 1 : -1];
/* v1116B COLOURS & PATTERNS (user: "add the color & pattern options to the clothing in the tailor menu"). The game's apparel screen had
 * an "advanced options" page: per material set of the outfit (DressShirts, Ties, Vest, suitcoat, FedoraWide ...) a PATTERN slider
 * (GetSliderInfo / SetSliderValue type 2 = material selection) and a TINT slider (type 3 = colour within that material). Natives
 * func_00393134 / func_00393594 switch on the type and call the editor E = *(0x10180F88):
 *   type 2: count func_0001211C(E, set), current func_000128BC(E, set), set func_00010AFC(E, set, i)
 *   type 3: count func_0001293C(E, set, material), current func_0001278C(E, set), set func_000137BC(E, set, i)
 * then ApplyCompositeTextures (func_00393A58) = func_000102DC(E). Enter on a worn + owned item opens its page (rows: back, then
 * "<part> pattern" / "<part> colour" per set; Left / Right or Enter change it). Each change: detach, 3 frames, set + composite +
 * 3 updates, live model back on (like a try-on). Choices are kept per set for the visit and re-applied after every re-parse (try-on
 * of another item); a change marks the outfit dirty so closing bakes it. Log [tailor] v1116B. */
#define TC_MAXS 4
static volatile int s_tc_item = -1, s_tc_ns, s_tc_valid, s_tc_busy;
static char s_tc_set[TC_MAXS][24];
static volatile int s_tc_npat[TC_MAXS], s_tc_pat[TC_MAXS], s_tc_ntint[TC_MAXS], s_tc_tint[TC_MAXS];
static struct { char set[24]; int pat, tint; } s_tc_keep[16]; static int s_tc_nkeep, s_tc_dirty;
static int s_tc_pend[8]; static int s_tc_npend;
static const char* tc_label(const char* set)
{
    static const struct { const char* k; const char* v; } m[] = { { "DressShirts", "Shirt" }, { "Ties", "Tie" }, { "Vest", "Vest" }, { "undershirt", "Undershirt" },
        { "suitcoat", "Coat" }, { "leisurlof", "Coat" }, { "leatherc", "Coat" }, { "zootcoat", "Coat" }, { "longcoat", "Coat" }, { "bombercoat", "Jacket" },
        { "DoubBreast", "Jacket" }, { "Tuxedo", "Jacket" } };
    for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++) if (!strcmp(m[i].k, set)) return m[i].v;
    return 0;
}
static void tc_open(int i);   /* render thread, below */
static int s_tl_on = -1, s_tl_st, s_tl_frames; static uint32_t s_tl_E, s_tl_gbuf; static double s_tl_ts;
static volatile int s_tl_cat = -1, s_tl_hl = -1, s_tl_reset_sel = -1, s_tl_ready;
static volatile uint8_t s_tl_owned[TL_N], s_tl_avail[TL_N];
static volatile int s_tl_equip[TV_SLOT_N], s_tl_cur[TV_SLOT_N];   /* bought / worn  vs  currently baked */
static int s_tl_want[TV_SLOT_N]; static double s_tl_hl_t; static int s_tl_hl_prev = -2;
static float s_tl_d[3]; static int s_tl_cam;
static int tl_row_item(int cat, int row) { int n = 0; for (int i = 0; i < TL_N; i++) if (k_tl[i].cat == cat && s_tl_avail[i]) { if (n == row) return i; n++; } return -1; }
static int tl_cat_count(int cat) { int n = 0; for (int i = 0; i < TL_N; i++) if (k_tl[i].cat == cat && s_tl_avail[i]) n++; return n; }
static int tl_gstr(const char* s)   /* main thread: the name into the guest scratch buffer */
{
    if (!s_tl_gbuf) { uint64_t m = 0; if (!tv_g(0x000283D4u, 128, 0, 0, 0, &m) || !tp_ok((uint32_t)m, 128)) return 0; s_tl_gbuf = (uint32_t)m; }
    size_t n = strlen(s); if (n > 120) n = 120; memcpy(vm_base + s_tl_gbuf, s, n); vm_base[s_tl_gbuf + n] = 0; return 1;
}
static void tl_ownership(void)   /* main thread, at open */
{
    tp_kv kv[1024]; const int nk = tp_map(kv, 1024); int no = 0, na = 0;
    for (int i = 0; i < TL_N; i++) {
        uint64_t h = 0; int st = -1;
        if (nk >= 0 && tl_gstr(k_tl[i].id) && tv_g(0x003FE410u, s_tl_gbuf, 0, 0, 0, &h)) for (int k = 0; k < nk; k++) if (kv[k].h == (uint32_t)h) { st = (int)kv[k].v; break; }
        s_tl_owned[i] = (uint8_t)(st == 3 || k_tl[i].state == 3);
        s_tl_avail[i] = (uint8_t)(!k_tl[i].need_unlock || st >= 0);
        no += s_tl_owned[i]; na += s_tl_avail[i];
    }
    printf("[tailor] v1112A ownership: clothing map %d entries, %d items owned, %d of %d available\n", nk, no, na, TL_N); fflush(stdout);
}
static int tl_read_slots(int* v) { uint32_t bs = 0; const uint32_t bp = tv_blob(&bs); if (!bp) return 0; for (int s = 0; s < TV_SLOT_N; s++) v[s] = (int)tp_rd32(bp + k_tv_slot_off[s]); return 1; }
static const char* tl_slot_item(int slot, int idx) { for (int i = 0; i < TL_N; i++) if (k_tl[i].slot == slot && k_tl[i].idx == idx) return k_tl[i].name; return "?"; }
static void tl_cam(uint32_t pl, int on)
{
    if (!on) { if (s_tl_cam) { g_gfd_cam_on = 0; s_tl_cam = 0; SLOG("[tailor] v1112A camera off\n"); } return; }
    float P[3]; if (!player_pos(pl, P)) return;
    static float dist = -1, up, shift; if (dist < 0) { const char* e;
        dist = (e = getenv("GF_TAILOR_CAM_DIST")) && atof(e) > 0.8 ? (float)atof(e) : 2.9f; up = (e = getenv("GF_TAILOR_CAM_UP")) && *e ? (float)atof(e) : 1.40f;   /* v1112F 2.4 -> 2.9 (user: too close) */
        shift = (e = getenv("GF_TAILOR_CAM_SHIFT")) && *e ? (float)atof(e) : 0.55f; }
    if (!s_tl_cam) {   /* v1112D: frozen at open. Facing = player matrix forward row +0x30 (logged); 16 directions scored by clear space at chest
                        * height (ray, capped at dist + 0.3) + preference for the front; distance = clearance - 0.35 (>= 1.1 m). */
        float fx = 0.0f, fz = 1.0f; { const uint32_t m = tp_rd32(pl + 0x68u); if (tp_ok(m, 0x30)) { __try { fx = rdf(m + 0x30); fz = rdf(m + 0x38);   /* v1112E: rows +10 right / +20 up / +30 forward / +40 pos */ } __except (EXCEPTION_EXECUTE_HANDLER) { } } }
        { const float L = sqrtf(fx * fx + fz * fz); if (L > 0.1f) { fx /= L; fz /= L; } else { fx = 0.0f; fz = 1.0f; } }
        static int flip = -1; if (flip < 0) { const char* e = getenv("GF_TAILOR_CAM_FLIP"); flip = e && *e == '1'; } if (flip) { fx = -fx; fz = -fz; }
        float best = -1e9f, bx = fx, bz = fz, bd = dist;
        for (int k = 0; k < 16; k++) {
            const float a = (float)k * 0.39269908f, cx = fx * cosf(a) - fz * sinf(a), cz = fx * sinf(a) + fz * cosf(a);
            const float o[3] = { P[0], P[1] + 1.3f, P[2] }, dv[3] = { cx, 0.0f, cz }; float t = dist + 0.3f, n[3];
            if (!gf_rag_ray(o, dv, dist + 0.3f, &t, n)) t = dist + 0.3f;
            const float sc = (t < dist + 0.3f ? t : dist + 0.3f) + 0.9f * (cx * fx + cz * fz);
            if (sc > best) { best = sc; bx = cx; bz = cz; bd = t - 0.35f; }
        }
        if (bd > dist) bd = dist; if (bd < 1.1f) bd = 1.1f;
        s_tl_d[0] = bx; s_tl_d[1] = bd; s_tl_d[2] = bz;
        SLOG("[tailor] v1112D camera on: player faces (%.2f %.2f), camera direction (%.2f %.2f) %.0f deg off his front, distance %.2f m\n",
             (double)fx, (double)fz, (double)bx, (double)bz, (double)(acosf(fmaxf(-1.0f, fminf(1.0f, bx * fx + bz * fz))) * 57.29578f), (double)bd);
    }
    const float dx = s_tl_d[0], dz = s_tl_d[2], d = s_tl_d[1], rx = dz, rz = -dx;
    g_gfd_cam_eye[0] = P[0] + dx * d; g_gfd_cam_eye[1] = P[1] + up; g_gfd_cam_eye[2] = P[2] + dz * d;
    const float sh = shift * d / dist;   /* v1112D: the sideways offset scales with the distance */
    g_gfd_cam_tgt[0] = P[0] - rx * sh; g_gfd_cam_tgt[1] = P[1] + 0.95f; g_gfd_cam_tgt[2] = P[2] - rz * sh;   /* v1112F: sign flipped (v1112E put him under the sheet, left) */
    g_gfd_cam_on = 1; s_tl_cam = 1;
}
/* v1112E (user screenshot 17:29: "place the player in front of the mirror and stand like this"): Gabriel's three-panel dressing
 * mirror on the partition, centre (-54.22, 1.40, -196.95), faces +X. Spot = (-53.15, feet, -196.95), the player faces +X (back to
 * the mirror, looking into the room). Moved like gf_coop_proxy.c place(): vt+0x30 SetMatrix (12 floats, rows right / up / fwd /
 * pos, w kept) + vt+0x24 SetPosition {x, y, z, 2} (the physics body). The orientation is his own rotated about Y onto +X.
 * GF_TAILOR_MIRROR=0 off. */
static uint32_t s_tl_mbuf; static double s_tl_curtain_t, s_tl_ready_t, s_tl_open_t;
static int tl_vcall(uint32_t ent, uint32_t slot, uint32_t a1);
/* v1112I gf_footcam.c (game thread, inside the on-foot camera update): move the player's entity + physics body (vt+0x24
 * SetPosition {x,y,z,2}). Used to take the run "lurch" back out. 1 = done. */
int gf_player_setpos(uint32_t pl, float x, float y, float z)
{
    if (!s_tl_mbuf || !pl || !tp_ok(tp_rd32(pl), 0x40)) return 0;
    uint32_t u; memcpy(&u, &x, 4); wr32(s_tl_mbuf + 0x50, u); memcpy(&u, &y, 4); wr32(s_tl_mbuf + 0x54, u); memcpy(&u, &z, 4); wr32(s_tl_mbuf + 0x58, u); wr32(s_tl_mbuf + 0x5C, 2u);
    return tl_vcall(pl, 0x24u, s_tl_mbuf + 0x50);
}
static int tl_vcall(uint32_t ent, uint32_t slot, uint32_t a1)
{
    const uint32_t vt = tp_rd32(ent), opd = tp_rd32(vt + slot), fn = tp_rd32(opd), toc = tp_rd32(opd + 4);
    if (!fn || !toc) return 0; return tv_call(fn, toc, ent, a1, 0, 0, NULL);
}
static void tl_to_mirror(uint32_t pl)
{
    static int on = -1; if (on < 0) { const char* e = getenv("GF_TAILOR_MIRROR"); on = !(e && *e == '0'); }
    if (!on || !pl) return;
    const uint32_t xf = tp_rd32(pl + 0x68u); if (!tp_ok(xf, 0x50)) return;
    if (!s_tl_mbuf) { uint64_t m = 0; if (!tv_g(0x000283D4u, 0x60, 0, 0, 0, &m) || !tp_ok((uint32_t)m, 0x60)) return; s_tl_mbuf = (uint32_t)m; }
    float M[16]; for (int i = 0; i < 16; i++) M[i] = rdf(xf + 0x10u + (uint32_t)i * 4u);
    const float tx = 1.0f, tz = 0.0f;                         /* wanted forward (+X) */
    float fx = M[8], fz = M[10]; { const float L = sqrtf(fx * fx + fz * fz); if (L < 0.1f) return; fx /= L; fz /= L; }
    const float c = fx * tx + fz * tz, sn = fx * tz - fz * tx;   /* rotation about Y taking (fx, fz) onto (tx, tz) */
    for (int r = 0; r < 3; r++) { const float x = M[r * 4], z = M[r * 4 + 2]; M[r * 4] = x * c - z * sn; M[r * 4 + 2] = x * sn + z * c; }
    const float px = -53.15f, py = M[13], pz = -196.95f;
    M[12] = px; M[14] = pz;
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) { uint32_t u; memcpy(&u, &M[r * 4 + k], 4); wr32(s_tl_mbuf + (uint32_t)(r * 16 + k * 4), u); }
    const int a = tl_vcall(pl, 0x30u, s_tl_mbuf);
    { uint32_t u; memcpy(&u, &px, 4); wr32(s_tl_mbuf + 0x40, u); memcpy(&u, &py, 4); wr32(s_tl_mbuf + 0x44, u); memcpy(&u, &pz, 4); wr32(s_tl_mbuf + 0x48, u); wr32(s_tl_mbuf + 0x4C, 2u); }
    const int b = tl_vcall(pl, 0x24u, s_tl_mbuf + 0x40);
    float P[3] = { 0, 0, 0 }; player_pos(pl, P);
    SLOG("[tailor] v1112E to the mirror: SetMatrix %s, SetPosition %s -> player at (%.2f %.2f %.2f), forward (%.2f %.2f)\n", a ? "ok" : "FAILED", b ? "ok" : "FAILED",
         (double)P[0], (double)P[1], (double)P[2], (double)rdf(xf + 0x30), (double)rdf(xf + 0x38));
    s_tl_cam = 0;   /* re-frame from the new spot */
}
static int tl_start_editor(void)
{
    if (tp_rd32(TV_EGLOB) || tp_rd32(TV_CGLOB) || s_tv_st) { SLOG("[tailor] v1112A editor busy (E %08X, menu %08X, F12 test %d)\n", tp_rd32(TV_EGLOB), tp_rd32(TV_CGLOB), s_tv_st); return 0; }
    uint64_t mem = 0;
    if (!tv_g(0x000283D4u, 0x88, 0, 0, 0, &mem) || !tp_ok((uint32_t)mem, 0x88)) return 0;
    if (!tv_g(0x00013ADCu, mem, tp_rd32(TV_ALLOC), TV_ARG5, 0, NULL)) return 0;
    s_tl_E = tp_rd32(TV_EGLOB); if (s_tl_E != (uint32_t)mem) { SLOG("[tailor] v1112A editor ctor did not publish (%08X)\n", s_tl_E); s_tl_E = 0; return 0; }
    if (tp_rd32(s_tl_E + 0x38) == 0) wr32(s_tl_E + 0x38, 1);
    SLOG("[tailor] v1112A editor %08X created\n", s_tl_E); return 1;
}
static int tl_update(void) { return s_tl_E && tp_rd32(TV_EGLOB) == s_tl_E && tv_g(0x00011C3Cu, s_tl_E, 0, 0, 0, NULL); }
static void tl_restore(uint32_t pl);
static void tl_begin_close(const char* why)
{
    s_tl_ready = 0; tl_restore(player());   /* v1112D */
    if (s_tl_E && tp_rd32(TV_EGLOB) == s_tl_E && tv_g(0x00010EACu, s_tl_E, 0, 0, 0, NULL)) { s_tl_st = TL_CLOSING; s_tl_ts = tp_qpc(); SLOG("[tailor] v1112A closing (%s): editor shutdown\n", why); }
    else { SLOG("[tailor] v1112A closing (%s): no editor to shut down\n", why); s_tl_E = 0; s_tl_st = TL_IDLE; }
}
static int tl_poke_parse(const int* v)   /* write the 5 slots + parse; 1 = ok */
{
    uint32_t bs = 0; const uint32_t bp = tv_blob(&bs); if (!bp) return 0;
    for (int s = 0; s < TV_SLOT_N; s++) wr32(bp + k_tv_slot_off[s], (uint32_t)v[s]);
    uint64_t r = 0; const double t = tp_qpc();
    const int ok2 = tv_g(0x00010C9Cu, tp_rd32(TV_PGLOB), 0, 0, 0, &r);
    { static int nl; if (nl++ < 200) SLOG("[tailor] v1112A dress: hat %d glasses %d shoes %d upper %d lower %d -> parse %s (%u) %.0f ms\n", v[0], v[1], v[2], v[3], v[4], ok2 ? "ok" : "FAILED", (uint32_t)r, (tp_qpc() - t) * 1000.0); }
    return ok2;
}
static int tl_bake(uint32_t pl)
{
    const uint32_t P = tp_rd32(TV_PGLOB); uint64_t face = 0; const double t = tp_qpc();
    if (!pl) return 0;
    int ok2 = tv_g(0x000141ACu, P, 0, 0, 0, NULL) && tv_g(0x000132ACu, s_tl_E, tp_rd32(P + 0x18), 0, 0, &face) && tp_ok((uint32_t)face, 12)
           && tv_g(0x000141ACu, P, face, 0, 0, NULL) && tv_g(0x00012B7Cu, s_tl_E, face, 0, 0, NULL) && tp_ok(tp_rd32((uint32_t)face), 4);
    if (ok2) ok2 = tv_g(0x001BC370u, pl, 1, 0, 0, NULL);
    { static int nl; if (nl++ < 200 || !ok2) SLOG("[tailor] v1112A bake %s in %.0f ms (face %08X model %08X)\n", ok2 ? "ok" : "FAILED", (tp_qpc() - t) * 1000.0, (uint32_t)face, tp_rd32((uint32_t)face)); }
    return ok2;
}
/* v1112D: while browsing the player wears the editor's LIVE model E+0x24 (what the game's preview entity gets, func_00394300 ->
 * vt+0x108); a bake (func_005E40D0) per change consumed the editor (2nd generate wrote through null buffers: AddressToOffset
 * REFUSED ea 0x3CE0.. + hang, v1112B/C). The bake now runs ONCE, when the sheet closes. */
static int s_tl_live, s_tl_orig[TV_SLOT_N];
static int tl_attach_live(uint32_t pl)
{
    const uint32_t m = tp_rd32(s_tl_E + 0x24), vt = tp_rd32(pl), opd = tp_rd32(vt + 0x108);
    if (!tp_ok(m, 4) || !tp_ok(opd, 8)) { SLOG("[tailor] v1112D no live model (E+24 %08X)\n", m); return 0; }
    const int ok2 = tv_call(tp_rd32(opd), tp_rd32(opd + 4), pl, m, 0, 0, NULL) && tv_g(0x00461E30u, pl, 0, 0, 0, NULL) && tv_g(0x00463660u, pl, pl + 0x2234u, 0, 0, NULL);
    { static int nl; if (nl++ < 100 || !ok2) SLOG("[tailor] v1112D player wears the live model %08X: %s\n", m, ok2 ? "ok" : "FAILED"); }
    if (ok2) s_tl_live = 1; return ok2;
}
static void tl_restore(uint32_t pl)   /* put the game's own baked face back (nothing baked this visit) */
{
    if (!s_tl_live || !pl) return;
    tv_g(0x001B4340u, pl, 0, 0, 0, NULL); tv_g(0x001BC370u, pl, 1, 0, 0, NULL); s_tl_live = 0;
    SLOG("[tailor] v1112D original model back on\n");
}
static void tl_buy(int i)   /* main thread, Enter on an item */
{
    if (i < 0 || i >= TL_N) return;
    const tl_item_t* it = &k_tl[i];
    if (s_tl_owned[i]) { s_tl_equip[it->slot] = it->idx; toast("Wearing the %s", ui_tr(it->name)); return; }
    if (!s_tl_avail[i]) { toast("Gabriel doesn't have that in your size yet"); return; }
    const int m = money_get(); if (m < 0) { toast("Not now"); return; }
    if (m < it->price) { toast("Not enough cash - the %s costs $%d", ui_tr(it->name), it->price); return; }
    const uint32_t mgr = tp_rd32(0x10181064u);
    if (!tp_ok(mgr, 0x40) || !tl_gstr(it->id)) { toast("The tailor can't sell that right now"); return; }
    if (it->price && !money_set(m - it->price)) { toast("The till is stuck - try again"); return; }
    if (!tv_g(0x0038B79Cu, mgr, s_tl_gbuf, 3, 0, NULL)) { if (it->price) money_set(m); toast("The tailor can't sell that right now"); return; }
    s_tl_owned[i] = 1; s_tl_equip[it->slot] = it->idx;
    SLOG("[tailor] v1112A bought %s (%s) for $%d -> clothing status 3\n", it->id, it->name, it->price);
    toast("Bought the %s  -$%d", ui_tr(it->name), it->price);
}
static int tc_call(uint32_t fn, const char* set, uint64_t a2, int* out)   /* main thread: fn(E, set, a2) */
{
    uint64_t r = 0; if (!s_tl_E || tp_rd32(TV_EGLOB) != s_tl_E || !tl_gstr(set) || !tv_g(fn, s_tl_E, s_tl_gbuf, a2, 0, &r)) return 0;
    if (out) *out = (int)(int32_t)(uint32_t)r; return 1;
}
static int tc_get(const char* set, int* npat, int* pat, int* ntint, int* tint)
{
    int a = 0, b = 0, c = 0, d = 0;
    if (!tc_call(0x0001211Cu, set, 0, &a) || !tc_call(0x000128BCu, set, 0, &b)) return 0;
    if (a < 0 || a > 256) a = 0; if (b < 0 || b >= (a > 0 ? a : 1)) b = 0;
    if (!tc_call(0x0001293Cu, set, (uint64_t)b, &c) || !tc_call(0x0001278Cu, set, 0, &d)) return 0;
    if (c < 0 || c > 256) c = 0; if (d < 0 || d >= (c > 0 ? c : 1)) d = 0;
    *npat = a; *pat = b; *ntint = c; *tint = d; return 1;
}
static void tc_keep(const char* set, int pat, int tint)
{
    int k = 0; for (; k < s_tc_nkeep; k++) if (!strcmp(s_tc_keep[k].set, set)) break;
    if (k == s_tc_nkeep) { if (s_tc_nkeep >= 16) return; s_tc_nkeep++; snprintf(s_tc_keep[k].set, sizeof s_tc_keep[k].set, "%s", set); }
    s_tc_keep[k].pat = pat; s_tc_keep[k].tint = tint;
}
static void tc_query(void)   /* counts + current values of the customised item's sets */
{
    const int n = s_tc_ns;
    for (int j = 0; j < n; j++) {
        int a = 0, b = 0, c = 0, d = 0;
        if (!tc_get(s_tc_set[j], &a, &b, &c, &d)) { a = b = c = d = 0; }
        s_tc_npat[j] = a; s_tc_pat[j] = b; s_tc_ntint[j] = c; s_tc_tint[j] = d;
        { static int nl; if (nl++ < 120) SLOG("[tailor] v1116B %s: pattern %d / %d, colour %d / %d\n", s_tc_set[j], b + 1, a, d + 1, c); }
    }
    s_tc_valid = 1;
}
static int tc_step(int v)   /* (set << 2) | (kind << 1) | up; 1 = something changed */
{
    const int j = (v >> 2) & 3, kind = (v >> 1) & 1, dir = (v & 1) ? 1 : -1;
    if (j >= s_tc_ns) return 0;
    const char* set = s_tc_set[j];
    int a, b, c, d; if (!tc_get(set, &a, &b, &c, &d)) { toast("The tailor can't find that fabric"); return 0; }
    if (kind == 0) {
        if (a <= 1) { toast("No other patterns for that one"); return 0; }
        const int nb = (b + dir + a) % a;
        if (!tc_call(0x00010AFCu, set, (uint64_t)nb, NULL)) return 0;
        int c2 = 0; if (tc_call(0x0001293Cu, set, (uint64_t)nb, &c2) && c2 > 0 && d >= c2) tc_call(0x000137BCu, set, 0, NULL);   /* keep the colour in range */
    } else {
        if (c <= 1) { toast("That pattern comes in one colour"); return 0; }
        const int nd = (d + dir + c) % c;
        if (!tc_call(0x000137BCu, set, (uint64_t)nd, NULL)) return 0;
    }
    if (tc_get(set, &a, &b, &c, &d)) tc_keep(set, b, d);
    s_tc_dirty = 1;
    SLOG("[tailor] v1116B %s %s %s -> pattern %d / %d, colour %d / %d\n", set, kind ? "colour" : "pattern", dir > 0 ? "next" : "previous", b + 1, a, d + 1, c);
    return 1;
}
static int tc_reapply(void)   /* after a parse: the visit's choices back onto every set the new outfit has */
{
    int any = 0;
    for (int k = 0; k < s_tc_nkeep; k++) {
        int a, b, c, d; if (!tc_get(s_tc_keep[k].set, &a, &b, &c, &d) || a <= 0) continue;
        if (s_tc_keep[k].pat < a && s_tc_keep[k].pat != b) { tc_call(0x00010AFCu, s_tc_keep[k].set, (uint64_t)s_tc_keep[k].pat, NULL); any = 1; }
        int c2 = c; tc_call(0x0001293Cu, s_tc_keep[k].set, (uint64_t)s_tc_keep[k].pat, &c2);
        if (s_tc_keep[k].tint < c2) { tc_call(0x000137BCu, s_tc_keep[k].set, (uint64_t)s_tc_keep[k].tint, NULL); any = 1; }
    }
    if (any) { tv_g(0x000102DCu, s_tl_E, 0, 0, 0, NULL); SLOG("[tailor] v1116B %d kept colour choice(s) re-applied after the parse\n", s_tc_nkeep); }
    return any;
}
static void gf_tailor_tick_v1112(uint32_t pl)   /* main thread, every frame */
{
    if (s_tl_on < 0) { const char* e = getenv("GF_TAILOR"); s_tl_on = !(e && *e == '0'); SLOG("[tailor] v1112A in-world tailor (B inside Gabriel's Tailoring): %s\n", s_tl_on ? "ON" : "off (GF_TAILOR=0)");  for (int i = 0; i < TL_N; i++) { s_tl_avail[i] = (uint8_t)!k_tl[i].need_unlock; s_tl_owned[i] = (uint8_t)(k_tl[i].state == 3); } }
    if (!s_tl_on) return;
    {   /* v1112B: Gabriel's own SHOP choice opens the game's apparel screen (controller *(0x10180F8C) + its editor). Press its
         * "Done" (START) as soon as it exists, then open this tailor when the old screen and its editor are gone. */
        static int hj = -1, st; static double t0, tp; extern volatile unsigned g_pad_inject_d1; extern volatile int g_pad_inject_polls;
        extern volatile int g_uiov_curtain_on; extern char g_uiov_curtain_text[64];   /* v1112I: the old screen happens behind a curtain */
        if (hj < 0) { const char* e = getenv("GF_TAILOR_HIJACK"); hj = !(e && *e == '0'); }
        const uint32_t ctl = tp_rd32(TV_CGLOB);
        if (hj && st == 0 && ctl && !s_tl_E && s_here == ST_TAILOR) { st = 1; t0 = tp = now_s(); SLOG("[tailor] v1112B the game's apparel screen opened (controller %08X) -> closing it for the in-world tailor\n", ctl);
            snprintf(g_uiov_curtain_text, 64, "Gabriel's Tailoring"); g_uiov_curtain_on = 1; s_tl_curtain_t = now_s(); }
        if (st == 1) {
            const uint32_t E = tp_rd32(TV_EGLOB), es = E ? tp_rd32(E + 0x38) : 0;
            if (ctl && E && es == 4 && now_s() - tp > 0.8) { tp = now_s(); g_pad_inject_d1 = 0x08; g_pad_inject_polls = 3; SLOG("[tailor] v1112B pressing Done on the old screen (%.1f s)\n", now_s() - t0); }
            if (!ctl && !E) { st = 2; tp = now_s(); SLOG("[tailor] v1112B old screen closed after %.1f s\n", now_s() - t0); }
            else if (now_s() - t0 > 15.0) { st = 0; g_uiov_curtain_on = 0; SLOG("[tailor] v1112B the old screen did not close -- left alone\n"); }
        }
        if (st == 2 && now_s() - tp > 0.1) {   /* v1112J 0.4 -> 0.1 s */ st = 0; if (s_here == ST_TAILOR) { s_req_page = 2; SLOG("[tailor] v1112B opening the in-world tailor\n"); } }
    }
    {   /* v1112I: lower the curtain once he stands at the mirror in the live model (or after 10 s, or the sheet closed) */
        extern volatile int g_uiov_curtain_on;
        if (g_uiov_curtain_on && s_tl_curtain_t > 0.0 && ((s_tl_st != TL_IDLE && now_s() - s_tl_open_t > 0.12) || now_s() - s_tl_curtain_t > 8.0)) {   /* v1112J: down as soon as he is at the mirror (editor loads behind the sheet) */
            g_uiov_curtain_on = 0; SLOG("[tailor] v1112I curtain down after %.1f s\n", now_s() - s_tl_curtain_t); s_tl_curtain_t = 0.0; }
    }
    if (!s_tl_mbuf && pl) { uint64_t m = 0; if (tv_g(0x000283D4u, 0x60, 0, 0, 0, &m) && tp_ok((uint32_t)m, 0x60)) s_tl_mbuf = (uint32_t)m; }   /* v1112I: also for gf_player_setpos */
    { extern volatile int g_uiov_shop_solo; g_uiov_shop_solo = s_here == ST_TAILOR; }   /* v1112J: no Tab / Pockets in the tailor */
    const int open = s_here == ST_TAILOR && now_s() - s_page_t < 0.25 && pl;
    tl_cam(pl, open);
    switch (s_tl_st) {
    case TL_IDLE:
        if (!open) return;
        s_tl_ready = 0; s_tl_cat = -1; s_tl_hl = -1; s_tl_hl_prev = -2; s_tc_item = -1; s_tc_valid = 0; s_tc_nkeep = 0; s_tc_dirty = 0; s_tc_npend = 0;
        if (!tl_read_slots((int*)s_tl_equip)) { toast("The tailor can't fit you right now"); s_req_page = -1; return; }
        memcpy((void*)s_tl_cur, (const void*)s_tl_equip, sizeof s_tl_cur); memcpy(s_tl_orig, (const void*)s_tl_equip, sizeof s_tl_orig); s_tl_live = 0;
        tl_ownership(); tl_to_mirror(pl);   /* v1112E */
        SLOG("[tailor] v1112A open: wearing %s / %s / %s / %s / %s\n", tl_slot_item(0, s_tl_equip[0]), tl_slot_item(1, s_tl_equip[1]), tl_slot_item(2, s_tl_equip[2]), tl_slot_item(3, s_tl_equip[3]), tl_slot_item(4, s_tl_equip[4]));
        if (!tl_start_editor()) { toast("The tailor is busy - try again in a moment"); s_req_page = -1; return; }
        s_tl_st = TL_START; s_tl_ts = tp_qpc(); s_tl_open_t = now_s(); return;
    case TL_START:
        if (!tl_update()) { tl_begin_close("update failed"); return; }
        if (tp_rd32(s_tl_E + 0x38) == 4) { s_tl_st = TL_READY; s_tl_ready = 1; s_tl_ready_t = now_s(); SLOG("[tailor] v1112A editor ready in %.0f ms\n", (tp_qpc() - s_tl_ts) * 1000.0); }
        else if (tp_qpc() - s_tl_ts > 20.0) { toast("The tailor is busy - try again in a moment"); s_req_page = -1; tl_begin_close("start timeout"); }
        if (!open && s_tl_st == TL_START) tl_begin_close("closed while starting");
        return;
    case TL_READY: {
        if (open) {
            for (int s = 0; s < TV_SLOT_N; s++) s_tl_want[s] = s_tl_equip[s];
            const int hl = s_tl_hl; if (hl != s_tl_hl_prev) { s_tl_hl_prev = hl; s_tl_hl_t = now_s(); }
            if (hl >= 0 && hl < TL_N) s_tl_want[k_tl[hl].slot] = k_tl[hl].idx;
            if (hl >= 0 && now_s() - s_tl_hl_t < 0.35) { tl_update(); return; }   /* wait for the highlight to settle */
        } else for (int s = 0; s < TV_SLOT_N; s++) s_tl_want[s] = s_tl_equip[s];   /* closing: back to what is bought / worn */
        const int tc_need_live = open && s_tc_item >= 0 && !s_tl_live;   /* v1116B: the editor must hold the worn outfit (parse) first */
        if (memcmp(s_tl_want, (const void*)s_tl_cur, sizeof s_tl_want) || tc_need_live) {
            /* v1112C: the worn model is baked from THIS editor's generated data -- a new parse rewrites it under the renderer
             * (v1112B: AddressToOffset REFUSED garbage + main-thread hang on the 2nd try-on). Detach first, let 2 frames draw
             * without it, then parse + update + bake + attach in ONE tick. */
            if (!pl || !tv_g(0x001B4340u, pl, 0, 0, 0, NULL)) { tl_begin_close("detach failed"); return; }
            s_tl_st = TL_DETACHED; s_tl_frames = 0; return;
        }
        if (open && s_tc_item >= 0 && s_tl_live) {   /* v1116B */
            const tl_item_t* ci = &k_tl[s_tc_item];
            if (s_tl_cur[ci->slot] != ci->idx) { s_tc_npend = 0; }
            else if (!s_tc_valid) tc_query();
            else if (s_tc_npend) { if (!pl || !tv_g(0x001B4340u, pl, 0, 0, 0, NULL)) { s_tc_npend = 0; toast("The tailor fumbled - try again"); }
                                   else { s_tc_busy = 1; s_tl_st = TL_CDETACH; s_tl_frames = 0; return; } }
        } else s_tc_npend = 0;
        if (!open) {   /* v1112D: final -- bake once if the outfit changed, else the original model back */
            if (s_tl_live) {
                tv_g(0x001B4340u, pl, 0, 0, 0, NULL);
                if (memcmp((const void*)s_tl_equip, s_tl_orig, sizeof s_tl_orig) || s_tc_dirty) {   /* v1116B: + colours / patterns */
                    if (tl_bake(pl)) { s_tl_live = 0; SLOG("[tailor] v1112D new outfit baked onto the player\n"); }
                    else { toast("The tailor fumbled - try again"); }
                } }
            tl_begin_close("sheet closed"); return; }
        tl_update(); return; }
    case TL_DETACHED:
        tl_update();
        if (++s_tl_frames < 3) return;
        if (!tl_poke_parse(s_tl_want)) { toast("The tailor fumbled - try again"); if (pl) tv_g(0x001BC370u, pl, 1, 0, 0, NULL); tl_begin_close("parse failed"); return; }
        tl_update(); tl_update(); tl_update();
        if (tc_reapply()) { tl_update(); tl_update(); }   /* v1116B: the parse reset the colours to the saved ones */
        s_tc_valid = 0;
        if (tl_attach_live(pl)) { memcpy((void*)s_tl_cur, s_tl_want, sizeof s_tl_want); s_tl_st = TL_READY; return; }
        s_tl_live = 1; tl_restore(pl); toast("The tailor fumbled - try again"); memcpy((void*)s_tl_cur, s_tl_want, sizeof s_tl_want); tl_begin_close("live model failed"); return;
    case TL_CDETACH: {   /* v1116B: apply the queued colour / pattern steps on the detached editor model */
        tl_update();
        if (++s_tl_frames < 3) return;
        int changed = 0;
        for (int q = 0; q < s_tc_npend; q++) changed |= tc_step(s_tc_pend[q]);
        s_tc_npend = 0;
        if (changed) { tv_g(0x000102DCu, s_tl_E, 0, 0, 0, NULL); tl_update(); tl_update(); tl_update(); }
        s_tc_busy = 0;
        if (tl_attach_live(pl)) { tc_query(); s_tl_st = TL_READY; return; }
        s_tl_live = 1; tl_restore(pl); toast("The tailor fumbled - try again"); tl_begin_close("live model failed (colours)"); return; }
    case TL_PARSED:
        if (tl_bake(pl)) { memcpy((void*)s_tl_cur, s_tl_want, sizeof s_tl_want); s_tl_st = TL_READY; }
        else { toast("The tailor fumbled - try again"); if (pl) tv_g(0x001BC370u, pl, 1, 0, 0, NULL);   /* v1112C: never leave him detached */
               memcpy((void*)s_tl_cur, s_tl_want, sizeof s_tl_want); tl_begin_close("bake failed"); }
        return;
    case TL_CLOSING: {
        if (!s_tl_E || tp_rd32(TV_EGLOB) != s_tl_E) { s_tl_E = 0; s_tl_st = TL_IDLE; return; }
        const uint32_t es = tp_rd32(s_tl_E + 0x38); if (es == 5) tv_g(0x00011C3Cu, s_tl_E, 0, 0, 0, NULL);
        const uint32_t es2 = tp_rd32(s_tl_E + 0x38);
        if (es2 == 6 || es2 == 0) {
            const uint32_t vt = tp_rd32(s_tl_E);
            if (vt == TV_EVT) { const uint32_t opd = tp_rd32(vt + 4); tv_call(tp_rd32(opd), tp_rd32(opd + 4), s_tl_E, 1, 0, 0, NULL); }
            if (tp_rd32(TV_EGLOB) == s_tl_E) wr32(TV_EGLOB, 0);
            SLOG("[tailor] v1112A editor released + deleted (%.0f ms)\n", (tp_qpc() - s_tl_ts) * 1000.0); s_tl_E = 0; s_tl_st = TL_IDLE;
        } else if (tp_qpc() - s_tl_ts > 10.0) { SLOG("[tailor] v1112A editor stuck in state %u at close -- left alive\n", es2); if (tp_rd32(TV_EGLOB) == s_tl_E) wr32(TV_EGLOB, 0); s_tl_E = 0; s_tl_st = TL_IDLE; }
        return; }
    }
}
/* page api for the tailor (render thread: read + record only) */
static const char* tl_title(void) { return "Gabriel's Tailoring"; }
static int tl_count(void) { if (s_tc_item >= 0) return 1 + 2 * s_tc_ns; if (s_tl_cat < 0) return TL_NCAT; return 1 + tl_cat_count(s_tl_cat); }
static int tc_row(int k, uiov_shop_row* o)   /* v1116B colours & patterns page */
{
    const tl_item_t* it = &k_tl[s_tc_item];
    if (k == 0) { snprintf(o->name, sizeof o->name, "< %s", it->name); snprintf(o->desc, sizeof o->desc, "Back to %s (Esc).", k_tl_cat[it->cat]); o->enabled = 1; return 1; }
    const int j = (k - 1) / 2, kind = (k - 1) & 1; if (j >= s_tc_ns) return 0;
    const char* lb = tc_label(s_tc_set[j]); if (!lb) lb = k_tl_cat[it->cat];
    snprintf(o->name, sizeof o->name, "%s %s", lb, kind ? "colour" : "pattern");
    if (!s_tc_valid || s_tc_busy) { snprintf(o->right, sizeof o->right, "..."); snprintf(o->desc, sizeof o->desc, "Gabriel is fetching the swatches..."); o->enabled = 0; return 1; }
    const int n = kind ? s_tc_ntint[j] : s_tc_npat[j], c = kind ? s_tc_tint[j] : s_tc_pat[j];
    if (n <= 1) { snprintf(o->right, sizeof o->right, n == 1 ? "only one" : "-"); snprintf(o->desc, sizeof o->desc, kind ? "That pattern only comes in one colour." : "No other patterns for this piece."); o->enabled = 0; return 1; }
    snprintf(o->right, sizeof o->right, "<  %d / %d  >", c + 1, n);
    snprintf(o->desc, sizeof o->desc, "Left / Right (or Enter) to change the %s %s. No charge - it's your suit.", lb, kind ? "colour" : "pattern");
    o->enabled = 1; return 1;
}
static void tc_open(int i)
{
    const char* sl = k_tl_sets[i]; int n = 0;
    while (*sl && n < TC_MAXS) { const char* e = strchr(sl, ','); const size_t L = e ? (size_t)(e - sl) : strlen(sl);
        snprintf(s_tc_set[n], sizeof s_tc_set[n], "%.*s", (int)(L < 23 ? L : 23), sl); n++; sl = e ? e + 1 : sl + L; }
    if (!n) return;
    s_tc_ns = n; s_tc_valid = 0; s_tc_item = i; s_tl_reset_sel = 1;
}
static int tl_row(int k, uiov_shop_row* o)
{
    if (s_tc_item >= 0) return tc_row(k, o);
    if (s_tl_cat < 0) {
        if (k < 0 || k >= TL_NCAT) return 0;
        snprintf(o->name, sizeof o->name, "%s", k_tl_cat[k]); snprintf(o->right, sizeof o->right, "%d", tl_cat_count(k));
        snprintf(o->desc, sizeof o->desc, "%s", s_tl_ready ? "Pick a category. Whatever you highlight, you try on." : "Gabriel is taking your measurements...");
        o->enabled = 1; return 1;
    }
    if (k == 0) { snprintf(o->name, sizeof o->name, "< %s", k_tl_cat[s_tl_cat]); snprintf(o->desc, sizeof o->desc, "Back to all categories (Esc)."); o->enabled = 1; return 1; }
    const int i = tl_row_item(s_tl_cat, k - 1); if (i < 0) return 0;
    const tl_item_t* it = &k_tl[i]; const int worn = s_tl_equip[it->slot] == it->idx;
    snprintf(o->name, sizeof o->name, "%s", it->name);
    if (worn) snprintf(o->right, sizeof o->right, "Wearing");
    else if (s_tl_owned[i]) snprintf(o->right, sizeof o->right, "Owned");
    else snprintf(o->right, sizeof o->right, "$%d", it->price);
    /* v1112G: no "+N respect" sub-label (it ran into the price on long names) -- it is in the description now */
    const int m = money_get();
    o->enabled = s_tl_owned[i] || m >= it->price;
    if (worn) snprintf(o->desc, sizeof o->desc, k_tl_sets[i][0] ? "You're wearing it. Enter for colours & patterns." : "You're wearing it.");
    else if (s_tl_owned[i]) snprintf(o->desc, sizeof o->desc, "Yours already. Enter to wear it.");
    else if (m >= it->price) snprintf(o->desc, sizeof o->desc, "Enter to buy it for $%d and wear it out of the shop. +%d respect.", it->price, it->respect);
    else snprintf(o->desc, sizeof o->desc, "You can try it on, but it costs $%d. Worth +%d respect.", it->price, it->respect);
    return 1;
}
static void tl_adjust(int k, int dir);
static void tl_activate(int k)
{
    if (s_tc_item >= 0) {   /* v1116B */
        if (k == 0) { const int i = s_tc_item, cat = k_tl[i].cat; int row = 0; for (int r = 0; r < TL_N; r++) if (tl_row_item(cat, r) == i) { row = r + 1; break; }
                      s_tc_item = -1; s_tl_reset_sel = row; return; }
        tl_adjust(k, +1); return;
    }
    if (s_tl_cat < 0) { if (k >= 0 && k < TL_NCAT) { s_tl_cat = k; s_tl_reset_sel = 1; } return; }
    if (k == 0) { const int c = s_tl_cat; s_tl_cat = -1; s_tl_hl = -1; s_tl_reset_sel = c; return; }
    const int i = tl_row_item(s_tl_cat, k - 1); if (i < 0) return;
    if (s_tc_item < 0 && s_tl_owned[i] && s_tl_equip[k_tl[i].slot] == k_tl[i].idx && k_tl_sets[i][0]) { tc_open(i); return; }   /* v1116B */
    push_rq((3 << 8) | i);
}
static void tl_adjust(int k, int dir)   /* v1116B: Left / Right on a colour / pattern row */
{
    if (s_tc_item < 0 || k <= 0 || !s_tc_valid || s_tc_busy) return;
    const int j = (k - 1) / 2, kind = (k - 1) & 1; if (j >= s_tc_ns) return;
    push_rq((4 << 8) | (j << 2) | (kind << 1) | (dir > 0 ? 1 : 0));
}
static int tl_selected(int k)
{
    const int r = s_tl_reset_sel; if (r >= 0) { s_tl_reset_sel = -1; k = r; }
    if (s_tc_item >= 0) { s_tl_hl = s_tc_item; return r; }   /* v1116B: keep the item on while choosing colours */
    s_tl_hl = (s_tl_cat >= 0 && k > 0) ? tl_row_item(s_tl_cat, k - 1) : -1;
    return r;
}
static int tl_back(void) { if (s_tc_item >= 0) { tl_activate(0); return 1; }   /* v1116B */ if (s_tl_cat < 0) return 0; const int c = s_tl_cat; s_tl_cat = -1; s_tl_hl = -1; s_tl_reset_sel = c; return 1; }

void gf_shop_init(void)
{
    const char* e = getenv("GF_SHOP"); if (e && *e == '0') return;
    g_fs_open_note = fs_note;
    g_uiov_shop = &k_shop_api;
}
