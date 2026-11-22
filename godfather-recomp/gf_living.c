/* gf_living.c -- Living City mod, step 1: scout the game's spawn groups.
 *
 * Every street population group (TrafficManager "requester": traffic, sidewalk people, police, street gangsters, mob war
 * cars) asks func_00285F30 (GetCurrentMaxActive, EU) how many entities it may have. The wrapper there hands the result to
 * gf_living_cap. With GF_LIVING=1 it logs each group once (vtable, type at +0xD8, its capacity fields, radius) and every
 * 20 s a summary: per group the live count (+0xE4) and the vtables of what it spawned (slot list +0xEC, 64 x 8 bytes).
 * GF_LIVING_MUL (default 1 = log only) multiplies the capacity of the groups whose type is listed in GF_LIVING_TYPES
 * (hex, comma separated). GF_LIVING_SPAWN_FASTER (default 1) divides each group's spawn delay (+0xAC, and +0xB0 used at
 * higher opposition levels: seconds until the next spawn, loaded into the countdown +0x2EC after each spawn) once, never
 * below 0.2 s, so a group fills up while the player drives. GF_LIVING_FORCE_TYPES (hex list): groups of these types get at
 * least their normal capacity (+0xB8) x GF_LIVING_MUL even when the game has them at 0 (the mob war groups 1C are 0 unless a
 * family is at war). Traffic (type 1E): at least GF_LIVING_TRAFFIC (0.7) x its normal capacity x GF_POP_MUL (the game cuts it
 * to 2 while the player drives, and Whompay's pop table only holds the first 24 groups), despawn radius at least 200 m. Log [living]. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>
#include <math.h>

extern uint8_t* vm_base;

static int rd(uint32_t ea, uint32_t* out)
{
    if (ea < 0x10000u || ea >= 0xF0000000u) return 0;
    __try { const uint8_t* p = vm_base + ea; *out = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static float rdf(uint32_t ea) { uint32_t u = 0; float f; rd(ea, &u); memcpy(&f, &u, 4); return f; }
static void wrf(uint32_t ea, float f)
{
    uint32_t u; memcpy(&u, &f, 4);
    __try { uint8_t* q = vm_base + ea; q[0] = (uint8_t)(u >> 24); q[1] = (uint8_t)(u >> 16); q[2] = (uint8_t)(u >> 8); q[3] = (uint8_t)u; }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static float faster(uint32_t ea, float div)
{
    const float v = rdf(ea);
    if (!(v > 0.0f && v < 600.0f)) return v;
    float n = v / div; if (n < 0.2f) n = v < 0.2f ? v : 0.2f;
    wrf(ea, n); return n;
}

#define MAXREQ 64
static struct { uint32_t req, vt, type; int game_cap, our_cap, maxcount; float delay; ULONGLONG seen; } s_req[MAXREQ];
static int s_n, s_on = -1, s_types_n; static float s_mul = 1.0f, s_faster = 1.0f; static uint32_t s_types[16], s_force[16]; static int s_force_n; static float s_traffic = 0.7f, s_popmul = 1.6f;
static ULONGLONG s_last;

static void setup(void)
{
    const char* e = getenv("GF_LIVING"); s_on = e && *e == '1';
    if ((e = getenv("GF_LIVING_MUL")) && atof(e) > 0.1) s_mul = (float)atof(e);
    if ((e = getenv("GF_LIVING_SPAWN_FASTER")) && atof(e) >= 1.0) s_faster = (float)atof(e);
    if ((e = getenv("GF_LIVING_TRAFFIC")) && atof(e) >= 0.0) s_traffic = (float)atof(e);
    if ((e = getenv("GF_POP_MUL")) && atof(e) > 0.1) s_popmul = (float)atof(e);
    if ((e = getenv("GF_LIVING_FORCE_TYPES")) && *e) {
        char buf[128]; strncpy(buf, e, sizeof buf - 1); buf[sizeof buf - 1] = 0;
        for (char* t = strtok(buf, ","); t && s_force_n < 16; t = strtok(NULL, ",")) s_force[s_force_n++] = (uint32_t)strtoul(t, NULL, 16);
    }
    if ((e = getenv("GF_LIVING_TYPES")) && *e) {
        char buf[128]; strncpy(buf, e, sizeof buf - 1); buf[sizeof buf - 1] = 0;
        for (char* t = strtok(buf, ","); t && s_types_n < 16; t = strtok(NULL, ",")) s_types[s_types_n++] = (uint32_t)strtoul(t, NULL, 16);
    }
    printf("[living] spawn group scout %s, capacity x%.2f for %d listed types (GF_LIVING / GF_LIVING_MUL / GF_LIVING_TYPES), %d types forced on (GF_LIVING_FORCE_TYPES), spawns %.1fx faster\n",
           s_on ? "ON" : "off", (double)s_mul, s_types_n, s_force_n, (double)s_faster); fflush(stdout);
}

/* vtables of up to 4 different things this group spawned */
static void spawned_vts(uint32_t req, char* out, size_t n)
{
    uint32_t seen[4]; int k = 0; out[0] = 0;
    for (int i = 0; i < 64 && k < 4; i++) {
        uint32_t a = 0, b = 0, vt = 0, e = 0;
        if (!rd(req + 0xECu + i * 8u, &a) || !rd(req + 0xF0u + i * 8u, &b)) break;
        (void)b; if (a < 0x10038u || !rd(a - 0x38u, &vt) || vt < 0x10000000u || vt >= 0x10200000u) continue;
        e = a - 0x38u;
        int dup = 0; for (int j = 0; j < k; j++) if (seen[j] == vt) dup = 1;
        if (dup) continue;
        seen[k++] = vt;
        const size_t l = strlen(out); snprintf(out + l, n - l, "%s%08X", l ? " " : "", vt);
    }
    if (!out[0]) snprintf(out, n, "-");
}

