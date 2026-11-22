/* gf_gps.c -- GPS route line (Onix, port of the "GPS Route Line" RPCS3 patch, Nexus mod 25): while the player is in a car,
 * a line of dots on the radar follows the streets from the car to the target, in the target's colour (white = waypoint,
 * blue = story, yellow = job, pink = favor). D-pad right (or G on the keyboard) in a car switches to the next target,
 * nearest first.
 *
 * Road network: tools/gf_gps_build.py joins every district's main road graph (the AI drivers' navigation GRAPH resources,
 * flags 0x4) into assets/gf_gps.bin. The route is a reverse Dijkstra from the target node over incoming edges, so the
 * car's nearest road segment only has to look up dist/next; it is solved again when the target moves.
 *
 * RE (BLES00043):
 *   func_000BDC1C(hud)  HUD per-frame update; hud+0xAC bit 0x2 = HUD active, hud+0x400 = radar marker manager. Hooked at
 *                       its entry (installer patch) -> gf_gps_hud(hud).
 *   func_000AC950(hud)  -> player entity. player+0x68 -> transform (+0x40/44/48 = x, y, z); player+0x1718 = vehicle.
 *   func_000AA4D8(hud, type, const float pos[3], 1 = radar only, 0, 0) -> marker slot (< 0 = none free).
 *   func_000AA530(hud, slot) removes it.  0x114EC (trampoline to 0x508178): (mgr, name) -> marker type by name.
 *   func_00027E0C(size) operator new (game heap).  *0x10180F5C pad manager: +0x134 = bit of the D-pad button that switches (right), +0x38 = buttons.
 *   marker manager: 200 slots of 0x2C at +0x44: x, y, z (float), +0x50 type, +0x54 flags (bit 0 = used).
 *   marker type (0x38): name, texture name, texture handle, uv[4], edge, size, max distance, flags, RGBA, 2 variants.
 * Env: GF_GPS=0 off; GF_GPS_KEY=<letter> the keyboard key (default G). Log tag [gps]. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "recompiled/ppu_recomp.h"

extern uint8_t* vm_base;
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern volatile int g_uiov_menu_open;   /* rsx_ui_overlay.c: one of the port's menus is open */
/* gf_crewdrive.c: the marker manager + target types, and the slot the crew driver is heading for (its route is shown) */
volatile uint32_t g_gps_mgr; volatile uint32_t g_gps_ttype[8]; volatile int g_gps_force_slot = -1;
/* read by rsx_ui_overlay.c (gps_build): the "switch GPS target" hint next to the radar while it is refreshed */
volatile unsigned long long g_gps_hint_t; volatile int g_gps_key = 'G';
typedef void (*gfn)(ppu_context*);
#define TOC 0x00B4BF60u

#define FN_GET_PLAYER  0x000AC950u
#define FN_MARKER_ADD  0x000AA4D8u
#define FN_MARKER_DEL  0x000AA530u
#define FN_TYPE_LOOKUP 0x000114ECu
#define FN_NEW         0x00027E0Cu
#define PAD_MGR_PTR    0x10180F5Cu

#define MAXDOTS 120
#define NSLOT   200
#define NTARGET 8      /* 0 waypoint, 1 story, 2 job, 3 favor (checkpoints), 4 truck, 5 story start, 6 job giver, 7 favor giver */
#define NDOT    4      /* white, blue, yellow, pink */

/* tuning (metres) */
#define SPACING  3.0f    /* between dots */
#define MAXLEN   450.0f  /* route length drawn */
#define VIS      170.0f  /* dots only within this straight-line distance of the player */
#define RETARGET 4.0f    /* solve again when the target moves more than this */
#define SNAP     3.0f    /* road segments within best + SNAP compete on route cost */
#define RESERVE  12      /* free marker slots left to the game */
#define UNIT     0.25f   /* metres per road coordinate unit (gf_gps_build.py) */
#define BIG      1e30f

static const char* const k_types[NTARGET] = { "player_settable", "story_mission_target", "job_target", "favor_target",
                                              "truck_destination", "story_mission", "job_giver", "favor_giver" };
static const uint32_t k_rgba[NDOT] = { 0xFFFFFFFFu, 0x3760C1FFu, 0xABAB00FFu, 0x9901AEFFu };

