/* gf_combat.c -- v1042X: what the player has in hand, for the PC keyboard/mouse combat layout (cellPad.c).
 *
 * Godfather's fist and melee-weapon attacks ("Black Hand") are right-stick flicks while locked on, and R1 fires a
 * gun. cellPad.c's KBM combat layout maps both to LMB, so it has to know whether a gun is drawn. It asks through
 * g_pad_weapon_fn, which this file sets: 1 = gun in hand, 0 = fists / melee weapon / thrown item, -1 = no player.
 *
 * Read the same way the game's own script native GetCurrentWeapon (func_003A33A8) does:
 *   player   = *(*(*(*(0x10181290)) + 4)) - 0x17FC        (TOC 0xB4BF60 + 0x508C holds 0x10181290)
 *   in hand  = *(player + 0x468) - 0x38                    (0 = nothing)
 *   item     = *(u8*)(in hand + 0xD9)                      (0x00..0x33)
 *   slot     = func_000E32C0's table below (0..10, 11 = none); the native also requires player+0x5C4 & 0x200.
 * Slots by item range: 10 = 0x00-0x04, 9 = 0x05-0x06, 5 = 0x07-0x15 (15 items: the melee weapons), 0 = 0x16-0x1A,
 * 1 = 0x1B-0x1F, 2 = 0x20-0x22 + 0x32, 3 = 0x23-0x25 + 0x2A + 0x33, 4 = 0x26-0x29, 6 = 0x2B, 8 = 0x2C, 7 = 0x2D,
 * 11 = 0x2E-0x31. Which slots are guns is GF_GUN_SLOTS (default "0,1,2,3": pistols, shotguns, SMGs, rifles -- the
 * weapon-wheel groups with 5/5/4/5 entries). [combat] logs every change of item / slot / gun, so a run shows
 * whether the guess is right; fix it with GF_GUN_SLOTS without a rebuild.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern uint8_t* vm_base;
extern int (*volatile g_pad_weapon_fn)(void);
extern int (*volatile g_pad_drawn_fn)(void);   /* v1049H cellPad.c: H key */

static const signed char k_slot[0x34] = {
    10, 10, 10, 10, 10, 9, 9, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 4, 3, 6, 8, 7, 11, 11, 11, 11, 2, 3 };
static unsigned s_gun_mask = 0x001Fu;   /* bit n = slot n is a gun. v1049W: + slot 4 (items 0x26-0x29, a firearm class:
                                          * drawn 0x26 with flag 0x200 set was treated as melee -> no free aim) */

static int ok(uint32_t ea) { return ea >= 0x10000u && ea < 0xF0000000u; }
static uint32_t rd32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* item in hand: -2 no player, -1 empty hand, else item id; *flags = player+0x5C4 */
static int read_item(uint32_t* flags)
{
    uint32_t a = rd32(0x10181290u);
    if (!ok(a)) return -2;
    a = rd32(a + 4);
    if (!ok(a)) return -2;
    a = rd32(a);
    if (!ok(a) || a < 0x17FCu) return -2;
    const uint32_t player = a - 0x17FCu;
    if (!ok(player)) return -2;
    *flags = rd32(player + 0x5C4);
    const uint32_t h = rd32(player + 0x468);
    if (!h) return -1;
    if (!ok(h) || h < 0x38u) return -2;
    return vm_base[h - 0x38u + 0xD9u];
}
/* v1053V "in a car" for cellPad.c's car controls (g_pad_incar_fn). Found with GF_PAUSEDIFF (user 21:21, F7 on foot / car /
 * foot / car, player 0x42F23410): player+0x1718 and player+0x4C = the vehicle (0x4340B7B8) in the car, 0 on foot; player+0x5C4
 * gains bit 0x8 in the car. Both must hold. */
static int incar_read(void)
{
    uint32_t a = rd32(0x10181290u);
    if (!ok(a)) return 0;
    a = rd32(a + 4); if (!ok(a)) return 0;
    a = rd32(a);     if (!ok(a) || a < 0x17FCu) return 0;
    const uint32_t player = a - 0x17FCu;
    if (!ok(player)) return 0;
    const uint32_t veh = rd32(player + 0x1718u);
    return ok(veh) && veh == rd32(player + 0x4Cu) && (rd32(player + 0x5C4u) & 0x8u) != 0;
}
/* v1130T car speed for cellPad.c (PC Classic: Space gets out only when stopped): position delta of the player's vehicle
 * (player+0x1718, see incar_read) over >= 100 ms windows. -1 = no car / unknown. */