static void summary(void)
{
    printf("[living] --- %d spawn groups ---\n", s_n);
    const ULONGLONG now = GetTickCount64();
    for (int i = 0; i < s_n; i++) {
        if (now - s_req[i].seen > 20000) continue;   /* not asked for 20 s: freed with its area */
        uint32_t cnt = 0; char v[64]; rd(s_req[i].req + 0xE4u, &cnt); spawned_vts(s_req[i].req, v, sizeof v);
        if ((int)cnt > s_req[i].maxcount) s_req[i].maxcount = (int)cnt;
        printf("[living] group %08X vt %08X type %02X: %u alive (most %d), cap %d -> %d, radius %.0f m, spawns %s\n",
               s_req[i].req, s_req[i].vt, s_req[i].type, cnt, s_req[i].maxcount, s_req[i].game_cap, s_req[i].our_cap,
               (double)rdf(s_req[i].req + 0xA8u), v);
    }
    fflush(stdout);
}

static void feud_tick(void);
static void cars_tick(void);
/* test keys (game window focused): Numpad 7 street fight now, Numpad 8 car chase now, Numpad 9 crash now */
static volatile int s_key_fight, s_key_chase, s_key_crash;
static void poll_keys(void)
{
    static int on = -1; if (on < 0) { const char* e = getenv("GF_LIVING_TESTKEYS"); on = e && *e == '1'; }
    if (!on) return;
    static int prev[3]; const int vk[3] = { VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9 };
    HWND w = GetForegroundWindow(); DWORD pid = 0; if (w) GetWindowThreadProcessId(w, &pid);
    const int fg = pid == GetCurrentProcessId();
    for (int i = 0; i < 3; i++) { const int k = fg && (GetAsyncKeyState(vk[i]) & 0x8000); if (k && !prev[i]) {
        if (i == 0) s_key_fight = 1; else if (i == 1) s_key_chase = 1; else s_key_crash = 1;
        printf("[living] test key: %s now\n", i == 0 ? "street fight" : i == 1 ? "car chase" : "crash"); fflush(stdout); }
        prev[i] = k; }
}
int gf_living_cap(uint32_t req, int cap)
{
    if (s_on < 0) setup();
    /* the street events (Living City) run on their own switches; the spawn group changes below need GF_LIVING (Busier Streets) */
    poll_keys();
    { static ULONGLONG ft; const ULONGLONG t0 = GetTickCount64(); if (t0 - ft >= 1000 || s_key_fight || s_key_chase || s_key_crash) { ft = t0; feud_tick(); cars_tick(); } }
    if (!s_on) return cap;
    uint32_t vt = 0, type = 0; rd(req, &vt); rd(req + 0xD8u, &type);
    const ULONGLONG t = GetTickCount64();
    int k = -1; for (int i = 0; i < s_n; i++) if (s_req[i].req == req) { k = i; break; }
    /* a new group can reuse a freed one's address: our delay is gone from it -> treat it as new */
    if (k >= 0 && s_faster > 1.0f && rdf(req + 0xACu) != s_req[k].delay) { s_req[k] = s_req[--s_n]; k = -1; }
    if (k < 0 && s_n >= MAXREQ) {   /* table full: drop the group seen longest ago */
        int o = 0; for (int i = 1; i < s_n; i++) if (s_req[i].seen < s_req[o].seen) o = i;
        s_req[o] = s_req[--s_n];
    }
    if (k < 0) {
        uint32_t b4 = 0, b8 = 0, bc = 0, c4 = 0; rd(req + 0xB4u, &b4); rd(req + 0xB8u, &b8); rd(req + 0xBCu, &bc); rd(req + 0xC4u, &c4);
        k = s_n++; memset(&s_req[k], 0, sizeof s_req[k]); s_req[k].req = req; s_req[k].vt = vt; s_req[k].type = type;
        const float d0 = rdf(req + 0xACu), d1 = rdf(req + 0xB0u);
        const float n0 = s_faster > 1.0f ? faster(req + 0xACu, s_faster) : d0, n1 = s_faster > 1.0f ? faster(req + 0xB0u, s_faster) : d1;
        s_req[k].delay = rdf(req + 0xACu);
        if (type == 0x1Eu && s_traffic > 0.0f && rdf(req + 0xA8u) < 200.0f) wrf(req + 0xA8u, 200.0f);
        printf("[living] new group %08X vt %08X type %02X: cap now %d (fields b4 %d, b8 %d, night %d, weather %d), radius %.0f m, spawn delay %.2f -> %.2f s (high %.2f -> %.2f)\n",
               req, vt, type, cap, (int)b4, (int)b8, (int)bc, (int)c4, (double)rdf(req + 0xA8u), (double)d0, (double)n0, (double)d1, (double)n1); fflush(stdout);
    }
    s_req[k].seen = t;
    int nc = cap;
    if (s_mul != 1.0f && cap > 0) for (int i = 0; i < s_types_n; i++) if (s_types[i] == type) {
        nc = (int)((float)cap * s_mul + 0.5f); if (nc > 62) nc = 62; break; }
    for (int i = 0; i < s_force_n; i++) if (s_force[i] == type) {
        uint32_t b8 = 0; rd(req + 0xB8u, &b8);
        int f = (int)((float)(int)b8 * s_mul + 0.5f); if (f > 62) f = 62;
        if ((int)b8 > 0 && nc < f) nc = f; break; }
    if (type == 0x1Eu && s_traffic > 0.0f) {
        uint32_t b8 = 0; rd(req + 0xB8u, &b8);
        int f = (int)((float)(int)b8 * s_popmul * s_traffic + 0.5f); if (f > 62) f = 62;
        if ((int)b8 > 0 && nc < f) nc = f; }
    if (type == 0x03u) {   /* police: GF_LIVING_POLICE_MUL (default 2) x the game's normal size, not the full street multiplier */
        static float pm = -1.0f; if (pm < 0.0f) { const char* e = getenv("GF_LIVING_POLICE_MUL"); pm = e && atof(e) > 0.0 ? (float)atof(e) : 2.0f; }
        uint32_t b8 = 0; rd(req + 0xB8u, &b8); const int lim = (int)((float)(int)b8 * pm + 0.5f);
        if ((int)b8 >= 0 && nc > lim) nc = lim; }
    s_req[k].type = type; s_req[k].game_cap = cap; s_req[k].our_cap = nc;
    if (t - s_last >= 20000) { s_last = t; summary(); }
    return nc;
}

