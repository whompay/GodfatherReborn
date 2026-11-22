/* gf_trainer.c -- v1045A: F10 trainer for The Godfather (page drawn by rsx_ui_overlay.c, rsx_ui_trainer.h).
 *
 * Everything here is the game's own machinery, found in the EBOOT (2026-11-02):
 *   player        = *(*(*(*(0x10181290)) + 4)) - 0x17FC                   (same chain as gf_combat.c)
 *   health        = player+0x14C8 (float), refill target / max = player+0x14CC
 *   cheat timers  = player+0x29B4 + 8 / +0xC / +0x10 (health / ammo / money; 300 s after use, must be 0 to work)
 *   cheats        = refillhealth func_001B5D90, fullammo func_001B5EDC (TOC table at 0xB44B80, no arguments)
 *   inventory     = player+0x1A7C: 11 wheel slots at +0x10 + slot*0x14 (+4 = item + 0x38), +0xF3 = "infinite ammo
 *                   unlocked" (Don's Edition reward; its handler func_000E5F04 sets item+0x1DC |= 0x20 on the gun
 *                   slots 0..4 and gives 1000 reserve via func_000A1AE0(item, 1000)), +0x129 = 53 "type owned" bytes
 *   weapon item   = vtable 0x10043FA0 (0x390 bytes, ctor func_000A1EF4): +0xD4 1 = gun, +0xD9 type, +0x1DC flags
 *                   (0x20 = infinite, the inventory screen prints "[Infinity]"), +0x1EC clip, +0x1F0 / +0x1F4 max
 *   give weapon   = the save loader's path (func_000E7EEC): item = func_0029B9F0(&templateGUID, *(player+0x18), 0, 0,
 *                   1); func_000E8EC4(item, 0); func_000E6CE4(inventory, item); func_000E8EC4(item, 1). Template GUIDs
 *                   (16 bytes, EA Attrib registry *(0x10181088), lookup func_004AD7F0(reg, &guid, 0)) are learned from
 *                   weapon items in RAM ("Scan for weapons"): the GUID field offset is found once by asking the
 *                   registry, then kept in gf_trainer_weapons.txt.
 *   money         = *(0x101D985C) + 0x24 (int), set with func_002EB6F8(mgr, value) (also updates the HUD)
 *   model         = func_001BC370(player, mode): 2 = Don's Edition "father model" (applies the model GUID at
 *                   player+0x2254; needs a non-zero GUID at +0x2244 too), 1 = back to your own character. Any entity's
 *                   model resource is entity+0xB8; the model registry *(0x101F3670) (buckets, count; node: +0x8
 *                   resource, +0x1C GUID, +0x2C next) maps it back to a GUID. Lock-on target = *(player+0x1544)-0x38.
 *
 * Threading: row()/status()/hud() run on the render thread and only read; change() only queues a request; the work
 * happens in tr_tick() on the game's main PPU thread (cellPadGetData port 0, inside an HLE call), where guest code can
 * be called. Guest calls run on the main thread's own guest stack, 4 KB below its current stack pointer.
 * GF_TRAINER=0 = off. Log [trainer]. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <ctype.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "recompiled/ppu_recomp.h"
#include "../ps3recomp/libs/video/rsx_ui_trainer.h"

extern uint8_t* vm_base;
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);

/* ------------------------------------------------------------------ guest memory */
static int ok(uint32_t ea) { return ea >= 0x10000u && ea < 0xF0000000u; }
static uint32_t rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void wr32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static float rdf(uint32_t ea) { uint32_t u = rd32(ea); float f; memcpy(&f, &u, 4); return f; }
static void wrf(uint32_t ea, float f) { uint32_t u; memcpy(&u, &f, 4); wr32(ea, u); }
static int fok(float f) { return f == f && f > -1e7f && f < 1e7f; }
static int guid_zero(const uint8_t* g) { for (int i = 0; i < 16; i++) if (g[i]) return 0; return 1; }
static void rdmem(uint32_t ea, void* dst, int n) { memcpy(dst, vm_base + ea, (size_t)n); }
static void wrmem(uint32_t ea, const void* src, int n) { memcpy(vm_base + ea, src, (size_t)n); }

static uint32_t player_raw(void)
{
    uint32_t a = rd32(0x10181290u); if (!ok(a)) return 0;
    a = rd32(a + 4); if (!ok(a)) return 0;
    a = rd32(a); if (!ok(a) || a < 0x17FCu) return 0;
    a -= 0x17FCu; return ok(a) ? a : 0;
}
static uint32_t player(void)
{
#ifdef _WIN32
    __try { return player_raw(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
#else
    return player_raw();
#endif
}

/* ------------------------------------------------------------------ guest calls (main PPU thread only) */
enum { TOC = 0x00B4BF60u };
typedef void (*gfn)(ppu_context*);
static gfn lookup(uint32_t addr)
{
    static uint32_t ca[32]; static gfn cf[32]; static int cn = 0;
    for (int i = 0; i < cn; i++) if (ca[i] == addr) return cf[i];
    gfn f = 0;
    for (uint64_t k = 0; k < function_table_count; k++) if ((uint32_t)function_table[k].addr == addr) { f = function_table[k].func; break; }
    if (cn < 32) { ca[cn] = addr; cf[cn] = f; cn++; }
    if (!f) { printf("[trainer] func_%08X not in the function table\n", addr); fflush(stdout); }
    return f;
}
static uint32_t s_scratch = 0;   /* 64-byte guest buffer between the caller's stack pointer and the callee's frame */
static int s_call_fail = 0;
static uint64_t gcall_raw(gfn f, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, double f1)
{
    ppu_context* cur = g_active_ctx;
    static ppu_context c;   /* not on the host stack: the struct is large and calls do not nest */
    memcpy(&c, cur, sizeof c);
    c.gpr[1] = ((uint32_t)cur->gpr[1] - 0x1000u) & ~0xFull;
    c.gpr[2] = TOC;
    c.gpr[3] = a0; c.gpr[4] = a1; c.gpr[5] = a2; c.gpr[6] = a3; c.gpr[7] = a4;
    c.fpr[1] = f1; c.lr = 0;
    g_active_ctx = &c;
    f(&c);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&c); }
    g_active_ctx = cur;
    return c.gpr[3];
}
static uint64_t gcallf(uint32_t addr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, double f1)
{
    ppu_context* cur = g_active_ctx;
    if (!cur) return 0;
    gfn f = lookup(addr); if (!f) return 0;
#ifdef _WIN32
    __try { return gcall_raw(f, a0, a1, a2, a3, a4, f1); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_active_ctx = cur; g_trampoline_fn = 0; s_call_fail++;
        printf("[trainer] guest call func_%08X faulted\n", addr); fflush(stdout); return 0; }
#else
    return gcall_raw(f, a0, a1, a2, a3, a4, f1);
#endif
}
static uint64_t gcall(uint32_t addr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{ return gcallf(addr, a0, a1, a2, a3, a4, 0.0); }
static uint32_t scratch(void)   /* refreshed each tick from the live stack pointer */
{
    ppu_context* cur = g_active_ctx; if (!cur) return 0;
    return ((uint32_t)cur->gpr[1] - 0x800u) & ~0xFu;
}

/* ------------------------------------------------------------------ page rows */
enum { R_H_PLAYER, R_GOD, R_HEALTH, R_H_WEAP, R_INF, R_AMMO, R_WSEL, R_GIVE, R_GIVEALL, R_SCANW, R_H_MONEY, R_M1, R_M2,
       R_MMAX, R_H_MODEL, R_STORY, R_BECOME, R_COPYM, R_RESETM, R_H_WEATHER, R_RAIN, R_COUNT };

/* ------------------------------------------------------------------ state shared with the render thread */
static volatile int s_god = 0, s_infammo = 0;
static volatile LONG s_req = 0;            /* bit per request (main thread takes them) */
enum { RQ_HEALTH = 1, RQ_AMMO = 2, RQ_GIVE = 4, RQ_GIVEALL = 8, RQ_SCANW = 0x10, RQ_MONEY1 = 0x20, RQ_MONEY2 = 0x40,
       RQ_MONEYMAX = 0x80, RQ_MODEL = 0x100, RQ_COPYMODEL = 0x200, RQ_SCANM = 0x400, RQ_RESETMODEL = 0x800,
       RQ_INFAMMO = 0x1000, RQ_BECOME = 0x2000, RQ_RAIN_ON = 0x4000, RQ_RAIN_OFF = 0x8000 };
/* Weather: the rain scheduler singleton (*0x101DB82C, see gf_cutscene.c gf_rain_poll); rain on = its start roll
 * func_00279138 (tick_impl), off = func_002790B0(sched, f1), which sends iMsgStartRainFadeOut. f1 = fade time. */
extern int (*volatile g_gf_rain_poll)(void);
#define RAIN_FADE 5.0
static void req(LONG b) { InterlockedOr(&s_req, b); }
static char s_msg[64] = "";                 /* last result, shown on the row that asked */
static volatile int s_msg_row = -1;
static void msg(int row, const char* fmt, ...)
{
    char b[64]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    memcpy(s_msg, b, sizeof b); s_msg_row = row;
    printf("[trainer] %s\n", b); fflush(stdout);
}

/* status line (written by the main thread, read by the renderer) */
static volatile float s_hp = 0, s_hpmax = 0; static volatile int s_money = -1, s_have_player = 0;

/* ------------------------------------------------------------------ weapon catalog */
static const signed char k_slot[0x34] = {
    10, 10, 10, 10, 10, 9, 9, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 4, 3, 6, 8, 7, 11, 11, 11, 11, 2, 3 };
static const char* k_group[12] = { "Pistol", "Shotgun", "SMG", "Rifle", "Heavy", "Melee", "Special 6", "Special 7",
                                   "Special 8", "Thrown", "Item", "Misc" };
typedef struct { uint8_t type; uint8_t guid[16]; } wentry;
static wentry s_w[0x34]; static volatile int s_nw = 0; static volatile int s_wsel = 0;
static int s_guid_off = -1;                 /* offset of the template GUID in a weapon item */
static void wname(int type, char* out, size_t n)
{
    if (type < 0 || type >= 0x34) { snprintf(out, n, "?"); return; }
    const int g = k_slot[type]; int lvl = 1;
    for (int t = 0; t < type; t++) if (k_slot[t] == g) lvl++;
    snprintf(out, n, "%s %d (0x%02X)", k_group[g], lvl, type);
}
static void w_add(int type, const uint8_t* guid)
{
    for (int i = 0; i < s_nw; i++) if (s_w[i].type == type) return;
    if (s_nw >= 0x34) return;
    /* keep sorted by type */
    int k = s_nw; while (k > 0 && s_w[k - 1].type > type) { s_w[k] = s_w[k - 1]; k--; }
    s_w[k].type = (uint8_t)type; memcpy(s_w[k].guid, guid, 16); s_nw++;
}
static void w_save(void)
{
    FILE* f = fopen("gf_trainer_weapons.txt", "w"); if (!f) return;
    fprintf(f, "# gf_trainer.c weapon templates: type guid (learned with Scan for weapons)\n");
    if (s_guid_off >= 0) fprintf(f, "off %X\n", s_guid_off);
    for (int i = 0; i < s_nw; i++) { fprintf(f, "%02X ", s_w[i].type); for (int j = 0; j < 16; j++) fprintf(f, "%02X", s_w[i].guid[j]); fprintf(f, "\n"); }
    fclose(f);
}
static int hexguid(const char* s, uint8_t* g)
{
    for (int j = 0; j < 16; j++) { unsigned v; if (sscanf(s + 2 * j, "%2X", &v) != 1) return 0; g[j] = (uint8_t)v; }
    return 1;
}
static void w_load(void)
{
    FILE* f = fopen("gf_trainer_weapons.txt", "r"); if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        unsigned t; char gs[64]; uint8_t g[16];
        if (!strncmp(line, "off ", 4)) { s_guid_off = (int)strtol(line + 4, 0, 16); continue; }
        if (sscanf(line, "%X %40s", &t, gs) == 2 && t < 0x34 && strlen(gs) >= 32 && hexguid(gs, g)) w_add((int)t, g);
    }
    fclose(f);
}