/* ---------------------------------------------------------------- guest memory */
static int ok(uint32_t ea) { return ea >= 0x10000u && ea < 0xF0000000u; }
static uint32_t rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t rd16(uint32_t ea) { const uint8_t* p = vm_base + ea; return (uint16_t)((p[0] << 8) | p[1]); }
static float rdf(uint32_t ea) { uint32_t u = rd32(ea); float f; memcpy(&f, &u, 4); return f; }
static void wr32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void wrf(uint32_t ea, float f) { uint32_t u; memcpy(&u, &f, 4); wr32(ea, u); }

static int fg_ours(void) { HWND w = GetForegroundWindow(); DWORD pid = 0; if (w) GetWindowThreadProcessId(w, &pid); return pid == GetCurrentProcessId(); }
static int key_down(void)
{
    static int vk = 0;
    if (!vk) { const char* e = getenv("GF_GPS_KEY"); vk = (e && isalnum((unsigned char)e[0])) ? toupper((unsigned char)e[0]) : 'G'; g_gps_key = vk; }
    return fg_ours() && !g_uiov_menu_open && (GetAsyncKeyState(vk) & 0x8000);
}

static gfn lookup(uint32_t addr)
{
    for (uint64_t k = 0; k < function_table_count; k++) if ((uint32_t)function_table[k].addr == addr) return function_table[k].func;
    return 0;
}
/* call a guest function on the current guest thread: up to 8 integer args (r3..r10); 0 = no context / fault */
static int gcall(gfn f, const uint64_t* a, int n, uint64_t* ret)
{
    ppu_context* cur = g_active_ctx; if (!cur || !f) return 0;
    static ppu_context c;
    __try {
        memcpy(&c, cur, sizeof c);
        c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1400u) & ~0xFull;
        c.gpr[2] = TOC; for (int i = 0; i < n && i < 8; i++) c.gpr[3 + i] = a[i];
        c.lr = 0;
        g_active_ctx = &c;
        f(&c);
        while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
        g_active_ctx = cur;
        if (ret) *ret = c.gpr[3];
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_active_ctx = cur; g_trampoline_fn = 0; return 0; }
}

/* ---------------------------------------------------------------- road network (assets/gf_gps.bin) */
static int s_nn, s_ne;
static int16_t *s_X, *s_Z;
static uint16_t *s_rs, *s_rsrc;
static float* s_dist; static uint16_t* s_next; static uint16_t* s_hn; static float* s_hk;
static void* s_blob;