/* ------------------------------------------------------------------ Family feuds (GF_LIVING_FEUD=1)
 * CombatNPC::IsHostile (func_00123FDC, EU; 1.0 = enemy) only makes rival mobsters enemies of the Corleones and the player.
 * The wrapper asks gf_living_hostile when the game says "not an enemy": a mobster of one rival family (+0x153C: 1 Corleone,
 * 2 Barzini, 3 Cuneo, 4 Stracci, 5 Tattaglia, 9 police, 10 business guards) then treats a mobster of another rival family
 * as an enemy. Ambient mobsters never look for enemies on their own, so the feud director (feud_tick, from the spawn group
 * query) starts fights: every GF_LIVING_FEUD_EVERY (default 120: 2-4 min, the first 90 s in) it picks a rival mobster within 70 m of the player and a
 * mobster of another family (rival or Corleone) and sends both, plus up to 2 friends each, the game's own attack order
 * (CombatNPC::HandleActionRequestEvent func_001299D8, action 1: ValidCombat + ForceTarget + combat). A CombatNPC is an
 * entity whose vtable holds that handler. Log [feud]. */
#include "recompiled/ppu_recomp.h"
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
typedef void (*gfn)(ppu_context*);
#define TOC 0x00B4BF60u
#define FN_ACTION_REQ 0x001299D8u
extern int gf_npcs_within(float maxd, uint32_t* out, int max);

static gfn lookup(uint32_t addr)
{
    for (uint64_t k = 0; k < function_table_count; k++) if ((uint32_t)function_table[k].addr == addr) return function_table[k].func;
    return 0;
}
static int gcall(gfn f, const uint64_t* a, int n)
{
    ppu_context* cur = g_active_ctx; if (!cur || !f) return 0;
    static ppu_context c;
    __try {
        memcpy(&c, cur, sizeof c);
        c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1400u) & ~0xFull;
        c.gpr[2] = TOC; for (int i = 0; i < n && i < 8; i++) c.gpr[3 + i] = a[i];
        c.lr = 0;
        g_active_ctx = &c; f(&c);
        while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
        g_active_ctx = cur; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_active_ctx = cur; g_trampoline_fn = 0; return 0; }
}
static void wr32(uint32_t ea, uint32_t v)
{ __try { uint8_t* q = vm_base + ea; q[0] = (uint8_t)(v >> 24); q[1] = (uint8_t)(v >> 16); q[2] = (uint8_t)(v >> 8); q[3] = (uint8_t)v; } __except (EXCEPTION_EXECUTE_HANDLER) { } }

