/* gf_crew.c -- crew mods (port of the RPCS3 "crewmod" patches to the EU build).
 *
 * Two crew members (GF_CREW_MAX, default 2; 1 = the game's own limit): the hire dialogue's condition 66
 * (func around 0x7BC74, EU) refuses any hire while the player has crew (flag 0x02000000 at player+0x1A74). The hook
 * there asks gf_crew_hire_blocked(player) instead: refused only when the crew count at player+0x2A20 has reached the
 * limit. The engine itself has 5 crew slots. Log [crew]. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

extern uint8_t* vm_base;

static uint32_t rd32(uint32_t ea)
{ const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static int envi(const char* n, int d) { const char* e = getenv(n); return (e && *e) ? atoi(e) : d; }

/* player: the player entity (r9 at the check); returns 1 when the hire must be refused */
int gf_crew_hire_blocked(uint64_t player)
{
    static int lim = -1;
    if (lim < 0) { lim = envi("GF_CREW_MAX", 2); if (lim < 1) lim = 1; if (lim > 5) lim = 5;
        printf("[crew] crew limit %d (GF_CREW_MAX)\n", lim); fflush(stdout); }
    const uint32_t p = (uint32_t)player;
    if (p < 0x10000u) return 1;
    const uint32_t n = rd32(p + 0x2A20);
    const int blocked = (int)n >= lim;
    printf("[crew] hire check: %u crew following, %s\n", n, blocked ? "refused" : "allowed"); fflush(stdout);
    return blocked;
}

/* ------------------------------------------------------------------ Crew Medic v1.2 (two-crew version), GF_MEDIC=1
 * A lethal hit while a crew member is alive knocks the player down at 1 HP, invulnerable, with the dynamite-style
 * 20 s countdown on the HUD. The first living crew member is pulled out of combat (his guard area shrunk onto the
 * player, the follow distances shortened); any other crew member keeps fighting. Once he is within 2.5 m (and the
 * player has been down >= 3 s) one point of damage opens his pain / recover interrupt, which the hooks turn into a calm
 * crouch (the "plant car bomb" clip in place of kneel_idle when his animation set has it); 3.2 s later the player gets
 * up with half health and he stands with a nod. 20 s without him, or no crew member left alive: the player dies.
 * Hooks (EU addresses): death path 0x23FCBC (gf_medic_death), Player::Update 0x1C479C (gf_medic_update), condition
 * evaluators 0x2254A0 player pain / 0x1A1AAC NPC pain / 0x1A6354 NPC recover / 0x146D3C combat AI (gf_medic_cond),
 * start-clip 0x3C870C / 0x3C92A4 and is-playing-clip 0x460B40 (gf_medic_clip). Log [medic]. */
#include <string.h>
#include <math.h>
#include <windows.h>
#include "recompiled/ppu_recomp.h"
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
typedef void (*gfn)(ppu_context*);
#define TOC 0x00B4BF60u

