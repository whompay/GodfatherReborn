/* gf_crewdrive.c -- "You drive": a crew member drives the player to a map marker (Onix, 2026-11-21).
 *
 * B (GF_CREWDRIVE_KEY) or holding D-pad LEFT opens the crew menu (gf_dialogue.c's menu look, rsx_ui_overlay.c gfd_build):
 *   YOU DRIVE -> "Where to, boss?" -> BLUE (story) / PINK (favor) / YELLOW (job) / CUSTOM (the player's waypoint).
 * On foot: "Get in a car, boss" -- the player gets in a car as usual (the game puts the crew in with him), then the seats are
 * switched: the first-hired crew member to the wheel, the player to the front passenger seat. In a car: switched at once.
 * While he drives the menu offers CHANGE DESTINATION / PULL OVER / I'LL DRIVE / FASTER-SLOWER.
 *
 * How the car is driven (no game AI involved, the player's own car physics):
 *  - the car's Havok vehicle (veh+0x1B0, verified veh+0x138 body) reads its controls from the device status (+0x9C):
 *    steering +8 (-1..1), accelerator +0x14, brake +0x18, handbrake u8 +0x10, reverse u8 +0x11 (gf_coop_vehicle.c v1070E).
 *    The pad's driver input (hkv+0x20, vt 0x1005E818) gets a private copy of the status (clone func_00432F20) while the crew
 *    drives, so the pad no longer reaches the car; we write the status every frame.
 *  - route: the AI drivers' road graph (assets/gf_gps.bin, the GPS mod's), reverse Dijkstra to the marker's road segment,
 *    driven 2 m right of the road centre line (AICarBehavior::GetOffsetForLane, lane 0) on two-way roads; the side is
 *    checked against the traffic around us (s_side).
 *  - pure pursuit steering, a speed plan from the route's curvature (lateral 3.8 m/s^2) and braking distance (3.5 m/s^2),
 *    cars / people within the lane ahead (now and where they will be) slow or stop us, a leading car is followed with a time gap.
 *  - stuck (no progress with the pedal down): back up with the wheel turned the other way, then go on; four times in a row ->
 *    the car is set back on the road ahead. Upside down -> set upright. Steering / forward / reverse signs calibrate themselves.
 * Seats (BaseVehicle, verified in the lifted code): veh+0x10C seat array (8 B, word 0 = occupant handle = ent + 0x38),
 *   veh+0x110 count; vt+0x200(veh, idx, desc) fills the seat locator {idx + 6, bone}; SwitchCurrentSeatLocator func_00434794
 *   (veh, ent, desc) moves an occupant to a free seat (vt+0x1D4 occupied? / vt+0x1D0 current / vt+0x1F8 relink);
 *   vt+0x1F8(veh, 0, desc) clears a seat link. The crew member's NPCRailShooterSM keeps its seat at +0x68 (gf_crew.c).
 * Markers: the radar marker manager (gf_gps.c g_gps_mgr): 200 slots of 0x2C at +0x44 (x, y, z, +0xC type, +0x10 bit 0 used).
 * Main PPU thread (gf_crewdrive_tick from gf_trainer.c main_tick). Env: GF_CREWDRIVE=0 off, GF_CREWDRIVE_KEY=<letter> (B),
 * GF_CREWDRIVE_KMH (55) cruise speed, GF_CREWDRIVE_FAST_KMH (80), GF_CREWDRIVE_LOG=1 more log, GF_CREWDRIVE_TEST=1 self test
 * (no crew needed: gets in the nearest car, moves the player to the passenger seat and drives to a marker or a far road).
 * Log [crewdrive]. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifdef CD_SIM
#include "crewdrive_sim_env.h"   /* offline driving simulator (F:/GodfatherReborn_modswork/crewdrive_sim) */
#else
#include "recompiled/ppu_recomp.h"
#endif

extern uint8_t* vm_base;
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern int gf_cutscene_active(void);
extern int gf_world_cars(uint32_t* out, int max);
extern int gf_world_npcs(uint32_t* out, int max);          /* gf_coop_worldre.c: the NPCs the factory made */
extern int gf_rag_ray(const float* o, const float* d, float maxd, float* t, float* n);
extern int gf_rag_tris_near(float x, float z, float r, float* out, int max);   /* gf_ragdoll.c: collision triangles */   /* gf_ragdoll.c: world collision */
extern int gf_npc_name(uint32_t ent, char* out, int cap);
extern uint32_t gf_crew_sm_of(uint32_t ent);                 /* gf_crew.c: the crew member's NPCRailShooterSM (in a car) */
extern volatile uint32_t g_gps_mgr; extern volatile uint32_t g_gps_ttype[8]; extern volatile int g_gps_force_slot;   /* gf_gps.c */
extern volatile int g_gfd_menu, g_gfd_sel, g_gfd_nopt, g_gfd_clear, g_gfd_key, g_gfd_lock;   /* gf_dialogue.c menu look */
extern volatile unsigned long long g_gfd_close_t, g_gfd_sub_t1;
extern char g_gfd_opt[5][40], g_gfd_who[48], g_gfd_sub_who[48], g_gfd_sub[200];
extern volatile unsigned g_pad_phys_buttons;                 /* cellPad.c: port 0 buttons before the overlay gate */
extern volatile int g_input_kbm;                             /* cellPad.c: 0 = the controller is in use (else keyboard + mouse) */
extern volatile unsigned g_pad_inject_d1, g_pad_inject_d2; extern volatile int g_pad_inject_polls;
extern volatile long long g_bench_world_ms;
extern int (*volatile g_pad_menu_fn)(void);
extern volatile int g_uiov_menu_open;
typedef void (*gfn)(ppu_context*);
#define TOC 0x00B4BF60u
#define HK_TOC 0x00B5BF60u

#define FN_GETVEHICLE  0x00233CCCu   /* (sentient) -> vehicle entity */
#define FN_SWITCHSEAT  0x00434794u   /* BaseVehicle::SwitchCurrentSeatLocator(veh, ent, locator) */
#define FN_STATUSCLONE 0x00432F20u   /* device status clone */
#define FN_SETLINVEL   0x0076FF90u   /* hkRigidBody::setLinearVelocity (Havok TOC) */
#define FN_SETANGVEL   0x00770064u
#define DI_VTABLE      0x1005E818u   /* the game's driver input (hkv+0x20) */

#define PAD_UP    0x0010u
#define PAD_DOWN  0x0040u
#define PAD_LEFT  0x0080u
#define PAD_CROSS 0x4000u
#define PAD_CIRCLE 0x2000u
#define PAD_TRIANGLE 0x1000u

/* ================================================================ guest memory / calls */
static int ok(uint32_t ea) { return ea >= 0x10000u && ea < 0xF0000000u; }
static uint32_t rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t rd16(uint32_t ea) { const uint8_t* p = vm_base + ea; return (uint16_t)((p[0] << 8) | p[1]); }
static void wr32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void wr16(uint32_t ea, uint16_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static float rdf(uint32_t ea) { uint32_t u = rd32(ea); float f; memcpy(&f, &u, 4); return f; }
static void wrf(uint32_t ea, float f) { uint32_t u; memcpy(&u, &f, 4); wr32(ea, u); }
static int fok(float f) { return f == f && f > -1e7f && f < 1e7f; }
#ifdef CD_SIM
static double now_s(void) { return sim_now(); }
#else
static double now_s(void) { static LARGE_INTEGER fq; LARGE_INTEGER c; if (!fq.QuadPart) QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)fq.QuadPart; }
#endif
static int s_logv;
static int s_nlog;
#define CLOG(...) do { if (s_nlog < 6000) { s_nlog++; printf(__VA_ARGS__); fflush(stdout); } } while (0)

static gfn lookup(uint32_t addr)
{
    static uint32_t ca[32]; static gfn cf[32]; static int cn;
    for (int i = 0; i < cn; i++) if (ca[i] == addr) return cf[i];
    gfn f = 0;
    for (uint64_t k = 0; k < function_table_count; k++) if ((uint32_t)function_table[k].addr == addr) { f = function_table[k].func; break; }
    if (cn < 32) { ca[cn] = addr; cf[cn] = f; cn++; }
    if (!f) CLOG("[crewdrive] func_%08X not in the function table\n", addr);
    return f;
}
static int gcall_t(gfn f, uint32_t toc, const uint64_t* a, int n, uint64_t* ret)
{
    ppu_context* cur = g_active_ctx; if (!cur || !f) return 0;
    static ppu_context c;
    __try {
        memcpy(&c, cur, sizeof c);
        c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1C00u) & ~0xFull;
        c.gpr[2] = toc; for (int i = 0; i < n && i < 8; i++) c.gpr[3 + i] = a[i];
        c.lr = 0;
        g_active_ctx = &c; f(&c);
        while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
        g_active_ctx = cur; if (ret) *ret = c.gpr[3]; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_active_ctx = cur; g_trampoline_fn = 0; CLOG("[crewdrive] guest call faulted\n"); return 0; }
}
static int gcall(uint32_t addr, const uint64_t* a, int n, uint64_t* ret) { return gcall_t(lookup(addr), TOC, a, n, ret); }
/* virtual call: slot = byte offset of the OPD pointer in the vtable; the OPD gives the function and its TOC */
static int vcall(uint32_t obj, uint32_t slot, const uint64_t* a, int n, uint64_t* ret)
{
    if (!ok(obj)) return 0; const uint32_t vt = rd32(obj); if (!ok(vt)) return 0;
    const uint32_t opd = rd32(vt + slot); if (!ok(opd)) return 0;
    const uint32_t fn = rd32(opd), toc = rd32(opd + 4); gfn f = lookup(fn); if (!f) return 0;
    uint64_t b[8] = { obj }; for (int i = 0; i < n && i < 7; i++) b[1 + i] = a[i];
    return gcall_t(f, toc ? toc : TOC, b, n + 1, ret);
}
/* scratch guest memory below the current guest stack (as gf_coop_vehicle.c) */
static uint32_t scratch(void) { if (!g_active_ctx) return 0; return ((uint32_t)g_active_ctx->gpr[1] - 0x900u) & ~0xFu; }