/* 1 = CombatNPC: some vtable slot's OPD points at the CombatNPC action handler (cached per vtable) */
static struct { uint32_t vt; int yes; } s_vtc[48]; static int s_vtc_n;
static int is_combat_npc(uint32_t e)
{
    uint32_t vt = 0; if (!rd(e, &vt) || vt < 0x10000000u || vt >= 0x10200000u) return 0;
    for (int i = 0; i < s_vtc_n; i++) if (s_vtc[i].vt == vt) return s_vtc[i].yes;
    int yes = 0;
    for (uint32_t k = 0; k < 0x400u && !yes; k += 4) { uint32_t opd = 0, code = 0; if (rd(vt + k, &opd) && rd(opd, &code) && code == FN_ACTION_REQ) yes = 1; }
    if (s_vtc_n < 48) { s_vtc[s_vtc_n].vt = vt; s_vtc[s_vtc_n].yes = yes; s_vtc_n++;
        printf("[feud] vtable %08X: %s\n", vt, yes ? "CombatNPC" : "not a CombatNPC"); fflush(stdout); }
    return yes;
}
static int in_car(uint32_t e) { uint32_t f = 0; return rd(e + 0x5C4u, &f) && (f & 8u); }
static int family(uint32_t e) { uint32_t f = 0; return rd(e + 0x153Cu, &f) ? (int)f : -1; }
static int alive(uint32_t e) { return rdf(e + 0x14C8u) > 0.0f; }
static int pos(uint32_t e, float p[3])
{ uint32_t m = 0; if (!rd(e + 0x68u, &m)) return 0; p[0] = rdf(m + 0x40u); p[1] = rdf(m + 0x44u); p[2] = rdf(m + 0x48u); return p[0] == p[0]; }
static float dist(const float a[3], const float b[3]) { const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2]; return sqrtf(x * x + y * y + z * z); }
static const char* fam_name(int f) { static const char* n[] = { "none", "Corleone", "Barzini", "Cuneo", "Stracci", "Tattaglia" }; return f >= 0 && f <= 5 ? n[f] : "?"; }

static int s_feud = -1; static volatile LONG s_feud_n[8][8];
static ULONGLONG s_feud_log;
static void feud_setup(void)
{
    const char* e = getenv("GF_LIVING_FEUD"); s_feud = e && *e == '1';
    printf("[feud] rival families fight each other %s (GF_LIVING_FEUD)\n", s_feud ? "ON" : "off"); fflush(stdout);
}
int gf_living_hostile(uint32_t self, uint32_t target)
{
    if (s_feud < 0) feud_setup();
    if (!s_feud || self == target || !is_combat_npc(target)) return 0;
    const int fa = family(self), fb = family(target);
    if (fa < 2 || fa > 5 || fb < 2 || fb > 5 || fa == fb) return 0;
    const LONG n = InterlockedIncrement(&s_feud_n[fa][fb]);
    const ULONGLONG t = GetTickCount64();
    if (n == 1 || t - s_feud_log >= 20000) { s_feud_log = t;
        printf("[feud] %s mobster %08X sees %s mobster %08X as an enemy (pairs so far:", fam_name(fa), self, fam_name(fb), target);
        for (int a = 2; a < 6; a++) for (int b = 2; b < 6; b++) if (s_feud_n[a][b]) printf(" %s>%s x%ld", fam_name(a), fam_name(b), s_feud_n[a][b]);
        printf(")\n"); fflush(stdout); }
    return 1;
}

/* the game's attack order: NPCActionRequestData {+4 action, +8 time, +0xC target's safe-pointer base (entity + 0x38)} */
static int order_attack(uint32_t npc, uint32_t target)
{
    static gfn f; if (!f) f = lookup(FN_ACTION_REQ); if (!f || !g_active_ctx) return 0;
    const uint32_t data = ((uint32_t)g_active_ctx->gpr[1] - 0x300u) & ~0xFu;
    wr32(data, 0); wr32(data + 4, 1); { float tm = 60.0f; uint32_t u; memcpy(&u, &tm, 4); wr32(data + 8, u); } wr32(data + 0xC, target + 0x38u);
    const uint64_t a[2] = { npc, data };
    return gcall(f, a, 2);
}