static void wr32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static float rdf(uint32_t ea) { uint32_t u = rd32(ea); float f; memcpy(&f, &u, 4); return f; }
static void wrf(uint32_t ea, float f) { uint32_t u; memcpy(&u, &f, 4); wr32(ea, u); }
static int ok(uint32_t ea) { return ea >= 0x10000u && ea < 0xF0000000u; }
static gfn lookup(uint32_t addr)
{
    for (uint64_t k = 0; k < function_table_count; k++) if ((uint32_t)function_table[k].addr == addr) return function_table[k].func;
    return 0;
}
static int gcall(gfn f, const uint64_t* a, int n, double f1, uint64_t* ret, double* fret)
{
    ppu_context* cur = g_active_ctx; if (!cur || !f) return 0;
    static ppu_context c;
    __try {
        memcpy(&c, cur, sizeof c);
        c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1400u) & ~0xFull;
        c.gpr[2] = TOC; for (int i = 0; i < n && i < 8; i++) c.gpr[3 + i] = a[i];
        c.fpr[1] = f1; c.lr = 0;
        g_active_ctx = &c; f(&c);
        while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
        g_active_ctx = cur; if (ret) *ret = c.gpr[3]; if (fret) *fret = c.fpr[1]; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_active_ctx = cur; g_trampoline_fn = 0; return 0; }
}
/* virtual call: slot = byte offset in the vtable (the OPD pointer there) */
static int vcall(uint32_t obj, uint32_t slot, const uint64_t* a, int n, double f1, uint64_t* ret, double* fret)
{
    if (!ok(obj)) return 0; const uint32_t vt = rd32(obj); if (!ok(vt)) return 0;
    const uint32_t opd = rd32(vt + slot); if (!ok(opd)) return 0;
    gfn f = lookup(rd32(opd)); if (!f) return 0;
    uint64_t b[8] = { obj }; for (int i = 0; i < n && i < 7; i++) b[1 + i] = a[i];
    return gcall(f, b, n + 1, f1, ret, fret);
}
static uint32_t player(void)
{
    __try { uint32_t a = rd32(0x10181290u); if (!ok(a)) return 0; a = rd32(a + 4); if (!ok(a)) return 0; a = rd32(a);
        return (ok(a) && a >= 0x17FCu) ? a - 0x17FCu : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

#define FOLLOW_A   0x00B4896Cu   /* FollowSM distances^2 (TOC data): 7.84, 14.44, 16.0, 25.0 */
#define TIMER_MGR  0x00B47390u   /* -> HUD timer manager pointer */
#define GAME_TIME  0x00B49C84u   /* -> game time */
#define FN_ADDTIMER 0x000C5D90u
#define FN_REMTIMER 0x000C5B7Cu
#define FN_NEW      0x00027E0Cu
#define CLIP_KNEEL_IDLE  0x7FA41DAAu
#define CLIP_CAR_BOMB    0x9C3D372Fu
#define CLIP_KNEEL_STAND 0x4EEDEA28u
#define CLIP_NOD_YES     0xAC07A64Bu
#define CLIP_STAND_BLOCK 0xE3048536u

static struct {
    int on, state;              /* 0 normal, 1 downed, 2 dying (the death hook lets the hit through) */
    ULONGLONG t0, kneel_t;
    uint32_t dmg;               /* guest copy of the DamageData that downed the player (64 bytes) */
    int timer;                  /* HUD countdown id, -1 none */
    uint32_t kneel_h;           /* crew handle (entity + 56) once he has started his crouch */
    uint32_t revive_h;          /* the crew member chosen to revive */
    uint32_t exit_h, exit_clip; int swap_done;
    uint32_t brain; uint32_t sv_r2, sv_o12, sv_o20;   /* his guard area while pulled out of combat */
} M = { -1, 0, 0, 0, 0, -1 };

static int medic_on(void)
{
    if (M.on < 0) { M.on = envi("GF_MEDIC", 1) != 0; printf("[medic] Crew Medic %s (GF_MEDIC)\n", M.on ? "ON" : "off"); fflush(stdout); }
    return M.on;
}
static uint32_t crew_alive(uint32_t pl)   /* first living crew member's handle, 0 none */
{
    for (int i = 0; i < 5; i++) {
        const uint32_t h = rd32(pl + 0x29F8u + 8u * (uint32_t)i);
        if (!ok(h) || h == 56u) continue;
        if (!(rd32(h + 7992u) & 0x20000000u)) continue;
        if (rdf(h + 5264u) > 0.0f) return h;
    }
    return 0;
}
static void follow_dist(int downed)
{
    wrf(FOLLOW_A + 0, downed ? 1.0f : 7.84f); wrf(FOLLOW_A + 4, downed ? 1.8f : 14.44f);
    wrf(FOLLOW_A + 8, downed ? 2.0f : 16.0f); wrf(FOLLOW_A + 12, downed ? 3.0f : 25.0f);
}
static void timer_remove(void)
{
    if (M.timer == -1) return;
    const int id = M.timer; M.timer = -1;
    const uint32_t mgr = rd32(rd32(TIMER_MGR)); if (!ok(mgr)) return;
    static gfn f; if (!f) f = lookup(FN_REMTIMER);
    const uint64_t a[2] = { mgr, (uint64_t)(int64_t)id }; gcall(f, a, 2, 0.0, NULL, NULL);
}
static void end_downed(uint32_t pl, int reason)   /* 1 healed elsewhere, 3 revived */
{
    if (reason == 1) M.exit_h = 0;
    M.kneel_h = 0; timer_remove(); follow_dist(0);
    M.state = 0; wr32(pl + 1288u, rd32(pl + 1288u) & ~1u);
    printf("[medic] %s\n", reason == 3 ? "revived by the crew -> back up with half health" : "downed state over (healed)"); fflush(stdout);
}
static void kill_player(uint32_t pl, int reason)   /* 2 no crew left, 4 out of time */
{
    M.exit_clip = CLIP_STAND_BLOCK; if (reason == 2) M.exit_h = 0;
    M.kneel_h = 0; timer_remove(); follow_dist(0);
    M.state = 2; wr32(pl + 1288u, rd32(pl + 1288u) & ~1u);
    printf("[medic] %s -> dead\n", reason == 2 ? "no crew member left" : "the crew did not make it in 20 s"); fflush(stdout);
    if (ok(M.dmg)) {
        wr32(M.dmg + 4, 0); wrf(M.dmg + 36, 100000.0f); wr32(M.dmg + 56, rd32(M.dmg + 56) & ~4u);
        const uint64_t a[2] = { rd32(rd32(GAME_TIME)), M.dmg }; vcall(pl, 456, a, 2, 0.0, NULL, NULL);
    }
    M.state = 0;
}

/* death path of TakeDamage (victim r30, DamageData r28): 1 = keep him alive (knocked down) */
int gf_medic_death(uint32_t victim, uint32_t dmg)
{
    if (!medic_on() || M.state == 2) return 0;
    const uint32_t pl = player(); if (!pl || victim != pl) return 0;
    if (!crew_alive(pl)) return 0;
    if (M.state != 1) {
        M.state = 1; M.t0 = GetTickCount64(); M.kneel_h = 0; M.swap_done = 0;
        if (ok(M.dmg) && ok(dmg)) memcpy(vm_base + M.dmg, vm_base + dmg, 64);
        wr32(dmg + 40, 4);   /* damage type 4 -> the knockedDown reaction */
        follow_dist(1);
        M.timer = -1;
        const uint32_t mgr = rd32(rd32(TIMER_MGR));
        static gfn f; if (!f) f = lookup(FN_ADDTIMER);
        if (ok(mgr) && f) { const uint64_t a[6] = { mgr, 0, 20000, 2, 0, 0 }; uint64_t r = 0;
            if (gcall(f, a, 6, 0.0, &r, NULL)) M.timer = (int)(int32_t)r; }
        printf("[medic] lethal hit with crew alive -> downed (20 s for the crew to reach you)\n"); fflush(stdout);
    }
    wr32(pl + 1288u, rd32(pl + 1288u) | 1u);   /* invulnerable while down */
    vcall(pl, 524, NULL, 0, 1.0, NULL, NULL);  /* SetHealth(1) */
    return 1;
}

/* Player::Update (r31 = the player) */
void gf_medic_update(uint32_t pl)
{
    if (!medic_on()) return;
    if (!M.dmg) { static gfn f; if (!f) f = lookup(FN_NEW); uint64_t a = 0x80, r = 0;
        if (f && gcall(f, &a, 1, 0.0, &r, NULL) && ok((uint32_t)r)) { M.dmg = (uint32_t)r; memset(vm_base + M.dmg, 0, 0x80); } else M.dmg = 1; }
    if (M.state != 1 || pl != player()) return;
    const float hp = rdf(pl + 0x14C8u), mx = rdf(pl + 0x14CCu);
    if (hp > mx * 0.5f) { end_downed(pl, 1); return; }
    wrf(pl + 0x14C8u, 1.0f);
    const uint32_t h = crew_alive(pl);
    if (!h) { kill_player(pl, 2); return; }
    M.revive_h = h;
    const uint32_t ct = rd32(h + 48u), pt = rd32(pl + 104u);
    float d2 = 1e9f;
    if (ok(ct) && ok(pt)) { const float dx = rdf(ct + 64) - rdf(pt + 64), dy = rdf(ct + 68) - rdf(pt + 68), dz = rdf(ct + 72) - rdf(pt + 72); d2 = dx * dx + dy * dy + dz * dz; }
    const ULONGLONG now = GetTickCount64();
    if (!M.kneel_h) {
        const ULONGLONG el = now - M.t0;
        if (el >= 20000) { kill_player(pl, 4); return; }
        if (el < 3000 || d2 >= 6.25f) return;
        /* he reached you: one point of damage opens his pain / recover interrupt; the hooks make it a calm crouch */
        M.kneel_h = h; M.exit_h = h; M.swap_done = 0; M.exit_clip = CLIP_NOD_YES; M.kneel_t = now;
        if (ok(M.dmg)) { wr32(M.dmg + 4, 0); wrf(M.dmg + 36, 1.0f);
            const uint64_t a[2] = { rd32(rd32(GAME_TIME)), M.dmg }; vcall(h - 56u, 456, a, 2, 0.0, NULL, NULL); }
        printf("[medic] crew member reached you -> tending\n"); fflush(stdout);
        return;
    }
    if (now - M.kneel_t < 3200) return;
    vcall(pl, 524, NULL, 0, (double)(mx * 0.5f), NULL, NULL);   /* SetHealth(max / 2) */
    end_downed(pl, 3);
}

/* condition evaluators: kind 0 player pain, 1 NPC pain, 2 NPC recover, 3 combat AI (r3 = state machine, r6 = condition,
 * r7 = parameter block). Returns 1 with *res set to override the answer. */
int gf_medic_cond(int kind, uint64_t sm64, uint64_t cond64, uint64_t par64, int* res)
{
    if (M.on <= 0) return 0;
    const uint32_t sm = (uint32_t)sm64, cond = (uint32_t)cond64, par = (uint32_t)par64;
    if (kind == 0) { if (M.state == 1 && cond == 7) { *res = 0; return 1; } return 0; }   /* stay knocked down */
    if (kind == 1 || kind == 2) {
        if (!M.kneel_h || !ok(sm) || rd32(sm + 76) + 56u != M.kneel_h) return 0;
        if (kind == 1) {   /* no hit reaction: straight to recover */
            if (cond == 4) { *res = 1; return 1; }
            if (cond == 11 || cond == 13) { *res = 0; return 1; }
            return 0;
        }
        if (cond == 11) { *res = ok(par) && rd32(par + 4) == 1; return 1; }   /* recover posture: kneel */
        if (cond == 17 || cond == 9 || cond == 14 || cond == 15 || cond == 16) { *res = 0; return 1; }   /* hold the kneel */
        return 0;
    }
    /* kind 3: combat AI */
    if (M.state != 1) {
        if (ok(M.brain)) { wr32(M.brain + 268, M.sv_r2); wr32(M.brain + 236, M.sv_o12); wr32(M.brain + 244, M.sv_o20); M.brain = 0; }
        return 0;
    }
    const uint32_t owner = ok(sm) ? rd32(sm + 80) : 0;
    if (!ok(owner) || !(rd32(owner + 8048) & 0x20000000u) || owner + 56u != M.revive_h) return 0;
    const uint32_t brain = rd32(sm + 148);
    if (ok(brain)) {
        if (!M.brain) { M.brain = brain; M.sv_r2 = rd32(brain + 268); M.sv_o12 = rd32(brain + 236); M.sv_o20 = rd32(brain + 244); }
        if (M.brain == brain) { wrf(brain + 268, 1.0f); wr32(brain + 236, 0); wr32(brain + 244, 0); }   /* guard right at the player */
    }
    if (cond == 55 || cond == 54) { *res = 1; return 1; }   /* -> guardObject */
    switch (cond) { case 53: case 40: case 58: case 39: case 21: case 25: case 69: case 71: case 38: case 44: case 60: *res = 0; return 1; }
    return 0;
}

static int set_has_clip(uint32_t comp, uint32_t hash)   /* the component's animation set has this clip */
{
    const uint32_t set = rd32(comp + 4); if (!ok(set)) return 0;
    const uint32_t tab = rd32(set + 48); if (!ok(tab)) return 0;
    const uint32_t n = rd32(tab + 4), b = rd32(tab); if (!n || !ok(b)) return 0;
    uint32_t node = rd32(b + 4u * (hash % n));
    for (int guard = 0; ok(node) && guard < 4096; guard++) { if (rd32(node) == hash) return 1; node = rd32(node + 12); }
    return 0;
}
/* kind 0: start clip (r3 = animation component, r4 = clip); kind 1: is-playing-clip (r3 = entity). Returns r4 to use. */
uint64_t gf_medic_clip(int kind, uint64_t r3_64, uint64_t r4_64)
{
    if (M.on <= 0) return r4_64;
    const uint32_t r3 = (uint32_t)r3_64, clip = (uint32_t)r4_64;
    __try {
        if (kind == 1) {
            if (M.swap_done && M.kneel_h && r3 + 56u == M.kneel_h && clip == CLIP_KNEEL_IDLE) return CLIP_CAR_BOMB;
            return r4_64;
        }
        if (clip == CLIP_KNEEL_IDLE && M.kneel_h && rd32(M.kneel_h + 140) == r3) {
            if (!set_has_clip(r3, CLIP_CAR_BOMB)) return r4_64;   /* no crouch clip: the normal kneel */
            if (!M.swap_done) { M.swap_done = 1; M.kneel_t = GetTickCount64(); }   /* the revive countdown starts with the clip */
            return CLIP_CAR_BOMB;
        }
        if (clip == CLIP_KNEEL_STAND && M.exit_h && rd32(M.exit_h + 140) == r3) {
            M.exit_h = 0;
            if (!M.swap_done) return r4_64;
            return set_has_clip(r3, M.exit_clip) ? M.exit_clip : CLIP_STAND_BLOCK;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return r4_64;
}

/* ------------------------------------------------------------------ second crew health bar (GF_CREW_HUD2, default 1)
 * The HUD layout (godfather_global_global.str chunk 29, built by build_hud2 for the EU file) holds a copy of the crew
 * bar's 6 elements, renamed crw2Health*, one frame lower. gf_crew_hud_init (after the HUD looks up its crewRank element,
 * 0xC1C9C) finds them; gf_crew_hud_frame (each HUD update, 0xBDC1C) shows them and fills them from the second crew
 * member while two follow the player, else hides them. Element finders / meter calls live in the second TOC module. */
#define TOC2 (TOC + 0x10000u)   /* EU trampolines: addis r2,r2,1 (the US build subtracted 16) */
#define FN_FINDMETER 0x0050BC64u
#define FN_FINDELEM  0x005062F0u
#define FN_SETVAL    0x0050BC2Cu
#define FN_HIDE      0x00506048u
#define FN_HUDPLAYER 0x000AC950u
#define FN_SHOW      0x000AD248u
static uint32_t s_h2[6];   /* meter, regen meter, border, colour normal, colour low, background */
static int gcall2(gfn f, const uint64_t* a, int n, uint64_t* ret)   /* a function of the second module (r2 = TOC2) */
{
    ppu_context* cur = g_active_ctx; if (!cur || !f) return 0;
    static ppu_context c;
    __try {
        memcpy(&c, cur, sizeof c);
        c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1600u) & ~0xFull;
        c.gpr[2] = TOC2; for (int i = 0; i < n && i < 8; i++) c.gpr[3 + i] = a[i];
        c.lr = 0; g_active_ctx = &c; f(&c);
        while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
        g_active_ctx = cur; if (ret) *ret = c.gpr[3]; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_active_ctx = cur; g_trampoline_fn = 0; return 0; }
}
/* The copy lives in the HUD file whether the mod is on or not, so it is always looked up and kept hidden
 * unless GF_CREW_HUD2 is on and two crew members follow the player. */
void gf_crew_hud_init(void)
{
    static const char* names[6] = { "crw2HealthMeter", "crw2HealthRegenMeter", "crw2HealthBorder", "crw2HealthMeterColorNormal",
                                    "crw2HealthMeterColorLow", "crw2HealthMeterBackground" };
    static uint32_t gstr; static gfn fnew, fm, fe;
    if (!fnew) { fnew = lookup(FN_NEW); fm = lookup(FN_FINDMETER); fe = lookup(FN_FINDELEM); }
    if (!gstr) { uint64_t a = 6 * 32, r = 0; if (!fnew || !gcall(fnew, &a, 1, 0.0, &r, NULL) || !ok((uint32_t)r)) return; gstr = (uint32_t)r;
        for (int i = 0; i < 6; i++) memcpy(vm_base + gstr + 32 * i, names[i], strlen(names[i]) + 1); }
    int found = 0;
    for (int i = 0; i < 6; i++) { uint64_t a = gstr + 32u * (uint32_t)i, r = 0; s_h2[i] = 0;
        if (gcall2(i < 2 ? fm : fe, &a, 1, &r) && ok((uint32_t)r)) { s_h2[i] = (uint32_t)r; found++; } }
    printf("[crew] second crew health bar: %d of 6 HUD elements found\n", found); fflush(stdout);
}
void gf_crew_hud_frame(uint64_t hud64)
{
    const uint32_t hud = (uint32_t)hud64;
    if (!s_h2[0] || !ok(hud)) return;
    __try {
        if (!(rd32(hud + 0xAC) & 2)) return;   /* HUD not active */
        static gfn fpl, fshow, fset, fhide;
        if (!fpl) { fpl = lookup(FN_HUDPLAYER); fshow = lookup(FN_SHOW); fset = lookup(FN_SETVAL); fhide = lookup(FN_HIDE); }
        uint64_t r = 0; uint32_t crew2 = 0;
        if (envi("GF_CREW_HUD2", 1) && gcall(fpl, (uint64_t[]){ hud }, 1, 0.0, &r, NULL) && ok((uint32_t)r) && rd32((uint32_t)r + 0x2A20) >= 2) {
            const uint32_t pl = (uint32_t)r; int n = 0;
            for (int i = 0; i < 5; i++) { const uint32_t h = rd32(pl + 0x29F8u + 8u * (uint32_t)i);
                if (!h || h == 0x38u) continue; if (++n == 2) { crew2 = h - 0x38u; break; } }
            if (crew2 && !(rdf(crew2 + 0x14C8) > 0.0f)) crew2 = 0;
        }
        if (crew2) {
            for (int i = 0; i < 6; i++) if (s_h2[i] && !(rd32(s_h2[i] + 0x20) & 1)) { const uint64_t a[4] = { hud, s_h2[i], 0, 0 }; gcall(fshow, a, 4, 0.0, NULL, NULL); }
            const int hp = (int)rdf(crew2 + 0x14C8), mx = (int)rdf(crew2 + 0x14CC), rg = (int)rdf(crew2 + 0x14D0);
            { const uint64_t a[2] = { s_h2[0], (uint64_t)(int64_t)hp }; gcall2(fset, a, 2, NULL); wr32(s_h2[0] + 0x5C, (uint32_t)mx); }
            if (s_h2[1]) { const uint64_t a[2] = { s_h2[1], (uint64_t)(int64_t)rg }; gcall2(fset, a, 2, NULL); wr32(s_h2[1] + 0x5C, (uint32_t)mx); }
        } else {
            for (int i = 0; i < 6; i++) if (s_h2[i] && (rd32(s_h2[i] + 0x20) & 1)) { const uint64_t a[2] = { s_h2[i], 1 }; gcall2(fhide, a, 2, NULL); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

/* ------------------------------------------------------------------ two crew in a car (GF_CREW_CAR, default 1)
 * Seats: a crew member getting into the player's car looks for one door (NPCRailShooterSM::EnteredVehicle, EU 0x197920,
 * its FindEntryLocator inlined): rear door 13 when the car has rear seats, else the front passenger door 11. With two
 * crew hired, gf_crew_entry_id sends the first one in to the front passenger seat (door 11) and the next one to the
 * rear seat behind the driver (door 12). One crew member alone keeps the game's rear seat.
 * Correction after the test: sending the first one to door 11 broke the second one's entry (both go in through the
 * front passenger door). Now both take the rear seats (doors 13 and 12, as the game can), and once both sit in the
 * back the first-hired one moves to the front passenger seat with the game's NPCRailShooterSM::MoveToFrontSeat
 * (EU 0x195008); the other ends up behind the driver (seat 8, via BaseVehicle::SwitchCurrentSeatLocator 0x434794).
 * Second correction: the moved one's window did not open (the window is picked when he gets in) and both still
 * walked to the front passenger door. Now each one gets his own door before walking up (VehicleEntrySM::
 * DetermineEntryLocator, EU 0x2DC750, reads the NPC's preferred door +0x538: 0 FL, 1 FR, 2 RL, 3 RR): first-hired ->
 * front passenger door, second-hired -> rear left door, and each is seated directly in the matching seat (doors 11 /
 * 12). The move above stays only as a fallback for when both end up in the back.
 * Windows: the game's crew buttons are R2 / R key = right window (flag 0x200 in rail shooter SM +0xFC) and L2 / Q key =
 * left window (0x100); UpdateInputToggle (EU 0x194A28) gave every press to every crew member in the car. With two in
 * the car, gf_crew_input_turn gives each one his own button: the one on the right side (seats 7 / 9) takes R, the one on
 * the left (6 / 8) takes Q, so both can lean out at once. Seat locator ids from the game's CheckSwitchSides. */
#define FN_GETVEHICLE 0x00233CCCu
#define FN_PRESSED    0x001B7B9Cu
#define FN_MOVEFRONT  0x00195008u
#define FN_SWITCHSEAT 0x00434794u
static int crew_hire_order(uint32_t pl, uint32_t ent)
{
    for (int i = 0; i < 5; i++) { const uint32_t h = rd32(pl + 0x29F8u + 8u * (uint32_t)i); if (h && h - 0x38u == ent) return i; }
    return 9;
}
/* door locator id this crew member should use (11 front passenger / 12 rear left), 0 = leave it to the game.
 * The door slot (+0x538) is an index into the car's own door list, so the wrapper in ppu_recomp_001 (EU 0x2DC750)
 * tries each slot and keeps the one that gives this door. */
int gf_crew_door_want(uint64_t sent64)
{
    if (!envi("GF_CREW_CAR", 1)) return 0;
    __try {
        const uint32_t e = (uint32_t)sent64, pl = player(); if (!pl || !ok(e) || e == pl || rd32(pl + 0x2A20) < 2) return 0;
        const int o = crew_hire_order(pl, e); return o == 0 ? 0xB : o == 1 ? 0xC : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
void gf_crew_door_done(uint64_t sent64, int want, int got)
{
    static uint32_t s_e; static int s_w, s_g;
    if ((uint32_t)sent64 == s_e && want == s_w && got == s_g) return;
    s_e = (uint32_t)sent64; s_w = want; s_g = got;
    printf("[crew] crew member walks to the %s door%s\n", want == 0xB ? "front passenger" : "rear left", got ? "" : " (NOT FOUND, game's choice)"); fflush(stdout);
}
/* Getting out, the game first moves a crew member to a free front seat (NPCRailShooterSM::MoveToFrontSeat, EU
 * 0x195008) and lets him out by the front door. With two crew the rear one was squeezed into the front too and was
 * left standing inside the car, so while another crew member shares the car he gets out by his own rear door. */
static int s_arranging;
static uint32_t crew_vehicle_of(uint32_t e);
int gf_crew_front_skip(uint64_t sm64)
{
    if (s_arranging || !envi("GF_CREW_CAR", 1)) return 0;
    __try {
        const uint32_t sm = (uint32_t)sm64, pl = player();
        if (!pl || !ok(sm) || rd32(pl + 0x2A20) < 2) return 0;
        const uint32_t seat = rd32(sm + 0x68), h = rd32(sm + 0x58), me = rd32(sm + 0x4C);
        if (seat != 8 && seat != 9) return 0;
        const uint32_t veh = h >= 0x38u ? h - 0x38u : 0;
        for (int i = 0; i < 5; i++) {
            const uint32_t ch = rd32(pl + 0x29F8u + 8u * (uint32_t)i);
            if (!ok(ch) || ch == 0x38u || ch - 0x38u == me) continue;
            const uint32_t v = crew_vehicle_of(ch - 0x38u);
            if (v && (v == veh || v == h)) {
                printf("[crew] rear crew member gets out by his own door\n"); fflush(stdout);
                return 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return 0;
}

/* both crew in the back: first-hired -> front passenger seat (7), the other -> behind the driver (8) */
static void crew_arrange(uint32_t pl, uint32_t smA, uint32_t smB)
{
    const uint32_t sa = rd32(smA + 0x68), sb = rd32(smB + 0x68);
    if (!((sa == 8 || sa == 9) && (sb == 8 || sb == 9))) return;
    const int fa = crew_hire_order(pl, rd32(smA + 0x4C)) < crew_hire_order(pl, rd32(smB + 0x4C));
    const uint32_t front = fa ? smA : smB, rear = fa ? smB : smA;
    static gfn fmove, fswitch; if (!fmove) { fmove = lookup(FN_MOVEFRONT); fswitch = lookup(FN_SWITCHSEAT); }
    const uint64_t a1[1] = { front };
    s_arranging = 1;
    const int moved = fmove && gcall(fmove, a1, 1, 0.0, NULL, NULL);
    s_arranging = 0;
    if (!moved || rd32(front + 0x68) != 7) return;
    wr32(front + 0xFC, (rd32(front + 0xFC) & ~2u) | 4u);
    printf("[crew] first-hired crew member moved to the front passenger seat\n"); fflush(stdout);
    if (rd32(rear + 0x68) == 9 && fswitch) {
        const uint64_t a2[3] = { rd32(rear + 0x58) - 0x38u, rd32(rear + 0x4C), rear + 0x70 };
        if (gcall(fswitch, a2, 3, 0.0, NULL, NULL)) {
            const uint32_t i0 = rd32(rear + 0x68), i1 = rd32(rear + 0x6C);
            wr32(rear + 0x68, rd32(rear + 0x70)); wr32(rear + 0x6C, rd32(rear + 0x74)); wr32(rear + 0x70, i0); wr32(rear + 0x74, i1);
            wr32(rear + 0xFC, rd32(rear + 0xFC) | 2u);
            printf("[crew] second crew member moved behind the driver\n"); fflush(stdout);
        }
    }
}
static uint32_t crew_vehicle_of(uint32_t e) { return (rd32(e + 0x5C4) & 8u) ? rd32(e + 0x1718) : 0; }
int gf_crew_entry_id(int id, uint64_t vh_addr64, uint64_t ent64)
{
    if (!envi("GF_CREW_CAR", 1) || id != 0xD) return id;
    const uint32_t vh = rd32((uint32_t)vh_addr64), veh = vh >= 0x38u ? vh - 0x38u : 0, ent = (uint32_t)ent64;
    const uint32_t pl = player(); if (!pl || !veh) return id;
    if (rd32(pl + 0x2A20) < 2) return id;                  /* one crew member: the game's rear seat */
    static uint32_t s_last_ent; static int s_last_id;
    const int r = crew_hire_order(pl, ent) == 0 ? 0xB : 0xC;
    if (ent != s_last_ent || r != s_last_id) { s_last_ent = ent; s_last_id = r;
        printf("[crew] crew member gets in -> %s\n", r == 0xB ? "front passenger seat" : "rear seat behind the driver"); fflush(stdout); }
    return r;
}
static struct { uint32_t sm; ULONGLONG t; uint32_t seat; } s_tog[4];
static int crew_right(uint32_t seat) { return seat == 7 || seat == 9; }
/* 1 = this crew member takes R (right window), 2 = Q (left window) */
static int crew_role(int k, int o)
{
    const uint32_t a = rd32(s_tog[k].sm + 0x68), b = rd32(s_tog[o].sm + 0x68);
    if (crew_right(a) != crew_right(b)) return crew_right(a) ? 1 : 2;
    if (a != b) return a < b ? 1 : 2;                     /* same side: the front one keeps it, the rear one moves over */
    return k < o ? 1 : 2;
}
/* 1 = run the game's UpdateInputToggle, 0 = handled here */
int gf_crew_input_turn(uint64_t sm64)
{
    const uint32_t sm = (uint32_t)sm64; const ULONGLONG now = GetTickCount64();
    if (!envi("GF_CREW_CAR", 1)) return 1;
    int k = -1, o = -1;
    for (int i = 0; i < 4; i++) { if (s_tog[i].sm && now - s_tog[i].t > 2000) s_tog[i].sm = 0; }
    for (int i = 0; i < 4; i++) if (s_tog[i].sm == sm) k = i;
    if (k < 0) for (int i = 0; i < 4; i++) if (!s_tog[i].sm) { s_tog[i].sm = sm; s_tog[i].seat = 0xFFFFFFFFu; k = i; break; }
    if (k < 0) return 1;
    s_tog[k].t = now;
    __try {
        const uint32_t seat = rd32(sm + 0x68);
        if (seat != s_tog[k].seat) { s_tog[k].seat = seat; printf("[crew] crew member in car seat %u (%s side)\n", seat, crew_right(seat) ? "right" : "left"); fflush(stdout); }
        for (int i = 0; i < 4; i++) if (i != k && s_tog[i].sm) { o = i; break; }
        if (o < 0) return 1;                               /* alone in the car: the game's own controls */
        const int role = crew_role(k, o);
        static gfn fveh, fpress; if (!fveh) { fveh = lookup(FN_GETVEHICLE); fpress = lookup(FN_PRESSED); }
        const uint32_t pl = player(); if (!pl || !fveh || !fpress) return 0;
        uint64_t v = 0; const uint64_t a1[1] = { pl };
        if (!gcall(fveh, a1, 1, 0.0, &v, NULL) || !v) return 0;
        const uint32_t h = rd32(sm + 0x58); if (!h || h - 0x38u != (uint32_t)v) return 0;
        if (rd32(s_tog[o].sm + 0x58) == h) crew_arrange(pl, sm, s_tog[o].sm);
        uint64_t p = 0; const uint64_t a2[2] = { pl, role == 1 ? 4u : 2u };
        if (gcall(fpress, a2, 2, 0.0, &p, NULL) && p) {
            const uint32_t bit = role == 1 ? 0x200u : 0x100u, f = rd32(sm + 0xFC);
            wr32(sm + 0xFC, (f & bit) ? (f & ~0x300u) : ((f & ~0x300u) | bit));
            printf("[crew] %s window: crew member %s\n", role == 1 ? "right (R)" : "left (Q)", (f & bit) ? "back in" : "leans out"); fflush(stdout);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return 0;
}
void gf_crew_input_done(uint64_t sm64, const uint32_t* before) { (void)sm64; (void)before; }

/* Crew teleport near the player's car (Player::GetCrewTeleportLocation, EU 0x1B9BBC) aims at GetVehicleEntryLocation
 * (EU 0x1B5C30) = the door of the first free front seat, so the second crew member landed at the front door and then
 * snapped to the rear left door. For him that point is now the rear left door (entry locator 12): car vfuncs
 * +0x1A8 / +0x1AC = first / next entry locator, +0xA4 = locator position (locator {u32 id, BoneInfo}). */
static uint32_t s_tp_npc;
/* When the player gets in a car the game counts the crew as out of range and teleports them next to it
 * (CombatNPC::HandleCrewTeleport). A crew member already close to the car (GF_CREW_WALK_DIST metres, default 25)
 * is left to walk to his door and open it; farther away he is still teleported. */
static int crew_pos(uint32_t e, float* p)
{
    const uint32_t t = rd32(e + 0x68); if (!ok(t)) return 0;
    p[0] = rdf(t + 0x40); p[1] = rdf(t + 0x44); p[2] = rdf(t + 0x48);
    return 1;
}
int gf_crew_tp_skip(uint64_t npc64)
{
    if (!envi("GF_CREW_CAR", 1)) return 0;
    __try {
        const uint32_t pl = player(), npc = (uint32_t)npc64;
        if (!pl || !ok(npc) || rd32(pl + 0x2A20) < 2 || crew_hire_order(pl, npc) > 1) return 0;
        const uint32_t car = crew_vehicle_of(pl); if (!ok(car)) return 0;
        float a[3], b[3];
        if (!crew_pos(npc, a) || !crew_pos(car, b)) return 0;
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2], d2 = dx * dx + dy * dy + dz * dz;
        const float lim = (float)envi("GF_CREW_WALK_DIST", 25);
        if (!(d2 < lim * lim)) return 0;
        static uint32_t s_last; static ULONGLONG s_t;
        const ULONGLONG now = GetTickCount64();
        if (npc != s_last || now - s_t > 5000) { s_last = npc; s_t = now;
            printf("[crew] crew member %.0f m from the car: walks to his door instead of teleporting\n", sqrtf(d2)); fflush(stdout); }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
void gf_crew_tp_begin(uint64_t npc) { s_tp_npc = (uint32_t)npc; }
void gf_crew_tp_end(void) { s_tp_npc = 0; }
void gf_crew_entry_location(uint64_t out64, uint64_t car64)
{
    if (!s_tp_npc || !envi("GF_CREW_CAR", 1)) return;
    __try {
        const uint32_t pl = player(), car = (uint32_t)car64;
        if (!pl || !ok(car) || rd32(pl + 0x2A20) < 2 || crew_hire_order(pl, s_tp_npc) != 1) return;
        static uint32_t buf; static gfn fnew;
        if (!buf) { if (!fnew) fnew = lookup(FN_NEW); uint64_t a = 16, r = 0; if (!fnew || !gcall(fnew, &a, 1, 0.0, &r, NULL) || !ok((uint32_t)r)) return; buf = (uint32_t)r; }
        uint64_t r = 0; { const uint64_t a[1] = { buf }; vcall(car, 0x1A8, a, 1, 0.0, &r, NULL); }
        for (int i = 0; r && i < 16; i++) {
            if (rd32(buf) == 0xC) {
                const uint64_t a[2] = { out64, buf + 4 };
                if (vcall(car, 0xA4, a, 2, 0.0, NULL, NULL)) { printf("[crew] second crew member brought to the rear left door\n"); fflush(stdout); }
                return;
            }
            const uint64_t a[2] = { buf, buf }; r = 0; vcall(car, 0x1AC, a, 2, 0.0, &r, NULL);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
