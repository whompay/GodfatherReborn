/* gf_coop_worldre.c -- v1060W1 world sync, population side (agent W1). See gf_coop_world.h / NOTES_world_sync.md.
 *
 * Every managed NPC / vehicle comes out of ONE factory: func_003388AC(classHash, attrib, owner, requester) (called only
 * by the ManagedEntity manager, func_003391F4 / func_0033DFD0; requests go in through func_0033EE8C & co). It knows 9
 * classes (alloc size, ctor):
 *   A5975EB2 0x9B0  func_002D2B30  vehicle (base)        10E5319E 0xBF0  func_002A6DEC  vehicle (civilian car)
 *   FE169CA3 0xC70  func_002B9CB0  police vehicle
 *   BDA2CAC1/D1812E99/F48F270E/BA1F9E7B 0x1F80 func_0012A40C  combat NPC (crew / gangster / ...)
 *   A37F4AB4 0x1F90 func_0012B184  NPC (cop?)            3E8BF7B3/5DA5581E 0x1CC0 func_0014445C  pedestrian
 * Hand edits (_patches/apply-pending.ps1 block "v1060W1"):
 *   ppu_recomp_001.cpp: func_003388AC wrapper -> gf_world_factory_pre / gf_world_factory_post (entity = return r3)
 *   ppu_recomp_000.cpp: func_00027D3C (operator delete: allocator vt+4 free) -> gf_world_free_note(ptr)
 *   ppu_recomp_002.cpp: func_0046316C wrapper (after gf_coop_after_anim) -> gf_world_anim_seen(ent)
 * World matrix of an entity: xf = *(ent+0x68), RwMatrix at xf+0x10 (rows right/up/at/pos, 3 floats + pad each).
 *
 * Env: GF_COOP_WORLD=0 off. GF_COOP_WORLD_LOG=1 per-spawn log lines (default: first 64). GF_COOP_WORLD_SUPPRESS=
 *   both (default since v1061AC: refuse ambient spawns in the factory + release what exists) | release (only despawn
 *   with the game's own release func_00455F58: the traffic system respawns ~2 cars/s) | factory (only refuse) | off.
 * v1061A (agent WA): ambient = spawned by the population requester (requester vtable 0x1004B5E8, traffic AND sidewalk
 *   peds BA1F9E7B, ambient police cars; GF_COOP_WORLD_AMBIENT_VT=hex,hex overrides) or a pedestrian class. Our own
 *   spawns (owner = requester = 0: co-op puppet, W3 proxies) are never enumerated nor released. Game release
 *   (func_00455F58 clears bit 0 of ent+0x34) -> entry dropped from the enumeration. Delete hook on all 4 operator
 *   delete variants (27D3C 27E74 27F30 27FEC). GF_COOP_WORLD_SUPPRESS_TEST=<s>: echo test, suppression ON from <s> s
 *   after the first spawn (W2 never suppresses in echo), GF_COOP_WORLD_SUPPRESS_TEST_LEN=<s> (default 60) then off.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "recompiled/ppu_recomp.h"
#include "gf_coop_world.h"
#include "gf_coop_remote.h"   /* v1126M gf_remote / gf_world_remotes */

extern uint8_t* vm_base;
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern uint32_t gf_coop_puppet_entity(void);
extern int gf_coop_is_puppet(uint32_t ent);   /* v1072W (S request): every peer's puppet (gf_coop_peers.c), not only the primary */
static int is_pup(uint32_t ent, uint32_t pup) { int r = 0; if (!ent) return 0; if (ent == pup) return 1; __try { r = gf_coop_is_puppet(ent); } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; } return r != 0; }

/* W3 provides gf_world_is_proxy; until it links, fall back to "nothing is a proxy" */
int gf_world_is_proxy_w1default(uint32_t ent) { (void)ent; return 0; }
#pragma comment(linker, "/alternatename:gf_world_is_proxy=gf_world_is_proxy_w1default")

enum { GAME_TOC = 0x00B4BF60u, W_MAX = 512, RQ_MAX = 32 };
typedef void (*gfn)(ppu_context*);

typedef struct {
    uint32_t ent, vt, hash, attrib, owner, req, req_vt;
    uint8_t  kind, guid_state;      /* 0 = unresolved, 1 = ok, 2 = failed */
    uint8_t  guid[16];
    uint8_t  released;              /* 1 = released by us */
    uint8_t  ours, alive_seen, gone; /* ours: owner = requester = 0 (puppet / proxies); gone: game released it */
    ULONGLONG born, anim_ms, pos_ms;
    float    pos[3], vel[3];
    float    prow[9], avel[3]; ULONGLONG rot_ms;   /* v1063H angular velocity (finite difference of the basis) */
    uint32_t gen;                                   /* v1063H spawn serial (address reuse = new identity) */
    uint8_t  msrc;                                  /* v1126M: migrated in (promoted proxy / re-promoted native) = a local SOURCE: enumerated
                                                     * even when 'ours' (factory proxy) or non-ambient, counts as ambient for release / info */
} wrec;

static SRWLOCK s_lock = SRWLOCK_INIT;
static wrec s_rec[W_MAX];
static int s_n;
static volatile uint32_t s_min = 0xFFFFFFFFu, s_max = 0;
static int s_on = -1, s_log = 0, s_logged = 0;
static volatile LONG s_suppress = 0;
static int s_sup_mode = 1;   /* 1 release, 2 factory, 3 both, 0 off.
 * v1067R default 3 -> 1 (release only). ROOT CAUSE of the "ghost town" when the joiner walks away from the host: a factory
 * refusal (func_003388AC returns 0) is NOT a failure for the ManagedEntity manager -- both call sites (func_003391F4
 * 0x003392E0, func_0033DFD0 0x0033E5DC) still link a 0x10 node {.., ent = 0, .., req} into the requester's list and mark the
 * request done (+0x8 = 2). Every refused ambient spawn became a permanent phantom "live" entry of the population budget
 * (requester vt 1004B5E8): user's 17:3x joiner log "refused 51" at join, then "ambient spawns 0" for the rest of the session,
 * also after "guest FAR from the host -> ambient spawn refusal off". Release (func_00455F58) frees the slot properly
 * (respawn churn ~2/s near the host, wa-13) -> GF_COOP_WORLD_SUPPRESS=both/factory only for experiments. */
static volatile LONG s_spawned = 0, s_freed = 0, s_refused = 0, s_released_n = 0, s_overflow = 0, s_stale = 0;
static struct { uint32_t vt; uint32_t hash; LONG n; } s_rq[RQ_MAX];   /* requester vtable stats */
/* v1061A diagnostics + ambient classification */
static volatile LONG s_anim_calls = 0, s_anim_inrange = 0, s_anim_hit = 0, s_gone_n = 0, s_amb_spawned = 0, s_keep_drv = 0;
static int s_anim_dn = 0; static int32_t s_anim_d[8];   /* unmatched in-range anim ents: delta to the next tracked ent below */
/* v1061D: synced population = ambient traffic/peds (1004B5E8) + the street mob/crew population (1004F270, 10049F90).
 * coop-local 2026-11-05: the host synced EVERY NPC but the guest only suppressed 1004B5E8, so the street mob existed
 * twice on the guest (its own + the host's proxies). Now the host sends exactly the set the guest suppresses; NPCs
 * outside it (requester 0 mission NPCs, 1004EFD0) stay local on both sides. GF_COOP_WORLD_SYNC_ALL=1 = old behaviour. */
static uint32_t s_amb_vt[8] = { 0x1004B5E8u, 0x1004F270u, 0x10049F90u }; static int s_amb_vt_n = 3; static int s_sync_all = 0;
static int s_sup_test = 0, s_sup_test_len = 60; static ULONGLONG s_first_spawn = 0;
/* v1063H (agent H): coverage. The host now syncs EVERY managed ped/car (not ours / player / puppet / proxies); the guest
 * still suppresses only the ambient set (is_ambient) and PAIRS its other natives (mission / crew / police from other
 * requesters) with the host's entity of the same class (gf_world_native_match -> gf_proxy_adopt). GF_COOP_WORLD_SYNC=ambient
 * (or GF_COOP_WORLD_SYNC_ALL=0) = v1061D behaviour. Spatial suppression: the guest refuses ambient spawns only while it is
 * within s_sup_near_m of the host and releases only natives within s_sup_rel_m of the host (players far apart -> the guest
 * keeps its own street life where the host has no population to send). */
static float s_hostpos[3]; static volatile LONG s_hostpos_ok = 0, s_sup_near = 1;
static float s_sup_near_m = 150.0f, s_sup_rel_m = 130.0f; static int s_spatial = 1;
/* v1072Z (Claude): FAR threshold split from the release radius. User: "the world has to work like single player when the players
 * aren't close" -- between 60 and 130 m from the host the joiner had spawn capacity 0 while the host's population only fills around
 * the host -> ghost town. s_sup_rel_m (130) still decides which of the joiner's natives near the HOST are released (no doubles when
 * close); s_far_m (60) decides when the joiner counts as far: own spawn capacity back + v1071D zone split (midline ownership) on
 * both sides. GF_COOP_ZONE_FAR_R (must match on all instances; 130 = old behaviour). (First try z72-1 lowered the release radius
 * itself -> echo z72-2 tracked 24 / 86 fps: doubles while close. Reverted.) */
static float s_far_m = -1.0f;
float gf_world_far_radius(void) { if (s_far_m < 0) { const char* e = getenv("GF_COOP_ZONE_FAR_R"); s_far_m = (e && atof(e) > 10.0) ? (float)atof(e) : 60.0f;
    printf("[coop-worldre] v1072Z far threshold %.0f m: beyond it a joiner spawns its own street life and the street splits at the midline (GF_COOP_ZONE_FAR_R)\n", (double)s_far_m); fflush(stdout); } return s_far_m; }
static volatile LONG s_n_kept_far = 0, s_n_match = 0;
static volatile LONG s_keep_owned = 0;   /* v1068R release skipped: native owned by us (zone spawn authority) */
extern int gf_coop_world_src_has(uint32_t ent);   /* v1068R gf_coop_world.c */
static int s_dup_log = 1;   /* v1064I GF_COOP_WORLD_DUPLOG=0: no guest-native duplicate census in the 10 s status */