#define MAXF 4
static struct { uint32_t a[MAXF], b[MAXF]; int na, nb, fa, fb; ULONGLONG start; } s_ev;
static ULONGLONG s_next_ev, s_ev_end;
static int side_alive(const uint32_t* v, int n) { int k = 0; for (int i = 0; i < n; i++) if (is_combat_npc(v[i]) && alive(v[i])) k++; return k; }
static unsigned feud_every(void) { const char* e = getenv("GF_LIVING_FEUD_EVERY"); return e && atoi(e) > 0 ? (unsigned)atoi(e) : 120u; }
static void feud_tick(void)
{
    if (s_feud < 0) feud_setup();
    if (!s_feud) return;
    const ULONGLONG t = GetTickCount64();
    if (!s_next_ev) s_next_ev = t + 60000u;
    if (s_key_fight) { s_key_fight = 0; if (!s_ev.start) s_next_ev = t; else { printf("[feud] a fight is already running\n"); fflush(stdout); } }
    if (s_ev.start) {
        const int la = side_alive(s_ev.a, s_ev.na), lb = side_alive(s_ev.b, s_ev.nb);
        if (!la || !lb || t - s_ev.start > 120000) {
            printf("[feud] fight %s vs %s over after %.0f s: %d of %d and %d of %d still standing\n", fam_name(s_ev.fa), fam_name(s_ev.fb),
                   (double)(t - s_ev.start) / 1000.0, la, s_ev.na, lb, s_ev.nb); fflush(stdout);
            s_ev.start = 0; s_ev_end = t;
            const unsigned base = feud_every();
            s_next_ev = t + (ULONGLONG)(base + (unsigned)(t % (base + 10u))) * 1000u;
        }
        return;
    }
    if (t < s_next_ev) return;
    s_next_ev = t + 5000;   /* nobody suitable: look again in 5 s */
    uint32_t nearby[160]; const int n = gf_npcs_within(70.0f, nearby, 160);
    uint32_t mob[64]; int nm = 0;
    for (int i = 0; i < n && nm < 64; i++) { const int f = family(nearby[i]); if (f >= 1 && f <= 5 && alive(nearby[i]) && is_combat_npc(nearby[i])) mob[nm++] = nearby[i]; }
    /* leader A: a rival (2-5); B: the nearest mobster of another family (rival or Corleone). Every other event prefers a
     * pair that are both in cars (a car chase), when there is one. */
    static int s_evn; const int want_car = (s_evn & 1) != 0;
    uint32_t A = 0, B = 0, CA = 0, CB = 0; float best = 1e9f, cbest = 1e9f, pa[3], pb[3];
    for (int i = 0; i < nm; i++) { const int fi = family(mob[i]); if (fi < 2 || !pos(mob[i], pa)) continue;
        for (int j = 0; j < nm; j++) { const int fj = family(mob[j]); if (fj == fi || !pos(mob[j], pb)) continue;
            const float d = dist(pa, pb); if (d < best) { best = d; A = mob[i]; B = mob[j]; }
            if (in_car(mob[i]) && in_car(mob[j]) && d < cbest) { cbest = d; CA = mob[i]; CB = mob[j]; } } }
    if (want_car && CA) { A = CA; B = CB; best = cbest; }
    if (A) s_evn++;
    if (!A) { static int said; if (said++ < 10) { printf("[feud] no rival pair near the player (%d NPCs, %d mobsters)\n", n, nm); fflush(stdout); } return; }
    memset(&s_ev, 0, sizeof s_ev); s_ev.fa = family(A); s_ev.fb = family(B);
    s_ev.a[s_ev.na++] = A; s_ev.b[s_ev.nb++] = B;
    pos(A, pa); pos(B, pb);
    for (int i = 0; i < nm; i++) { float p[3]; if (mob[i] == A || mob[i] == B || !pos(mob[i], p)) continue;
        if (family(mob[i]) == s_ev.fa && s_ev.na < 3 && dist(p, pa) < 25.0f) s_ev.a[s_ev.na++] = mob[i];
        else if (family(mob[i]) == s_ev.fb && s_ev.nb < 3 && dist(p, pb) < 25.0f) s_ev.b[s_ev.nb++] = mob[i]; }
    int oks = 0;
    for (int i = 0; i < s_ev.na; i++) oks += order_attack(s_ev.a[i], s_ev.b[i % s_ev.nb]);
    for (int i = 0; i < s_ev.nb; i++) oks += order_attack(s_ev.b[i], s_ev.a[i % s_ev.na]);
    s_ev.start = t;
    int ca = 0, cb = 0; for (int i = 0; i < s_ev.na; i++) ca += in_car(s_ev.a[i]); for (int i = 0; i < s_ev.nb; i++) cb += in_car(s_ev.b[i]);
    printf("[feud] %s started: %d %s (%d in cars) vs %d %s (%d in cars), %.0f m apart (%d orders sent; %d mobsters near the player)\n",
           ca && cb ? "CAR CHASE" : "fight", s_ev.na, fam_name(s_ev.fa), ca, s_ev.nb, fam_name(s_ev.fb), cb, (double)best, oks, nm); fflush(stdout);
}

/* The police join a street fight and then test everyone near them; the player (a Corleone) counted as an enemy when
 * Corleone soldiers were fighting and got arrested for driving past. During a fight the director started and 30 s after
 * it, police (family 9) do not treat the player as an enemy. */