/* ------------------------------------------------------------------ model library */
static uint8_t s_st_saved[80][16], s_st_saved_sk[80][16]; static char s_st_saved_code[80][16]; static int s_nsaved = 0;   /* from the file */

enum { MAXM = 64 };
static uint8_t s_m[MAXM][16]; static volatile int s_nm = 0; static volatile int s_msel = 0;   /* 0 = your character */
static uint32_t s_npc_vt[8]; static int s_nvt = 0;   /* vtables of characters seen as lock-on targets */
static uint32_t s_last_tgt = 0;
static int m_add(const uint8_t* g)
{
    for (int i = 0; i < s_nm; i++) if (!memcmp(s_m[i], g, 16)) return i;
    if (s_nm >= MAXM || guid_zero(g)) return -1;
    memcpy(s_m[s_nm], g, 16); return s_nm++;
}
static void m_save(void)
{
    FILE* f = fopen("gf_trainer_models.txt", "w"); if (!f) return;
    fprintf(f, "# gf_trainer.c character models (model GUIDs) and character vtables\n");
    for (int i = 0; i < s_nvt; i++) fprintf(f, "vt %08X\n", s_npc_vt[i]);
    for (int i = 0; i < s_nm; i++) { fprintf(f, "m "); for (int j = 0; j < 16; j++) fprintf(f, "%02X", s_m[i][j]); fprintf(f, "\n"); }
    fclose(f);
}
static void m_load(void)
{
    FILE* f = fopen("gf_trainer_models.txt", "r"); if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        unsigned v; char gs[64]; uint8_t g[16];
        char code[32], gs2[64]; uint8_t g2[16];   /* v1045C format; v1045B "s" lines (mesh only, often wrong) are dropped */
        if (sscanf(line, "s2 %31s %40s %40s", code, gs, gs2) == 3 && hexguid(gs, g) && hexguid(gs2, g2) && s_nsaved < 80) {
            snprintf(s_st_saved_code[s_nsaved], 16, "%s", code); memcpy(s_st_saved[s_nsaved], g, 16);
            memcpy(s_st_saved_sk[s_nsaved], g2, 16); s_nsaved++; continue; }
        if (sscanf(line, "vt %X", &v) == 1 && s_nvt < 8) s_npc_vt[s_nvt++] = v;
        else if (sscanf(line, "m %40s", gs) == 1 && strlen(gs) >= 32 && hexguid(gs, g)) m_add(g);
    }
    fclose(f);
}
/* model registry: resource -> GUID, GUID -> loaded? */
static int mreg_find(uint32_t res, const uint8_t* want, uint8_t* out)
{
    const uint32_t tab = rd32(0x101F3670u); if (!ok(tab)) return 0;
    const uint32_t bk = rd32(tab), n = rd32(tab + 4);
    if (!ok(bk) || n == 0 || n > (1u << 20)) return 0;
    long budget = 400000;
    for (uint32_t b = 0; b < n; b++) {
        uint32_t node = rd32(bk + 4 * b);
        while (node && ok(node) && budget-- > 0) {
            if (res ? rd32(node + 8) == res : (rd32(node + 8) != 0 && !memcmp(vm_base + node + 0x1C, want, 16))) {
                if (out) rdmem(node + 0x1C, out, 16);
                return 1;
            }
            node = rd32(node + 0x2C);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ RAM scan helper */
typedef void (*scan_cb)(uint32_t ea, void* user);
static void ram_scan_word(uint32_t word, scan_cb cb, void* user)
{
    const uint8_t w[4] = { (uint8_t)(word >> 24), (uint8_t)(word >> 16), (uint8_t)(word >> 8), (uint8_t)word };
    for (uint64_t ea = 0x20000000ull; ea < 0xF0000000ull; ) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vm_base + ea, &mi, sizeof mi)) break;
        uint64_t rs = (uint64_t)((uint8_t*)mi.BaseAddress - vm_base), re = rs + mi.RegionSize;
        if (re <= ea) break;
        if (rs < ea) rs = ea;
        if (re > 0xF0000000ull) re = 0xF0000000ull;
        const DWORD pr = mi.Protect & 0xFF;
        if (mi.State == MEM_COMMIT && !(mi.Protect & PAGE_GUARD) && (pr == PAGE_READWRITE || pr == PAGE_EXECUTE_READWRITE)) {
            __try {
                const uint8_t* b = vm_base + rs; const size_t len = (size_t)(re - rs);
                for (size_t i = 0; i + 4 <= len; i += 4)
                    if (b[i] == w[0] && !memcmp(b + i, w, 4)) cb((uint32_t)(rs + i), user);
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
        ea = re;
    }
}

/* ------------------------------------------------------------------ actions (main thread) */
static int attrib_known(uint32_t guid_ea)   /* the game's template registry knows this GUID */
{
    const uint32_t reg = rd32(0x10181088u); if (!ok(reg)) return 0;
    return (uint32_t)gcall(0x004AD7F0u, reg, guid_ea, 0, 0, 0) != 0;
}
typedef struct { uint32_t items[256]; int n; } wscan;
static void wscan_cb(uint32_t ea, void* u)
{
    wscan* s = (wscan*)u;
    if (s->n >= 256) return;
    const uint8_t t = vm_base[ea + 0xD9], g = vm_base[ea + 0xD4];
    if (t < 0x34 && g <= 1) s->items[s->n++] = ea;
}
static void do_scan_weapons(int row)
{
    static wscan s; s.n = 0;
    ram_scan_word(0x10043FA0u, wscan_cb, &s);
    if (!s.n) { msg(row, "no weapon items in memory"); return; }
    const uint32_t buf = scratch(); if (!buf) return;
    if (s_guid_off < 0) {   /* find the GUID field once: the first offset whose 16 bytes the registry knows */
        for (int k = 0; k < s.n && s_guid_off < 0 && k < 4; k++)
            for (int off = 0x220; off >= 0x30 && s_guid_off < 0; off -= 4) {   /* 0x220 first: the ctor clears 16 bytes there */
                uint8_t g[16]; rdmem(s.items[k] + off, g, 16);
                if (guid_zero(g)) continue;
                wrmem(buf, g, 16);
                if (attrib_known(buf)) s_guid_off = off;
            }
        if (s_guid_off < 0)
            for (int k = 0; k < s.n && s_guid_off < 0 && k < 4; k++)
                for (int off = 0x224; off <= 0x380 && s_guid_off < 0; off += 4) {
                    uint8_t g[16]; rdmem(s.items[k] + off, g, 16);
                    if (guid_zero(g)) continue;
                    wrmem(buf, g, 16);
                    if (attrib_known(buf)) s_guid_off = off;
                }
        printf("[trainer] weapon template GUID offset: %s0x%X (%d items)\n", s_guid_off < 0 ? "NOT FOUND " : "", s_guid_off < 0 ? 0 : s_guid_off, s.n);
        if (s_guid_off < 0) { msg(row, "could not find the template id (see log)"); return; }
    }
    const int before = s_nw;
    for (int k = 0; k < s.n; k++) {
        uint8_t g[16]; rdmem(s.items[k] + (uint32_t)s_guid_off, g, 16);
        if (guid_zero(g)) continue;
        wrmem(buf, g, 16);
        if (!attrib_known(buf)) continue;
        w_add(vm_base[s.items[k] + 0xD9], g);
    }
    w_save();
    msg(row, "%d items, %d new types (%d known)", s.n, s_nw - before, s_nw);
}
static uint32_t slot_item(uint32_t inv, int slot)
{
    const uint32_t p = rd32(inv + 0x10 + 0x14 * (uint32_t)slot + 4);
    return (p > 0x38u && ok(p)) ? p - 0x38u : 0;
}
static int give_one(uint32_t pl, int idx)
{
    const uint32_t buf = scratch(), owner = rd32(pl + 0x18), inv = pl + 0x1A7C;
    if (!buf || !ok(owner)) return 0;
    wrmem(buf, s_w[idx].guid, 16);
    const uint32_t item = (uint32_t)gcall(0x0029B9F0u, buf, owner, 0, 0, 1);
    if (!item || !ok(item)) return 0;
    gcall(0x000E8EC4u, item, 0, 0, 0, 0);
    const int added = (int)(uint32_t)gcall(0x000E6CE4u, inv, item, 0, 0, 0);
    gcall(0x000E8EC4u, item, 1, 0, 0, 0);
    if (vm_base[item + 0xD4] == 1) {   /* full ammo, like the fullammo cheat */
        gcall(0x000A1AE0u, item, rd32(item + 0x1F4), 0, 0, 0);
        gcall(0x000A1A08u, item, rd32(item + 0x1F0), 0, 0, 0);
        if (s_infammo) wr32(item + 0x1DC, rd32(item + 0x1DC) | 0x20u);
    }
    printf("[trainer] give type 0x%02X -> item 0x%08X added %d\n", s_w[idx].type, item, added); fflush(stdout);
    return 1;
}
/* infinite ammo: the game's own Don's Edition unlock (inventory +0xF3, item flag 0x20) */
static int s_inf_orig_f3 = -1; static uint32_t s_inf_items[16]; static int s_ninf = 0; static uint32_t s_inf_inv = 0;
static void inf_apply(uint32_t pl)
{
    const uint32_t inv = pl + 0x1A7C;
    if (s_inf_inv != inv) { s_inf_inv = inv; s_ninf = 0; s_inf_orig_f3 = vm_base[inv + 0xF3]; }
    vm_base[inv + 0xF3] = 1;
    for (int sl = 0; sl < 11; sl++) {
        const uint32_t it = slot_item(inv, sl);
        if (!it || vm_base[it + 0xD4] != 1) continue;
        const uint32_t fl = rd32(it + 0x1DC);
        if (fl & 0x20u) continue;
        wr32(it + 0x1DC, fl | 0x20u);
        gcall(0x000A1AE0u, it, 1000, 0, 0, 0);
        if (s_ninf < 16) s_inf_items[s_ninf++] = it;
    }
}
static void inf_remove(uint32_t pl)
{
    const uint32_t inv = pl + 0x1A7C;
    for (int i = 0; i < s_ninf; i++)
        for (int sl = 0; sl < 11; sl++) if (slot_item(inv, sl) == s_inf_items[i]) wr32(s_inf_items[i] + 0x1DC, rd32(s_inf_items[i] + 0x1DC) & ~0x20u);
    if (s_inf_inv == inv && s_inf_orig_f3 >= 0) vm_base[inv + 0xF3] = (uint8_t)s_inf_orig_f3;
    s_ninf = 0; s_inf_inv = 0;
}
static void money_add(int row, long long add, int set_max)
{
    const uint32_t mgr = rd32(0x101D985Cu); if (!ok(mgr)) { msg(row, "no money manager"); return; }
    long long cur = (int32_t)rd32(mgr + 0x24), v = set_max ? 9999995 : cur + add;
    if (v > 99999995) v = 99999995; if (v < 0) v = 0;
    gcall(0x002EB6F8u, mgr, (uint64_t)(int64_t)(int32_t)v, 0, 0, 0);
    msg(row, "$%lld -> $%d", cur, (int32_t)rd32(mgr + 0x24));
}
/* models */
static uint8_t s_orig_father[32]; static int s_have_orig = 0;
/* v1045C: func_001BC370(player, 2) needs TWO loaded GUIDs: +0x2244 = skeleton (looked up by func_004596B8, handed to
 * vtable +0x104 = func_00464890, which only accepts resources whose type tag *(*(res+4)+8) is 0x9E3791CF / 0xF3F21004)
 * and +0x2254 = the mesh (func_00463660 -> entity+0xB8). v1045B only wrote +0x2244 when it was empty -- it holds the
 * (unloaded) father skeleton, so the lookup failed and every switch returned 0. */
static int model_apply2(uint32_t pl, const uint8_t* model, const uint8_t* skel, int row)
{
    if (!mreg_find(0, model, 0)) { msg(row, "that model is not loaded here"); return 0; }
    if (!mreg_find(0, skel, 0)) { msg(row, "that skeleton is not loaded here"); return 0; }
    if (rd32(pl + 0x1A64) == 2) gcall(0x001BC370u, pl, 1, 0, 0, 0);
    if (!s_have_orig) { rdmem(pl + 0x2244, s_orig_father, 32); s_have_orig = 1; }
    wrmem(pl + 0x2244, skel, 16);
    wrmem(pl + 0x2254, model, 16);
    const int r = (int)(uint32_t)gcall(0x001BC370u, pl, 2, 0, 0, 0);
    printf("[trainer] model apply -> %d (mode now %u)\n", r, rd32(pl + 0x1A64)); fflush(stdout);
    return r;
}
/* v1045D: the type tag lives on the registry NODE (func_00464890 gets the node from func_004596B8): *(*(node+4)+8) */
static uint32_t node_type(uint32_t node) { if (!ok(node)) return 0; const uint32_t r = rd32(node + 4); return ok(r) ? rd32(r + 8) : 0; }
static uint32_t node_of_res(uint32_t res)
{
    const uint32_t tab = rd32(0x101F3670u); if (!ok(tab) || !ok(res)) return 0;
    const uint32_t bk = rd32(tab), n = rd32(tab + 4);
    if (!ok(bk) || n == 0 || n > (1u << 20)) return 0;
    long budget = 400000;
    for (uint32_t b = 0; b < n; b++)
        for (uint32_t node = rd32(bk + 4 * b); node && ok(node) && budget-- > 0; node = rd32(node + 0x2C))
            if (rd32(node + 8) == res) return node;
    return 0;
}
static int is_skel_type(uint32_t t) { return t == 0x9E3791CFu || t == 0xF3F21004u; }
/* skeleton GUID of a live character: registry skeleton whose data the entity's animation controller (+0x64) points at */
static int skel_of_entity(uint32_t ent, uint8_t* out)
{
    const uint32_t ctl = rd32(ent + 0x64); if (!ok(ctl)) return 0;
    const uint32_t tab = rd32(0x101F3670u); if (!ok(tab)) return 0;
    const uint32_t bk = rd32(tab), n = rd32(tab + 4);
    if (!ok(bk) || n == 0 || n > (1u << 20)) return 0;
    long budget = 400000;
    for (uint32_t b = 0; b < n; b++)
        for (uint32_t node = rd32(bk + 4 * b); node && ok(node) && budget-- > 0; node = rd32(node + 0x2C)) {
            const uint32_t res = rd32(node + 8);
            if (!res || !is_skel_type(node_type(node))) continue;
            const uint32_t d1 = rd32(res + 8), d2 = ok(d1) ? rd32(d1 + 8) : 0;
            for (uint32_t o = 0; o < 0x400; o += 4) {
                const uint32_t w = rd32(ctl + o);
                if (w && (w == d1 || w == d2 || w == res)) { rdmem(node + 0x1C, out, 16); return 1; }
            }
        }
    return 0;
}
static int model_apply(uint32_t pl, const uint8_t* g, int row)   /* mesh only: skeleton from the player */
{
    uint8_t sk[16];
    if (!skel_of_entity(pl, sk)) { msg(row, "could not find a skeleton"); return 0; }
    return model_apply2(pl, g, sk, row);
}
static void model_reset(uint32_t pl)
{
    if (rd32(pl + 0x1A64) == 2) gcall(0x001BC370u, pl, 1, 0, 0, 0);
    if (s_have_orig) { wrmem(pl + 0x2244, s_orig_father, 32); s_have_orig = 0; }
}
static int vt_known(uint32_t vt) { for (int i = 0; i < s_nvt; i++) if (s_npc_vt[i] == vt) return 1; return 0; }
typedef struct { int added; uint32_t pl; } mscan;
static void mscan_cb(uint32_t ea, void* u)
{
    mscan* s = (mscan*)u;
    if (ea == s->pl) return;
    const uint32_t res = rd32(ea + 0xB8); uint8_t g[16];
    if (!ok(res) || !mreg_find(res, 0, g)) return;
    const int before = s_nm; m_add(g); if (s_nm > before) s->added++;
}


/* ------------------------------------------------------------------ v1045B story characters
 * Every character model is its own stream, godfather_v4\GlobalFolder\npc_models\<code>_model.str. The stream manager
 * *(0x10180F58) keeps the TOC sets at +0x2264 (count at +0x2264+0x80); a set: +8 prefix string, +0x10 u16 entry count,
 * entry i at +0x1C + 0x14*i with its name pointer at +4 (lookup func_004BF938: "<prefix>\<entry>", case-insensitive).
 * Loading = the mission script's LoadStream action (func_00256BA8): func_004B9D80(mgr, name, 0, 0, 0, 0, f1 = 32768).
 * The new model shows up in the model registry once the stream is in; its GUID is then applied like a lock-on copy
 * and remembered ("s <code> <guid>" in gf_trainer_models.txt) so the next switch is instant when it is loaded. */
static const struct { const char* code; const char* name; } k_cast[] = {
    {"vita","Vito Corleone"},{"vitb","Vito Corleone (b)"},{"vitc","Vito Corleone (c)"},{"vitd","Vito Corleone (d)"},
    {"vitf","Vito Corleone (f)"},{"sona","Sonny Corleone"},{"sonc","Sonny Corleone (b)"},{"mica","Michael Corleone"},
    {"micb","Michael Corleone (b)"},{"frea","Fredo Corleone?"},{"toma","Tom Hagen"},{"clea","Peter Clemenza"},
    {"tesa","Sal Tessio"},{"luca","Luca Brasi"},{"luaa","Luca Brasi (b)"},{"paua","Paulie Gatto"},{"roca","Rocco Lampone"},
    {"alna","Al Neri"},{"alnb","Al Neri (b)"},{"riza","Carlo Rizzi"},{"sola","Virgil Sollozzo"},{"phta","Philip Tattaglia"},
    {"brua","Bruno Tattaglia"},{"bara","Emilio Barzini"},{"bara_00","Emilio Barzini (b)"},{"bara_01","Emilio Barzini (c)"},
    {"cpka","Capt. McCluskey?"},{"mcka","McCluskey?"},{"mckb","McCluskey? (b)"},{"moea","Moe Greene"},{"wola","Jack Woltz"},
    {"jona","Johnny Fontane"},{"jonb","Johnny Fontane (b)"},{"mona","Monk Malone"},{"monc","Monk Malone (b)"},
    {"fraa","Frankie Malone?"},{"frab","Frankie Malone? (b)"},{"joha","Johnny Trapani?"},{"mana","Mama?"},{"rosa","rosa"},
    {"rosb","rosb"},{"kida","Kid"},{"cica","cica"},{"cdra","cdra"},{"jima","jima"},{"herc","herc"},{"olwa","olwa"},
    {"vena","vena"},{"dbaa","dbaa"},
};
typedef struct { char code[16]; char label[40]; char full[120]; uint8_t guid[16]; uint8_t skel[16]; int have; } story_t;
static story_t s_st[80]; static volatile int s_nst = 0, s_stsel = 0; static int s_st_built = 0;
static int gstr(uint32_t ea, char* out, int n)
{
    if (!ok(ea)) { out[0] = 0; return 0; }
    int i = 0; for (; i < n - 1; i++) { const char c = (char)vm_base[ea + i]; if (!c) break; if (c < 32 || c > 126) { out[0] = 0; return 0; } out[i] = c; }
    out[i] = 0; return i;
}
static void st_build(void)
{
    const uint32_t mgr = rd32(0x10180F58u); if (!ok(mgr)) return;
    const uint32_t base = mgr + 0x2264u, nsets = rd32(base + 0x80);
    if (nsets == 0 || nsets > 32) return;
    story_t tmp[80]; int nt = 0, logged = 0;
    for (uint32_t si = 0; si < nsets; si++) {
        const uint32_t set = rd32(base + 4 * si); if (!ok(set)) continue;
        char prefix[96]; gstr(rd32(set + 8), prefix, sizeof prefix);
        const unsigned cnt = (unsigned)((vm_base[set + 0x10] << 8) | vm_base[set + 0x11]);
        for (unsigned k = 0; k < cnt && k < 20000 && nt < 80; k++) {
            char nm[96]; if (!gstr(rd32(set + 0x1C + 0x14 * k + 4), nm, sizeof nm)) continue;
            char low[96]; for (int j = 0; ; j++) { low[j] = (char)tolower((unsigned char)nm[j]); if (!nm[j]) break; }
            if (!strstr(low, "npc_models")) continue;
            if (logged++ < 3) { printf("[trainer] stream TOC entry: \"%s\" in set \"%s\"\n", nm, prefix); fflush(stdout); }
            const char* b = strrchr(low, '\\'); if (!b) b = strrchr(low, '/'); b = b ? b + 1 : low;
            char code[16]; int j = 0; for (; b[j] && j < 15; j++) code[j] = b[j]; code[j] = 0;
            char* m = strstr(code, "_model"); if (m) { if (m[6] == '_' || m[6] == 0) { memmove(m, m + 6, strlen(m + 6) + 1); } }
            m = strstr(code, ".str"); if (m) *m = 0;
            int named = -1; for (int c = 0; c < (int)(sizeof k_cast / sizeof k_cast[0]); c++) if (!strcmp(k_cast[c].code, code)) { named = c; break; }
            if (named < 0) continue;   /* story / named characters only -- no generic townspeople */
            story_t* e = &tmp[nt++]; memset(e, 0, sizeof *e);
            snprintf(e->code, sizeof e->code, "%s", code); snprintf(e->label, sizeof e->label, "%s", k_cast[named].name);
            snprintf(e->full, sizeof e->full, "%s\\%s", prefix, nm);
            for (int q = 0; q < s_nsaved; q++) if (!strcmp(s_st_saved_code[q], code)) { memcpy(e->guid, s_st_saved[q], 16); memcpy(e->skel, s_st_saved_sk[q], 16); e->have = 1; }
        }
    }
    /* order like the cast table */
    int n = 0;
    for (int c = 0; c < (int)(sizeof k_cast / sizeof k_cast[0]); c++)
        for (int i = 0; i < nt; i++) if (!strcmp(tmp[i].code, k_cast[c].code) && n < 80) s_st[n++] = tmp[i];
    s_nst = n; s_st_built = 1;
    printf("[trainer] v1045D %d story characters in the stream TOC (%u sets)\n", n, nsets); fflush(stdout);
}
static void st_save(void)
{
    FILE* f = fopen("gf_trainer_models.txt", "w"); if (!f) return;
    fprintf(f, "# gf_trainer.c v1045C: s2 <character code> <mesh GUID> <skeleton GUID> (learned on first load)\n");
    for (int i = 0; i < s_nst; i++) if (s_st[i].have) {
        fprintf(f, "s2 %s ", s_st[i].code); for (int j = 0; j < 16; j++) fprintf(f, "%02X", s_st[i].guid[j]);
        fprintf(f, " "); for (int j = 0; j < 16; j++) fprintf(f, "%02X", s_st[i].skel[j]); fprintf(f, "\n"); }
    fclose(f);
}
/* registry snapshot: which model nodes existed before the load */
static uint32_t* s_snap = 0; static int s_nsnap = 0;
static int u32cmp(const void* a, const void* b) { const uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b; return x < y ? -1 : x > y; }
static int mreg_walk(uint32_t* out, int cap)   /* all live model nodes; returns count (capped) */
{
    const uint32_t tab = rd32(0x101F3670u); if (!ok(tab)) return 0;
    const uint32_t bk = rd32(tab), n = rd32(tab + 4);
    if (!ok(bk) || n == 0 || n > (1u << 20)) return 0;
    int c = 0; long budget = 400000;
    for (uint32_t b = 0; b < n; b++)
        for (uint32_t node = rd32(bk + 4 * b); node && ok(node) && budget-- > 0; node = rd32(node + 0x2C))
            if (rd32(node + 8)) { if (c < cap) out[c] = node; c++; }
    return c < cap ? c : cap;
}
static volatile int s_pend = -1; static ULONGLONG s_pend_t0 = 0; static int s_pend_seen = 0;
static void become(uint32_t pl, int i)
{
    story_t* e = &s_st[i];
    if (e->have && mreg_find(0, e->guid, 0) && mreg_find(0, e->skel, 0)) {   /* already in memory */
        s_msg_row = -1; if (model_apply2(pl, e->guid, e->skel, R_BECOME)) msg(R_BECOME, "done");
        else if (s_msg_row != R_BECOME) msg(R_BECOME, "switch failed");
        return; }
    if (!s_snap) s_snap = (uint32_t*)malloc(sizeof(uint32_t) * 65536);
    if (!s_snap) return;
    s_nsnap = mreg_walk(s_snap, 65536); qsort(s_snap, (size_t)s_nsnap, 4, u32cmp);
    const uint32_t mgr = rd32(0x10180F58u), buf = scratch();
    if (!ok(mgr) || !buf) { msg(R_BECOME, "no stream manager"); return; }
    wrmem(buf, e->full, (int)strlen(e->full) + 1);
    const uint32_t h = (uint32_t)gcallf(0x004B9D80u, mgr, buf, 0, 0, 0, 32768.0);
    printf("[trainer] LoadStream \"%s\" -> handle 0x%08X (%d models before)\n", e->full, h, s_nsnap); fflush(stdout);
    if (!h) { msg(R_BECOME, "the game refused to load it"); return; }
    s_pend = i; s_pend_t0 = GetTickCount64(); s_pend_seen = 0;
    msg(R_BECOME, "loading...");
}
static void become_poll(uint32_t pl)
{
    const int i = s_pend; if (i < 0) return;
    story_t* e = &s_st[i];
    static uint32_t cur[65536];
    const int n = mreg_walk(cur, 65536);
    static uint32_t mtype = 0; if (!mtype) mtype = node_type(node_of_res(rd32(pl + 0xB8)));   /* type of the player's own mesh */
    uint32_t mesh = 0, skel = 0; int nf = 0;
    for (int k = 0; k < n; k++) {
        if (bsearch(&cur[k], s_snap, (size_t)s_nsnap, 4, u32cmp)) continue;
        nf++;
        const uint32_t t = node_type(cur[k]);
        if (!skel && is_skel_type(t)) skel = cur[k];
        if (!mesh && t == mtype && mtype) mesh = cur[k];
    }
    if (nf && ++s_pend_seen == 3) {   /* list what arrived once, for the log */
        printf("[trainer] %s: %d new resources (mesh type %08X):", e->code, nf, mtype);
        for (int k = 0, shown = 0; k < n && shown < 12; k++)
            if (!bsearch(&cur[k], s_snap, (size_t)s_nsnap, 4, u32cmp)) { printf(" %08X", node_type(cur[k])); shown++; }
        printf("\n"); fflush(stdout);
    }
    if (mesh && s_pend_seen >= 3) {   /* a few frames for the stream to finish fixing up */
        rdmem(mesh + 0x1C, e->guid, 16);
        if (skel) rdmem(skel + 0x1C, e->skel, 16);
        else if (!skel_of_entity(pl, e->skel)) { s_pend = -1; msg(R_BECOME, "no skeleton found"); return; }
        e->have = 1; st_save(); s_pend = -1;
        printf("[trainer] %s: mesh node 0x%08X, skeleton %s\n", e->code, mesh, skel ? "from the stream" : "the player's"); fflush(stdout);
        s_msg_row = -1; if (model_apply2(pl, e->guid, e->skel, R_BECOME)) msg(R_BECOME, "done");
        else if (s_msg_row != R_BECOME) msg(R_BECOME, "switch failed");
        return;
    }
    if (GetTickCount64() - s_pend_t0 > 20000) {
        s_pend = -1;
        msg(R_BECOME, nf ? "no mesh found (see log)" : "already loaded: lock on + Copy");
    }
}

/* ------------------------------------------------------------------ main-thread tick */
static void tick_impl(void)
{
    const uint32_t pl = player();
    s_have_player = pl != 0;
    LONG rq = InterlockedExchange(&s_req, 0);
    if (!pl) { if (rq) msg(s_msg_row, "not in game"); return; }
    { const uint32_t mgr = rd32(0x101D985Cu); s_money = ok(mgr) ? (int32_t)rd32(mgr + 0x24) : -1; }
    s_hp = rdf(pl + 0x14C8); s_hpmax = rdf(pl + 0x14CC);
    { const uint32_t q = rd32(pl + 0x1544);   /* remember the last lock-on target */
      if (ok(q) && q > 0x38u && q - 0x38u != pl && ok(rd32(q - 0x38u + 0xB8))) s_last_tgt = q - 0x38u; }

    if (!s_st_built) { static ULONGLONG lt = 0; const ULONGLONG t = GetTickCount64(); if (t - lt > 2000) { lt = t; st_build(); } }
    become_poll(pl);
    if (rq & RQ_BECOME) { const int k = s_stsel; if (s_pend >= 0) msg(R_BECOME, "still loading..."); else if (k >= 0 && k < s_nst) become(pl, k); }
    if (s_god) {
        const float h = s_hp, m = s_hpmax;
        if (fok(h) && fok(m) && m > 0.0f && h > 0.0f && h < m) wrf(pl + 0x14C8, m);
    }
    { static ULONGLONG last = 0; const ULONGLONG t = GetTickCount64();
      if (s_infammo && t - last >= 500) { last = t; inf_apply(pl); } }

    if (rq & RQ_INFAMMO) { if (s_infammo) inf_apply(pl); else inf_remove(pl); }
    if (rq & RQ_HEALTH) { wrf(pl + 0x29B4 + 0x8, 0.0f); gcall(0x001B5D90u, 0, 0, 0, 0, 0); msg(R_HEALTH, "health refilled"); }
    if (rq & RQ_AMMO)   { wrf(pl + 0x29B4 + 0xC, 0.0f); gcall(0x001B5EDCu, 0, 0, 0, 0, 0); msg(R_AMMO, "ammo refilled"); }
    if (rq & RQ_SCANW)  do_scan_weapons(R_SCANW);
    if (rq & RQ_GIVE) {
        const int i = s_wsel;
        if (i < 0 || i >= s_nw) msg(R_GIVE, "scan for weapons first");
        else { char n[48]; wname(s_w[i].type, n, sizeof n); msg(R_GIVE, give_one(pl, i) ? "gave %s" : "could not give %s", n); }
    }
    if (rq & RQ_GIVEALL) { int g = 0; for (int i = 0; i < s_nw; i++) g += give_one(pl, i); msg(R_GIVEALL, "gave %d of %d", g, s_nw); }
    if (rq & RQ_MONEY1) money_add(R_M1, 10000, 0);
    if (rq & RQ_MONEY2) money_add(R_M2, 100000, 0);
    if (rq & RQ_MONEYMAX) money_add(R_MMAX, 0, 1);
    if (rq & RQ_COPYMODEL) {
        const uint32_t t = s_last_tgt; uint8_t g[16];
        if (!t) msg(R_COPYM, "lock on to someone first (T / RMB), then press this");
        else if (!mreg_find(rd32(t + 0xB8), 0, g)) msg(R_COPYM, "their model is not in the registry");
        else {
            const uint32_t vt = rd32(t); if (!vt_known(vt) && s_nvt < 8) s_npc_vt[s_nvt++] = vt;
            uint8_t sk[16]; const int own = !skel_of_entity(t, sk);
            if (own && !skel_of_entity(pl, sk)) msg(R_COPYM, "could not find a skeleton");
            else { s_msel = 1; printf("[trainer] copy: skeleton from %s\n", own ? "player (target's not found)" : "target"); fflush(stdout);
                   s_msg_row = -1; if (model_apply2(pl, g, sk, R_COPYM)) msg(R_COPYM, "now wearing their model");
                   else if (s_msg_row != R_COPYM) msg(R_COPYM, "switch failed"); }
        }
    }
    if (rq & RQ_RESETMODEL) { model_reset(pl); s_msel = 0; s_pend = -1; msg(R_RESETM, "back to your own character"); }
    if (rq & (RQ_RAIN_ON | RQ_RAIN_OFF)) {
        const uint32_t sched = rd32(0x101DB82Cu);
        if (!ok(sched)) msg(R_RAIN, "weather not ready");
        else if (rq & RQ_RAIN_ON) {
            /* func_00279138(sched) is the scheduler's own "maybe start rain" roll: rain starts when rand() x k beats
             * (const - sched+0x88), then it picks an intensity and sends iMsgStartRainFadeIn plus the intensity message
             * (the falling rain). A huge +0x88 for that one call makes the roll always win. */
            const uint32_t keep = rd32(sched + 0x88);
            wrf(sched + 0x88, 1.0e9f);
            gcall(0x00279138u, sched, 0, 0, 0, 0);
            wr32(sched + 0x88, keep);
            msg(R_RAIN, "rain starting");
        }
        else { gcallf(0x002790B0u, sched, 0, 0, 0, 0, RAIN_FADE); msg(R_RAIN, "rain stopping"); }
    }
}
static int s_trainer_on = 0;
static void tr_tick(void);
static void main_tick(void)   /* v1058E: main PPU thread, once per frame */
{
    extern void gf_coop_glue_tick(void); extern void gf_shop_tick(void);
    if (s_trainer_on) tr_tick();
    gf_coop_glue_tick();
    gf_shop_tick();   /* v1092S inventory / stores */
    { extern void gf_dialogue_tick(void); gf_dialogue_tick(); }   /* v1106D street dialogue (T) */
    { extern void gf_sit_tick(void); gf_sit_tick(); }   /* v1109C sittable furniture (U) */
    { extern void gf_crewdrive_tick(void); gf_crewdrive_tick(); }   /* crew "You drive" (B) */
    { extern void gf_pop_car_watch(void); gf_pop_car_watch(); }   /* v1109Y: cars that vanish while on screen (log) */
}
static void tr_tick(void)
{
#ifdef _WIN32
    __try { tick_impl(); } __except (EXCEPTION_EXECUTE_HANDLER) { }
#else
    tick_impl();
#endif
}

/* ------------------------------------------------------------------ page (render thread) */
static int t_count(void) { return R_COUNT; }
static void val(uiov_trainer_row* o, int row, const char* dflt)
{
    if (s_msg_row == row && s_msg[0]) snprintf(o->value, sizeof o->value, "%s", s_msg);
    else snprintf(o->value, sizeof o->value, "%s", dflt);
}
static int t_row(int i, uiov_trainer_row* o)
{
    switch (i) {
    case R_H_PLAYER: o->kind = UIOV_TR_HEADER; strcpy(o->label, "PLAYER"); return 1;
    case R_GOD: o->kind = UIOV_TR_CHOICE; o->on = s_god; strcpy(o->label, "God mode"); strcpy(o->value, s_god ? "On" : "Off"); return 1;
    case R_HEALTH: o->kind = UIOV_TR_ACTION; strcpy(o->label, "Refill health"); val(o, i, ""); return 1;
    case R_H_WEAP: o->kind = UIOV_TR_HEADER; strcpy(o->label, "WEAPONS"); return 1;
    case R_AMMO: o->kind = UIOV_TR_ACTION; strcpy(o->label, "Refill ammo"); val(o, i, ""); return 1;
    case R_INF: o->kind = UIOV_TR_CHOICE; o->on = s_infammo; strcpy(o->label, "Infinite ammo"); strcpy(o->value, s_infammo ? "On" : "Off"); return 1;
    case R_WSEL:
        o->kind = UIOV_TR_CHOICE; strcpy(o->label, "Weapon");
        if (!s_nw) strcpy(o->value, "none known yet - use Scan below");
        else { char n[48]; const int k = s_wsel < s_nw ? s_wsel : 0; wname(s_w[k].type, n, sizeof n);
               snprintf(o->value, sizeof o->value, "%s  (%d/%d)", n, k + 1, s_nw); }
        return 1;
    case R_GIVE: o->kind = UIOV_TR_ACTION; strcpy(o->label, "Give this weapon"); val(o, i, ""); return 1;
    case R_GIVEALL: o->kind = UIOV_TR_ACTION; strcpy(o->label, "Give all known weapons"); { char d[32]; snprintf(d, sizeof d, "%d known", s_nw); val(o, i, d); } return 1;
    case R_SCANW: o->kind = UIOV_TR_ACTION; strcpy(o->label, "Scan for weapons nearby"); val(o, i, "learns guns you / NPCs carry"); return 1;
    case R_H_MONEY: o->kind = UIOV_TR_HEADER; strcpy(o->label, "MONEY"); return 1;
    case R_M1: o->kind = UIOV_TR_ACTION; strcpy(o->label, "+ $10,000"); val(o, i, ""); return 1;
    case R_M2: o->kind = UIOV_TR_ACTION; strcpy(o->label, "+ $100,000"); val(o, i, ""); return 1;
    case R_MMAX: o->kind = UIOV_TR_ACTION; strcpy(o->label, "Set $9,999,995"); val(o, i, ""); return 1;
    case R_H_MODEL: o->kind = UIOV_TR_HEADER; strcpy(o->label, "MODEL"); return 1;
    case R_STORY:
        o->kind = UIOV_TR_CHOICE; strcpy(o->label, "Character");
        if (!s_nst) strcpy(o->value, s_have_player ? "reading the game's model list..." : "load into the game first");
        else { const int k = s_stsel < s_nst ? s_stsel : 0;
               snprintf(o->value, sizeof o->value, "%s  (%d/%d)", s_st[k].label, k + 1, s_nst); }
        return 1;
    case R_BECOME: o->kind = UIOV_TR_ACTION; strcpy(o->label, "Become this character"); val(o, i, ""); return 1;
    case R_COPYM: o->kind = UIOV_TR_ACTION; strcpy(o->label, "Copy lock-on target's model"); val(o, i, s_last_tgt ? "target ready" : "lock on first"); return 1;
    case R_RESETM: o->kind = UIOV_TR_ACTION; strcpy(o->label, "Back to your character"); val(o, i, ""); return 1;
    case R_H_WEATHER: o->kind = UIOV_TR_HEADER; strcpy(o->label, "WEATHER"); return 1;
    case R_RAIN: { const int r = g_gf_rain_poll ? g_gf_rain_poll() : 0;
        o->kind = UIOV_TR_CHOICE; o->on = r; strcpy(o->label, "Rain"); val(o, i, r ? "On" : "Off"); return 1; }
    }
    return 0;
}
static void t_change(int i, int dir)
{
    if (s_msg_row == i) s_msg_row = -1;
    switch (i) {
    case R_GOD: s_god = !s_god; printf("[trainer] god mode %s\n", s_god ? "ON" : "OFF"); fflush(stdout); break;
    case R_HEALTH: req(RQ_HEALTH); break;
    case R_AMMO: req(RQ_AMMO); break;
    case R_INF: s_infammo = !s_infammo; req(RQ_INFAMMO); printf("[trainer] infinite ammo %s\n", s_infammo ? "ON" : "OFF"); fflush(stdout); break;
    case R_WSEL: if (s_nw) s_wsel = (s_wsel + (dir < 0 ? s_nw - 1 : 1)) % s_nw; break;
    case R_GIVE: req(RQ_GIVE); break;
    case R_GIVEALL: req(RQ_GIVEALL); break;
    case R_SCANW: req(RQ_SCANW); break;
    case R_M1: req(RQ_MONEY1); break;
    case R_M2: req(RQ_MONEY2); break;
    case R_MMAX: req(RQ_MONEYMAX); break;
    case R_STORY: if (s_nst) s_stsel = (s_stsel + (dir < 0 ? s_nst - 1 : 1)) % s_nst; break;
    case R_BECOME: req(RQ_BECOME); s_msel = 1; break;
    case R_COPYM: req(RQ_COPYMODEL); break;
    case R_RESETM: req(RQ_RESETMODEL); break;
    case R_RAIN: req(g_gf_rain_poll && g_gf_rain_poll() ? RQ_RAIN_OFF : RQ_RAIN_ON); break;
    }
}
static int t_status(char* t, size_t n)
{
    if (!s_have_player) { snprintf(t, n, "not in game"); return 1; }
    snprintf(t, n, "Health %.0f / %.0f    Money $%d", (double)s_hp, (double)s_hpmax, s_money);
    return 1;
}
/* v1058C co-op status for the HUD line (gf_coop.c public API only; render thread, reads only) */
extern int gf_coop_connected(void);
extern const char* gf_coop_partner_name(void);
extern int gf_coop_partner_pose(float out_m[16], float* hp);
static int coop_hud(char* t, size_t n)
{
    extern int gf_coop_mode(void);
    const int on = gf_coop_mode() != 0;   /* v1119M: co-op starts / stops at runtime (pause menu > Co-op) */
    if (!on || n == 0) return 0;
    if (!gf_coop_connected()) { extern int gf_coop_mode(void); snprintf(t, n, gf_coop_mode() == 2 ? "CO-OP: connecting to the host..." : "CO-OP: hosting, waiting for players (pause menu > Co-op)"); return 1; }
    float m[16], hp = 0;
    if (!gf_coop_partner_pose(m, &hp)) { snprintf(t, n, "CO-OP: %s (not in the world)", gf_coop_partner_name()); return 1; }
    const uint32_t pl = player(); float d = -1;
    if (pl) { __try { const uint32_t xf = rd32(pl + 0x68);
        if (ok(xf)) { const float dx = m[12] - rdf(xf + 0x40), dy = m[13] - rdf(xf + 0x44), dz = m[14] - rdf(xf + 0x48); d = sqrtf(dx*dx + dy*dy + dz*dz); } }
        __except (EXCEPTION_EXECUTE_HANDLER) { d = -1; } }
    /* v1059E: say which window this is and whether the partner is moving (speed rides in m[15], see gf_coop.c add_motion) */
    char me_name[24]; { extern const char* gf_coop_local_name(void); const char* nm = gf_coop_local_name(); snprintf(me_name, sizeof me_name, "%s", nm && *nm ? nm : "you"); }   /* v1119M */
    const float sp = (m[15] == m[15] && m[15] >= 0.0f && m[15] < 60.0f) ? m[15] : 0.0f;
    const char* mv = sp < 0.3f ? "standing" : sp < 3.5f ? "walking" : "running";
    if (d >= 0) snprintf(t, n, "CO-OP [%s] partner %s  %.1f m  %s  HP %.0f", me_name, gf_coop_partner_name(), (double)d, mv, (double)hp);
    else snprintf(t, n, "CO-OP [%s] partner %s  %s  HP %.0f", me_name, gf_coop_partner_name(), mv, (double)hp);
    {   extern int gf_coop_mission_hud(char* out, int cap); char mh[96];   /* v1073H host-run mission banner */
        if (gf_coop_mission_hud(mh, (int)sizeof mh)) { const size_t l = strlen(t); if (l + 4 < n) snprintf(t + l, n - l, "  |  %s", mh); } }
    return 1;
}
static int t_hud(char* t, size_t n)
{
    char co[224]; const int has_co = coop_hud(co, sizeof co);   /* v1059E 96 -> 128, v1073H 224 */
    if (!s_god && !s_infammo && s_msel == 0) { if (has_co) snprintf(t, n, "%s", co); return has_co; }
    snprintf(t, n, "%s%s%s%s%s", s_god ? "GOD  " : "", s_infammo ? "INF AMMO  " : "", s_msel ? "MODEL" : "", has_co ? "    " : "", has_co ? co : "");
    return 1;
}
static const uiov_trainer_api k_api = { "TRAINER", t_count, t_row, t_change, t_status, t_hud };

void gf_trainer_init(void)
{
    const char* e = getenv("GF_TRAINER");
    { extern void gf_pcsettings_init(void); gf_pcsettings_init(); }   /* v1045G pause-menu PC SETTINGS (independent of the trainer) */
    { extern void gf_mainmenu_init(void); gf_mainmenu_init(); }       /* v1058Q main-menu QUIT TO DESKTOP (independent of the trainer) */
    { extern void gf_coop_init(void); gf_coop_init(); }               /* v1058C online co-op (GF_COOP=host|join|echo) */
    { extern void gf_coop_glue_init(void); gf_coop_glue_init(); }     /* v1058E co-op probes / puppet / pose */
    g_pad_title_tick_fn = main_tick;                                   /* v1058E: co-op ticks even with GF_TRAINER=0 */
    { extern void gf_shop_init(void); gf_shop_init(); }               /* v1092S inventory (I) + stores (B) */
    if (e && *e == '0') { printf("[trainer] v1045A off (GF_TRAINER=0)\n"); fflush(stdout); return; }
    w_load(); m_load();
    s_trainer_on = 1;
    g_uiov_trainer = &k_api;
    printf("[trainer] v1045D ready: F10 (%d weapon types, %d story characters remembered)\n", s_nw, s_nsaved); fflush(stdout);
}

/* ------------------------------------------------------------------ v1045G "PC SETTINGS" in the game's pause menu
 * The pause movie (shared\ui\menus\pause\pause.uix in godfather_global_global.str) builds a 14-entry array and then
 * splices out what the platform lacks; LEADERBOARDS ({Lable:"$top_don_menu_leaderboards", opensub:"leaderboards"}) stays
 * only if  pause.IsUserSignedIn() && frontend.GetCurrentState() == 9  and would sit right above QUIT GAME. So:
 *   IsUserSignedIn  (native func_00395DC0, pause-only)   -> true in-game, and arms a 50 ms window in which
 *   GetCurrentState (native func_0037F74C, frontend)     -> 9   (the very next call is the menu's own check)
 *   OpenSubMenu     (native func_003969E0)               -> name "leaderboards": g_uiov_request = 1 (rsx_ui_overlay.c
 *                     opens the PC Settings page: resolution, display mode, vsync, frame cap, prompts, ...) and returns
 *                     false, so the movie stays on the main list (it only jumps to frm_submenu on true); anything else
 *                     goes to the game's func_003965D4 exactly as before.
 *   label           the loaded English string "Leaderboards" is rewritten to "PC Settings" while in-game and put back at
 *                     the main menu (the frontend's Top Don menu uses the same string).
 * Natives are reached through the APT VM's indirect calls, so ppu_register_function() over the lifted entries is enough.
 * GF_PCSETTINGS=0 = off. Log [pcsettings]. */
extern volatile int g_uiov_request;
extern void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
static gfn o_signed, o_state, o_opensub, f_10EBC, f_12E0C, f_1236C, f_1132C, f_12D1C, f_12FCC, f_3965D4, f_109BC;
static volatile ULONGLONG s_force_state_t = 0;
static void wr64(uint32_t ea, uint64_t v) { wr32(ea, (uint32_t)(v >> 32)); wr32(ea + 4, (uint32_t)v); }
static void cc(gfn f, ppu_context* c)
{
    c->gpr[2] = TOC; f(c);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(c); }
}
static void hk_signed(ppu_context* c)
{
    if (!player()) { o_signed(c); return; }
    s_force_state_t = GetTickCount64();
    c->gpr[3] = 1; cc(f_10EBC, c);
}
/* v1045H: the movie's test is  if (!IsUserSignedIn() || GetCurrentState() == 9) splice(leaderboards)  -- v1045G read it the
 * wrong way round and FORCED 9, which removed the entry again (log: real state 16 in-game). Now the state is left alone;
 * only if it is 9 right after the sign-in check does the menu get 16 instead. */
static void hk_state(ppu_context* c)
{
    const ULONGLONG t0 = s_force_state_t;
    if (t0 && GetTickCount64() - t0 < 50) {
        s_force_state_t = 0;
        const int real = (int32_t)rd32(0x101EC408u);
        { static int n = 0; if (n++ < 3) { printf("[pcsettings] v1045H pause menu built (frontend state %d) -> PC SETTINGS entry kept\n", real); fflush(stdout); } }
        if (real == 9) { c->gpr[3] = 16; cc(f_12E0C, c); return; }
    }
    o_state(c);
}
static void hk_opensub(ppu_context* c)
{
    if (!player()) { o_opensub(c); return; }
    const uint64_t r1 = c->gpr[1], self = c->gpr[3], lr = c->lr, r28 = c->gpr[28], r29 = c->gpr[29];
    c->gpr[1] = r1 - 160; wr64((uint32_t)c->gpr[1], r1);   /* same frame as the original */
    const uint32_t obj = (uint32_t)(r1 - 160 + 112);
    c->gpr[3] = obj; cc(f_1236C, c);                        /* string object */
    c->gpr[3] = 0; cc(f_1132C, c);                          /* argument 0 */
    c->gpr[4] = obj; cc(f_12D1C, c);                        /* -> string */
    c->gpr[3] = obj; cc(f_12FCC, c);                        /* -> char* */
    const uint32_t sp = (uint32_t)c->gpr[3];
    char name[40]; name[0] = 0;
    if (ok(sp)) { int i = 0; for (; i < 39 && vm_base[sp + i]; i++) name[i] = (char)vm_base[sp + i]; name[i] = 0; }
    if (!strcmp(name, "leaderboards")) {
        g_uiov_request = 1;
        printf("[pcsettings] v1119M CO-OP selected -> co-op / PC settings page\n"); fflush(stdout);
        c->gpr[3] = 0;
    } else { c->gpr[3] = self; c->gpr[4] = sp; cc(f_3965D4, c); }
    cc(f_10EBC, c);
    const uint64_t val = c->gpr[3];
    c->gpr[3] = obj; cc(f_109BC, c);
    c->gpr[1] = r1; c->gpr[3] = val; c->lr = lr; c->gpr[28] = r28; c->gpr[29] = r29; c->gpr[2] = TOC;
}
/* label: "Leaderboards" <-> "Co-op" (v1119M; was "PC Settings" -- the page now has CO-OP and PC SETTINGS tabs). The new text is
 * NUL-padded to the 12 + NUL slot. */
static const char k_lbl_new[13] = "Co-op\0\0\0\0\0\0\0";   /* 13 bytes */
static uint32_t s_lbl[8]; static int s_nlbl = 0;
static void lbl_find(void)
{
    static const char want[] = "\0Leaderboards\0", alt[] = "\0Co-op\0\0\0\0\0\0\0\0";   /* both 14 bytes */
    s_nlbl = 0;
    for (uint64_t ea = 0x20000000ull; ea < 0xF0000000ull && s_nlbl < 8; ) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vm_base + ea, &mi, sizeof mi)) break;
        uint64_t rs = (uint64_t)((uint8_t*)mi.BaseAddress - vm_base), re = rs + mi.RegionSize;
        if (re <= ea) break;
        if (rs < ea) rs = ea;
        if (re > 0xF0000000ull) re = 0xF0000000ull;
        const DWORD pr = mi.Protect & 0xFF;
        if (mi.State == MEM_COMMIT && !(mi.Protect & PAGE_GUARD) && (pr == PAGE_READWRITE || pr == PAGE_EXECUTE_READWRITE)) {
            __try {
                const uint8_t* b = vm_base + rs; const size_t len = (size_t)(re - rs);
                for (size_t i = 0; i + 15 <= len && s_nlbl < 8; i++)
                    if ((b[i + 1] == 'L' || b[i + 1] == 'C') && (!memcmp(b + i, want, 14) || !memcmp(b + i, alt, 14))) s_lbl[s_nlbl++] = (uint32_t)(rs + i + 1);
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
        ea = re;
    }
}
static void lbl_set(int pc)
{
    for (int i = 0; i < s_nlbl; i++) {
        uint8_t* p = vm_base + s_lbl[i];
        if (memcmp(p, "Leaderboards", 13) && memcmp(p, k_lbl_new, 13)) continue;   /* memory reused */
        if (pc) memcpy(p, k_lbl_new, 13); else memcpy(p, "Leaderboards", 13);
    }
}
static DWORD WINAPI lbl_thread(LPVOID arg)
{
    (void)arg; int state = -1; ULONGLONG last_find = 0;
    for (;;) {
        Sleep(500);
        __try {
            const int ing = player() != 0;
            const ULONGLONG t = GetTickCount64();
            if (ing && (s_nlbl == 0 || state != 1) && (s_nlbl || t - last_find > 10000 || !last_find)) {
                if (!s_nlbl) { last_find = t; lbl_find(); printf("[pcsettings] \"Leaderboards\" label: %d cop%s\n", s_nlbl, s_nlbl == 1 ? "y" : "ies"); fflush(stdout); }
                if (s_nlbl) { lbl_set(1); state = 1; }
            } else if (ing && state == 1) {
                for (int i = 0; i < s_nlbl; i++) if (memcmp(vm_base + s_lbl[i], k_lbl_new, 12)) { s_nlbl = 0; state = -1; break; }   /* reloaded */
            } else if (!ing && state == 1) { lbl_set(0); state = 0; }
        } __except (EXCEPTION_EXECUTE_HANDLER) { s_nlbl = 0; state = -1; }
    }
}
void gf_pcsettings_init(void)
{
    const char* e = getenv("GF_PCSETTINGS");
    if (e && *e == '0') { printf("[pcsettings] v1045G off (GF_PCSETTINGS=0)\n"); fflush(stdout); return; }
    o_signed = lookup(0x00395DC0u); o_state = lookup(0x0037F74Cu); o_opensub = lookup(0x003969E0u);
    f_10EBC = lookup(0x00010EBCu); f_12E0C = lookup(0x00012E0Cu); f_1236C = lookup(0x0001236Cu); f_1132C = lookup(0x0001132Cu);
    f_12D1C = lookup(0x00012D1Cu); f_12FCC = lookup(0x00012FCCu); f_3965D4 = lookup(0x003965D4u); f_109BC = lookup(0x000109BCu);
    if (!o_signed || !o_state || !o_opensub || !f_10EBC || !f_12E0C || !f_1236C || !f_1132C || !f_12D1C || !f_12FCC || !f_3965D4 || !f_109BC) {
        printf("[pcsettings] v1045G: a game function is missing -- pause menu entry off\n"); fflush(stdout); return; }
    ppu_register_function(0x00395DC0u, hk_signed);
    ppu_register_function(0x0037F74Cu, hk_state);
    ppu_register_function(0x003969E0u, hk_opensub);
    { HANDLE h = CreateThread(NULL, 256u << 10, lbl_thread, NULL, 0, NULL); if (h) CloseHandle(h); }
    printf("[pcsettings] v1045H ready: PC SETTINGS in the pause menu (above QUIT GAME)\n"); fflush(stdout);
}