static int load_roads(void)
{
    char exe[MAX_PATH], path[MAX_PATH + 64]; FILE* f = NULL;
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (n && n < MAX_PATH) { char* s = strrchr(exe, '\\'); if (s) *s = 0;
        static const char* rel[] = { "\\assets\\gf_gps.bin", "\\..\\assets\\gf_gps.bin", "\\..\\..\\assets\\gf_gps.bin" };
        for (int i = 0; i < 3 && !f; i++) { snprintf(path, sizeof path, "%s%s", exe, rel[i]); f = fopen(path, "rb"); } }
    if (!f) { printf("[gps] no assets\\gf_gps.bin (tools/gf_gps_build.py) -> GPS off\n"); return 0; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    s_blob = malloc((size_t)sz); if (!s_blob || fread(s_blob, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return 0; }
    fclose(f);
    const uint32_t* h = (const uint32_t*)s_blob;
    if (sz < 16 || memcmp(h, "GPS2", 4) != 0) { printf("[gps] gf_gps.bin: bad header -> GPS off\n"); return 0; }
    s_nn = (int)h[1]; s_ne = (int)h[2];
    uint8_t* p = (uint8_t*)s_blob + 16;
    s_X = (int16_t*)p; p += 2 * s_nn; s_Z = (int16_t*)p; p += 2 * s_nn;
    s_rs = (uint16_t*)p; p += 2 * (s_nn + 1); s_rsrc = (uint16_t*)p; p += 2 * s_ne;
    if (p > (uint8_t*)s_blob + sz) { printf("[gps] gf_gps.bin: truncated -> GPS off\n"); return 0; }
    s_dist = (float*)malloc(sizeof(float) * s_nn); s_next = (uint16_t*)malloc(2 * s_nn);
    s_hn = (uint16_t*)malloc(2 * (s_nn + s_ne)); s_hk = (float*)malloc(sizeof(float) * (s_nn + s_ne));
    printf("[gps] road network: %d nodes, %d edges\n", s_nn, s_ne);
    return s_dist && s_next && s_hn && s_hk;
}

/* binary heap keyed on float (lazy deletion) */
static void hpush(uint32_t* n, uint16_t node, float key)
{
    uint32_t i = (*n)++;
    while (i) { uint32_t p = (i - 1) >> 1; if (s_hk[p] <= key) break; s_hn[i] = s_hn[p]; s_hk[i] = s_hk[p]; i = p; }
    s_hn[i] = node; s_hk[i] = key;
}
static void hpop(uint32_t* n)
{
    uint32_t m = --(*n), i = 0; uint16_t node = s_hn[m]; float key = s_hk[m];
    for (;;) {
        uint32_t l = 2 * i + 1; if (l >= m) break;
        if (l + 1 < m && s_hk[l + 1] < s_hk[l]) l++;
        if (s_hk[l] >= key) break;
        s_hn[i] = s_hn[l]; s_hk[i] = s_hk[l]; i = l;
    }
    s_hn[i] = node; s_hk[i] = key;
}
/* reverse Dijkstra from t over incoming edges: dist[u] = cost u -> t, next[u] = following node */
static void solve(int t)
{
    uint32_t hs = 0, cap = (uint32_t)(s_nn + s_ne);
    for (int i = 0; i < s_nn; i++) { s_dist[i] = BIG; s_next[i] = 0xFFFF; }
    s_dist[t] = 0; hpush(&hs, (uint16_t)t, 0);
    while (hs) {
        uint16_t v = s_hn[0]; float dv = s_hk[0]; hpop(&hs);
        if (dv > s_dist[v]) continue;
        float vx = s_X[v], vz = s_Z[v];
        for (int e = s_rs[v]; e < s_rs[v + 1]; e++) {
            uint16_t u = s_rsrc[e]; float dx = s_X[u] - vx, dz = s_Z[u] - vz;
            float nd = dv + sqrtf(dx * dx + dz * dz) * UNIT;
            if (nd < s_dist[u]) { s_dist[u] = nd; s_next[u] = v; if (hs < cap) hpush(&hs, u, nd); }
        }
    }
}

/* ---------------------------------------------------------------- state */
static struct {
    gfn get_player, marker_add, marker_del, type_lookup, op_new;
    uint32_t gmem;             /* guest block: dot marker types, texture name, type names, scratch position */
    uint32_t mgr, ttype[NTARGET], dtype[NDOT];
    int16_t ids[MAXDOTS]; int nids;
    int have_route, tnode; float tx, tz;
    int sel_slot; uint32_t sel_type; int prevbtn, lastveh;
    unsigned frame;
} G = { .sel_slot = -1 };

#define GM_TYPES   0x000u               /* NDOT x 0x38 */
#define GM_TEX     0x0E0u               /* "hud001" */
#define GM_NAMES   0x0F0u               /* NTARGET x 32 */
#define GM_POS     0x1F0u               /* float[3] for marker_add */
#define GM_SIZE    0x200u

static int setup(void)
{
    G.get_player = lookup(FN_GET_PLAYER); G.marker_add = lookup(FN_MARKER_ADD); G.marker_del = lookup(FN_MARKER_DEL);
    G.type_lookup = lookup(FN_TYPE_LOOKUP); G.op_new = lookup(FN_NEW);
    if (!G.get_player || !G.marker_add || !G.marker_del || !G.type_lookup || !G.op_new) {
        printf("[gps] game functions not found -> GPS off\n"); return 0; }
    uint64_t a = GM_SIZE, r = 0;
    if (!gcall(G.op_new, &a, 1, &r) || !ok((uint32_t)r)) { printf("[gps] guest allocation failed -> GPS off\n"); return 0; }
    G.gmem = (uint32_t)r; memset(vm_base + G.gmem, 0, GM_SIZE);
    memcpy(vm_base + G.gmem + GM_TEX, "hud001", 7);
    for (int k = 0; k < NTARGET; k++) memcpy(vm_base + G.gmem + GM_NAMES + 32 * k, k_types[k], strlen(k_types[k]) + 1);
    for (int k = 0; k < NDOT; k++) {          /* radar marker types for the dots: the big white disc of hud001, tinted */
        const uint32_t d = G.gmem + GM_TYPES + 0x38u * k;
        wr32(d + 0x00, G.gmem + GM_TEX); wr32(d + 0x04, G.gmem + GM_TEX); wr32(d + 0x08, 0);
        wrf(d + 0x0C, 8.0f / 256); wrf(d + 0x10, 9.0f / 256); wrf(d + 0x14, 112.0f / 256); wrf(d + 0x18, 110.0f / 256);
        wrf(d + 0x1C, 0.46f); wrf(d + 0x20, 10.0f); wrf(d + 0x24, 0.0f);
        wr32(d + 0x28, 0x3E4); wr32(d + 0x2C, k_rgba[k]); wr32(d + 0x30, 0); wr32(d + 0x34, 0);
        G.dtype[k] = d;
    }
    return 1;
}

static void resolve_types(uint32_t mgr)
{
    for (int k = 0; k < NTARGET; k++) {
        uint64_t a[2] = { mgr, G.gmem + GM_NAMES + 32u * k }, r = 0;
        G.ttype[k] = gcall(G.type_lookup, a, 2, &r) ? (uint32_t)r : 0;
    }
    G.mgr = mgr; G.nids = 0;
    for (int k = 0; k < NTARGET; k++) g_gps_ttype[k] = G.ttype[k];
    g_gps_mgr = mgr;
    printf("[gps] marker types: waypoint %08X story %08X job %08X favor %08X\n", G.ttype[0], G.ttype[1], G.ttype[2], G.ttype[3]);
}

static uint32_t slot_ea(uint32_t mgr, int s) { return mgr + 0x44u + 0x2Cu * (uint32_t)s; }

/* ---------------------------------------------------------------- radar draw order
 * func_00509B38 draws the radar markers in slot order (0..199, later = on top). The game's icons usually hold the low
 * slots, so the dots (allocated later) covered them. Its slot loop asks here which slot to draw at step i: our dots
 * first, then every other slot in its usual order, so the icons draw over the route. Always a permutation of 0..199. */
uint32_t gf_gps_draw_slot(uint32_t mgr, uint32_t i)
{
    static uint8_t order[NSLOT];
    if (i >= NSLOT) return i;
    if (i == 0) {
        uint8_t mine[NSLOT]; int n = 0;
        memset(mine, 0, sizeof mine);
        if (mgr == G.mgr) for (int k = 0; k < G.nids && k < MAXDOTS; k++) if (G.ids[k] >= 0 && G.ids[k] < NSLOT) mine[G.ids[k]] = 1;
        for (int s = 0; s < NSLOT; s++) if (mine[s]) order[n++] = (uint8_t)s;
        for (int s = 0; s < NSLOT; s++) if (!mine[s]) order[n++] = (uint8_t)s;
    }
    return order[i];
}

/* ---------------------------------------------------------------- per HUD update */
void gf_gps_hud(uint64_t hud64)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GF_GPS"); on = (e && *e == '0') ? 0 : 1;
                  if (on) on = load_roads() && setup(); if (!on) printf("[gps] off\n"); }
    if (!on) return;
    const uint32_t hud = (uint32_t)hud64;
    if (!ok(hud) || !(rd32(hud + 0xAC) & 2)) return;
    if (++G.frame & 1) return;                                     /* every 2nd frame */
    const uint32_t mgr = rd32(hud + 0x400);
    if (!ok(mgr)) return;
    if (mgr != G.mgr) resolve_types(mgr);

    /* drop our ids if the manager was reset (save loaded, HUD rebuilt) */
    for (int i = 0; i < G.nids; i++) {
        const int s = G.ids[i]; const uint32_t ty = s < NSLOT ? rd32(slot_ea(mgr, s) + 0xC) : 0;
        int ours = 0; for (int k = 0; k < NDOT; k++) if (ty == G.dtype[k]) ours = 1;
        if (s >= NSLOT || !(vm_base[slot_ea(mgr, s) + 0x10] & 1) || !ours) { G.nids = 0; break; }
    }

    uint64_t a[1] = { hud }, r = 0;
    const uint32_t player = gcall(G.get_player, a, 1, &r) ? (uint32_t)r : 0;
    int need = 0; float pos[MAXDOTS][3]; int dotcol = 0;
    if (ok(player) && ok(rd32(player + 0x68))) need = -1;          /* -1: compute below */
    if (need == -1) {
        const uint32_t tr = rd32(player + 0x68);
        const float px = rdf(tr + 0x40), py = rdf(tr + 0x44), pz = rdf(tr + 0x48);

        /* candidate targets sorted by distance */
        int cs[48]; int ck[48]; float cd[48]; int nc = 0, nfree = 0;
        for (int s = 0; s < NSLOT; s++) {
            const uint32_t e = slot_ea(mgr, s);
            if (!(vm_base[e + 0x10] & 1)) { nfree++; continue; }
            const uint32_t ty = rd32(e + 0xC);
            for (int k = 0; k < NTARGET; k++) {
                if (!G.ttype[k] || ty != G.ttype[k] || nc >= 48) continue;
                const float mx = rdf(e), mz = rdf(e + 8), d = (mx - px) * (mx - px) + (mz - pz) * (mz - pz);
                int i = nc++;
                while (i && cd[i - 1] > d) { cs[i] = cs[i - 1]; ck[i] = ck[i - 1]; cd[i] = cd[i - 1]; i--; }
                cs[i] = s; ck[i] = k; cd[i] = d;
                break;
            }
        }
        int cur = -1;
        for (int i = 0; i < nc; i++) if (cs[i] == G.sel_slot && G.ttype[ck[i]] == G.sel_type) cur = i;
        if (g_gps_force_slot >= 0) for (int i = 0; i < nc; i++) if (cs[i] == g_gps_force_slot) cur = i;   /* the crew driver's destination */
        if (cur < 0) {                                             /* automatic: waypoint, else checkpoint, else mission start */
            int pcls = 9;
            for (int i = 0; i < nc; i++) { const int cls = ck[i] == 0 ? 0 : ck[i] <= 4 ? 1 : 2; if (cls < pcls) { pcls = cls; cur = i; } }
        }
        const int veh = rd32(player + 0x1718) != 0;
        int btn = 0; const uint32_t pad = rd32(PAD_MGR_PTR);
        if (ok(pad)) { const uint32_t idx = rd32(pad + 0x134); if (idx < 16) btn = (rd16(pad + 0x38) >> idx) & 1; }
        if (key_down()) btn = 1;
        if (btn && !G.prevbtn && veh && nc > 1 && cur >= 0) cur = (cur + 1) % nc;   /* D-pad right / G: next target */
        G.prevbtn = btn;
        if (veh && nc > 1) g_gps_hint_t = GetTickCount64();     /* something to switch to: show the hint */
        if (veh != G.lastveh) { G.lastveh = veh; printf("[gps] %s a car, %d targets\n", veh ? "in" : "out of", nc); }
        need = 0;
        if (cur < 0) { G.have_route = 0; G.sel_slot = -1; }
        else {
            G.sel_slot = cs[cur]; G.sel_type = G.ttype[ck[cur]];
            const int bestk = ck[cur];
            dotcol = (bestk == 0 || bestk == 4) ? 0 : (bestk <= 3 ? bestk : bestk - 4);
            if (veh) {                                             /* the line only while in a vehicle */
                const uint32_t te = slot_ea(mgr, cs[cur]);
                const float tx = rdf(te), tz = rdf(te + 8), iu = 1.0f / UNIT;
                if (!G.have_route || fabsf(tx - G.tx) + fabsf(tz - G.tz) > RETARGET) {
                    int tn = 0; float td = BIG; const float qx = tx * iu, qz = tz * iu;
                    for (int i = 0; i < s_nn; i++) { const float dx = s_X[i] - qx, dz = s_Z[i] - qz, d = dx * dx + dz * dz; if (d < td) { td = d; tn = i; } }
                    solve(tn); G.have_route = 1; G.tx = tx; G.tz = tz; G.tnode = tn;
                }
                /* nearest road segment to the player (u -> v), ties broken by route cost */
                const float qx = px * iu, qz = pz * iu, snap = SNAP * iu;
                float bseg = BIG, bcost = BIG, sx = qx, sz = qz; int sv = 0;
                for (int pass = 0; pass < 2; pass++)
                    for (int v = 0; v < s_nn; v++) {
                        const float vx = s_X[v], vz = s_Z[v];
                        for (int e = s_rs[v]; e < s_rs[v + 1]; e++) {
                            const int u = s_rsrc[e]; const float ux = s_X[u], uz = s_Z[u], ex = vx - ux, ez = vz - uz;
                            const float L2 = ex * ex + ez * ez; float t = 0;
                            if (L2 > 0) { t = ((qx - ux) * ex + (qz - uz) * ez) / L2; if (t < 0) t = 0; if (t > 1) t = 1; }
                            const float cx = ux + ex * t, cz = uz + ez * t, d = sqrtf((qx - cx) * (qx - cx) + (qz - cz) * (qz - cz));
                            if (pass == 0) { if (d < bseg) bseg = d; continue; }
                            if (d > bseg + snap || s_dist[v] >= BIG) continue;
                            const float rest = sqrtf((vx - cx) * (vx - cx) + (vz - cz) * (vz - cz)) * UNIT + s_dist[v];
                            if (rest < bcost) { bcost = rest; sv = v; sx = cx; sz = cz; }
                        }
                    }
                if (bcost < BIG) {                                 /* walk the route, a dot every SPACING metres */
                    int room = G.nids + nfree; room = room > RESERVE ? room - RESERVE : 0; if (room > MAXDOTS) room = MAXDOTS;
                    float ax = sx * UNIT, az = sz * UNIT, carry = 0, walked = 0; const float vis2 = VIS * VIS;
                    int node = sv, last = 0;
                    while (need < room && walked < MAXLEN && !last) {
                        float bx, bz;
                        if (node == G.tnode || s_next[node] == 0xFFFF) { bx = tx; bz = tz; last = 1; }
                        else { bx = s_X[node] * UNIT; bz = s_Z[node] * UNIT; }
                        const float ex = bx - ax, ez = bz - az, L = sqrtf(ex * ex + ez * ez);
                        if (L > 0) {
                            float t = SPACING - carry;
                            while (t <= L && need < room && walked + t < MAXLEN) {
                                const float x = ax + ex * (t / L), z = az + ez * (t / L);
                                if ((x - px) * (x - px) + (z - pz) * (z - pz) < vis2) { pos[need][0] = x; pos[need][1] = py; pos[need][2] = z; need++; }
                                t += SPACING;
                            }
                            carry = L - (t - SPACING); walked += L;
                        }
                        ax = bx; az = bz;
                        if (!last) node = s_next[node];
                    }
                }
            }
        }
    }
    if (need < 0) need = 0;

    /* move the dots we own, then add or remove the difference */
    const uint32_t dt = G.dtype[dotcol];
    const int keep = G.nids < need ? G.nids : need;
    for (int i = 0; i < keep; i++) {
        const uint32_t e = slot_ea(mgr, G.ids[i]);
        wrf(e, pos[i][0]); wrf(e + 4, pos[i][1]); wrf(e + 8, pos[i][2]); wr32(e + 0xC, dt);
    }
    while (G.nids > need) { uint64_t d[2] = { hud, (uint64_t)G.ids[--G.nids] }; gcall(G.marker_del, d, 2, NULL); }
    for (int i = keep; i < need; i++) {
        const uint32_t p = G.gmem + GM_POS;
        wrf(p, pos[i][0]); wrf(p + 4, pos[i][1]); wrf(p + 8, pos[i][2]);
        uint64_t d[6] = { hud, dt, p, 1, 0, 0 }, id = 0;
        if (!gcall(G.marker_add, d, 6, &id) || (int32_t)(uint32_t)id < 0) break;   /* no free slot */
        G.ids[G.nids++] = (int16_t)(int32_t)(uint32_t)id;
    }
}