static int in_event(uint32_t e);
static int near_chase(uint32_t e);
static uint32_t the_player(void)
{
    uint32_t a = 0; if (!rd(0x10181290u, &a) || !rd(a + 4, &a) || !rd(a, &a)) return 0;
    return a >= 0x17FCu ? a - 0x17FCu : 0;
}
/* the player's hired crew (player +0x29F8: up to 5 handles = entity + 0x38, as gf_crew.c reads them) */
static int is_crew(uint32_t ent)
{
    const uint32_t pl = the_player();
    if (!pl || !ent) return 0;
    for (uint32_t i = 0; i < 5; i++) { uint32_t h = 0; if (rd(pl + 0x29F8u + 8u * i, &h) && h > 0x38u && h - 0x38u == ent) return 1; }
    return 0;
}
int gf_living_calm_police(uint32_t self, uint32_t target)
{
    if (s_feud != 1) return 0;
    if (is_crew(self) || is_crew(target)) return 0;   /* the player's crew fights the police with him (Onix) */
    {   /* police and mobsters leave each other alone (Onix: no police joining the family fights); the player is not affected */
        static int ign = -1; if (ign < 0) { const char* e = getenv("GF_LIVING_POLICE_IGNORE"); ign = !(e && *e == '0');
            printf("[feud] police and mobsters ignore each other %s (GF_LIVING_POLICE_IGNORE)\n", ign ? "ON" : "off"); fflush(stdout); }
        const int fs = family(self);
        if (ign && target != the_player() && is_combat_npc(target)) {
            const int ft = family(target);
            const uint32_t mob = fs == 9 ? target : self;
            if (((fs == 9 && ft >= 1 && ft <= 5) || (fs >= 1 && fs <= 5 && ft == 9)) && !in_event(mob)) {
                static volatile LONG n; const LONG k = InterlockedIncrement(&n);
                if (k == 1 || k % 500 == 0) { printf("[feud] police / mobster fight prevented (%ld so far)\n", k); fflush(stdout); }
                return 1; } }
        if (fs >= 1 && fs <= 5 && target && target == the_player() && near_chase(self)) {
            static ULONGLONG said; const ULONGLONG tn = GetTickCount64();
            if (tn - said > 10000) { said = tn; printf("[cars] chase mobster %08X leaves the player alone\n", self); fflush(stdout); }
            return 1; }
        if (fs != 9) return 0;
    }
    const ULONGLONG t = GetTickCount64();
    if (!s_ev.start && (!s_ev_end || t - s_ev_end > 30000)) return 0;
    if (!target || target != the_player()) return 0;
    static ULONGLONG said; if (t - said > 10000) { said = t; printf("[feud] police %08X: player left alone during the street fight\n", self); fflush(stdout); }
    return 1;
}

/* ------------------------------------------------------------------ Car chases and crashes (GF_LIVING_CARS=1)
 * The game's chase: AICarBehavior::SetChaseBrain (func_002A65C4, EU: the attached VehicleBrain -> brain type 2 + shooter timer,
 * car+0x7C4 |= 0x800) and the brain's target SafePtr at brain+0x80 (Wii VehicleBrain::SetTargetEntity; set here with the game's
 * own SafePtr assign func_00215FA8, which writes at r3+0xEC) + flag 0x100 in the u16 at brain+0xEC (target is a car).
 * Cars come from the spawn groups' slot lists. A car's family = its own field +0x738 (Wii GetFamilyOfVehicle fallback).
 *  - chase (every GF_LIVING_CHASE_EVERY, default 200 s): a rival mob car (family 2-5) within 120 m chases the nearest car of
 *    another family; its passengers shoot from the windows like a mob war car.
 *  - crash (every GF_LIVING_CRASH_EVERY, default 240 s): a traffic car (group type 1E) gets the chase brain aimed at the
 *    nearest other car within 25 m for 3 s, then its old brain type back; the game's own collision reaction does the rest.
 * Log [cars]. */