/* ------------------------------------------------------------------ v1058Q "QUIT TO DESKTOP" on the main menu
 * Main menu = class MainMenu in the APT movie shared\ui\menus\frontend\frontend.uix (frontend\frontend.str, RefPack
 * chunk 0x2F6000; apt data at chunk+0x59C20, const file at +0x5C740). Its constructor does
 *     totalButtons = 6;  buttonText = [playgame, loadgame, options, credits, training_videos, fe_le_bonus_001];
 * (one PushData + InitArray; button i is placed at _y = 24 * i, so index 5 = BONUS VIDEOS at the bottom), Up/Down wrap
 * mod totalButtons, and ACCEPT calls the native frontend.SetMainMenuOption(index) (func_00382948, ignores index > 5).
 * gf_mainmenu_uix() patches the file buffer right before the uix tag parser (func_003F87CC, hand-edited call at
 * 0x003F32CC in ppu_recomp_001.cpp) reads it -- offsets are still the file's own then:
 *   - the button-text PushData list (9 entries at apt+0x2594) grows to 10, taking the 1-entry list right after it
 *     (apt+0x25B8) that belonged to a debug  trace("**********screen_stub.as**********") ; that PushData now pushes
 *     nothing and the Trace opcode becomes Stop (the block ends with stop() anyway). The loader converts every PushData
 *     list in place, so a list must never be shared: v1058Q first pointed the trace at the next list ('this') -> that
 *     list was converted twice -> black frontend;
 *   - the new 7th element is that trace's string constant, rewritten to "Quit to Desktop" (the menu draws in caps);
 *   - the two integer constants 6 (totalButtons, InitArray count; each used once, only here) become 7.
 * hk_setmm (over SetMainMenuOption): index 6 -> flush logs + TerminateProcess (same exit as the window's X button).
 * Every byte is checked against the original first; any mismatch = no patch. GF_QUIT_MENU=0 = off. Log [quitmenu]. */