/* ================================================================ entities */
static uint32_t player(void)
{
    __try { uint32_t a = rd32(0x10181290u); if (!ok(a)) return 0; a = rd32(a + 4); if (!ok(a)) return 0; a = rd32(a);
        return (ok(a) && a >= 0x17FCu) ? a - 0x17FCu : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int alive(uint32_t e) { const float h = rdf(e + 0x14C8u); return fok(h) && h > 0.0f; }
static int ent_pos(uint32_t e, float* p) { const uint32_t t = rd32(e + 0x68u); if (!ok(t)) return 0; p[0] = rdf(t + 0x40); p[1] = rdf(t + 0x44); p[2] = rdf(t + 0x48); return fok(p[0]) && fok(p[1]) && fok(p[2]); }
/* RwMatrix rows at xf+0x10 right, +0x20 up, +0x30 at, +0x40 pos */
static int ent_rows(uint32_t e, float* r, float* u, float* a, float* p)
{
    const uint32_t t = rd32(e + 0x68u); if (!ok(t)) return 0;
    for (int k = 0; k < 3; k++) { r[k] = rdf(t + 0x10 + 4 * k); u[k] = rdf(t + 0x20 + 4 * k); a[k] = rdf(t + 0x30 + 4 * k); p[k] = rdf(t + 0x40 + 4 * k); }
    return fok(a[0]) && fok(a[2]) && fok(p[0]) && fok(p[2]);
}
static float dist2d(const float* a, const float* b) { const float dx = a[0] - b[0], dz = a[2] - b[2]; return sqrtf(dx * dx + dz * dz); }
static uint32_t vehicle_of(uint32_t e)   /* the car this sentient sits in, 0 = none */
{
#ifdef CD_SIM
    return sim_vehicle_of(e);
#endif
    if (!ok(e) || !(rd32(e + 0x5C4u) & 8u)) return 0;
    uint64_t v = 0; const uint64_t a[1] = { e };
    if (!gcall(FN_GETVEHICLE, a, 1, &v) || !ok((uint32_t)v)) return 0;
    return (uint32_t)v;
}
static int crew_count(uint32_t pl) { const uint32_t n = rd32(pl + 0x2A20u); return n <= 5 ? (int)n : 0; }
static uint32_t crew_ent(uint32_t pl, int i) { const uint32_t h = rd32(pl + 0x29F8u + 8u * (uint32_t)i); return (ok(h) && h > 0x38u) ? h - 0x38u : 0; }
static int crew_list(uint32_t pl, uint32_t* out)   /* living crew in hire order */
{
    int n = 0;
    for (int i = 0; i < 5; i++) { const uint32_t e = crew_ent(pl, i); if (e && (rd32(e + 0x38u + 7992u) & 0x20000000u) && alive(e)) out[n++] = e; }
    return n;
}

/* ================================================================ seats */
static int seat_count(uint32_t veh) { const uint32_t n = rd32(veh + 0x110u); return n <= 8 ? (int)n : 0; }
static uint32_t seat_occ(uint32_t veh, int i)
{
    const uint32_t arr = rd32(veh + 0x10Cu); if (i < 0 || i >= seat_count(veh) || !ok(arr)) return 0;
    const uint32_t h = rd32(arr + 8u * (uint32_t)i); return h > 0x38u && ok(h) ? h - 0x38u : 0;
}
static int seat_of(uint32_t veh, uint32_t e) { for (int i = 0; i < seat_count(veh); i++) if (seat_occ(veh, i) == e) return i; return -1; }
static int seat_desc(uint32_t veh, int idx, uint32_t desc)
{
    wr32(desc, 0); wr32(desc + 4, 0);
    const uint64_t a[2] = { (uint64_t)(uint32_t)idx, desc }; return vcall(veh, 0x200u, a, 2, NULL) && rd32(desc) == (uint32_t)idx + 6u;
}
static int move_seat(uint32_t veh, uint32_t e, int idx)
{
    const uint32_t buf = scratch(); if (!buf) return 0;
    if (seat_of(veh, e) == idx) return 1;
    if (!seat_desc(veh, idx, buf + 0x40u)) { CLOG("[crewdrive] seat %d: no locator\n", idx); return 0; }
    uint64_t r = 0; const uint64_t a[3] = { veh, e, buf + 0x40u };
    if (!gcall(FN_SWITCHSEAT, a, 3, &r)) return 0;
    const int got = seat_of(veh, e) == idx;
    CLOG("[crewdrive] seat switch %08X -> seat %d: %s (ret %d)\n", e, idx, got ? "ok" : "FAILED", (int)r);
    return got;
}
static int clear_seat(uint32_t veh, int idx)
{
    const uint32_t buf = scratch(); if (!buf || !seat_desc(veh, idx, buf + 0x40u)) return 0;
    const uint64_t a[2] = { 0, buf + 0x40u }; return vcall(veh, 0x1F8u, a, 2, NULL);
}
static int link_seat(uint32_t veh, uint32_t e, int idx)
{
    const uint32_t buf = scratch(); if (!buf || !seat_desc(veh, idx, buf + 0x40u)) return 0;
    const uint64_t a[2] = { e, buf + 0x40u }; return vcall(veh, 0x1F8u, a, 2, NULL) && seat_of(veh, e) == idx;
}
static int free_seat(uint32_t veh, int not1)   /* a free seat, rear first */
{
    for (int i = seat_count(veh) - 1; i >= 1; i--) if (!seat_occ(veh, i) && !(not1 && i == 1)) return i;
    return -1;
}
/* the crew member's own record of his seat (NPCRailShooterSM +0x68 = locator {id, bone}, +0xFC flags: 4 front / 2 rear) */
static void crew_sm_seat(uint32_t veh, uint32_t e, int idx)
{
    const uint32_t sm = gf_crew_sm_of(e), buf = scratch();
    if (!ok(sm) || !buf || !seat_desc(veh, idx, buf + 0x40u)) { CLOG("[crewdrive] crew %08X: no rail-shooter state to update (sm %08X)\n", e, sm); return; }
    wr32(sm + 0x68u, rd32(buf + 0x40u)); wr32(sm + 0x6Cu, rd32(buf + 0x44u));
    uint32_t f = rd32(sm + 0xFCu) & ~0x300u; f = idx <= 1 ? ((f & ~2u) | 4u) : (f | 2u); wr32(sm + 0xFCu, f);
}

/* ================================================================ car physics */
static uint32_t car_body(uint32_t veh) { const uint32_t b = rd32(veh + 0x138u); return ok(b) && ok(rd32(b + 0xA0u)) ? b : 0; }
static int body_live(uint32_t veh, uint32_t b) { return b && ok(rd32(b + 0x8u)) && !(rd32(veh + 0x7C4u) & 0x10u); }
static uint32_t car_hkv(uint32_t veh)
{
    const uint32_t p = rd32(veh + 0x1B0u), b = rd32(veh + 0x138u);
    if (!ok(p) || (p & 3u) || rd32(p + 0x18u) != b) return 0;
    const uint32_t n = rd32(p + 0x4Cu); if (n < 2 || n > 8) return 0;
    return p;
}
static uint32_t car_status(uint32_t hkv) { const uint32_t s = rd32(hkv + 0x9Cu); return ok(s) && !(s & 3u) ? s : 0; }
static int car_vel(uint32_t veh, float* v)
{
    const uint32_t b = car_body(veh); if (!b) return 0;
    for (int k = 0; k < 3; k++) v[k] = rdf(b + 0x180u + 4u * (uint32_t)k);
    return fok(v[0]) && fok(v[2]);
}
static int car_avel_y(uint32_t veh, float* w) { const uint32_t b = car_body(veh); if (!b) return 0; *w = rdf(b + 0x194u); return fok(*w); }

/* ================================================================ road network (assets/gf_gps.bin, the GPS mod's) */
#define UNIT 0.25f
#define BIG 1e30f
static int s_nn, s_ne; static float *s_fx, *s_fy, *s_fz; static int s_hasy; static uint16_t *s_rs, *s_rsrc;
static float *s_distA, *s_distB; static uint16_t *s_nextA, *s_nextB; static uint16_t* s_hn; static float* s_hk;
static FILE* open_asset(const char* name)
{
#ifdef CD_SIM
    return sim_open_asset(name);
#endif
    char exe[MAX_PATH], path[MAX_PATH + 96]; FILE* f = NULL;
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (n && n < MAX_PATH) { char* s = strrchr(exe, '\\'); if (s) *s = 0;
        static const char* rel[] = { "\\assets\\", "\\..\\assets\\", "\\..\\..\\assets\\" };
        for (int i = 0; i < 3 && !f; i++) { snprintf(path, sizeof path, "%s%s%s", exe, rel[i], name); f = fopen(path, "rb"); } }
    return f;
}
static void corridor_build(void);
/* assets/gf_crewdrive_roads.bin (tools/gf_crewdrive_build.py: with heights, bridges kept apart), else the GPS mod's gf_gps.bin */
static void main_component(void);
static int load_roads(void)
{
    FILE* f = open_asset("gf_crewdrive_roads.bin"); int cdr = f != NULL;
    if (!f) f = open_asset("gf_gps.bin");
    if (!f) { CLOG("[crewdrive] no assets\\gf_crewdrive_roads.bin / gf_gps.bin -> off\n"); return 0; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    void* blob = malloc((size_t)sz); if (!blob || fread(blob, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return 0; }
    fclose(f);
    const uint32_t* h = (const uint32_t*)blob;
    if (sz < 16 || memcmp(h, cdr ? "CDR1" : "GPS2", 4) != 0) return 0;
    s_nn = (int)h[1]; s_ne = (int)h[2];
    uint8_t* p = (uint8_t*)blob + 16;
    s_fx = (float*)malloc(sizeof(float) * s_nn); s_fy = (float*)malloc(sizeof(float) * s_nn); s_fz = (float*)malloc(sizeof(float) * s_nn);
    if (!s_fx || !s_fy || !s_fz) return 0;
    if (cdr) { memcpy(s_fx, p, 4 * s_nn); p += 4 * s_nn; memcpy(s_fy, p, 4 * s_nn); p += 4 * s_nn; memcpy(s_fz, p, 4 * s_nn); p += 4 * s_nn; s_hasy = 1; }
    else { const int16_t* X = (const int16_t*)p; p += 2 * s_nn; const int16_t* Z = (const int16_t*)p; p += 2 * s_nn;
           for (int i = 0; i < s_nn; i++) { s_fx[i] = X[i] * UNIT; s_fz[i] = Z[i] * UNIT; s_fy[i] = 0; } s_hasy = 0; }
    s_rs = (uint16_t*)p; p += 2 * (s_nn + 1); s_rsrc = (uint16_t*)p; p += 2 * s_ne;
    if (p > (uint8_t*)blob + sz) return 0;
    s_distA = (float*)malloc(sizeof(float) * s_nn); s_distB = (float*)malloc(sizeof(float) * s_nn);
    s_nextA = (uint16_t*)malloc(2 * s_nn); s_nextB = (uint16_t*)malloc(2 * s_nn);
    s_hn = (uint16_t*)malloc(2 * (s_nn + s_ne + 4)); s_hk = (float*)malloc(sizeof(float) * (s_nn + s_ne + 4));
    corridor_build();
    main_component();
    CLOG("[crewdrive] road network: %d nodes, %d edges%s\n", s_nn, s_ne, s_hasy ? " (with heights)" : " (GPS map, no heights)");
    return s_distA && s_distB && s_nextA && s_nextB && s_hn && s_hk;
}
/* the main road network: the nodes that reach each other both ways (99.5% of them). The rest are scraps the road map lost
 * the link to (one-way stubs, a few driveways); a target or a start there has no route, so the driver ignores them. */
static uint8_t* s_main;
static void main_component(void)
{
    s_main = (uint8_t*)calloc((size_t)s_nn, 1); if (!s_main) return;
    int* os = (int*)calloc((size_t)s_nn + 1, sizeof(int)); int* od = (int*)malloc(sizeof(int) * (size_t)(s_ne > 0 ? s_ne : 1)); int* fill = (int*)calloc((size_t)s_nn, sizeof(int));
    uint8_t* fw = (uint8_t*)malloc((size_t)s_nn); uint8_t* bw = (uint8_t*)malloc((size_t)s_nn); int* q = (int*)malloc(sizeof(int) * (size_t)s_nn);
    int best = 0;
    if (os && od && fill && fw && bw && q) {
        for (int v = 0; v < s_nn; v++) for (int e = s_rs[v]; e < s_rs[v + 1]; e++) os[s_rsrc[e] + 1]++;
        for (int i = 0; i < s_nn; i++) os[i + 1] += os[i];
        for (int v = 0; v < s_nn; v++) for (int e = s_rs[v]; e < s_rs[v + 1]; e++) { const int u = s_rsrc[e]; od[os[u] + fill[u]++] = v; }
        for (int seed = 0; seed < s_nn && best * 2 <= s_nn; seed += 1 + s_nn / 16) {   /* most nodes are in it: a seed or two finds it */
            memset(fw, 0, (size_t)s_nn); memset(bw, 0, (size_t)s_nn);
            int h = 0, t = 0; q[t++] = seed; fw[seed] = 1;
            while (h < t) { const int u = q[h++]; for (int k = os[u]; k < os[u + 1]; k++) { const int v = od[k]; if (!fw[v]) { fw[v] = 1; q[t++] = v; } } }
            h = 0; t = 0; q[t++] = seed; bw[seed] = 1;
            while (h < t) { const int v = q[h++]; for (int e = s_rs[v]; e < s_rs[v + 1]; e++) { const int u = s_rsrc[e]; if (!bw[u]) { bw[u] = 1; q[t++] = u; } } }
            int n = 0; for (int i = 0; i < s_nn; i++) n += fw[i] && bw[i];
            if (n > best) { best = n; for (int i = 0; i < s_nn; i++) s_main[i] = (uint8_t)(fw[i] && bw[i]); }
        }
    }
    if (best * 2 <= s_nn) memset(s_main, 1, (size_t)s_nn);   /* no big piece (odd map): use everything */
    free(os); free(od); free(fill); free(fw); free(bw); free(q);
    CLOG("[crewdrive] main road network: %d of %d nodes (the rest are cut-off scraps)\n", best, s_nn);
}
static int on_main(int u, int v) { return !s_main || (s_main[u] && s_main[v]); }
static float NX(int i) { return s_fx[i]; }
static float NZ(int i) { return s_fz[i]; }
/* ---------------------------------------------------------------- road corridor
 * assets/gf_collision.bin has ~114,000 triangles that lost their placement piled up within 40 m of the world origin, on a
 * real street where the game's traffic drives (measured 2026-11-21). Near the origin (150 m) walls whose middle lies inside
 * a road corridor (on the road's level) are ignored by the driver; elsewhere everything counts (el-train pillars, tunnel
 * walls and docked ships do stand in road corridors). Hashed edges in 16 m cells. */
#define CG 16.0f
static float s_cgx0, s_cgz0; static int s_cgnx, s_cgnz; static int *s_cgs, *s_cgl;
static int has_edge(int u, int v);
static void corridor_build(void)
{
    float mnx = 1e9f, mnz = 1e9f, mxx = -1e9f, mxz = -1e9f;
    for (int i = 0; i < s_nn; i++) { if (s_fx[i] < mnx) mnx = s_fx[i]; if (s_fx[i] > mxx) mxx = s_fx[i]; if (s_fz[i] < mnz) mnz = s_fz[i]; if (s_fz[i] > mxz) mxz = s_fz[i]; }
    s_cgx0 = mnx - 2 * CG; s_cgz0 = mnz - 2 * CG; s_cgnx = (int)((mxx - s_cgx0) / CG) + 3; s_cgnz = (int)((mxz - s_cgz0) / CG) + 3;
    const int nc = s_cgnx * s_cgnz;
    s_cgs = (int*)calloc((size_t)nc + 1, sizeof(int)); if (!s_cgs) return;
    for (int pass = 0; pass < 2; pass++) {
        int* fill = pass ? (int*)calloc((size_t)nc, sizeof(int)) : NULL;
        for (int v = 0; v < s_nn; v++) for (int e = s_rs[v]; e < s_rs[v + 1]; e++) {
            const int u = s_rsrc[e];
                const float x0 = fminf(s_fx[u], s_fx[v]) - 8, x1 = fmaxf(s_fx[u], s_fx[v]) + 8, z0 = fminf(s_fz[u], s_fz[v]) - 8, z1 = fmaxf(s_fz[u], s_fz[v]) + 8;
            for (int cz = (int)((z0 - s_cgz0) / CG); cz <= (int)((z1 - s_cgz0) / CG); cz++)
                for (int cx = (int)((x0 - s_cgx0) / CG); cx <= (int)((x1 - s_cgx0) / CG); cx++) {
                    if (cx < 0 || cz < 0 || cx >= s_cgnx || cz >= s_cgnz) continue;
                    const int c = cz * s_cgnx + cx;
                    if (!pass) s_cgs[c + 1]++; else s_cgl[s_cgs[c] + fill[c]++] = e;
                }
        }
        if (!pass) { for (int c = 0; c < nc; c++) s_cgs[c + 1] += s_cgs[c]; s_cgl = (int*)malloc(sizeof(int) * (size_t)(s_cgs[nc] + 1)); if (!s_cgl) { free(s_cgs); s_cgs = NULL; return; } }
        else free(fill);
    }
}
static uint16_t* s_edst;   /* edge index -> its destination node v */
static int in_corridor(float x, float y, float z)
{
    /* only the origin pile: elsewhere what stands in a road is real (el-train pillars, tunnel walls, docked ships) */
    if (!s_cgs || fabsf(x) > 150.0f || fabsf(z) > 150.0f) return 0;
    if (!s_edst) { s_edst = (uint16_t*)malloc(2 * (size_t)(s_ne + 1)); if (!s_edst) return 0;
        for (int v = 0; v < s_nn; v++) for (int e = s_rs[v]; e < s_rs[v + 1]; e++) s_edst[e] = (uint16_t)v; }
    const int cx = (int)((x - s_cgx0) / CG), cz = (int)((z - s_cgz0) / CG);
    if (cx < 0 || cz < 0 || cx >= s_cgnx || cz >= s_cgnz) return 0;
    const int c = cz * s_cgnx + cx;
    for (int k = s_cgs[c]; k < s_cgs[c + 1]; k++) {
        const int e = s_cgl[k], v = s_edst[e], u = s_rsrc[e];
        const float ux = s_fx[u], uz = s_fz[u], ex = s_fx[v] - ux, ez = s_fz[v] - uz, L2 = ex * ex + ez * ez;
        float t = 0; if (L2 > 1e-6f) { t = ((x - ux) * ex + (z - uz) * ez) / L2; if (t < 0) t = 0; if (t > 1) t = 1; }
        const float dx = x - (ux + ex * t), dz = z - (uz + ez * t), d2 = dx * dx + dz * dz;
        const float r = has_edge(v, u) ? 7.0f : 5.0f;   /* the whole roadway (two-way / one-way) */
        if (d2 < r * r && (!s_hasy || fabsf(s_fy[u] + (s_fy[v] - s_fy[u]) * t - y) < 2.5f)) return 1;
    }
    return 0;
}
/* the origin pile: a triangle any part of which (corners, edge middles, middle) lies inside a road corridor */
static int ghost_tri(const float* w, float ymin)
{
    if (fabsf(w[0]) > 150.0f && fabsf(w[3]) > 150.0f && fabsf(w[6]) > 150.0f) return 0;
    if (fabsf(w[2]) > 150.0f && fabsf(w[5]) > 150.0f && fabsf(w[8]) > 150.0f) return 0;
    if (in_corridor((w[0] + w[3] + w[6]) / 3.0f, ymin, (w[2] + w[5] + w[8]) / 3.0f)) return 1;
    for (int e = 0; e < 3; e++) { const float* a = w + 3 * e; const float* b = w + 3 * ((e + 1) % 3);
        if (in_corridor(a[0], ymin, a[2]) || in_corridor(0.5f * (a[0] + b[0]), ymin, 0.5f * (a[2] + b[2]))) return 1; }
    return 0;
}
/* a steep (wall) triangle the driver should respect: not one standing inside a road corridor */
static int real_wall(const float* w)
{
    const float e1[3] = { w[3] - w[0], w[4] - w[1], w[5] - w[2] }, e2[3] = { w[6] - w[0], w[7] - w[1], w[8] - w[2] };
    const float nx = e1[1] * e2[2] - e1[2] * e2[1], ny = e1[2] * e2[0] - e1[0] * e2[2], nz = e1[0] * e2[1] - e1[1] * e2[0];
    const float nl = sqrtf(nx * nx + ny * ny + nz * nz); if (nl < 1e-6f || fabsf(ny) / nl > 0.6f) return 0;
    const float ymin = fminf(w[1], fminf(w[4], w[7]));
    return !ghost_tri(w, ymin);
}
/* ---------------------------------------------------------------- the driver's wall index
 * Only the walls of assets/gf_collision.bin (steep triangles, the road-corridor junk left out) in an 8 m grid, read once on a
 * background thread. gf_rag_tris_near serves every triangle (floors too) and drops duplicates with a 512-entry list, which
 * near the junk pile at the origin made one lane check cost up to 18 ms; this index answers the same question in a fraction.
 * Until it is ready (or if the file is missing) the queries go to gf_rag_tris_near as before. */
#define WC 8.0f
static struct { float* tri; int ntri, nx, nz; float x0, z0; int* cs; int* idx; unsigned* stamp; unsigned st; } W;
static volatile LONG s_widx_state;   /* 0 not started, 1 building, 2 ready, 3 failed */
static DWORD WINAPI widx_build(LPVOID arg)
{
    (void)arg;
    FILE* f = open_asset("gf_collision.bin"); if (!f) { s_widx_state = 3; return 0; }
    char mg[8]; float hf[3]; int hi[4];
    if (fread(mg, 1, 8, f) != 8 || memcmp(mg, "GFCOL1", 6) || fread(hf, 4, 3, f) != 3 || fread(hi, 4, 4, f) != 4 || hi[2] <= 0) { fclose(f); s_widx_state = 3; return 0; }
    const int ntri = hi[2]; float* keep = (float*)malloc((size_t)ntri * 36); int nk = 0;
    float buf[9 * 4096]; int left = ntri;
    float mnx = 1e9f, mnz = 1e9f, mxx = -1e9f, mxz = -1e9f;
    while (keep && left > 0) {
        const int chunk = left < 4096 ? left : 4096; if (fread(buf, 36, (size_t)chunk, f) != (size_t)chunk) { free(keep); keep = NULL; break; }
        for (int i = 0; i < chunk; i++) {
            const float* w = buf + 9 * i; int bad = 0; for (int k = 0; k < 9; k++) if (!(w[k] == w[k]) || fabsf(w[k]) > 1e5f) bad = 1; if (bad) continue;
            if (!real_wall(w)) continue;   /* floors, ramps and the junk in the road corridors */
            memcpy(keep + 9 * nk, w, 36); nk++;
            for (int k = 0; k < 3; k++) { mnx = fminf(mnx, w[3 * k]); mxx = fmaxf(mxx, w[3 * k]); mnz = fminf(mnz, w[3 * k + 2]); mxz = fmaxf(mxz, w[3 * k + 2]); }
        }
        left -= chunk;
    }
    fclose(f);
    if (!keep || !nk) { free(keep); s_widx_state = 3; return 0; }
    W.x0 = mnx - WC; W.z0 = mnz - WC; W.nx = (int)((mxx - W.x0) / WC) + 2; W.nz = (int)((mxz - W.z0) / WC) + 2;
    int* cnt = (int*)calloc((size_t)W.nx * W.nz + 1, sizeof(int));
    if (!cnt) { free(keep); s_widx_state = 3; return 0; }
    #define WCELLS(w, BODY) { float a0 = fminf(w[0], fminf(w[3], w[6])), a1 = fmaxf(w[0], fmaxf(w[3], w[6])), b0 = fminf(w[2], fminf(w[5], w[8])), b1 = fmaxf(w[2], fmaxf(w[5], w[8])); \
        const int x0_ = (int)((a0 - W.x0) / WC), x1_ = (int)((a1 - W.x0) / WC), z0_ = (int)((b0 - W.z0) / WC), z1_ = (int)((b1 - W.z0) / WC); \
        for (int cz = z0_; cz <= z1_; cz++) for (int cx = x0_; cx <= x1_; cx++) if (cx >= 0 && cz >= 0 && cx < W.nx && cz < W.nz) { const int ci = cz * W.nx + cx; BODY; } }
    long long nidx = 0;
    for (int i = 0; i < nk; i++) { const float* w = keep + 9 * i; WCELLS(w, (cnt[ci + 1]++, nidx++)); }
    for (int i = 0; i < W.nx * W.nz; i++) cnt[i + 1] += cnt[i];
    int* idx = nidx < 0x7FFFFFFF ? (int*)malloc((size_t)nidx * sizeof(int) + 4) : NULL; int* fill = (int*)calloc((size_t)W.nx * W.nz, sizeof(int));
    unsigned* stamp = (unsigned*)calloc((size_t)nk, sizeof(unsigned));
    if (!idx || !fill || !stamp) { free(keep); free(cnt); free(idx); free(fill); free(stamp); s_widx_state = 3; return 0; }
    for (int i = 0; i < nk; i++) { const float* w = keep + 9 * i; WCELLS(w, idx[cnt[ci] + fill[ci]++] = i); }
    #undef WCELLS
    free(fill);
    W.tri = keep; W.ntri = nk; W.cs = cnt; W.idx = idx; W.stamp = stamp; W.st = 0;
    CLOG("[crewdrive] wall index: %d of %d triangles are walls, %d x %d cells of %.0f m\n", nk, ntri, W.nx, W.nz, WC);
    InterlockedExchange(&s_widx_state, 2);   /* published: the game thread may use W from now on */
    return 0;
}
/* wall triangles near (x,z) (a square of half side r) -- only from the game thread (the stamp is not shared) */
static int cd_tris_near(float x, float z, float r, float* out, int max)
{
    if (s_widx_state == 0) {
#ifdef CD_SIM
        s_widx_state = 1; widx_build(NULL);
#else
        s_widx_state = 1; { HANDLE th = CreateThread(NULL, 0, widx_build, NULL, 0, NULL); if (th) CloseHandle(th); else s_widx_state = 3; }
#endif
    }
    int n = 0;
    if (InterlockedCompareExchange(&s_widx_state, 0, 0) != 2) { __try { n = gf_rag_tris_near(x, z, r, out, max); } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; } return n; }
    if (++W.st == 0) { memset(W.stamp, 0, sizeof(unsigned) * (size_t)W.ntri); W.st = 1; }
    const int cx0 = (int)floorf((x - r - W.x0) / WC), cx1 = (int)floorf((x + r - W.x0) / WC), cz0 = (int)floorf((z - r - W.z0) / WC), cz1 = (int)floorf((z + r - W.z0) / WC);
    for (int cz = cz0 < 0 ? 0 : cz0; cz <= cz1 && cz < W.nz; cz++) for (int cx = cx0 < 0 ? 0 : cx0; cx <= cx1 && cx < W.nx; cx++) {
        const int ci = cz * W.nx + cx;
        for (int k = W.cs[ci]; k < W.cs[ci + 1] && n < max; k++) {
            const int t = W.idx[k]; if (W.stamp[t] == W.st) continue; W.stamp[t] = W.st;
            const float* w = W.tri + 9 * t;
            if (fmaxf(w[0], fmaxf(w[3], w[6])) < x - r || fminf(w[0], fminf(w[3], w[6])) > x + r || fmaxf(w[2], fmaxf(w[5], w[8])) < z - r || fminf(w[2], fminf(w[5], w[8])) > z + r) continue;
            memcpy(out + 9 * n, w, 36); n++; } }
#ifdef CD_SIM
    n += sim_props_near(x, z, r, out + 9 * n, max - n);   /* the simulator's market stalls */
#endif
    return n;
}
/* the first real wall along a ray (gf_rag_ray without the corridor ghosts) */
static float s_rbuf[9 * 3000];
static int cd_ray(const float* o, const float* d, float maxd, float* tout)
{
    const int n = cd_tris_near(o[0] + d[0] * maxd * 0.5f, o[2] + d[2] * maxd * 0.5f, maxd * 0.5f + 0.5f, s_rbuf, 3000);
    float best = maxd; int hit = 0;
    for (int i = 0; i < n; i++) {
        const float* w = s_rbuf + 9 * i;
        const float e1[3] = { w[3] - w[0], w[4] - w[1], w[5] - w[2] }, e2[3] = { w[6] - w[0], w[7] - w[1], w[8] - w[2] };
        const float p[3] = { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
        const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2]; if (fabsf(det) < 1e-9f) continue;
        const float inv = 1.0f / det, s[3] = { o[0] - w[0], o[1] - w[1], o[2] - w[2] };
        const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv; if (u < 0 || u > 1) continue;
        const float q[3] = { s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0] };
        const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv; if (v < 0 || u + v > 1) continue;
        const float t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv; if (t < 0 || t >= best) continue;
        if (!real_wall(w)) continue;
        best = t; hit = 1;
    }
    if (hit) *tout = best;
    return hit;
}
/* is the road u -> v (at fraction t) on our level? (a bridge vs the street under it) */
static int y_ok(int u, int v, float t, float y, float tol) { if (!s_hasy || !fok(y)) return 1; const float yy = s_fy[u] + (s_fy[v] - s_fy[u]) * t; return fabsf(yy - y) < tol; }
static int has_edge(int u, int v) { for (int e = s_rs[v]; e < s_rs[v + 1]; e++) if (s_rsrc[e] == u) return 1; return 0; }   /* u -> v */
static void hpush(uint32_t* n, uint16_t node, float key)
{
    uint32_t i = (*n)++;
    while (i) { uint32_t p = (i - 1) >> 1; if (s_hk[p] <= key) break; s_hn[i] = s_hn[p]; s_hk[i] = s_hk[p]; i = p; }
    s_hn[i] = node; s_hk[i] = key;
}
static void hpop(uint32_t* n)
{
    uint32_t m = --(*n), i = 0; uint16_t node = s_hn[m]; float key = s_hk[m];
    for (;;) { uint32_t l = 2 * i + 1; if (l >= m) break; if (l + 1 < m && s_hk[l + 1] < s_hk[l]) l++; if (s_hk[l] >= key) break; s_hn[i] = s_hn[l]; s_hk[i] = s_hk[l]; i = l; }
    s_hn[i] = node; s_hk[i] = key;
}
static void solve(int t, float* dist, uint16_t* next)   /* reverse Dijkstra: dist[u] = road metres u -> t, next[u] */
{
    uint32_t hs = 0, cap = (uint32_t)(s_nn + s_ne);
    for (int i = 0; i < s_nn; i++) { dist[i] = BIG; next[i] = 0xFFFF; }
    dist[t] = 0; hpush(&hs, (uint16_t)t, 0);
    while (hs) {
        uint16_t v = s_hn[0]; float dv = s_hk[0]; hpop(&hs);
        if (dv > dist[v]) continue;
        for (int e = s_rs[v]; e < s_rs[v + 1]; e++) {
            uint16_t u = s_rsrc[e]; const float dx = NX(u) - NX(v), dz = NZ(u) - NZ(v);
            const float nd = dv + sqrtf(dx * dx + dz * dz);
            if (nd < dist[u]) { dist[u] = nd; next[u] = v; if (hs < cap) hpush(&hs, u, nd); }
        }
    }
}

/* ================================================================ config / calibration */
static int s_on = -1, s_test = 0, s_key = 'B';
static float s_cruise = 22.2f, s_fast = 30.6f, s_easy = 13.9f;   /* 80 / 110 / 50 km/h */
static float s_off = 2.0f;           /* lane offset from the centre line (m) */
static int s_side = 1;               /* +1: right = (-fz, fx) of the travel direction; -1 mirrored (traffic check) */
static int s_steer_sign = -1, s_fwd_sign = 1, s_rev_mode = 0;   /* steering: measured in game (2026-11-21) */
static float s_steer_gain = 0.55f;   /* front wheel angle (rad) at full steering, estimated from the yaw rate */
static double s_side_sum; static int s_side_n;
/* the "Step on it" / "Take it easy" prompt while a crew member drives (rsx_ui_overlay.c cdr_build draws it above the GPS hint) */
volatile unsigned long long g_cdr_hint_t; volatile int g_cdr_key = 'N'; char g_cdr_hint[32];
static void cfg(void)
{
    if (s_on >= 0) return;
    const char* e = getenv("GF_CREWDRIVE"); s_on = !(e && *e == '0');
    if ((e = getenv("GF_CREWDRIVE_KEY")) && isalnum((unsigned char)e[0])) s_key = toupper((unsigned char)e[0]);
    if ((e = getenv("GF_CREWDRIVE_KMH")) && atof(e) > 10) s_cruise = (float)atof(e) / 3.6f;
    if ((e = getenv("GF_CREWDRIVE_SPEED_KEY")) && isalnum((unsigned char)e[0])) g_cdr_key = toupper((unsigned char)e[0]);
    if ((e = getenv("GF_CREWDRIVE_FAST_KMH")) && atof(e) > 10) s_fast = (float)atof(e) / 3.6f;
    if ((e = getenv("GF_CREWDRIVE_LOG")) && *e) s_logv = atoi(e);
    if ((e = getenv("GF_CREWDRIVE_TEST")) && *e) s_test = atoi(e);
    if ((e = getenv("GF_CREWDRIVE_SIDE")) && *e) s_side = atoi(e) < 0 ? -1 : 1;
    if (s_on) s_on = load_roads();
    CLOG("[crewdrive] crew driver %s (key %c, cruise %.0f km/h, fast %.0f km/h)%s\n", s_on ? "ON" : "off", s_key, s_cruise * 3.6, s_fast * 3.6, s_test ? " SELF TEST" : "");
}

/* ================================================================ input */
static int fg_ours(void) { HWND w = GetForegroundWindow(); DWORD pid = 0; if (w) GetWindowThreadProcessId(w, &pid); return pid == GetCurrentProcessId(); }
static int s_kprev[24];
static int kedge(int i, int vk) { const int k = fg_ours() && (GetAsyncKeyState(vk) & 0x8000); const int e = k && !s_kprev[i]; s_kprev[i] = k; return e; }
static unsigned s_pprev;
static unsigned pad_edges(void) { const unsigned b = g_pad_phys_buttons; const unsigned e = b & ~s_pprev; s_pprev = b; return fg_ours() ? e : 0; }

/* ================================================================ subtitles */
static void say(const char* who, const char* text, double secs)
{
    snprintf(g_gfd_sub_who, sizeof g_gfd_sub_who, "%s", who ? who : ""); snprintf(g_gfd_sub, sizeof g_gfd_sub, "%s", text ? text : "");
    g_gfd_sub_t1 = GetTickCount64() + (unsigned long long)(secs * 1000.0) + 400;
    CLOG("[crewdrive] says: %s: %s\n", who ? who : "", text ? text : "");
}
static void crew_name(uint32_t e, char* out, int cap)
{
    out[0] = 0;
    __try { if (e) gf_npc_name(e, out, cap); } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
    if (!out[0]) snprintf(out, (size_t)cap, "Crew");
}

/* ================================================================ voices
 * The crew members speak their own recorded lines from the disc (audiostreams/<dir>/dx_*.exa.snu), played on gf_dialogue.c's
 * voice channel (his mouth moves with it). His voice is the one of his voice bank (entity +0x1964 -> ".../dlg/<hash>"): the crew
 * banks hold ma12, ma15 or ma16 (fm06 = the woman's). These lines have no subtitle on the disc, so the table carries their words
 * and the subtitle shows what he really says; without a fitting line the text given to speak() is shown.
 * GF_CREWDRIVE_VOICE=0 off, GF_CREWDRIVE_VOICE_VOL (1.0). */
#ifdef CD_SIM
static double gf_dialogue_play_stream(const char* rel, float gain, uint32_t ent) { (void)rel; (void)gain; (void)ent; return 0.0; }
#else
extern double gf_dialogue_play_stream(const char* rel, float gain, uint32_t ent);   /* gf_dialogue.c */
#endif
enum { V_WHERE, V_OK, V_GETIN, V_ARRIVE, V_FAST, V_EASY, V_DANGER, V_BAIL, V_PULL, V_YOURS, V_NO, V_GO, V_N };
static const struct { const char* bank; int v; } k_vbank[] = {
    { "7EF0C205", 0 }, { "AE128B1B", 0 }, { "E345E176", 0 }, { "2391BD20", 1 }, { "2E1C3844", 1 }, { "A06958B0", 1 },
    { "2EDCDA20", 2 }, { "F74B6C02", 2 }, { "8A93A742", 2 }, { "A5E0AD31", 2 }, { "0045A40F", 3 } };
/* a disc line (ai 0) answers its moment (cat); an extra line (ai 1: a 16-bit mono WAV under audiostreams/) would answer its
 * exact sentence */
typedef struct { unsigned char cat, v, ai; const char* file; const char* text; } VLine;
static const VLine k_vl[] = {
/* VOICE TABLE BEGIN */
    { V_WHERE, 0, 0, "hat_ma12/dx_chat_ma12_crew_lwd_x0463.exa.snu", "Where we going to?" },
    { V_WHERE, 0, 0, "hat_ma12/dx_chat_ma12_crew_lwd_x0462.exa.snu", "What's the plan, boss?" },
    { V_WHERE, 1, 0, "hat_ma15/dx_chat_ma15_crew_lwd_x0571.exa.snu", "Where are we going?" },
    { V_WHERE, 1, 0, "hat_ma15/dx_chat_ma15_crew_lwd_x0570.exa.snu", "What's the plan, boss?" },
    { V_WHERE, 2, 0, "hat_ma16/dx_chat_ma16_crew_lwd_x0517.exa.snu", "Where we going?" },
    { V_WHERE, 2, 0, "hat_ma16/dx_chat_ma16_crew_lwd_x0516.exa.snu", "What's the plan, boss?" },
    { V_WHERE, 3, 0, "rew_fm06/dx_crew_fm06_cr07_lwd_z1135.exa.snu", "Where to, babe?" },
    { V_GO, 0, 0, "hat_ma12/dx_chat_ma12_crew_lwd_x0459.exa.snu", "All right boss, let's roll." },
    { V_GO, 0, 0, "hat_ma12/dx_chat_ma12_crew_lwd_x0460.exa.snu", "We're ready to go!" },
    { V_GO, 0, 0, "rew_ma12/dx_crew_ma12_cr05_lwd_x0763.exa.snu", "You got it! Let's roll!" },
    { V_GO, 1, 0, "hat_ma15/dx_chat_ma15_crew_lwd_x0567.exa.snu", "Alright, boss. Let's roll." },
    { V_GO, 1, 0, "hat_ma15/dx_chat_ma15_crew_lwd_x0569.exa.snu", "Okay, let's move." },
    { V_GO, 1, 0, "rew_ma15/dx_crew_ma15_cr05_lwd_x0819.exa.snu", "You got it. Andiamo, boss!" },
    { V_GO, 2, 0, "hat_ma16/dx_chat_ma16_crew_lwd_x0513.exa.snu", "Alright boss, let's roll." },
    { V_GO, 2, 0, "hat_ma16/dx_chat_ma16_crew_lwd_x0515.exa.snu", "Okay, let's move." },
    { V_GO, 2, 0, "rew_ma16/dx_crew_ma16_cr05_lwd_x0790.exa.snu", "It's a deal, boss. Let's hit the road." },
    { V_GO, 3, 0, "rew_fm06/dx_crew_fm06_cr07_lwd_z1136.exa.snu", "Let's go, babe. I'm ready to get busy." },
    { V_DANGER, 0, 0, "hat_ma12/dx_chat_ma12_crew_lwd_z0555.exa.snu", "We gotta get out of here!" },
    { V_DANGER, 0, 0, "hat_ma12/dx_chat_ma12_crew_lwd_z0567.exa.snu", "Let's get out of here!" },
    { V_DANGER, 0, 0, "hat_ma12/dx_chat_ma12_crew_lwd_x0484.exa.snu", "They're right on our tail!" },
    { V_DANGER, 1, 0, "hat_ma15/dx_chat_ma15_crew_lwd_z0608.exa.snu", "We gotta get out of here!" },
    { V_DANGER, 1, 0, "hat_ma15/dx_chat_ma15_crew_lwd_z0620.exa.snu", "Let's get out of here!" },
    { V_DANGER, 1, 0, "hat_ma15/dx_chat_ma15_crew_lwd_x0592.exa.snu", "They're on our tail!" },
    { V_DANGER, 2, 0, "hat_ma16/dx_chat_ma16_crew_lwd_z0584.exa.snu", "We gotta get out of here!" },
    { V_DANGER, 2, 0, "hat_ma16/dx_chat_ma16_crew_lwd_z0596.exa.snu", "Let's get out of here!" },
    { V_DANGER, 2, 0, "hat_ma16/dx_chat_ma16_crew_lwd_x0538.exa.snu", "They're right on our tail!" },
    { V_BAIL, 0, 0, "hat_ma12/dx_chat_ma12_crew_lwd_z0569.exa.snu", "It's gonna blow!" },
    { V_BAIL, 0, 0, "hat_ma12/dx_chat_ma12_crew_lwd_z0571.exa.snu", "It's gonna explode!" },
    { V_BAIL, 1, 0, "hat_ma15/dx_chat_ma15_crew_lwd_z0622.exa.snu", "It's gonna blow!" },
    { V_BAIL, 1, 0, "hat_ma15/dx_chat_ma15_crew_lwd_z0624.exa.snu", "It's gonna explode!" },
    { V_BAIL, 2, 0, "hat_ma16/dx_chat_ma16_crew_lwd_z0598.exa.snu", "It's gonna blow!" },
    { V_BAIL, 2, 0, "hat_ma16/dx_chat_ma16_crew_lwd_z0600.exa.snu", "It's gonna explode!" },
/* VOICE TABLE END */
};
static int s_voice = -1; static float s_voice_vol = 1.0f;
static int voice_of(uint32_t e)   /* 0 ma12, 1 ma15, 2 ma16, 3 fm06 */
{
    static uint32_t ce; static int cv = -1;
    if (e == ce && cv >= 0) return cv;
    int v = -1; char b[9] = "-";
#ifndef CD_SIM
    __try {
        const uint32_t vp = ok(e) ? rd32(e + 0x1964) : 0;
        if (ok(vp)) { int L = 0; while (L < 120 && vm_base[vp + (uint32_t)L]) L++;
            if (L >= 8) { for (int k = 0; k < 8; k++) b[k] = (char)toupper(vm_base[vp + (uint32_t)(L - 8 + k)]); b[8] = 0;
                for (int i = 0; i < (int)(sizeof k_vbank / sizeof k_vbank[0]); i++) if (!strcmp(b, k_vbank[i].bank)) v = k_vbank[i].v; } }
        if (v < 0 && ok(e) && vm_base[e + 0x1817] == 3) v = 3;
    } __except (EXCEPTION_EXECUTE_HANDLER) { v = -1; }
#endif
    if (v < 0) v = (int)((e >> 4) % 3u);   /* not a crew bank: one of the three crew voices, always the same for him */
    CLOG("[crewdrive] voice of %08X: bank %s -> %s\n", e, b, v == 0 ? "ma12" : v == 1 ? "ma15" : v == 2 ? "ma16" : "fm06");
    ce = e; cv = v; return v;
}
/* e says a line for moment cat in his own voice (subtitle = its words); text (NULL = say nothing) is shown when there is no line */
static void speak(uint32_t e, int cat, const char* text, double secs)
{
    char nm[48]; crew_name(e, nm, sizeof nm);
    if (s_voice < 0) { const char* x = getenv("GF_CREWDRIVE_VOICE"); s_voice = !(x && *x == '0');
        x = getenv("GF_CREWDRIVE_VOICE_VOL"); if (x && atof(x) > 0.0) s_voice_vol = (float)atof(x); }
    if (s_voice && e && cat >= 0 && cat < V_N) {
        const int v = voice_of(e); int cand[32], nc = 0;
        for (int i = 0; i < (int)(sizeof k_vl / sizeof k_vl[0]) && nc < 32; i++) { const VLine* L = &k_vl[i];
            if (L->v == v && (L->ai ? (text && !strcmp(L->text, text)) : L->cat == cat)) cand[nc++] = i; }
        static int last[V_N];
        if (nc) { int k = (int)((GetTickCount64() / 7u) % (unsigned)nc); if (nc > 1 && cand[k] + 1 == last[cat]) k = (k + 1) % nc;
            const VLine* L = &k_vl[cand[k]]; last[cat] = cand[k] + 1;
            const double l = gf_dialogue_play_stream(L->file, s_voice_vol, e);
            if (l > 0.0) { say(nm, L->text, l + 0.8); return; } }
    }
    if (text) say(nm, text, secs);
}

/* ================================================================ markers */
enum { K_CUSTOM, K_BLUE, K_YELLOW, K_PINK, K_N };
static const char* const k_kind_label[K_N] = { "Custom waypoint", "Blue checkpoint", "Yellow checkpoint", "Pink checkpoint" };
static const char* const k_kind_say[K_N] = { "your waypoint", "the blue one", "the yellow one", "the pink one" };
static const char* const k_kind_sayc[K_N] = { "Your waypoint", "The blue one", "The yellow one", "The pink one" };
/* nearest marker of a kind (checkpoint first, its giver as fallback); 1 = found */
static int find_marker(int kind, const float* from, float* out, int* slot)
{
    const uint32_t mgr = g_gps_mgr; if (!ok(mgr)) return 0;
    uint32_t t1 = 0, t2 = 0;
    switch (kind) { case K_CUSTOM: t1 = g_gps_ttype[0]; break; case K_BLUE: t1 = g_gps_ttype[1]; t2 = g_gps_ttype[5]; break;
                    case K_YELLOW: t1 = g_gps_ttype[2]; t2 = g_gps_ttype[6]; break; case K_PINK: t1 = g_gps_ttype[3]; t2 = g_gps_ttype[7]; break; }
    for (int pass = 0; pass < 2; pass++) {
        const uint32_t want = pass ? t2 : t1; if (!want) continue;
        float bd = BIG; int bs = -1;
        for (int s = 0; s < 200; s++) {
            const uint32_t e = mgr + 0x44u + 0x2Cu * (uint32_t)s;
            if (!(vm_base[e + 0x10] & 1) || rd32(e + 0xC) != want) continue;
            const float p[3] = { rdf(e), rdf(e + 4), rdf(e + 8) }; if (!fok(p[0]) || !fok(p[2])) continue;
            const float d = from ? dist2d(p, from) : 0.0f;
            if (d < bd) { bd = d; bs = s; out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; }
        }
        if (bs >= 0) { if (slot) *slot = bs; return 1; }
    }
    return 0;
}

/* ================================================================ route */
static float s_l_act;         /* where the car really is, metres right of the path line */
#define PMAX 400                    /* path samples, every PSTEP metres */
#define PSTEP 2.0f
static float s_px[PMAX], s_pz[PMAX], s_pv[PMAX]; static int s_np; static int s_pnode[PMAX];
static float s_end_x, s_end_z; static int s_path_ends;   /* the stop point is on this path */
static int s_tA = -1, s_tB = -1; static float s_tx, s_tz, s_tpx, s_tpz;   /* target edge A -> B (projection tp), solved */
static float s_edge_t;                                    /* 0..1 along A -> B */
static int s_have_target;
static void lane_right(float dx, float dz, float* rx, float* rz) { *rx = -dz * (float)s_side; *rz = dx * (float)s_side; }
static float edge_off(int u, int v) { return has_edge(v, u) ? s_off : 0.0f; }   /* one-way roads: the centre line */
/* off-road connectors (cached): start (car -> road) and end (road stop point -> a marker off the road) */
static struct { float x[64], z[64]; int n; float sx, sz, gx, gz; double t; int ok; } s_cs, s_ce;   /* s.. = where it starts */
static void set_target_pos(float x, float y, float z)
{
    /* nearest road segment (either direction) to the marker, on its level (a second pass without the height if none) */
    float bd = BIG; int ba = -1, bb = -1; float bt = 0;
    for (int pass = 0; pass < 2 && ba < 0; pass++)
    for (int v = 0; v < s_nn; v++) for (int e = s_rs[v]; e < s_rs[v + 1]; e++) {
        const int u = s_rsrc[e]; if (!on_main(u, v)) continue; const float ux = NX(u), uz = NZ(u), ex = NX(v) - ux, ez = NZ(v) - uz, L2 = ex * ex + ez * ez;
        float t = 0; if (L2 > 0) { t = ((x - ux) * ex + (z - uz) * ez) / L2; if (t < 0) t = 0; if (t > 1) t = 1; }
        if (pass == 0 && !y_ok(u, v, t, y, 6.0f)) continue;
        const float cx = ux + ex * t, cz = uz + ez * t, d = (x - cx) * (x - cx) + (z - cz) * (z - cz);
        if (d < bd) { bd = d; ba = u; bb = v; bt = t; }
    }
    if (ba < 0) { s_have_target = 0; return; }
    s_tA = ba; s_tB = bb; s_edge_t = bt; s_tx = x; s_tz = z; s_cs.ok = 0; s_ce.ok = 0;
    s_tpx = NX(ba) + (NX(bb) - NX(ba)) * bt; s_tpz = NZ(ba) + (NZ(bb) - NZ(ba)) * bt;
    solve(ba, s_distA, s_nextA);   /* arrive at A, then drive A -> B */
    solve(bb, s_distB, s_nextB);   /* or arrive at B and drive B -> A (if that way exists) */
    s_have_target = 1;
    CLOG("[crewdrive] target (%.1f, %.1f): road %d -> %d at %.0f%%, %.1f m from the marker\n", x, z, ba, bb, bt * 100.0f, sqrtf(bd));
}
/* ---------------------------------------------------------------- off-road: a walkable-for-cars grid from the world collision
 * Where the road graph has nothing (the Corleone compound, car parks, plazas) the walls / buildings / posts of
 * assets/gf_collision.bin (gf_rag_tris_near) are drawn into a 1.5 m grid around start and goal, blown up by one cell
 * (half a car), and A* finds the way; the path is then pulled straight where the line of sight is clear. */
#define GN 220
#define GC 1.5f
#define MAXT 16000
static uint8_t s_gb[GN * GN], s_gi[GN * GN], s_gcl[GN * GN], s_gnear[GN * GN];
static float s_gg[GN * GN]; static int s_gfrom[GN * GN]; static int s_gheap[GN * GN * 2]; static float s_gkey[GN * GN * 2];
static float s_gx0, s_gz0; static float* s_tbuf;
static int g_cell(float x, float z, int* cx, int* cz) { *cx = (int)floorf((x - s_gx0) / GC); *cz = (int)floorf((z - s_gz0) / GC); return *cx >= 0 && *cz >= 0 && *cx < GN && *cz < GN; }
static void g_mark(float x, float z) { int cx, cz; if (g_cell(x, z, &cx, &cz)) s_gb[cz * GN + cx] = 1; }
static int grid_build(float cx, float cz, float y, float r)   /* r: half the side of the square that matters (cells outside count as walls) */
{
    if (!s_tbuf) { s_tbuf = (float*)malloc(sizeof(float) * 9 * MAXT); if (!s_tbuf) return 0; }
    s_gx0 = cx - GN * GC * 0.5f; s_gz0 = cz - GN * GC * 0.5f;
    memset(s_gb, 0, sizeof s_gb);
    if (r > GN * GC * 0.5f) r = GN * GC * 0.5f;
    const int n = cd_tris_near(cx, cz, r, s_tbuf, MAXT);
    int used = 0;
    for (int i = 0; i < n; i++) {
        const float* w = s_tbuf + 9 * i;
        const float e1[3] = { w[3] - w[0], w[4] - w[1], w[5] - w[2] }, e2[3] = { w[6] - w[0], w[7] - w[1], w[8] - w[2] };
        const float nx = e1[1] * e2[2] - e1[2] * e2[1], ny = e1[2] * e2[0] - e1[0] * e2[2], nz = e1[0] * e2[1] - e1[1] * e2[0];
        const float nl = sqrtf(nx * nx + ny * ny + nz * nz); if (nl < 1e-6f || fabsf(ny) / nl > 0.6f) continue;   /* ground / ramps */
        const float ymin = fminf(w[1], fminf(w[4], w[7])), ymax = fmaxf(w[1], fmaxf(w[4], w[7]));
        if (ymax - ymin < 0.45f || ymax < y - 2.5f || ymin > y + 4.0f) continue;   /* kerbs, things far below / overhead */
        if (ghost_tri(w, ymin)) continue;   /* ghosts in the road */
        used++;
        for (int e = 0; e < 3; e++) {
            const float* a = w + 3 * e; const float* b = w + 3 * ((e + 1) % 3);
            const float L = sqrtf((b[0] - a[0]) * (b[0] - a[0]) + (b[2] - a[2]) * (b[2] - a[2])); const int st = (int)(L / 0.5f) + 1;
            for (int k = 0; k <= st; k++) { const float f = (float)k / (float)st; g_mark(a[0] + (b[0] - a[0]) * f, a[2] + (b[2] - a[2]) * f); }
        }
    }
    { const int lo = (int)((GN * GC * 0.5f - r) / GC), hi = GN - 1 - lo;   /* outside the square: closed */
      if (lo > 0) for (int z = 0; z < GN; z++) for (int x = 0; x < GN; x++) if (x < lo || z < lo || x > hi || z > hi) s_gb[z * GN + x] = 1; }
    for (int z = 0; z < GN; z++) for (int x = 0; x < GN; x++) {   /* half a car around every wall cell */
        uint8_t v = 0;
        for (int dz = -1; dz <= 1 && !v; dz++) for (int dx = -1; dx <= 1; dx++) { const int xx = x + dx, zz = z + dz; if (xx >= 0 && zz >= 0 && xx < GN && zz < GN && s_gb[zz * GN + xx]) { v = 1; break; } }
        s_gi[z * GN + x] = v;
    }
    for (int z = 0; z < GN; z++) for (int x = 0; x < GN; x++) {   /* 1 / 2 cells from a wall: costs more (drive in the middle) */
        uint8_t c = 0; for (int dz = -2; dz <= 2; dz++) for (int dx = -2; dx <= 2; dx++) { const int xx = x + dx, zz = z + dz;
            if (xx >= 0 && zz >= 0 && xx < GN && zz < GN && s_gi[zz * GN + xx]) { const uint8_t w = (abs(dx) <= 1 && abs(dz) <= 1) ? 2 : 1; if (w > c) c = w; } }
        s_gcl[z * GN + x] = c; }
    memcpy(s_gnear, s_gcl, sizeof s_gnear);
    if (s_logv) CLOG("[crewdrive] off-road grid at (%.0f, %.0f): %d triangles, %d walls\n", cx, cz, n, used);
    return 1;
}
static int g_free(int x, int z) { return x >= 0 && z >= 0 && x < GN && z < GN && !s_gi[z * GN + x]; }
static int g_los(int x0, int z0, int x1, int z1)
{
    const int n = (abs(x1 - x0) > abs(z1 - z0) ? abs(x1 - x0) : abs(z1 - z0)) * 2 + 1;
    for (int k = 0; k <= n; k++) { const float f = (float)k / (float)n; if (!g_free((int)lrintf(x0 + (x1 - x0) * f), (int)lrintf(z0 + (z1 - z0) * f))) return 0; }
    return 1;
}
static void g_unblock(int cx, int cz) { for (int dz = -2; dz <= 2; dz++) for (int dx = -2; dx <= 2; dx++) { const int x = cx + dx, z = cz + dz; if (x >= 0 && z >= 0 && x < GN && z < GN) s_gi[z * GN + x] = 0; } }
/* A* from (sx,sz) to (tx,tz) on the grid; out = waypoints after the start (world), returns the count, -1 = no way */
static int grid_path(float sx, float sz, float tx, float tz, float* ox, float* oz, int maxo)
{
    int scx, scz, tcx, tcz;
    if (!g_cell(sx, sz, &scx, &scz) || !g_cell(tx, tz, &tcx, &tcz)) return -1;
    if (s_gi[scz * GN + scx]) {   /* our car's spot touches a wall: start from the nearest free cell (never erase walls) */
        int best = -1, bd = 1 << 30;
        for (int dz = -3; dz <= 3; dz++) for (int dx = -3; dx <= 3; dx++) if (g_free(scx + dx, scz + dz) && dx * dx + dz * dz < bd) { bd = dx * dx + dz * dz; best = (scz + dz) * GN + scx + dx; }
        if (best < 0) g_unblock(scx, scz); else { scx = best % GN; scz = best / GN; }
    }
    if (s_gi[tcz * GN + tcx]) {                            /* goal inside a wall: the nearest free cell */
        int best = -1, bd = 1 << 30;
        for (int dz = -8; dz <= 8; dz++) for (int dx = -8; dx <= 8; dx++) if (g_free(tcx + dx, tcz + dz) && dx * dx + dz * dz < bd) { bd = dx * dx + dz * dz; best = (tcz + dz) * GN + tcx + dx; }
        if (best < 0) return -1; tcx = best % GN; tcz = best / GN;
    }
    for (int i = 0; i < GN * GN; i++) { s_gg[i] = BIG; s_gcl[i] = 0; s_gfrom[i] = -1; }
    int hn = 0; const int s0 = scz * GN + scx, goal = tcz * GN + tcx;
    s_gg[s0] = 0;
    #define HPUSH(node, key) do { int i_ = hn++; while (i_) { int p_ = (i_ - 1) >> 1; if (s_gkey[p_] <= (key)) break; s_gheap[i_] = s_gheap[p_]; s_gkey[i_] = s_gkey[p_]; i_ = p_; } s_gheap[i_] = (node); s_gkey[i_] = (key); } while (0)
    HPUSH(s0, 0.0f);
    int found = 0, pops = 0;
    while (hn > 0 && pops < GN * GN * 2) {
        const int cur = s_gheap[0]; pops++;
        { const int m = --hn; const int nd = s_gheap[m]; const float k = s_gkey[m]; int i = 0;
          for (;;) { int l = 2 * i + 1; if (l >= m) break; if (l + 1 < m && s_gkey[l + 1] < s_gkey[l]) l++; if (s_gkey[l] >= k) break; s_gheap[i] = s_gheap[l]; s_gkey[i] = s_gkey[l]; i = l; }
          s_gheap[i] = nd; s_gkey[i] = k; }
        if (s_gcl[cur]) continue; s_gcl[cur] = 1;
        if (cur == goal) { found = 1; break; }
        const int cx = cur % GN, cz = cur / GN;
        for (int dz = -1; dz <= 1; dz++) for (int dx = -1; dx <= 1; dx++) {
            if (!dx && !dz) continue; const int nx = cx + dx, nz = cz + dz; if (!g_free(nx, nz)) continue;
            if (dx && dz && (!g_free(cx + dx, cz) || !g_free(cx, cz + dz))) continue;   /* no corner cutting */
            const int ni = nz * GN + nx; const float ng = s_gg[cur] + (dx && dz ? 1.4142f : 1.0f) * (1.0f + 0.8f * (float)s_gnear[ni]);
            if (ng < s_gg[ni] && hn < GN * GN * 2 - 1) { s_gg[ni] = ng; s_gfrom[ni] = cur;
                const int ddx = abs(nx - tcx), ddz = abs(nz - tcz); const float h = (float)(ddx > ddz ? ddx : ddz) + 0.4142f * (float)(ddx < ddz ? ddx : ddz);
                HPUSH(ni, ng + h); }
        }
    }
    #undef HPUSH
    if (!found) return -1;
    static int chain[GN * 4]; int nc = 0;
    for (int c = goal; c >= 0 && nc < GN * 4; c = s_gfrom[c]) chain[nc++] = c;
    /* reversed: chain[nc-1] = start. Pull the string: from each anchor, the farthest cell still in sight */
    int no = 0, a = nc - 1;
    while (a > 0 && no < maxo) {
        int b = a - 1;
        for (int k = 0; k < a; k++) { if (g_los(chain[a] % GN, chain[a] / GN, chain[k] % GN, chain[k] / GN)) { b = k; break; } }
        ox[no] = s_gx0 + ((float)(chain[b] % GN) + 0.5f) * GC; oz[no] = s_gz0 + ((float)(chain[b] / GN) + 0.5f) * GC; no++;
        a = b;
    }
    return no;
}
/* cached connectors: start (car -> road) and end (road stop point -> marker off the road) */
static double perf_ms(void) { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return (double)c.QuadPart * 1000.0 / (double)f.QuadPart; }
static int connector(float sx, float sz, float tx, float tz, float y, float* ox, float* oz, int maxo)
{
    const double p0 = perf_ms();
    const float mx = (sx + tx) * 0.5f, mz = (sz + tz) * 0.5f;
    if (fabsf(sx - tx) > GN * GC - 20.0f || fabsf(sz - tz) > GN * GC - 20.0f) return -1;
    if (!grid_build(mx, mz, y, fmaxf(fabsf(sx - tx), fabsf(sz - tz)) * 0.5f + 45.0f)) return -1;
    const double p1 = perf_ms();
    const int n = grid_path(sx, sz, tx, tz, ox, oz, maxo);
    { const double p2 = perf_ms(); if (p2 - p0 > 4.0) CLOG("[crewdrive] slow off-road way: grid %.1f ms, search %.1f ms\n", p1 - p0, p2 - p1); }
    CLOG("[crewdrive] off-road way (%.0f, %.0f) -> (%.0f, %.0f): %s (%d turns)\n", sx, sz, tx, tz, n >= 0 ? "found" : "NONE, straight", n);
    return n;
}

/* builds the raw lane polyline from the car (pos, heading) to the stop point, resampled every PSTEP m */
static float s_rawx[512], s_rawz[512]; static uint8_t s_rawoff[512]; static int s_nraw; static uint8_t s_poff[PMAX];
static int s_rawflag;
static void raw_add(float x, float z) { if (s_nraw < 512) { s_rawx[s_nraw] = x; s_rawz[s_nraw] = z; s_rawoff[s_nraw] = (uint8_t)s_rawflag; s_nraw++; } }
static int s_prev_u = -1, s_prev_v = -1;
/* a lane point: the road line (x,y,z) moved sideways by (ox,oz) -- but never closer than half a car + 0.4 m to a wall
 * (the road map calls some one-lane streets and ramps two-way: the right lane would be in the kerb wall) */
static void raw_lane(float x, float y, float z, float ox, float oz)
{
    const float L = sqrtf(ox * ox + oz * oz);
    if (L > 0.1f) { const float o3[3] = { x, y + 0.75f, z }, d3[3] = { ox / L, 0.0f, oz / L }; float t = 0;
        if (cd_ray(o3, d3, L + 1.35f, &t)) { const float k = fmaxf(0.0f, t - 1.35f) / L; ox *= k; oz *= k; } }
    raw_add(x + ox, z + oz);
}
#define OFFROAD_LAT 9.0f
static int build_path(const float* pos, float hx, float hz, float* lat_err)
{
    s_np = 0; s_nraw = 0; s_path_ends = 0; s_rawflag = 0;
    if (!s_have_target) return 0;
    const double now = now_s();
    const float LAB = sqrtf((NX(s_tB) - NX(s_tA)) * (NX(s_tB) - NX(s_tA)) + (NZ(s_tB) - NZ(s_tA)) * (NZ(s_tB) - NZ(s_tA)));
    const int revok = has_edge(s_tB, s_tA);
    /* best start segment u -> v: route cost + lateral distance + heading (far ones: reached off-road) */
    float best = BIG; int bu = -1, bv = -1, bvia = 0; float bcx = pos[0], bcz = pos[2], blat = 0;
    float minlat = BIG;   /* how far is the nearest lane at all? on the road we never leave it for a shortcut */
    for (int v = 0; v < s_nn; v++) for (int e = s_rs[v]; e < s_rs[v + 1]; e++) {
        const int u = s_rsrc[e]; if (!on_main(u, v)) continue; const float ux = NX(u), uz = NZ(u), ex = NX(v) - ux, ez = NZ(v) - uz, L = sqrtf(ex * ex + ez * ez); if (L < 0.5f) continue;
        if (fabsf(ux - pos[0]) > 300.0f + L || fabsf(uz - pos[2]) > 300.0f + L) continue;
        const float dx = ex / L, dz = ez / L; float rx, rz; lane_right(dx, dz, &rx, &rz); const float off = edge_off(u, v);
        const float lx = ux + rx * off, lz = uz + rz * off; float t = (pos[0] - lx) * dx + (pos[2] - lz) * dz; if (t < 0) t = 0; if (t > L) t = L; if (!y_ok(u, v, t / L, pos[1], 3.5f)) continue;
        const float cx = lx + dx * t, cz = lz + dz * t, lat = sqrtf((pos[0] - cx) * (pos[0] - cx) + (pos[2] - cz) * (pos[2] - cz)); if (lat < minlat) minlat = lat; }
    const int onroad = minlat <= OFFROAD_LAT; const float latmax = onroad ? OFFROAD_LAT + 3.0f : minlat + 25.0f;
    for (int v = 0; v < s_nn; v++) for (int e = s_rs[v]; e < s_rs[v + 1]; e++) {
        const int u = s_rsrc[e]; if (!on_main(u, v)) continue;
        const float ux = NX(u), uz = NZ(u), ex = NX(v) - ux, ez = NZ(v) - uz, L = sqrtf(ex * ex + ez * ez); if (L < 0.5f) continue;
        if (fabsf(ux - pos[0]) > 300.0f + L || fabsf(uz - pos[2]) > 300.0f + L) continue;
        const float dx = ex / L, dz = ez / L; float rx, rz; lane_right(dx, dz, &rx, &rz); const float off = edge_off(u, v);
        const float lx = ux + rx * off, lz = uz + rz * off;
        float t = (pos[0] - lx) * dx + (pos[2] - lz) * dz; if (t < 0) t = 0; if (t > L) t = L; if (!y_ok(u, v, t / L, pos[1], 3.5f)) continue;
        const float cx = lx + dx * t, cz = lz + dz * t, lat = sqrtf((pos[0] - cx) * (pos[0] - cx) + (pos[2] - cz) * (pos[2] - cz));
        if (lat > latmax) continue;
        const float rest = L - t, dot = dx * hx + dz * hz;
        float cA = BIG, cB = BIG;
        if (u == s_tA && v == s_tB && t <= s_edge_t * L + 0.5f) cA = s_edge_t * L - t;
        else if (s_distA[v] < BIG) cA = rest + s_distA[v] + s_edge_t * LAB;
        if (revok) { if (u == s_tB && v == s_tA && t <= (1.0f - s_edge_t) * L + 0.5f) cB = (1.0f - s_edge_t) * L - t;
                     else if (s_distB[v] < BIG) cB = rest + s_distB[v] + (1.0f - s_edge_t) * LAB; }
        const int via = cB < cA; float c = via ? cB : cA; if (c >= BIG) continue;
        c += lat * (lat < OFFROAD_LAT ? 3.0f : 2.0f) + (lat < OFFROAD_LAT ? (dot < 0.0f ? 60.0f * -dot + 25.0f : (1.0f - dot) * 8.0f) : 0.0f);
        if (u == s_prev_u && v == s_prev_v) c -= 4.0f;
        if (c < best) { best = c; bu = u; bv = v; bvia = via; bcx = cx; bcz = cz; blat = lat; }
    }
    if (lat_err) *lat_err = blat;
#ifdef CD_SIM
    if (getenv("CD_DBG_PATH")) CLOG("[dbg] start edge %d->%d via %d cost %.0f lat %.1f onroad %d minlat %.1f mdist %.0f end (%.0f, %.0f) ce %d/%d\n", bu, bv, bvia, best, blat, onroad, minlat, sqrtf((s_tx - pos[0]) * (s_tx - pos[0]) + (s_tz - pos[2]) * (s_tz - pos[2])), s_end_x, s_end_z, s_ce.ok, s_ce.n);
#endif
#ifdef CD_SIM
    { static double tdb; if ((bu < 0 || (getenv("CD_DBG_PATH") && minlat > OFFROAD_LAT)) && now - tdb > 5.0) { tdb = now; CLOG("[dbg] no start road: pos y %.1f minlat %.1f latmax %.1f distA %s\n", pos[1], minlat, latmax, "");
        for (int v = 0; v < s_nn; v++) for (int e = s_rs[v]; e < s_rs[v + 1]; e++) { const int u = s_rsrc[e];
            const float ux = NX(u), uz = NZ(u), ex = NX(v) - ux, ez = NZ(v) - uz, L = sqrtf(ex * ex + ez * ez); if (L < 0.5f) continue;
            float t = ((pos[0] - ux) * ex + (pos[2] - uz) * ez) / (L * L); if (t < 0) t = 0; if (t > 1) t = 1;
            const float cx = ux + ex * t, cz = uz + ez * t, d = sqrtf((pos[0] - cx) * (pos[0] - cx) + (pos[2] - cz) * (pos[2] - cz));
            if (d < 20.0f) CLOG("[dbg]   edge %d->%d main %d d %.1f y %.1f..%.1f distA %.0f distB %.0f\n", u, v, on_main(u, v), d, s_fy[u], s_fy[v], s_distA[v] < BIG ? s_distA[v] : -1.0f, s_distB[v] < BIG ? s_distB[v] : -1.0f); } } }
#endif
    const float mdist = sqrtf((s_tx - pos[0]) * (s_tx - pos[0]) + (s_tz - pos[2]) * (s_tz - pos[2]));
    /* already on the last off-road piece (road -> a marker in a yard): keep going, do not turn back to the road */
    int on_ce = 0; if (!onroad && s_ce.ok && s_ce.n > 0 && fabsf(s_ce.gx - s_tx) + fabsf(s_ce.gz - s_tz) < 3.0f)
        for (int i = 0; i < s_ce.n; i++) {   /* near its polyline (from where it leaves the road) */
            const float ax = i ? s_ce.x[i - 1] : s_ce.sx, az = i ? s_ce.z[i - 1] : s_ce.sz, ex = s_ce.x[i] - ax, ez = s_ce.z[i] - az, L2 = ex * ex + ez * ez;
            float t = L2 > 1e-3f ? ((pos[0] - ax) * ex + (pos[2] - az) * ez) / L2 : 0.0f; if (t < 0) t = 0; if (t > 1) t = 1;
            const float dx = ax + ex * t - pos[0], dz = az + ez * t - pos[2]; if (dx * dx + dz * dz < 12.0f * 12.0f) on_ce = 1; }
    if (bu < 0 || (!onroad && mdist < 120.0f && (mdist < blat || on_ce))) {
        /* no road near (or the marker is nearer than any road): straight there, off-road */
        if (!s_ce.ok || fabsf(s_ce.gx - s_tx) + fabsf(s_ce.gz - s_tz) > 3.0f || now - s_ce.t > 4.0) {
            s_ce.n = connector(pos[0], pos[2], s_tx, s_tz, pos[1], s_ce.x, s_ce.z, 64); s_ce.sx = pos[0]; s_ce.sz = pos[2]; s_ce.gx = s_tx; s_ce.gz = s_tz; s_ce.t = now; s_ce.ok = 1; }
        s_rawflag = 1; raw_add(pos[0], pos[2]);
        if (s_ce.n > 0) for (int i = 0; i < s_ce.n; i++) raw_add(s_ce.x[i], s_ce.z[i]);
        s_end_x = s_tx; s_end_z = s_tz; raw_add(s_end_x, s_end_z);
        goto resample;
    }
    s_prev_u = bu; s_prev_v = bv;
    {
    const int tFirst = bvia ? s_tB : s_tA, tSecond = bvia ? s_tA : s_tB;
    const float tfrac = bvia ? 1.0f - s_edge_t : s_edge_t;
    const uint16_t* next = bvia ? s_nextB : s_nextA;
    int chain[256], nc = 0; chain[nc++] = bu;
    const int on_target = (bu == tFirst && bv == tSecond);
    if (!on_target) { int n = bv; chain[nc++] = n; while (n != tFirst && nc < 250) { n = next[n]; if (n == 0xFFFF) break; chain[nc++] = n; } }
    if (blat > OFFROAD_LAT) {   /* off the road: find the way onto it */
        if (!s_cs.ok || fabsf(s_cs.gx - bcx) + fabsf(s_cs.gz - bcz) > 6.0f || now - s_cs.t > 4.0) {
            s_cs.n = connector(pos[0], pos[2], bcx, bcz, pos[1], s_cs.x, s_cs.z, 64); s_cs.gx = bcx; s_cs.gz = bcz; s_cs.t = now; s_cs.ok = 1; }
        s_rawflag = 1; raw_add(pos[0], pos[2]);
        if (s_cs.n > 0) { int k0 = 0; float bd = BIG;   /* skip the turns already behind us */
            for (int i = 0; i < s_cs.n; i++) { const float d = (s_cs.x[i] - pos[0]) * (s_cs.x[i] - pos[0]) + (s_cs.z[i] - pos[2]) * (s_cs.z[i] - pos[2]); if (d < bd) { bd = d; k0 = i; } }
            if (bd < 9.0f && k0 + 1 < s_cs.n) k0++;
            for (int i = k0; i < s_cs.n; i++) raw_add(s_cs.x[i], s_cs.z[i]); }
        s_rawflag = 0;
    }
    raw_add(bcx, bcz);
    for (int i = 1; i < nc; i++) {
        const int a = chain[i - 1], b = chain[i];
        float d1x = NX(b) - NX(a), d1z = NZ(b) - NZ(a); const float L1 = sqrtf(d1x * d1x + d1z * d1z); if (L1 < 0.3f) continue; d1x /= L1; d1z /= L1;
        float r1x, r1z; lane_right(d1x, d1z, &r1x, &r1z); const float o1 = edge_off(a, b);
        int c = -1; float o2 = o1, d2x = d1x, d2z = d1z;
        if (i + 1 < nc) c = chain[i + 1]; else if (b == tFirst) c = tSecond;
        if (c >= 0) { d2x = NX(c) - NX(b); d2z = NZ(c) - NZ(b); const float L2 = sqrtf(d2x * d2x + d2z * d2z); if (L2 > 0.3f) { d2x /= L2; d2z /= L2; o2 = edge_off(b, c); } else { d2x = d1x; d2z = d1z; } }
        float r2x, r2z; lane_right(d2x, d2z, &r2x, &r2z);
        const float dt = d1x * d2x + d1z * d2z;
        if (dt > -0.2f) {
            float mx = r1x * o1 + r2x * o2, mz = r1z * o1 + r2z * o2; const float ml = sqrtf(mx * mx + mz * mz);
            if (ml > 1e-3f) { const float want = 0.5f * (o1 + o2) / sqrtf((1.0f + dt) * 0.5f); const float k = (want > 6.0f ? 6.0f : want) / ml; mx *= k; mz *= k; }
            raw_lane(NX(b), s_fy[b], NZ(b), mx, mz);
        } else { raw_lane(NX(b), s_fy[b], NZ(b), r1x * o1, r1z * o1); raw_lane(NX(b), s_fy[b], NZ(b), r2x * o2, r2z * o2); }
    }
    {   /* the stop point on the lane of tFirst -> tSecond; a marker off the road (compound, yard): on to it */
        float dx = NX(tSecond) - NX(tFirst), dz = NZ(tSecond) - NZ(tFirst); const float L = sqrtf(dx * dx + dz * dz);
        if (L > 0.3f) { dx /= L; dz /= L; } float rx, rz; lane_right(dx, dz, &rx, &rz); const float o = edge_off(tFirst, tSecond);
        s_end_x = NX(tFirst) + dx * L * tfrac + rx * o; s_end_z = NZ(tFirst) + dz * L * tfrac + rz * o;
        raw_add(s_end_x, s_end_z);
        const float off_m = sqrtf((s_tx - s_end_x) * (s_tx - s_end_x) + (s_tz - s_end_z) * (s_tz - s_end_z));
        const float pd = sqrtf((s_end_x - pos[0]) * (s_end_x - pos[0]) + (s_end_z - pos[2]) * (s_end_z - pos[2]));
        if (off_m > 14.0f && off_m < 130.0f && pd < 250.0f) {   /* plan it once we are near */
            if (!s_ce.ok || fabsf(s_ce.gx - s_tx) + fabsf(s_ce.gz - s_tz) > 3.0f) {
                s_ce.n = connector(s_end_x, s_end_z, s_tx, s_tz, pos[1], s_ce.x, s_ce.z, 64); s_ce.sx = s_end_x; s_ce.sz = s_end_z; s_ce.gx = s_tx; s_ce.gz = s_tz; s_ce.t = now; s_ce.ok = 1; }
            if (s_ce.n > 0) { s_rawflag = 1; for (int i = 0; i < s_ce.n; i++) raw_add(s_ce.x[i], s_ce.z[i]); s_end_x = s_ce.x[s_ce.n - 1]; s_end_z = s_ce.z[s_ce.n - 1]; s_rawflag = 0; }
        }
    }
    }
resample:
    {
    float carry = 0.0f; s_px[0] = s_rawx[0]; s_pz[0] = s_rawz[0]; s_poff[0] = s_rawoff[0]; s_np = 1;
    for (int i = 1; i < s_nraw && s_np < PMAX; i++) {
        const float ax = s_rawx[i - 1], az = s_rawz[i - 1], ex = s_rawx[i] - ax, ez = s_rawz[i] - az, L = sqrtf(ex * ex + ez * ez);
        if (L < 1e-3f) continue;
        float t = PSTEP - carry;
        while (t <= L && s_np < PMAX) { s_px[s_np] = ax + ex * (t / L); s_pz[s_np] = az + ez * (t / L); s_poff[s_np] = (uint8_t)(s_rawoff[i] | s_rawoff[i - 1]); s_np++; t += PSTEP; }
        carry = L - (t - PSTEP);
    }
    if (s_np < PMAX) { s_px[s_np] = s_end_x; s_pz[s_np] = s_end_z; s_poff[s_np] = s_rawoff[s_nraw - 1]; s_np++; s_path_ends = 1; }
    }
    return s_np >= 2;
}
/* speed plan: the turn's real radius (heading change over 24 m), then braking distance backwards */
static void plan_speeds(float vmax, float alat, float adec)
{
    for (int i = 0; i < s_np; i++) {
        const int a = i >= 6 ? i - 6 : 0, b = i + 6 < s_np ? i + 6 : s_np - 1;
        float v = vmax;
        if (b > i && i > a) {
            const float h1 = atan2f(s_px[i] - s_px[a], s_pz[i] - s_pz[a]), h2 = atan2f(s_px[b] - s_px[i], s_pz[b] - s_pz[i]);
            float dh = h2 - h1; while (dh > 3.14159265f) dh -= 6.2831853f; while (dh < -3.14159265f) dh += 6.2831853f;
            const float len = (float)(b - a) * PSTEP * 0.5f, kap = fabsf(dh) / (len > 1.0f ? len : 1.0f);
            if (kap > 1e-4f) { const float vc = sqrtf(alat / kap); if (vc < v) v = vc; }
        }
        if (s_poff[i] && v > 9.0f) v = 9.0f;   /* off-road: careful */
        if (v < 6.0f) v = 6.0f;
        s_pv[i] = v;
    }
    if (s_path_ends) s_pv[s_np - 1] = 0.0f;
    for (int i = s_np - 2; i >= 0; i--) { const float lim = sqrtf(s_pv[i + 1] * s_pv[i + 1] + 2.0f * adec * PSTEP); if (s_pv[i] > lim) s_pv[i] = lim; }
}

/* ================================================================ the drive */
enum { D_OFF, D_BOARD, D_DRIVE, D_ARRIVED };
static struct {
    int st;
    uint32_t car, driver;            /* driver = crew member at the wheel (0 in the self test) */
    uint32_t hkv, di, s_orig, s_clone; int redir;
    int kind, slot; float tgt[3];
    int fast;
    double t_start, t_plan, t_board, t_last, t_stuck, t_rev_end, t_said, t_arrive, t_log, t_unstick_win;
    int ip;                          /* nearest path index */
    float steer, gas, brake; int rev, hb;
    int unstick_n; float best_rest; double t_progress;
    double cal_dot, cal_yaw; int cal_n, cal_yn;
    unsigned frames;
    float c_want, c_cur, vlim_cur, free_cur, rev_steer; int kind_cur;   /* lane offset (m, + = right) */
    double t_choose, t_wake, t_stop, t_flip;
    uint32_t bdrv; int early; double t_sm, t_wait_log;          /* boarding: the crew member who takes the wheel; player already moved over */
    unsigned id;                     /* counts the drives (per-drive state elsewhere resets when it changes) */
    int pause_redir;                 /* the car's controls were handed back for the pause menu: take them again after it */
} R;
/* battle: damage to the car (not from our own bumping) or to the people in it -> drive fast and never wait; the car on fire
 * (it explodes a few seconds later) -> stop and everybody out. t_danger = the last hit, bail = the time we started stopping. */
static struct { double t_danger, bail; float hp_last, v_last, php, dhp; double t_impact, t_said, t_hp; uint32_t car; unsigned id; } B;
static int rdf_ok(uint32_t ea, float* f) { if (!ok(ea)) return 0; *f = rdf(ea); return 1; }
static int rd32_ok(uint32_t ea, uint32_t* u) { if (!ok(ea)) return 0; *u = rd32(ea); return 1; }
static void input_redirect(uint32_t hkv)
{
    R.redir = 0; R.hkv = hkv; R.di = 0;
    const uint32_t s = car_status(hkv), di = rd32(hkv + 0x20u);
    if (!s || !ok(di)) { CLOG("[crewdrive] no device status / driver input (status %08X, di %08X)\n", s, di); return; }
    const uint32_t dvt = rd32(di), cur = rd32(di + 0x18u); const uint16_t rc = rd16(s + 6u);
    CLOG("[crewdrive] car controls: hkVehicle %08X status %08X driver input %08X (vt %08X, writes %08X, refs %u)\n", hkv, s, di, dvt, cur, rc);
    if (dvt != DI_VTABLE || cur != s || rc < 2) { CLOG("[crewdrive] driver input not the expected one -> writing the status every frame instead\n"); return; }
    uint64_t cl = 0; const uint64_t a[1] = { s };
    if (!gcall(FN_STATUSCLONE, a, 1, &cl) || !ok((uint32_t)cl) || rd32((uint32_t)cl) != rd32(s)) { CLOG("[crewdrive] status clone failed\n"); return; }
    /* reference counts: the clone gets one extra (never freed under the car's input), the original keeps its own (never
     * freed early). Changing them (2026-11-21) left freed memory being written -> the pause map broke after a drive. */
    wr16((uint32_t)cl + 6u, (uint16_t)(rd16((uint32_t)cl + 6u) + 1)); wr32(di + 0x18u, (uint32_t)cl);
    R.redir = 1; R.di = di; R.s_orig = s; R.s_clone = (uint32_t)cl;
    CLOG("[crewdrive] the pad no longer reaches the car (its input goes to %08X)\n", (uint32_t)cl);
}
static void input_restore(void)
{
    if (R.redir != 1) return; R.redir = 0;
    __try {
        /* only while the car still has this driver input (it may be gone once the player got out) */
        if (ok(R.hkv) && rd32(R.hkv + 0x20u) == R.di && ok(R.di) && rd32(R.di) == DI_VTABLE && rd32(R.di + 0x18u) == R.s_clone) {
            wr32(R.di + 0x18u, R.s_orig);
            CLOG("[crewdrive] pad controls the car again\n");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static void write_controls(float steer, float gas, float brake, int hb, int rev)
{
    const uint32_t s = R.hkv ? car_status(R.hkv) : 0; if (!s) return;
    __try {
        wrf(s + 8u, steer); wrf(s + 0x14u, gas); wrf(s + 0x18u, brake);
        vm_base[s + 0x10u] = (uint8_t)(hb ? 1 : 0); vm_base[s + 0x11u] = (uint8_t)(rev ? 1 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static int place_car(uint32_t veh, float x, float y, float z, float fx, float fz)   /* upright on the road, heading (fx, fz) */
{
#ifdef CD_SIM
    return sim_place_car(veh, x, y, z, fx, fz);
#endif
    const uint32_t buf = scratch(), xf = rd32(veh + 0x68u); if (!buf || !ok(xf)) return 0;
    const float l = sqrtf(fx * fx + fz * fz); if (l < 1e-3f) return 0; fx /= l; fz /= l;
    float rowR[3], rowU[3] = { 0, 1, 0 }, rowA[3] = { fx * (float)s_fwd_sign, 0, fz * (float)s_fwd_sign };
    /* right = up x at (keeps the car's own handedness: compare with its current rows) */
    float cr[3], cu[3], ca[3], cp[3]; if (!ent_rows(veh, cr, cu, ca, cp)) return 0;
    rowR[0] = rowU[1] * rowA[2] - rowU[2] * rowA[1]; rowR[1] = rowU[2] * rowA[0] - rowU[0] * rowA[2]; rowR[2] = rowU[0] * rowA[1] - rowU[1] * rowA[0];
    { const float hand = cr[0] * (cu[1] * ca[2] - cu[2] * ca[1]) + cr[1] * (cu[2] * ca[0] - cu[0] * ca[2]) + cr[2] * (cu[0] * ca[1] - cu[1] * ca[0]);
      if (hand < 0) { rowR[0] = -rowR[0]; rowR[1] = -rowR[1]; rowR[2] = -rowR[2]; } }
    const float* rows[4] = { rowR, rowU, rowA, NULL }; const float P[3] = { x, y, z };
    for (int r = 0; r < 4; r++) { const float* v = r < 3 ? rows[r] : P;
        for (int k = 0; k < 3; k++) wrf(buf + (uint32_t)(r * 16 + k * 4), v[k]); wr32(buf + (uint32_t)(r * 16 + 12), rd32(xf + 0x10u + (uint32_t)(r * 16 + 12))); }
    const uint64_t a[1] = { buf }; if (!vcall(veh, 0x2Cu, a, 1, NULL)) return 0;
    const uint32_t blk = buf + 0x40u; wrf(blk, x); wrf(blk + 4, y); wrf(blk + 8, z); wr32(blk + 12, 2u);
    const uint64_t b[1] = { blk }; vcall(veh, 0x24u, b, 1, NULL);
    const uint32_t body = car_body(veh);
    if (body_live(veh, body)) { const uint32_t vb = buf + 0x80u; for (int k = 0; k < 4; k++) wrf(vb + 4u * (uint32_t)k, 0.0f);
        const uint64_t c[2] = { body, vb }; gcall_t(lookup(FN_SETLINVEL), HK_TOC, c, 2, NULL); gcall_t(lookup(FN_SETANGVEL), HK_TOC, c, 2, NULL); }
    return 1;
}

/* seats: crew member to the wheel, the player to the front passenger seat */
static int swap_to_crew(uint32_t pl, uint32_t car, uint32_t drv)
{
    const int n = seat_count(car), sp = seat_of(car, pl);
    CLOG("[crewdrive] seats before: %d seats, player %d, driver-to-be %08X at %d, occupants %08X %08X %08X %08X\n", n, sp, drv, drv ? seat_of(car, drv) : -1,
         seat_occ(car, 0), seat_occ(car, 1), seat_occ(car, 2), seat_occ(car, 3));
    if (n < 2) return 0;
    if (sp == 1 && (!drv || seat_of(car, drv) == 0)) return 1;   /* already arranged (he came in by the driver's door) */
    if (sp != 0) return 0;
    if (!drv) return move_seat(car, pl, 1);   /* self test: the player alone moves over */
    int sd = seat_of(car, drv); if (sd < 0) return 0;
    /* seat 1 must be free for the player: whoever sits there (the driver-to-be or the other crew member) moves to the back */
    const uint32_t o1 = seat_occ(car, 1);
    if (o1) { const int f = free_seat(car, 1); if (f >= 2) { if (!move_seat(car, o1, f)) return 0; if (o1 != drv) crew_sm_seat(car, o1, f); else sd = f; } }
    if (seat_occ(car, 1)) {   /* a two-seat car: lift the crew member off, the player over, the crew member in */
        if (!clear_seat(car, 1)) return 0;
        if (!move_seat(car, pl, 1)) { link_seat(car, drv, 1); return 0; }
        if (!link_seat(car, drv, 0)) { CLOG("[crewdrive] could not seat the crew member at the wheel\n"); return 0; }
    } else {
        if (!move_seat(car, pl, 1)) return 0;
        if (!move_seat(car, drv, 0)) { move_seat(car, pl, 0); return 0; }
    }
    crew_sm_seat(car, drv, 0);
    CLOG("[crewdrive] seats after: player %d, driver %08X at %d\n", seat_of(car, pl), drv, seat_of(car, drv));
    return seat_of(car, pl) == 1 && seat_of(car, drv) == 0;
}
static void swap_back(uint32_t pl, uint32_t car, uint32_t drv)
{
    if (!ok(car) || seat_of(car, pl) < 0) return;
    if (drv && seat_of(car, drv) == 0) {
        const int f = free_seat(car, 1);
        if (f >= 2) { if (move_seat(car, drv, f)) crew_sm_seat(car, drv, f); }
        else { clear_seat(car, 0); }
    }
    if (!seat_occ(car, 0)) move_seat(car, pl, 0);
    if (drv && seat_of(car, drv) < 0) { if (link_seat(car, drv, 1)) crew_sm_seat(car, drv, 1); }
    else if (drv && seat_of(car, drv) >= 2 && !seat_occ(car, 1)) { if (move_seat(car, drv, 1)) crew_sm_seat(car, drv, 1); }
    CLOG("[crewdrive] seats back: player %d, crew %08X at %d\n", seat_of(car, pl), drv, drv ? seat_of(car, drv) : -1);
}
static void drive_end(const char* why, int give_back)
{
    if (R.st == D_OFF) return;
    const uint32_t pl = player();
    if (R.st == D_BOARD && R.early && pl && ok(R.car) && vehicle_of(pl) == R.car && seat_of(R.car, pl) == 1 && !seat_occ(R.car, 0)) move_seat(R.car, pl, 0);
    if (R.st == D_BOARD) input_restore();
    if (R.st == D_DRIVE || R.st == D_ARRIVED) {
        write_controls(0.0f, 0.0f, 0.0f, 0, 0);
        input_restore();
        if (give_back && pl && ok(R.car) && vehicle_of(pl) == R.car) swap_back(pl, R.car, R.driver);
    }
    g_gps_force_slot = -1;
    CLOG("[crewdrive] drive ended: %s\n", why);
    R.st = D_OFF; R.car = 0; R.driver = 0; R.bdrv = 0; R.early = 0;
}
static int pick_driver(uint32_t pl, uint32_t car)   /* first-hired living crew member sitting in this car */
{
    uint32_t cl[5]; const int n = crew_list(pl, cl);
    for (int i = 0; i < n; i++) if (vehicle_of(cl[i]) == car && seat_of(car, cl[i]) >= 0) return (int)cl[i];
    return 0;
}
static int all_crew_in(uint32_t pl, uint32_t car)
{
    uint32_t cl[5]; const int n = crew_list(pl, cl); int in = 0;
    for (int i = 0; i < n; i++) if (vehicle_of(cl[i]) == car && seat_of(car, cl[i]) >= 0) in++;
    return n > 0 && in == n;
}
static void target_refresh(const float* pos)
{
    float p[3]; int slot = -1;
    if (find_marker(R.kind, pos, p, &slot)) {
        R.slot = slot; g_gps_force_slot = slot;
        if (!s_have_target || fabsf(p[0] - R.tgt[0]) + fabsf(p[2] - R.tgt[2]) > 4.0f) { R.tgt[0] = p[0]; R.tgt[1] = p[1]; R.tgt[2] = p[2]; set_target_pos(p[0], p[1], p[2]); }
    }
}
static int drive_begin(uint32_t pl, uint32_t car, uint32_t drv)
{
    const uint32_t hkv = car_hkv(car); if (!hkv) { CLOG("[crewdrive] car %08X has no Havok vehicle\n", car); return 0; }
#ifndef CD_SIM
    if (!swap_to_crew(pl, car, drv)) { CLOG("[crewdrive] seat switch failed\n"); return 0; }
#endif
    R.car = car; R.driver = drv;
    if (R.redir != 1 || R.hkv != hkv) input_redirect(hkv);
    R.c_want = R.c_cur = 0; R.vlim_cur = BIG; R.free_cur = 90; R.kind_cur = 0; R.t_choose = R.t_wake = R.t_stop = R.t_flip = 0; R.t_unstick_win = 0;
    { static unsigned s_drives; R.id = ++s_drives; }
    R.st = D_DRIVE; R.t_start = R.t_plan = R.t_last = now_s(); R.t_stuck = 0; R.t_rev_end = 0; R.ip = 0; R.unstick_n = 0;
    R.best_rest = BIG; R.t_progress = R.t_start; R.steer = 0; R.cal_n = R.cal_yn = 0; R.cal_dot = R.cal_yaw = 0; s_prev_u = s_prev_v = -1;
    s_have_target = 0;
    float pos[3]; if (ent_pos(car, pos)) { if (R.kind >= 0) target_refresh(pos); else set_target_pos(R.tgt[0], fok(R.tgt[1]) ? R.tgt[1] : pos[1], R.tgt[2]); }
    CLOG("[crewdrive] driving to (%.1f, %.1f) %s\n", R.tgt[0], R.tgt[2], R.kind >= 0 ? k_kind_label[R.kind] : "(test road)");
    return 1;
}

/* ---------------------------------------------------------------- what is ahead
 * Everything within 85 m is put in path coordinates: s = metres along our lane ahead, l = metres to the right of it (left =
 * toward the centre line / oncoming lane). Cars from the factory list, people from the spawn list (no memory scan). */
typedef struct { float s, l, vs, vl, hw, hl; int car; float x, z, ax, az, speed; } Obs;   /* + world place, heading, speed (manoeuvres) */
#define OBS_MAX 96
static Obs s_obs[OBS_MAX]; static int s_nobs;
static int s_ignore_people;   /* nudging past people who stay in the road (they jump aside) */
static void path_frame(int k, float* tx, float* tz, float* nx, float* nz)
{
    int a = k > 0 ? k - 1 : 0, b = k + 1 < s_np ? k + 1 : k; if (a == b && a > 0) a--;
    float dx = s_px[b] - s_px[a], dz = s_pz[b] - s_pz[a]; const float L = sqrtf(dx * dx + dz * dz);
    if (L > 1e-3f) { dx /= L; dz /= L; } else { dx = 0; dz = 1; }
    *tx = dx; *tz = dz; lane_right(dx, dz, nx, nz);
}
static void collect_obs(uint32_t car, const float* pos)
{
    s_nobs = 0;
    static uint32_t cars[128], npc[512]; int nc = 0, nn = 0;
    __try { nc = gf_world_cars(cars, 128); } __except (EXCEPTION_EXECUTE_HANDLER) { nc = 0; }
    __try { nn = gf_world_npcs(npc, 512); } __except (EXCEPTION_EXECUTE_HANDLER) { nn = 0; }
    const int i0 = R.ip, iend = i0 + 42 < s_np ? i0 + 42 : s_np;
    if (iend - i0 < 2) return;
    for (int k = 0; k < nc + nn && s_nobs < OBS_MAX; k++) {
        const int is_car = k < nc; const uint32_t o = is_car ? cars[k] : npc[k - nc];
        if (o == car || !ok(o) || !ok(rd32(o + 0x68u))) continue;
        float p[3]; if (!ent_pos(o, p) || dist2d(p, pos) > 85.0f || fabsf(p[1] - pos[1]) > 4.0f) continue;
        if (!is_car && (!alive(o) || (rd32(o + 0x5C4u) & 8u) || s_ignore_people)) continue;   /* dead, in a car, or being nudged past */
        float bd = BIG; int bk = i0;
        for (int i = i0; i < iend; i++) { const float dx = s_px[i] - p[0], dz = s_pz[i] - p[2], d = dx * dx + dz * dz; if (d < bd) { bd = d; bk = i; } }
        float tx, tz, nx, nz; path_frame(bk, &tx, &tz, &nx, &nz);
        const float rx = p[0] - s_px[bk], rz = p[2] - s_pz[bk];
        Obs O; O.s = (float)(bk - i0) * PSTEP + rx * tx + rz * tz; O.l = rx * nx + rz * nz;
        if (O.s < -9.0f || O.s > 82.0f || fabsf(O.l) > 14.0f) continue;
        float v[3] = { 0, 0, 0 }; if (is_car) car_vel(o, v);
        O.vs = v[0] * tx + v[2] * tz; O.vl = v[0] * nx + v[2] * nz;
        O.hw = is_car ? 1.1f : 0.45f; O.hl = is_car ? 2.6f : 0.45f; O.car = is_car;
        O.x = p[0]; O.z = p[2]; O.ax = 0; O.az = 1; O.speed = sqrtf(v[0] * v[0] + v[2] * v[2]);
        if (is_car) { float r_[3], u_[3], a_[3], p_[3]; if (ent_rows(o, r_, u_, a_, p_)) { const float l_ = sqrtf(a_[0] * a_[0] + a_[2] * a_[2]); if (l_ > 1e-3f) { O.ax = a_[0] / l_; O.az = a_[2] / l_; } } }
        s_obs[s_nobs++] = O;
    }
}
/* the world's own collision (assets/gf_collision.bin): walls, posts, lamps, hydrants along a shifted lane.
 * Also sideways: a wall running alongside the lane (railings, kerb walls) closer than the car's side + 0.1 m blocks it there;
 * s_side_clear = the closest one (metres from the lane line, up to 1.6) so the lane choice keeps away from them. */
static float s_side_clear;
static float static_free(float c, float y)
{
    static const float segs[] = { 2.0f, 8.0f, 14.0f, 20.0f, 27.0f, 34.0f };
    s_side_clear = 1.6f;
    for (int j = 0; j + 1 < (int)(sizeof segs / sizeof segs[0]); j++) {
        const int ka = R.ip + (int)(segs[j] / PSTEP), kb = R.ip + (int)(segs[j + 1] / PSTEP);
        if (kb >= s_np) break;
        float tx, tz, nxa, nza, nxb, nzb; path_frame(ka, &tx, &tz, &nxa, &nza); path_frame(kb, &tx, &tz, &nxb, &nzb);
        if (j < 4) {   /* sideways from the lane line, at the segment's end (and its middle) */
            for (int m = 0; m < 2; m++) {
                const int k = m ? kb : (ka + kb) / 2; float t2x, t2z, nx2, nz2; path_frame(k, &t2x, &t2z, &nx2, &nz2);
                const float o2[3] = { s_px[k] + nx2 * c, y, s_pz[k] + nz2 * c };
                for (int sg = -1; sg <= 1; sg += 2) {
                    const float d2[3] = { nx2 * (float)sg, 0.0f, nz2 * (float)sg }; float t2 = 0;
                    if (cd_ray(o2, d2, 1.6f, &t2)) { if (t2 < s_side_clear) s_side_clear = t2; if (t2 < 1.05f) return segs[j] + (m ? segs[j + 1] - segs[j] : 0.5f * (segs[j + 1] - segs[j])); }
                }
            }
        }
        static const float ws[5] = { 0.0f, -0.62f, 0.62f, -1.2f, 1.2f };   /* across the car, every 0.6 m (posts are 0.3 m) */
        for (int e = 0; e < 5; e++) {
            const float w = ws[e];
            const float ca = j == 0 ? s_l_act : c;   /* moving over: the diagonal the car really drives */
            const float o[3] = { s_px[ka] + nxa * (ca + w), y, s_pz[ka] + nza * (ca + w) }, b[3] = { s_px[kb] + nxb * (c + w), y, s_pz[kb] + nzb * (c + w) };
            float d[3] = { b[0] - o[0], 0.0f, b[2] - o[2] }; const float L = sqrtf(d[0] * d[0] + d[2] * d[2]); if (L < 0.1f) continue;
            d[0] /= L; d[2] /= L; float t = 0;
            if (cd_ray(o, d, L, &t)) return segs[j] + t;   /* a real wall / post (ground, ramps and road-corridor ghosts ignored) */
        }
    }
    return 90.0f;
}
static int s_aggr;        /* 0 normal, 1 Step on it, 2 a fight: how readily we pass */
static int s_room_need;   /* lane_free: a lane would be open if we had room to swing out (set while evaluating it) */
/* free metres in a lane shifted by c (and the speed we may have there) */
static float lane_free(float c, float v, float margin, float adec, float* vlim, int* kind, float sfree)
{
    float F = sfree; *vlim = sfree < 89.0f ? sqrtf(2.0f * adec * fmaxf(0.0f, sfree - 3.0f)) : BIG; *kind = sfree < 89.0f ? 5 : 0;
    const float me_hw = 1.05f, me_hl = 2.4f, vm = v > 1.0f ? v : 1.0f;
    const float c0 = s_l_act, lo = fminf(c0, c) - me_hw, hi = fmaxf(c0, c) + me_hw;   /* the band we sweep while moving over */
    const float shift_need = 3.0f + 2.0f * fabsf(c - c0) + 0.6f * v;                   /* metres of road a lane change takes */
    for (int i = 0; i < s_nobs; i++) {
        const Obs* o = &s_obs[i];
        const float reach = o->hw + me_hw + margin;
        /* moving over: something ahead inside the band we sweep. Right in front of us: the gap must let our front corner swing
         * clear of its corner (full lock, radius ~5.5 m: sideways d^2/2R + 2.3 d/R after d metres); otherwise its back must be
         * beyond the distance the whole move takes. Too close: that lane is shut for now (and we may back up for room). */
        if (fabsf(c - c0) > 0.5f && o->s > 0.0f && o->l + o->hw > lo && o->l - o->hw < hi) {
            const float gap = o->s - o->hl - me_hl; float need = shift_need;
            if (fabsf(o->l - c0) < o->hw + me_hw) {
                const float ov = o->hw + me_hw - fabsf(o->l - c0) + 0.3f; float d = 0.5f;
                while (d < 12.0f && d * d / 11.0f + 2.3f * d / 5.5f < ov) d += 0.25f;
                need = d + 0.5f * v;
            }
            if (gap < need) { const float g = fmaxf(0.0f, gap); if (g < F) F = g; if (o->vs < 1.0f) s_room_need = 1; }
        }
        if (o->s > -(o->hl + me_hl) && o->s < o->hl + me_hl) {   /* beside us: that lane is taken right now */
            if (fabsf(o->l - c) < o->hw + me_hw && fabsf(c) > 0.3f) { if (0.5f < F) F = 0.5f; }
            continue;
        }
        /* the oncoming lane: only with room to finish the pass (closing speed x 4.5 s + margin) */
        if (c < -2.0f && o->car && o->vs < -1.0f && fabsf(o->l - c) < reach + 1.0f) {
            const float need = (s_aggr == 2 ? 12.0f : s_aggr ? 16.0f : 20.0f) + (v - o->vs) * (s_aggr == 2 ? 3.0f : s_aggr ? 3.8f : 4.5f), meet = (o->s - o->hl - me_hl) * vm / (vm - o->vs);
            if (meet < need) { const float g = fmaxf(0.0f, meet * 0.4f); if (g < F) F = g; float lim = sqrtf(2.0f * adec * fmaxf(0.0f, meet * 0.5f - 3.0f)); if (lim < *vlim) { *vlim = lim; *kind = 6; } continue; }
        }
        if (o->s < 0) continue;
        const float gap = o->s - o->hl - me_hl;
        if (o->car && o->vs > v + 1.5f) continue;                 /* pulling away from us */
        const float closing = v - o->vs, tm = closing > 0.5f ? gap / closing : 4.0f, tt = tm < 3.0f ? tm : 3.0f;
        const float lp = o->l + o->vl * tt;
        if (!(fabsf(o->l - c) < reach || fabsf(lp - c) < reach || (o->l - c) * (lp - c) < 0.0f)) continue;   /* now, later, or crossing it */
        float eff = gap; if (o->vs < -1.0f) { eff = gap * vm / (vm - o->vs); if (c < -2.0f) eff *= 0.6f; }   /* oncoming: where we would meet (in its lane: keep well clear) */
        if (eff < F) F = eff;
        float lim;
        if (o->car && o->vs > (s_aggr == 2 ? 4.0f : 1.5f)) lim = o->vs + 0.6f * (gap - (4.5f + 0.9f * v));   /* in a fight a crawling car counts as standing */
        else lim = sqrtf(2.0f * adec * fmaxf(0.0f, eff - (o->car && fabsf(o->vs) < 1.0f ? 4.5f : 2.5f)));   /* behind a standing car: room to swing out */
        if (lim < 0) lim = 0;
        if (lim < *vlim) { *vlim = lim; *kind = o->car ? (o->vs > (s_aggr == 2 ? 4.0f : 1.5f) ? 2 : o->vs < -1.0f ? 6 : 1) : 4; }
    }
    return F;
}
/* ================================================================ manoeuvres (tight spots)
 * Stopped behind something that does not move, wedged against a wall, or a U-turn in a narrow street: a hybrid A* search
 * over forward / reverse moves (5 wheel angles, 1.2 m steps, the car's own turning circle) in a 38 m square around the car,
 * against the real walls (gf_collision.bin minus the origin pile) and the cars / people standing around, finds the shortest
 * sequence that puts the car back on the route past the obstacle, heading along it. It is then driven slowly, piece by
 * piece; contact or no progress -> plan again (at most 3 times, then the old last resort). */
#define MG 96
#define MGC 0.4f
#define MH 36
#define MNODES 150000
static uint8_t s_mocc[MG * MG];                          /* for the car's 3 circle centres: 2 = contact, 1 = inside the safety margin */
static float s_mx0, s_mz0;
static float s_mcost[MG * MG * MH];
static struct { float x, z, h; int parent; signed char dir, st; float g, len; } s_mn[MNODES];
static int s_mheap[MNODES]; static float s_mkey[MNODES];
typedef struct { signed char dir; float steer, len; } ManSeg;
static ManSeg s_mseg[48]; static int s_nmseg;
static struct { int active, seg, fails; float done; double t_seg, t_last_move, t_win; float lx, lz; unsigned id; } M;
static float s_mtb[9 * 8000];
static void m_mark_disc(float x, float z, float r, uint8_t val)
{
    const int c0x = (int)floorf((x - r - s_mx0) / MGC), c1x = (int)floorf((x + r - s_mx0) / MGC), c0z = (int)floorf((z - r - s_mz0) / MGC), c1z = (int)floorf((z + r - s_mz0) / MGC);
    for (int cz = c0z; cz <= c1z; cz++) for (int cx = c0x; cx <= c1x; cx++) {
        if (cx < 0 || cz < 0 || cx >= MG || cz >= MG) continue;
        const float px = s_mx0 + (cx + 0.5f) * MGC, pz = s_mz0 + (cz + 0.5f) * MGC;
        if ((px - x) * (px - x) + (pz - z) * (pz - z) <= r * r && s_mocc[cz * MG + cx] < val) s_mocc[cz * MG + cx] = val; }
}
/* occupancy for the car's three circle centres (radius 1.2 = half width + margin) */
static void man_grid(const float* c, float y)
{
    memset(s_mocc, 0, sizeof s_mocc);
    s_mx0 = c[0] - MG * MGC * 0.5f; s_mz0 = c[2] - MG * MGC * 0.5f;
    const float R0 = 1.35f, RH = 1.05f;   /* margin / contact radius around a circle centre (5 circles cover the body to its corners) */
    const int n = cd_tris_near(c[0], c[2], MG * MGC * 0.5f + 2.0f, s_mtb, 8000);
    /* every cell learns the nearest wall point: points along the wall edges seed their cell, two sweeps pass them on
     * (a distance transform: the same as a disc round every point, at a fraction of the cost) */
    static float nx_[MG * MG], nz_[MG * MG];
    for (int i = 0; i < MG * MG; i++) { nx_[i] = 1e9f; nz_[i] = 1e9f; }
    #define CEN_X(cx) (s_mx0 + ((float)(cx) + 0.5f) * MGC)
    #define CEN_Z(cz) (s_mz0 + ((float)(cz) + 0.5f) * MGC)
    #define D2(i, cx, cz) ((nx_[i] - CEN_X(cx)) * (nx_[i] - CEN_X(cx)) + (nz_[i] - CEN_Z(cz)) * (nz_[i] - CEN_Z(cz)))
    for (int i = 0; i < n; i++) {
        const float* w = s_mtb + 9 * i;
        const float ymin = fminf(w[1], fminf(w[4], w[7])), ymax = fmaxf(w[1], fmaxf(w[4], w[7]));
        if (ymax < y + 0.45f || ymin > y + 1.6f || !real_wall(w)) continue;   /* in the car's height band */
        for (int e = 0; e < 3; e++) { const float* a = w + 3 * e; const float* b = w + 3 * ((e + 1) % 3);
            const float L = sqrtf((b[0] - a[0]) * (b[0] - a[0]) + (b[2] - a[2]) * (b[2] - a[2])); const int st = (int)(L / 0.2f) + 1;
            for (int k = 0; k <= st; k++) { const float f = (float)k / (float)st, px = a[0] + (b[0] - a[0]) * f, pz = a[2] + (b[2] - a[2]) * f;
                const int cx = (int)floorf((px - s_mx0) / MGC), cz = (int)floorf((pz - s_mz0) / MGC);
                if (cx < 0 || cz < 0 || cx >= MG || cz >= MG) continue;
                const int ci = cz * MG + cx; const float d = (px - CEN_X(cx)) * (px - CEN_X(cx)) + (pz - CEN_Z(cz)) * (pz - CEN_Z(cz));
                if (d < D2(ci, cx, cz)) { nx_[ci] = px; nz_[ci] = pz; } } }
    }
    for (int pass = 0; pass < 2; pass++) {
        const int st = pass ? -1 : 1;
        for (int cz = pass ? MG - 1 : 0; cz >= 0 && cz < MG; cz += st) for (int cx = pass ? MG - 1 : 0; cx >= 0 && cx < MG; cx += st) {
            const int ci = cz * MG + cx; float best = D2(ci, cx, cz);
            static const int nb[4][2] = { { -1, 0 }, { 0, -1 }, { -1, -1 }, { 1, -1 } };
            for (int k = 0; k < 4; k++) { const int ax = cx + nb[k][0] * st, az = cz + nb[k][1] * st; if (ax < 0 || az < 0 || ax >= MG || az >= MG) continue;
                const int ai = az * MG + ax; if (nx_[ai] > 1e8f) continue;
                const float d = (nx_[ai] - CEN_X(cx)) * (nx_[ai] - CEN_X(cx)) + (nz_[ai] - CEN_Z(cz)) * (nz_[ai] - CEN_Z(cz));
                if (d < best) { best = d; nx_[ci] = nx_[ai]; nz_[ci] = nz_[ai]; } }
        }
    }
    for (int cz = 0; cz < MG; cz++) for (int cx = 0; cx < MG; cx++) { const int ci = cz * MG + cx; if (nx_[ci] > 1e8f) continue;
        const float d = D2(ci, cx, cz); s_mocc[ci] = d <= RH * RH ? 2 : d <= R0 * R0 ? 1 : 0; }
    #undef D2
    #undef CEN_X
    #undef CEN_Z
    for (int i = 0; i < s_nobs; i++) { const Obs* o = &s_obs[i];
        if (o->speed > 1.5f) continue;   /* moving ones are watched while driving the manoeuvre */
        if (!o->car) { m_mark_disc(o->x, o->z, 0.45f + R0, 1); m_mark_disc(o->x, o->z, 0.45f + RH, 2); continue; }
        for (int k = -2; k <= 2; k++) { const float px = o->x + o->ax * 1.1f * (float)k, pz = o->z + o->az * 1.1f * (float)k; m_mark_disc(px, pz, 1.0f + R0, 1); m_mark_disc(px, pz, 1.0f + RH, 2); } }
}
static int m_free_d(float x, float z, float fx, float fz)   /* 0 contact, 1 inside the safety margin, 2 free; (fx,fz) = sin/cos of the heading */
{
    int r = 2;
    for (int k = -2; k <= 2; k++) {
        const int cx = (int)floorf((x + fx * 0.95f * (float)k - s_mx0) / MGC), cz = (int)floorf((z + fz * 0.95f * (float)k - s_mz0) / MGC);
        if (cx < 1 || cz < 1 || cx >= MG - 1 || cz >= MG - 1 || s_mocc[cz * MG + cx] == 2) return 0;
        if (s_mocc[cz * MG + cx]) r = 1; }
    return r;
}
static int m_free(float x, float z, float h) { return m_free_d(x, z, sinf(h), cosf(h)); }
static int m_end_contact(float x, float z, float h, int k)   /* the car's front (k 1) / rear (k -1) circle touches something */
{
    const int cx = (int)floorf((x + sinf(h) * 1.9f * (float)k - s_mx0) / MGC), cz = (int)floorf((z + cosf(h) * 1.9f * (float)k - s_mz0) / MGC);
    return cx < 1 || cz < 1 || cx >= MG - 1 || cz >= MG - 1 || s_mocc[cz * MG + cx] == 2;
}
static int m_hidx(float x, float z, float h)
{
    const int cx = (int)floorf((x - s_mx0) / MGC), cz = (int)floorf((z - s_mz0) / MGC);
    float hh = fmodf(h, 6.2831853f); if (hh < 0) hh += 6.2831853f; const int ih = (int)(hh / 6.2831853f * MH) % MH;
    if (cx < 0 || cz < 0 || cx >= MG || cz >= MG) return -1;
    return (cz * MG + cx) * MH + ih;
}
static int man_plan_(const float* pos, float h0, float y);
static int man_plan(const float* pos, float h0, float y)   /* 1 planned, 0 no way, -1 not now (one plan per 0.5 s) */
{
    static double t_last; const double tn = now_s(); if (tn - t_last < 0.5 && tn >= t_last) return -1; t_last = tn;
    const double p0 = perf_ms(); const int r = man_plan_(pos, h0, y); const double ms = perf_ms() - p0;
    if (ms > 4.0) CLOG("[crewdrive] slow manoeuvre plan: %.1f ms\n", ms);
    return r;
}
static int man_plan_(const float* pos, float h0, float y)
{
    if (s_np < 4) return 0;
    /* the goal: the first route point 10..40 m ahead (inside the window) where the car fits at one of our lane offsets */
    float cxz[3] = { pos[0] + sinf(h0) * 6.0f, 0, pos[2] + cosf(h0) * 6.0f };
    const double pg0 = perf_ms(); man_grid(cxz, y); const double pg1 = perf_ms();
    float gx = 0, gz = 0, gh = 0; int found = 0;
    static const float offs[] = { 0.0f, -1.7f, -3.4f, 1.4f };
    for (int k = R.ip + 5; k < s_np - 1 && k < R.ip + 22 && !found; k++) {
        float tx, tz, nx, nz; path_frame(k, &tx, &tz, &nx, &nz);
        for (int o = 0; o < 4 && !found; o++) {
            const float x = s_px[k] + nx * offs[o], z = s_pz[k] + nz * offs[o], h = atan2f(tx, tz);
            if (fabsf(x - cxz[0]) > MG * MGC * 0.5f - 3 || fabsf(z - cxz[2]) > MG * MGC * 0.5f - 3) continue;
            if (m_free(x, z, h) == 2 && m_free(x + tx * 3.0f, z + tz * 3.0f, h) == 2) { gx = x; gz = z; gh = h; found = 1; } }
    }
    if (!found) { CLOG("[crewdrive] manoeuvre: no free spot on the route ahead\n"); return 0; }
    for (int i = 0; i < MG * MG * MH; i++) s_mcost[i] = BIG;
    const double pg2 = perf_ms(); if (pg2 - pg0 > 3.0) CLOG("[crewdrive] manoeuvre map %.1f ms, clear %.1f ms\n", pg1 - pg0, pg2 - pg1);
    int nn = 0, hn = 0, goal = -1;
    #define MPUSH(node, key) do { int i_ = hn++; while (i_) { int p_ = (i_ - 1) >> 1; if (s_mkey[p_] <= (key)) break; s_mheap[i_] = s_mheap[p_]; s_mkey[i_] = s_mkey[p_]; i_ = p_; } s_mheap[i_] = (node); s_mkey[i_] = (key); } while (0)
    s_mn[0].x = pos[0]; s_mn[0].z = pos[2]; s_mn[0].h = h0; s_mn[0].parent = -1; s_mn[0].dir = 1; s_mn[0].st = 0; s_mn[0].g = 0; s_mn[0].len = 0; nn = 1;
    MPUSH(0, 0.0f);
    static const float steers[5] = { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f };
    const float kmax = tanf(0.55f) / 2.9f, ds = 1.2f;
    float rot_c[2][5], rot_s[2][5];   /* the heading turn of one 0.4 m sub-step, per direction and wheel angle */
    for (int dd = 0; dd < 2; dd++) for (int si = 0; si < 5; si++) { const float a = (dd ? 1.0f : -1.0f) * (ds / 3.0f) * kmax * steers[si]; rot_c[dd][si] = cosf(a); rot_s[dd][si] = sinf(a); }
    int pops = 0; float best_h = BIG;
    const int start_contact = m_free(pos[0], pos[2], h0) == 0;
    /* touching with the nose: first back away; touching with the tail: first pull forward */
    const int c_front = start_contact && m_end_contact(pos[0], pos[2], h0, 1), c_rear = start_contact && m_end_contact(pos[0], pos[2], h0, -1);
    while (hn > 0 && pops < 14000) {
        const int cur = s_mheap[0]; pops++;
        { const int m = --hn; const int nd = s_mheap[m]; const float kk = s_mkey[m]; int i = 0;
          for (;;) { int l = 2 * i + 1; if (l >= m) break; if (l + 1 < m && s_mkey[l + 1] < s_mkey[l]) l++; if (s_mkey[l] >= kk) break; s_mheap[i] = s_mheap[l]; s_mkey[i] = s_mkey[l]; i = l; }
          s_mheap[i] = nd; s_mkey[i] = kk; }
        const float x = s_mn[cur].x, z = s_mn[cur].z, h = s_mn[cur].h;
        float dh = h - gh; while (dh > 3.14159265f) dh -= 6.2831853f; while (dh < -3.14159265f) dh += 6.2831853f;
        if ((x - gx) * (x - gx) + (z - gz) * (z - gz) < 1.6f * 1.6f && fabsf(dh) < 0.45f) { goal = cur; break; }
        const float hfx = sinf(h), hfz = cosf(h);
        for (int d = 1; d >= -1; d -= 2) for (int si = 0; si < 5; si++) {
            if (nn >= MNODES) break;
            if (start_contact && s_mn[cur].len < 2.4f && ((c_front && !c_rear && d > 0) || (c_rear && !c_front && d < 0))) continue;
            float nx2 = x, nz2 = z, nh = h, cfx = hfx, cfz = hfz; int okm = 1, tight = 0;
            const float st = ds / 3.0f, da = (float)d * st * kmax * steers[si], ca = rot_c[d > 0][si], sa = rot_s[d > 0][si]; (void)da;
            for (int sub = 1; sub <= 3 && okm; sub++) {
                nh += da; { const float t2 = cfx * ca + cfz * sa; cfz = cfz * ca - cfx * sa; cfx = t2; }
                nx2 += (float)d * st * cfx; nz2 += (float)d * st * cfz;
                const int f = m_free_d(nx2, nz2, cfx, cfz); if (!f && !(start_contact && s_mn[cur].len < 2.4f)) okm = 0; else if (f <= 1) tight = 1; }
            if (!okm) continue;
            const int hi = m_hidx(nx2, nz2, nh); if (hi < 0) continue;
            float g = s_mn[cur].g + ds * (d > 0 ? 1.0f : 1.8f) + 0.25f * fabsf(steers[si]) + 0.4f * fabsf(steers[si] - (float)s_mn[cur].st * 0.5f)
                    + (cur && d != s_mn[cur].dir ? 3.0f : 0.0f) + (tight ? 2.5f : 0.0f);
            if (g >= s_mcost[hi]) continue;
            s_mcost[hi] = g;
            s_mn[nn].x = nx2; s_mn[nn].z = nz2; s_mn[nn].h = nh; s_mn[nn].parent = cur; s_mn[nn].dir = (signed char)d; s_mn[nn].st = (signed char)lrintf(steers[si] * 2.0f); s_mn[nn].g = g; s_mn[nn].len = s_mn[cur].len + ds;
            float dh2 = nh - gh; while (dh2 > 3.14159265f) dh2 -= 6.2831853f; while (dh2 < -3.14159265f) dh2 += 6.2831853f;
            const float hcost = sqrtf((nx2 - gx) * (nx2 - gx) + (nz2 - gz) * (nz2 - gz)) + 1.5f * fabsf(dh2);
            if (hcost < best_h) best_h = hcost;
            MPUSH(nn, g + hcost); nn++;
        }
    }
    #undef MPUSH
#ifdef CD_SIM
    if (goal < 0) sim_dump_grid(s_mocc, MG, MGC, s_mx0, s_mz0, pos[0], pos[2], h0, gx, gz, gh);
#endif
    if (goal < 0) { CLOG("[crewdrive] manoeuvre: no way found (%d tried, start %s, goal %.0f m away, got within %.1f)\n", pops,
                         start_contact ? "in contact" : m_free(pos[0], pos[2], h0) == 1 ? "tight" : "free", sqrtf((gx - pos[0]) * (gx - pos[0]) + (gz - pos[2]) * (gz - pos[2])), best_h); return 0; }
    /* the chain of moves, merged into pieces with the same direction and wheel angle */
    static int chain[MNODES]; int nc = 0; for (int c = goal; c > 0 && nc < MNODES; c = s_mn[c].parent) chain[nc++] = c;
    s_nmseg = 0;
    for (int i = nc - 1; i >= 0; i--) {
        const int c = chain[i]; const float st = (float)s_mn[c].st * 0.5f;
        if (s_nmseg && s_mseg[s_nmseg - 1].dir == s_mn[c].dir && fabsf(s_mseg[s_nmseg - 1].steer - st) < 0.01f) s_mseg[s_nmseg - 1].len += ds;
        else if (s_nmseg < 48) { s_mseg[s_nmseg].dir = s_mn[c].dir; s_mseg[s_nmseg].steer = st; s_mseg[s_nmseg].len = ds; s_nmseg++; }
    }
    /* only the part that matters: until the last reverse piece and one forward piece after it (then the lane driving takes over) */
    int last_rev = -1; for (int i = 0; i < s_nmseg; i++) if (s_mseg[i].dir < 0) last_rev = i;
    if (last_rev >= 0 && last_rev + 2 < s_nmseg) s_nmseg = last_rev + 2;
    else if (last_rev < 0 && s_nmseg > 3) s_nmseg = 3;
    M.active = 1; M.seg = 0; M.done = 0; M.t_seg = now_s(); M.t_last_move = M.t_seg; M.lx = pos[0]; M.lz = pos[2];
    CLOG("[crewdrive] manoeuvre: %d moves (%d nodes):", s_nmseg, pops);
    for (int i = 0; i < s_nmseg; i++) CLOG(" %s%.1fm@%+.1f", s_mseg[i].dir > 0 ? "F" : "R", s_mseg[i].len, s_mseg[i].steer);
    CLOG("\n");
    return 1;
}
/* driving the manoeuvre: wheel set first, then slowly along the piece; 1 = still busy (controls written to *steer/gas/brake/rev) */
static int man_tick(const float* pos, float speed, float vf, double t, float* want, float* gas, float* brake, int* rev)
{
    if (!M.active) return 0;
    const ManSeg* S = &s_mseg[M.seg];
    M.done += sqrtf((pos[0] - M.lx) * (pos[0] - M.lx) + (pos[2] - M.lz) * (pos[2] - M.lz)); M.lx = pos[0]; M.lz = pos[2];
    if (speed > 0.3f) M.t_last_move = t;
    /* something moving comes close on the side we go to: wait */
    int danger = 0;
    for (int i = 0; i < s_nobs; i++) { const Obs* o = &s_obs[i]; if (o->speed < 1.0f) continue;
        const float dx = o->x - pos[0], dz = o->z - pos[2]; if (dx * dx + dz * dz < 7.0f * 7.0f) danger = 1; }
    *want = S->steer; *rev = S->dir < 0;
    const float wheel_ok = fabsf(R.steer - S->steer) < 0.25f;
    if (danger || !wheel_ok) { *gas = 0; *brake = 1.0f; M.t_last_move = t; return 1; }
    const float vt = S->dir > 0 ? 2.2f : 1.8f, vdir = S->dir > 0 ? vf : -vf;
    *gas = vdir < vt ? 0.45f + 0.15f * (vt - vdir) : 0.0f; *brake = vdir > vt + 0.8f ? 0.4f : 0.0f; if (*gas > 0.8f) *gas = 0.8f;
    if (M.done >= S->len) {   /* next piece: stop first if the direction changes */
        if (M.seg + 1 >= s_nmseg) { M.active = 0; R.t_plan = 0; R.t_choose = 0; CLOG("[crewdrive] manoeuvre done\n"); return 0; }
        if (s_mseg[M.seg + 1].dir != S->dir && speed > 0.4f) { *gas = 0; *brake = 1.0f; return 1; }
        M.seg++; M.done = 0; M.t_seg = t; M.t_last_move = t;
    }
    if (t - M.t_last_move > 1.6) { M.active = 0; M.fails++; R.t_plan = 0; CLOG("[crewdrive] manoeuvre blocked (piece %d) -> plan again\n", M.seg); return 0; }
    return 1;
}
static void wake_car(uint32_t car, float fx, float fz)   /* a stopped car falls asleep in Havok: a push wakes it */
{
#ifdef CD_SIM
    return;
#endif
    const uint32_t body = car_body(car), buf = scratch(); if (!buf || !body_live(car, body)) return;
    float v[3] = { 0, 0, 0 }; car_vel(car, v);
    wrf(buf + 0xC0, v[0] + fx * 0.35f); wrf(buf + 0xC4, v[1] + 0.02f); wrf(buf + 0xC8, v[2] + fz * 0.35f); wrf(buf + 0xCC, 0.0f);
    const uint64_t a[2] = { body, buf + 0xC0 }; gcall_t(lookup(FN_SETLINVEL), HK_TOC, a, 2, NULL);
}

static void drive_tick(uint32_t pl)
{
    const double t = now_s(); float dt = (float)(t - R.t_last); R.t_last = t; if (dt <= 0.0f || dt > 0.1f) dt = 0.016f;
    R.frames++;
    if (!ok(R.car) || !alive(pl) || vehicle_of(pl) != R.car) { drive_end("the player left the car", 0); return; }
    if (R.driver && !alive(R.driver)) { drive_end("the driver is dead", 1); return; }
    if (R.driver && vehicle_of(R.driver) != R.car) {   /* he got out (a fight): the car waits, he takes the wheel again when he is back */
        CLOG("[crewdrive] the driver got out -> waiting for him\n");
        write_controls(0, 0, 1, 1, 0); R.bdrv = R.driver; R.st = D_BOARD; R.early = 1; R.t_board = now_s(); R.t_sm = 0; return; }
    const uint32_t s = car_status(R.hkv); if (!s) { drive_end("car controls gone", 0); return; }
    float rr[3], ru[3], ra[3], pos[3], vel[3] = { 0, 0, 0 };
    if (!ent_rows(R.car, rr, ru, ra, pos)) return;
    car_vel(R.car, vel);
    float fx = ra[0] * (float)s_fwd_sign, fz = ra[2] * (float)s_fwd_sign; { const float l = sqrtf(fx * fx + fz * fz); if (l > 1e-3f) { fx /= l; fz /= l; } }
    const float vf = vel[0] * fx + vel[2] * fz, speed = sqrtf(vel[0] * vel[0] + vel[2] * vel[2]);
    /* self-calibration (forward axis, steering sign, wheel angle) */
    if (R.gas > 0.3f && !R.rev && speed > 1.5f && R.cal_n < 120) { R.cal_dot += vf; R.cal_n++;
        if (R.cal_n == 120 && R.cal_dot < 0) { s_fwd_sign = -s_fwd_sign; CLOG("[crewdrive] calibration: the car's forward axis is -at -> flipped\n"); } }
    { float w = 0; if (car_avel_y(R.car, &w) && speed > 3.0f && fabsf(R.steer) > 0.15f && !R.rev) {
        R.cal_yaw += (double)(w * (R.steer > 0 ? 1.0f : -1.0f)); R.cal_yn++;
        const float est = fabsf(atanf(fabsf(w) * 2.9f / (speed > 1.0f ? speed : 1.0f))) / fabsf(R.steer);
        if (est > 0.1f && est < 1.2f) s_steer_gain += (est - s_steer_gain) * 0.01f;
        if (R.cal_yn == 90) { CLOG("[crewdrive] calibration: yaw vs steer %.2f (n %d), wheel angle at full lock ~%.2f rad\n", R.cal_yaw / R.cal_yn, R.cal_yn, s_steer_gain);
            if (R.cal_yaw / R.cal_yn < -0.15) { s_steer_sign = -s_steer_sign; CLOG("[crewdrive] calibration: steering sign flipped to %d\n", s_steer_sign); } } } }
    /* ---- battle */
    if (B.car != R.car || B.id != R.id) { memset(&B, 0, sizeof B); B.car = R.car; B.id = R.id; B.hp_last = -1; B.php = -1; B.dhp = -1; }   /* each drive starts calm */
    { float hp = 0, hpmax = 0; const int hp_ok = rdf_ok(R.car + 0x38Cu, &hp) && rdf_ok(R.car + 0x390u, &hpmax) && hpmax > 1.0f && hpmax < 1e7f && hp == hp;
      if (fabsf(speed - B.v_last) > 25.0f * dt) B.t_impact = t;   /* a bump (ours): its damage is not a fight */
      B.v_last = speed;
      const char* why = NULL;
      if (hp_ok) { if (B.hp_last < 0) { B.hp_last = hp; B.t_hp = t; }   /* damage over half a second (hits, or a fire), without a bump of ours in it */
                   else if (t - B.t_hp >= 0.5) { if (hp < B.hp_last - 0.008f * hpmax && t - B.t_impact > 0.8) why = "the car is taking hits"; B.hp_last = hp; B.t_hp = t;
#ifdef CD_SIM
                     if (getenv("CD_DBG_HP")) CLOG("[dbg] hp %.0f / %.0f impact %.1f s ago\n", hp, hpmax, t - B.t_impact);
#endif
                   } }
      { float ph; if (rdf_ok(pl + 0x14C8u, &ph) && ph == ph) { if (B.php > 0 && ph < B.php - 0.5f) why = "the boss is hit"; B.php = ph; } }
      if (R.driver) { float dh; if (rdf_ok(R.driver + 0x14C8u, &dh) && dh == dh) { if (B.dhp > 0 && dh < B.dhp - 0.5f) why = "the driver is hit"; B.dhp = dh; } }
      if (why) { if (!(B.t_danger > 0 && t - B.t_danger < 25.0)) { CLOG("[crewdrive] BATTLE: %s -> getting out of here fast\n", why);
                     if (t - B.t_said > 20.0) { speak(R.driver, V_DANGER, "Hold on, boss! I'm getting us out of here!", 3.0); B.t_said = t; } }
                 B.t_danger = t; R.t_choose = 0; }
      /* on fire (the game sets the fire effect once the car is below its last 10 %): stop, everybody out */
      uint32_t burn = 0; const int burning = rd32_ok(R.car + 0x71Cu, &burn) && burn != 0;
      if ((burning || (hp_ok && B.hp_last >= 0 && hp < 0.08f * hpmax)) && !B.bail) {
          B.bail = t; speak(R.driver, V_BAIL, "She's gonna blow! Everybody out!", 3.0);
          CLOG("[crewdrive] the car is %s (health %.0f / %.0f) -> stopping, everybody out\n", burning ? "on fire" : "nearly wrecked", hp, hpmax); }
      if (B.bail) {
          write_controls(R.steer * (float)s_steer_sign, 0, 1, speed < 6.0f, 0);
          if (speed < 1.5f || t - B.bail > 2.5) {
              drive_end("the car is on fire: everybody out", 0);
              g_pad_inject_d1 = 0; g_pad_inject_d2 = PAD_TRIANGLE >> 8; g_pad_inject_polls = 8;   /* the boss gets out (the crew follows him) */
          }
          return;
      }
    }
    const int danger = B.t_danger > 0 && t - B.t_danger < 25.0;
    const int mode = (R.fast > 0 || danger) ? 1 : R.fast;   /* -1 easy, 0 normal, 1 fast (Step on it, or a fight) */
    s_aggr = danger ? 2 : R.fast > 0 ? 1 : 0;
    const float vmax = mode > 0 ? s_fast : mode < 0 ? s_easy : s_cruise, alat = mode > 0 ? 9.5f : mode < 0 ? 4.5f : 7.5f, adec = mode > 0 ? 8.5f : mode < 0 ? 5.0f : 7.0f;
    if (R.kind >= 0) { static double tr; if (t - tr > 1.0) { tr = t; target_refresh(pos); } }
    float lat = 0;
    if (t - R.t_plan > 0.5 || s_np < 2) {
        R.t_plan = t;
        if (!build_path(pos, fx, fz, &lat)) { write_controls(0, 0, 1, 1, 0); if (R.frames % 120 == 1) CLOG("[crewdrive] no route from here\n"); return; }
        plan_speeds(vmax, alat, adec);
        R.ip = 0;
    }
    { float bd = BIG; int bi = R.ip; const int lo = R.ip > 3 ? R.ip - 3 : 0, hi = R.ip + 30 < s_np ? R.ip + 30 : s_np;
      for (int i = lo; i < hi; i++) { const float dx = s_px[i] - pos[0], dz = s_pz[i] - pos[2], d = dx * dx + dz * dz; if (d < bd) { bd = d; bi = i; } }
      R.ip = bi; lat = sqrtf(bd); }
    const float rest = (float)(s_np - 1 - R.ip) * PSTEP + lat;
    const float dend = sqrtf((s_end_x - pos[0]) * (s_end_x - pos[0]) + (s_end_z - pos[2]) * (s_end_z - pos[2]));
    if (s_path_ends && (rest < 5.0f || dend < 6.0f) && speed < 1.2f) {
        if (R.st != D_ARRIVED) { R.st = D_ARRIVED; R.t_arrive = t; speak(R.driver, V_ARRIVE, "We're here, boss.", 3.0);
            CLOG("[crewdrive] ARRIVED %.1f m from the stop point, %.0f s\n", rest, t - R.t_start); }
        write_controls(0, 0, 1, 1, 0); return;
    }
    if (R.st == D_ARRIVED) { if (rest > 12.0f) R.st = D_DRIVE; else { write_controls(0, 0, 1, 1, 0); return; } }
    /* which lane offset: our lane, further left (overtake / oncoming lane) or right (kerb side) -- every 0.15 s */
    { float tx1, tz1, nx1, nz1; path_frame(R.ip, &tx1, &tz1, &nx1, &nz1); s_l_act = (pos[0] - s_px[R.ip]) * nx1 + (pos[2] - s_pz[R.ip]) * nz1; }
    const int jammed = R.t_stop > 0 && t - R.t_stop > (danger ? 1.0 : 5.0);   /* stopped a while: squeeze through with smaller margins */
    const float margin = jammed ? 0.05f : mode > 0 ? 0.25f : 0.45f;
    if (t - R.t_choose > 0.15) {
        R.t_choose = t; const double pc0 = perf_ms();
        collect_obs(R.car, pos);
        static const float cand_n[] = { 0.0f, -1.7f, -3.4f, -4.8f, 1.4f, 2.8f };
        static const float cand_s[] = { 0.0f, -1.7f, 1.4f };
        const float* cand = mode < 0 ? cand_s : cand_n; const int nc = mode < 0 ? 3 : (jammed ? 6 : 5);
        float bestc = R.c_want, bestcost = BIG, curcost = BIG, bvl = BIG, bF = 0; int bkd = 0, room_alt = 0;
        for (int i = 0; i < nc; i++) {
            const float c = cand[i]; float vl; int kd;
            if (c > 0.5f && speed > 4.0f && R.c_want < 0.5f && !(R.kind_cur == 5 && R.free_cur < 45.0f) && !(s_aggr == 2 || (s_aggr && R.kind_cur == 2))) continue;   /* the parking side: only creeping past something standing, or round a wall / post in our lane */
            const float sf = static_free(c, pos[1] + 0.75f), side = s_side_clear;
            s_room_need = 0;
            const float F = lane_free(c, speed, margin, adec, &vl, &kd, sf);
            if (s_room_need && fabsf(c - R.c_want) > 0.5f && sf > 20.0f) room_alt = 1;
            /* stuck behind a slower car in that lane: the speed we would lose counts too (so we pass when the other lane is clear) */
            const float slow = (kd == 2 || (s_aggr == 2 && kd == 1)) && vl < vmax ? (vmax - fmaxf(vl, 0.0f)) * (s_aggr == 2 ? 3.0f : s_aggr ? 2.2f : 1.4f) : 0.0f;
            const float cost = -fminf(F, 70.0f) + 1.6f * fabsf(c) + (c < -2.6f ? 4.0f : 0.0f) + 0.4f * fabsf(c - R.c_want) + (side < 1.6f ? (1.6f - side) * 10.0f : 0.0f) + slow;
            if (s_logv && ((R.t_stop > 0 && t - R.t_stop > 5.0 && fmod(t - R.t_stop, 5.0) < 0.16) || (R.kind_cur == 2 && speed < 0.6f * vmax && fmod(t, 3.0) < 0.16)))
                CLOG("[crewdrive]   waiting: lane %+.1f free %.1f m (walls %.0f m) speed cap %.1f kind %d room %d cost %.1f\n", c, F, sf, vl, kd, s_room_need, cost);
            if (fabsf(c - R.c_want) < 0.01f) { curcost = cost; R.vlim_cur = vl; R.kind_cur = kd; R.free_cur = F; }
            if (cost < bestcost) { bestcost = cost; bestc = c; bvl = vl; bkd = kd; bF = F; }
        }
        { const double pc1 = perf_ms(); if (pc1 - pc0 > 4.0) CLOG("[crewdrive] slow lane check: %.1f ms at (%.0f, %.0f)\n", pc1 - pc0, pos[0], pos[2]); }
        const float hyst = fabsf(bestc) < fabsf(R.c_want) ? 0.8f : (s_aggr ? 1.0f : 2.0f);   /* back into our lane easily, out of it only for a reason */
        /* a decision stands for 1.2 s (no flip-flopping) unless the chosen lane turns dangerous */
        static double t_commit;
        const int committed = t - t_commit < 1.2 && curcost < BIG && R.free_cur > 2.0f + 0.3f * speed;
        if (fabsf(bestc - R.c_want) > 0.01f && !committed && (curcost >= BIG || curcost - bestcost > hyst)) {
            if (s_logv) CLOG("[crewdrive] lane offset %.1f -> %.1f (free %.0f m)\n", R.c_want, bestc, bF);
            R.c_want = bestc; R.vlim_cur = bvl; R.kind_cur = bkd; R.free_cur = bF; t_commit = t;
        }
    }
    { const float rate = ((s_aggr ? 0.75f : 0.5f) * speed + 0.4f) * dt;   /* sideways only as we roll forward */
      float d = R.c_want - R.c_cur; if (d > rate) d = rate; if (d < -rate) d = -rate; R.c_cur += d; }
    /* pure pursuit on the shifted lane */
    const float Ld = 5.0f + 0.42f * (speed > 0 ? speed : 0); const int look = R.ip + (int)(Ld / PSTEP + 0.5f) < s_np ? R.ip + (int)(Ld / PSTEP + 0.5f) : s_np - 1;
    float tx0, tz0, nlx, nlz; path_frame(look, &tx0, &tz0, &nlx, &nlz);
    const float endfade = rest < 25.0f ? rest / 25.0f : 1.0f;   /* back into the lane before stopping */
    const float tx = s_px[look] + nlx * R.c_want * endfade - pos[0], tz = s_pz[look] + nlz * R.c_want * endfade - pos[2];
    float alpha = atan2f(tx, tz) - atan2f(fx, fz); while (alpha > 3.14159265f) alpha -= 6.2831853f; while (alpha < -3.14159265f) alpha += 6.2831853f;
    const float dist_l = fmaxf(sqrtf(tx * tx + tz * tz), 2.0f);
    float want = atan2f(2.0f * 2.9f * sinf(alpha), dist_l) / (s_steer_gain > 0.2f ? s_steer_gain : 0.2f);
    if (fabsf(alpha) > 1.6f) want = alpha > 0 ? 1.0f : -1.0f;
    if (want > 1) want = 1; if (want < -1) want = -1;
    /* speed */
    float vt = s_pv[R.ip + 1 < s_np ? R.ip + 1 : R.ip];
    if (s_path_ends && vt < 1.5f && dend >= 6.0f) vt = 1.5f;   /* the plan stops at the end point: roll on until we are there */
    if (s_path_ends && dend < 16.0f) {   /* the last metres: gently down to a stop within 6 m of it (no circling round it) */
        const float vs = sqrtf(2.0f * 2.5f * fmaxf(dend - 4.0f, 0.0f)); if (vt > vs) vt = dend >= 6.0f ? fmaxf(vs, 1.5f) : 0.0f; }
    if (fabsf(alpha) > 0.9f && vt > 7.0f) vt = 7.0f;
    if (R.vlim_cur < vt) vt = R.vlim_cur;
    if (!s_aggr && fabsf(R.c_cur - R.c_want) > 1.0f && R.free_cur < 8.0f + 0.4f * speed && vt > 5.0f) vt = 5.0f;   /* still moving over */
    if (!s_aggr && fabsf(R.c_want) > 0.5f && vt > vmax * 0.85f) vt = vmax * 0.85f;
    { static double t_ppl, t_creep_end;
      if (speed < 0.6f && R.kind_cur == 4 && vt < 0.6f && !s_ignore_people) { if (!t_ppl) t_ppl = t; } else if (!s_ignore_people) t_ppl = 0;
      if (t_ppl && t - t_ppl > (danger ? 0.7 : 2.5) && !s_ignore_people) { s_ignore_people = 1; t_creep_end = t + 4.0; t_ppl = 0; R.t_choose = 0; CLOG("[crewdrive] people stay in the road -> creeping through slowly (they step aside)\n"); }
      if (s_ignore_people) { if (t > t_creep_end) { s_ignore_people = 0; R.t_choose = 0; } else if (vt > (danger ? 3.5f : 2.0f)) vt = danger ? 3.5f : 2.0f; } }
    /* tight spots: stopped behind something that does not move (not a moving queue), or a U-turn with a wall close ahead ->
     * the manoeuvre planner; while a manoeuvre runs it drives the car */
    float gas = 0, brake = 0; int hb = 0, rev = 0;
    { static double t_block;
      if (M.id != R.id) { memset(&M, 0, sizeof M); M.id = R.id; t_block = 0; }   /* a new drive: forget the last one's manoeuvres */
      int moving_ahead = 0;
      for (int i = 0; i < s_nobs; i++) { const Obs* o = &s_obs[i]; if (o->s > 0 && o->s < 14.0f && fabsf(o->l - s_l_act) < o->hw + 1.3f && o->vs > 1.0f) moving_ahead = 1; }   /* a queue driving away (not one nosing at us) */
      const int blocked = speed < 0.5f && vt < 0.6f && R.free_cur < 10.0f && (R.kind_cur == 1 || R.kind_cur == 5 || (R.kind_cur == 4 && R.t_stop > 0 && t - R.t_stop > 4.0)) && !moving_ahead;   /* people who stay put (a stall's vendor): drive round them too */
      if (blocked && !M.active) { if (!t_block) t_block = t; } else t_block = 0;
#ifdef CD_SIM
      if (speed < 0.5f && R.t_stop > 0 && t - R.t_stop > 5.0 && fmod(t - R.t_stop, 5.0) < 0.02) {
          CLOG("[dbg] blocked %d vt %.2f free %.1f kind %d moving %d fails %d rev_end %.1f t %.1f\n", blocked, vt, R.free_cur, R.kind_cur, moving_ahead, M.fails, R.t_rev_end, t);
          for (int i = 0; i < s_nobs; i++) { const Obs* o = &s_obs[i]; if (o->s > -5 && o->s < 20) CLOG("[dbg]   obs s %.1f l %.1f vs %.1f speed %.1f car %d hw %.1f\n", o->s, o->l, o->vs, o->speed, o->car, o->hw); } }
#endif
      int uturn = 0;
      if (!M.active && fabsf(alpha) > 1.9f && speed < 4.0f) { float bt; const float o3[3] = { pos[0], pos[1] + 0.75f, pos[2] }, fd[3] = { fx, 0, fz }; uturn = cd_ray(o3, fd, 9.0f, &bt); }
      if (!M.active && R.t_rev_end <= t && ((t_block && t - t_block > (danger ? 0.7 : 1.5)) || uturn)) {
          if (t - M.t_win > 30.0 || t < M.t_win) { M.t_win = t; M.fails = 0; }
          t_block = 0;
          if (M.fails < 3) { CLOG("[crewdrive] %s -> planning a manoeuvre\n", uturn ? "U-turn in a narrow street" : "blocked by something standing"); if (man_plan(pos, atan2f(fx, fz), pos[1]) == 0) M.fails++; }
      }
      if (M.active) { float mw = want, mg = 0, mb = 0; int mr = 0; if (man_tick(pos, speed, vf, t, &mw, &mg, &mb, &mr)) { want = mw; gas = mg; brake = mb; rev = mr; R.t_stuck = 0; goto controls; } } }
    if (R.t_rev_end > t) { rev = 1; gas = 0.6f; want = R.rev_steer; if (s_rev_mode) { gas = 0; brake = 0.6f; } }
    else {
        const float err = vt - vf;
        if (err > 0.3f) gas = 0.3f + 0.25f * err; else if (err > -0.6f) gas = 0.12f * (err + 0.6f);
        if (err < -0.8f) { brake = 0.22f * (-err); gas = 0; }
        if (vt < vf && vf > 1.0f && R.free_cur < 45.0f && R.kind_cur != 2) {   /* something standing ahead: brake as hard as stopping in time needs */
            const float room = fmaxf(R.free_cur - 1.5f, 0.3f), areq = vf * vf / (2.0f * room);
            if (areq > 2.5f && areq / 8.0f > brake) { brake = areq / 8.0f; gas = 0; } }
        if (vt < 0.3f) { gas = 0; brake = speed > 0.5f ? 0.9f : 1.0f; }
        if (gas > 1) gas = 1; if (brake > 1) brake = 1;
        if (vf < -0.5f && vt > 1.0f) { brake = 0.6f; gas = 0; }
        if (speed < 0.5f) { if (R.t_stop <= 0) R.t_stop = t; } else R.t_stop = 0;
        /* wake the car if we want to go and it does not move */
        if (gas > 0.05f && speed < 0.4f && t - R.t_wake > 0.4) { R.t_wake = t; wake_car(R.car, fx, fz); }
        /* stuck: pedal down, nothing known in the way, not moving */
        if ((gas > 0.3f && speed < 0.6f && vt > 2.0f) || (speed < 0.5f && R.kind_cur == 5 && R.free_cur < 6.0f)) { if (!R.t_stuck) R.t_stuck = t; } else R.t_stuck = 0;
        if (R.t_stuck && t - R.t_stuck > 1.5) {
            R.t_stuck = 0;
            if (t - R.t_unstick_win > 25.0) { R.t_unstick_win = t; R.unstick_n = 0; }
            R.unstick_n++;
            CLOG("[crewdrive] stuck at (%.1f, %.1f) -> backing up (#%d)\n", pos[0], pos[2], R.unstick_n);
            if (R.unstick_n >= 4) {
                const int k = R.ip + 5 < s_np ? R.ip + 5 : s_np - 1, k2 = k + 1 < s_np ? k + 1 : k;
                float hx = s_px[k2] - s_px[k], hz = s_pz[k2] - s_pz[k]; if (fabsf(hx) + fabsf(hz) < 1e-3f) { hx = fx; hz = fz; }
                if (place_car(R.car, s_px[k], pos[1] + 0.6f, s_pz[k], hx, hz)) CLOG("[crewdrive] set the car back on the road 10 m ahead\n");
                R.unstick_n = 0;
            } else if (!(M.fails < 3 && man_plan(pos, atan2f(fx, fz), pos[1]) > 0)) {
                R.t_rev_end = t + 1.3; R.rev_steer = want > 0 ? -1.0f : 1.0f; wake_car(R.car, -fx, -fz); s_cs.ok = 0; s_ce.ok = 0; }
        }
    }
    if (ru[1] < 0.25f) { if (R.t_flip <= 0) R.t_flip = t; if (t - R.t_flip > 2.5) { R.t_flip = 0; place_car(R.car, pos[0], pos[1] + 1.0f, pos[2], fx, fz); CLOG("[crewdrive] car was on its roof/side -> set upright\n"); } } else R.t_flip = 0;
    { static int rev_ok; if (rev && vf < -0.8f) rev_ok = 1;
      if (!rev_ok && rev && R.t_rev_end - t < 0.7 && R.t_rev_end - t > 0.65 && vf > 0.3f) { s_rev_mode = !s_rev_mode; CLOG("[crewdrive] reverse did not go backwards -> reverse mode %d\n", s_rev_mode); } }
controls:
    { const float maxd = (mode > 0 ? 4.5f : 3.5f) * dt; float d = want - R.steer; if (d > maxd) d = maxd; if (d < -maxd) d = -maxd; R.steer += d; }
    R.gas = gas; R.brake = brake; R.rev = rev; R.hb = hb;
    write_controls(R.steer * (float)s_steer_sign, gas, brake, hb, rev);
    if (t - R.t_log > (s_logv ? 0.5 : 2.0)) { R.t_log = t;
        static const char* const kn[] = { "", " | car ahead", " | following a car", " | car crossing", " | person ahead", " | wall/post ahead", " | oncoming car" };
        CLOG("[crewdrive] t %.0f pos %.1f %.1f v %.1f -> %.1f | steer %.2f gas %.2f brake %.2f%s | lane %+.1f (want %+.1f) err %.1f, free %.0f m, rest %.0f m%s\n",
             t - R.t_start, pos[0], pos[2], speed, vt, R.steer, gas, brake, rev ? " REV" : "", R.c_cur, R.c_want, lat, R.free_cur, rest, kn[R.kind_cur >= 0 && R.kind_cur < 7 ? R.kind_cur : 0]); }
}
/* which side does the traffic drive on? signed offset of moving traffic cars from the nearest road, along their motion */
static void side_watch(uint32_t mycar)
{
    static double tl; const double t = now_s(); if (t - tl < 0.5) return; tl = t;
    uint32_t cars[128]; int nc = 0; __try { nc = gf_world_cars(cars, 128); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    for (int k = 0; k < nc && k < 24; k++) {
        const uint32_t c = cars[k]; if (c == mycar) continue;
        float p[3], v[3]; if (!ent_pos(c, p) || !car_vel(c, v)) continue;
        const float sp = sqrtf(v[0] * v[0] + v[2] * v[2]); if (sp < 4.0f) continue;
        float bd = BIG, bo = 0;
        for (int vv = 0; vv < s_nn; vv++) { const float ax = NX(vv), az = NZ(vv); if (fabsf(ax - p[0]) > 140 || fabsf(az - p[2]) > 140) continue;
            for (int e = s_rs[vv]; e < s_rs[vv + 1]; e++) {
                const int u = s_rsrc[e]; const float ux = NX(u), uz = NZ(u), ex = ax - ux, ez = az - uz, L = sqrtf(ex * ex + ez * ez); if (L < 5) continue;
                const float dx = ex / L, dz = ez / L; if (dx * v[0] + dz * v[2] < 0.8f * sp) continue;   /* edges in its direction */
                float tt = (p[0] - ux) * dx + (p[2] - uz) * dz; if (tt < 0 || tt > L) continue; if (!y_ok(u, vv, tt / L, p[1], 3.5f)) continue;
                const float cx = ux + dx * tt, cz = uz + dz * tt, d = sqrtf((p[0] - cx) * (p[0] - cx) + (p[2] - cz) * (p[2] - cz));
                if (d < bd) { bd = d; bo = (p[0] - cx) * (-dz) + (p[2] - cz) * dx; } } }
        if (bd < 9.0f && fabsf(bo) > 0.8f) { s_side_sum += bo > 0 ? 1.0 : -1.0; s_side_n++; }
    }
    if (s_side_n >= 25) {
        const int want = s_side_sum >= 0 ? 1 : -1;
        if (want != s_side) CLOG("[crewdrive] traffic drives on the other side (%.0f of %d) -> lane side %d\n", s_side_sum, s_side_n, want);
        else if (s_logv) CLOG("[crewdrive] traffic side check: %.0f of %d -> side %d\n", s_side_sum, s_side_n, want);
        s_side = want; s_side_sum *= 0.5; s_side_n /= 2;
    }
}

/* ================================================================ menu */
enum { M_CLOSED, M_MAIN, M_DEST };
enum { A_YOUDRIVE, A_CHANGE, A_PULLOVER, A_ILLDRIVE, A_SPEED, A_LEAVE, A_DEST0 };
static struct { int st; int acts[5]; int n; double t_open; uint32_t crew; int saved_key; } Mn = { M_CLOSED };
static void menu_show(const char* title)
{
    for (int i = 0; i < Mn.n; i++) {
        const int a = Mn.acts[i]; const char* l = "";
        switch (a) { case A_YOUDRIVE: l = "You drive"; break; case A_CHANGE: l = "Change destination"; break; case A_PULLOVER: l = "Pull over here"; break;
                     case A_ILLDRIVE: l = "I'll drive"; break; case A_SPEED: l = R.fast > 0 ? "Take it easy" : "Step on it"; break; case A_LEAVE: l = "Leave"; break;
                     default: l = k_kind_label[a - A_DEST0]; break; }
        snprintf(g_gfd_opt[i], 40, "%s", l);
    }
    g_gfd_nopt = Mn.n; if (g_gfd_sel < 0 || g_gfd_sel >= Mn.n) g_gfd_sel = 0;
    snprintf(g_gfd_who, sizeof g_gfd_who, "%s", title);
    if (!g_gfd_menu) { Mn.saved_key = g_gfd_key; g_gfd_key = s_key; }
    g_gfd_menu = 1; Mn.t_open = now_s();
}
static void menu_close(void)
{
    if (Mn.st == M_CLOSED) return;
    Mn.st = M_CLOSED; g_gfd_menu = 0; g_gfd_close_t = GetTickCount64(); g_gfd_clear = 1; if (Mn.saved_key) g_gfd_key = Mn.saved_key;
}
static void menu_main(uint32_t pl)
{
    Mn.n = 0;
    if (R.st == D_DRIVE || R.st == D_ARRIVED) { Mn.acts[Mn.n++] = A_CHANGE; Mn.acts[Mn.n++] = A_PULLOVER; Mn.acts[Mn.n++] = A_ILLDRIVE; Mn.acts[Mn.n++] = A_SPEED; }
    else Mn.acts[Mn.n++] = A_YOUDRIVE;
    Mn.acts[Mn.n++] = A_LEAVE;
    Mn.st = M_MAIN; g_gfd_sel = 0; (void)pl;
    char nm[48]; crew_name(Mn.crew, nm, sizeof nm); menu_show(nm);
}
static int menu_dest(uint32_t pl)
{
    float pos[3]; if (!ent_pos(pl, pos)) return 0;
    Mn.n = 0;
    static const int order[K_N] = { K_BLUE, K_PINK, K_YELLOW, K_CUSTOM };
    for (int i = 0; i < K_N; i++) { float p[3]; if (find_marker(order[i], pos, p, NULL)) Mn.acts[Mn.n++] = A_DEST0 + order[i]; }
    char nm[48]; crew_name(Mn.crew, nm, sizeof nm);
    if (!Mn.n) { speak(Mn.crew, V_NO, "There's nothing marked on the map, boss.", 3.0); return 0; }
    Mn.acts[Mn.n++] = A_LEAVE;
    Mn.st = M_DEST; g_gfd_sel = 0;
    speak(Mn.crew, V_WHERE, "Where to, boss?", 3.0);
    menu_show("Where to, boss?");
    return 1;
}
static void choose_dest(uint32_t pl, int kind)
{
    char nm[48]; crew_name(Mn.crew, nm, sizeof nm);
    float pos[3]; ent_pos(pl, pos);
    float p[3]; int slot = -1; if (!find_marker(kind, pos, p, &slot)) { speak(Mn.crew, V_NO, "I can't find it on the map, boss.", 3.0); return; }
    R.kind = kind; R.slot = slot; R.tgt[0] = p[0]; R.tgt[1] = p[1]; R.tgt[2] = p[2];
    char line[120];
    if (R.st == D_DRIVE || R.st == D_ARRIVED) { s_have_target = 0; target_refresh(pos); snprintf(line, sizeof line, "Sure thing. %s it is.", k_kind_sayc[kind]); speak(Mn.crew, V_OK, line, 3.0); R.st = D_DRIVE; return; }
    R.bdrv = Mn.crew; R.early = 0; R.fast = 0; R.redir = 0;
    const uint32_t car = vehicle_of(pl);
    if (car && seat_of(car, pl) == 0) { R.st = D_BOARD; R.t_board = now_s(); R.car = car; snprintf(line, sizeof line, "Okay boss, %s. Let me get behind the wheel.", k_kind_say[kind]); speak(Mn.crew, V_OK, line, 3.5); }
    else { R.st = D_BOARD; R.t_board = now_s(); R.car = 0; snprintf(line, sizeof line, "Okay boss, %s. Get in a car, I'll drive.", k_kind_say[kind]); speak(Mn.crew, V_GETIN, line, 3.5); }
}
static unsigned s_pe;   /* this frame's new pad presses (pad_edges once a frame) */
static void menu_tick(uint32_t pl, int open_req)
{
    const unsigned pe = pad_edges(); s_pe = pe;
    if (Mn.st == M_CLOSED) {
        if (!open_req) return;
        uint32_t cl[5]; const int n = crew_list(pl, cl);
        if (!n) { say("", "You have no crew with you.", 2.5); return; }
        /* the crew member who will drive: the one at the wheel already, else the first-hired */
        Mn.crew = (R.st != D_OFF && R.driver) ? R.driver : cl[0];
        menu_main(pl); return;
    }
    const int up = kedge(1, 'W') | kedge(2, VK_UP) | ((pe & PAD_UP) != 0), dn = kedge(3, 'S') | kedge(4, VK_DOWN) | ((pe & PAD_DOWN) != 0);
    const int go = kedge(5, VK_RETURN) | kedge(6, VK_SPACE) | ((pe & PAD_CROSS) != 0);
    const int back = kedge(7, VK_BACK) | kedge(8, VK_ESCAPE) | open_req | ((pe & PAD_CIRCLE) != 0);
    int pick = -1; for (int i = 0; i < 5; i++) if (kedge(9 + i, '1' + i)) pick = i;
    if (pick >= Mn.n) pick = -1;
    if (back) { if (Mn.st == M_DEST) { menu_main(pl); return; } menu_close(); return; }
    if (up) g_gfd_sel = (g_gfd_sel + Mn.n - 1) % Mn.n;
    if (dn) g_gfd_sel = (g_gfd_sel + 1) % Mn.n;
    if (pick >= 0) g_gfd_sel = pick;
    if (!(go || pick >= 0)) { if (now_s() - Mn.t_open > 40.0) menu_close(); return; }
    const int a = Mn.acts[g_gfd_sel];
    char nm[48]; crew_name(Mn.crew, nm, sizeof nm);
    switch (a) {
    case A_YOUDRIVE: case A_CHANGE: if (!menu_dest(pl)) menu_close(); return;
    case A_PULLOVER: if (R.st == D_DRIVE && s_np > 1) { R.kind = -2; g_gps_force_slot = -1; const int k = R.ip + 12 < s_np ? R.ip + 12 : s_np - 1; R.tgt[0] = s_px[k]; R.tgt[2] = s_pz[k]; { float cp[3]; R.tgt[1] = ent_pos(R.car, cp) ? cp[1] : NAN; } set_target_pos(s_px[k], R.tgt[1], s_pz[k]); }
        speak(Mn.crew, V_PULL, "Pulling over, boss.", 2.5); menu_close(); return;
    case A_ILLDRIVE: speak(Mn.crew, V_YOURS, "Sure, boss. All yours.", 2.5); menu_close(); drive_end("the player takes the wheel", 1); return;
    case A_SPEED: R.fast = R.fast > 0 ? 0 : 1; speak(Mn.crew, R.fast > 0 ? V_FAST : V_EASY, R.fast > 0 ? "Hold on to something." : "Nice and easy, boss.", 2.5); menu_close(); return;
    case A_LEAVE: menu_close(); return;
    default: menu_close(); choose_dest(pl, a - A_DEST0); return;
    }
}

/* ================================================================ self test (GF_CREWDRIVE_TEST=1): no crew needed */
static struct { int st; double t; uint32_t car; int tries; } T;
static int test_pick_far_road(const float* pos, float* out)
{
    /* a road node 350..600 m away by road from the nearest node */
    int nn = -1; float bd = BIG;
    for (int i = 0; i < s_nn; i++) { const float dx = NX(i) - pos[0], dz = NZ(i) - pos[2], d = dx * dx + dz * dz; if (d < bd) { bd = d; nn = i; } }
    if (nn < 0) return 0;
    solve(nn, s_distA, s_nextA);   /* dist to nn = (roughly) from nn, the graph is mostly two-way */
    int best = -1; float bt = 0;
    for (int i = 0; i < s_nn; i++) if (s_distA[i] > 350.0f && s_distA[i] < 600.0f) { const float sc = s_distA[i] + (float)((i * 2654435761u) % 97u); if (sc > bt) { bt = sc; best = i; } }
    if (best < 0) return 0;
    out[0] = NX(best); out[1] = pos[1]; out[2] = NZ(best);
    CLOG("[crewdrive] test: far road node %d at (%.1f, %.1f), %.0f m by road\n", best, out[0], out[2], s_distA[best]);
    return 1;
}
static void test_tick(uint32_t pl)
{
    const double t = now_s();
    if (T.st == 0) { if (!g_bench_world_ms) return; T.st = 1; T.t = t + 12.0; CLOG("[crewdrive] test: world loaded, starting in 12 s\n"); return; }
    if (t < T.t) return;
    float pp[3]; if (!ent_pos(pl, pp)) return;
    switch (T.st) {
    case 1: {   /* nearest car: stand by its left side and press Triangle */
        uint32_t cars[128]; const int nc = gf_world_cars(cars, 128); uint32_t best = 0; float bd = 70.0f;
        for (int k = 0; k < nc; k++) { float p[3], v[3] = { 0, 0, 0 }; if (ent_pos(cars[k], p) && car_hkv(cars[k])) { car_vel(cars[k], v); const float d = dist2d(p, pp) + (fabsf(v[0]) + fabsf(v[2]) > 1.0f ? 40.0f : 0.0f) + (T.tries / 2) * 15.0f * (float)((cars[k] >> 4) % 3); if (d < bd) { bd = d; best = cars[k]; } } }
        if (!best) { CLOG("[crewdrive] test: no car within 70 m (%d cars)\n", nc); T.t = t + 3.0; if (++T.tries > 20) T.st = 99; return; }
        float r[3], u[3], a[3], p[3]; ent_rows(best, r, u, a, p);
        const float side = (T.tries & 1) ? 1.0f : -1.0f;
        float tgt[3] = { p[0] + r[0] * 2.3f * side, p[1] + 0.2f, p[2] + r[2] * 2.3f * side };
        /* teleport the player next to it (SetMatrix vt+0x30 + position vt+0x24, like gf_sit.c) */
        const uint32_t xf = rd32(pl + 0x68u), buf = scratch();
        if (ok(xf) && buf) { for (uint32_t k = 0; k < 64; k += 4) wr32(buf + k, rd32(xf + 0x10u + k)); wrf(buf + 0x30, tgt[0]); wrf(buf + 0x34, tgt[1]); wrf(buf + 0x38, tgt[2]);
            const uint64_t aa[1] = { buf }; vcall(pl, 0x30u, aa, 1, NULL);
            const uint32_t blk = buf + 0x40u; wrf(blk, tgt[0]); wrf(blk + 4, tgt[1]); wrf(blk + 8, tgt[2]); wr32(blk + 12, 2u); const uint64_t bb[1] = { blk }; vcall(pl, 0x24u, bb, 1, NULL); }
        CLOG("[crewdrive] test: car %08X %.1f m away (seats %d, driver %08X, speed?) -> player put at its %s side, pressing Triangle\n", best, bd, seat_count(best), seat_occ(best, 0), side < 0 ? "left" : "right");
        T.car = best; T.st = 2; T.t = t + 1.0; return; }
    case 2: g_pad_inject_d1 = 0; g_pad_inject_d2 = PAD_TRIANGLE >> 8; g_pad_inject_polls = 8; T.st = 3; T.t = t + 7.0; return;
    case 3: {
        const uint32_t car = vehicle_of(pl);
        if (car && seat_of(car, pl) == 0) { CLOG("[crewdrive] test: player is driving car %08X (seats %d)\n", car, seat_count(car)); T.car = car; T.st = 4; T.t = t + 2.0; return; }
        CLOG("[crewdrive] test: not in a car (vehicle %08X seat %d) -> retry\n", car, car ? seat_of(car, pl) : -1); T.tries++; T.st = T.tries > 12 ? 99 : 1; T.t = t + 1.0; return; }
    case 4: {
        float p[3]; int slot = -1, kind = -1;
        static const int order[K_N] = { K_CUSTOM, K_BLUE, K_YELLOW, K_PINK };
        for (int i = 0; i < K_N && kind < 0; i++) if (find_marker(order[i], pp, p, &slot) && dist2d(p, pp) > 150.0f && dist2d(p, pp) < 1500.0f) kind = order[i];
        if (kind >= 0) { R.kind = kind; R.tgt[0] = p[0]; R.tgt[1] = p[1]; R.tgt[2] = p[2]; CLOG("[crewdrive] test: driving to the %s, %.0f m\n", k_kind_label[kind], dist2d(p, pp)); }
        else if (test_pick_far_road(pp, R.tgt)) R.kind = -1;
        else { T.st = 99; return; }
        R.fast = 0;
        if (!drive_begin(pl, T.car, 0)) { CLOG("[crewdrive] test: drive_begin failed\n"); T.st = 99; return; }
        T.st = 5; T.t = t + 0.0; return; }
    case 5:
        if (R.st == D_ARRIVED && t - R.t_arrive > 4.0) { CLOG("[crewdrive] test: arrived -> taking the wheel back, then a second trip\n"); drive_end("test", 1); T.st = 6; T.t = t + 3.0; return; }
        if (R.st == D_OFF) { CLOG("[crewdrive] test: the drive stopped\n"); T.st = 99; return; }
        if (t - R.t_start > 240.0) { CLOG("[crewdrive] test: 240 s without arriving\n"); drive_end("test timeout", 1); T.st = 6; T.t = t + 3.0; }
        return;
    case 6: {   /* second trip: another far road, fast */
        const uint32_t car = vehicle_of(pl);
        CLOG("[crewdrive] test: after the swap back the player is in seat %d of %08X\n", car ? seat_of(car, pl) : -1, car);
        if (!car || seat_of(car, pl) != 0 || !test_pick_far_road(pp, R.tgt)) { T.st = 7; T.t = t + 1.0; return; }
        R.kind = -1; R.fast = 1; if (!drive_begin(pl, car, 0)) { T.st = 7; return; }
        T.st = 8; return; }
    case 8:
        if (R.st == D_ARRIVED && t - R.t_arrive > 3.0) { drive_end("test 2", 0); T.st = 7; T.t = t + 1.0; return; }
        if (R.st == D_OFF || t - R.t_start > 200.0) { drive_end("test 2 timeout", 0); T.st = 7; T.t = t + 1.0; }
        return;
    case 7: CLOG("[crewdrive] test: pressing Triangle to get out (player seat %d)\n", vehicle_of(pl) ? seat_of(vehicle_of(pl), pl) : -1);
        g_pad_inject_d1 = 0; g_pad_inject_d2 = PAD_TRIANGLE >> 8; g_pad_inject_polls = 8; T.st = 9; T.t = t + 5.0; return;
    case 9: CLOG("[crewdrive] test: DONE, player %s a car\n", vehicle_of(pl) ? "still in" : "out of"); T.st = 99; return;
    default: return;
    }
}

/* ================================================================ per frame */
/* GF_CREWDRIVE_DOOR=1: send him to the driver's door (2026-11-21 test: the crew entry code bounces him out of the driver's seat
 * in a loop, so off by default: he gets in by his own door and is moved to the wheel once seated) */
static int s_door = -1;
static int door_on(void) { if (s_door < 0) { const char* e = getenv("GF_CREWDRIVE_DOOR"); s_door = e && *e == '1'; } return s_door; }
uint32_t gf_crewdrive_board_driver(void) { return R.st == D_BOARD && door_on() ? R.bdrv : 0; }   /* gf_crew.c: he takes the driver's door */
uint32_t gf_crewdrive_driver(void) { return (R.st == D_DRIVE || R.st == D_ARRIVED) ? R.driver : 0; }   /* gf_crew.c: no leaning out at the wheel */
static void tick_body(void)
{
    __try {
        const uint32_t pl = player();
        if (!pl || !alive(pl) || gf_cutscene_active()) { if (Mn.st != M_CLOSED) menu_close(); if (R.st != D_OFF && (!pl || !alive(pl))) drive_end("player down", 0); return; }
        const int paused = (g_pad_menu_fn && g_pad_menu_fn() > 0) || g_uiov_menu_open;
        /* the pause menu / map: the car gets its own input object back for as long as it is open (opening the map while our
         * copy was in place crashed the game twice on 2026-11-21: its menu code overflowed the graphics command buffer) */
        if (paused) {
            if (R.redir == 1 && R.st != D_OFF) { write_controls(0, 0, 1, 1, 0); input_restore(); R.pause_redir = 1; CLOG("[crewdrive] pause menu: car controls handed back while it is open\n"); }
            return;
        }
        if (R.pause_redir) {
            R.pause_redir = 0;
            if (R.st != D_OFF && ok(R.car) && vehicle_of(pl) == R.car) { const uint32_t hkv = car_hkv(R.car); if (hkv) { input_redirect(hkv); CLOG("[crewdrive] back from the pause menu: driving again\n"); } }
        }
        const uint32_t mycar = vehicle_of(pl);
        if (R.st == D_DRIVE) side_watch(mycar);
        if (s_test) test_tick(pl);
        /* open: B, or D-pad LEFT held 0.6 s */
        static double t_left; int open_req = kedge(0, s_key);
        { const int held = (g_pad_phys_buttons & PAD_LEFT) != 0 && fg_ours();
          if (held) { if (!t_left) t_left = now_s(); else if (t_left > 0 && now_s() - t_left > 0.6) { open_req = 1; t_left = -1; } } else t_left = 0; }
        if (open_req && Mn.st == M_CLOSED && g_gfd_menu) open_req = 0;   /* the street-talk menu is open */
        menu_tick(pl, open_req);
        switch (R.st) {
        case D_BOARD: {
            const double tn = now_s();
            if (tn - R.t_board > 150.0) { drive_end("nobody got in", 0); break; }
            const uint32_t car = vehicle_of(pl);
            if (!car) { if (R.early) { R.early = 0; input_restore(); } break; }
            if (R.car != car) { R.car = car; R.t_board = tn; R.early = 0; R.t_sm = 0; }
            uint32_t drv = R.bdrv;
            if (!drv || !alive(drv)) { uint32_t cl[5]; drv = crew_list(pl, cl) ? cl[0] : 0; R.bdrv = drv; }
            if (!drv) { drive_end("no crew", 0); break; }
            const int sp = seat_of(car, pl), ds = vehicle_of(drv) == car ? seat_of(car, drv) : -1;
            char nm[48];
            if (ds < 0) {
                /* he is still outside: the player slides over so the driver's seat is free for him (gf_crew.c sends him to that door) */
                if (door_on() && sp == 0 && !seat_occ(car, 1) && seat_count(car) >= 2) {
                    const uint32_t hkv = car_hkv(car);
                    if (hkv && move_seat(car, pl, 1)) { R.early = 1; input_redirect(hkv); CLOG("[crewdrive] player moved over, the driver's seat waits for the crew member\n"); }
                }
                if (R.early) write_controls(0, 0, 1, 1, 0);
                if (tn - R.t_board > (R.early ? 90.0 : 30.0)) { speak(drv, V_NO, "I can't get to the car, boss.", 2.5); drive_end("crew did not get in", 0); }
                if (!R.t_wait_log || tn - R.t_wait_log > 5.0) { R.t_wait_log = tn; CLOG("[crewdrive] waiting for the crew member to get in (player seat %d, %.0f s)\n", sp, tn - R.t_board); }
                break;
            }
            if (R.early) write_controls(0, 0, 1, 1, 0);
            if (sp == 1 && ds != 0 && !seat_occ(car, 0)) {   /* he got in elsewhere: over to the wheel (once his seat record exists) */
                if (!gf_crew_sm_of(drv)) { if (!R.t_sm) R.t_sm = tn; if (tn - R.t_sm < 3.0) break; }
                if (move_seat(car, drv, 0)) crew_sm_seat(car, drv, 0);
                break;
            }
            /* the crew member's seat record must exist before a switch (gf_crew.c sees it once he sits) */
            if (sp == 0 && !gf_crew_sm_of(drv)) { if (!R.t_sm) R.t_sm = tn; if (tn - R.t_sm < 4.0) break; { static double tl; if (tn - tl > 10.0) { tl = tn; CLOG("[crewdrive] no seat record for him after 4 s, switching anyway\n"); } } }
            /* wait for the others (the game brings them in) up to 8 s after he sits */
            if (!R.t_sm) R.t_sm = tn;
            if (!all_crew_in(pl, car) && tn - R.t_sm < 8.0) break;
            if (!drive_begin(pl, car, drv)) { speak(drv, V_NO, "This car won't work, boss.", 2.5); drive_end("drive could not start", 1); }
            else speak(drv, V_GO, NULL, 0.0);   /* "Alright boss, let's roll." */
            break; }
        case D_DRIVE: case D_ARRIVED:
            if (R.st == D_DRIVE && Mn.st == M_CLOSED && !g_gfd_menu) {   /* the prompt, and its key: N / Cross (the gas pedal is his now) */
                snprintf(g_cdr_hint, sizeof g_cdr_hint, "%s", R.fast > 0 ? "Take it easy" : "Step on it"); g_cdr_hint_t = GetTickCount64();
                if (kedge(14, g_cdr_key) || (!g_input_kbm && (s_pe & PAD_CROSS))) {   /* Cross only on a controller (W is Cross in a car on the keyboard) */
                    R.fast = R.fast > 0 ? 0 : 1;
                    speak(R.driver, R.fast > 0 ? V_FAST : V_EASY, R.fast > 0 ? "Hold on to something." : "Nice and easy, boss.", 2.5); CLOG("[crewdrive] speed: %s\n", R.fast > 0 ? "step on it" : "take it easy"); }
            }
            drive_tick(pl); break;
        default: break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { CLOG("[crewdrive] fault in the tick\n"); }
}
/* the tick, timed: anything over 3 ms on the game thread is logged (stutter hunt, 2026-11-21) */
void gf_crewdrive_tick(void)
{
    cfg(); if (!s_on) return;
    const double t0 = now_s();
    tick_body();
    const double ms = (now_s() - t0) * 1000.0;
    static double tl; static int nslow; static double worst;
    if (ms > 3.0) { nslow++; if (ms > worst) worst = ms; }
    if (nslow && now_s() - tl > 2.0) { tl = now_s(); CLOG("[crewdrive] slow ticks: %d over 3 ms, worst %.1f ms (state %d)\n", nslow, worst, R.st); nslow = 0; worst = 0; }
}