#define FN_SET_CHASE   0x002A65C4u
#define FN_GET_BRAIN   0x0043397Cu
#define FN_SET_BRAIN   0x002BC428u
#define FN_SAFEPTR_EC  0x00215FA8u
static int gcall_r(gfn f, const uint64_t* a, int n, uint64_t* ret)
{
    ppu_context* cur = g_active_ctx; if (!cur || !f) return 0;
    static ppu_context c;
    __try {
        memcpy(&c, cur, sizeof c);
        c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1400u) & ~0xFull;
        c.gpr[2] = TOC; for (int i = 0; i < n && i < 8; i++) c.gpr[3 + i] = a[i];
        c.lr = 0;
        g_active_ctx = &c; f(&c);
        while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
        g_active_ctx = cur; if (ret) *ret = c.gpr[3]; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_active_ctx = cur; g_trampoline_fn = 0; return 0; }
}
static uint32_t brain_of(uint32_t car)
{
    static gfn f; if (!f) f = lookup(FN_GET_BRAIN);
    uint64_t r = 0; const uint64_t a[1] = { car };
    if (!gcall_r(f, a, 1, &r)) return 0;
    const uint32_t b = (uint32_t)r; uint32_t v = 0; return rd(b, &v) && v ? b : 0;
}
static int set_target(uint32_t brain, uint32_t target)
{
    static gfn f; if (!f) f = lookup(FN_SAFEPTR_EC);
    const uint64_t a[2] = { brain + 0x80u - 0xECu, target };
    if (!gcall_r(f, a, 2, 0)) return 0;
    __try { uint8_t* q = vm_base + brain + 0xECu; const unsigned v = ((unsigned)q[0] << 8 | q[1]) | 0x100u; q[0] = (uint8_t)(v >> 8); q[1] = (uint8_t)v; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}
static int chase(uint32_t car, uint32_t target)
{
    static gfn f; if (!f) f = lookup(FN_SET_CHASE);
    const uint32_t b = brain_of(car); if (!b) return 0;
    const uint64_t a[1] = { car };
    if (!gcall_r(f, a, 1, 0)) return 0;
    return set_target(b, target);
}
/* 1 = AI car: some vtable slot's OPD points at SetChaseBrain (cached per vtable) */
static struct { uint32_t vt; int yes; } s_carvt[48]; static int s_carvt_n;
static int is_ai_car(uint32_t e)
{
    uint32_t vt = 0; if (!rd(e, &vt) || vt < 0x10000000u || vt >= 0x10200000u) return 0;
    for (int i = 0; i < s_carvt_n; i++) if (s_carvt[i].vt == vt) return s_carvt[i].yes;
    int yes = 0;
    for (uint32_t k = 0; k < 0x600u && !yes; k += 4) { uint32_t opd = 0, code = 0; if (rd(vt + k, &opd) && rd(opd, &code) && code == FN_SET_CHASE) yes = 1; }
    if (s_carvt_n < 48) { s_carvt[s_carvt_n].vt = vt; s_carvt[s_carvt_n].yes = yes; s_carvt_n++;
        printf("[cars] vtable %08X: %s\n", vt, yes ? "AI car" : "not an AI car"); fflush(stdout); }
    return yes;
}
static int car_family(uint32_t car) { uint32_t f = 0; return rd(car + 0x738u, &f) ? (int)f : -1; }

/* A spawn group's slot list (+0xEC, 64 x 8 bytes) holds SafePtrs: word 0 = entity + 0x38 (its SafeObj base), word 1 = list
 * link. The group holds the car itself (gf_world_is_car, Whompay) or, for some groups, the driver: then his car (+0x5C4 bit 8,
 * +0x1718, within 5 m). fam[] = the driver's family if there is one, else the car's family field +0x738. */
extern int gf_world_is_car(uint32_t ent);
static uint32_t slot_ent(uint32_t req, uint32_t i) { uint32_t a = 0; return rd(req + 0xECu + i * 8u, &a) && a >= 0x10038u ? a - 0x38u : 0; }
/* the game's GetVehicle (func_00233CCC, EU; our crew code uses it for the player): the car this NPC sits in, 0 if none */
#define FN_GETVEHICLE 0x00233CCCu
static uint32_t vehicle_of(uint32_t e)
{
    static gfn f; if (!f) f = lookup(FN_GETVEHICLE);
    uint64_t r = 0; const uint64_t a[1] = { e };
    if (!gcall_r(f, a, 1, &r)) return 0;
    return gf_world_is_car((uint32_t)r) ? (uint32_t)r : 0;
}
/* every live car (gf_world_cars, Whompay's car list), not the player's own; fam[] = the car's family field +0x738.
 * only_traffic: civilian cars only (not 1-5 families, not police 9). */
extern int gf_world_cars(uint32_t* out, int max);
static int group_cars(uint32_t* out, int* fam, int max, int only_traffic)
{
    uint32_t all[160]; const int na = gf_world_cars(all, 160);
    const uint32_t pl = the_player(), mine = pl ? vehicle_of(pl) : 0;
    int n = 0;
    for (int i = 0; i < na && n < max; i++) {
        if (all[i] == mine) continue;
        const int f = car_family(all[i]);
        if (only_traffic && ((f >= 1 && f <= 5) || f == 9)) continue;
        out[n] = all[i]; fam[n] = f; n++;
    }
    { static ULONGLONG said; const ULONGLONG now = GetTickCount64(); if (now - said > 60000) { said = now;
        printf("[cars] %d live cars (%d %s), families:", na, n, only_traffic ? "traffic" : "street");
        for (int k = 0; k < n && k < 16; k++) printf(" %d", fam[k]); printf("\n"); fflush(stdout); } }
    return n;
}
/* the chase the director started: both cars, for 2 minutes */
static struct { uint32_t a, b; ULONGLONG start; } s_chase;
static struct { uint32_t car, brain, old_type; ULONGLONG until; } s_crash;
static ULONGLONG s_next_chase, s_next_crash;
static unsigned env_secs(const char* k, unsigned d) { const char* e = getenv(k); return e && atoi(e) > 0 ? (unsigned)atoi(e) : d; }
static void cars_tick(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GF_LIVING_CARS"); on = e && *e == '1';
        printf("[cars] rival car chases every ~%u s and crashes every ~%u s %s (GF_LIVING_CARS)\n", env_secs("GF_LIVING_CHASE_EVERY", 200), env_secs("GF_LIVING_CRASH_EVERY", 240), on ? "ON" : "off"); fflush(stdout); }
    if (!on) return;
    const ULONGLONG t = GetTickCount64();
    const uint32_t pl = the_player(); float pp[3]; if (!pl || !pos(pl, pp)) return;
    if (!s_next_chase) { s_next_chase = t + 60000u; s_next_crash = t + 75000u; }
    const int forced_chase = s_key_chase; if (s_key_chase) { s_key_chase = 0; s_next_chase = t; }
    if (s_key_crash) { s_key_crash = 0; if (!s_crash.car) s_next_crash = t; }
    if (s_crash.car && t >= s_crash.until) {   /* crash over: the old brain type back */
        static gfn f; if (!f) f = lookup(FN_SET_BRAIN);
        const uint64_t a[3] = { s_crash.brain, s_crash.old_type, 1 };
        gcall_r(f, a, 3, 0); set_target(s_crash.brain, 0);
        printf("[cars] crash car %08X back to normal driving (brain type %u)\n", s_crash.car, s_crash.old_type); fflush(stdout);
        memset(&s_crash, 0, sizeof s_crash);
    }
    uint32_t cars[128];
    if (t >= s_next_chase) {
        s_next_chase = t + 10000u;   /* nobody suitable: again in 10 s */
        int fams[128]; int n = group_cars(cars, fams, 128, 0);
        {   uint32_t nearby[160]; const int nn = gf_npcs_within(150.0f, nearby, 160);
            for (int i = 0; i < nn && n < 128; i++) { const int f = family(nearby[i]); if (f < 1 || f > 5 || !is_combat_npc(nearby[i])) continue;
                const uint32_t c = vehicle_of(nearby[i]); if (!c) continue;
                int dup = 0; for (int k = 0; k < n; k++) if (cars[k] == c) dup = 1;
                if (!dup) { cars[n] = c; fams[n] = f; n++; } } }
        uint32_t A = 0, B = 0; int FA = 0, FB = 0; float best = 1e9f;
        for (int i = 0; i < n; i++) { float pa[3]; const int fa = fams[i]; if (fa < 2 || fa > 5 || !pos(cars[i], pa) || dist(pa, pp) > 120.0f) continue;
            for (int j = 0; j < n; j++) { float pb[3]; const int fb = fams[j]; if (j == i || fb < 1 || fb > 5 || fb == fa || !pos(cars[j], pb)) continue;
                const float d = dist(pa, pb); if (d < best && d < 150.0f) { best = d; A = cars[i]; B = cars[j]; FA = fa; FB = fb; } } }
        if (!A && forced_chase) {
            int tf[128]; const int tn = group_cars(cars, tf, 128, 1); float tb = 80.0f;
            for (int i = 0; i < tn; i++) { float pa[3]; if (!pos(cars[i], pa) || dist(pa, pp) > 100.0f) continue;
                for (int j = 0; j < tn; j++) { float pb[3]; if (j == i || !pos(cars[j], pb)) continue;
                    const float d = dist(pa, pb); if (d < tb && d > 8.0f) { tb = d; A = cars[i]; B = cars[j]; } } }
            if (A) { best = tb; printf("[cars] test chase: no mobster cars near, using two traffic cars\n"); fflush(stdout); }
        }
        if (A && chase(A, B)) {
            s_chase.a = A; s_chase.b = B; s_chase.start = t;
            printf("[cars] CAR CHASE: %s car %08X chases %s car %08X, %.0f m apart\n", fam_name(FA), A, fam_name(FB), B, (double)best); fflush(stdout);
            const unsigned e = env_secs("GF_LIVING_CHASE_EVERY", 200); s_next_chase = t + (ULONGLONG)(e + (unsigned)(t % (e / 2 + 1))) * 1000u;
        } else { static int said; if (said++ < 6) { printf("[cars] no rival mob cars to chase (%d mob cars tracked)\n", n); fflush(stdout); } }
    }
    if (!s_crash.car && t >= s_next_crash) {
        s_next_crash = t + 10000u;
        int fams[128]; const int n = group_cars(cars, fams, 128, 1);
        uint32_t A = 0, B = 0; float best = 25.0f;
        for (int i = 0; i < n; i++) { float pa[3]; if (!pos(cars[i], pa)) continue; const float dp = dist(pa, pp); if (dp < 25.0f || dp > 90.0f) continue;
            for (int j = 0; j < n; j++) { float pb[3]; if (j == i || !pos(cars[j], pb)) continue;
                const float d = dist(pa, pb); if (d < best && d > 4.0f) { best = d; A = cars[i]; B = cars[j]; } } }
        const uint32_t br = A ? brain_of(A) : 0; uint32_t old = 0;
        if (br && rd(br + 0xE0u, &old) && chase(A, B)) {
            s_crash.car = A; s_crash.brain = br; s_crash.old_type = old; s_crash.until = t + 3000u;
            printf("[cars] CRASH: traffic car %08X steers into car %08X (%.0f m apart, brain type %u)\n", A, B, (double)best, old); fflush(stdout);
            const unsigned e = env_secs("GF_LIVING_CRASH_EVERY", 240); s_next_crash = t + (ULONGLONG)(e + (unsigned)(t % (e / 2 + 1))) * 1000u;
        } else { static int said; if (said++ < 6) { printf("[cars] no traffic pair for a crash (%d traffic cars)\n", n); fflush(stdout); } }
    }
}

static int near_chase(uint32_t e)
{
    if (!s_chase.start || GetTickCount64() - s_chase.start > 120000) return 0;
    float p[3], q[3]; if (!pos(e, p)) return 0;
    if (pos(s_chase.a, q) && dist(p, q) < 50.0f) return 1;
    if (pos(s_chase.b, q) && dist(p, q) < 50.0f) return 1;
    return 0;
}
/* a mobster in a fight or chase the director started */
static int in_event(uint32_t e)
{
    if (s_ev.start) { for (int i = 0; i < s_ev.na; i++) if (s_ev.a[i] == e) return 1; for (int i = 0; i < s_ev.nb; i++) if (s_ev.b[i] == e) return 1; }
    return near_chase(e);
}