extern int gf_rag_ent_frame(uint32_t ent, float* M9, float* T3);
static float gf_pad_car_speed(void)
{
    static uint32_t lv = 0; static float lp[3], spd = 0.0f; static ULONGLONG lt = 0;
    __try {
        if (!incar_read()) { lv = 0; return -1.0f; }
        uint32_t a = rd32(rd32(rd32(0x10181290u) + 4)); const uint32_t veh = rd32(a - 0x17FCu + 0x1718u);
        float M[9], T[3]; if (!gf_rag_ent_frame(veh, M, T)) return -1.0f;
        const ULONGLONG now = GetTickCount64();
        if (veh != lv) { lv = veh; memcpy(lp, T, sizeof lp); lt = now; spd = 0.0f; return 0.0f; }
        if (now - lt >= 100) { const float dx = T[0] - lp[0], dy = T[1] - lp[1], dz = T[2] - lp[2];
            spd = sqrtf(dx * dx + dy * dy + dz * dz) / ((float)(now - lt) / 1000.0f); memcpy(lp, T, sizeof lp); lt = now; }
        return spd;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1.0f; }
}
static int gf_pad_incar(void)
{
#ifdef _WIN32
    __try { return incar_read(); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
#else
    return incar_read();
#endif
}
static int read_item_safe(uint32_t* flags)
{
#ifdef _WIN32
    __try { return read_item(flags); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
#else
    return read_item(flags);
#endif
}

/* v1043Y pause-menu detector for cellPad.c g_pad_menu_fn (WASD = d-pad while paused). Found with GF_PAUSEDIFF in two
 * user sessions (playing / paused / playing / paused): a 4-entry array at 0x40C77ED4, stride 0x18, same address both runs,
 * reads {0, 0} per entry while playing and {1, 2} while paused. All four entries must match (no false "paused" while
 * walking). GF_PAUSE_PROBE=0 = off. */
extern int (*volatile g_pad_menu_fn)(void);
/* v1044F direct free-aim camera (user: free aim "EXTREMELY slow"). Reverse engineered (EBOOT + [getstick-chain] probe):
 * the free-aim camera is class vtable 0x100417A8 (ctor 0x61734, update 0x60AE0 reads stick 1 at 0x60CF0). Its tuning block
 * is obj+0x78: +0x08 base 0.5 deg/frame, +0x0C max 2.0 deg/frame (ramp after 1 s of full stick, +0x10), smoothing +0x24;
 * the update integrates yaw obj+0x10C and pitch obj+0x110 (degrees, wrapped to +-180) -> 30..120 deg/s by stick. Here the
 * mouse is written straight into those angles (stick held at 0 by cellPad.c). Objects are found by scanning guest RAM for
 * the vtable word (background thread, rescan every 3 s while aiming); each write re-checks the vtable. GF_AIM_DIRECT=0
 * off, PAD_KBM_AIM_DEG (cellPad.c) = degrees per mouse count, PAD_KBM_AIM_INVY=1 inverts up/down, GF_AIM_PITCH_MAX (70).
 * Log [aim]. */
enum { GCAM_VT = 0x100417A8u, GCAM_MAX = 48 };
static volatile uint32_t s_cam[GCAM_MAX]; static volatile int s_ncam = 0, s_scan_req = 0;
/* v1044P (user: still "so bad"; log 01:00): the game makes a NEW camera object on every free-aim entry and the old ones
 * stay in RAM with a valid vtable/position. The scan often ran before the new one existed, found only dead ones, and then
 * only rescanned every 3 s -- the mouse moved dead cameras while the real crosshair (stick held at 0) stood still.
 * Now: on entry every known object is recorded as dead (address + yaw/pitch); an object that is not in that list, or is
 * in it with different values (the ctor resets yaw/pitch at a reused address), is the live one and gets ALL writes.
 * Until it is found the scan repeats every ~30 ms; after GF_AIM_FALLBACK_MS (250) the mouse goes to every candidate
 * (writes to dead objects are harmless) while the search continues. */
static uint32_t s_dead[64]; static float s_dead_y[64], s_dead_p[64]; static int s_ndead = 0;
static volatile uint32_t s_live = 0;
static volatile uint32_t s_win_lo = 0, s_win_hi = 0;   /* scan window around the last live camera (0 = full scan) */
static float rdf(uint32_t ea);
static void dead_note(uint32_t o)   /* remember o (and its current angles) as a camera from an earlier entry */
{
    if (rd32(o) != GCAM_VT) return;
    int k = 0; while (k < s_ndead && s_dead[k] != o) k++;
    if (k == s_ndead) { if (s_ndead < 64) s_ndead++; else { memmove(s_dead, s_dead + 1, 63 * 4); memmove(s_dead_y, s_dead_y + 1, 63 * 4);
                                                             memmove(s_dead_p, s_dead_p + 1, 63 * 4); k = 63; } }
    s_dead[k] = o; s_dead_y[k] = rdf(o + 0x110); s_dead_p[k] = rdf(o + 0x10C);
}
static float rdf(uint32_t ea) { uint32_t u = rd32(ea); float f; memcpy(&f, &u, 4); return f; }
static void wrf(uint32_t ea, float f) { uint32_t u; memcpy(&u, &f, 4); uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(u >> 24); p[1] = (uint8_t)(u >> 16); p[2] = (uint8_t)(u >> 8); p[3] = (uint8_t)u; }
static int fok(float f) { return f == f && f > -100000.0f && f < 100000.0f; }
static uint32_t gad_player(void);
static int gad_pos(uint32_t obj, float* p);
static int gcam_valid(uint32_t o)
{
    if (rd32(o) != GCAM_VT) return 0;
    { /* v1044K: a live free-aim camera sits next to the player (dead objects keep their old vtable and position) */
      const uint32_t pl = gad_player(); float pp[3];
      if (pl && gad_pos(pl, pp)) {
          const float x = rdf(o + 0xE0) - pp[0], y = rdf(o + 0xE4) - pp[1], z = rdf(o + 0xE8) - pp[2];
          if (!(x * x + y * y + z * z < 64.0f)) return 0;
      } }
    const float base = rdf(o + 0x78 + 0x08), mx = rdf(o + 0x78 + 0x0C), yaw = rdf(o + 0x10C), pit = rdf(o + 0x110);
    return fok(base) && fok(mx) && base >= 0.0f && base < 100.0f && mx >= 0.0f && mx < 100.0f &&
           fok(yaw) && yaw >= -720.0f && yaw <= 720.0f && fok(pit) && pit >= -720.0f && pit <= 720.0f;
}
static void gcam_scan(void)
{
    uint32_t found[GCAM_MAX]; int n = 0;
    const uint8_t want[4] = { 0x10, 0x04, 0x17, 0xA8 };
    const uint64_t lo = s_win_lo ? s_win_lo : 0x20000000ull, hi = s_win_lo ? s_win_hi : 0xF0000000ull;
    for (uint64_t ea = lo; ea < hi && n < GCAM_MAX; ) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vm_base + ea, &mi, sizeof mi)) break;
        uint64_t rs = (uint64_t)((uint8_t*)mi.BaseAddress - vm_base), re = rs + mi.RegionSize;
        if (re <= ea) break;
        if (rs < ea) rs = ea;
        if (re > hi) re = hi;
        const DWORD pr = mi.Protect & 0xFF;
        if (mi.State == MEM_COMMIT && !(mi.Protect & PAGE_GUARD) && (pr == PAGE_READWRITE || pr == PAGE_EXECUTE_READWRITE)) {
#ifdef _WIN32
            __try {
#endif
                const uint8_t* b = vm_base + rs; const size_t len = (size_t)(re - rs);
                for (size_t i = 0; i + 4 <= len && n < GCAM_MAX; i += 4)
                    if (b[i] == 0x10 && !memcmp(b + i, want, 4) && gcam_valid((uint32_t)(rs + i))) found[n++] = (uint32_t)(rs + i);
#ifdef _WIN32
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
#endif
        }
        ea = re;
    }
    for (int i = 0; i < n; i++) s_cam[i] = found[i];
    if (!s_live) {   /* v1044P: pick the camera made by this entry */
        uint32_t fresh = 0, fresh0 = 0;
        for (int i = 0; i < n && !fresh0; i++) {
            int dead = 0;
            for (int k = 0; k < s_ndead; k++)
                if (s_dead[k] == found[i]) {
                    { uint32_t yu, pu; memcpy(&yu, &s_dead_y[k], 4); memcpy(&pu, &s_dead_p[k], 4);
                      dead = rd32(found[i] + 0x110) == yu && rd32(found[i] + 0x10C) == pu; }
                    if (!dead) dead = -1;   /* reused address, values reset -> fresh */
                    break;
                }
            if (dead <= 0) { if (!fresh) fresh = found[i];
                             if (rdf(found[i] + 0x110) == 0.0f && rdf(found[i] + 0x10C) == 0.0f) fresh0 = found[i]; }   /* ctor angles */
        }
        if (fresh0) fresh = fresh0;
        if (fresh) {
            s_live = fresh;
            s_win_lo = fresh > 0x21000000u ? fresh - 0x01000000u : 0x20000000u;
            s_win_hi = fresh < 0xEF000000u ? fresh + 0x01000000u : 0xF0000000u;
            printf("[aim] v1044P live free-aim camera 0x%08X (%d candidate(s), %d dead)\n", fresh, n, s_ndead); fflush(stdout);
        } else if (s_win_lo && n == 0) { s_win_lo = s_win_hi = 0; }   /* nothing in the window: full scan next time */
    }
    { static int last = -1, nlog = 0;
      if ((n != last || nlog < 3) && nlog < 40) { nlog++; last = n;
          printf("[aim] v1044F free-aim camera scan: %d object(s)", n);
          for (int i = 0; i < n && i < 6; i++)
              printf(" 0x%08X(yaw %.1f pitch %.1f speed %.2f..%.2f)", found[i], rdf(found[i] + 0x110), rdf(found[i] + 0x10C),
                     rdf(found[i] + 0x80), rdf(found[i] + 0x84));
          printf("\n"); fflush(stdout); } }
    s_ncam = n;
}
/* v1044H GF_AIMDUMP=1 (diagnostic for aim assist): every second while a free-aim camera is known, dump the camera
 * object (0x200 bytes as floats), the player (object, vtable, transform pos = *(player+0x68)+0x40), and the nearest
 * objects sharing the player's vtable (other characters?) with their positions and distances. Log [aimdump]. */
static uint32_t gad_player(void)
{
    uint32_t a = rd32(0x10181290u); if (!ok(a)) return 0;
    a = rd32(a + 4); if (!ok(a)) return 0;
    a = rd32(a); if (!ok(a) || a < 0x17FCu) return 0;
    return a - 0x17FCu;
}
static int gad_pos(uint32_t obj, float* p)
{
    const uint32_t m = rd32(obj + 0x68); if (!ok(m)) return 0;
    p[0] = rdf(m + 0x40); p[1] = rdf(m + 0x44); p[2] = rdf(m + 0x48);
    return fok(p[0]) && fok(p[1]) && fok(p[2]);
}
static void gad_dump(void)
{
    static int n = 0; if (n >= 25) return; n++;
    const uint32_t o = s_ncam ? s_cam[0] : 0;
    if (o && gcam_valid(o))
        for (int r = 0; r < 0x200; r += 0x40) {
            printf("[aimdump] #%d cam 0x%08X+%03X:", n, o, r);
            for (int k = 0; k < 0x40; k += 4) printf(" %g", rdf(o + r + k));
            printf("\n");
        }
    const uint32_t pl = gad_player();
    float pp[3] = {0, 0, 0};
    const int havep = pl && gad_pos(pl, pp);
    const uint32_t vt = pl ? rd32(pl) : 0;
    printf("[aimdump] #%d player 0x%08X vt 0x%08X +0x68 0x%08X pos %s %g %g %g\n", n, pl, vt, pl ? rd32(pl + 0x68) : 0,
           havep ? "" : "(bad)", pp[0], pp[1], pp[2]);
    if (!havep || !vt) { fflush(stdout); return; }
    typedef struct { uint32_t o; float d, x, y, z; } ped;
    ped best[10]; int nb = 0, total = 0;
    const uint8_t want[4] = { (uint8_t)(vt >> 24), (uint8_t)(vt >> 16), (uint8_t)(vt >> 8), (uint8_t)vt };
    for (uint64_t ea = 0x20000000ull; ea < 0xF0000000ull; ) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vm_base + ea, &mi, sizeof mi)) break;
        uint64_t rs = (uint64_t)((uint8_t*)mi.BaseAddress - vm_base), re = rs + mi.RegionSize;
        if (re <= ea) break;
        if (rs < ea) rs = ea;
        if (re > 0xF0000000ull) re = 0xF0000000ull;
        const DWORD pr = mi.Protect & 0xFF;
        if (mi.State == MEM_COMMIT && !(mi.Protect & PAGE_GUARD) && (pr == PAGE_READWRITE || pr == PAGE_EXECUTE_READWRITE)) {
#ifdef _WIN32
            __try {
#endif
                const uint8_t* b = vm_base + rs; const size_t len = (size_t)(re - rs);
                for (size_t i = 0; i + 4 <= len; i += 4) {
                    if (b[i] != want[0] || memcmp(b + i, want, 4)) continue;
                    const uint32_t q = (uint32_t)(rs + i); float qp[3];
                    if (q == pl || !gad_pos(q, qp)) continue;
                    const float dx = qp[0] - pp[0], dy = qp[1] - pp[1], dz = qp[2] - pp[2];
                    const float d = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (!(d < 5000.0f)) continue;
                    total++;
                    int k; if (nb < 10) k = nb++; else { if (d >= best[9].d) continue; k = 9; }
                    best[k].o = q; best[k].d = d; best[k].x = qp[0]; best[k].y = qp[1]; best[k].z = qp[2];
                    for (int m = k; m > 0 && best[m].d < best[m - 1].d; m--) { ped t = best[m]; best[m] = best[m - 1]; best[m - 1] = t; }
                }
#ifdef _WIN32
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
#endif
        }
        ea = re;
    }
    printf("[aimdump] #%d %d objects with the player's vtable; nearest:", n, total);
    for (int i = 0; i < nb; i++) printf(" [0x%08X d=%.2f %.2f %.2f %.2f]", best[i].o, best[i].d, best[i].x, best[i].y, best[i].z);
    printf("\n"); fflush(stdout);
}
/* v1044I GF_TARGETDUMP=1 (diagnostic): every 150 ms, words in player+0 .. +0x2000 that point at an object with a valid
 * transform (+0x68 -> +0x40 position) 0.5..60 m from the player; prints each (offset, object) pair when it appears and
 * when it goes away, with the object's vtable and distance. Lock on (T) to someone to see which field holds the target. */