static int ea_ok(uint32_t ea, uint32_t n) { return vm_base && ea >= 0x10000u && ea < 0xF0000000u && n <= 0xF0000000u - ea; }
static uint32_t rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static float rdf(uint32_t ea) { uint32_t u = rd32(ea); float f; memcpy(&f, &u, 4); return f; }
static int safe32(uint32_t ea, uint32_t* out)
{
    if (!ea_ok(ea, 4)) return 0;
    __try { *out = rd32(ea); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* v1068Z (agent Z): func_004530A8(attrib) walks records p = attrib+0x14 while (*(p+4) != 0x40000000) p += *p (when byte +0x10
 * bit 0 is clear). An attrib whose block was freed / zeroed / reused (area change) makes that loop spin FOREVER on the main thread:
 * t68-6-echo FLIP_STALL ~165 s after "entity gone (area change)" -- every [HANG] RIP symbolized (build-perf pdb) to
 * func_004530A8 ppu_recomp_002.cpp:63433-63435 (+ vm_read32). Pre-walk it here, bounded and with checked reads; 0 = the game's
 * walk would not terminate -> the caller treats the attrib as invalid. GF_COOP_ATTRIB_GUARD=0 = old (no pre-walk). */
static int attrib_walk_ok(uint32_t at)
{
    static int s_ag = -1; uint32_t w = 0, p, step = 0, tag = 0; int i;
    if (s_ag < 0) { const char* e = getenv("GF_COOP_ATTRIB_GUARD"); s_ag = !(e && *e == '0');
        printf("[coop-attrib] v1068Z %s: template attrib walk (func_004530A8) pre-checked %s (GF_COOP_ATTRIB_GUARD)\n", __FILE__, s_ag ? "ON" : "off"); fflush(stdout); }
    if (!s_ag) return 1;
    if (!safe32(at + 0x10u, &w)) return 0;
    if ((w >> 24) & 1u) return 1;   /* byte +0x10 bit 0 set: the game takes the other (non-looping) branch */
    p = at + 0x14u;
    for (i = 0; i < 16384; i++) {
        if (!safe32(p + 4u, &tag)) return 0;
        if (tag == 0x40000000u) return 1;
        if (!safe32(p, &step) || !step) { static int nl; if (nl++ < 10) { printf("[coop-attrib] v1068Z attrib %08X: record walk stuck at %08X (step 0) -> attrib rejected (would hang func_004530A8)\n", at, p); fflush(stdout); } return 0; }
        p += step;   /* 32-bit wrap like the game's rldicl */
    }
    { static int nl; if (nl++ < 10) { printf("[coop-attrib] v1068Z attrib %08X: record walk did not end after %d steps -> attrib rejected\n", at, i); fflush(stdout); } }
    return 0;
}
static ULONGLONG now_ms(void) { return GetTickCount64(); }
static int on(void)
{
    if (s_on < 0) {
        const char* e = getenv("GF_COOP_WORLD"); const char* m = getenv("GF_COOP"); const char* l = getenv("GF_COOP_WORLD_LOG");
        const char* sm = getenv("GF_COOP_WORLD_SUPPRESS");
        s_on = !(m && (!strcmp(m, "0") || !strcmp(m, "off"))) && !(e && !strcmp(e, "0"));   /* v1119M: track population in every game
         * (co-op can start from the pause menu at any time; the host's world sync needs the peds that already exist). Ambient
         * suppression stays a joiner-only, connected-only action (gf_world_suppress_ambient). GF_COOP=off / GF_COOP_WORLD=0 = off. */
        s_log = l && *l && *l != '0';
        if (sm && !strcmp(sm, "factory")) s_sup_mode = 2; else if (sm && !strcmp(sm, "both")) s_sup_mode = 3;
        else if (sm && !strcmp(sm, "release")) s_sup_mode = 1;
        else if (sm && (!strcmp(sm, "off") || !strcmp(sm, "0"))) s_sup_mode = 0;
        { const char* sa = getenv("GF_COOP_WORLD_SYNC_ALL"); const char* sy = getenv("GF_COOP_WORLD_SYNC");
          s_sync_all = 1;   /* v1063H default: everything */
          if (sa && *sa) s_sync_all = *sa != '0';
          if (sy && !strcmp(sy, "ambient")) s_sync_all = 0; else if (sy && !strcmp(sy, "all")) s_sync_all = 1; }
        { const char* e2 = getenv("GF_COOP_WORLD_SPATIAL"); if (e2 && *e2 == '0') s_spatial = 0;
          if ((e2 = getenv("GF_COOP_WORLD_SUP_NEAR")) && atof(e2) > 0) s_sup_near_m = (float)atof(e2);
          if ((e2 = getenv("GF_COOP_WORLD_R")) && atof(e2) > 1.0) s_sup_rel_m = (float)atof(e2) + 10.0f;
          if ((e2 = getenv("GF_COOP_WORLD_SUP_REL")) && atof(e2) > 0) s_sup_rel_m = (float)atof(e2);
          if ((e2 = getenv("GF_COOP_WORLD_DUPLOG")) && *e2 == '0') s_dup_log = 0; }   /* v1064I */
        { const char* av = getenv("GF_COOP_WORLD_AMBIENT_VT");
          if (av && *av) { char* q = (char*)av; s_amb_vt_n = 0;
              while (*q && s_amb_vt_n < 8) { char* q0 = q; const unsigned long v = strtoul(q, &q, 16); if (q == q0) break;
                  if (v) s_amb_vt[s_amb_vt_n++] = (uint32_t)v; while (*q == ',' || *q == ' ') q++; } } }
        { const char* st = getenv("GF_COOP_WORLD_SUPPRESS_TEST"); const char* sl = getenv("GF_COOP_WORLD_SUPPRESS_TEST_LEN");
          if (st && atoi(st) > 0) s_sup_test = atoi(st);
          if (sl && atoi(sl) > 0) s_sup_test_len = atoi(sl); }
        if (s_on) { printf("[coop-worldre] v1060W1 population tracking ON (factory func_003388AC + delete hook), suppress mode %s\n",
                           s_sup_mode == 3 ? "both" : s_sup_mode == 2 ? "factory" : s_sup_mode == 1 ? "release" : "off");
                   printf("[coop-worldre] v1061A ambient = requester vt %08X (+%d more) or pedestrian class; own spawns (owner = requester = 0) skipped%s\n",
                          s_amb_vt[0], s_amb_vt_n - 1, s_sup_test ? " | SUPPRESS TEST armed" : "");
                   printf("[coop-worldre] v1067R ambient suppression = %s (factory refusal leaves phantom population entries -> ghost town; GF_COOP_WORLD_SUPPRESS)\n", s_sup_mode == 1 ? "release only" : s_sup_mode == 0 ? "off" : "WITH FACTORY REFUSAL");
                   printf("[coop-worldre] v1063H host syncs %s | guest: non-ambient natives paired/adopted, spatial suppression %s (refuse < %.0f m from host, release < %.0f m)\n",
                          s_sync_all ? "ALL managed peds/cars" : "the ambient set only (GF_COOP_WORLD_SYNC=ambient)", s_spatial ? "ON" : "off",
                          (double)s_sup_near_m, (double)s_sup_rel_m);
                   printf("[coop-worldre] v1064I guest-native duplicate census %s (GF_COOP_WORLD_DUPLOG)\n", s_dup_log ? "ON" : "off");
                   fflush(stdout); }
    }
    return s_on;
}
static uint8_t kind_of(uint32_t h)
{
    if (h == 0xA5975EB2u || h == 0x10E5319Eu || h == 0xFE169CA3u) return GF_WK_CAR;
    return GF_WK_PED;
}
/* ambient = what the guest must not spawn on its own. v1061A (w1-2-echo / w2-2-echo requester table): the population
 * requester (vt 1004B5E8) spawns traffic 10E5319E, ambient police cars FE169CA3 AND the sidewalk peds BA1F9E7B; crew /
 * mission NPCs come from other requesters (1004F270 crew BDA2CAC1, 1004EFD0 F48F270E) or with requester 0 and an owner
 * id (A37F4AB4). Own spawns (owner = requester = 0) are never ambient. */
static int is_amb_vt(uint32_t vt) { for (int i = 0; i < s_amb_vt_n; i++) if (vt && vt == s_amb_vt[i]) return 1; return 0; }
static int is_ambient(uint32_t h, uint32_t owner, uint32_t req, uint32_t req_vt)
{
    if (!owner && !req) return 0;
    if (is_amb_vt(req_vt)) return 1;
    return req && (h == 0x3E8BF7B3u || h == 0x5DA5581Eu);
}
static uint32_t player(void)
{
    uint32_t a;
    if (!safe32(0x10181290u, &a) || !ea_ok(a, 8)) return 0;
    if (!safe32(a + 4, &a) || !ea_ok(a, 4)) return 0;
    if (!safe32(a, &a) || a < 0x17FCu) return 0;
    a -= 0x17FCu;
    return ea_ok(a, 4) ? a : 0;
}

/* ---------------------------------------------------------------- guest calls (main PPU thread only) */
static gfn lookup(uint32_t addr)
{
    static uint32_t ca[8]; static gfn cf[8]; static int cn;
    uint64_t i; gfn f = 0;
    for (i = 0; i < (uint64_t)cn; i++) if (ca[i] == addr) return cf[i];
    for (i = 0; i < function_table_count; i++)
        if ((uint32_t)function_table[i].addr == addr) { f = function_table[i].func; break; }
    if (cn < 8) { ca[cn] = addr; cf[cn] = f; cn++; }
    return f;
}
static int s_gfail;
static uint64_t gcall(uint32_t addr, uint64_t a0, uint64_t a1)
{
    ppu_context* cur = g_active_ctx; static ppu_context c; gfn f;
    if (!cur || !(f = lookup(addr))) return 0;
    __try {
        memcpy(&c, cur, sizeof c);
        c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1000u) & ~0xFull;
        c.gpr[2] = GAME_TOC; c.gpr[3] = a0; c.gpr[4] = a1; c.gpr[5] = 0; c.gpr[6] = 0; c.lr = 0;
        g_active_ctx = &c;
        f(&c);
        while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
        g_active_ctx = cur;
        return c.gpr[3];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_active_ctx = cur; g_trampoline_fn = 0; s_gfail++;
        printf("[coop-worldre] guest call %08X faulted\n", addr); fflush(stdout); return 0;
    }
}

/* ---------------------------------------------------------------- hooks (any PPU thread) */
static void rq_note(uint32_t vt, uint32_t h)
{
    for (int i = 0; i < RQ_MAX; i++) {
        if (s_rq[i].vt == vt && s_rq[i].hash == h) { InterlockedIncrement(&s_rq[i].n); return; }
        if (!s_rq[i].vt && !s_rq[i].hash) { s_rq[i].vt = vt; s_rq[i].hash = h; s_rq[i].n = 1; return; }
    }
}
/* v1108W (user: "cars don't collide with the dead bodies"): an ALWAYS-ON list of every car the factory makes (the tracking
 * below only runs in co-op), for gf_ragdoll.c: dead bodies get the nearest moving car (traffic too, not only the player's) as
 * a solid in their physics world. Entries die with operator delete / a changed vtable. gf_world_cars() = live ones. */
#define CL_MAX 128
static uint32_t s_cl[CL_MAX], s_cl_vt[CL_MAX]; static int s_cl_n; static volatile LONG s_cl_lock; static uint32_t s_cl_lo = 0xFFFFFFFFu, s_cl_hi = 0;
static void cl_lock(void) { while (InterlockedExchange(&s_cl_lock, 1)) Sleep(0); }
static void cl_unlock(void) { InterlockedExchange(&s_cl_lock, 0); }
static void cl_add(uint32_t ent, uint32_t vt)
{
    cl_lock();
    int i; for (i = 0; i < s_cl_n; i++) if (s_cl[i] == ent) break;
    if (i == s_cl_n) { if (s_cl_n < CL_MAX) s_cl_n++; else i = (int)(GetTickCount64() % CL_MAX); }   /* full: overwrite one */
    s_cl[i] = ent; s_cl_vt[i] = vt;
    if (ent < s_cl_lo) s_cl_lo = ent; if (ent > s_cl_hi) s_cl_hi = ent;
    cl_unlock();
}
static void cl_del(uint32_t ptr)
{
    { int hit = 0; const int nn = s_cl_n; for (int i = 0; i < nn && i < CL_MAX; i++) if (s_cl[i] == ptr) { hit = 1; break; } if (!hit) return; }   /* hot path: every delete */
    cl_lock();
    for (int i = 0; i < s_cl_n; i++) if (s_cl[i] == ptr) { s_cl[i] = s_cl[s_cl_n - 1]; s_cl_vt[i] = s_cl_vt[s_cl_n - 1]; s_cl_n--; break; }
    cl_unlock();
}
/* v1109Y: 1 = ent is a live car from the factory (any mode, solo too) -- gf_coop_npcfix.c gives cars a longer in-view keep range */
int gf_world_is_car(uint32_t ent)
{
    if (!ent || ent < s_cl_lo || ent > s_cl_hi) return 0;
    int hit = 0; uint32_t want = 0;
    cl_lock();
    for (int i = 0; i < s_cl_n; i++) if (s_cl[i] == ent) { hit = 1; want = s_cl_vt[i]; break; }
    cl_unlock();
    if (!hit) return 0;
    { uint32_t vt = 0; if (!safe32(ent, &vt) || vt != want) return 0; }
    return 1;
}
/* v1110T (gf_shop.c shopkeeper): ALWAYS-ON spawn info of every NPC the factory makes (the tracking above only runs in co-op):
 * class hash, owner, requester vtable (0x1004B5E8 = the street population). Entries are trusted while the vtable is unchanged. */
#define NR_MAX 512
static struct { uint32_t ent, vt, hash, owner, req_vt; } s_nr[NR_MAX]; static unsigned s_nr_i;
void gf_world_npc_note(uint32_t ent, uint32_t hash, uint32_t owner, uint32_t req)
{
    uint32_t vt = 0, rv = 0; if (!safe32(ent, &vt)) return; if (req) safe32(req, &rv);
    int k = -1; for (int i = 0; i < NR_MAX; i++) if (s_nr[i].ent == ent) { k = i; break; }
    if (k < 0) k = (int)(s_nr_i++ % NR_MAX);
    s_nr[k].ent = ent; s_nr[k].vt = vt; s_nr[k].hash = hash; s_nr[k].owner = owner; s_nr[k].req_vt = rv;
}
int gf_world_npcs(uint32_t* out, int max)   /* gf_crewdrive.c: the living-or-not NPCs the factory made (cheap, no memory scan) */
{
    int n = 0;
    for (int i = 0; i < NR_MAX && n < max; i++) { uint32_t vt = 0; if (s_nr[i].ent && safe32(s_nr[i].ent, &vt) && vt == s_nr[i].vt) out[n++] = s_nr[i].ent; }
    return n;
}
int gf_world_spawn_info(uint32_t ent, uint32_t* hash, uint32_t* owner, uint32_t* req_vt)   /* 1 = known */
{
    for (int i = 0; i < NR_MAX; i++) if (s_nr[i].ent == ent && ent) {
        uint32_t vt = 0; if (!safe32(ent, &vt) || vt != s_nr[i].vt) return 0;
        if (hash) *hash = s_nr[i].hash; if (owner) *owner = s_nr[i].owner; if (req_vt) *req_vt = s_nr[i].req_vt; return 1; }
    return 0;
}
int gf_world_cars(uint32_t* out, int max)
{
    int n = 0;
    cl_lock();
    for (int i = 0; i < s_cl_n; i++) {
        uint32_t vt = 0;
        if (!safe32(s_cl[i], &vt) || vt != s_cl_vt[i]) { s_cl[i] = s_cl[s_cl_n - 1]; s_cl_vt[i] = s_cl_vt[s_cl_n - 1]; s_cl_n--; i--; continue; }
        if (n < max) out[n++] = s_cl[i];
    }
    cl_unlock();
    return n;
}
int gf_world_factory_pre(uint32_t hash, uint32_t attrib, uint32_t owner, uint32_t req)
{
    uint32_t rvt = 0;
    (void)attrib;
    if (owner && !req) { extern int gf_shop_refuse_spawn(uint32_t hash, uint32_t owner); if (gf_shop_refuse_spawn(hash, owner)) return 1; }   /* v1110X */
    if (!on() || !s_suppress || s_sup_mode < 2) return 0;
    if (!owner && !req) return 0;   /* our own spawns (W3 proxies, the co-op puppet) pass owner = requester = 0 */
    if (req) safe32(req, &rvt);
    if (!is_ambient(hash, owner, req, rvt)) return 0;
    if (s_spatial && s_hostpos_ok && !s_sup_near) { InterlockedIncrement(&s_n_kept_far); return 0; }   /* v1063H far from the host: own street life */
    InterlockedIncrement(&s_refused);
    return 1;
}
void gf_world_factory_post(uint32_t hash, uint32_t attrib, uint32_t owner, uint32_t req, uint32_t ent)
{
    uint32_t vt = 0, rvt = 0;
    if (ent && kind_of(hash) == GF_WK_CAR) { uint32_t cvt = 0; if (safe32(ent, &cvt)) cl_add(ent, cvt); }   /* v1108W */
    else if (ent) { extern void gf_world_npc_note(uint32_t ent, uint32_t hash, uint32_t owner, uint32_t req); gf_world_npc_note(ent, hash, owner, req); }   /* v1110T */
    if (!on() || !ent) return;
    safe32(ent, &vt); if (req) safe32(req, &rvt);
    InterlockedIncrement(&s_spawned);
    AcquireSRWLockExclusive(&s_lock);
    rq_note(rvt, hash);
    {
        int i;
        for (i = 0; i < s_n; i++) if (s_rec[i].ent == ent) break;   /* same address reused without a delete we saw */
        if (i == s_n) { if (s_n < W_MAX) s_n++; else { InterlockedIncrement(&s_overflow); i = -1; } }
        if (i >= 0) {
            wrec* r = &s_rec[i]; memset(r, 0, sizeof *r);
            r->ent = ent; r->vt = vt; r->hash = hash; r->attrib = attrib; r->owner = owner; r->req = req; r->req_vt = rvt;
            r->kind = kind_of(hash); r->born = now_ms(); r->ours = !owner && !req;
            { static LONG s_gen = 0; r->gen = (uint32_t)InterlockedIncrement(&s_gen); }
            if (!s_first_spawn) s_first_spawn = r->born;
            if (is_ambient(hash, owner, req, rvt)) InterlockedIncrement(&s_amb_spawned);
            if (ent < s_min) s_min = ent;
            if (ent > s_max) s_max = ent;
        }
    }
    ReleaseSRWLockExclusive(&s_lock);
    if (s_log || s_logged < 64) { s_logged++;
        printf("[coop-worldre] spawn class %08X attrib %08X owner %08X requester %08X (vt %08X) -> ent %08X vt %08X %s\n",
               hash, attrib, owner, req, rvt, ent, vt, kind_of(hash) == GF_WK_CAR ? "car" : "ped"); fflush(stdout); }
}
void gf_world_free_note(uint32_t ptr)
{
    if (ptr >= s_cl_lo && ptr <= s_cl_hi) { extern void gf_pop_car_freed(uint32_t ent); int was = 0;   /* v1109Y diagnostic */
        { const int nn = s_cl_n; for (int i = 0; i < nn && i < CL_MAX; i++) if (s_cl[i] == ptr) { was = 1; break; } }
        if (was) gf_pop_car_freed(ptr);
        cl_del(ptr); }   /* v1108W */
    if (ptr < s_min || ptr > s_max || s_on != 1) return;   /* fast path: not one of ours */
    AcquireSRWLockExclusive(&s_lock);
    for (int i = 0; i < s_n; i++) if (s_rec[i].ent == ptr) { s_rec[i] = s_rec[--s_n]; InterlockedIncrement(&s_freed); break; }
    ReleaseSRWLockExclusive(&s_lock);
}
void gf_world_status_tick(void);
/* v1061A echo suppression test: ON at +T s after the first spawn for LEN s, re-asserted every 2 s. Driven from
 * gf_world_enum (W2, 15 Hz) -- v1061AB: func_0046316C only runs in menus / scripted scenes, not per NPC in free roam. */
static void sup_test_tick(void)
{
    static ULONGLONG s_tnext = 0; ULONGLONG t, a, b;
    if (!s_sup_test || !s_first_spawn) return;
    t = now_ms(); a = s_first_spawn + (ULONGLONG)s_sup_test * 1000u; b = a + (ULONGLONG)s_sup_test_len * 1000u;
    if (t >= a && t < b && t >= s_tnext) { s_tnext = t + 2000; __try { gf_world_suppress_ambient(1); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
    else if (t >= b && s_suppress) { __try { gf_world_suppress_ambient(0); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
}
void gf_world_anim_seen(uint32_t ent)   /* main PPU thread, after every func_0046316C */
{
    static ULONGLONG s_chk = 0;
    if (s_on < 0) on();
    if (s_on == 1) { const ULONGLONG t = now_ms(); if (t >= s_chk) { s_chk = t + 1000; __try { gf_world_status_tick(); } __except (EXCEPTION_EXECUTE_HANDLER) { }
        } }
    InterlockedIncrement(&s_anim_calls);
    if (ent < s_min || ent > s_max || s_on != 1) return;
    InterlockedIncrement(&s_anim_inrange);
    {
        int hit = 0; uint32_t below = 0;
        AcquireSRWLockShared(&s_lock);
        for (int i = 0; i < s_n; i++) { if (s_rec[i].ent == ent) { s_rec[i].anim_ms = now_ms(); hit = 1; break; }
            if (s_rec[i].ent < ent && s_rec[i].ent > below) below = s_rec[i].ent; }
        ReleaseSRWLockShared(&s_lock);
        if (hit) InterlockedIncrement(&s_anim_hit);
        else if (below && s_anim_dn < 8) { s_anim_d[s_anim_dn++] = (int32_t)(ent - below); }   /* main thread only */
    }
}

/* ---------------------------------------------------------------- main thread API */
/* v1061A game-side liveness: the release func_00455F58(ent + 0x30) clears bit 0 of *(ent + 0x34) and queues the entity
 * for destruction. Only trusted after the bit was seen set on this entity. Returns 0 = released by the game. */
static int game_alive(wrec* r)
{
    uint32_t f;
    if (r->gone) return 0;
    if (!safe32(r->ent + 0x34u, &f)) return 1;
    if (f & 1u) { r->alive_seen = 1; return 1; }
    if (r->alive_seen) { r->gone = 1; InterlockedIncrement(&s_gone_n); return 0; }
    return 1;
}
static int read_world(uint32_t ent, float m[12])
{
    uint32_t xf;
    if (!safe32(ent + 0x68u, &xf) || !ea_ok(xf, 0x50u)) return 0;
    __try {
        for (int r = 0; r < 4; r++) for (int k = 0; k < 3; k++) m[r * 3 + k] = rdf(xf + 0x10u + (uint32_t)r * 16u + (uint32_t)k * 4u);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    for (int i = 0; i < 12; i++) if (!isfinite(m[i]) || fabsf(m[i]) > 100000.0f) return 0;
    if (fabsf(m[9]) + fabsf(m[10]) + fabsf(m[11]) < 0.01f) return 0;   /* not placed yet */
    return 1;
}
static void resolve_guids(void)   /* func_004530A8(attrib) -> GUID pointer (as gf_coop_puppet.c). NO lock held: guest code
                                  * may delete / spawn entities, and those hooks take s_lock. */
{
    uint32_t ents[8], ats[8], gps[8]; int k = 0;
    AcquireSRWLockShared(&s_lock);
    for (int i = 0; i < s_n && k < 8; i++)
        if (!s_rec[i].guid_state && s_rec[i].attrib) { ents[k] = s_rec[i].ent; ats[k] = s_rec[i].attrib; k++; }
    ReleaseSRWLockShared(&s_lock);
    for (int j = 0; j < k; j++) gps[j] = attrib_walk_ok(ats[j]) ? (uint32_t)gcall(0x004530A8u, ats[j], 0) : 0;   /* v1068Z: 0 -> guid_state 2 (no GUID) */
    AcquireSRWLockExclusive(&s_lock);
    for (int j = 0; j < k; j++) for (int i = 0; i < s_n; i++) {
        wrec* r = &s_rec[i]; int nz = 0;
        if (r->ent != ents[j] || r->attrib != ats[j] || r->guid_state) continue;
        r->guid_state = 2;
        if (ea_ok(gps[j], 16)) {
            __try { memcpy(r->guid, vm_base + gps[j], 16); } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            for (int b = 0; b < 16; b++) nz |= r->guid[b];
            if (nz) r->guid_state = 1;
        }
        break;
    }
    ReleaseSRWLockExclusive(&s_lock);
}
void gf_world_status_tick(void);
static int s_in_status = 0;
int gf_world_enum2(gf_world_ent* out, int max, int ambient_only);
int gf_world_enum(gf_world_ent* out, int max) { return gf_world_enum2(out, max, 0); }
float gf_world_release_radius(void) { on(); return s_sup_rel_m; }   /* v1068R: zone radius of the joiner's spawn authority */
int gf_world_enum2(gf_world_ent* out, int max, int ambient_only)   /* v1068R ambient_only: the joiner sources only its street life */
{
    const uint32_t pl = player(), pup = gf_coop_puppet_entity();
    const ULONGLONG t = now_ms();
    int n = 0;
    if (!on() || !out || max <= 0) return 0;
    if (!s_in_status) { sup_test_tick(); gf_world_status_tick(); }
    resolve_guids();
    AcquireSRWLockExclusive(&s_lock);   /* main thread; the delete hook may run on other threads */
    for (int i = 0; i < s_n && n < max; i++) {
        wrec* r = &s_rec[i]; uint32_t vt; float m[12];
        if (r->released || (r->ours && !r->msrc) || r->ent == pl || is_pup(r->ent, pup) || (!r->msrc && gf_world_is_proxy(r->ent))) continue;   /* v1126M msrc */
        if (!r->msrc && (ambient_only || !s_sync_all) && !is_ambient(r->hash, r->owner, r->req, r->req_vt)) continue;   /* v1068R ambient_only; v1061D: only what the guest suppresses (v1063H: default ALL) */
        if (!safe32(r->ent, &vt) || vt != r->vt) { s_rec[i] = s_rec[--s_n]; i--; InterlockedIncrement(&s_stale); continue; }
        if (!game_alive(r)) continue;
        /* v1061AB: no anim-age filter -- func_0046316C is not the per-NPC free-roam anim tick (wa-9: 66 hits, then the hook
         * stopped for the rest of the run), so a stale anim_ms dropped live peds. Liveness = vtable + game_alive(). */
        if (!read_world(r->ent, m)) continue;
        if (r->pos_ms && t > r->pos_ms) {
            const float dt = (float)(t - r->pos_ms) * 0.001f;
            if (dt > 0.02f && dt < 1.0f) for (int k = 0; k < 3; k++) {
                const float v = (m[9 + k] - r->pos[k]) / dt;
                r->vel[k] = isfinite(v) && fabsf(v) < 80.0f ? r->vel[k] * 0.5f + v * 0.5f : 0.0f; }
            else if (dt >= 1.0f) r->vel[0] = r->vel[1] = r->vel[2] = 0.0f;
        }
        if (!r->pos_ms || t - r->pos_ms >= 20) { r->pos[0] = m[9]; r->pos[1] = m[10]; r->pos[2] = m[11]; r->pos_ms = t; }
        if (r->rot_ms && t > r->rot_ms && t - r->rot_ms >= 20) {   /* v1063H: omega from dR = R_now * R_prev^T (columns = rows here) */
            const float dt = (float)(t - r->rot_ms) * 0.001f;
            if (dt < 1.0f) {
                /* basis vectors b_i (rows of m). R = [b0 b1 b2] as columns; dR = sum_i b_i(now) b_i(prev)^T.
                 * skew part W = (dR - dR^T)/2 -> omega = (W21, W02, W10) / dt (small angle) */
                float d[3][3]; for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) { d[a][b] = 0;
                    for (int i = 0; i < 3; i++) d[a][b] += m[i * 3 + a] * r->prow[i * 3 + b]; }
                float w[3] = { 0.5f * (d[2][1] - d[1][2]) / dt, 0.5f * (d[0][2] - d[2][0]) / dt, 0.5f * (d[1][0] - d[0][1]) / dt };
                for (int k = 0; k < 3; k++) r->avel[k] = isfinite(w[k]) && fabsf(w[k]) < 30.0f ? r->avel[k] * 0.5f + w[k] * 0.5f : 0.0f;
            } else r->avel[0] = r->avel[1] = r->avel[2] = 0.0f;
        }
        if (!r->rot_ms || t - r->rot_ms >= 20) { memcpy(r->prow, m, sizeof r->prow); r->rot_ms = t; }
        out[n].ent = r->ent; out[n].class_hash = r->hash; out[n].kind = r->kind;
        memcpy(out[n].guid, r->guid_state == 1 ? r->guid : (const uint8_t*)"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16);
        memcpy(out[n].m, m, sizeof m); memcpy(out[n].vel, r->vel, sizeof r->vel); memcpy(out[n].avel, r->avel, sizeof r->avel); out[n].gen = r->gen;
        n++;
    }
    ReleaseSRWLockExclusive(&s_lock);
    return n;
}
/* despawn: the game's own release of a managed entity (as gf_coop_puppet.c release): func_00455F58(ent + 0x30) */
/* the car the local player sits in is never released (driver veh+0x104 / occupants +0x10C/+0x110 hold ent+0x38) */
static int player_in(uint32_t veh, uint32_t pl)
{
    uint32_t h;
    if (!pl) return 0;
    if (safe32(veh + 0x104u, &h) && h == pl + 0x38u) return 1;
    if (safe32(veh + 0x10Cu, &h) && h == pl + 0x38u) return 1;
    if (safe32(veh + 0x110u, &h) && h == pl + 0x38u) return 1;
    return 0;
}
static int cap0_near(void);   /* v1071D (below) */
static int s_zs = -1; static volatile LONG s_zs_keep;   /* v1071D zone split (GF_COOP_ZONE_SPLIT) */
static int zs_on_re(void) { if (s_zs < 0) { const char* e = getenv("GF_COOP_ZONE_SPLIT"); s_zs = !(e && *e == '0'); } return s_zs; }
int gf_world_joiner_far(void) { return zs_on_re() && s_hostpos_ok && !cap0_near(); }   /* v1071D: joiner out of the host zone (10 m hysteresis) */
static void release_ambient(void)
{
    uint32_t todo[16]; int k = 0;
    const uint32_t pl = player(), pup = gf_coop_puppet_entity();
    float me12[12]; int far_ = 0;   /* v1071D: joiner beyond the host zone -> natives nearer to us than to the host are ours */
    if (gf_world_joiner_far() && pl && read_world(pl, me12)) far_ = 1;
    AcquireSRWLockExclusive(&s_lock);
    for (int i = 0; i < s_n && k < 16; i++) {
        wrec* r = &s_rec[i]; uint32_t vt;
        if (r->released || r->ours || r->gone || !is_ambient(r->hash, r->owner, r->req, r->req_vt) || r->ent == pl || is_pup(r->ent, pup)
            || gf_world_is_proxy(r->ent)) continue;
        if (now_ms() - r->born < 200) continue;   /* let construction / post-spawn init finish first */
        if (gf_coop_world_src_has(r->ent)) { InterlockedIncrement(&s_keep_owned); continue; }   /* v1068R: we own it (synced to the host) -> it stays */
        if (!safe32(r->ent, &vt) || vt != r->vt) continue;
        if (!game_alive(r)) continue;
        if (r->kind == GF_WK_CAR && player_in(r->ent, pl)) { InterlockedIncrement(&s_keep_drv); continue; }
        if (s_spatial && s_hostpos_ok) {   /* v1063H: only where the host has population to send */
            float m[12]; if (read_world(r->ent, m)) { const float dx = m[9] - s_hostpos[0], dz = m[11] - s_hostpos[2];
                if (dx * dx + dz * dz > s_sup_rel_m * s_sup_rel_m) continue;
                if (far_) { const float jx = m[9] - me12[9], jz = m[11] - me12[11]; if (jx * jx + jz * jz < dx * dx + dz * dz) { InterlockedIncrement(&s_zs_keep); continue; } } } }
        r->released = 1; todo[k++] = r->ent;
    }
    ReleaseSRWLockExclusive(&s_lock);
    for (int i = 0; i < k; i++) { gcall(0x00455F58u, todo[i] + 0x30u, 0); InterlockedIncrement(&s_released_n); }
}
void gf_world_suppress_ambient(int on_)
{
    if (!on()) return;
    if ((LONG)(on_ != 0) != s_suppress) { InterlockedExchange(&s_suppress, on_ != 0);
        printf("[coop-worldre] ambient peds/traffic suppression %s (mode %s)\n", on_ ? "ON" : "off",
               s_sup_mode == 3 ? "both" : s_sup_mode == 2 ? "factory" : s_sup_mode == 1 ? "release" : "off"); fflush(stdout); }
    if (on_ && (s_sup_mode == 1 || s_sup_mode == 3)) release_ambient();   /* call every tick while on: new ambient spawns go right away */
    if (!s_in_status) gf_world_status_tick();
}
/* 10 s status line + requester table (main thread; W2 may call it from the world tick, gf_world_enum calls it too) */
void gf_world_status_tick(void)
{
    static ULONGLONG next = 0, rq_next = 0; const ULONGLONG t = now_ms();
    if (!on() || t < next || s_in_status) return;
    next = t + 10000; s_in_status = 1;
    {
        int peds = 0, cars = 0, seen = 0, guids = 0, ours = 0, amb = 0, alive = 0, gone = 0, rel = 0;
        static LONG l_amb = 0, l_rel = 0, l_ref = 0;
        AcquireSRWLockShared(&s_lock);
        for (int i = 0; i < s_n; i++) { const wrec* r = &s_rec[i]; if (r->kind == GF_WK_CAR) cars++; else peds++;
            if (r->anim_ms && t - r->anim_ms < 3000) seen++;
            if (r->guid_state == 1) guids++;
            if (r->ours) ours++;
            if (is_ambient(r->hash, r->owner, r->req, r->req_vt)) amb++;
            if (r->alive_seen) alive++;
            if (r->gone) gone++;
            if (r->released) rel++; }
        ReleaseSRWLockShared(&s_lock);
        printf("[coop-worldre] tracked %d peds %d cars (%d animated <3s, %d GUIDs) | spawned %ld freed %ld stale %ld released %ld refused %ld overflow %ld\n",
               peds, cars, seen, guids, s_spawned, s_freed, s_stale, s_released_n, s_refused, s_overflow);
        printf("[coop-worldre] v1061A entries: ours %d, ambient %d, alive-bit %d, game-released %d (total %ld), we released %d | anim hook %ld calls, %ld in range, %ld hits"
               " | 10s: ambient spawns %ld, released %ld, refused %ld | suppress %s, kept player car %ld\n",
               ours, amb, alive, gone, s_gone_n, rel, s_anim_calls, s_anim_inrange, s_anim_hit,
               s_amb_spawned - l_amb, s_released_n - l_rel, s_refused - l_ref, s_suppress ? "ON" : "off", s_keep_drv);
        l_amb = s_amb_spawned; l_rel = s_released_n; l_ref = s_refused;
        { extern void gf_world_req_cap_log(void); gf_world_req_cap_log(); }   /* v1071C */
        if (s_hostpos_ok && s_dup_log) {   /* v1064I guest: own natives (not our proxies) still alive near the host = possible DUPLICATES of host entities */
            const uint32_t pl = player(), pup = gf_coop_puppet_entity(); int n_amb = 0, n_bound = 0, n_unb = 0, n_far = 0, shown = 0;
            char ex[3][96]; ex[0][0] = ex[1][0] = ex[2][0] = 0;
            AcquireSRWLockExclusive(&s_lock);
            for (int i = 0; i < s_n; i++) { wrec* r = &s_rec[i]; uint32_t vt; float m[12];
                if (r->ours || r->released || r->gone || r->ent == pl || is_pup(r->ent, pup)) continue;
                if (!safe32(r->ent, &vt) || vt != r->vt || !game_alive(r) || !read_world(r->ent, m)) continue;
                { const float dx = m[9] - s_hostpos[0], dz = m[11] - s_hostpos[2]; if (dx * dx + dz * dz > s_sup_near_m * s_sup_near_m) { n_far++; continue; }
                  if (gf_world_is_proxy(r->ent)) { n_bound++; continue; }
                  if (is_ambient(r->hash, r->owner, r->req, r->req_vt)) { n_amb++; continue; }
                  n_unb++;
                  if (shown < 3) snprintf(ex[shown++], sizeof ex[0], "%s %08X class %08X owner %X req-vt %08X %.0f m", r->kind == GF_WK_CAR ? "car" : "ped",
                                          r->ent, r->hash, r->owner, r->req_vt, (double)sqrtf(dx * dx + dz * dz)); } }
            ReleaseSRWLockExclusive(&s_lock);
            printf("[coop-worldre] v1064I guest natives near host (< %.0f m): adopted as proxies %d | ambient not yet released %d | UNBOUND non-ambient %d (possible duplicates) | beyond %d\n",
                   (double)s_sup_near_m, n_bound, n_amb, n_unb, n_far);
            for (int k = 0; k < shown; k++) printf("[coop-worldre]   v1064I unbound native %s\n", ex[k]);
        }
        if (s_anim_dn) { printf("[coop-worldre]   unmatched anim ents, delta to tracked ent below:");
            for (int i = 0; i < s_anim_dn; i++) printf(" %X", (unsigned)s_anim_d[i]);
            printf("\n"); s_anim_dn = 0; }
    }
    if (t >= rq_next) { rq_next = t + 60000;
        for (int i = 0; i < RQ_MAX && (s_rq[i].vt || s_rq[i].hash); i++)
            printf("[coop-worldre]   requester vt %08X class %08X spawns %ld\n", s_rq[i].vt, s_rq[i].hash, s_rq[i].n); }
    {   /* sample: the 3 nearest to the player */
        gf_world_ent e[64]; float pp[12]; const int n = gf_world_enum(e, 64); const uint32_t pl = player();
        if (pl && read_world(pl, pp)) {
            for (int s = 0; s < 3 && s < n; s++) {
                int best = -1; float bd = 1e30f;
                for (int i = 0; i < n; i++) { float dx = e[i].m[9] - pp[9], dz = e[i].m[11] - pp[11], d = dx * dx + dz * dz;
                    if (d < bd && e[i].ent) { bd = d; best = i; } }
                if (best < 0) break;
                printf("[coop-worldre]   near %s %08X class %08X at %.1f %.1f %.1f (%.1f m) vel %.1f %.1f %.1f\n", e[best].kind == GF_WK_CAR ? "car" : "ped",
                       e[best].ent, e[best].class_hash, e[best].m[9], e[best].m[10], e[best].m[11], sqrtf(bd), e[best].vel[0], e[best].vel[1], e[best].vel[2]);
                e[best].ent = 0;
            }
        }
    }
    fflush(stdout);
    s_in_status = 0;
}

/* ---------------------------------------------------------------- v1063H (agent H) */
void gf_world_set_host_pos(const float pos[3], int valid)   /* guest main thread, every world tick */
{
    if (!valid || !pos) { InterlockedExchange(&s_hostpos_ok, 0); InterlockedExchange(&s_sup_near, 1); return; }
    s_hostpos[0] = pos[0]; s_hostpos[1] = pos[1]; s_hostpos[2] = pos[2];
    {   const uint32_t pl = player(); float m[12]; LONG near_ = 1;
        if (pl && read_world(pl, m)) { const float dx = m[9] - pos[0], dz = m[11] - pos[2]; const float d2 = dx * dx + dz * dz;
            /* hysteresis 20 m so the refusal does not flap at the edge */
            near_ = s_sup_near ? d2 < (s_sup_near_m + 20.0f) * (s_sup_near_m + 20.0f) : d2 < s_sup_near_m * s_sup_near_m; }
        if (near_ != s_sup_near) { printf("[coop-worldre] v1063H guest %s the host -> ambient spawn refusal %s\n", near_ ? "near" : "FAR from", near_ ? "ON" : "off (own street life)"); fflush(stdout); }
        InterlockedExchange(&s_sup_near, near_); }
    InterlockedExchange(&s_hostpos_ok, 1);
}
/* guest: best native (not ours, not ambient, not a proxy / adopted, alive) of class cls (+ same GUID when both known)
 * within maxd metres (XZ, |dy| < 4 m) of pos. 0 = none. Main PPU thread. */
uint32_t gf_world_native_match(uint32_t cls, const uint8_t guid[16], const float pos[3], float maxd)
{
    uint32_t best = 0; float bd = maxd * maxd; int gz = 1;
    const uint32_t pl = player(), pup = gf_coop_puppet_entity();
    if (!on() || !pos) return 0;
    resolve_guids();   /* the guest never runs gf_world_enum -> resolve native GUIDs here (<= 8 per call) */
    if (guid) for (int b = 0; b < 16; b++) if (guid[b]) { gz = 0; break; }
    AcquireSRWLockExclusive(&s_lock);
    for (int i = 0; i < s_n; i++) {
        wrec* r = &s_rec[i]; uint32_t vt; float m[12];
        if (r->hash != cls || r->ours || r->released || r->gone || r->ent == pl || is_pup(r->ent, pup)) continue;
        if (is_ambient(r->hash, r->owner, r->req, r->req_vt)) continue;
        if (!gz && r->guid_state == 1 && memcmp(r->guid, guid, 16)) continue;
        if (!safe32(r->ent, &vt) || vt != r->vt || !game_alive(r) || gf_world_is_proxy(r->ent)) continue;
        if (!read_world(r->ent, m)) continue;
        { const float dx = m[9] - pos[0], dy = m[10] - pos[1], dz = m[11] - pos[2]; const float d2 = dx * dx + dz * dz;
          if (fabsf(dy) < 4.0f && d2 < bd) { bd = d2; best = r->ent; } }
    }
    ReleaseSRWLockExclusive(&s_lock);
    if (best) InterlockedIncrement(&s_n_match);
    return best;
}
void gf_world_v1063_status(char* buf, int n)
{
    snprintf(buf, (size_t)n, "sync %s, spatial %s, host pos %s, guest near host %s, kept far spawns %ld, native matches %ld, v1068R owned kept %ld",
             s_sync_all ? "ALL" : "ambient", s_spatial ? "on" : "off", s_hostpos_ok ? "known" : "unknown", s_sup_near ? "yes" : "NO", s_n_kept_far, s_n_match, s_keep_owned);
}

/* ---------------------------------------------------------------- v1070B (agent B) overlap helpers for gf_coop_world.c
 * gf_world_native_info: 1 = ent is a tracked native of THIS game (not ours / released / gone); *ambient = population set
 * (what the joiner suppresses), *age_ms = time since the factory spawned it.
 * gf_world_release_native: release ONE ambient native with the game's own release (func_00455F58(ent + 0x30), as
 * release_ambient). Refuses: ours / proxies / player / puppet / non-ambient (mission) / gone / < 200 ms old / a car the
 * local player or the partner's puppet sits in. Main PPU thread only. 1 = released. */
int gf_world_native_info(uint32_t ent, int* ambient, unsigned* age_ms)
{
    int ok = 0;
    if (!ent || !on()) return 0;
    AcquireSRWLockShared(&s_lock);
    for (int i = 0; i < s_n; i++) { const wrec* r = &s_rec[i]; if (r->ent != ent) continue;
        if ((!r->ours || r->msrc) && !r->released && !r->gone) { ok = 1;   /* v1126M: + migrated-in sources */
            if (ambient) *ambient = r->msrc || is_ambient(r->hash, r->owner, r->req, r->req_vt);
            if (age_ms) { const ULONGLONG t = now_ms(); *age_ms = t > r->born ? (unsigned)(t - r->born > 0xFFFFFFF0u ? 0xFFFFFFF0u : t - r->born) : 0; } }
        break; }
    ReleaseSRWLockShared(&s_lock);
    return ok;
}
static volatile LONG s_rel_v1070 = 0;
int gf_world_release_native(uint32_t ent)
{
    const uint32_t pl = player(), pup = gf_coop_puppet_entity(); int go = 0;
    if (!ent || !on() || ent == pl || is_pup(ent, pup)) return 0;
    AcquireSRWLockExclusive(&s_lock);
    for (int i = 0; i < s_n; i++) { wrec* r = &s_rec[i]; uint32_t vt;
        if (r->ent != ent) continue;
        if (r->released || (r->ours && !r->msrc) || r->gone || !(r->msrc || is_ambient(r->hash, r->owner, r->req, r->req_vt)) || gf_world_is_proxy(r->ent)) break;   /* v1126M msrc */
        if (now_ms() - r->born < 200) break;
        if (!safe32(r->ent, &vt) || vt != r->vt || !game_alive(r)) break;
        if (r->kind == GF_WK_CAR && (player_in(r->ent, pl) || player_in(r->ent, pup))) { InterlockedIncrement(&s_keep_drv); break; }
        r->released = 1; go = 1; break; }
    ReleaseSRWLockExclusive(&s_lock);
    if (!go) return 0;
    gcall(0x00455F58u, ent + 0x30u, 0); InterlockedIncrement(&s_released_n); InterlockedIncrement(&s_rel_v1070);
    return 1;
}

/* v1126M (agent M) migration helpers for gf_coop_world.c. mark_src: ent became a local SOURCE by migration (promoted proxy or a
 * demoted native taken back) -> enumerated / releasable like street life. alive: 1 = still a live tracked native (vtable + game
 * alive bit), 0 = gone / released / freed (our population culled it). Main PPU thread. */
int gf_world_mark_src(uint32_t ent, int on_)
{
    int ok = 0;
    if (!ent || !on()) return 0;
    AcquireSRWLockExclusive(&s_lock);
    for (int i = 0; i < s_n; i++) if (s_rec[i].ent == ent) { s_rec[i].msrc = on_ ? 1 : 0; if (on_) { s_rec[i].released = 0; } ok = 1; break; }
    ReleaseSRWLockExclusive(&s_lock);
    return ok;
}
int gf_world_native_alive(uint32_t ent)
{
    int ok = 0;
    if (!ent || !on()) return 0;
    AcquireSRWLockExclusive(&s_lock);
    for (int i = 0; i < s_n; i++) { wrec* r = &s_rec[i]; uint32_t vt;
        if (r->ent != ent) continue;
        ok = !r->released && !r->gone && safe32(r->ent, &vt) && vt == r->vt && game_alive(r);
        break; }
    ReleaseSRWLockExclusive(&s_lock);
    return ok;
}
uint32_t gf_world_req_of(uint32_t ent)   /* v1126M (P request): requester (factory 4th arg) of a tracked native, 0 = unknown / none */
{
    uint32_t q = 0;
    if (!ent || !on()) return 0;
    AcquireSRWLockShared(&s_lock);
    for (int i = 0; i < s_n; i++) if (s_rec[i].ent == ent) { q = s_rec[i].req; break; }
    ReleaseSRWLockShared(&s_lock);
    return q;
}
extern int gf_coop_world_migrate_on(void);   /* gf_coop_world.c v1126M */

/* ================================================================ v1071C (Claude, 2026-11-06): no spawn churn on the joiner
 * User clip 14:35 (joiner 31.9 m from the host): on the sidewalk ahead NPCs (and a car) fade in at the SAME spot, walk in place,
 * vanish, and the next one fades in there. Joiner log: "10s: ambient spawns 71, released 55" -- the joiner's OWN ambient
 * population (requester vt 1004B5E8 peds + cars) keeps spawning inside the host's zone; the suppression releases every one
 * (func_00455F58 after 200 ms), the game finishes that release only later (they stand there visible), and the freed budget makes
 * the spawner put the next one on the same spawn point = an endless pop cycle in view.
 * Fix: the requester's capacity func_00285F30(req) (target count, compared with the count at req+0xE4 by the requester update
 * func_002889C4 and the slot append func_00287414) is wrapped (apply-pending.ps1 block v1071C): on the JOINER, for the ambient
 * requesters it suppresses (s_amb_vt), while it stands within s_sup_rel_m (130 m) of the host and suppression is on, the capacity
 * is 0 -> the requester never asks for a spawn; what exists is released as before. No factory refusal = no phantom budget entries
 * (v1067R ghost town): beyond 130 m the real capacity comes back and street life spawns normally.
 * GF_COOP_WORLD_CAP0=0 = old; GF_COOP_WORLD_CAP0_TEST=1 = also in echo (empties the echo street; test aid only). */
extern int gf_coop_mode(void); extern int gf_coop_connected(void);
static int s_cap0 = -1, s_cap0_test = 0; static volatile LONG s_cap0_n, s_cap0_amb_calls, s_cap0_far;
static int cap0_near(void)   /* joiner within s_sup_rel_m of the host (cached 100 ms) */
{
    static ULONGLONG s_t; static int s_v;
    const ULONGLONG t = now_ms();
    if (t - s_t >= 100) { float m[12]; const uint32_t pl = player(); const int pv = s_v; s_t = t; s_v = 0;
        if (!s_spatial) s_v = 1;
        else if (s_hostpos_ok && pl && read_world(pl, m)) { const float dx = m[9] - s_hostpos[0], dz = m[11] - s_hostpos[2];
            const float fr = gf_world_far_radius() < s_sup_rel_m ? gf_world_far_radius() : s_sup_rel_m;   /* v1072Z */
            const float lim = pv || !zs_on_re() ? fr : fr - 10.0f;   /* v1071D: back into the zone only below 120 m (no flapping at 130) */
            s_v = dx * dx + dz * dz <= lim * lim; } }
    return s_v;
}
/* v1101P bigger street population (user: "NPCs despawn so quickly when I turn my back, make the pool bigger so more NPCs & cars
 * spawn -- right now it's only a small stretch near the player, the game looks soulless"). The street population requester
 * (vtable 1004B5E8: sidewalk peds, traffic, police cars; mission / crew requesters untouched) gets GF_POP_MUL (2.0) x the game's
 * capacity, clamped to 62 (its slot list req+0xEC holds 64 x 8 B: req+0x2EC is the next field), and its radius req+0xA8 (out-of-view
 * despawn distance) x GF_POP_RADIUS_MUL (1.5) once per requester. GF_POP=0 = the game's own numbers. Log [pop]. */
static int s_pop = -1; static float s_pop_mul = 2.5f, s_pop_rmul = 1.5f;   /* v1109S: 2.0 -> 2.5 (the population now covers a much wider area) */
static float s_pop_reach = 100.0f, s_pop_car_reach = 200.0f;   /* v1109S: despawn radius floors (peds: game radius < 60 m; traffic / other street: < 150 m) */
static struct { uint32_t req; int cap0, cap1; float r0, r1; int capmax; } s_pop_req[24]; static int s_pop_nreq;
static float s_pop_floor = 0.6f;   /* v1109W-2: GF_POP_CAP_FLOOR -- never below this share of the highest capacity seen (the game cut 30 -> 3: mass cull) */
static int pop_scale(uint32_t req, int cap)
{
    uint32_t vt = 0;
    if (s_pop < 0) { const char* e = getenv("GF_POP"); s_pop = !(e && *e == '0');
        if ((e = getenv("GF_POP_MUL")) && atof(e) > 0.1) s_pop_mul = (float)atof(e);
        if ((e = getenv("GF_POP_RADIUS_MUL")) && atof(e) > 0.1) s_pop_rmul = (float)atof(e);
        if ((e = getenv("GF_POP_REACH")) && atof(e) >= 0.0) s_pop_reach = (float)atof(e);
        if ((e = getenv("GF_POP_CAR_REACH")) && atof(e) >= 0.0) s_pop_car_reach = (float)atof(e);
        printf("[pop] v1109S despawn radius floors: peds %.0f m, traffic %.0f m (GF_POP_REACH / GF_POP_CAR_REACH)\n", (double)s_pop_reach, (double)s_pop_car_reach);
        printf("[pop] v1101P street population: capacity x%.2f (max 62), despawn radius x%.2f %s (GF_POP)\n", (double)s_pop_mul, (double)s_pop_rmul, s_pop ? "ON" : "off"); fflush(stdout); }
    if (!s_pop || cap <= 0 || !safe32(req, &vt) || vt != 0x1004B5E8u) return cap;
    int k = -1; for (int i = 0; i < s_pop_nreq; i++) if (s_pop_req[i].req == req) { k = i; break; }
    if (k < 0 && s_pop_nreq < 24) {
        k = s_pop_nreq++; s_pop_req[k].req = req; s_pop_req[k].cap0 = cap; s_pop_req[k].r0 = s_pop_req[k].r1 = 0.0f;
        uint32_t ru = 0;
        if (safe32(req + 0xA8u, &ru)) { float r; memcpy(&r, &ru, 4);
            if (r > 5.0f && r < 400.0f) { float r2 = r * s_pop_rmul; uint32_t w;
                if (r < 60.0f) { if (r2 < s_pop_reach) r2 = s_pop_reach; } else if (r < 150.0f) { if (r2 < s_pop_car_reach) r2 = s_pop_car_reach; }   /* v1109S */
                memcpy(&w, &r2, 4);
                __try { uint8_t* q = vm_base + req + 0xA8u; q[0] = (uint8_t)(w >> 24); q[1] = (uint8_t)(w >> 16); q[2] = (uint8_t)(w >> 8); q[3] = (uint8_t)w; s_pop_req[k].r0 = r; s_pop_req[k].r1 = r2; }
                __except (EXCEPTION_EXECUTE_HANDLER) { } } }
    }
    int nc = (int)((float)cap * s_pop_mul + 0.5f); if (nc > 62) nc = 62; if (nc < cap) nc = cap;
    if (k >= 0) { if (nc > s_pop_req[k].capmax) s_pop_req[k].capmax = nc;   /* v1109W-2 floor */
        { static int fl = -1; if (fl < 0) { const char* e = getenv("GF_POP_CAP_FLOOR"); if (e && *e) s_pop_floor = (float)atof(e); fl = 1; } }
        const int fl = (int)((float)s_pop_req[k].capmax * s_pop_floor + 0.5f); if (nc < fl) nc = fl; }
    if (k >= 0 && s_pop_req[k].cap1 != nc) { s_pop_req[k].cap1 = nc;
        static int nl = 0; if (nl++ < 40) { uint32_t cls = 0; safe32(req + 4u, &cls);
            printf("[pop] v1101P requester %08X: capacity %d -> %d, despawn radius %.0f -> %.0f m\n", req, cap, nc, (double)s_pop_req[k].r0, (double)s_pop_req[k].r1); fflush(stdout); } }
    return nc;
}
int gf_world_req_cap(uint32_t req, int cap)
{
    uint32_t vt = 0; int amb = 0, m;
    cap = pop_scale(req, cap);   /* v1101P */
    {   /* v1126M population per player: no host zone / CAP0. Every game fills its own half; while another player stands close both
         * fill the same area -> the street requesters' capacity goes from x0.5 (partner < 40 m) to x1 (> 160 m). */
        int mg = 0; __try { mg = gf_coop_world_migrate_on(); } __except (EXCEPTION_EXECUTE_HANDLER) { mg = 0; }
        static int cs = -1; if (cs < 0) { const char* e = getenv("GF_COOP_MIGRATE_CAPSCALE"); cs = e && *e == '1';   /* opt-in: a capacity drop makes the game cull at once */
            if (mg) { printf("[migrate] v1126M population per player: no host zone / CAP0; street capacity scaled by partner distance %s (GF_COOP_MIGRATE_CAPSCALE)\n", cs ? "ON" : "off"); fflush(stdout); } else cs = -1; }
        if (mg && cs != 1) return cap;
        if (mg) { uint32_t v2 = 0; int amb2 = 0; if (cap <= 1 || !safe32(req, &v2)) return cap;
            for (int i = 0; i < s_amb_vt_n; i++) if (v2 == s_amb_vt[i]) amb2 = 1;
            if (!amb2) return cap;
            {   gf_remote rr[GF_REMOTE_MAX]; float m[12]; const uint32_t pl = player(); const int nr = gf_world_remotes(rr, GF_REMOTE_MAX); float bd = 1e9f, f;
                if (nr <= 0 || !pl || !read_world(pl, m)) return cap;
                for (int i = 0; i < nr; i++) { const float dx = rr[i].p[0] - m[9], dz = rr[i].p[2] - m[11], d = sqrtf(dx * dx + dz * dz); if (d < bd) bd = d; }
                f = bd <= 40.0f ? 0.5f : bd >= 160.0f ? 1.0f : 0.5f + 0.5f * (bd - 40.0f) / 120.0f;
                { const int nc = (int)((float)cap * f + 0.5f); return nc < 1 ? 1 : nc; } } } }
    if (s_cap0 < 0) { const char* e = getenv("GF_COOP_WORLD_CAP0"); s_cap0 = !(e && *e == '0');
        e = getenv("GF_COOP_WORLD_CAP0_TEST"); s_cap0_test = e && *e == '1';
        printf("[coop-worldre] v1071C joiner ambient spawn capacity 0 inside the host zone (%.0f m): %s%s (GF_COOP_WORLD_CAP0)\n", (double)s_sup_rel_m,
               s_cap0 ? "ON" : "off", s_cap0_test ? " + ECHO TEST" : ""); fflush(stdout); }
    if (!s_cap0 || cap <= 0 || !on()) return cap;
    if (!safe32(req, &vt)) return cap;
    for (int i = 0; i < s_amb_vt_n; i++) if (vt == s_amb_vt[i]) amb = 1;
    if (!amb) return cap;
    m = gf_coop_mode();
    if (m == 3) { if (!s_cap0_test) return cap; }
    else {
        if (m != 2 || !gf_coop_connected() || !s_suppress || !(s_sup_mode == 1 || s_sup_mode == 3)) return cap;
        InterlockedIncrement(&s_cap0_amb_calls);
        if (!cap0_near()) { InterlockedIncrement(&s_cap0_far); return cap; }
    }
    InterlockedIncrement(&s_cap0_n);
    return 0;
}
void gf_world_req_cap_log(void)
{
    if (s_cap0 < 0) return;
    printf("[coop-worldre] v1071C spawn capacity: ambient requester checks %ld, set to 0 (joiner in the host zone) %ld, normal (joiner > %.0f m from the host) %ld | v1071D zone split: natives nearer to us than to the host kept (not released) %ld\n",
           s_cap0_amb_calls, s_cap0_n, (double)s_sup_rel_m, s_cap0_far, s_zs_keep); fflush(stdout);
}

/* v1109S street population SPREAD (user: "the density is okay, but I want it spread out even more, so there are cars driving waaay
 * further down and NPCs walking longer down the road, instead of almost everyone in this small section").
 * Root cause (VERIFIED, lifted code): the three ambient spawn-point scorers func_00338DE0 / func_003390D0 / func_0033BCA8 give each
 * candidate point  score = (view term) - min(dist, TOC-relative 400.0f)  with dist = func_002841F0(pos) (distance to the player focus,
 * TOC+0x93C) and keep the MAX -> the game always takes the NEAREST free point just outside its view test. Together with the street
 * requesters' small out-of-view despawn radius (req+0xA8: sidewalk peds 20 m) everyone lives in a small ring around the player.
 * Change: (a) pop_scale raises the radius to >= GF_POP_REACH (100 m peds) / GF_POP_CAR_REACH (200 m traffic) so far NPCs/cars survive;
 * (b) func_002841F0 is wrapped (apply-pending.ps1 block v1109S): at the six scorer call sites only (lr 338FC4/339094, 339184/3391AC,
 * 33C084/33C260) the returned "distance" is remapped: inside the ring [lo, hi] = a stable pseudo-random 0..300 per point (hash of the
 * point + a 400 ms epoch) -> uniform pick among ring points = spread evenly down the streets; nearer than lo = 330+; beyond hi = 350+
 * (always < 400 so the game's own branch is unchanged, and any ring point beats any outside point). hi = 0.85 x the requester's new
 * radius when the requester update func_002889C4 (also wrapped: tags the requester per thread) is on the stack; else GF_POP_SPREAD_FAR
 * (85 m: below the peds' 100 m radius; soak m38: after the first ~110 spawns ALL scoring runs untagged, outside func_002889C4).
 * The view term is untouched (v1101P-2 spawn guard still applies). GF_POP_SPREAD=0 = the game's nearest-first. Log [pop]. */
static __declspec(thread) uint32_t t_pop_req;
uint32_t gf_pop_req_swap(uint32_t req) { const uint32_t o = t_pop_req; t_pop_req = req; return o; }
static int s_spr = -1; static float s_spr_lo = 35.0f, s_spr_car_lo = 45.0f, s_spr_far = 95.0f, s_spr_frac = 0.95f;   /* v1109W: 85 -> 95 m so far in-view spawns (>= 80 m, gf_coop_npcfix.c) exist */
static volatile LONG st_spr_tag, st_spr_untag, st_spr_in, st_spr_near, st_spr_beyond, st_spr_behind;
static __declspec(thread) uint32_t t_spr_pos; static __declspec(thread) float t_spr_val; static __declspec(thread) uint32_t t_spr_ep;
static uint32_t spr_hash(uint32_t a, uint32_t b, uint32_t c)
{
    uint64_t x = ((uint64_t)a << 32 | b) ^ ((uint64_t)c * 0x9E3779B97F4A7C15ull);
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 27; x *= 0x94D049BB133111EBull; x ^= x >> 31;
    return (uint32_t)x;
}
/* v1109W census: live entities per street requester (count req+0xE4 / the game's capacity / ours) for the 10 s [pop] line */
void gf_pop_census_log(void)
{
    int tot = 0, totc = 0; char b[400]; int o = 0;
    for (int i = 0; i < s_pop_nreq && o < 360; i++) { uint32_t c = 0; if (!safe32(s_pop_req[i].req + 0xE4u, &c) || c > 200) continue;
        tot += (int)c; totc += s_pop_req[i].cap1; o += snprintf(b + o, sizeof b - (size_t)o, " %d/%d", (int)c, s_pop_req[i].cap1); }
    b[o] = 0; printf("[pop] v1109W census: %d live street entities (capacity %d) per requester (live/cap):%s\n", tot, totc, b); fflush(stdout);
}
float gf_pop_spawn_dist(uint32_t pos, uint32_t lr, float d)
{
    if (s_spr < 0) { const char* e = getenv("GF_POP_SPREAD"); s_spr = !(e && *e == '0');
        if ((e = getenv("GF_POP_SPREAD_MIN")) && atof(e) >= 0.0) s_spr_lo = (float)atof(e);
        if ((e = getenv("GF_POP_SPREAD_CAR_MIN")) && atof(e) >= 0.0) s_spr_car_lo = (float)atof(e);
        if ((e = getenv("GF_POP_SPREAD_FAR")) && atof(e) > 10.0) s_spr_far = (float)atof(e);
        if ((e = getenv("GF_POP_SPREAD_FRAC")) && atof(e) > 0.1 && atof(e) <= 1.0) s_spr_frac = (float)atof(e);
        printf("[pop] v1109S spawn spread: street spawn points picked at random within %.0f (cars %.0f) .. %.2f x the despawn radius (untagged: %.0f m) instead of nearest-first %s (GF_POP_SPREAD)\n",
               (double)s_spr_lo, (double)s_spr_car_lo, (double)s_spr_frac, (double)s_spr_far, s_spr ? "ON" : "off"); fflush(stdout); }
    { extern int gf_pop_steal_fix_site(uint32_t lr); extern int gf_pop_victim_seen(uint32_t pos);   /* v1110A (gf_coop_npcfix.c) */
      if (gf_pop_steal_fix_site(lr)) return (d == d && gf_pop_victim_seen(pos)) ? 0.0f : d; }   /* seen = never stolen; hidden = real distance */
    {   /* v1126M population per player: a spawn point nearer to another player than to us belongs to that player's population ->
         * scored as the farthest possible point (399 m); the shared view (gf_coop_npcfix.c) adds the in-view penalty. */
        int mg = 0; __try { mg = gf_coop_world_migrate_on(); } __except (EXCEPTION_EXECUTE_HANDLER) { mg = 0; }
        if (mg && (lr == 0x00338FC4u || lr == 0x00339094u || lr == 0x00339184u || lr == 0x003391ACu || lr == 0x0033C084u || lr == 0x0033C260u)) {
            static ULONGLONG t_rr; static gf_remote rr[GF_REMOTE_MAX]; static int nr; static float me[3]; static int me_ok; const ULONGLONG nw = GetTickCount64();
            if (nw - t_rr > 50) { float m[12]; const uint32_t pl = player(); t_rr = nw; nr = gf_world_remotes(rr, GF_REMOTE_MAX); me_ok = pl && read_world(pl, m); if (me_ok) { me[0] = m[9]; me[1] = m[10]; me[2] = m[11]; } }
            if (nr > 0 && me_ok) { uint32_t qx = 0, qz = 0; float px, pz; safe32(pos, &qx); safe32(pos + 8u, &qz); memcpy(&px, &qx, 4); memcpy(&pz, &qz, 4);
                if (px == px && pz == pz) { const float ax = px - me[0], az = pz - me[2], a2 = ax * ax + az * az;
                    for (int i = 0; i < nr; i++) { const float bx = px - rr[i].p[0], bz = pz - rr[i].p[2];
                        if (bx * bx + bz * bz < a2) { static volatile LONG n_ref; static ULONGLONG lt; InterlockedIncrement(&n_ref);
                            if (nw - lt > 10000) { lt = nw; printf("[migrate] v1126M population per player: spawn points refused (nearer to another player) %ld\n", n_ref); fflush(stdout); }
                            return 399.0f; } } } } } }
    if (!s_spr) return d;
    if (!(lr == 0x00338FC4u || lr == 0x00339094u || lr == 0x00339184u || lr == 0x003391ACu || lr == 0x0033C084u || lr == 0x0033C260u)) return d;
    if (!(d == d) || d < 0.0f) return d;
    const uint32_t ep = (uint32_t)(GetTickCount64() / 400u);
    if (pos == t_spr_pos && ep - t_spr_ep <= 1u) return t_spr_val;   /* same candidate, 2nd call (the int subtraction) -> same value */
    float lo = s_spr_lo, hi = s_spr_far; int car = 0;
    const uint32_t rq = t_pop_req;
    if (rq) { InterlockedIncrement(&st_spr_tag);
        for (int i = 0; i < s_pop_nreq; i++) if (s_pop_req[i].req == rq) {
            if (s_pop_req[i].r1 > 1.0f) { hi = s_pop_req[i].r1 * s_spr_frac; car = s_pop_req[i].r0 >= 60.0f; }
            break; } }
    else InterlockedIncrement(&st_spr_untag);
    if (car) lo = s_spr_car_lo;
    if (hi < lo + 10.0f) hi = lo + 10.0f;
    float v;
    if (d >= lo && d <= hi) { uint32_t qx = 0, qz = 0; safe32(pos, &qx); safe32(pos + 8u, &qz);
        v = (float)(spr_hash(qx, qz, ep) >> 8) * (200.0f / 16777216.0f); InterlockedIncrement(&st_spr_in);
        /* v1109X (user video 01:28: street ahead empty): hidden spots BEHIND the player won every pick and he walked away from
         * them. Points behind the camera plane get +130 (still < 400): ahead (round the next corner, behind a building, far down
         * the street) wins. */
        { extern int gf_coop_local_cam(float* fx, float* fz); float fx, fz, m[12], px, pz; const uint32_t pl = player();
          if (pl && read_world(pl, m) && gf_coop_local_cam(&fx, &fz)) { float u; memcpy(&u, &qx, 4); px = u; memcpy(&u, &qz, 4); pz = u;
            if ((px - m[9]) * fx + (pz - m[11]) * fz < 0.0f) { v += 130.0f; InterlockedIncrement(&st_spr_behind); } } } }
    else if (d < lo) { v = 330.0f + d * 0.5f; InterlockedIncrement(&st_spr_near); }
    else { const float o = d - hi; v = 350.0f + (o < 49.0f ? o : 49.0f); InterlockedIncrement(&st_spr_beyond); }
    t_spr_pos = pos; t_spr_val = v; t_spr_ep = ep;
    { static ULONGLONG lt; const ULONGLONG now = GetTickCount64(); if (now - lt > 10000) { lt = now;
        printf("[pop] v1109S spawn spread: candidates in ring %ld (behind the player %ld), nearer %ld, beyond %ld | requester tagged %ld, untagged %ld\n",
               st_spr_in, st_spr_behind, st_spr_near, st_spr_beyond, st_spr_tag, st_spr_untag); fflush(stdout); } }
    return v;
}

/* v1128F (agent F28, c28-1 joiner freeze): our factory bodies (owner = requester = 0: puppet + proxies, incl. the ones the game's
 * release func_00455F58 never frees once the park pool is full) still alive. gf_coop_proxy.c caps new factory spawns on it. */
int gf_world_ours_alive(void)
{
    int n = 0; const ULONGLONG t = now_ms(); AcquireSRWLockExclusive(&s_lock);   /* v1128G (agent F28b): ours are never enumerated -> prune here */
    for (int i = 0; i < s_n; i++) { wrec* r = &s_rec[i]; uint32_t vt = 0;
        if (!r->ent || !r->ours) continue;
        if (!safe32(r->ent, &vt) || vt != r->vt) { s_rec[i] = s_rec[--s_n]; i--; InterlockedIncrement(&s_stale); continue; }   /* freed / reused */
        if (vt == 0x1005F6C8u) continue;                                                   /* destroyed (base vtable) */
        if (game_alive(r) || (r->anim_ms && t - r->anim_ms < 5000)) n++; }               /* alive, or released but still animated */
    ReleaseSRWLockExclusive(&s_lock); return n;
}