static int s_qd_on = -1;
static volatile int s_qd_patched = 0;
static gfn o_setmm, f_1315C;
static int qd_enabled(void)
{
    if (s_qd_on < 0) { const char* e = getenv("GF_QUIT_MENU"); s_qd_on = !(e && *e == '0'); }
    return s_qd_on;
}
void gf_mainmenu_uix(uint64_t stream)
{
    if (!qd_enabled()) return;
    const uint32_t so = (uint32_t)stream;
    if (!ok(so)) return;
    const uint32_t base = rd32(so), size = rd32(so + 4);
    if (!ok(base) || size < 0x3000u || size > 0x200000u || !ok(base + size)) return;
    if (memcmp(vm_base + base, "uixf", 4)) return;
    uint32_t A = 0, C = 0, tit = 0;
    for (uint32_t p = base + 8, n = 0; p + 8 <= base + size && n < 64; n++) {   /* tag walk: id, size incl. header */
        const uint32_t tag = rd32(p), sz = rd32(p + 4);
        if (sz < 8 || sz > size) break;
        if (tag == 0x7469746Cu) tit = p + 8;          /* titl */
        else if (tag == 0x61707469u) A = p + 8;       /* apti */
        else if (tag == 0x636F6E73u) C = p + 8;       /* cons */
        p += sz;
    }
    if (!tit || memcmp(vm_base + tit, "frontend", 9)) return;   /* some other movie */
    static const char k_trace[] = "**********screen_stub.as**********", k_label[] = "Quit to Desktop";
    if (!A || !C || C <= A || C - A > 0x10000u) { printf("[quitmenu] v1058Q frontend.uix without apt/const data -- not patched\n"); fflush(stdout); return; }
    /* already patched (buffer re-parsed)? */
    if (rd32(A + 0x161C) == 10 && !memcmp(vm_base + C + 0x135C, k_label, sizeof k_label)) { s_qd_patched = 1; return; }
    static const uint32_t k_list[10] = { 206, 207, 208, 209, 210, 211, 212, 213, 214, 215 };
    int good = !memcmp(vm_base + A, "Apt Data:7\x1a", 11) && !memcmp(vm_base + C, "Apt constant file\x1a", 18)
            && rd32(C + 0x18) == 0x218u
            && vm_base[A + 0x161A] == 0x96 && rd32(A + 0x161C) == 9 && rd32(A + 0x1620) == 0x2594u      /* PushData buttonText */
            && vm_base[A + 0x1626] == 0x96 && rd32(A + 0x1628) == 1 && rd32(A + 0x162C) == 0x25B8u      /* PushData trace str */
            && vm_base[A + 0x1630] == 0x26                                                             /* Trace */
            && rd32(C + 0x20 + 8 * 201) == 7 && rd32(C + 0x24 + 8 * 201) == 6                          /* int 6 totalButtons */
            && rd32(C + 0x20 + 8 * 214) == 7 && rd32(C + 0x24 + 8 * 214) == 6                          /* int 6 array count */
            && rd32(C + 0x20 + 8 * 42) == 1 && rd32(C + 0x24 + 8 * 42) == 0x135Cu                      /* string const 42 */
            && !memcmp(vm_base + C + 0x135C, k_trace, sizeof k_trace);
    for (int i = 0; good && i < 10; i++) if (rd32(A + 0x2594 + 4u * (uint32_t)i) != k_list[i]) good = 0;
    if (!good) { printf("[quitmenu] v1058Q frontend.uix layout not recognised -- main menu left as is\n"); fflush(stdout); return; }
    static const uint32_t k_new[10] = { 206, 207, 215, 208, 209, 210, 211, 212, 213, 214 };   /* reg1,'buttonText',QUIT,bonus..playgame,7 */
    wr32(A + 0x1628, 0);                                              /* trace's PushData: empty (its list slot is ours now) */
    vm_base[A + 0x1630] = 0x07;                                       /* Trace -> Stop (the block ends with stop() anyway) */
    for (int i = 0; i < 10; i++) wr32(A + 0x2594 + 4u * (uint32_t)i, k_new[i]);
    wr32(A + 0x161C, 10);
    wr32(C + 0x24 + 8 * 201, 7); wr32(C + 0x24 + 8 * 214, 7);
    memset(vm_base + C + 0x135C, 0, sizeof k_trace); memcpy(vm_base + C + 0x135C, k_label, sizeof k_label);
    s_qd_patched = 1;
    printf("[quitmenu] v1058Q main menu: QUIT TO DESKTOP added under BONUS VIDEOS (apt 0x%08X)\n", A); fflush(stdout);
}
static void hk_setmm(ppu_context* c)
{
    if (s_qd_patched) {
        const uint64_t r1 = c->gpr[1], self = c->gpr[3], lr = c->lr;
        c->gpr[1] = r1 - 0x90; wr64((uint32_t)c->gpr[1], r1);   /* own frame: the getters' stubs save r2 at 0x28(r1) */
        c->gpr[3] = 0; cc(f_1132C, c);                          /* argument 0 */
        cc(f_1315C, c);                                         /* -> int (same two calls as the native) */
        const int opt = (int32_t)c->gpr[3];
        c->gpr[1] = r1; c->gpr[3] = self; c->lr = lr; c->gpr[2] = TOC;
        if (opt == 6) {
            printf("[quitmenu] QUIT TO DESKTOP selected -> exiting\n"); fflush(stdout); fflush(stderr);
            Sleep(150);                                         /* let the menu's push sound start */
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }
    o_setmm(c);
}
void gf_mainmenu_init(void)
{
    if (!qd_enabled()) { printf("[quitmenu] v1058Q off (GF_QUIT_MENU=0)\n"); fflush(stdout); return; }
    o_setmm = lookup(0x00382948u); f_1315C = lookup(0x0001315Cu);
    if (!f_1132C) f_1132C = lookup(0x0001132Cu);
    if (!o_setmm || !f_1315C || !f_1132C) { s_qd_on = 0; printf("[quitmenu] v1058Q: a game function is missing -- off\n"); fflush(stdout); return; }
    ppu_register_function(0x00382948u, hk_setmm);
    printf("[quitmenu] v1058Q ready: QUIT TO DESKTOP on the main menu\n"); fflush(stdout);
}