static void gtd_scan(void)
{
    static uint32_t prev_off[256], prev_obj[256]; static int nprev = 0, nlog = 0;
    const uint32_t pl = gad_player(); float pp[3];
    if (!pl || !gad_pos(pl, pp) || nlog > 400) return;
    uint32_t cur_off[256], cur_obj[256]; int ncur = 0;
    for (uint32_t off = 0; off < 0x2000 && ncur < 256; off += 4) {
        const uint32_t q = rd32(pl + off);
        if (!ok(q) || q == pl || (q >= pl && q < pl + 0x2000) || (q & 3)) continue;
        float qp[3]; if (!gad_pos(q, qp)) continue;
        const float dx = qp[0] - pp[0], dy = qp[1] - pp[1], dz = qp[2] - pp[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (!(d > 0.5f && d < 60.0f)) continue;
        cur_off[ncur] = off; cur_obj[ncur] = q; ncur++;
    }
    for (int i = 0; i < ncur; i++) {
        int was = 0; for (int j = 0; j < nprev; j++) if (prev_off[j] == cur_off[i] && prev_obj[j] == cur_obj[i]) { was = 1; break; }
        if (!was) { float qp[3]; gad_pos(cur_obj[i], qp);
            const float dx = qp[0] - pp[0], dy = qp[1] - pp[1], dz = qp[2] - pp[2];
            printf("[targetdump] t=%llu NEW player+0x%04X -> 0x%08X vt 0x%08X d=%.2f pos %.2f %.2f %.2f\n",
                   (unsigned long long)GetTickCount64(), cur_off[i], cur_obj[i], rd32(cur_obj[i]), sqrtf(dx*dx+dy*dy+dz*dz), qp[0], qp[1], qp[2]);
            nlog++; }
    }
    for (int j = 0; j < nprev; j++) {
        int still = 0; for (int i = 0; i < ncur; i++) if (prev_off[j] == cur_off[i] && prev_obj[j] == cur_obj[i]) { still = 1; break; }
        if (!still) { printf("[targetdump] t=%llu GONE player+0x%04X -> 0x%08X\n", (unsigned long long)GetTickCount64(), prev_off[j], prev_obj[j]); nlog++; }
    }
    memcpy(prev_off, cur_off, sizeof(uint32_t) * ncur); memcpy(prev_obj, cur_obj, sizeof(uint32_t) * ncur); nprev = ncur;
    fflush(stdout);
}
static DWORD WINAPI gtd_thread(LPVOID arg)
{
    (void)arg;
    printf("[targetdump] v1044I ON: lock on (T) to different people a few times\n"); fflush(stdout);
    for (;;) { Sleep(150);
#ifdef _WIN32
        __try { gtd_scan(); } __except (EXCEPTION_EXECUTE_HANDLER) { }
#else
        gtd_scan();
#endif
    }
}

/* ---- v1044T own lock-on inside free aim (user: "I can't have a locked aim and get fluently into free aim").
 * The game's lock-on (L1) and free aim (R3) are separate modes and every way of switching between them by faking
 * button presses broke (v1044K-S). Now RMB = the game's free aim only, and the LOCK is done here, on the free-aim
 * camera: at RMB the NPC nearest the crosshair (GF_ASSIST_CONE deg, GF_ASSIST_RANGE m) is snapped onto and FOLLOWED;
 * once the mouse has moved GF_ASSIST_BREAK deg (default 1.5) the lock lets go -- already in free aim, no mode change.
 * NPC list: characters are entities whose +0xB8 model resource has the same registry type as the player's mesh
 * (model registry *(0x101F3670): node +0x4 -> type at +0x8, +0x8 resource, +0x2C next; v1045 trainer findings). A
 * background scan (every 600 ms while in game) finds every word in guest RAM equal to such a resource; object =
 * word - 0xB8, kept if its transform (+0x68 -> +0x40) is within range and it is not the player. Dead: health float at
 * +0x14C8 (the player's field) <= 0 is skipped when it looks like a float. Log [aim] v1044T. */
enum { NPC_MAX = 96 };
static volatile uint32_t s_npc[NPC_MAX]; static volatile int s_nnpc = 0;
static int u32cmp_c(const void* a, const void* b) { const uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b; return x < y ? -1 : x > y; }
static uint32_t node_type_c(uint32_t node) { if (!ok(node)) return 0; const uint32_t r = rd32(node + 4); return ok(r) ? rd32(r + 8) : 0; }
static void npc_scan(void)
{
    const uint32_t pl = gad_player(); float pp[3];
    if (!pl || !gad_pos(pl, pp)) { s_nnpc = 0; return; }
    const uint32_t tab = rd32(0x101F3670u); if (!ok(tab)) return;
    const uint32_t bk = rd32(tab), nb = rd32(tab + 4);
    if (!ok(bk) || nb == 0 || nb > (1u << 20)) return;
    const uint32_t myres = rd32(pl + 0xB8);
    uint32_t mtype = 0; long budget = 400000;
    for (uint32_t b = 0; b < nb && !mtype; b++)
        for (uint32_t n = rd32(bk + 4 * b); n && ok(n) && budget-- > 0; n = rd32(n + 0x2C)) if (rd32(n + 8) == myres) { mtype = node_type_c(n); break; }
    if (!mtype) return;
    static uint32_t res[1024]; int nr = 0; budget = 400000;
    for (uint32_t b = 0; b < nb; b++)
        for (uint32_t n = rd32(bk + 4 * b); n && ok(n) && budget-- > 0 && nr < 1024; n = rd32(n + 0x2C))
            if (node_type_c(n) == mtype && rd32(n + 8)) res[nr++] = rd32(n + 8);
    if (!nr) return;
    qsort(res, (size_t)nr, 4, u32cmp_c);
    const uint32_t lo = res[0], hi = res[nr - 1];
    static float range = -1.0f; if (range < 0.0f) { const char* e = getenv("GF_ASSIST_RANGE"); range = (e && *e) ? (float)atof(e) : 30.0f; }
    uint32_t found[NPC_MAX]; int nf = 0;
    for (uint64_t ea = 0x20000000ull; ea < 0xF0000000ull && nf < NPC_MAX; ) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vm_base + ea, &mi, sizeof mi)) break;
        uint64_t rs = (uint64_t)((uint8_t*)mi.BaseAddress - vm_base), re = rs + mi.RegionSize;
        if (re <= ea) break;
        if (rs < ea) rs = ea;
        if (re > 0xF0000000ull) re = 0xF0000000ull;
        const DWORD pr = mi.Protect & 0xFF;
        if (mi.State == MEM_COMMIT && !(mi.Protect & PAGE_GUARD) && (pr == PAGE_READWRITE || pr == PAGE_EXECUTE_READWRITE)) {
            __try {
                const uint8_t* bp = vm_base + rs; const size_t len = (size_t)(re - rs);
                for (size_t i = 0xB8; i + 4 <= len && nf < NPC_MAX; i += 4) {
                    const uint32_t w = ((uint32_t)bp[i] << 24) | ((uint32_t)bp[i + 1] << 16) | ((uint32_t)bp[i + 2] << 8) | bp[i + 3];
                    if (w < lo || w > hi || !bsearch(&w, res, (size_t)nr, 4, u32cmp_c)) continue;
                    const uint32_t o = (uint32_t)(rs + i - 0xB8); float q[3];
                    if (o == pl || !gad_pos(o, q)) continue;
                    const float dx = q[0] - pp[0], dy = q[1] - pp[1], dz = q[2] - pp[2];
                    if (!(dx * dx + dy * dy + dz * dz <= range * range) || dx * dx + dz * dz < 0.25f) continue;
                    int dup = 0; for (int k = 0; k < nf; k++) if (found[k] == o) dup = 1;
                    if (!dup) found[nf++] = o;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
        ea = re;
    }
    for (int k = 0; k < nf; k++) s_npc[k] = found[k];
    s_nnpc = nf;
    { static int n = 0; if (n < 6) { n++; printf("[aim] v1044T NPC scan: %d character mesh(es), %d character(s) within %.0f m\n", nr, nf, range); fflush(stdout); } }
}
static int npc_alive(uint32_t o)
{
    const float h = rdf(o + 0x14C8);
    if (!fok(h) || h > 100000.0f || h < -100000.0f) return 1;   /* not a health float here: do not filter */
    return h > 0.0f;
}
static volatile LONG s_gcam_running = 0, s_bgscan_started = 0;   /* v1107S */
static DWORD WINAPI gcam_thread(LPVOID arg)
{
    (void)arg;
    static int dump = -1; if (dump < 0) { const char* e = getenv("GF_AIMDUMP"); dump = (e && *e == '1') ? 1 : 0; }
    ULONGLONG last = 0;
    { const char* e = getenv("GF_ASSIST"); if (!(e && *e == '0')) InterlockedExchange(&s_gcam_running, 1); }   /* v1107S: this thread keeps the NPC list from now on */
    for (;;) {
        Sleep(30);
        if (s_scan_req) { gcam_scan(); s_scan_req = 0; }
        { static ULONGLONG nl = 0; const ULONGLONG t = GetTickCount64();   /* v1044T NPC list */
          static int asst = -1; if (asst < 0) { const char* e = getenv("GF_ASSIST"); asst = !(e && *e == '0'); }   /* v1053F2: only needed while the assist is on (default) */
          if (asst && t - nl >= 600) { nl = t;
#ifdef _WIN32
              __try { npc_scan(); } __except (EXCEPTION_EXECUTE_HANDLER) { }
#else
              npc_scan();
#endif
          } }
        if (dump && s_ncam && GetTickCount64() - last >= 1000) { last = GetTickCount64();
#ifdef _WIN32
            __try { gad_dump(); } __except (EXCEPTION_EXECUTE_HANDLER) { }
#else
            gad_dump();
#endif
        }
    }
}
/* v1044J aim assist (Rockstar style). Lock-on target = *(player+0x1544) - 0x38 when nonzero (lock-on camera 0x6ACCC);
 * entity position = *(entity+0x68)+0x40 (feet). Free-aim camera: position obj+0xE0, pivot obj+0xEC, view = pivot - pos;
 * heading atan2(x,z) and pitch atan2(y, |xz|) change 1:1 (same sign) with the yaw obj+0x110 / pitch obj+0x10C fields
 * (aimdump 01:23). Snap: for GF_ASSIST_SNAP_MS (220) after the camera is up, pull the view onto the target's chest
 * (GF_ASSIST_HEIGHT 1.25 m) with time constant 60 ms; then within GF_ASSIST_STICK_DEG (4) the mouse is scaled by
 * GF_ASSIST_SLOW (0.55) and the view follows the target gently; beyond GF_ASSIST_BREAK_DEG (15) the target is dropped. */
static volatile uint32_t s_tgt = 0; static volatile int s_tgt_new = 0;
static volatile ULONGLONG s_fa_t0 = 0;               /* v1044L: free aim entered (camera search window) */
static volatile int s_pick = 0;                       /* v1044T: choose a lock target once the free-aim camera is up */
static float s_pend_yaw = 0.0f, s_pend_pit = 0.0f;   /* v1044L: mouse movement while the new camera is being found */
static uint32_t asst_target_now(void)
{
    const uint32_t pl = gad_player(); if (!pl) return 0;
    const uint32_t q = rd32(pl + 0x1544);
    if (!ok(q) || q < 0x38u) return 0;
    const uint32_t t = q - 0x38u; float p[3], pp[3];
    if (t == pl || !gad_pos(t, p) || !gad_pos(pl, pp)) return 0;
    /* v1044R: player+0x1544 keeps the LAST lock target after a lock ends; only count it as a target when it is near
     * (GF_LOCK_RANGE m, default 25) -- the game's own lock-on range is about that. */
    static float range = -1.0f; if (range < 0.0f) { const char* e = getenv("GF_LOCK_RANGE"); range = (e && *e) ? (float)atof(e) : 25.0f; }
    const float dx = p[0] - pp[0], dy = p[1] - pp[1], dz = p[2] - pp[2];
    return dx * dx + dy * dy + dz * dz <= range * range ? t : 0;
}
static int asst_acq_impl(int phase)   /* v1044K: 1 = lock-on starts, 0 = locked (poll), 2 = switching to free aim, 3 = free-aim camera up? */
{
    if (phase == 3) return s_live && gcam_valid(s_live);
    if (phase == 1) { s_tgt = 0; s_tgt_new = 0; return 0; }   /* v1044R: the v1044Q zeroing of player+0x1544 froze the game -- gone */
    if (phase == 2) {
        { int w = 0;                          /* v1044P: everything known now belongs to earlier entries (kept across entries) */
          for (int k = 0; k < s_ndead; k++) if (rd32(s_dead[k]) == GCAM_VT) { s_dead[w] = s_dead[k]; s_dead_y[w] = s_dead_y[k]; s_dead_p[w] = s_dead_p[k]; w++; }
          s_ndead = w;
          for (int i = 0; i < s_ncam; i++) dead_note(s_cam[i]);
          if (s_live) dead_note(s_live);
          s_live = 0; }
        s_ncam = 0; s_scan_req = 1;           /* the game makes a NEW free-aim camera object on every entry */
        s_fa_t0 = GetTickCount64(); s_pend_yaw = s_pend_pit = 0.0f; s_pick = 1;
        if (s_tgt) { s_tgt_new = 1; float p[3]; if (gad_pos(s_tgt, p)) {
            printf("[aim] v1044K free aim starts on target 0x%08X at %.2f %.2f %.2f\n", s_tgt, p[0], p[1], p[2]); fflush(stdout); } }
        return 0;
    }
    const uint32_t t = asst_target_now();
    if (t) s_tgt = t;
    return t != 0;
}
static int gf_aim_acq(int start)
{
#ifdef _WIN32
    __try { return asst_acq_impl(start); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
#else
    return asst_acq_impl(start);
#endif
}
static float asst_wrap(float a) { while (a > 180.0f) a -= 360.0f; while (a < -180.0f) a += 360.0f; return a; }
/* angles (deg) from the camera view to the target's chest; 0 = no valid geometry */
static int asst_delta(uint32_t cam, uint32_t tgt, float* dyaw, float* dpit)
{
    static float hgt = -1.0f; if (hgt < 0.0f) { const char* e = getenv("GF_ASSIST_HEIGHT"); hgt = (e && *e) ? (float)atof(e) : 1.25f; }
    float tp[3]; if (!gad_pos(tgt, tp)) return 0;
    const float cx = rdf(cam + 0xE0), cy = rdf(cam + 0xE4), cz = rdf(cam + 0xE8);
    const float lx = rdf(cam + 0xEC), ly = rdf(cam + 0xF0), lz = rdf(cam + 0xF4);
    if (!fok(cx) || !fok(cy) || !fok(cz) || !fok(lx) || !fok(ly) || !fok(lz)) return 0;
    const float fx = lx - cx, fy = ly - cy, fz = lz - cz;
    const float tx = tp[0] - cx, ty = tp[1] + hgt - cy, tz = tp[2] - cz;
    const float fh = sqrtf(fx * fx + fz * fz), th = sqrtf(tx * tx + tz * tz);
    if (fh < 1e-4f || th < 0.5f || th > 120.0f) return 0;
    const float R2D = 57.29578f;
    *dyaw = asst_wrap(atan2f(tx, tz) * R2D - atan2f(fx, fz) * R2D);
    *dpit = atan2f(ty, th) * R2D - atan2f(fy, fh) * R2D;
    return 1;
}
static volatile ULONGLONG s_aim_t = 0;   /* v1103A: last free-aim mouse update (= the player is aiming) */
static int aim_impl(float dyaw, float dpitch)
{
    s_aim_t = GetTickCount64();
    /* v1044J assist: adjust the mouse deltas before they are applied (v1044K: snap only by default, stick 0) */
    { static int on = -1; static float snap_ms = 250, stick = 0, slow = 0.55f, brk = 15;
      static ULONGLONG t_up = 0; static LARGE_INTEGER qf, qlast;
      if (on < 0) { const char* e = getenv("GF_ASSIST"); on = !(e && *e == '0');   /* v1053F2: our assist stays ON (user: only the game's T lock-on goes) */
          if ((e = getenv("GF_ASSIST_SNAP_MS")) && *e) snap_ms = (float)atof(e);
          if ((e = getenv("GF_ASSIST_STICK_DEG")) && *e) stick = (float)atof(e);
          if ((e = getenv("GF_ASSIST_SLOW")) && *e) slow = (float)atof(e);
          if ((e = getenv("GF_ASSIST_BREAK_DEG")) && *e) brk = (float)atof(e);
          QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qlast); }
      LARGE_INTEGER qn; QueryPerformanceCounter(&qn);
      float dt = (float)((double)(qn.QuadPart - qlast.QuadPart) / (double)qf.QuadPart); qlast = qn;
      if (dt > 0.1f) dt = 0.1f; if (dt < 0.0f) dt = 0.0f;
      const uint32_t cam = (s_live && gcam_valid(s_live)) ? s_live : 0;
      static float cone = -1.0f, brk_mouse = 1.5f, s_acc = 0.0f;
      if (cone < 0.0f) { const char* e = getenv("GF_ASSIST_CONE"); cone = (e && *e) ? (float)atof(e) : 20.0f;
                         if ((e = getenv("GF_ASSIST_BREAK")) && *e) brk_mouse = (float)atof(e); }
      /* v1053G -> v1053H below (user: free aim is the PRIMARY shooting method; "it doesn't snap to anyone when I'm aiming on them"):
       * modern aim assist. v1044T only picked a target at the RMB press (20 deg cone) and dropped it after 1.5 deg of
       * mouse. Now:
       *   entry snap  -- RMB: the NPC nearest the crosshair within GF_ASSIST_CONE (20) is snapped onto (as before);
       *                  mouse input past GF_ASSIST_BREAK (1.5 deg) or GF_ASSIST_SNAP_MS ends the snap -> sticky.
       *   acquisition -- while aiming, an NPC within GF_ASSIST_ACQ (5 deg) of the crosshair becomes the target.
       *   sticky      -- on a target the mouse is scaled by GF_ASSIST_FRICTION (0.6) and the view is pulled onto the
       *                  target's chest with time constant GF_ASSIST_PULL_MS (120) -> it also follows a moving target.
       *   release     -- the crosshair pulled more than GF_ASSIST_KEEP (10 deg) off the target, or the target dies.
       * Log [aim] v1053G. */
      /* v1053H (user: "can't do headshots, legshots -- it keeps locking to the torso"; "2 close to each other, I can't
       * target the other one"): the sticky phase no longer pulls onto the chest. It FOLLOWS the target -- the target's
       * own angular motion (error change not caused by the camera turn we applied) is added -- and scales the mouse by
       * GF_ASSIST_FRICTION (0.75), so the crosshair stays wherever the player put it on the body. The target zone is an
       * ellipse around the chest: GF_ASSIST_ACQ (4 deg) sideways, GF_ASSIST_ACQ_V (8 deg) up/down (head..feet); release
       * past GF_ASSIST_KEEP (1.5 x the zone). Every 25 ms another NPC clearly closer to the crosshair (zone-normalised
       * distance 0.25 lower) takes over. The RMB entry snap still goes to the chest. */
      static float acq = -1.0f, acqv = 8.0f, keep = 1.5f, fric = 0.75f; static int snapping = 0;
      static float prev_ey = 0.0f, prev_ep = 0.0f, app_y = 0.0f, app_p = 0.0f;
      if (acq < 0.0f) { const char* e = getenv("GF_ASSIST_ACQ"); acq = (e && *e) ? (float)atof(e) : 4.0f;
          if ((e = getenv("GF_ASSIST_ACQ_V")) && *e) acqv = (float)atof(e);
          if ((e = getenv("GF_ASSIST_KEEP")) && *e) keep = (float)atof(e);
          if ((e = getenv("GF_ASSIST_FRICTION")) && *e) fric = (float)atof(e);
          printf("[aim] v1053H aim assist: entry snap %.0f deg, zone %.1f x %.1f deg, keep x%.2f, friction %.2f, follow (no pull)\n",
                 cone, acq, acqv, keep, fric); fflush(stdout); }
      const ULONGLONG now = GetTickCount64();
      if (on && cam && s_pick) {
          if (now - s_fa_t0 > 900) s_pick = 0;
          else {
              s_pick = 0; uint32_t best = 0; float bestc = 1e9f;
              for (int k = 0; k < s_nnpc; k++) {
                  const uint32_t o = s_npc[k]; float ey, ep;
                  if (!npc_alive(o) || !asst_delta(cam, o, &ey, &ep)) continue;
                  const float c = sqrtf(ey * ey + ep * ep);
                  if (c < cone && c < bestc) { bestc = c; best = o; }
              }
              if (best) { s_tgt = best; s_tgt_new = 1; snapping = 1; printf("[aim] v1053H snap onto 0x%08X (%.1f deg off, %d candidates)\n", best, bestc, s_nnpc); fflush(stdout); }
              else { printf("[aim] v1053H nobody within %.0f deg (%d candidates) -> plain free aim\n", cone, s_nnpc); fflush(stdout); }
          }
      }
      if (on && cam && !s_pick && !snapping) {   /* acquisition / switching while aiming (zone-normalised distance) */
          static ULONGLONG lastq = 0;
          if (now - lastq >= 25) { lastq = now;
              uint32_t best = 0; float bestd = 1e9f, curd = 1e9f;
              for (int k = 0; k < s_nnpc; k++) {
                  const uint32_t o = s_npc[k]; float ey, ep;
                  if (!npc_alive(o) || !asst_delta(cam, o, &ey, &ep)) continue;
                  const float d = sqrtf((ey / acq) * (ey / acq) + (ep / acqv) * (ep / acqv));
                  if (o == s_tgt) curd = d;
                  if (d < 1.0f && d < bestd) { bestd = d; best = o; }
              }
              if (best && best != s_tgt && (!s_tgt || bestd + 0.25f < curd)) {
                  const int sw = s_tgt != 0;
                  s_tgt = best; s_tgt_new = 1;
                  static int n = 0; if (n < 200) { n++; printf("[aim] v1053H %s 0x%08X (zone distance %.2f)\n", sw ? "switched to" : "target", best, bestd); fflush(stdout); }
              }
          }
      }
      if (on && cam && s_tgt) {
          float ey, ep;
          if (!npc_alive(s_tgt) || !asst_delta(cam, s_tgt, &ey, &ep)) { s_tgt = 0; snapping = 0; }
          else {
              if (s_tgt_new) { s_tgt_new = 0; t_up = now; s_acc = 0.0f; prev_ey = ey; prev_ep = ep; app_y = app_p = 0.0f; }
              if (snapping) {
                  s_acc += fabsf(dyaw) + fabsf(dpitch);
                  if (s_acc > brk_mouse || (float)(now - t_up) >= snap_ms) { snapping = 0; prev_ey = ey; prev_ep = ep; app_y = app_p = 0.0f; }
                  else { const float k = 1.0f - expf(-dt / 0.035f); dyaw = ey * k; dpitch = ep * k; }
              }
              if (!snapping) {
                  const float d = sqrtf((ey / acq) * (ey / acq) + (ep / acqv) * (ep / acqv));
                  if (d > keep) {
                      s_tgt = 0;
                      static int n = 0; if (n < 200) { n++; printf("[aim] v1053H target released (zone distance %.2f)\n", d); fflush(stdout); }
                  } else {
                      /* the target's own motion since the last call = error now - (error then - what we turned) */
                      float my = ey - (prev_ey - app_y), mp = ep - (prev_ep - app_p);
                      if (my > 3.0f) my = 3.0f; if (my < -3.0f) my = -3.0f;
                      if (mp > 3.0f) mp = 3.0f; if (mp < -3.0f) mp = -3.0f;
                      dyaw = dyaw * fric + my * 0.9f; dpitch = dpitch * fric + mp * 0.9f;
                      prev_ey = ey; prev_ep = ep; app_y = dyaw; app_p = dpitch;
                  }
              }
          }
      }
    }

    static int on = -1; static float pmax = 70.0f; static ULONGLONG last = 0;
    if (on < 0) { const char* e = getenv("GF_AIM_DIRECT"); on = !(e && *e == '0');
                  const char* p = getenv("GF_AIM_PITCH_MAX"); if (p && *p) pmax = (float)atof(p);
                  if (on) { HANDLE h = CreateThread(NULL, 256u << 10, gcam_thread, NULL, 0, NULL); if (h) CloseHandle(h); } }
    if (!on) return 0;
    const ULONGLONG t = GetTickCount64();
    static int fb_ms = -1; if (fb_ms < 0) { const char* e = getenv("GF_AIM_FALLBACK_MS"); fb_ms = (e && *e) ? atoi(e) : 250; }
    const int live_ok = s_live && gcam_valid(s_live);
    if (s_live && !live_ok) { s_live = 0; s_ncam = 0; }          /* camera went away: find it again */
    /* v1044P: no live camera yet -> scan every ~30 ms (was 3 s once any old object had been seen) */
    if (!s_scan_req && t - last >= (live_ok ? 150ull : (t - s_fa_t0 < 3000 ? 30ull : 150ull))) { last = t; s_scan_req = 1; }
    if (!live_ok && s_win_lo && s_fa_t0 && t - s_fa_t0 > 1000) { s_win_lo = s_win_hi = 0; }   /* not near the last one: full scan */
    if (!live_ok && s_fa_t0 && t - s_fa_t0 < (ULONGLONG)fb_ms) {   /* hold the mouse back briefly, applied once found */
        s_pend_yaw += dyaw; s_pend_pit += dpitch; return 1;
    }
    if (s_pend_yaw != 0.0f || s_pend_pit != 0.0f) { dyaw += s_pend_yaw; dpitch += s_pend_pit; s_pend_yaw = s_pend_pit = 0.0f; }
    /* v1044R (user: free aim "comes out weird"): the game recycles a small pool of free-aim camera objects, so the "live"
     * guess flipped between them (log: 0x40B20DE0 / 0x40B20CC0 / 0x40B108E0) and the mouse sometimes drove one that is
     * not on screen. Every valid candidate near the player now gets the mouse (an unused camera is not drawn); s_live is
     * only used to aim the snap. Rescans run every 150 ms while aiming so a new camera is picked up at once. */
    uint32_t list[GCAM_MAX]; int nl = 0;
    if (live_ok) list[nl++] = s_live;
    for (int i = 0; i < s_ncam && nl < GCAM_MAX; i++) if (s_cam[i] != s_live) list[nl++] = s_cam[i];
    int applied = 0;
    for (int i = 0; i < nl; i++) {
        const uint32_t o = list[i];
        if (!gcam_valid(o)) continue;
        /* v1044G: yaw = obj+0x110, pitch = obj+0x10C (0x60CF0 x -> 0x59840 -> +0x108 -> +0x110, y -> +0x104 -> +0x10C) */
        float yaw = rdf(o + 0x110) + dyaw;
        while (yaw > 180.0f) yaw -= 360.0f;
        while (yaw < -180.0f) yaw += 360.0f;
        float pit = rdf(o + 0x10C) + dpitch;
        if (pit > pmax) pit = pmax; if (pit < -pmax) pit = -pmax;
        if (dyaw != 0.0f) wrf(o + 0x110, yaw);
        if (dpitch != 0.0f) wrf(o + 0x10C, pit);
        wrf(o + 0x104, 0.0f); wrf(o + 0x108, 0.0f);   /* no leftover stick velocity */
        if (!live_ok) for (int k = 0; k < s_ndead; k++) if (s_dead[k] == o) { s_dead_y[k] = rdf(o + 0x110); s_dead_p[k] = rdf(o + 0x10C); }
        applied = 1;
    }
    return applied;
}
static int gf_aim(float dyaw, float dpitch)
{
#ifdef _WIN32
    __try { return aim_impl(dyaw, dpitch); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
#else
    return aim_impl(dyaw, dpitch);
#endif
}
extern int (*volatile g_pad_aim_fn)(float, float);
static int read_paused(void)
{
    for (int i = 0; i < 4; i++) {
        const uint32_t ea = 0x40C77ED4u + 0x18u * (uint32_t)i;
        if (rd32(ea) != 1u || rd32(ea + 4) != 2u) return 0;
    }
    return 1;
}
static int gf_pad_menu(void)
{
    int p;
#ifdef _WIN32
    __try { p = read_paused(); } __except (EXCEPTION_EXECUTE_HANDLER) { p = 0; }
#else
    p = read_paused();
#endif
    static int last = -1;
    if (p != last) { last = p; printf("[combat] v1043Y pause menu %s\n", p ? "OPEN (WASD = d-pad)" : "closed"); fflush(stdout); }
    return p;
}

static void wa_learn_tick(void);   /* v1109A */
int gf_pad_weapon(void)
{
    wa_learn_tick();   /* v1109A */
    uint32_t flags = 0;
    const int item = read_item_safe(&flags);
    int gun, slot = -1;
    if (item == -2) gun = -1;
    else if (item < 0 || item >= 0x34) gun = 0;
    else {
        slot = k_slot[item];
        /* v1042X: the item is only in hand while player+0x5C4 & 0x200 is set (GetCurrentWeapon returns nil otherwise).
         * First test: a holstered gun (item 0x1B, flag clear) was still reported as "gun", so fists never fought. */
        gun = (flags & 0x200u) && slot <= 10 && ((s_gun_mask >> slot) & 1u) ? 1 : 0;
    }
    static int last_item = -99, last_gun = -99; static uint32_t last_fl = 0xFFFFFFFFu;
    const uint32_t fl = flags & 0x200u;
    if (item != last_item || gun != last_gun || fl != last_fl) {
        static int n = 0;
        if (n < 300) {
            n++;
            if (item == -2) printf("[combat] v1042X no player\n");
            else printf("[combat] v1042X in hand: %s item 0x%02X slot %d (flag 0x200 %s) -> %s\n",
                        item < 0 ? "nothing" : "", item < 0 ? 0 : item, slot, fl ? "set" : "clear",
                        gun > 0 ? "gun drawn: normal keys" : "melee: RMB lock-on + F punch, R charge, RMB+LMB grab, Q/E swing");
            fflush(stdout);
        }
        last_item = item; last_gun = gun; last_fl = fl;
    }
    return gun;
}

/* v1053M (user: melee lock-on, then pistol out + RMB -> "the old aiming thing instead of our free aim"): a gun is SELECTED
 * but holstered (flag 0x200 clear) -> gf_pad_weapon says melee and RMB sent L1, which draws the gun straight into the
 * game's lock-on aim. cellPad.c (g_pad_gun_holstered_fn) now draws it (d-pad UP) and free-aims instead. */
/* v1053N2: 1 = gun selected + holstered, 2 = and it was selected (weapon wheel / switch) within GF_GUN_INTENT_MS (1500)
 * -- only a fresh pick-up of the gun means "aim"; a long-holstered gun keeps RMB = melee lock-on (user: "the player has
 * to unselect their weapon each time? wack"). */
int gf_pad_gun_holstered(void)
{
    uint32_t flags = 0;
    const int item = read_item_safe(&flags);
    static int last_item = -99; static ULONGLONG t_sel = 0; static int win = -1;
    if (win < 0) { const char* e = getenv("GF_GUN_INTENT_MS"); win = (e && *e) ? atoi(e) : 1500; }
    const ULONGLONG now = GetTickCount64();
    if (item != last_item) { if (last_item != -99) t_sel = now; last_item = item; }
    if (item < 0 || item >= 0x34 || (flags & 0x200u)) return 0;
    const int slot = k_slot[item];
    if (!(slot <= 10 && ((s_gun_mask >> slot) & 1u))) return 0;
    return (t_sel && now - t_sel < (ULONGLONG)win) ? 2 : 1;
}
/* v1049H: a weapon (gun or melee) is in hand -- the same test as the game's GetCurrentWeapon (flag 0x200 + item) */
static int gf_pad_drawn(void)
{
    uint32_t flags = 0;
    const int item = read_item_safe(&flags);
    return (item >= 0 && item < 0x34 && (flags & 0x200u)) ? 1 : 0;
}
/* ================= v1109A weapon wheel AMMO (user: "make the weapon wheel show ammo -- you can't equip a gun without ammo,
 * so it would be extremely useful to tell which ones are usable"). rsx_ui_overlay.c (Noir Ring) asks g_gfw_ammo_fn per wheel
 * position (0..7 ring, 0 = up clockwise; 8..11 d-pad U/R/D/L) which inventory item sits there and what it holds.
 * Data (VERIFIED, lifted code): inventory player+0x1A7C, 11 slots +0x10 + slot*0x14 (+4 = item + 0x38); func_000E6CE4 puts
 * ONE item per slot (guns 0..4 replace each other via func_000E622C; 5 melee; 6..10 the rest; fists = no item).
 * Clip item+0x1EC (max +0x1F0); reserve = func_000A1A98 = pool[idx]: *(*(item+0x1D4) + 4 + 4 * *(item+0x1F8)) (pool shared by
 * the ammo type); infinite = item+0x1DC & 0x20. Gun = item+0xD4 == 1.
 * Position -> slot: the game's wheel is Flash ("weaponwheel" UI movie) -- no table found. Default from the user's log
 * (ring 4 Fists, 5 Bat = slot 5, 6 Pistol 0x16 = slot 0, 7 Revolver): guns 0..4 clockwise from 6 (6,7,0,1,2), melee 5,
 * fists 4. Self-correcting: when the wheel closes, the item that ends up in hand tells which slot the selected position is
 * ("learned", saved to gf_wheel_slots.txt next to the exe's cwd); and if exactly ONE shown position and ONE occupied slot are
 * still unmatched they are paired. GF_WHEEL_AMMO=0 = no ammo on the wheel. Log [gfw-ammo]. */
extern int (*volatile g_gfw_ammo_fn)(int pos, unsigned occ, int* clip, int* res);
extern void (*volatile g_gfw_closed_fn)(int pos);
static int s_wmap[12] = { 2, 3, 4, -1, -2, 5, 0, 1, -1, -1, -1, -1 };   /* slot, -1 unknown, -2 fists */
static const int k_wmap0[12] = { 2, 3, 4, -1, -2, 5, 0, 1, -1, -1, -1, -1 };   /* v1112O: the defaults (fists never learnable) */
static volatile uint32_t s_wpend_h0;   /* v1112O: item in hand when the wheel closed */
static int s_wlearn[12];                                               /* 1 = learned / saved */
static volatile int s_wpend_pos = -1; static volatile ULONGLONG s_wpend_t;
static uint32_t wa_player(void)
{
    uint32_t a = rd32(0x10181290u); if (!ok(a)) return 0;
    a = rd32(a + 4); if (!ok(a)) return 0;
    a = rd32(a); if (!ok(a) || a < 0x17FCu) return 0;
    a -= 0x17FCu; return ok(a) ? a : 0;
}
static uint32_t wa_slot_item(uint32_t pl, int slot)
{
    const uint32_t p = rd32(pl + 0x1A7Cu + 0x10u + 0x14u * (uint32_t)slot + 4u);
    return (p > 0x38u && ok(p)) ? p - 0x38u : 0;
}
static void wa_save(void)
{
    FILE* f = fopen("gf_wheel_slots.txt", "w"); if (!f) return;
    fprintf(f, "# v1109A weapon wheel position (0..7 ring from the top clockwise, 8..11 d-pad U/R/D/L) -> inventory slot, learned in play\n");
    for (int p = 0; p < 12; p++) if (s_wlearn[p]) fprintf(f, "%d %d\n", p, s_wmap[p]);
    fclose(f);
}
static void wa_load(void)
{
    FILE* f = fopen("gf_wheel_slots.txt", "r"); char ln[128]; int n = 0; if (!f) return;
    while (fgets(ln, sizeof ln, f)) { int p, sl; if (ln[0] != '#' && sscanf(ln, "%d %d", &p, &sl) == 2 && p >= 0 && p < 12 && sl >= -2 && sl <= 10 && k_wmap0[p] != -2) {   /* v1112O: never on the fists position */ s_wmap[p] = sl; s_wlearn[p] = 1; n++; } }
    fclose(f); printf("[gfw-ammo] v1109A %d learned wheel positions loaded (gf_wheel_slots.txt)\n", n); fflush(stdout);
}
static void wa_set(int pos, int slot, const char* why)
{
    if (s_wmap[pos] == slot && s_wlearn[pos]) return;
    if (k_wmap0[pos] == -2) return;   /* v1112O (user: the wheel showed ammo on the FISTS): the fists position has no slot */
    for (int p = 0; p < 12; p++) if (p != pos && s_wmap[p] == slot && slot >= 0) { s_wmap[p] = -1; s_wlearn[p] = 0; }
    printf("[gfw-ammo] v1109A wheel position %d -> slot %d (%s; was %d)\n", pos, slot, why, s_wmap[pos]); fflush(stdout);
    s_wmap[pos] = slot; s_wlearn[pos] = 1; wa_save();
}
/* pad thread (gf_pad_weapon): ~0.5 s after the wheel closed, see what is in hand */
static void wa_learn_tick(void)
{
    const int pos = s_wpend_pos;
    if (pos < 0 || GetTickCount64() - s_wpend_t < 500) return;
    const int expired = GetTickCount64() - s_wpend_t > 2000;   /* v1112O: keep looking for up to 2 s */
    __try {
        const uint32_t pl = wa_player(); if (!pl) { s_wpend_pos = -1; return; }
        const uint32_t h = rd32(pl + 0x468);
        if (h == s_wpend_h0) { if (expired) s_wpend_pos = -1; return; }   /* v1112O: unchanged -- still putting the old gun away, or re-picked it */
        if (!h || !ok(h) || h < 0x38u) { if (expired) s_wpend_pos = -1; return; }   /* empty hand (between holster and draw, or fists): keep waiting */
        s_wpend_pos = -1;
        const uint32_t it = h - 0x38u;
        for (int sl = 0; sl < 11; sl++) if (wa_slot_item(pl, sl) == it) { wa_set(pos, sl, "equipped from the wheel"); return; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static void gfw_closed(int pos)
{
    if (pos < 0 || pos >= 12) return;
    uint32_t h0 = 0; __try { const uint32_t pl = wa_player(); if (pl) h0 = rd32(pl + 0x468); } __except (EXCEPTION_EXECUTE_HANDLER) { h0 = 0; }
    s_wpend_h0 = h0; s_wpend_t = GetTickCount64(); s_wpend_pos = pos;
    {   /* v1112P diagnostic (user: "the gun in hand shows 6/15, the holstered one 7/0, they switch"): every slot's raw ammo fields */
        static int nd; if (nd++ < 40) { __try { const uint32_t pl = wa_player(); if (!pl) return;
            printf("[gfw-ammo] v1112P wheel closed on %d | hand %08X\n", pos, h0);
            for (int sl = 0; sl < 11; sl++) { const uint32_t it = wa_slot_item(pl, sl); if (!it) continue;
                const uint32_t pool = rd32(it + 0x1D4), idx = rd32(it + 0x1F8);
                printf("[gfw-ammo]   slot %2d item %08X (+38 %08X%s) gun %u clip %d max %d pool %08X idx %u -> %d | inf %u | +1D8 %08X +1E0 %08X +1E4 %08X +1E8 %08X +1F4 %08X +1FC %08X +200 %08X\n",
                       sl, it, it + 0x38u, it + 0x38u == h0 ? " IN HAND" : "", vm_base[it + 0xD4], (int)rd32(it + 0x1EC), (int)rd32(it + 0x1F0), pool, idx,
                       (pool && ok(pool) && idx < 64u) ? (int)rd32(pool + 4u + 4u * idx) : -1, (rd32(it + 0x1DC) & 0x20u) ? 1u : 0u,
                       rd32(it + 0x1D8), rd32(it + 0x1E0), rd32(it + 0x1E4), rd32(it + 0x1E8), rd32(it + 0x1F4), rd32(it + 0x1FC), rd32(it + 0x200)); }
            fflush(stdout); } __except (EXCEPTION_EXECUTE_HANDLER) { } } }
}
/* 0 = nothing to show; 1 = gun (clip, reserve); 2 = counted item (clip = count); | 4 = infinite */
static int gfw_ammo_impl(int pos, unsigned occ, int* clip, int* res)
{
    const uint32_t pl = wa_player(); if (!pl) return 0;
    int slot = s_wmap[pos];
    if (slot == -1) {   /* pair the only unmatched shown position with the only unmatched occupied slot */
        int up = -1, nup = 0, us = -1, nus = 0;
        for (int p = 0; p < 12; p++) if (((occ >> p) & 1u) && s_wmap[p] == -1) { up = p; nup++; }
        for (int sl = 0; sl < 11; sl++) { if (!wa_slot_item(pl, sl)) continue; int cl = 0;
            for (int p = 0; p < 12; p++) if (((occ >> p) & 1u) && s_wmap[p] == sl) cl = 1;
            if (!cl) { us = sl; nus++; } }
        if (nup == 1 && nus == 1 && up == pos) { slot = us; wa_set(pos, us, "only unmatched pair"); }
    }
    if (slot < 0) return 0;
    const uint32_t it = wa_slot_item(pl, slot); if (!it) return 0;
    const int gun = vm_base[it + 0xD4] == 1;
    const int c = (int)rd32(it + 0x1EC); const uint32_t pool = rd32(it + 0x1D4), idx = rd32(it + 0x1F8);
    int r = 0, has_pool = 0;
    if (pool && ok(pool) && idx < 64u) { r = (int)rd32(pool + 4u + 4u * idx); has_pool = 1; }
    if (c < 0 || c > 100000 || r < 0 || r > 100000) return 0;
    const int inf = (rd32(it + 0x1DC) & 0x20u) ? 4 : 0;
    if (gun) { *clip = c; *res = r; return 1 | inf; }
    if (has_pool || c > 0) { *clip = c + r; *res = 0; return 2 | inf; }
    return 0;
}
static int gfw_ammo(int pos, unsigned occ, int* clip, int* res)
{
    if (pos < 0 || pos >= 12) return 0;
    __try { return gfw_ammo_impl(pos, occ, clip, res); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void wa_init(void)
{
    const char* e = getenv("GF_WHEEL_AMMO");
    if (e && *e == '0') { printf("[gfw-ammo] v1109A weapon wheel ammo OFF (GF_WHEEL_AMMO=0)\n"); fflush(stdout); return; }
    wa_load();
    g_gfw_ammo_fn = gfw_ammo; g_gfw_closed_fn = gfw_closed;
    printf("[gfw-ammo] v1109A weapon wheel ammo ON: clip | reserve under every gun, NO AMMO / INF (GF_WHEEL_AMMO)\n"); fflush(stdout);
}
void gf_combat_init(void)
{
    wa_init();   /* v1109A */
    const char* e = getenv("GF_GUN_SLOTS");
    if (e && *e) {
        unsigned m = 0;
        for (const char* p = e; *p; ) {
            char* end; const long v = strtol(p, &end, 10);
            if (end == p) { p++; continue; }
            if (v >= 0 && v <= 11) m |= 1u << v;
            p = end;
        }
        s_gun_mask = m;
    }
    const char* off = getenv("PAD_KBM_COMBAT");
    if (off && *off == '0') { printf("[combat] v1042X PC combat layout OFF (PAD_KBM_COMBAT=0)\n"); fflush(stdout); return; }
    g_pad_weapon_fn = gf_pad_weapon;
    { extern int (*volatile g_pad_gun_holstered_fn)(void); g_pad_gun_holstered_fn = gf_pad_gun_holstered; }   /* v1053M */
    { extern int (*volatile g_pad_incar_fn)(void); g_pad_incar_fn = gf_pad_incar; }   /* v1053V */
    { extern float (*volatile g_pad_car_speed_fn)(void); g_pad_car_speed_fn = gf_pad_car_speed; }   /* v1130T */
    g_pad_drawn_fn = gf_pad_drawn;   /* v1049H */
    { const char* pp = getenv("GF_PAUSE_PROBE"); if (!(pp && *pp == '0')) g_pad_menu_fn = gf_pad_menu; }
    g_pad_aim_fn = gf_aim;   /* v1044F */
    { extern int (*volatile g_pad_aim_acq_fn)(int); g_pad_aim_acq_fn = gf_aim_acq; }   /* v1044J */
    { const char* td = getenv("GF_TARGETDUMP");
      if (td && *td == '1') { HANDLE h = CreateThread(NULL, 256u << 10, gtd_thread, NULL, 0, NULL); if (h) CloseHandle(h); } }
    printf("[combat] v1042X PC combat layout ON: gun slots mask 0x%03X (GF_GUN_SLOTS)\n", s_gun_mask);
    fflush(stdout);
}

/* v1091C (gf_ragdoll.c corpse hits): the live free-aim camera's view ray (position obj+0xE0, pivot obj+0xEC); 0 = none */
/* v1103A free-aim target HUD (user: "with the ordinary aiming you see HP and the name of the person you're targeting -- add it
 * to the free aim"): the living NPC nearest the crosshair inside an ellipse of GF_AIMHUD_DEG (3) sideways x 2.2x that up/down
 * (head..feet) while free aiming (aim_impl ran in the last 150 ms, free-aim camera known); the assist's current target wins
 * while it stays within 1.5x the ellipse. 0 = none. Read-only: called from the PPU thread (gf_ragdoll.c aimhud_tick). */
/* v1105T nearest living NPC within maxd m of the player (voice tester); 0 = none */
/* v1107S: background NPC list for gf_nearest_npc when the aim thread (gcam_thread, started on the first aim) is not running yet */
static DWORD WINAPI npc_scan_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        if (s_gcam_running) return 0;   /* the aim thread scans from now on */
        __try { npc_scan(); } __except (EXCEPTION_EXECUTE_HANDLER) { }
        Sleep(600);
    }
}
static void gf_npc_scan_bg_start(void)
{
    if (s_gcam_running || InterlockedCompareExchange(&s_bgscan_started, 1, 0) != 0) return;
    HANDLE h = CreateThread(NULL, 256u << 10, npc_scan_thread, NULL, 0, NULL); if (h) { SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(h); }
}
uint32_t gf_nearest_npc(float maxd)
{
    const uint32_t pl = gad_player(); float pp[3], p[3]; if (!pl || !gad_pos(pl, pp)) return 0;
    gf_npc_scan_bg_start();   /* v1107S: the list is filled off the game thread (npc_scan reads ALL guest memory: on the game thread it stuttered every 0.8 s) */
    uint32_t best = 0; float bd = maxd * maxd;
    for (int k = 0; k < s_nnpc; k++) { const uint32_t o = s_npc[k];
        if (o == pl || !npc_alive(o) || !gad_pos(o, p)) continue;
        const float dx = p[0] - pp[0], dy = p[1] - pp[1], dz = p[2] - pp[2], d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bd) { bd = d2; best = o; } }
    return best;
}
/* v1110T gf_shop.c: every living NPC within maxd of the player (shopkeeper search) */
int gf_npcs_within(float maxd, uint32_t* out, int max)
{
    const uint32_t pl = gad_player(); float pp[3], p[3]; if (!pl || !gad_pos(pl, pp)) return 0;
    gf_npc_scan_bg_start();
    int n = 0; const float r2 = maxd * maxd;
    for (int k = 0; k < s_nnpc && n < max; k++) { const uint32_t o = s_npc[k];
        if (o == pl || !npc_alive(o) || !gad_pos(o, p)) continue;
        const float dx = p[0] - pp[0], dy = p[1] - pp[1], dz = p[2] - pp[2];
        if (dx * dx + dy * dy + dz * dz < r2) out[n++] = o; }
    return n;
}
uint32_t gf_aim_target_now(void)
{
    static float deg = -1.0f; if (deg < 0.0f) { const char* e = getenv("GF_AIMHUD_DEG"); deg = (e && atof(e) > 0.2) ? (float)atof(e) : 3.0f; }
    if (GetTickCount64() - s_aim_t > 150) return 0;
    const uint32_t cam = (s_live && gcam_valid(s_live)) ? s_live : 0; if (!cam) return 0;
    uint32_t best = 0; float bd = 1e9f, ey, ep;
    const uint32_t t = s_tgt;
    if (t && npc_alive(t) && asst_delta(cam, t, &ey, &ep)) { const float z = sqrtf((ey / deg) * (ey / deg) + (ep / (deg * 2.2f)) * (ep / (deg * 2.2f))); if (z < 1.5f) return t; }
    for (int k = 0; k < s_nnpc; k++) { const uint32_t o = s_npc[k];
        if (!npc_alive(o) || !asst_delta(cam, o, &ey, &ep)) continue;
        const float z = sqrtf((ey / deg) * (ey / deg) + (ep / (deg * 2.2f)) * (ep / (deg * 2.2f)));
        if (z < 1.0f && z < bd) { bd = z; best = o; } }
    return best;
}
int gf_aim_ray(float* o, float* d)
{
#ifdef _WIN32
    __try {
#endif
        const uint32_t c = s_live; if (!c || !gcam_valid(c)) return 0;
        float v[3]; for (int i = 0; i < 3; i++) { o[i] = rdf(c + 0xE0 + 4u * (uint32_t)i); v[i] = rdf(c + 0xEC + 4u * (uint32_t)i) - o[i]; }
        const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (!(l > 1e-4f) || !(l < 1e4f)) return 0;
        for (int i = 0; i < 3; i++) d[i] = v[i] / l;
        return 1;
#ifdef _WIN32
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
#endif
}
