/*
 * ps3recomp - cellPad HLE implementation
 *
 * Reads real gamepad input from the host and translates to PS3 pad format.
 *
 * Backend selection:
 *   - Windows default: XInput (no extra dependencies)
 *   - Everywhere else / if PS3RECOMP_PAD_USE_SDL2 is defined: SDL2 GameController
 *
 * Define PS3RECOMP_PAD_USE_SDL2 to force SDL2 backend on Windows.
 */

#include <time.h>
#include "cellPad.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include "../../runtime/platform/win32_compat.h"   /* GetTickCount64 on POSIX */
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: guest EA -> host */
#ifdef PS3RECOMP_SDL3
#include "pad_sdl3.h"           /* PlayStation / Steam controllers through SDL3 (pad_sdl3.c) */
#endif
/* v1041R benchmark world mark (set from ppu_fs.cpp on the first open of BENCH_WORLD_MARK, default "world_terrain.str"). */
volatile long long g_bench_world_ms = 0;
int pad_bench_world_mark(const char* gpath)
{
    const char* mk = getenv("BENCH_WORLD_MARK");
    if (!mk || !*mk) mk = "world_terrain.str";
    if (!gpath || !strstr(gpath, mk)) return 0;
    g_bench_world_ms = (long long)GetTickCount64();
    fprintf(stderr, "[BENCH] v1041R world mark '%s' at tick %lld ms\n", gpath, g_bench_world_ms);
    return 1;
}

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_PAD_USE_SDL2)
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#elif defined(_WIN32)
  #define PAD_BACKEND_SDL2  0
  #define PAD_BACKEND_XINPUT 1
#else
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#endif

#if PAD_BACKEND_XINPUT
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <xinput.h>
  #pragma comment(lib, "xinput.lib")
#endif

#if PAD_BACKEND_SDL2
  #include <SDL2/SDL.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

#define PAD_MAX_HOST_PORTS  4  /* XInput supports max 4; SDL may support more */

typedef struct {
    int  connected;
    u16  buttons;           /* CELL_PAD_CTRL_* bitmask */
    u8   analog_lx;         /* 0-255, center=128 */
    u8   analog_ly;
    u8   analog_rx;
    u8   analog_ry;
    u8   trigger_l2;        /* 0-255 */
    u8   trigger_r2;        /* 0-255 */
    /* Pressure-sensitive face buttons (0-255) */
    u8   press_right;
    u8   press_left;
    u8   press_up;
    u8   press_down;
    u8   press_triangle;
    u8   press_circle;
    u8   press_cross;
    u8   press_square;
    u8   press_l1;
    u8   press_r1;
} PadHostState;

static int           s_pad_initialized = 0;
static u32           s_max_connect = 0;
static u32           s_port_setting[CELL_PAD_MAX_PORT_NUM];
static PadHostState  s_host_state[PAD_MAX_HOST_PORTS];

/* Some console titles use CELL_PAD_CAPABILITY_SENSOR_MODE to enable gameplay
 * paths that cannot be represented by a conventional PC controller or KBM.
 * Keep the shared runtime's DS3-compatible default, but let a port explicitly
 * expose a motionless virtual pad with PAD_SENSOR=0.  All capability/probe and
 * setting entry points below use this one policy so the guest never receives
 * contradictory answers. */
static int pad_sensor_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char* value = getenv("PAD_SENSOR");
        enabled = (!value || !*value || atoi(value) != 0) ? 1 : 0;
        printf("[cellPad] motion sensor support %s (PAD_SENSOR=%s)\n",
               enabled ? "ON" : "OFF", value ? value : "default");
        fflush(stdout);
    }
    return enabled;
}

static u32 pad_device_capabilities(void)
{
    u32 capabilities = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                     | CELL_PAD_CAPABILITY_PRESS_MODE
                     | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                     | CELL_PAD_CAPABILITY_ACTUATOR;
    if (pad_sensor_enabled())
        capabilities |= CELL_PAD_CAPABILITY_SENSOR_MODE;
    return capabilities;
}

/* Some launchers present a virtual port 0 from the first host poll, so a title
 * never observes the real libpad DISCONNECTED -> CONNECTED assignment edge.
 * PAD_ASSIGN_CHANGES=1 supplies that edge once after cellPadInit.  It must not
 * remain asserted: ASSIGN_CHANGES is a transition notification, and repeatedly
 * reporting it can make a title continuously tear down and reassign player 1. */
static int s_assign_changes_enabled = -1;
static u8 s_assign_change_pending[CELL_PAD_MAX_PORT_NUM];

static void pad_assignment_reset(void)
{
    if (s_assign_changes_enabled < 0) {
        const char* value = getenv("PAD_ASSIGN_CHANGES");
        s_assign_changes_enabled = (value && *value && atoi(value) != 0) ? 1 : 0;
        if (s_assign_changes_enabled) {
            printf("[cellPad] one-shot port assignment edge enabled (PAD_ASSIGN_CHANGES=1)\n");
            fflush(stdout);
        }
    }
    memset(s_assign_change_pending,
           s_assign_changes_enabled ? 1 : 0,
           sizeof(s_assign_change_pending));
}

/* PAD_CONNECT_WAIT=<base>:<off>[:<off>...] -- "late connect" for the virtual pad on port 0.
 *
 * A real DUALSHOCK/SIXAXIS enumerates a moment AFTER cellPadInit, so titles see port 0 go
 * disconnected -> connected and many react only to that edge.  The Godfather is one: its pad
 * layer seeds a per-port "seen" flag from the GetInfo made inside its pad init, and calls its
 * controller manager's connect callback ONLY on a later 0 -> 1 transition.  The manager
 * registers that callback afterwards (same boot function, 0x19264), so with port 0 connected
 * from the first call the callback never fired, the manager's connected-port mask stayed 0,
 * and the game sat forever on "Please reconnect SIXAXIS ... press START" -- START could not
 * help because the mask, not the button, gates the dialog.
 *
 * The env value is a guest pointer chain: p = base; for each off: p = read32(p) + off; then
 * port 0 stays hidden while read32(p) == 0 (i.e. until the title has registered whatever it
 * needs).  Godfather: 0xB44CA0:0:0x188 = *(*(TOC-0x72C0)) + 0x188 = pad object's connect
 * callback.  A 15 s safety timeout always reveals the pad.  Unset = old behaviour. */
static int pad_port0_hidden(void)
{
    static int state = -1;            /* -1 unparsed, 0 visible for good, 1 waiting */
    static u32 chain[8]; static int n = 0;
    static ULONGLONG t0 = 0;
    if (state == 0) return 0;
    if (state < 0) {
        const char* e = getenv("PAD_CONNECT_WAIT");
        if (!e || !*e) { state = 0; return 0; }
        char buf[128]; strncpy(buf, e, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
        for (char* t = strtok(buf, ":,"); t && n < 8; t = strtok(NULL, ":,")) chain[n++] = (u32)strtoul(t, NULL, 0);
        if (n < 1) { state = 0; return 0; }
        t0 = GetTickCount64();
        state = 1;
        printf("[cellPad] port 0 held disconnected until the title's pointer chain %s is set (PAD_CONNECT_WAIT)\n", e);
        fflush(stdout);
    }
    u32 p = chain[0];
    for (int i = 1; i < n && p; i++) { u32 v = vm_read32(p); p = v ? v + chain[i] : 0; }
    u32 v = p ? vm_read32(p) : 0;
    if (v || GetTickCount64() - t0 > 15000) {
        state = 0;
        printf("[cellPad] port 0 now reported CONNECTED (%s) -> title sees the controller plug in\n",
               v ? "title ready" : "15 s timeout");
        fflush(stdout);
        return 0;
    }
    return 1;
}

static u32 pad_connected_status(u32 port_no)
{
    u32 status = CELL_PAD_STATUS_CONNECTED;
    if (port_no < CELL_PAD_MAX_PORT_NUM && s_assign_change_pending[port_no]) {
        status |= CELL_PAD_STATUS_ASSIGN_CHANGES;
        s_assign_change_pending[port_no] = 0;
    }
    return status;
}

/* Optional title-owned filter.  The generic runtime only exposes the completed
 * host-endian pad packet; a port can install a filter without teaching the
 * shared input backend about game-specific controls.  Called before pressure
 * words are derived so injected digital buttons receive matching pressure. */
void (*g_pad_filter)(u32 port, u16* buttons, s32 len) = NULL;
#ifdef PS3RECOMP_COOP_MOD
/* Co-op combat (infamous-recomp/coop_mod.cpp): the digital buttons port 0 last delivered to the title,
 * after every injector and filter, as (DIGITAL2 << 8) | DIGITAL1 (R1 = 0x0800, L1 = 0x0400).
 * Zero while a system dialog owns the pad.  Written by the polling thread, read by the co-op tick. */
volatile unsigned g_coop_pad_digital;
#endif
/* Per-port "a report has arrived since your last read" flag; see cellPadGetData. */
static int s_data_fresh[PAD_MAX_HOST_PORTS];

/* v1042S hold-to-skip for cutscenes. The title registers g_pad_cutscene_fn (Godfather: gf_cutscene.c, which tracks
 * its in-engine IGC entities). While a cutscene plays, holding Cross (Space / Enter on the keyboard) for
 * PAD_SKIP_HOLD_MS (default 1000) starts an off-screen drain that lasts until the cutscene ends: g_gf_ffwd tells
 * the title clock/limiter and frame clock to consume the remaining scripted events as quickly as possible, while
 * main.cpp suppresses intermediate presents and cellAudio suppresses their sound. This preserves mission-state
 * cleanup without showing the cutscene in fast-forward. PAD_SKIP=0 disables. Log: [skip]. */
int (*volatile g_pad_cutscene_fn)(void) = 0;
/* v1042X PC melee layout for the generic KBM profile (pad_poll_kbm_generic). The title registers g_pad_weapon_fn
 * (Godfather: gf_combat.c): 1 = a gun is in hand, 0 = fists / melee weapon, -1 = unknown (no player). NULL = off. */
int (*volatile g_pad_weapon_fn)(void) = 0;
int (*volatile g_pad_drawn_fn)(void) = 0;
volatile unsigned long long g_pad_fa_cam_ms = 0;   /* v1049A gf_footcam.c: last tick of the game's free-aim camera update (0x60AE0) */   /* v1049H gf_combat.c: 1 = a weapon (gun or melee) is out, 0 = holstered / fists */
volatile int   g_gf_ffwd = 0;              /* cutscene skip drain active; read by title, video, and audio */
volatile int   g_gf_combat_btn = 0;        /* v1087C: port 0 combat buttons held (DIGITAL2 & 0x8F) */
volatile int   g_gf_cross_held = 0;        /* v1085W: port 0 Cross held this poll (sprint) */
volatile int   g_gf_classic_aim = 0;       /* v1119L gf_coop_mission.c: 1 = Monk's shooting lesson (s1) -> classic PS3 aiming: RMB = L1 lock-on, mouse = right stick, MMB / Z = R3 */
static int     s_fa_busy_pub = 0;          /* v1119L: the free-aim state of the last poll (classic aim switches only outside free aim) */
volatile int   g_gf_force_walk = 0;        /* v1085W gf_smoke.c: != 0 = cap the left stick while smoking; 1 = PAD_KBM_WALK, 2..127 = that deflection (v1086W) */
volatile int   g_gf_skip_request = 0;      /* v1073H co-op: 1 = skip the running cutscene now (host skipped) */
volatile int   g_gf_skip_block = 0;        /* v1073H co-op: 1 = the player may not skip (host controls the cutscene) */
volatile int   g_gf_skip_speed = 40;       /* hidden-drain time warp factor (the title may lower it) */
volatile float g_skip_progress = 0.0f;     /* 0..1 while holding */
volatile int   g_skip_visible = 0;         /* 1 = draw the prompt, 2 = "Skipping..." */

/* v1042M SIXAXIS gestures for a PC port (PAD_MOTION, default 1; 0 = old motionless pad).
 * Godfather's motion moves are data-driven: USRDIR/gsmachine.gsm holds one sequence per move, each a
 * direction vector + magnitude threshold (raw sensor counts relative to the at-rest baseline, ~113 = 1 g)
 * + accept/reject cone angles. Decoded vectors (game X/Y/Z = raw SENSOR_X/Y/Z deltas):
 *   Push (0,9,47) mag 100   WallSlam (0,78,251) 40   ThrowForward (0,1,1) 120   Pistol Whip/Rifle Butt (0,1,1)
 *   HeadButt (0,-355,-98) 120   PullToStanding (0,-120,-19) 80   OverHeadSmash (0,-78,0) 120
 *   ThrowRight -X 40 then +X 40 (3 stages)   ThrowLeft +X then -X   EarlySwingLeft +X 55 / Right -X 55
 *   NeckSnap: any direction, mag 40, plus an orientation (gravity) term -> a wrist roll.
 * So a host action is turned into a short sensor pulse along the matching vector; the sample stays exactly at
 * rest otherwise, so the game's AccelerometerAtRest baseline is undisturbed. Gestures:
 *   forward (wall slam / push / throw forward / pistol whip), lift (head butt / pull up / overhead smash),
 *   down, back, left / right (swing-throws: biphasic), twist (neck snap).
 * Tuning: PAD_MOTION_PEAK (counts, default 180), PAD_MOTION_MS (pulse length, default 160),
 *         PAD_MOTION_FLIP=XYZ (negate axes if a direction comes out mirrored). Log: [motion]. */
static unsigned long long pad_now_ms(void);
volatile unsigned long long g_pad_autocross_at = 0;   /* v1042Z GF_NET_AUTODISMISS (set by cellNetCtl.c) */
volatile unsigned g_pad_inject_d1 = 0, g_pad_inject_d2 = 0; volatile int g_pad_inject_polls = 0;   /* v1112B the title presses pad buttons (port 0) for N polls (gf_shop.c tailor: START = Done) */
enum { MOT_NONE = 0, MOT_FORWARD, MOT_LIFT, MOT_DOWN, MOT_BACK, MOT_LEFT, MOT_RIGHT, MOT_TWIST,
       MOT_TILT, MOT_KNEE_L, MOT_KNEE_R, MOT_COUNT };
static const char* const s_mot_names[MOT_COUNT] = { "none", "forward", "lift", "down", "back", "left", "right", "twist",
                                                    "tilt-up", "down-left", "down-right" };
static volatile int s_mot_kind = MOT_NONE;
static volatile unsigned long long s_mot_t0 = 0;
static int s_mot_peak = 180, s_mot_ms = 160, s_mot_flip = 0, s_mot_inited = 0, s_mot_on = 1;
static void pad_motion_init(void)
{
    if (s_mot_inited) return;
    const char* e = getenv("PAD_MOTION"); s_mot_on = !(e && *e == '0');
    e = getenv("PAD_MOTION_PEAK"); if (e && *e) s_mot_peak = atoi(e);
    if (s_mot_peak < 40) s_mot_peak = 40; if (s_mot_peak > 400) s_mot_peak = 400;
    e = getenv("PAD_MOTION_MS"); if (e && *e) s_mot_ms = atoi(e);
    if (s_mot_ms < 60) s_mot_ms = 60; if (s_mot_ms > 600) s_mot_ms = 600;
    e = getenv("PAD_MOTION_FLIP");
    if (e) { if (strchr(e, 'X') || strchr(e, 'x')) s_mot_flip |= 1;
             if (strchr(e, 'Y') || strchr(e, 'y')) s_mot_flip |= 2;
             if (strchr(e, 'Z') || strchr(e, 'z')) s_mot_flip |= 4; }
    s_mot_inited = 1;
    printf("[motion] v1042M SIXAXIS gesture synthesis %s (peak %d, %d ms, flip 0x%X)\n",
           s_mot_on ? "ON" : "OFF (PAD_MOTION=0)", s_mot_peak, s_mot_ms, s_mot_flip);
    fflush(stdout);
}
static int pad_motion_duration(int k)
{
    if (k == MOT_LEFT || k == MOT_RIGHT) return s_mot_ms * 2;
    if (k == MOT_TWIST || k == MOT_TILT) return s_mot_ms * 3;
    return s_mot_ms;
}
static void pad_motion_trigger(int k)
{
    pad_motion_init();
    if (!s_mot_on || k <= MOT_NONE || k >= MOT_COUNT) return;
    const unsigned long long now = pad_now_ms();
    const int cur = s_mot_kind;
    /* one gesture at a time, plus a short settle at rest so the recogniser sees distinct moves */
    if (cur != MOT_NONE && now - s_mot_t0 < (unsigned long long)pad_motion_duration(cur) + 80) return;
    s_mot_t0 = now; s_mot_kind = k;
    static int n = 0;
    if (n < 300) { n++; printf("[motion] gesture %s\n", s_mot_names[k]); fflush(stdout); }
}
static void pad_motion_sample(u16* sx, u16* sy, u16* sz, u16* sg)
{
    pad_motion_init();
    float dx = 0.0f, dy = 0.0f, dz = 0.0f;
    const int k = s_mot_kind;
    if (k != MOT_NONE) {
        const float T = (float)pad_motion_duration(k);
        const float t = (float)(pad_now_ms() - s_mot_t0);
        if (t >= 0.0f && t < T) {
            const float u = t / T, P = (float)s_mot_peak, PI = 3.14159265f;
            const float s1 = sinf(PI * u), s2 = sinf(2.0f * PI * u);
            switch (k) {
            case MOT_FORWARD: dy = 0.300f * P * s1; dz = 0.954f * P * s1; break;
            case MOT_LIFT:    dy = -0.964f * P * s1; dz = -0.265f * P * s1; break;
            case MOT_DOWN:    dy = 0.920f * P * s1; dz = -0.390f * P * s1; break;   /* down and in (knee / lock-on down) */
            case MOT_KNEE_L:  dx = 0.500f * P * s1; dy = 0.650f * P * s1; dz = -0.570f * P * s1; break;
            case MOT_KNEE_R:  dx = -0.500f * P * s1; dy = 0.650f * P * s1; dz = -0.570f * P * s1; break;
            case MOT_BACK:    dz = -P * s1; break;
            case MOT_LEFT:    dx = P * s2; break;     /* +X push, then the -X stop */
            case MOT_RIGHT:   dx = -P * s2; break;    /* -X push, then the +X stop */
            case MOT_TWIST: {                          /* roll ~75 deg and back: gravity moves from Y into X */
                const float a = u < 0.3333f ? u * 3.0f : (u < 0.6667f ? 1.0f : (1.0f - u) * 3.0f);
                const float th = a * 1.309f;
                dx = 113.0f * sinf(th); dy = 113.0f * (1.0f - cosf(th));
                break; }
            case MOT_TILT: {                           /* pitch the front up ~60 deg and back: gravity moves from Y into Z */
                const float a = u < 0.3333f ? u * 3.0f : (u < 0.6667f ? 1.0f : (1.0f - u) * 3.0f);
                const float th = a * 1.047f;
                dy = 113.0f * (1.0f - cosf(th)); dz = 113.0f * sinf(th);
                break; }
            default: break;
            }
        }
    }
    if (s_mot_flip & 1) dx = -dx;
    if (s_mot_flip & 2) dy = -dy;
    if (s_mot_flip & 4) dz = -dz;
    int x = 512 + (int)lrintf(dx), y = 399 + (int)lrintf(dy), z = 512 + (int)lrintf(dz);
    if (x < 0) x = 0; if (x > 1023) x = 1023;
    if (y < 0) y = 0; if (y > 1023) y = 1023;
    if (z < 0) z = 0; if (z > 1023) z = 1023;
    *sx = (u16)x; *sy = (u16)y; *sz = (u16)z; *sg = 512;
}
/* Press latch (2026-10-04): every button seen down by ANY host poll since the
 * guest's last delivered packet. A title that reads the pad once per frame at a
 * low frame rate (inFamous: ~4 reads/s at 4 fps) otherwise never sees a normal
 * key tap that starts and ends between two reads -- "Press Start" ignored. */
static volatile long s_latch[PAD_MAX_HOST_PORTS];
/* Godfather reconnect compatibility: number of upcoming port-0 reports that
 * should carry START after PAD_START_ON_ANY_INPUT fires.  A short held press
 * survives the title polling connection status and button data in either
 * order, unlike a single latch that can be consumed before rebinding. */
static volatile long s_any_input_start_reports = 0;
#ifdef _WIN32
  #define PAD_LATCH_OR(p, v)   InterlockedOr(&s_latch[p], (long)(v))
  #define PAD_LATCH_TAKE(p)    ((u16)InterlockedExchange(&s_latch[p], 0))
#else
  #define PAD_LATCH_OR(p, v)   __sync_fetch_and_or(&s_latch[p], (long)(v))
  #define PAD_LATCH_TAKE(p)    ((u16)__sync_lock_test_and_set(&s_latch[p], 0))
#endif

#if PAD_BACKEND_SDL2
static SDL_GameController* s_sdl_controllers[PAD_MAX_HOST_PORTS];
static int s_sdl_inited = 0;
#endif

/* ---------------------------------------------------------------------------
 * XInput backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_XINPUT

/* Deadzone for analog sticks (same as XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) */
#define PAD_STICK_DEADZONE  7849
#define PAD_TRIGGER_THRESHOLD 30

static u8 pad_xinput_stick_to_u8(short raw, short deadzone)
{
    float normalized;
    if (raw > deadzone)
        normalized = (float)(raw - deadzone) / (float)(32767 - deadzone);
    else if (raw < -deadzone)
        normalized = (float)(raw + deadzone) / (float)(32767 - deadzone);
    else
        normalized = 0.0f;

    /* Map -1.0..1.0 to 0..255 with center at 128 */
    int val = (int)(normalized * 127.0f) + 128;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

/* Keyboard fallback for port 0.
 *
 * The only backend here is XInput, so on a machine with no controller plugged
 * in a title gets a pad that is reported present and never presses anything --
 * it can be watched but not played. That is what The Simpsons Arcade Game hit:
 * it reached its attract loop and no input existed to start a game.
 *
 * Only fills in for port 0, only when XInput found nothing there, so a real
 * controller always wins and nothing changes for a port that has one. Keys are
 * read only while a window of THIS process is in the foreground, so typing in
 * another application does not drive the game. Set PAD_NO_KEYBOARD=1 to
 * disable it entirely.
 *
 * Arrows = d-pad, Z/X/A/S = cross/circle/square/triangle, Q/W = L1/R1,
 * 1/2 = L2/R2, Enter = START, Tab = SELECT. The left stick mirrors the d-pad
 * so a title that reads the stick instead is playable too. */
/* v219 PAD_FILE stick hold (bench driver): lx, ly, rx, ry */
static unsigned char g_pf_stick[4] = {128, 128, 128, 128};
volatile unsigned g_pad_file_held = 0;   /* v219: PAD_FILE mask being held (rsx_ui_overlay.c dialogs/menu) */
volatile unsigned g_pad_phys_buttons = 0;   /* v1093M: port 0 buttons as the devices gave them, BEFORE the overlay gate (rsx_ui_overlay.c pages: any controller) */
volatile unsigned long long g_pad_phys_ms = 0;
static int g_pf_stick_on = 0;

/* ---- v1129RR PAD_RECORD=<file> / PAD_REPLAY=<file>: record every raw input, replay it exactly -----------------------
 * (user: "make a launcher that copies every single thing I do, and use that as the benchmarker").
 * RECORD: once per 4 ms input tick the host poll thread stores the raw inputs the rest of this file and the D3D12 window
 * see: keyboard (256 keys, only while the game window has focus), Caps Lock, cursor position + client rect/origin,
 * focus, the raw mouse counts the window received (pad_rr_mouse_in), and the controller states XInput/SDL3 produced
 * (before the keyboard merge). A frame is written when anything changed. Times are QPC ms from the first input tick
 * (segment 0: boot + menus) and from the benchmark world mark (segment 1: in the world, see pad_bench_world_mark).
 * REPLAY: the same tick feeds the recorded state back through every reader (GetAsyncKeyState / GetKeyState /
 * GetCursorPos / GetForegroundWindow / ScreenToClient / ClientToScreen / GetClientRect wrappers below, the raw mouse
 * accumulators incl. gf_footcam's look counts, the controller states). The real keyboard, mouse and controllers are
 * ignored; SetCursorPos / ClipCursor do nothing. Load-time drift: a frame that changes a key / click / controller
 * button waits until the same menu is on screen (sig = name grid or menu item count, as at record time) up to
 * PAD_REPLAY_GATE_MS (default 15000; PAD_REPLAY_GATE=0 = off). Segment 1 restarts its clock at the world mark.
 * Both modes switch off PAD_AUTOPRESS, PAD_SCRIPT and BENCH_MOVE. Drift report: every 2 s in the world the player
 * position (title hook g_pad_player_pos_fn) is stored; the replay logs its distance to the recorded one ([padrr]). */
#ifdef _WIN32
static int pad_envi(const char* n, int d);
extern volatile long g_pad_raw_mdx, g_pad_raw_mdy, g_pad_raw_wheel, g_pad_raw_look_dx, g_pad_raw_look_dy;
extern volatile int g_pad_raw_ok;
extern volatile int g_gf_grid_valid, g_gf_mp_n;
int (*volatile g_pad_player_pos_fn)(float* p) = 0;   /* title hook (Godfather: gf_shop.c) */
typedef struct {
    unsigned ofs;            /* ms into the segment */
    unsigned char seg, flags; short sig;   /* flags: 1 focused, 2 Caps Lock on, 4 controller block follows, 8 raw mouse ok */
    unsigned char keys[32];
    int mdx, mdy, wheel;     /* raw mouse counts received during this tick */
    int cx, cy, ox, oy, rw, rh;   /* cursor (screen), client origin (screen), client size */
} RRFrame;
typedef struct { unsigned ofs; unsigned char seg, pad_[3]; float p[3]; } RRPos;
static int s_rr_mode = 0;    /* 0 off, 1 record, 2 replay */
static FILE* s_rr_f = 0;
static RRFrame s_rr_cur;     /* replay: state handed to the wrappers */
static PadHostState s_rr_gp[PAD_MAX_HOST_PORTS];
static volatile LONG s_rr_acc_dx = 0, s_rr_acc_dy = 0, s_rr_acc_wh = 0;
static RRFrame* s_rr_fr = 0; static PadHostState* s_rr_frgp = 0; static int s_rr_n = 0, s_rr_i = 0;
static unsigned short* s_rr_age = 0;   /* ms the frame's screen had been up at record time (cap 3000) */
static RRPos* s_rr_pos = 0; static int s_rr_np = 0, s_rr_pi = 0;
static double s_rr_t0 = 0.0, s_rr_t1 = 0.0, s_rr_pause[2] = { 0.0, 0.0 };
static double rr_ms(void)
{
    static LARGE_INTEGER f; LARGE_INTEGER c; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c); return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
static int rr_sig(void) { return g_gf_grid_valid ? 1000 : (g_gf_mp_n > 0 && g_gf_mp_n < 999 ? g_gf_mp_n : 0); }
static BOOL CALLBACK rr_enum_cb(HWND h, LPARAM lp)
{
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid); RECT r;
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && GetClientRect(h, &r) && r.right > 64) { *(HWND*)lp = h; return FALSE; }
    return TRUE;
}
static HWND rr_own_hwnd(void) { static HWND h = 0; if (!h || !IsWindow(h)) { h = 0; EnumWindows(rr_enum_cb, (LPARAM)&h); } return h ? h : (HWND)(intptr_t)1; }
static void rr_close(void) { if (s_rr_f) { fflush(s_rr_f); fclose(s_rr_f); s_rr_f = 0; } }
static void rr_init(void)
{
    static int done = 0; if (done) return; done = 1;
    const char* rp = getenv("PAD_REPLAY"); const char* rc = getenv("PAD_RECORD");
    char magic[8]; unsigned hd[4];
    if (rp && *rp) {
        FILE* f = fopen(rp, "rb");
        if (!f || fread(magic, 1, 8, f) != 8 || memcmp(magic, "PADRR1\n", 8) || fread(hd, sizeof hd, 1, f) != 1 ||
            hd[0] != 1 || hd[1] != sizeof(RRFrame) || hd[2] != sizeof(PadHostState) || hd[3] != PAD_MAX_HOST_PORTS) {
            fprintf(stderr, "[padrr] v1129RR PAD_REPLAY: cannot use '%s' (missing or wrong format) -> replay OFF%c", rp, 10);
            if (f) fclose(f); return;
        }
        int cap = 1 << 16, pcap = 1024; s_rr_fr = (RRFrame*)malloc(sizeof(RRFrame) * cap);
        s_rr_frgp = (PadHostState*)malloc(sizeof(PadHostState) * PAD_MAX_HOST_PORTS * cap); s_rr_pos = (RRPos*)malloc(sizeof(RRPos) * pcap);
        PadHostState gp[PAD_MAX_HOST_PORTS]; memset(gp, 0, sizeof gp);
        int ty;
        while (s_rr_fr && s_rr_frgp && s_rr_pos && (ty = fgetc(f)) != EOF) {
            if (ty == 1) {
                RRFrame fr; if (fread(&fr, sizeof fr, 1, f) != 1) break;
                if ((fr.flags & 4) && fread(gp, sizeof gp, 1, f) != 1) break;
                if (s_rr_n == cap) { cap *= 2; s_rr_fr = (RRFrame*)realloc(s_rr_fr, sizeof(RRFrame) * cap);
                                     s_rr_frgp = (PadHostState*)realloc(s_rr_frgp, sizeof(PadHostState) * PAD_MAX_HOST_PORTS * cap); if (!s_rr_fr || !s_rr_frgp) break; }
                s_rr_fr[s_rr_n] = fr; memcpy(&s_rr_frgp[s_rr_n * PAD_MAX_HOST_PORTS], gp, sizeof gp); s_rr_n++;
            } else if (ty == 2) {
                RRPos ps; if (fread(&ps, sizeof ps, 1, f) != 1) break;
                if (s_rr_np == pcap) { pcap *= 2; s_rr_pos = (RRPos*)realloc(s_rr_pos, sizeof(RRPos) * pcap); if (!s_rr_pos) break; }
                s_rr_pos[s_rr_np++] = ps;
            } else break;
        }
        fclose(f);
        if (!s_rr_fr || !s_rr_frgp || !s_rr_pos || s_rr_n == 0) { fprintf(stderr, "[padrr] v1129RR PAD_REPLAY: '%s' holds no input -> replay OFF%c", rp, 10); return; }
        s_rr_age = (unsigned short*)malloc(sizeof(unsigned short) * s_rr_n);
        if (!s_rr_age) { fprintf(stderr, "[padrr] v1129RR PAD_REPLAY: out of memory -> replay OFF%c", 10); return; }
        { int sg = -1, sig = -99999; unsigned on = 0;
          for (int i = 0; i < s_rr_n; i++) { const RRFrame* q = &s_rr_fr[i];
              if (q->seg != sg || q->sig != sig) { sg = q->seg; sig = q->sig; on = q->ofs; }
              const unsigned a = q->ofs - on; s_rr_age[i] = (unsigned short)(a > 3000u ? 3000u : a); } }
        double m0 = 0.0, m1 = 0.0;
        for (int i = 0; i < s_rr_n; i++) { if (s_rr_fr[i].seg == 0) m0 = s_rr_fr[i].ofs / 1000.0; else m1 = s_rr_fr[i].ofs / 1000.0; }
        memset(&s_rr_cur, 0, sizeof s_rr_cur); memset(s_rr_gp, 0, sizeof s_rr_gp);
        s_rr_mode = 2;
        fprintf(stderr, "[padrr] v1129RR REPLAY <- %s: %d frames, %.1f s boot/menus + %.1f s in the world, %d positions (real keyboard / mouse / controllers ignored)%c",
                rp, s_rr_n, m0, m1, s_rr_np, 10);
    } else if (rc && *rc) {
        s_rr_f = fopen(rc, "wb");
        if (!s_rr_f) { fprintf(stderr, "[padrr] v1129RR PAD_RECORD: cannot write '%s' -> record OFF%c", rc, 10); return; }
        setvbuf(s_rr_f, 0, _IOFBF, 1 << 20);
        hd[0] = 1; hd[1] = sizeof(RRFrame); hd[2] = sizeof(PadHostState); hd[3] = PAD_MAX_HOST_PORTS;
        fwrite("PADRR1\n", 1, 8, s_rr_f); fwrite(hd, sizeof hd, 1, s_rr_f); fflush(s_rr_f);
        atexit(rr_close);
        s_rr_mode = 1;
        fprintf(stderr, "[padrr] v1129RR RECORD -> %s (every input until the game closes)%c", rc, 10);
    }
}
/* D3D12 window / raw-input thread: one WM_INPUT mouse packet (replaces the direct adds there). */
void pad_rr_mouse_in(long dx, long dy, long wheel)
{
    if (s_rr_mode == 2) return;   /* replay: the real mouse is ignored */
    InterlockedExchangeAdd(&g_pad_raw_mdx, dx); InterlockedExchangeAdd(&g_pad_raw_mdy, dy);
    InterlockedExchangeAdd(&g_pad_raw_look_dx, dx); InterlockedExchangeAdd(&g_pad_raw_look_dy, dy);
    if (wheel) InterlockedExchangeAdd(&g_pad_raw_wheel, wheel);
    if (s_rr_mode == 1) { InterlockedExchangeAdd(&s_rr_acc_dx, dx); InterlockedExchangeAdd(&s_rr_acc_dy, dy); if (wheel) InterlockedExchangeAdd(&s_rr_acc_wh, wheel); }
    g_pad_raw_ok = 1;
}
static int rr_edge(const RRFrame* a, const PadHostState* ga, const RRFrame* b, const PadHostState* gb)
{
    if (memcmp(a->keys, b->keys, sizeof a->keys) || ((a->flags ^ b->flags) & 1)) return 1;
    for (int p = 0; p < PAD_MAX_HOST_PORTS; p++) if (ga[p].buttons != gb[p].buttons || ga[p].connected != gb[p].connected) return 1;
    return 0;
}
static void rr_apply(int i, int mouse)
{
    const RRFrame* f = &s_rr_fr[i];
    if (mouse && (f->mdx || f->mdy)) {
        InterlockedExchangeAdd(&g_pad_raw_mdx, f->mdx); InterlockedExchangeAdd(&g_pad_raw_mdy, f->mdy);
        InterlockedExchangeAdd(&g_pad_raw_look_dx, f->mdx); InterlockedExchangeAdd(&g_pad_raw_look_dy, f->mdy);
    }
    if (mouse && f->wheel) InterlockedExchangeAdd(&g_pad_raw_wheel, f->wheel);
    if (f->flags & 8) g_pad_raw_ok = 1;
    s_rr_cur = *f;
    memcpy(s_rr_gp, &s_rr_frgp[i * PAD_MAX_HOST_PORTS], sizeof s_rr_gp);
}
static RRFrame s_rr_rec, s_rr_last; static PadHostState s_rr_lastgp[PAD_MAX_HOST_PORTS]; static int s_rr_nw = 0;
/* host poll thread, start of each input tick (before the controller and keyboard sweeps) */
static void rr_tick_begin(void)
{
    if (!s_rr_mode) return;
    const double now = rr_ms();
    if (s_rr_t0 == 0.0) s_rr_t0 = now;
    if (g_bench_world_ms && s_rr_t1 == 0.0) {
        s_rr_t1 = now;
        if (s_rr_mode == 2) {   /* world reached: menu input not played yet is folded in (state only) */
            int skipped = 0; while (s_rr_i < s_rr_n && s_rr_fr[s_rr_i].seg == 0) { rr_apply(s_rr_i++, 0); skipped++; }
            fprintf(stderr, "[padrr] v1129RR replay: world mark after %.1f s (%d menu frames left over, folded in)%c", (now - s_rr_t0) / 1000.0, skipped, 10);
        }
    }
    const int seg = s_rr_t1 != 0.0;
    const double base = seg ? s_rr_t1 : s_rr_t0;
    if (s_rr_mode == 1) {
        RRFrame* f = &s_rr_rec; memset(f, 0, sizeof *f);
        f->seg = (unsigned char)seg; f->ofs = (unsigned)(now - base);
        HWND fg = GetForegroundWindow(); DWORD pid = 0; if (fg) GetWindowThreadProcessId(fg, &pid);
        const int foc = fg && pid == GetCurrentProcessId();
        if (foc) { f->flags |= 1; for (int k = 1; k < 256; k++) if (GetAsyncKeyState(k) & 0x8000) f->keys[k >> 3] |= (unsigned char)(1u << (k & 7)); }
        if (GetKeyState(VK_CAPITAL) & 1) f->flags |= 2;
        if (g_pad_raw_ok) f->flags |= 8;
        POINT p; if (GetCursorPos(&p)) { f->cx = p.x; f->cy = p.y; }
        RECT r; POINT o = { 0, 0 };
        if (foc && GetClientRect(fg, &r) && ClientToScreen(fg, &o)) { f->ox = o.x; f->oy = o.y; f->rw = r.right; f->rh = r.bottom; }
        else { f->ox = s_rr_last.ox; f->oy = s_rr_last.oy; f->rw = s_rr_last.rw; f->rh = s_rr_last.rh; }
        f->mdx = (int)InterlockedExchange(&s_rr_acc_dx, 0); f->mdy = (int)InterlockedExchange(&s_rr_acc_dy, 0); f->wheel = (int)InterlockedExchange(&s_rr_acc_wh, 0);
        f->sig = (short)rr_sig();
        return;
    }
    /* replay */
    static int gate_on = -1, gate_ms = 15000; static double gate_t0 = 0.0; static int said_end = 0, ngate = 0;
    if (gate_on < 0) { gate_on = pad_envi("PAD_REPLAY_GATE", 1); gate_ms = pad_envi("PAD_REPLAY_GATE_MS", 15000); }
    static int live_sig = -99999; static double live_on = 0.0;
    { const int ls = rr_sig(); if (ls != live_sig) { live_sig = ls; live_on = now; } }
    const double clock = now - base - s_rr_pause[seg];
    while (s_rr_i < s_rr_n && s_rr_fr[s_rr_i].seg == seg && (double)s_rr_fr[s_rr_i].ofs <= clock) {
        const RRFrame* f = &s_rr_fr[s_rr_i];
        if (gate_on && rr_edge(f, &s_rr_frgp[s_rr_i * PAD_MAX_HOST_PORTS], &s_rr_cur, s_rr_gp) &&
            (f->sig != live_sig || now - live_on < (double)s_rr_age[s_rr_i])) {   /* same screen, up as long as when you pressed */
            if (gate_t0 == 0.0) { gate_t0 = now;
                if (ngate++ < 60) fprintf(stderr, "[padrr] v1129RR replay seg %d t=%.1f s: input waits for screen %d (now %d, up %.1f s, you pressed after %.1f s)%c", seg, f->ofs / 1000.0, f->sig, live_sig, (now - live_on) / 1000.0, s_rr_age[s_rr_i] / 1000.0, 10); }
            if (now - gate_t0 < (double)gate_ms) { s_rr_pause[seg] = now - base - (double)f->ofs; break; }
            fprintf(stderr, "[padrr] v1129RR replay: screen %d never came (%d ms) -> input played anyway%c", f->sig, gate_ms, 10);
        } else if (gate_t0 != 0.0 && ngate <= 60) {
            fprintf(stderr, "[padrr] v1129RR replay: screen %d up after %.1f s, input continues%c", f->sig, (now - gate_t0) / 1000.0, 10);
        }
        gate_t0 = 0.0;
        rr_apply(s_rr_i++, 1);
    }
    if (s_rr_i >= s_rr_n && !said_end) {
        said_end = 1;   /* end of the recording: release everything */
        memset(s_rr_cur.keys, 0, sizeof s_rr_cur.keys);
        for (int p = 0; p < PAD_MAX_HOST_PORTS; p++) { PadHostState* g = &s_rr_gp[p]; const int c = g->connected; memset(g, 0, sizeof *g);
            g->connected = c; g->analog_lx = g->analog_ly = g->analog_rx = g->analog_ry = 128; }
        fprintf(stderr, "[padrr] v1129RR replay finished (segment %d, %.1f s): all input released%c", seg, (now - base) / 1000.0, 10);
    }
}
/* host poll thread, after the controller sweep (XInput / SDL3), before the keyboard merge */
static void rr_tick_gamepad(void)
{
    if (!s_rr_mode) return;
    const double now = rr_ms();
    if (s_rr_mode == 1) {
        RRFrame* f = &s_rr_rec;
        const int gpch = memcmp(s_rr_lastgp, s_host_state, sizeof s_rr_lastgp) != 0 || s_rr_nw == 0;
        RRFrame a = *f, b = s_rr_last; a.ofs = b.ofs = 0;
        if (gpch || f->mdx || f->mdy || f->wheel || memcmp(&a, &b, sizeof a)) {
            if (gpch) f->flags |= 4;
            fputc(1, s_rr_f); fwrite(f, sizeof *f, 1, s_rr_f);
            if (gpch) { fwrite(s_host_state, sizeof s_rr_lastgp, 1, s_rr_f); memcpy(s_rr_lastgp, s_host_state, sizeof s_rr_lastgp); }
            s_rr_last = *f; s_rr_last.flags &= (unsigned char)~4; s_rr_last.mdx = s_rr_last.mdy = s_rr_last.wheel = 0; s_rr_nw++;
        }
        static double tf = 0.0; if (now - tf >= 500.0) { tf = now; fflush(s_rr_f); }
    } else {
        memcpy(s_host_state, s_rr_gp, sizeof s_rr_gp);   /* recorded controllers instead of the real ones */
    }
    /* route drift: player position every 2 s in the world */
    static double tp = 0.0, maxd = 0.0, sumd = 0.0; static int nd = 0;
    if (s_rr_t1 != 0.0 && g_pad_player_pos_fn && now - tp >= 2000.0) {
        tp = now; float p[3];
        if (g_pad_player_pos_fn(p)) {
            const unsigned ofs = (unsigned)(now - s_rr_t1 - (s_rr_mode == 2 ? s_rr_pause[1] : 0.0));
            if (s_rr_mode == 1) { RRPos ps; memset(&ps, 0, sizeof ps); ps.ofs = ofs; ps.seg = 1; memcpy(ps.p, p, sizeof ps.p); fputc(2, s_rr_f); fwrite(&ps, sizeof ps, 1, s_rr_f); }
            else if (s_rr_np > 0) {
                while (s_rr_pi + 1 < s_rr_np && s_rr_pos[s_rr_pi + 1].ofs <= ofs) s_rr_pi++;
                const RRPos* q = &s_rr_pos[s_rr_pi];
                const float dx = p[0] - q->p[0], dy = p[1] - q->p[1], dz = p[2] - q->p[2];
                const double d = sqrt((double)(dx * dx + dy * dy + dz * dz));
                if (d > maxd) maxd = d; sumd += d; nd++;
                static double tl = 0.0;
                if (now - tl >= 10000.0) { tl = now;
                    fprintf(stderr, "[padrr] v1129RR route t=%.0f s: player %.1f %.1f %.1f, recorded %.1f %.1f %.1f -> drift %.1f m (mean %.1f, max %.1f)%c",
                            ofs / 1000.0, (double)p[0], (double)p[1], (double)p[2], (double)q->p[0], (double)q->p[1], (double)q->p[2], d, sumd / nd, maxd, 10); }
            }
        }
    }
}
/* reader wrappers (replay = the recorded state; otherwise the real call) */
static SHORT rr_GetAsyncKeyState(int vk)
{
    if (s_rr_mode != 2) return GetAsyncKeyState(vk);
    return (vk > 0 && vk < 256 && ((s_rr_cur.keys[vk >> 3] >> (vk & 7)) & 1)) ? (SHORT)0x8000 : (SHORT)0;
}
static SHORT rr_GetKeyState(int vk)
{
    if (s_rr_mode != 2) return GetKeyState(vk);
    SHORT r = (vk > 0 && vk < 256 && ((s_rr_cur.keys[vk >> 3] >> (vk & 7)) & 1)) ? (SHORT)0x8000 : (SHORT)0;
    if (vk == VK_CAPITAL && (s_rr_cur.flags & 2)) r |= 1;
    return r;
}
static BOOL rr_GetCursorPos(LPPOINT p) { if (s_rr_mode != 2) return GetCursorPos(p); p->x = s_rr_cur.cx; p->y = s_rr_cur.cy; return TRUE; }
static HWND rr_GetForegroundWindow(void) { if (s_rr_mode != 2) return GetForegroundWindow(); return (s_rr_cur.flags & 1) ? rr_own_hwnd() : (HWND)0; }
static BOOL rr_ScreenToClient(HWND h, LPPOINT p) { if (s_rr_mode != 2) return ScreenToClient(h, p); p->x -= s_rr_cur.ox; p->y -= s_rr_cur.oy; return TRUE; }
static BOOL rr_ClientToScreen(HWND h, LPPOINT p) { if (s_rr_mode != 2) return ClientToScreen(h, p); p->x += s_rr_cur.ox; p->y += s_rr_cur.oy; return TRUE; }
static BOOL rr_GetClientRect(HWND h, LPRECT r)
{
    if (s_rr_mode != 2) return GetClientRect(h, r);
    r->left = r->top = 0; r->right = s_rr_cur.rw; r->bottom = s_rr_cur.rh; return s_rr_cur.rw > 0;
}
static BOOL rr_SetCursorPos(int x, int y) { if (s_rr_mode == 2) return TRUE; return SetCursorPos(x, y); }
static BOOL rr_ClipCursor(const RECT* r) { if (s_rr_mode == 2) return TRUE; return ClipCursor(r); }
/* exported for title code (pad_rr.h) */
SHORT pad_rr_GetAsyncKeyState(int vk) { return rr_GetAsyncKeyState(vk); }
SHORT pad_rr_GetKeyState(int vk) { return rr_GetKeyState(vk); }
HWND pad_rr_GetForegroundWindow(void) { return rr_GetForegroundWindow(); }
#define GetAsyncKeyState rr_GetAsyncKeyState
#define GetKeyState rr_GetKeyState
#define GetCursorPos rr_GetCursorPos
#define GetForegroundWindow rr_GetForegroundWindow
#define ScreenToClient rr_ScreenToClient
#define ClientToScreen rr_ClientToScreen
#define GetClientRect rr_GetClientRect
#define SetCursorPos rr_SetCursorPos
#define ClipCursor rr_ClipCursor
#else
static int s_rr_mode = 0;
static void rr_init(void) {}
static void rr_tick_begin(void) {}
static void rr_tick_gamepad(void) {}
#endif
/* ---- end v1129RR ---- */

#ifdef _WIN32
static int pad_host_window_focused(void)
{
    if (s_rr_mode == 2) return (s_rr_cur.flags & 1) != 0;   /* v1129RR replay: recorded focus */
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

/* v150 KEYBOARD + MOUSE (PAD_KBM, default 1; 0 = the old key map below).
 *   W/A/S/D = left stick (walk), hold Shift = run (full deflection)
 *   mouse   = right stick (camera), left click = R1 (shoot), right click = L1 (aim)
 *   Space = Cross (jump), F = Square, E = Triangle, C = Circle, R = R2, Q = L2,
 *   Ctrl = d-pad up (Precision), middle click = R3, V = L3, Enter = START, Tab = SELECT.
 *   On the map, mouse motion / WASD pan and the mouse wheel zooms.
 *   Hold Alt to free the mouse pointer. (Esc still closes the window.)
 * The pointer is hidden and re-centred while the game window has focus; mouse motion becomes a
 * right-stick deflection that decays over ~30 ms, with a small offset past the game's stick dead zone.
 * Tuning: PAD_MOUSE_SENS (default 6), PAD_MOUSE_INVERT_Y=1, PAD_MOUSE_DZ (default 22),
 * PAD_KBM_WALK (walk deflection 0..127, default 64). Merged into port 0 when a controller is present. */
static int pad_kbm_on(void);
static int pad_envi(const char* n, int d);
/* v193: which device the player used last -- read by the renderer to show keyboard/mouse or controller button
 * prompts (rsx_d3d12_backend.c KBM GLYPHS). 1 = keyboard/mouse, 0 = controller, -1 = nothing yet. */
volatile int g_input_kbm = -1;
static void pad_note_device(int kbm)
{
    if (g_input_kbm != kbm) {
        g_input_kbm = kbm;
        static int n = 0; if ((n < 40 && n++ < 40)) { printf("[cellPad] v193 input device -> %s (button prompts follow)\n", kbm ? "keyboard/mouse" : "controller"); fflush(stdout); }
    }
}
volatile int g_pad_mouse_captured = 0;   /* read by the D3D12 window proc (WM_SETCURSOR) */
/* v1107U last input device: 1 = keyboard / mouse, 0 = controller. The pointer only shows while the keyboard / mouse
 * was used last (D3D12 WM_SETCURSOR), and the game's menus free it then (pad_poll_kbm_generic, PAD_MENU_CURSOR=0 = off). */
volatile int g_pad_kbm_last = 1;
static void pad_set_kbm_last(int v)
{
    if (g_pad_kbm_last == v) return;
    g_pad_kbm_last = v;
    HWND fg = GetForegroundWindow(); DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (fg && pid == GetCurrentProcessId()) PostMessageA(fg, WM_SETCURSOR, (WPARAM)fg, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));   /* redraw the pointer now */
}
/* v153 RAW MOUSE: the D3D12 window registers for WM_INPUT and adds the device's raw counts here (no Windows
 * pointer acceleration, no counts lost between GetCursorPos and SetCursorPos, no clamping at the screen edge
 * on a fast flick). Used instead of the cursor delta once the first raw packet arrives; PAD_RAWINPUT=0 = off. */
volatile long g_pad_raw_mdx = 0, g_pad_raw_mdy = 0, g_pad_raw_wheel = 0;
volatile long g_pad_raw_look_dx = 0, g_pad_raw_look_dy = 0;   /* v1052M: the same raw counts for gf_footcam.c, drained per camera frame */
volatile int  g_pad_raw_ok = 0;
static int s_pad_clipped = 0;   /* v1043K cursor confined to the game window */
/* v151 DIRECT MOUSE (PAD_MOUSE_DIRECT, default 1): v150 fed the mouse through the right-stick BYTES, so the game's
 * dead zone, response curve and the stick's full-deflection speed limit applied ("feels like a controller").
 * Now the poll thread only accumulates raw pixel deltas; once per game frame the lifted pad update
 * (func_00152EFC, right after func_001523C0 converts the right stick -- hook at lr 0x153968) calls
 * ps3_pad_mouse_inject(), which writes the stick FLOATS the camera reads (ctrl+0x10 x, +0x14 y [+ = up],
 * +0x18 / +0x1F4 magnitude) from mouse VELOCITY (pixels per second), linear, no dead zone, and allowed past 1.0
 * (PAD_MOUSE_MAX, default 4) so fast flicks turn faster than a stick can. PAD_MOUSE_SPEED = pixels/s that equal
 * a full stick (default 600; lower = faster). Controller right stick still works when the mouse is still. */
static volatile int s_pad_xinput_merged = 0;   /* v165 diag: a controller owns port 0 (keys/mouse merged) */
static volatile long s_mdx = 0, s_mdy = 0;   /* accumulated pixels since the last game frame */
/* v152: the game's stick getter (func_00152240) squares the stick (out = v*|v|), so v151's linear velocity turned
 * slow moves into almost nothing and fast ones into a lot ("has to catch up"). We now write sign(v)*sqrt(|v|),
 * which the getter squares back to v: camera speed is linear in mouse speed. And the camera adds a damped spring
 * offset (camera obj +0x250 offset, +0x260 velocity, func_00073AB8 loc_00074190) that eases the view after a turn;
 * while the mouse is moving it is cleared every frame (ps3_pad_mouse_cam_spring) so the view stops where the mouse
 * stops. PAD_MOUSE_LINEAR=0 / PAD_MOUSE_NOSPRING=0 turn either off. */
static volatile unsigned long long s_mouse_active_ms = 0;
static volatile int s_pad_kbm_map_active = 0;
static int pad_mouse_direct(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("PAD_MOUSE_DIRECT"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
/* v236 MOUSE -> LEFT STICK WHEN NOTHING READS THE RIGHT STICK (PAD_MOUSE_LSTICK, default 1; 0 = off).
 * The Sasha boss "struggle" (GrappleGame: "Use the left stick to aim at the target, press X repeatedly") aims with the
 * LEFT stick only and the camera is scripted, so the mouse did nothing and WASD (8 digital directions at full or walk
 * deflection) could not hold the reticle on the target. Detection is generic, no struggle-specific address: the game
 * reads the right stick only through func_00152240(ctrl, which=1, ..) from its two camera updates (lr 0x73FEC =
 * third-person camera, only when the stick is deflected; lr 0x856F0 = per-stick "control kind" camera). Hand patches
 * there call ps3_pad_rstick_read(). When the mouse writes a clear right-stick deflection for PAD_MOUSE_LSTICK_FRAMES
 * (6) mouse frames in a row and no camera read it, the mouse also drives the LEFT stick floats (ctrl+0x04 x, +0x08 y
 * [+ = up], +0x0C / +0x19C magnitude; velocity like mouse look, |v| <= 1, x PAD_MOUSE_LSTICK_GAIN). The first camera
 * read of the right stick, 2 s without mouse motion, or the pause menu end it. WASD / a controller stick still win.
 * Log: [mouse-lstick] on enter / leave + read counts per site and the 0x856F0 control kinds seen. */
static volatile LONG s_rs_reads = 0, s_rs_n73 = 0, s_rs_n85 = 0;
static volatile LONG s_rs_kind[8];
static volatile int s_kbm_lstick_keys = 0;      /* WASD is moving the left stick this poll (pad_poll_kbm) */
void ps3_pad_rstick_read(u32 site, u32 which, u32 kind)
{
    if (!which) return;
    InterlockedIncrement(&s_rs_reads);
    if (site == 0x73FECu) InterlockedIncrement(&s_rs_n73);
    else { InterlockedIncrement(&s_rs_n85); InterlockedIncrement(&s_rs_kind[kind < 7u ? kind : 7u]); }
}
static void pad_mouse_lstick(u32 ctrl, long dx, long dy, double dt, int captured)
{
    static int on = -1, need = 6; static float gain = 1.0f, spd = 450.0f;
    if (on < 0) { on = pad_envi("PAD_MOUSE_LSTICK", 1); need = pad_envi("PAD_MOUSE_LSTICK_FRAMES", 6); if (need < 2) need = 2;
                  const char* e = getenv("PAD_MOUSE_LSTICK_GAIN"); if (e && *e) gain = (float)atof(e); if (gain <= 0.0f) gain = 1.0f;
                  e = getenv("PAD_MOUSE_SPEED"); if (e && *e) spd = (float)atof(e); if (spd < 10.0f) spd = 10.0f;
                  printf("[mouse-lstick] v236 mouse drives the left stick when no camera reads the right stick: %s (frames %d, gain %.2f)\n",
                         on ? "ON" : "off", need, gain); fflush(stdout); }
    if (!on) return;
    static LONG s_judge = -1;          /* s_rs_reads right after the last clear mouse frame, -1 = nothing to judge */
    static int s_unread = 0, s_active = 0, s_nlog = 0;
    static float s_lx = 0.0f, s_ly = 0.0f;
    static unsigned long long s_last_move = 0;
    extern int (*volatile g_uiov_paused_fn)(void);
    const unsigned long long now = GetTickCount64();
    const int paused = g_uiov_paused_fn ? g_uiov_paused_fn() : 0;
    const int moved = captured && (dx || dy);
    const LONG reads = s_rs_reads;
#define LSTICK_LOG(what) do { if (s_nlog < 60) { s_nlog++; \
        printf("[mouse-lstick] v236 %s | right-stick reads: 3rd-person cam %ld, kind cam %ld (kinds 0:%ld 1:%ld 2:%ld 3:%ld 4:%ld)\n", what, \
               (long)s_rs_n73, (long)s_rs_n85, (long)s_rs_kind[0], (long)s_rs_kind[1], (long)s_rs_kind[2], (long)s_rs_kind[3], (long)s_rs_kind[4]); \
        fflush(stdout); } } while (0)
    if (s_judge >= 0 && reads != s_judge) {            /* a camera consumed the right stick since the last mouse frame */
        if (s_active) LSTICK_LOG("off (a camera reads the right stick again)");
        s_active = 0; s_unread = 0; s_judge = -1;
    }
    if (paused || !captured) { if (s_active) LSTICK_LOG(paused ? "off (paused)" : "off (mouse released)");
                               s_active = 0; s_unread = 0; s_judge = -1; s_lx = s_ly = 0.0f; return; }
    float tx = 0.0f, ty = 0.0f;
    if (moved) {
        s_last_move = now;
        tx = (float)((double)dx / dt) / spd;
        ty = -(float)((double)dy / dt) / spd;           /* mouse up -> stick up (+) */
        if (s_judge >= 0 && !s_active && ++s_unread >= need) { s_active = 1; s_lx = s_ly = 0.0f; LSTICK_LOG("ON (mouse moves, nothing reads the right stick)"); }
        /* judge only clear deflections (the 3rd-person camera skips its read below a small magnitude) */
        s_judge = (sqrtf(tx * tx + ty * ty) >= 0.25f) ? reads : -1;
        if (s_judge < 0 && !s_active) s_unread = 0;
    } else if (s_active && now - s_last_move > 2000) {
        LSTICK_LOG("off (mouse idle 2 s)"); s_active = 0; s_unread = 0; s_judge = -1;
    }
    if (!s_active) return;
    /* light smoothing so frames without a mouse packet do not drop the stick to zero */
    s_lx = s_lx * 0.35f + tx * gain * 0.65f;
    s_ly = s_ly * 0.35f + ty * gain * 0.65f;
    float m = sqrtf(s_lx * s_lx + s_ly * s_ly);
    if (m > 1.0f) { s_lx /= m; s_ly /= m; m = 1.0f; }
    if (m < 0.002f) { s_lx = s_ly = 0.0f; return; }    /* still: leave the converted stick (controller) alone */
    if (s_kbm_lstick_keys) return;                     /* WASD wins */
    { union { u32 u; float f; } cx; cx.u = vm_read32(ctrl + 0x0C);
      if (cx.f > 0.2f) return; }                       /* a controller stick is already deflected: it wins */
    union { float f; u32 u; } ux, uy, um;
    ux.f = s_lx; uy.f = s_ly; um.f = m;
    vm_write32(ctrl + 0x04, ux.u);
    vm_write32(ctrl + 0x08, uy.u);
    vm_write32(ctrl + 0x0C, um.u);
    vm_write32(ctrl + 0x19C, um.u);
#undef LSTICK_LOG
}
void ps3_pad_mouse_inject(u32 ctrl)
{
    if (!pad_kbm_on() || !pad_mouse_direct() || !ctrl) return;
    if (s_pad_kbm_map_active) {
        InterlockedExchange(&s_mdx, 0);
        InterlockedExchange(&s_mdy, 0);
        return;                         /* map consumes the mouse as the left stick */
    }
    if (vm_read32(ctrl + 0x20) != 0) return;               /* pad port 0 only */
    static LARGE_INTEGER s_f, s_t0; LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (!s_f.QuadPart) { QueryPerformanceFrequency(&s_f); s_t0 = t; return; }
    double dt = (double)(t.QuadPart - s_t0.QuadPart) / (double)s_f.QuadPart;
    s_t0 = t;
    const long dx = InterlockedExchange(&s_mdx, 0), dy = InterlockedExchange(&s_mdy, 0);
    pad_mouse_lstick(ctrl, dx, dy, dt < 0.004 ? 0.004 : (dt > 0.1 ? 0.1 : dt), g_pad_mouse_captured);   /* v236 */
    if (!g_pad_mouse_captured || (dx == 0 && dy == 0)) return;
    if (dt < 0.004) dt = 0.004;
    if (dt > 0.1) dt = 0.1;
    static float s_speed = -1.0f, s_max = 8.0f; static int s_inv = 0;
    { extern volatile int g_ui_mouse_speed; if (g_ui_mouse_speed > 0 && s_speed > 0.0f) s_speed = (float)g_ui_mouse_speed; }   /* v194 */
    if (s_speed < 0.0f) { const char* e = getenv("PAD_MOUSE_SPEED"); s_speed = e ? (float)atof(e) : 450.0f; if (s_speed < 10.0f) s_speed = 10.0f;
                          e = getenv("PAD_MOUSE_MAX"); if (e) s_max = (float)atof(e); if (s_max < 1.0f) s_max = 1.0f;
                          s_inv = pad_envi("PAD_MOUSE_INVERT_Y", 0);
                          printf("[cellPad] v151 direct mouse look: full stick = %.0f px/s, cap %.1f\n", s_speed, s_max); fflush(stdout); }
    float x = (float)((double)dx / dt) / s_speed;
    float y = -(float)((double)dy / dt) / s_speed;             /* mouse down -> look down (stick y + = up) */
    if (s_inv) y = -y;
    float m = sqrtf(x * x + y * y);
    if (m > s_max) { x *= s_max / m; y *= s_max / m; m = s_max; }
    s_mouse_active_ms = GetTickCount64();
    if (pad_envi("PAD_MOUSE_LINEAR", 1)) {                     /* pre-compensate the getter's v*|v| */
        x = (x >= 0 ? 1.0f : -1.0f) * sqrtf(fabsf(x));
        y = (y >= 0 ? 1.0f : -1.0f) * sqrtf(fabsf(y));
        m = sqrtf(m);
    }
    union { float f; u32 u; } cx, cy, cm;
    cx.f = x; cy.f = y; cm.f = m;
    /* v165 MOUSEDIAG (default on, rate-limited): what the stick conversion left in the floats, what the mouse
     * writes, the game's look-invert flags ([[toc-0x7DC0]-0x7FB4]+0xB0 x / +0xB1 y, read by func_00152240) and
     * whether a controller is merged into port 0 -- for the 'mouse inverted after using the gamepad' report. */
    { static int s_md = -1; static unsigned long long s_mt = 0; static int s_n = 0; static int s_inv_prev = -1;
      if (s_md < 0) s_md = pad_envi("PAD_MOUSE_DIAG", 1);
      if (s_md) {
          const u32 pool = vm_read32(0x0078BFC0u - 0x7DC0u), g = pool ? vm_read32(pool - 0x7FB4u) : 0;
          const int inv = g ? (int)((vm_read8(g + 0xB0) ? 1 : 0) | (vm_read8(g + 0xB1) ? 2 : 0)) : -1;
          union { u32 u; float f; } px, py; px.u = vm_read32(ctrl + 0x10); py.u = vm_read32(ctrl + 0x14);
          const unsigned long long now = GetTickCount64();
          if (inv != s_inv_prev) { printf("[mousediag] game look-invert flags now x=%d y=%d (was %d)\n", inv & 1, (inv >> 1) & 1, s_inv_prev); fflush(stdout); s_inv_prev = inv; }
          if (now - s_mt >= 250 && s_n < 400) { s_mt = now; s_n++;
              printf("[mousediag] dx %ld dy %ld -> write x %.3f y %.3f | stick conv x %.3f y %.3f | pad bytes rx %u ry %u | ctrl merged %d | inv x%d y%d | raw %d\n",
                     dx, dy, x, y, px.f, py.f, (unsigned)s_host_state[0].analog_rx, (unsigned)s_host_state[0].analog_ry,
                     s_pad_xinput_merged, inv & 1, (inv >> 1) & 1, g_pad_raw_ok); fflush(stdout); } } }
    vm_write32(ctrl + 0x10, cx.u);
    vm_write32(ctrl + 0x14, cy.u);
    vm_write32(ctrl + 0x18, cm.u);
    vm_write32(ctrl + 0x1F4, cm.u);
}
/* v153 RAW TURN: the real cause of the "drift / has to catch up" feel. The camera (func_00073AB8) does not turn at
 * stick * rate: it keeps a turn VELOCITY (vr29 -> cam+0x1D0) and moves it toward stick * rate by at most a fixed
 * step per frame (func_00382AE8 = clamp the change's length), i.e. turn ACCELERATION -- the view speeds up after
 * the mouse starts and keeps sliding after it stops. While the mouse is driving the camera that clamp is skipped
 * (velocity = target every frame) and the stick axis snap (pure horizontal / vertical within a few degrees of an
 * axis, loc_00075CB0 / loc_00075D18) is off, so the view turns exactly with the mouse and stops with it.
 * Controller stick unchanged. PAD_MOUSE_RAWTURN=0 = off. */
int ps3_pad_mouse_raw_turn(void)
{
    static int on = -1;
    if (on < 0) { on = pad_envi("PAD_MOUSE_RAWTURN", 1);
                  printf("[cellPad] v153 mouse raw turn (no turn acceleration / axis snap): %s\n", on ? "ON" : "off"); fflush(stdout); }
    if (!on || !g_pad_mouse_captured || !pad_kbm_on() || !pad_mouse_direct()) return 0;
    return (GetTickCount64() - s_mouse_active_ms) < 400;
}
void ps3_pad_mouse_cam_spring(u32 cam)
{
    static int on = -1;
    if (on < 0) on = pad_envi("PAD_MOUSE_NOSPRING", 1);
    if (!on || !cam || !pad_kbm_on() || !pad_mouse_direct()) return;
    if (GetTickCount64() - s_mouse_active_ms > 120) return;    /* only while the mouse is turning the camera */
    for (u32 o = 0; o < 0x30; o += 4) vm_write32(cam + 0x240 + o, 0);   /* v153: + the 0x240 spring */
}
static int pad_kbm_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("PAD_KBM"); v = (e && *e == '0') ? 0 : 1;
                 printf("[cellPad] v150 keyboard+mouse controls %s (PAD_KBM)\n", v ? "ON" : "off"); fflush(stdout); }
    return v;
}
static int pad_envi(const char* n, int d) { const char* e = getenv(n); return e && *e ? atoi(e) : d; }
static u8 pad_axis(float v)   /* v in -127..127 -> 0..255, 128 = centre */
{
    if (v > 127.0f) v = 127.0f;
    if (v < -127.0f) v = -127.0f;
    int r = 128 + (int)(v >= 0 ? v + 0.5f : v - 0.5f);
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    return (u8)r;
}
static void pad_poll_kbm(int merge)
{
    PadHostState* hs = &s_host_state[0];
    static float s_mx = 0.0f, s_my = 0.0f;
    static int s_sens = -1, s_inv = 0, s_dz = 0, s_walk = 0;
    if (s_sens < 0) { s_sens = pad_envi("PAD_MOUSE_SENS", 6); s_inv = pad_envi("PAD_MOUSE_INVERT_Y", 0);
                      s_dz = pad_envi("PAD_MOUSE_DZ", 22); s_walk = pad_envi("PAD_KBM_WALK", 64); }
    const int focused = pad_host_window_focused();
    const long raw_dx = InterlockedExchange(&g_pad_raw_mdx, 0), raw_dy = InterlockedExchange(&g_pad_raw_mdy, 0);
    const long raw_wheel = InterlockedExchange(&g_pad_raw_wheel, 0);
    if (!focused) {
        s_pad_kbm_map_active = 0;
        g_pad_mouse_captured = 0;
        s_mx = s_my = 0.0f;
        if (!merge) {
            hs->buttons = 0;
            hs->analog_lx = hs->analog_ly = 128;
            hs->analog_rx = hs->analog_ry = 128;
            hs->trigger_l2 = hs->trigger_r2 = 0;
            hs->connected = 1;
        }
        return;
    }
#define KD(vk) ((GetAsyncKeyState(vk) & 0x8000) != 0)
    /* v155 MENU MODE: Esc = Start (opens/closes the pause menu) and frees the mouse pointer (visible, not
     * recentred, mouse buttons not mapped) until Esc again or a left click in the game window. */
    static int s_menu_free = 0, s_esc_prev = 0, s_lmb_block = 0;
    {
        const int esc = KD(VK_ESCAPE);
        extern volatile int g_uiov_menu_open;   /* v194: Esc belongs to the settings menu while it is open */
        if (esc && !s_esc_prev && !g_uiov_menu_open) s_menu_free = !s_menu_free;
        s_esc_prev = esc;
        if (s_menu_free && KD(VK_LBUTTON)) {
            HWND fw = GetForegroundWindow(); POINT cp; RECT cr;
            if (fw && GetCursorPos(&cp) && ScreenToClient(fw, &cp) && GetClientRect(fw, &cr) &&
                cp.x >= 0 && cp.y >= 0 && cp.x < cr.right && cp.y < cr.bottom) {
                s_menu_free = 0; s_lmb_block = 1;   /* the recapturing click is not a shot */
            }
        }
        if (s_lmb_block && !KD(VK_LBUTTON)) s_lmb_block = 0;
    }
    /* v196 MENU KEYS: while the game is paused (pause menu and its pages; g_uiov_paused_fn = stubs.cpp probe)
     * W/A/S/D = d-pad, Enter = Cross, Backspace = Circle. Decided per key press: a key already held when the
     * pause started keeps its old meaning until released (Enter that opened the menu does not select Powers,
     * W held while running does not scroll the list). Esc stays Start (close the pause menu). */
    extern int (*volatile g_uiov_paused_fn)(void);
    static int s_mk_mode[5];   /* W A S D Enter: 0 up, 1 game/map meaning, 2 menu meaning */
    const int paused_now = g_uiov_paused_fn ? g_uiov_paused_fn() : 0;
    /* Tab opens the world map. The pause probe takes 300 ms to notice that simulation stopped, so retain a
     * short pending window after the key press. Circle/Esc and a second Tab close it immediately; if the game
     * never pauses, the pending state expires instead of hijacking normal mouse-look. */
    static int s_map_mode = 0, s_tab_prev = 0;
    static unsigned long long s_map_pending_until = 0;
    const int tab_now = KD(VK_TAB);
    const unsigned long long input_now = GetTickCount64();
    if (tab_now && !s_tab_prev) {
        s_map_mode = !s_map_mode;
        s_map_pending_until = s_map_mode ? input_now + 1500 : 0;
    }
    s_tab_prev = tab_now;
    if (s_map_mode && (KD('C') || KD(VK_ESCAPE) || KD(VK_BACK))) s_map_mode = 0;
    if (s_map_mode && !paused_now && input_now >= s_map_pending_until) s_map_mode = 0;
    const int map_now = s_map_mode && (paused_now || input_now < s_map_pending_until);
    s_pad_kbm_map_active = map_now;
    static const int mk_vk[5] = { 'W', 'A', 'S', 'D', VK_RETURN };
    for (int i = 0; i < 5; i++) {
        if (!KD(mk_vk[i])) s_mk_mode[i] = 0;
        else if (!s_mk_mode[i]) s_mk_mode[i] = (paused_now && !map_now) ? 2 : 1;
    }
#define MK_GAME(i) (s_mk_mode[i] == 1)
#define MK_MENU(i) (s_mk_mode[i] == 2)
    u16 btns = 0;
    if (MK_MENU(0)) btns |= CELL_PAD_CTRL_UP;
    if (MK_MENU(1)) btns |= CELL_PAD_CTRL_LEFT;
    if (MK_MENU(2)) btns |= CELL_PAD_CTRL_DOWN;
    if (MK_MENU(3)) btns |= CELL_PAD_CTRL_RIGHT;
    if (MK_MENU(4)) btns |= CELL_PAD_CTRL_CROSS;
    if (paused_now && KD(VK_BACK)) btns |= CELL_PAD_CTRL_CIRCLE;
    if (KD(VK_CONTROL)) btns |= CELL_PAD_CTRL_UP;       /* Precision power/mode */
    if (paused_now && !map_now && KD(VK_UP)) btns |= CELL_PAD_CTRL_UP; /* menu navigation only */
    if (KD(VK_DOWN))    btns |= CELL_PAD_CTRL_DOWN;
    if (KD(VK_LEFT))    btns |= CELL_PAD_CTRL_LEFT;
    if (KD(VK_RIGHT))   btns |= CELL_PAD_CTRL_RIGHT;
    if (KD(VK_SPACE))   btns |= CELL_PAD_CTRL_CROSS;
    if (KD('C'))         btns |= CELL_PAD_CTRL_CIRCLE;
    if (KD('F'))        btns |= CELL_PAD_CTRL_SQUARE;
    if (KD('E'))        btns |= CELL_PAD_CTRL_TRIANGLE;
    if (!s_menu_free && KD(VK_RBUTTON)) btns |= CELL_PAD_CTRL_L1;
    if (!s_menu_free && !s_lmb_block && KD(VK_LBUTTON)) btns |= CELL_PAD_CTRL_R1;
    if (KD('Q'))        btns |= CELL_PAD_CTRL_L2;
    if (KD('R'))        btns |= CELL_PAD_CTRL_R2;
    if (KD('V'))        btns |= CELL_PAD_CTRL_L3;
    if (!s_menu_free && KD(VK_MBUTTON)) btns |= CELL_PAD_CTRL_R3;
    if (MK_GAME(4) || KD(VK_ESCAPE)) btns |= CELL_PAD_CTRL_START;   /* v155: Esc = Start (pause) */
    if (KD(VK_TAB))     btns |= CELL_PAD_CTRL_SELECT;
    /* left stick */
    float lx = 0.0f, ly = 0.0f;
    if (MK_GAME(1)) lx -= 1.0f;
    if (MK_GAME(3)) lx += 1.0f;
    if (MK_GAME(0)) ly -= 1.0f;   /* PS3: up = 0 */
    if (MK_GAME(2)) ly += 1.0f;
#undef MK_GAME
#undef MK_MENU
    const int keyboard_moving = (lx != 0.0f || ly != 0.0f);
    s_kbm_lstick_keys = keyboard_moving;   /* v236 */
    if (keyboard_moving) {
        const float mag = KD(VK_SHIFT) ? 127.0f : (float)s_walk;
        const float n = (lx != 0.0f && ly != 0.0f) ? 0.70710678f : 1.0f;
        lx *= mag * n; ly *= mag * n;
    }
    /* mouse -> right stick */
    const int alt = KD(VK_MENU);
#undef KD
    HWND fg = GetForegroundWindow();
    int have_mouse = 0;
    extern volatile int g_uiov_menu_open;   /* v194: settings menu open -> free the mouse pointer */
    if (fg && !alt && !s_menu_free && !g_uiov_menu_open) {
        RECT rc; POINT c, p;
        if (GetClientRect(fg, &rc) && rc.right > 8 && rc.bottom > 8) {
            c.x = (rc.right - rc.left) / 2; c.y = (rc.bottom - rc.top) / 2;
            ClientToScreen(fg, &c);
            if (GetCursorPos(&p)) {
                if (g_pad_mouse_captured && pad_mouse_direct()) {
                    if (map_now) {
                        const float dx = (float)(g_pad_raw_ok ? raw_dx : p.x - c.x);
                        const float dy = (float)(g_pad_raw_ok ? raw_dy : p.y - c.y);
                        s_mx = s_mx * 0.6f + dx * (float)s_sens;
                        s_my = s_my * 0.6f + (s_inv ? -dy : dy) * (float)s_sens;
                    } else if (g_pad_raw_ok) {   /* v153: raw device counts */
                        InterlockedExchangeAdd(&s_mdx, raw_dx);
                        InterlockedExchangeAdd(&s_mdy, raw_dy);
                        s_mx = s_my = 0.0f;
                    } else {
                        InterlockedExchangeAdd(&s_mdx, (long)(p.x - c.x));
                        InterlockedExchangeAdd(&s_mdy, (long)(p.y - c.y));
                        s_mx = s_my = 0.0f;
                    }
                } else if (g_pad_mouse_captured) {
                    const float dx = (float)(p.x - c.x), dy = (float)(p.y - c.y);
                    s_mx = s_mx * 0.6f + dx * (float)s_sens;
                    s_my = s_my * 0.6f + (s_inv ? -dy : dy) * (float)s_sens;
                } else { s_mx = s_my = 0.0f; }
                if (p.x != c.x || p.y != c.y) SetCursorPos(c.x, c.y);
                g_pad_mouse_captured = 1;
                have_mouse = 1;
            }
        }
    }
    if (!have_mouse) { g_pad_mouse_captured = 0; s_mx = s_my = 0.0f; }
    float rx = 0.0f, ry = 0.0f;
    if (s_mx > 0.5f || s_mx < -0.5f) rx = (s_mx > 0 ? 1.0f : -1.0f) * (float)s_dz + s_mx;
    if (s_my > 0.5f || s_my < -0.5f) ry = (s_my > 0 ? 1.0f : -1.0f) * (float)s_dz + s_my;
    const int mouse_motion = (rx != 0.0f || ry != 0.0f);
    static float s_map_zoom = 0.0f;
    static unsigned long long s_map_zoom_until = 0;
    if (map_now && raw_wheel) {
        s_map_zoom = raw_wheel > 0 ? -127.0f : 127.0f;
        s_map_zoom_until = input_now + 100;
    }
    if (!map_now || input_now >= s_map_zoom_until) s_map_zoom = 0.0f;
    if (map_now) {
        if (mouse_motion) { lx = rx; ly = ry; }
        rx = 0.0f; ry = s_map_zoom;             /* wheel -> right-stick vertical zoom */
    }
    const int map_panning = map_now && mouse_motion;
    const int moving = keyboard_moving || map_panning;
    const int looking = (!map_now && mouse_motion) || (map_now && ry != 0.0f);
    /* v193: keyboard/mouse activity -> keyboard/mouse prompts (raw mouse jitter of a few counts is ignored) */
    { const long am = (raw_dx < 0 ? -raw_dx : raw_dx) + (raw_dy < 0 ? -raw_dy : raw_dy);
      const int clicks = ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000) != 0;
      if (btns || moving || am > 3 || clicks) pad_note_device(1);
      else if (g_input_kbm < 0 && !merge) pad_note_device(1); }

    if (merge) {   /* a controller owns port 0: add the keys, take the sticks only while used */
        hs->buttons |= btns;
        if (moving)  { hs->analog_lx = pad_axis(lx); hs->analog_ly = pad_axis(ly); }
        if (looking) { hs->analog_rx = pad_axis(rx); hs->analog_ry = pad_axis(ry); }
        if (btns & CELL_PAD_CTRL_L2) hs->trigger_l2 = 255;
        if (btns & CELL_PAD_CTRL_R2) hs->trigger_r2 = 255;
    } else {
        hs->buttons = btns;
        hs->connected = 1;
        hs->analog_lx = pad_axis(lx); hs->analog_ly = pad_axis(ly);
        hs->analog_rx = pad_axis(rx); hs->analog_ry = pad_axis(ry);
        hs->trigger_l2 = (u8)((btns & CELL_PAD_CTRL_L2) ? 255 : 0);
        hs->trigger_r2 = (u8)((btns & CELL_PAD_CTRL_R2) ? 255 : 0);
    }
    const u16 b = hs->buttons;
    hs->press_up       = (b & CELL_PAD_CTRL_UP)       ? 255 : 0;
    hs->press_down     = (b & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
    hs->press_left     = (b & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
    hs->press_right    = (b & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
    hs->press_triangle = (b & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
    hs->press_circle   = (b & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
    hs->press_cross    = (b & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
    hs->press_square   = (b & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
    hs->press_l1       = (b & CELL_PAD_CTRL_L1)       ? 255 : 0;
    hs->press_r1       = (b & CELL_PAD_CTRL_R1)       ? 255 : 0;
    { static int said = 0; if (!said && (btns || moving || looking)) { said = 1;
        printf("[cellPad] v150 keyboard/mouse input active on port 0%s\n", merge ? " (merged with controller)" : ""); fflush(stdout); } }
}

/* PAD_KBM_PROFILE=generic -- keyboard + mouse for titles without inFamous' lifted-code hooks.
 *
 * The default KBM path (pad_poll_kbm) was built for inFamous: mouse look goes through
 * ps3_pad_mouse_inject(), which only that game's lifted code calls, and its menu keys rely on
 * an inFamous pause probe.  In any other title the mouse therefore did nothing, Up-arrow only
 * worked "while paused" and Enter was START.  This profile drives everything through the pad
 * packet itself, so it works in every game:
 *
 *   W/A/S/D  left stick (full), Ctrl = walk on / off (Shift sprint ends it)     mouse   right stick
 *   Space / Enter  Cross      C / Backspace  Circle      F  Square      E  Triangle
 *   LMB  R1      RMB  L1      Q  L2      R  R2      V / LShift  L3      MMB / Z  R3
 *   arrows  d-pad      1/2/3/4  d-pad up/right/down/left      Esc  START      Tab  SELECT
 *   Esc also frees the mouse pointer; click in the window to grab it again. Hold Alt = free pointer.
 *
 * Remap with PAD_KBM_MAP="KEY=BUTTON,..." (overrides / adds; BUTTON=NONE unbinds), e.g.
 *   PAD_KBM_MAP=LSHIFT=CROSS,SPACE=SQUARE
 * Keys: A-Z 0-9 SPACE ENTER ESC TAB BACKSPACE LSHIFT RSHIFT LCTRL RCTRL UP DOWN LEFT RIGHT
 *       LMB RMB MMB MB4 MB5 F1-F12.  Buttons: CROSS CIRCLE SQUARE TRIANGLE L1 R1 L2 R2 L3 R3
 *       START SELECT UP DOWN LEFT RIGHT NONE.
 * Mouse: PAD_MOUSE_SPEED (px/s for a full stick, default 1200), PAD_MOUSE_DZ (default 30),
 *        PAD_MOUSE_INVERT_Y=1, PAD_MOUSE_SMOOTH_MS (default 60). PAD_KBM_WALK (default 64). */
typedef struct { int vk; u16 btn; } KbmBind;
static KbmBind s_kbm_binds[96];
static int     s_kbm_nbinds = 0;
static int kbm_key_vk(const char* k)
{
    static const struct { const char* n; int vk; } names[] = {
        {"SPACE",VK_SPACE},{"ENTER",VK_RETURN},{"RETURN",VK_RETURN},{"ESC",VK_ESCAPE},{"ESCAPE",VK_ESCAPE},
        {"TAB",VK_TAB},{"BACKSPACE",VK_BACK},{"LSHIFT",VK_LSHIFT},{"RSHIFT",VK_RSHIFT},{"SHIFT",VK_SHIFT},
        {"LCTRL",VK_LCONTROL},{"RCTRL",VK_RCONTROL},{"CTRL",VK_CONTROL},{"UP",VK_UP},{"DOWN",VK_DOWN},
        {"LEFT",VK_LEFT},{"RIGHT",VK_RIGHT},{"LMB",VK_LBUTTON},{"RMB",VK_RBUTTON},{"MMB",VK_MBUTTON},
        {"MB4",VK_XBUTTON1},{"MB5",VK_XBUTTON2},{"CAPS",VK_CAPITAL},
    };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) if (!_stricmp(k, names[i].n)) return names[i].vk;
    if (k[0] && !k[1]) { char c = (char)toupper((unsigned char)k[0]); if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c; }
    if ((k[0] == 'F' || k[0] == 'f') && k[1]) { int n = atoi(k + 1); if (n >= 1 && n <= 12) return VK_F1 + n - 1; }
    return 0;
}
static int kbm_btn(const char* b, u16* out)
{
    static const struct { const char* n; u16 v; } names[] = {
        {"CROSS",CELL_PAD_CTRL_CROSS},{"X",CELL_PAD_CTRL_CROSS},{"CIRCLE",CELL_PAD_CTRL_CIRCLE},{"SQUARE",CELL_PAD_CTRL_SQUARE},
        {"TRIANGLE",CELL_PAD_CTRL_TRIANGLE},{"L1",CELL_PAD_CTRL_L1},{"R1",CELL_PAD_CTRL_R1},{"L2",CELL_PAD_CTRL_L2},
        {"R2",CELL_PAD_CTRL_R2},{"L3",CELL_PAD_CTRL_L3},{"R3",CELL_PAD_CTRL_R3},{"START",CELL_PAD_CTRL_START},
        {"SELECT",CELL_PAD_CTRL_SELECT},{"UP",CELL_PAD_CTRL_UP},{"DOWN",CELL_PAD_CTRL_DOWN},{"LEFT",CELL_PAD_CTRL_LEFT},
        {"RIGHT",CELL_PAD_CTRL_RIGHT},{"NONE",0},
    };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) if (!_stricmp(b, names[i].n)) { *out = names[i].v; return 1; }
    return 0;
}
static void kbm_bind(int vk, u16 btn)
{
    for (int i = 0; i < s_kbm_nbinds; i++) if (s_kbm_binds[i].vk == vk) { s_kbm_binds[i].btn = btn; return; }
    if (s_kbm_nbinds < (int)(sizeof s_kbm_binds / sizeof s_kbm_binds[0])) { s_kbm_binds[s_kbm_nbinds].vk = vk; s_kbm_binds[s_kbm_nbinds].btn = btn; s_kbm_nbinds++; }
}
/* v1130P keyboard / mouse scheme (settings menu "Keyboard Controls", settings.ini KBM_SCHEME, restart):
 *   reborn  (default) = the port's own layout (pad_poll_kbm_generic_reborn),
 *   classic = "PC Classic": the original EA PC port's controls (pad_poll_kbm_generic). Read once. */
volatile int g_kbm_classic = -1;
static int kbm_classic(void)
{
    if (g_kbm_classic < 0) { const char* e = getenv("KBM_SCHEME"); g_kbm_classic = (e && !_stricmp(e, "classic")) ? 1 : 0;
                             printf("[cellPad] v1130P keyboard/mouse scheme: %s\n", g_kbm_classic ? "PC Classic" : "Reborn"); fflush(stdout); }
    return g_kbm_classic;
}
static void kbm_generic_init(void)
{
    if (kbm_classic()) {   /* v1130P "PC Classic": the original EA PC port scheme */
        /* v1130E EA PC scheme (default.ini of the original PC port + its in-game prompts): Enter = Cross, Space = Triangle (talk),
         * C = Circle (crouch), Caps Lock / Num+ = Square (block / cover), J = L3, X = L3 (strangle), F = R3, LAlt = L2, RAlt / V = R2,
         * P = R1, Ctrl = L1, R / H / Z / [ ] = d-pad. Shift = sprint (Cross) except in combat, where it is the strong-attack modifier. */
        kbm_bind(VK_RETURN, CELL_PAD_CTRL_CROSS);   kbm_bind(VK_SPACE, CELL_PAD_CTRL_TRIANGLE);
        kbm_bind('C', CELL_PAD_CTRL_CIRCLE);        kbm_bind(VK_BACK, CELL_PAD_CTRL_CIRCLE);
        kbm_bind('T', CELL_PAD_CTRL_TRIANGLE);
        kbm_bind(VK_CAPITAL, CELL_PAD_CTRL_SQUARE); kbm_bind(VK_ADD, CELL_PAD_CTRL_SQUARE);
        kbm_bind(VK_LBUTTON, CELL_PAD_CTRL_R1);     kbm_bind(VK_RBUTTON, CELL_PAD_CTRL_L1);
        kbm_bind(VK_LCONTROL, CELL_PAD_CTRL_L1);    kbm_bind('P', CELL_PAD_CTRL_R1);
        kbm_bind(VK_LMENU, CELL_PAD_CTRL_L2);       kbm_bind(VK_RMENU, CELL_PAD_CTRL_R2);   kbm_bind('V', CELL_PAD_CTRL_R2);
        kbm_bind('J', CELL_PAD_CTRL_L3);            kbm_bind('X', CELL_PAD_CTRL_L3);   /* + R3 while grabbing (strangle = L3 + R3, v1130J below) */
        kbm_bind('F', CELL_PAD_CTRL_R3);            kbm_bind('Z', CELL_PAD_CTRL_RIGHT);
        kbm_bind(VK_LSHIFT, CELL_PAD_CTRL_CROSS);   kbm_bind(VK_RSHIFT, CELL_PAD_CTRL_CROSS);
        kbm_bind(VK_MBUTTON, CELL_PAD_CTRL_DOWN);
        kbm_bind('R', CELL_PAD_CTRL_UP);            kbm_bind('H', CELL_PAD_CTRL_DOWN);
        kbm_bind(VK_DECIMAL, CELL_PAD_CTRL_DOWN);
        kbm_bind(VK_NUMPAD0, CELL_PAD_CTRL_R1);     kbm_bind(VK_MULTIPLY, CELL_PAD_CTRL_L3);   /* v1130K EA PC */
        kbm_bind(VK_OEM_4, CELL_PAD_CTRL_LEFT);     kbm_bind(VK_OEM_6, CELL_PAD_CTRL_RIGHT);
        kbm_bind(VK_UP, CELL_PAD_CTRL_UP);          kbm_bind(VK_DOWN, CELL_PAD_CTRL_DOWN);
        kbm_bind(VK_LEFT, CELL_PAD_CTRL_LEFT);      kbm_bind(VK_RIGHT, CELL_PAD_CTRL_RIGHT);
        kbm_bind('1', CELL_PAD_CTRL_UP);            kbm_bind('2', CELL_PAD_CTRL_RIGHT);
        kbm_bind('3', CELL_PAD_CTRL_DOWN);          kbm_bind('4', CELL_PAD_CTRL_LEFT);
        kbm_bind(VK_ESCAPE, CELL_PAD_CTRL_START);   kbm_bind(VK_PAUSE, CELL_PAD_CTRL_SELECT);   kbm_bind('N', CELL_PAD_CTRL_SELECT);
    } else {               /* "Reborn": the port's own scheme */
        kbm_bind(VK_SPACE, CELL_PAD_CTRL_CROSS);    kbm_bind(VK_RETURN, CELL_PAD_CTRL_CROSS);
        kbm_bind('C', CELL_PAD_CTRL_CIRCLE);        kbm_bind(VK_BACK, CELL_PAD_CTRL_CIRCLE);
        kbm_bind('F', CELL_PAD_CTRL_SQUARE);        kbm_bind('E', CELL_PAD_CTRL_TRIANGLE);
        kbm_bind(VK_LBUTTON, CELL_PAD_CTRL_R1);     kbm_bind(VK_RBUTTON, CELL_PAD_CTRL_L1);
        kbm_bind('Q', CELL_PAD_CTRL_L2);            kbm_bind('R', CELL_PAD_CTRL_R2);
        kbm_bind('V', CELL_PAD_CTRL_L3);            /* v1043Y (user): Shift = hold to run (Cross), was L3 (still V) */
        kbm_bind(VK_LSHIFT, CELL_PAD_CTRL_CROSS);   kbm_bind(VK_RSHIFT, CELL_PAD_CTRL_CROSS);
        kbm_bind(VK_MBUTTON, CELL_PAD_CTRL_R3);     kbm_bind('Z', CELL_PAD_CTRL_R3);
        kbm_bind(VK_UP, CELL_PAD_CTRL_UP);          kbm_bind(VK_DOWN, CELL_PAD_CTRL_DOWN);
        kbm_bind(VK_LEFT, CELL_PAD_CTRL_LEFT);      kbm_bind(VK_RIGHT, CELL_PAD_CTRL_RIGHT);
        kbm_bind('1', CELL_PAD_CTRL_UP);            kbm_bind('2', CELL_PAD_CTRL_RIGHT);
        kbm_bind('3', CELL_PAD_CTRL_DOWN);          kbm_bind('4', CELL_PAD_CTRL_LEFT);
        kbm_bind(VK_ESCAPE, CELL_PAD_CTRL_START);   kbm_bind(VK_TAB, CELL_PAD_CTRL_SELECT);
    }
    const char* e = getenv("PAD_KBM_MAP");
    if (e && *e) {
        char buf[1024]; strncpy(buf, e, sizeof buf - 1); buf[sizeof buf - 1] = 0;
        for (char* t = strtok(buf, ",;"); t; t = strtok(NULL, ",;")) {
            while (*t == ' ') t++;
            char* eq = strchr(t, '='); if (!eq) continue; *eq = 0;
            char* k = t; char* b = eq + 1;
            for (char* z = k + strlen(k); z > k && z[-1] == ' '; ) *--z = 0;
            while (*b == ' ') b++;
            for (char* z = b + strlen(b); z > b && z[-1] == ' '; ) *--z = 0;
            int vk = kbm_key_vk(k); u16 btn;
            if (vk && kbm_btn(b, &btn)) kbm_bind(vk, btn);
            else printf("[cellPad] PAD_KBM_MAP: ignored '%s=%s'\n", k, b);
        }
    }
    printf("[cellPad] generic keyboard+mouse profile ON (%d bindings%s)\n", s_kbm_nbinds, (e && *e) ? ", PAD_KBM_MAP applied" : "");
    fflush(stdout);
}
static int pad_kbm_generic(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("PAD_KBM_PROFILE"); v = (e && !_stricmp(e, "generic")) ? 1 : 0; if (v) kbm_generic_init(); }
    return v;
}
/* ---- v1043T keyboard typing on Godfather's ENTER NAME screen (generic KBM profile) -------------------------------
 * The screen is an on-screen letter grid (EA APT movie "intKeyBoard"): rows a-j, k-t, u-z (columns 2-7 of the same
 * 10-column grid), then Space / Backspace / Done. It always opens with the cursor on 'a'. The title side
 * (gf_prompts.c) reports each opening through g_pad_namekb_seq_fn (a counter). While typing mode is on, physical
 * keys become the d-pad + Cross presses that walk the cursor to each character, using only transitions observed on
 * the real screen (soaks T02/T03): letter rows keep their column vertically (row1<->row2 only in columns 2-7),
 * row0 col1 UP = Space, Space RIGHT = Backspace, Backspace RIGHT = Done, Done LEFT = Backspace, Done DOWN = row0
 * col6, Backspace DOWN = row0 col5, row0 col6 UP = Done. Square = Caps Lock.
 * Keys: letters (Shift / Caps Lock = capitals), Space, Backspace, Enter = Done, Esc = Back. Every other keyboard
 * binding is off while typing. PAD_NAMEKB=0 = off; PAD_NAMEKB_STEP_MS (default 50) = press and release time. */
int (*volatile g_pad_namekb_seq_fn)(void) = 0;
/* v1043Y: title hook, nonzero = a menu is up over the game (Godfather: pause menu, gf_prompts.c) -> WASD = d-pad */
int (*volatile g_pad_menu_fn)(void) = 0;
int (*volatile g_pad_incar_fn)(void) = 0;
float (*volatile g_pad_car_speed_fn)(void) = 0;   /* v1130T gf_combat.c: player's car speed m/s, < 0 unknown */
int (*volatile g_pad_shop_fn)(void) = 0;   /* v1130Q gf_dialogue.c: 1 = the weapon dealer's buy screen is up, 2 = its Yes / No pop-up (v1130R) */
static int s_shop_popup = 0;
static int s_shop_ptr = 0;                  /* v1130Q: PC Classic buy screen with the mouse (free pointer) */
int (*volatile g_pad_gun_holstered_fn)(void) = 0;   /* v1053M: 1 = a gun is selected but holstered (gf_combat.c) */
static unsigned long long s_draw_req_ms = 0;   /* v1053N2: last draw request (H / d-pad UP keys) */   /* v1053V: 1 = player is in a vehicle (Godfather: gf_combat.c) */
/* v1044C: title hook for direct free-aim yaw (Godfather: gf_combat.c). Called while free aim is on with the mouse yaw of
 * this poll in the game's units (degrees); returns 1 = applied to the game's camera (no stick X), 2 = busy locating the
 * camera value (no stick X, mouse ignored), 0 = not available (stick as before). */
int (*volatile g_pad_aim_yaw_fn)(float deg) = 0;   /* v1044C (superseded by g_pad_aim_fn) */
/* v1044F: direct free-aim camera (Godfather gf_combat.c): yaw / pitch deltas in degrees from the mouse; 1 = applied to the
 * game's camera (stick X and Y held at 0), 0 = not available (stick as before). */
int (*volatile g_pad_aim_fn)(float dyaw, float dpitch) = 0;
/* v1046L GTA-style mouse look on foot (gf_footcam.c): mouse deltas in degrees go straight into the on-foot camera;
 * returns 1 while that camera is live (the mouse then does not drive the right stick). */
int (*volatile g_pad_look_fn)(float dyaw, float dpitch) = 0;
/* v1046V camera views (gf_footcam.c): V tap = next view (game / close / far / first person); V held >= PAD_KBM_V_HOLD_MS
 * (300) = L3 as before (strangle etc.). NULL = V is plain L3. */
void (*volatile g_pad_camcycle_fn)(void) = 0;
/* v1044J aim assist: title hook polled while the lock-on (L1) is held briefly on RMB press so the game picks a target;
 * arg 1 = acquisition starts, 0 = still acquiring; returns 1 once a target is captured. */
int (*volatile g_pad_aim_acq_fn)(int start) = 0;
volatile int g_pad_fa_act = 0;   /* v1044E: free aim on (diagnostics) */
void (*volatile g_pad_snapshot_fn)(void) = 0;   /* v1043N GF_MEMDIFF (gf_prompts.c) */
/* v1043T-3 (user: typing too slow -- 100 ms per cursor step): each step is now synced to the reports the game actually
 * receives (cellPadGetData port 0): hold until a delivered report carried the press, release until one carried the
 * release, then the next step -- one game frame each (~7-17 ms) instead of 50 + 50 ms. */
static volatile unsigned s_nk_rep_seq = 0;      /* delivered port-0 reports */
static volatile u16 s_nk_rep_btn = 0;           /* buttons in the last delivered report */
static volatile u16 s_nk_out = 0;               /* v1043T-4: the step being delivered */
/* v1043T-4 g_gf_turbo: queued name-entry steps pending -> the frame limiter does not wait (ppu_recomp_002.cpp patch),
 * the FIFO clock ticks at 500 Hz and nothing is presented (main.cpp, rsx_d3d12_backend.c): the screen keeps the last
 * picture and jumps to the result. */
volatile int g_gf_turbo = 0;
/* v1043W (user: "I type a word and it doesn't show up until I'm done"): the held picture only updated when the whole
 * queue was empty. Now each entered character bumps g_gf_show_req and rsx_d3d12_backend.c presents the next few frames
 * (PAD_NAMEKB_SHOW_FRAMES, default 3) -> every letter appears as it is typed; only the cursor walk stays hidden. */
volatile unsigned g_gf_show_req = 0;
static struct {
    int on, row, col, caps, last_seq;    /* row 3 = bottom buttons, col 0 Space / 1 Backspace / 2 Done */
    u16 q[512]; int qh, qt;
    unsigned long long t_next; int pressed;
    unsigned char prev[256];
} s_nk;
static void nk_push(u16 b) { if (((s_nk.qt + 1) & 511) != s_nk.qh) { s_nk.q[s_nk.qt] = b; s_nk.qt = (s_nk.qt + 1) & 511; } }
static void nk_leave_bottom(void)
{
    if (s_nk.row != 3) return;
    if (s_nk.col == 0) { nk_push(CELL_PAD_CTRL_RIGHT); s_nk.col = 1; }
    nk_push(CELL_PAD_CTRL_DOWN);
    s_nk.row = 0; s_nk.col = (s_nk.col == 2) ? 6 : 5;
}
static void nk_goto(int tr, int tc)
{
    nk_leave_bottom();
    if (s_nk.row == 2 && tr != 2) { nk_push(CELL_PAD_CTRL_UP); s_nk.row = 1; }
    while (s_nk.col < tc) { nk_push(CELL_PAD_CTRL_RIGHT); s_nk.col++; }
    while (s_nk.col > tc) { nk_push(CELL_PAD_CTRL_LEFT); s_nk.col--; }
    while (s_nk.row < tr) { nk_push(CELL_PAD_CTRL_DOWN); s_nk.row++; }
    while (s_nk.row > tr) { nk_push(CELL_PAD_CTRL_UP); s_nk.row--; }
}
static void nk_to_bottom(int item)   /* 0 Space, 1 Backspace, 2 Done */
{
    if (s_nk.row == 3 && item == 0 && s_nk.col != 0) nk_leave_bottom();      /* no observed Backspace -> Space move */
    if (s_nk.row != 3) {
        if (item == 2) { nk_goto(0, 6); nk_push(CELL_PAD_CTRL_UP); s_nk.row = 3; s_nk.col = 2; }
        else           { nk_goto(0, 1); nk_push(CELL_PAD_CTRL_UP); s_nk.row = 3; s_nk.col = 0; }
    }
    while (s_nk.col < item) { nk_push(CELL_PAD_CTRL_RIGHT); s_nk.col++; }
    if (s_nk.col == 2 && item == 1) { nk_push(CELL_PAD_CTRL_LEFT); s_nk.col = 1; }
}
static void nk_type_char(int ch, int upper) __attribute__((unused));
static void nk_type_char(int ch, int upper)
{
    int r, c;
    if (ch <= 'j') { r = 0; c = ch - 'a'; }
    else if (ch <= 't') { r = 1; c = ch - 'k'; }
    else { r = 2; c = ch - 'u' + 2; }
    if (upper != s_nk.caps) { nk_push(CELL_PAD_CTRL_SQUARE); s_nk.caps = upper; }
    nk_goto(r, c);
    nk_push(CELL_PAD_CTRL_CROSS);
}
/* ---- v1043G typing driven by the real cursor --------------------------------------------------------------------
 * rsx_d3d12_backend.c's grid probe reads the highlighted cell from the rendered frame (g_gf_grid_*). Typing mode is on
 * exactly while the grid is on screen. Keys and mouse clicks go into a CHARACTER queue; each character is planned from
 * the cursor position read back from a frame rendered after the previous character finished (no dead reckoning), then
 * its steps are handed to the game by cellPadGetData (2 whole frames per press / release) while g_gf_turbo runs the
 * menu uncapped and holds the picture. Keys: letters (Shift / Caps Lock = capitals), Space, Backspace / Delete
 * (deletes), Enter = Done, Esc = Back; left click on a letter / Space / Backspace / Done = that key. PAD_NAMEKB=0 = off. */
extern volatile int g_gf_grid_want, g_gf_grid_valid, g_gf_grid_row, g_gf_grid_col;
extern volatile unsigned g_gf_grid_seq;
enum { NKC_SPACE = 1, NKC_BACK = 2, NKC_DONE = 3, NKC_ESC = 4, NKC_CELL = 0x100, NKC_UPPER = 0x1000 };
static volatile int s_nkc_q[256]; static volatile int s_nkc_h = 0, s_nkc_t = 0;   /* poll thread -> GetData */
static volatile long s_nkc_lock = 0;   /* v1043V: window thread (WM_KEYDOWN) + poll thread (clicks, test text) */
static void nkc_push(int c)
{
    while (InterlockedExchange(&s_nkc_lock, 1)) YieldProcessor();
    { const int n = (s_nkc_t + 1) & 255; if (n != s_nkc_h) { s_nkc_q[s_nkc_t] = c; s_nkc_t = n; } }
    InterlockedExchange(&s_nkc_lock, 0);
}
static unsigned s_nk_need_seq = 0;   /* plan only when g_gf_grid_seq >= this */
static volatile unsigned long long s_nk_t_on = 0;   /* v1043G-2: when the grid appeared (screen fade-in ignores Cross) */
/* v1043V (user: fast typing "JohnnyHansen" -> "JohhnzHrsen"): the moves of a character and its Cross went out in one
 * burst, planned from a readback only 2 probe frames after the previous character -- with keys already queued the game
 * sometimes dropped / added a move (or the readback was still stale) and Cross landed on the wrong cell. Now each
 * character is: moves -> VERIFY (wait until the screen cursor is on the target cell; if it settles on another cell,
 * re-plan the moves from there) -> final press -> SETTLE (a few frames before the next input). Square (caps) also gets
 * a settle gap. Keys come from WM_KEYDOWN in typing order (rsx_d3d12_backend.c -> pad_namekb_wm_key). */
static volatile int s_nk_phase = 0;   /* 0 idle, 1 moves, 2 verify, 3 final press, 4 settle */
static int s_nk_fin = 0, s_nk_tr = -1, s_nk_tc = -1, s_nk_replans = 0, s_nk_cur = 0;
static volatile int s_nk_model_ok = 0;   /* s_nk.row/col confirmed by the screen */
static void nk_route(int tr, int tc) { if (tr < 3) nk_goto(tr, tc); else nk_to_bottom(tc); }
static void nk_plan(int c)
{
    if (!s_nk_model_ok && g_gf_grid_valid && g_gf_grid_row >= 0) { s_nk.row = g_gf_grid_row; s_nk.col = g_gf_grid_col; }
    s_nk_fin = CELL_PAD_CTRL_CROSS; s_nk_tr = -1; s_nk_tc = -1; s_nk_cur = c;
    if (c & NKC_CELL) { s_nk_tr = (c >> 4) & 15; s_nk_tc = c & 15; }
    else if (c == NKC_SPACE) { s_nk_tr = 3; s_nk_tc = 0; }
    else if (c == NKC_BACK)  { s_nk_tr = 3; s_nk_tc = 1; }
    else if (c == NKC_DONE)  { s_nk_tr = 3; s_nk_tc = 2; }
    else if (c == NKC_ESC)   { s_nk_fin = CELL_PAD_CTRL_CIRCLE; }
    else {
        const int ch = c & 0xFF;
        if (ch >= 'a' && ch <= 'z') {
            const int upper = (c & NKC_UPPER) ? 1 : 0;
            if (ch <= 'j') { s_nk_tr = 0; s_nk_tc = ch - 'a'; }
            else if (ch <= 't') { s_nk_tr = 1; s_nk_tc = ch - 'k'; }
            else { s_nk_tr = 2; s_nk_tc = ch - 'u' + 2; }
            if (upper != s_nk.caps) { nk_push(CELL_PAD_CTRL_SQUARE); s_nk.caps = upper; }
        } else s_nk_fin = 0;
    }
    if (s_nk_tr >= 0) nk_route(s_nk_tr, s_nk_tc);
}
/* window thread: WM_KEYDOWN while the name grid is on screen. 1 = consumed by typing. */
int pad_namekb_wm_key(unsigned vk, unsigned long lp)
{
    static int wm = -1; if (wm < 0) wm = pad_envi("PAD_NAMEKB_WM", 1);
    if (!s_nk.on || !wm || s_rr_mode) return 0;   /* v1129RR: record / replay sample the keys per tick */
    const int rep = (lp & (1ul << 30)) != 0;
    if (vk >= 'A' && vk <= 'Z') {
        if (rep) return 1;
        const int upper = ((GetKeyState(VK_SHIFT) & 0x8000) != 0) ^ ((GetKeyState(VK_CAPITAL) & 1) != 0);
        nkc_push((int)(vk - 'A' + 'a') | (upper ? NKC_UPPER : 0)); return 1;
    }
    switch (vk) {
    case VK_SPACE:  if (!rep) nkc_push(NKC_SPACE); return 1;
    case VK_BACK: case VK_DELETE: nkc_push(NKC_BACK); return 1;   /* held = repeat delete */
    case VK_RETURN: if (!rep) nkc_push(NKC_DONE); return 1;
    case VK_ESCAPE: if (!rep) nkc_push(NKC_ESC); return 1;
    default: return 0;
    }
}
/* poll thread: 1 = typing mode handled this poll */
static int pad_namekb_poll(PadHostState* hs, int merge, int focused)
{
    static int on = -1;
    if (on < 0) { on = pad_envi("PAD_NAMEKB", 1); }
    const int menus = !(g_pad_weapon_fn && g_pad_weapon_fn() >= 0);
    g_gf_grid_want = on && menus;
    if (!on) return 0;
    static unsigned long long t_gone = 0;
    const unsigned long long t = pad_now_ms();
    if (menus && g_gf_grid_valid) {
        t_gone = 0;
        if (!s_nk.on) {
            s_nk.caps = 0; s_nk.qh = s_nk.qt = 0; s_nk_out = 0; s_nkc_h = s_nkc_t = 0; s_nk_need_seq = 0; s_nk_t_on = t;
            s_nk_phase = 0; s_nk_model_ok = 0; s_nk.on = 1;
            for (int k = 0; k < 256; k++) s_nk.prev[k] = (GetAsyncKeyState(k) & 0x8000) ? 1 : 0;
            printf("[namekb] v1043G name grid on screen: keyboard typing + mouse clicks ON (Enter = Done, Esc = Back)\n"); fflush(stdout);
            const char* tst = getenv("PAD_NAMEKB_TEST");   /* test: type this text as if keyed in ('<' = Backspace, '>' = Done) */
            for (const char* c = tst; c && *c; c++) {
                if (*c >= 'a' && *c <= 'z') nkc_push(*c);
                else if (*c >= 'A' && *c <= 'Z') nkc_push((*c - 'A' + 'a') | NKC_UPPER);
                else if (*c == ' ') nkc_push(NKC_SPACE);
                else if (*c == '<') nkc_push(NKC_BACK);
                else if (*c == '>') nkc_push(NKC_DONE);   /* v1061A (agent WA): '>' = Done, unattended new-game soaks */
            }
        }
    } else if (s_nk.on) {
        if (!t_gone) t_gone = t;
        if (!menus || t - t_gone > 400) {
            s_nk.on = 0; s_nkc_h = s_nkc_t = 0; s_nk.qh = s_nk.qt = 0; s_nk_out = 0; g_gf_turbo = 0; s_nk_phase = 0; s_nk_model_ok = 0;
            printf("[namekb] v1043G name grid gone: typing OFF\n"); fflush(stdout);
        }
    }
    if (!s_nk.on) return 0;
    if (s_pad_clipped) { ClipCursor(NULL); s_pad_clipped = 0; }   /* free pointer for clicking */
    if (focused) {
        unsigned char cur[256];
        for (int k = 0; k < 256; k++) cur[k] = (GetAsyncKeyState(k) & 0x8000) ? 1 : 0;
        static int wm = -1; if (wm < 0) wm = pad_envi("PAD_NAMEKB_WM", 1) && !s_rr_mode;   /* v1043V: 1 = keys from WM_KEYDOWN (v1129RR: off while recording / replaying) */
        if (!wm) {   /* old 4 ms sampling: keys pressed in the same poll come out in A-Z order, fast double letters lost */
        const int upper = cur[VK_SHIFT] ^ ((GetKeyState(VK_CAPITAL) & 1) != 0);
        for (int k = 'A'; k <= 'Z'; k++) if (cur[k] && !s_nk.prev[k]) nkc_push((k - 'A' + 'a') | (upper ? NKC_UPPER : 0));
        if (cur[VK_SPACE]  && !s_nk.prev[VK_SPACE])  nkc_push(NKC_SPACE);
        if (cur[VK_BACK]   && !s_nk.prev[VK_BACK])   nkc_push(NKC_BACK);
        if (cur[VK_DELETE] && !s_nk.prev[VK_DELETE]) nkc_push(NKC_BACK);
        if (cur[VK_RETURN] && !s_nk.prev[VK_RETURN]) nkc_push(NKC_DONE);
        if (cur[VK_ESCAPE] && !s_nk.prev[VK_ESCAPE]) nkc_push(NKC_ESC);
        }
        if (cur[VK_LBUTTON] && !s_nk.prev[VK_LBUTTON]) {   /* click on a cell of the grid */
            HWND fg = GetForegroundWindow(); POINT p; RECT rc;
            if (fg && GetCursorPos(&p) && ScreenToClient(fg, &p) && GetClientRect(fg, &rc) && rc.right > 0 && rc.bottom > 0) {
                const float gx = (float)p.x * 1280.0f / (float)rc.right, gy = (float)p.y * 720.0f / (float)rc.bottom;
                int br = -1, bc = -1; float bd = 1e9f;
                for (int r = 0; r < 4; r++) {
                    const int c0 = (r == 2) ? 2 : 0, c1 = (r == 2) ? 7 : (r == 3) ? 2 : 9;
                    for (int c = c0; c <= c1; c++) {
                        float cx, cy;
                        if (r < 3) { cx = 373.0f + 59.9f * (float)c; cy = (r == 0) ? 393.0f : (r == 1) ? 451.0f : 509.0f; }
                        else { cx = (c == 0) ? 403.0f : (c == 1) ? 642.0f : 882.0f; cy = 567.0f; }
                        const float ax = (r == 3) ? 70.0f : 28.0f, dx = (gx - cx) / ax, dy = (gy - cy) / 26.0f;
                        const float d = dx * dx + dy * dy;
                        if (d < 1.0f && d < bd) { bd = d; br = r; bc = c; }
                    }
                }
                if (br >= 0) {
                    printf("[namekb] v1043G click -> %s\n", br < 3 ? "letter" : bc == 0 ? "Space" : bc == 1 ? "Backspace" : "Done"); fflush(stdout);
                    nkc_push(NKC_CELL | (br << 4) | bc);
                }
            }
        }
        memcpy(s_nk.prev, cur, sizeof cur);
    }
    (void)hs; (void)merge;
    pad_note_device(1);
    return 1;
}

static void pad_namekb_getdata_step(void)
{
    if (!s_nk.on) { if (g_gf_turbo) g_gf_turbo = 0; return; }
    extern unsigned cellGcm_flip_request_count(void);
    static int v_stable = -1, v_stable_ms = 60, v_max_ms = 1000, st_frames = 3, st_ms = 12, v_match = 2;
    if (v_stable < 0) {
        v_stable = pad_envi("PAD_NAMEKB_VSTABLE", 8); if (v_stable < 1) v_stable = 1;   /* same wrong cell N reads -> re-plan */
        v_stable_ms = pad_envi("PAD_NAMEKB_VSTABLE_MS", 60);
        v_max_ms = pad_envi("PAD_NAMEKB_VERIFY_MS", 1000);                             /* no answer at all -> press anyway */
        st_frames = pad_envi("PAD_NAMEKB_SETTLE_FRAMES", 3); st_ms = pad_envi("PAD_NAMEKB_SETTLE_MS", 12);
        /* user log 21:25: the cursor reached 'n' but 'h' was typed -- Cross accepted while the highlight was still moving */
        v_match = pad_envi("PAD_NAMEKB_VMATCH", 2); if (v_match < 1) v_match = 1;   /* fresh reads on the target before Cross */
    }
    static unsigned v_seq = 0, v_last = 0, st_f0 = 0; static unsigned long long v_t0 = 0, st_t0 = 0;
    static int mm_r = -9, mm_c = -9, mm_n = 0, ok_n = 0;
    const unsigned long long now_ms = pad_now_ms();
    const int q_idle = (s_nk.qh == s_nk.qt && !s_nk_out);
    { static int nlog = 0;
    if (s_nk_phase == 0) {
        /* v1043G-2: keys typed during the screen's fade-in wait (it ignores Cross then) */
        if (q_idle && s_nkc_h != s_nkc_t && g_gf_grid_valid && now_ms - s_nk_t_on >= 400ull) {
            const int c = s_nkc_q[s_nkc_h]; s_nkc_h = (s_nkc_h + 1) & 255;
            if (nlog < 80) { nlog++;
                printf("[namekb] v1043V char 0x%X: screen cursor r%d c%d, model r%d c%d (%s)\n", c, g_gf_grid_row, g_gf_grid_col,
                       s_nk.row, s_nk.col, s_nk_model_ok ? "confirmed" : "from screen"); fflush(stdout); }
            nk_plan(c); s_nk_replans = 0;
            if (!s_nk_fin) s_nk_phase = 0;
            else if (s_nk_tr < 0) { nk_push((u16)s_nk_fin); s_nk_phase = 3; }   /* Esc: nothing to verify */
            else s_nk_phase = 1;
        }
    } else if (s_nk_phase == 1) {
        if (q_idle) { s_nk_phase = 2; v_seq = g_gf_grid_seq + 2u; v_last = g_gf_grid_seq; v_t0 = now_ms; mm_n = 0; mm_r = mm_c = -9; ok_n = 0; }
    } else if (s_nk_phase == 2) {
        const unsigned sq = g_gf_grid_seq;
        if ((int)(sq - v_seq) >= 0 && sq != v_last && g_gf_grid_valid) {
            v_last = sq;
            const int gr = g_gf_grid_row, gc = g_gf_grid_col;
            if (gr == s_nk_tr && gc == s_nk_tc) { if (++ok_n >= v_match) { nk_push((u16)s_nk_fin); s_nk_phase = 3; s_nk_model_ok = 1; } }
            else {
                ok_n = 0;
                if (gr == mm_r && gc == mm_c) mm_n++; else { mm_r = gr; mm_c = gc; mm_n = 1; }
                if (mm_n >= v_stable && now_ms - v_t0 >= (unsigned long long)v_stable_ms) {
                    if (s_nk_replans < 4) {
                        s_nk_replans++;
                        if (nlog < 80) { nlog++;
                            printf("[namekb] v1043V char 0x%X: cursor at r%d c%d, wanted r%d c%d -> re-plan %d\n", s_nk_cur, gr, gc,
                                   s_nk_tr, s_nk_tc, s_nk_replans); fflush(stdout); }
                        s_nk.row = gr; s_nk.col = gc; nk_route(s_nk_tr, s_nk_tc); s_nk_phase = 1;
                    } else {
                        printf("[namekb] v1043V char 0x%X dropped: cursor stuck at r%d c%d (wanted r%d c%d)\n", s_nk_cur, gr, gc, s_nk_tr, s_nk_tc); fflush(stdout);
                        s_nk_model_ok = 0; s_nk_phase = 4; st_f0 = cellGcm_flip_request_count(); st_t0 = now_ms;
                    }
                }
            }
        }
        if (s_nk_phase == 2 && now_ms - v_t0 > (unsigned long long)v_max_ms) {
            printf("[namekb] v1043V char 0x%X: no cursor confirmation in %d ms, pressing anyway\n", s_nk_cur, v_max_ms); fflush(stdout);
            nk_push((u16)s_nk_fin); s_nk_phase = 3; s_nk_model_ok = 0;
        }
    } else if (s_nk_phase == 3) {
        if (q_idle) {
            s_nk_phase = 4; st_f0 = cellGcm_flip_request_count(); st_t0 = now_ms;
            if (s_nk_cur == NKC_DONE || s_nk_cur == NKC_ESC) s_nk_model_ok = 0;
            g_gf_show_req++;   /* v1043W: show this letter now */
        }
    } else if (s_nk_phase == 4) {
        if (cellGcm_flip_request_count() - st_f0 >= (unsigned)(st_frames < 4 ? 4 : st_frames) && now_ms - st_t0 >= (unsigned long long)st_ms) s_nk_phase = 0;   /* v1043W: >= 4 frames so the shown frames carry the letter */
    } }
    /* v1043T-6 (soak U06: at ~2 ms turbo frames the grid dropped a few moves, e.g. row -> bottom-row focus changes): each
     * press / release also lasts >= PAD_NAMEKB_MIN_MS (default 5) of real time -- the 7 ms frames of soak U02 never
     * dropped one. */
    /* v1043T-7 (soak U08: still drops -- the game reads the pad several times per frame, so a press and its release
     * could both fall inside one frame and the game, which keeps the last report, never saw the press): a press / release
     * changes only after the game completed >= PAD_NAMEKB_FRAMES (2) whole frames with it (guest flip count) and
     * >= PAD_NAMEKB_MIN_MS (2) ms. */
    static int min_ms = -1, nfr = 2; static unsigned long long t_phase = 0; static unsigned f_phase = 0;
    static unsigned f_extra = 0;   /* v1043V: extra frames after a Square (caps) release */
    if (min_ms < 0) { min_ms = pad_envi("PAD_NAMEKB_MIN_MS", 2); if (min_ms < 0) min_ms = 0;
                      nfr = pad_envi("PAD_NAMEKB_FRAMES", 2); if (nfr < 1) nfr = 1; }
    LARGE_INTEGER qf, qn; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qn);   /* GetTickCount64 is 15.6 ms coarse */
    const unsigned long long tn = (unsigned long long)(qn.QuadPart * 1000 / qf.QuadPart);
    const unsigned fc = cellGcm_flip_request_count();
    if (tn - t_phase >= (unsigned long long)min_ms + (f_extra ? (unsigned long long)st_ms : 0ull) && fc - f_phase >= (unsigned)nfr + f_extra) {
        if (s_nk_out) { f_extra = (s_nk_out & CELL_PAD_CTRL_SQUARE) ? (unsigned)st_frames : 0u; s_nk_out = 0; t_phase = tn; f_phase = fc; }   /* release */
        else if (s_nk.qh != s_nk.qt) { f_extra = 0; s_nk_out = s_nk.q[s_nk.qh]; s_nk.qh = (s_nk.qh + 1) & 511; t_phase = tn; f_phase = fc; }   /* press */
    }
    PadHostState* hs = &s_host_state[0];
    const u16 b = s_nk_out;
    hs->buttons = b; hs->connected = 1;
    hs->analog_lx = hs->analog_ly = hs->analog_rx = hs->analog_ry = 128; hs->trigger_l2 = hs->trigger_r2 = 0;
    hs->press_up = (b & CELL_PAD_CTRL_UP) ? 255 : 0;             hs->press_down = (b & CELL_PAD_CTRL_DOWN) ? 255 : 0;
    hs->press_left = (b & CELL_PAD_CTRL_LEFT) ? 255 : 0;         hs->press_right = (b & CELL_PAD_CTRL_RIGHT) ? 255 : 0;
    hs->press_triangle = 0;                                      hs->press_circle = (b & CELL_PAD_CTRL_CIRCLE) ? 255 : 0;
    hs->press_cross = (b & CELL_PAD_CTRL_CROSS) ? 255 : 0;       hs->press_square = (b & CELL_PAD_CTRL_SQUARE) ? 255 : 0;
    hs->press_l1 = hs->press_r1 = 0;
    s_data_fresh[0] = 1;                                          /* every read is a new report while typing */
    g_gf_turbo = (s_nk_phase != 0 || s_nk_out || s_nk.qh != s_nk.qt || (s_nkc_h != s_nkc_t && pad_now_ms() - s_nk_t_on >= 400ull)) ? 1 : 0;
}

/* v1107X/Y fast point-and-click in the game's menus + the character creator's sliders.
 * rsx_d3d12_backend.c reads the menu off the frame (g_gf_mp_*: item rows in 1280x720, which one is red = highlighted;
 * each item's slider track + knob). The pointer drives the game with d-pad pulses that cellPadGetData itself paces
 * (pad_menu_getdata_step: a press / release changes only after the game completed >= PAD_MENU_FRAMES (2) frames with it
 * and >= PAD_MENU_MIN_MS (6) ms) and counts (s_mm_pos_v / s_mm_pos_h).
 * v1107Y (user 20:07: "sliders' responsiveness kinda bad, bugs out when I move the mouse around the menu"; log: the
 * highlight trailed the pointer by 2-3 rows, the knob crawled one verified step at a time, the turbo picture hold
 * stuttered): PREDICTIVE. A model = last confirmed highlight / knob + pulses sent since; when the pointer target changes,
 * the difference goes out at once (pending pulses the wrong way are flushed). The probe only re-syncs the model when
 * nothing is moving (>= 2 analysed frames after the last edge, knob stopped). Slider step = learned from re-syncs
 * (default track / PAD_MENU_SLIDER_STEPS = 20, measured 18.7 px on FACE/HEAD). Turbo is off (PAD_MENU_TURBO=1 = on).
 * Sliders: pointer over a row = highlight it; LMB on the track = knob under the pointer, follows while held; LMB on the
 * arrows = one step (held: repeats); a click on a slider label only highlights. Other menus: a click = highlight + Cross;
 * a click where no menu is found = Cross. PAD_MENU_HOVER=0 = only clicks move.
 * v1107Z (user 20:16: "slider moves on its own", main menu "rolls through all the options" going back up to NEW GAME,
 * OPTIONS "really buggy"): highlights FADE and knobs GLIDE, and v1107Y re-synced its model mid-fade -> it thought a press
 * was lost, sent another, and on a wrapping menu one extra UP at the top lands on the bottom. Now a re-sync needs
 * PAD_MENU_SYNC_MS (150) without any edge AND 60 ms of an unchanged reading (the backend also publishes only readings seen
 * twice in a row). The knob is moved only while LMB is held on the track (no after-release convergence). OPTIONS-style
 * boxed lists: a click on a value word (Normal / Inverted, On / Off) selects it with left / right; a click on a row
 * label only highlights. A click with no menu found is a plain Cross only if no menu was seen for 500 ms. */
#define MP_MAXI 24
#define MP_MAXV 12   /* v1107Z4: = rsx_menu_probe_core.h */
#define MM_MASK ((u16)(CELL_PAD_CTRL_UP | CELL_PAD_CTRL_DOWN | CELL_PAD_CTRL_LEFT | CELL_PAD_CTRL_RIGHT | CELL_PAD_CTRL_CROSS))
static volatile u16 s_mm_out = 0;            /* pulse being delivered (getdata paced) */
static volatile u16 s_mm_hold = 0;
static volatile int s_mm_drive = 0;          /* the pointer owns d-pad + Cross */
static volatile int s_mm_idle = 1;           /* queue empty and the last release delivered */
static volatile int s_mm_flush = 0;          /* poll thread asks cellPadGetData to drop the queued pulses */
static volatile int s_mm_pos_v = 0, s_mm_pos_h = 0;   /* signed pulses pressed so far (down / right = +) */
static volatile unsigned long long s_mm_t_edge = 0;   /* last press / release edge */
static u16 s_mm_q[64]; static volatile unsigned s_mm_qh = 0, s_mm_qt = 0;   /* poll thread pushes (qt), cellPadGetData pops (qh) */
static volatile unsigned long long s_mm_t_burst = 0;
volatile int g_gf_menu_turbo = 0;
/* v1107Z5 (user 20:52: "make it move instantly instead of going down 1 at a time, pretty easy to see we're emulating a
 * controller"): while the pointer WALKS a highlight / value (not while dragging a knob or bar), g_gf_turbo is on -- the
 * game runs its frames back to back with one vblank each, nothing is presented (rsx_d3d12_backend.c present hold, as for
 * name typing), so the screen jumps straight to the result. At most PAD_MENU_TURBO_MAX_MS (500) per walk, then frames show
 * again. PAD_MENU_TURBO=0 = visible walking. */
static volatile int s_mm_walk = 0; static volatile unsigned long long s_mm_t_walk0 = 0;
volatile int g_gf_menu_fast = 0;              /* v1107X: menu probe every frame (rsx_d3d12_backend.c mp_copy) */
static void mm_push(u16 b, int n)
{
    if (n > 40) n = 40;
    for (int i = 0; i < n && s_mm_qt - s_mm_qh < 63u; i++) { s_mm_q[s_mm_qt & 63u] = b; MemoryBarrier(); s_mm_qt++; }
    s_mm_idle = 0; s_mm_t_burst = pad_now_ms(); s_mm_drive = 1;
}
static int mm_pending(int vert)
{
    int s = 0;
    for (unsigned i = s_mm_qh; i != s_mm_qt; i++) {
        const u16 b = s_mm_q[i & 63u];
        if (vert) { if (b == CELL_PAD_CTRL_UP) s--; else if (b == CELL_PAD_CTRL_DOWN) s++; }
        else      { if (b == CELL_PAD_CTRL_LEFT) s--; else if (b == CELL_PAD_CTRL_RIGHT) s++; }
    }
    return s;
}
static void pad_menu_reset(void)
{
    if (!s_mm_drive && s_mm_qh == s_mm_qt) return;
    s_mm_flush = 1; s_mm_out = 0; s_mm_hold = 0; s_mm_drive = 0; g_gf_menu_turbo = 0; g_gf_menu_fast = 0;
}
/* cellPadGetData port 0: delivers the queued pulses, one edge per >= PAD_MENU_FRAMES game frames */
static void pad_menu_getdata_step(void)
{
    extern unsigned cellGcm_flip_request_count(void);
    static int nfr = -1, min_ms = 6, min_ms_t = 2, turbo_on = 1, turbo_max = 500;
    if (nfr < 0) {
        nfr = pad_envi("PAD_MENU_FRAMES", 2); if (nfr < 1) nfr = 1;
        min_ms = pad_envi("PAD_MENU_MIN_MS", 6); if (min_ms < 0) min_ms = 0;
        min_ms_t = pad_envi("PAD_MENU_MIN_MS_TURBO", 2); if (min_ms_t < 0) min_ms_t = 0;
        turbo_on = pad_envi("PAD_MENU_TURBO", 1); turbo_max = pad_envi("PAD_MENU_TURBO_MAX_MS", 400);
    }
    if (s_mm_flush) { s_mm_qh = s_mm_qt; s_mm_flush = 0; }
    if (!s_mm_drive) { if (g_gf_menu_turbo) g_gf_menu_turbo = 0; if (!s_nk.on && g_gf_turbo) g_gf_turbo = 0; return; }
    static unsigned long long t_ph = 0; static unsigned f_ph = 0;
    LARGE_INTEGER qf, qn; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qn);
    const unsigned long long tn = (unsigned long long)(qn.QuadPart * 1000 / qf.QuadPart);
    const unsigned fc = cellGcm_flip_request_count();
    const int busy = s_mm_out || s_mm_qh != s_mm_qt;
    const int turbo = turbo_on && s_mm_walk && pad_now_ms() - s_mm_t_walk0 < (unsigned long long)turbo_max;
    const unsigned long long need_ms = (unsigned long long)(turbo ? min_ms_t : min_ms);
    /* v1114P (user: "the pause menu is fucked up, I can't use the cursor or the keyboard"): s_mm_drive was only cleared
     * when the menu closed, so after ONE pointer pulse (hover, wheel, click) the keyboard's / controller's d-pad and Cross
     * stayed masked out for as long as that menu was open. The pointer now gives them back once its queue is empty and
     * the last release is PAD_MENU_IDLE_MS (250) old; the next hover / click takes them again (mm_push). */
    { static int idle_ms = -1; if (idle_ms < 0) { idle_ms = pad_envi("PAD_MENU_IDLE_MS", 250); if (idle_ms < 0) idle_ms = 0; }
      if (!busy && !s_mm_hold && pad_now_ms() - s_mm_t_edge >= (unsigned long long)idle_ms && pad_now_ms() - s_mm_t_burst >= (unsigned long long)idle_ms) {
          s_mm_drive = 0; s_mm_idle = 1; g_gf_menu_turbo = 0; g_gf_menu_fast = 0; if (!s_nk.on && g_gf_turbo) g_gf_turbo = 0; return; } }
    if (tn - t_ph >= need_ms && fc - f_ph >= (unsigned)nfr) {
        if (s_mm_out) { s_mm_out = 0; t_ph = tn; f_ph = fc; s_mm_t_edge = pad_now_ms(); }                 /* release */
        else if (s_mm_qh != s_mm_qt) {                                                                    /* press */
            MemoryBarrier(); const u16 b = s_mm_q[s_mm_qh & 63u]; s_mm_qh++;
            if (b == CELL_PAD_CTRL_UP) s_mm_pos_v--; else if (b == CELL_PAD_CTRL_DOWN) s_mm_pos_v++;
            else if (b == CELL_PAD_CTRL_LEFT) s_mm_pos_h--; else if (b == CELL_PAD_CTRL_RIGHT) s_mm_pos_h++;
            s_mm_out = b; t_ph = tn; f_ph = fc; s_mm_idle = 0; s_mm_t_edge = pad_now_ms(); }
        else s_mm_idle = 1;
    }
    g_gf_menu_turbo = turbo;
    if (!s_nk.on) g_gf_turbo = turbo;
    PadHostState* hs = &s_host_state[0];
    const u16 b = (u16)((hs->buttons & ~MM_MASK) | s_mm_out | s_mm_hold);
    hs->buttons = b; hs->connected = 1;
    hs->press_up = (b & CELL_PAD_CTRL_UP) ? 255 : 0;       hs->press_down = (b & CELL_PAD_CTRL_DOWN) ? 255 : 0;
    hs->press_left = (b & CELL_PAD_CTRL_LEFT) ? 255 : 0;   hs->press_right = (b & CELL_PAD_CTRL_RIGHT) ? 255 : 0;
    hs->press_cross = (b & CELL_PAD_CTRL_CROSS) ? 255 : 0;
    s_data_fresh[0] = 1;
}
/* poll thread (4 ms): pointer + probe -> model -> pulses */
static void pad_menu_mouse(int lmb)
{
    extern volatile unsigned g_gf_mp_seq;
    extern volatile int g_gf_mp_n, g_gf_mp_sel, g_gf_mp_pitch, g_gf_mp_x0, g_gf_mp_x1, g_gf_mp_nsl, g_gf_mp_kw;
    extern volatile short g_gf_mp_y0[MP_MAXI], g_gf_mp_y1[MP_MAXI], g_gf_mp_ty[MP_MAXI], g_gf_mp_tx0[MP_MAXI], g_gf_mp_tx1[MP_MAXI], g_gf_mp_kx[MP_MAXI];
    extern volatile int g_gf_mp_box; extern volatile signed char g_gf_mp_nv[MP_MAXI], g_gf_mp_vsel[MP_MAXI];
    extern volatile short g_gf_mp_vx0[MP_MAXI][MP_MAXV], g_gf_mp_vx1[MP_MAXI][MP_MAXV];
    static int hold_move = 0, val_busy = 0; static unsigned long long t_hold = 0;
    static int sync_ms = -1, val_row = -1, val_idx = -1, o_sel = -9, o_n = -9, o_k = -9;
    static unsigned long long t_obs = 0, t_menu_seen = 0, t_val = 0;
    if (sync_ms < 0) sync_ms = pad_envi("PAD_MENU_SYNC_MS", 150);
    static int hover_on = -1, nsteps = 20, lmb_prev = 0, lx = -1, ly = -1;
    /* vertical model: highlight = v_off + pos_v; horizontal: knob px = h_k0 + (pos_h - h_p0) * step */
    static int v_ok = 0, v_off = 0, v_n = 0, h_ok = 0, h_row = -1, h_k0 = 0, h_p0 = 0, h_ks = -1, h_ps = 0;
    static double step = 0.0;
    static int drag = 0, g_row = -1, g_tgt = 0, click_row = -1, arrow_dir = 0, arrow_row = -1, arrow_held = 0, want_cross = 0;
    static unsigned long long t_moved = 0, t_seq = 0, t_click = 0, t_arrow = 0, t_act = 0, t_drag_end = 0;
    static unsigned seq_seen = 0, seq_quiet = 0, seq_kx = 0; static int quiet_on = 0, last_kx = -9999, arrow_sent = 0, arrow_first = 1;
    if (hover_on < 0) { hover_on = pad_envi("PAD_MENU_HOVER", 1); nsteps = pad_envi("PAD_MENU_SLIDER_STEPS", 20); if (nsteps < 2) nsteps = 20; }
    const unsigned long long now = pad_now_ms();
    int n = 0, sel = -1, pitch = 0, x0 = 0, x1 = 0, nsl = 0, kw = 0, box = 0; unsigned sq = 0;
    short y0[MP_MAXI], y1[MP_MAXI], ty[MP_MAXI], tx0[MP_MAXI], tx1[MP_MAXI], kx[MP_MAXI], vx0[MP_MAXI][MP_MAXV], vx1[MP_MAXI][MP_MAXV]; signed char nv[MP_MAXI], vsel[MP_MAXI];
    for (int tries = 0; tries < 4; tries++) {
        const unsigned a = g_gf_mp_seq; if (a & 1u) continue;
        MemoryBarrier();
        n = g_gf_mp_n; sel = g_gf_mp_sel; pitch = g_gf_mp_pitch; x0 = g_gf_mp_x0; x1 = g_gf_mp_x1; nsl = g_gf_mp_nsl; kw = g_gf_mp_kw; box = g_gf_mp_box;
        if (n > MP_MAXI) n = MP_MAXI; if (n < 0) n = 0;
        for (int i = 0; i < n; i++) { y0[i] = g_gf_mp_y0[i]; y1[i] = g_gf_mp_y1[i]; ty[i] = g_gf_mp_ty[i]; tx0[i] = g_gf_mp_tx0[i]; tx1[i] = g_gf_mp_tx1[i]; kx[i] = g_gf_mp_kx[i];
            nv[i] = g_gf_mp_nv[i]; vsel[i] = g_gf_mp_vsel[i]; for (int k = 0; k < MP_MAXV; k++) { vx0[i][k] = g_gf_mp_vx0[i][k]; vx1[i][k] = g_gf_mp_vx1[i][k]; } }
        MemoryBarrier();
        if (g_gf_mp_seq == a) { sq = a; break; }
        n = 0;
    }
    if (sq != seq_seen) { seq_seen = sq; t_seq = now; }
    const int fresh = now - t_seq < 400;
    if (kw <= 0) kw = 52;
    /* v1107Z2 (user 20:33 on OPTIONS: "it just moves around on its own"; log: the probe saw 4 rows, then 3 rows starting at
     * the boxed last row = "row 0" -> the model was 3 rows off and the wrapping list rolled): row numbers come from a
     * LAYOUT CACHE of the screen (row centres by y, same pitch / x / kind), merged over readings, so a reading that misses
     * rows cannot renumber them. Detected rows are re-indexed by their y; missing rows keep their cached position. */
    {
        static short L_cy[MP_MAXI]; static int L_n = 0, L_pitch = 0, L_x0 = 0, L_kind = -1; static unsigned long long t_bad = 0, t_gone = 0;
        const int kind = box ? 2 : (nsl > 0 ? 1 : 0);
        if (!fresh || n == 0) { if (!t_gone) t_gone = now; if (now - t_gone > 300) L_n = 0; }
        else {
            t_gone = 0;
            const int pt = pitch > 0 ? pitch : L_pitch;
            const int compat = L_n > 0 && kind == L_kind && abs(x0 - L_x0) <= 12 && (pitch <= 0 || L_pitch <= 0 || abs(pitch - L_pitch) <= 6);
            if (!compat) { if (!t_bad) t_bad = now; if (L_n == 0 || now - t_bad > 250) { L_n = 0; L_kind = kind; L_x0 = x0; L_pitch = pitch; t_bad = 0; } }
            else { t_bad = 0; if (pitch > 0) L_pitch = pitch; }
            if (L_n == 0 || compat) {
                const int tol = (pt > 0 ? pt : 30) / 3;
                for (int i = 0; i < n; i++) {
                    const int cy = (y0[i] + y1[i]) / 2; int f = -1;
                    for (int k = 0; k < L_n; k++) if (abs(L_cy[k] - cy) <= tol) { f = k; break; }
                    if (f < 0 && L_n < MP_MAXI) { int k = L_n++; while (k > 0 && L_cy[k - 1] > cy) { L_cy[k] = L_cy[k - 1]; k--; } L_cy[k] = (short)cy; }
                }
            }
        }
        if (fresh && n > 0 && L_n >= n && L_kind == kind) {   /* re-index the reading into the cached layout */
            short a_y0[MP_MAXI], a_y1[MP_MAXI], a_ty[MP_MAXI], a_tx0[MP_MAXI], a_tx1[MP_MAXI], a_kx[MP_MAXI], a_vx0[MP_MAXI][MP_MAXV], a_vx1[MP_MAXI][MP_MAXV];
            signed char a_nv[MP_MAXI], a_vsel[MP_MAXI]; int a_sel = -1, ok = 1;
            const int half = (L_pitch > 0 ? L_pitch : 30) / 2, tol = (L_pitch > 0 ? L_pitch : 30) / 3;
            for (int k = 0; k < L_n; k++) { a_y0[k] = (short)(L_cy[k] - half / 2); a_y1[k] = (short)(L_cy[k] + half / 2); a_ty[k] = a_tx0[k] = a_tx1[k] = a_kx[k] = -1; a_nv[k] = 0; a_vsel[k] = -1; }
            for (int i = 0; i < n && ok; i++) {
                const int cy = (y0[i] + y1[i]) / 2; int f = -1;
                for (int k = 0; k < L_n; k++) if (abs(L_cy[k] - cy) <= tol) { f = k; break; }
                if (f < 0) { ok = 0; break; }
                a_y0[f] = y0[i]; a_y1[f] = y1[i]; a_ty[f] = ty[i]; a_tx0[f] = tx0[i]; a_tx1[f] = tx1[i]; a_kx[f] = kx[i]; a_nv[f] = nv[i]; a_vsel[f] = vsel[i];
                for (int q = 0; q < MP_MAXV; q++) { a_vx0[f][q] = vx0[i][q]; a_vx1[f][q] = vx1[i][q]; }
                if (i == sel) a_sel = f;
            }
            if (ok) {
                n = L_n; sel = a_sel; if (L_pitch > 0) pitch = L_pitch;
                for (int k = 0; k < n; k++) { y0[k] = a_y0[k]; y1[k] = a_y1[k]; ty[k] = a_ty[k]; tx0[k] = a_tx0[k]; tx1[k] = a_tx1[k]; kx[k] = a_kx[k]; nv[k] = a_nv[k]; vsel[k] = a_vsel[k];
                    for (int q = 0; q < MP_MAXV; q++) { vx0[k][q] = a_vx0[k][q]; vx1[k][q] = a_vx1[k][q]; } }
            }
        }
    }
    if (fresh && n > 0) t_menu_seen = now;
    { const int k = (sel >= 0 && sel < n) ? kx[sel] : -1; if (sel != o_sel || n != o_n || k != o_k) { o_sel = sel; o_n = n; o_k = k; t_obs = now; } }
    int px = -1, py = -1;
    { HWND fg = GetForegroundWindow(); POINT p; RECT rc;
      if (fg && GetCursorPos(&p) && ScreenToClient(fg, &p) && GetClientRect(fg, &rc) && rc.right > 0 && rc.bottom > 0 &&
          p.x >= 0 && p.y >= 0 && p.x < rc.right && p.y < rc.bottom) { px = p.x * 1280 / rc.right; py = p.y * 720 / rc.bottom; } }
    if (px != lx || py != ly) { if (lx >= 0 && px >= 0) t_moved = now; lx = px; ly = py; }
    int hit = -1, on_track = 0, on_arrow = 0, on_value = -1;
    if (fresh && n > 0 && px >= 0) {
        for (int i = 0; i < n; i++) {
            if (nsl > 0 && ty[i] >= 0) {
                const int top = y0[i] - 6, bot = ty[i] + 16, l = (x0 < tx0[i] ? x0 : tx0[i]) - 40, r = (x1 > tx1[i] ? x1 : tx1[i]) + 40;
                if (py >= top && py < bot && px >= l && px <= r) {
                    hit = i;
                    if (py >= ty[i] - 10) {
                        if (px >= tx0[i] - 4 && px <= tx1[i] + 4) on_track = 1;
                        else if (px >= tx0[i] - 34 && px < tx0[i] - 4) on_arrow = -1;
                        else if (px > tx1[i] + 4 && px <= tx1[i] + 34) on_arrow = 1;
                    }
                    break;
                }
            } else if (px >= x0 - 40 && px <= x1 + 180) {
                const int cy = (y0[i] + y1[i]) / 2, half = pitch > 0 ? pitch / 2 : (y1[i] - y0[i]) / 2 + 8;
                if (py >= cy - half && py < cy + half) {
                    hit = i;
                    /* v1107Z4: the nearest value word / bar segment (gaps between segments count too) */
                    if (box && nv[i] > 0 && px >= vx0[i][0] - 20 && px <= vx1[i][nv[i] - 1] + 20) {
                        int bd = 1 << 30; for (int k = 0; k < nv[i] && k < MP_MAXV; k++) { const int c = (vx0[i][k] + vx1[i][k]) / 2, dd = abs(c - px); if (dd < bd) { bd = dd; on_value = k; } } }
                    break; }
            }
        }
    }
    const int edge = lmb && !lmb_prev, rel = !lmb && lmb_prev; lmb_prev = lmb;
    const int slider_menu = nsl > 0;
    /* --- re-sync the model when nothing moves --- */
    const int moving = s_mm_out || s_mm_qh != s_mm_qt || s_mm_flush;
    if (moving || now - s_mm_t_edge < 20) quiet_on = 0;
    else if (!quiet_on) { quiet_on = 1; seq_quiet = sq; }
    if (!fresh || n == 0) { v_ok = 0; h_ok = 0; }
    const int stable = quiet_on && (int)(sq - (seq_quiet + 4u)) >= 0 && now - s_mm_t_edge >= (unsigned long long)sync_ms && now - t_obs >= 60;
    if (!fresh || n == 0) { }
    else if (stable && sel >= 0) {   /* nothing pressed for PAD_MENU_SYNC_MS and the reading has settled (fades / glides) */
        if (!v_ok || v_n != n || v_off + s_mm_pos_v != sel) {
            if (v_ok && v_n == n && !box) { hold_move = 1; t_hold = now;   /* the model was wrong: no hover corrections until the pointer moves */
                static int nl = 0; if (nl < 60) { nl++; printf("[cellPad] v1107Z2 menu model %d corrected to %d (menu %d) -- hover paused until the mouse moves\n", v_off + s_mm_pos_v, sel, n); fflush(stdout); } }
            v_off = sel - s_mm_pos_v; v_n = n; v_ok = 1; }
        const int k = kx[sel];
        if (k >= 0 && ty[sel] >= 0) {
            if (1) {   /* the backend only publishes a knob seen twice; t_obs >= 60 ms = it has stopped */   /* knob stopped (it glides) */
                const int lo = tx0[sel] + kw / 2, hi = tx1[sel] - kw / 2;
                if (step <= 0.0) step = (double)(hi - lo) / (double)nsteps;
                const int dp = s_mm_pos_h - h_ps;
                if (h_ks >= 0 && h_row == sel && (dp >= 2 || dp <= -2) && h_ks > lo + 4 && h_ks < hi - 4 && k > lo + 4 && k < hi - 4 && k != h_ks) {
                    const double per = (double)(k > h_ks ? k - h_ks : h_ks - k) / (double)(dp > 0 ? dp : -dp);
                    if (per > step * 0.5 && per < step * 1.6) step = step * 0.6 + per * 0.4;
                }
                h_ks = k; h_ps = s_mm_pos_h;
                h_ok = 1; h_row = sel; h_k0 = k; h_p0 = s_mm_pos_h;
            }
            if (sq != seq_kx) { last_kx = k; seq_kx = sq; }
        } else { h_ok = 0; last_kx = -9999; }
    }
    const int same = v_ok && v_n == n;   /* item count flickered / another screen: no targeting until re-synced */
    /* --- pointer intent --- */
    if (edge) {
        t_act = now; t_drag_end = 0;   /* a new click ends the post-drag convergence */
        if (hit >= 0 && on_track) { drag = 1; g_row = hit; }
        else if (hit >= 0 && on_arrow) { arrow_dir = on_arrow; arrow_row = hit; arrow_held = 1; arrow_sent = 0; arrow_first = 1; t_arrow = now; g_row = hit; }
        else if (hit >= 0 && box && on_value >= 0) { g_row = hit; val_row = hit; val_idx = on_value; t_val = now; }
        else if (hit >= 0) { g_row = hit; if (!slider_menu && (!box || nv[hit] == 0)) { click_row = hit; t_click = now; } }   /* v1107Z4: box rows without values are pressed */
        else if ((!fresh || n == 0) && now - t_menu_seen > 500) mm_push(CELL_PAD_CTRL_CROSS, 1);   /* no menu at all: plain Cross */
    }
    if (rel) { if (drag) { t_drag_end = now; if (mm_pending(0)) s_mm_flush = 1; } drag = 0; arrow_held = 0; if (arrow_sent) arrow_dir = 0; }
    if (hold_move && t_moved > t_hold) hold_move = 0;
    /* v1107Z4: LMB held on a value row = drag along the bar: the target follows the nearest segment */
    if (lmb && !edge && val_row >= 0 && hit == val_row && on_value >= 0) { val_idx = on_value; t_val = now; }
    if (hold_move && click_row < 0 && val_row < 0 && !drag && !arrow_dir) g_row = -1;
    if (!lmb && hover_on && !hold_move && hit >= 0 && now - t_moved < 400 && click_row < 0 && val_row < 0) g_row = hit;   /* hover */
    if (val_row >= 0 && now - t_val > 2000) val_row = -1;
    if (click_row >= 0 && now - t_click > 1500) click_row = -1;
    if (drag && px >= 0 && g_row >= 0 && g_row < n && tx0[g_row] >= 0) {
        int lo = tx0[g_row] + kw / 2, hi = tx1[g_row] - kw / 2, t = px; if (t < lo) t = lo; if (t > hi) t = hi; g_tgt = t; }
    /* --- vertical: highlight g_row --- */
    int row_done = 0;
    /* v1107Z3 (user 20:43: OPTIONS "still moves on its own sometimes, only after I've moved"; log: "model 2 corrected to 0"
     * over and over -- the box SLIDES between rows and this screen drops / delays presses during the slide, so an open-loop
     * burst runs ahead of the screen): boxed lists are stepped CLOSED-LOOP -- one press, then wait until the reading shows
     * the box on the expected row (PAD_MENU_BOX_WAIT_MS, 450, then the press counts as lost and is re-planned from what
     * the screen shows). Never more than one press in flight, so nothing can arrive late. */
    /* after the box arrives, wait bx_settle (learned: +40 ms per lost press, up to 300; starts at PAD_MENU_BOX_SETTLE_MS 40)
     * so the next press does not land in the end of the slide */
    static int bx_wait = 0, bx_exp = -1, bx_settle = -1; static unsigned long long t_bx = 0, t_arr = 0; static int bx_ms = -1;
    if (bx_ms < 0) { bx_ms = pad_envi("PAD_MENU_BOX_WAIT_MS", 450); bx_settle = pad_envi("PAD_MENU_BOX_SETTLE_MS", 40); }
    static int z5_turbo = -1; if (z5_turbo < 0) z5_turbo = pad_envi("PAD_MENU_TURBO", 1);
    /* under turbo the game runs several frames per real ms: the settle gap shrinks (game time still passes) */
    const unsigned long long eff_settle = (unsigned long long)((z5_turbo && g_gf_menu_turbo) ? bx_settle / 4 : bx_settle);
    /* v1107Z6 (user 20:55 after Z5: "now the main menu is fucked"; log: "model 6 corrected to 0", "3 corrected to 4" -- under
     * turbo the main menu's fade timing no longer matched the open-loop bursts and one extra press wrapped it): EVERY list is
     * stepped closed-loop now (it is hidden by turbo anyway); it only ever steps toward the target, so it cannot wrap. */
    /* v1107Z8 (user 21:01: "everything is waaaay slower, in the main menu it has to go through every single one again"):
     * closed-loop + picture hold only for BOXED lists (OPTIONS / AUDIO, which drop presses while the box slides); the main
     * menu and other plain lists are back on the fast open-loop model of v1107Z4 (no turbo there). */
    if (box) {
        if (bx_wait && fresh && sel == bx_exp && !s_mm_out && s_mm_qh == s_mm_qt) { bx_wait = 0; t_arr = now; }
        else if (bx_wait && now - t_bx > (unsigned long long)bx_ms) {
            bx_wait = 0; t_arr = now;
            if (bx_settle < 300) bx_settle += 40;
            { static int nl = 0; if (nl < 40) { nl++; printf("[cellPad] v1107Z3 box list: press lost (box at %d, wanted %d) -> settle %d ms\n", sel, bx_exp, bx_settle); fflush(stdout); } }
        }
        if (!bx_wait && now - t_arr >= eff_settle && fresh && sel >= 0 && g_row >= 0 && g_row < n && !s_mm_out && s_mm_qh == s_mm_qt && !s_mm_flush) {
            if (sel != g_row) {
                const int d = g_row > sel ? 1 : -1;
                bx_exp = sel + d; bx_wait = 1; t_bx = now; t_act = now;
                mm_push(d > 0 ? CELL_PAD_CTRL_DOWN : CELL_PAD_CTRL_UP, 1);
            } else row_done = 1;
        }
        v_ok = 0;   /* the open-loop model is not used here */
    } else
    if (same && g_row >= 0 && g_row < n && !s_mm_flush) {
        const int pv = mm_pending(1), cur = v_off + s_mm_pos_v, end = cur + pv;
        if (end != g_row) {
            if (pv != 0) s_mm_flush = 1;   /* re-target: drop the queued rest, push next poll */
            else { const int d = g_row - cur; mm_push(d > 0 ? CELL_PAD_CTRL_DOWN : CELL_PAD_CTRL_UP, d > 0 ? d : -d); t_act = now; }
        } else if (pv == 0 && !s_mm_out) row_done = (sel == g_row);
    }
    /* reached: forget the hover target (so a wheel scroll is not pulled back) unless a drag / click / arrow needs it */
    if (row_done && !drag && click_row < 0 && val_row < 0 && !arrow_dir && now - t_moved >= 400) g_row = -1;
    /* click: Cross once the probe shows the clicked row highlighted and nothing is moving */
    if (click_row >= 0 && row_done && stable && sel == click_row && !s_mm_out && s_mm_qh == s_mm_qt) { mm_push(CELL_PAD_CTRL_CROSS, 1); click_row = -1; t_act = now; }
    /* value word / bar segment clicked (boxed list): v1107Z4 closed-loop like the rows -- one left / right press, wait until
     * the red (chosen) one is where expected (or PAD_MENU_BOX_WAIT_MS), settle, next; held LMB keeps following the pointer */
    { static int vw = 0, vexp = -1; static unsigned long long t_vw = 0, t_varr = 0;
      if (val_row >= 0 && row_done && sel == val_row && fresh && !s_mm_out && s_mm_qh == s_mm_qt) {
          const int cv = vsel[val_row];
          /* v1107Z9 (user 21:04: volume "doesn't go over instantly, choppy"): values change in ONE burst (all ticks at once),
           * checked once at the end; whatever is left (dropped ticks) is sent as the next burst. */
          static int vn = 0, lcv = -9; static unsigned long long t_cv = 0;
          if (cv != lcv) { lcv = cv; t_cv = now; }
          /* burst done: on target, or all ticks delivered and the bar unchanged for 60 ms (dropped ticks) -> re-plan the rest */
          if (vw && (cv == vexp || (s_mm_qh == s_mm_qt && !s_mm_out && now - s_mm_t_edge >= 60 && now - t_cv >= 60) ||
                     now - t_vw > (unsigned long long)(bx_ms + 30 * vn))) { vw = 0; t_varr = now; }
          if (!vw && now - t_varr >= eff_settle && cv >= 0) {
              if (cv == val_idx) { if (!lmb) val_row = -1; }
              else { const int d = val_idx - cv; vn = d > 0 ? d : -d; vexp = val_idx; vw = 1; t_vw = now; t_act = now; t_val = now;
                     mm_push(d > 0 ? CELL_PAD_CTRL_RIGHT : CELL_PAD_CTRL_LEFT, vn); }
          }
      } else if (val_row < 0) vw = 0;
      val_busy = vw || (val_row >= 0 && val_row < n && vsel[val_row] >= 0 && vsel[val_row] != val_idx); }
    /* --- horizontal: knob to the pointer (drag) or one arrow step --- */
    if (h_ok && h_row == g_row && fresh && sel == g_row && !bx_wait && mm_pending(1) == 0 && step > 0.0 && !s_mm_flush) {
        const int lo = tx0[h_row] + kw / 2, hi = tx1[h_row] - kw / 2, ph = mm_pending(0);
        if (drag) {   /* v1107Z: only while LMB is held on the track */
            double pred = (double)h_k0 + (double)(s_mm_pos_h + ph - h_p0) * step;
            static int end_done = 0;   /* +1 / -1: the extra end press was sent */
            const int zone = px >= hi - step * 0.5 ? 1 : (px <= lo + step * 0.5 ? -1 : 0);
            if (zone != end_done) {
                /* past an end the game ignored the presses: forget the excess so the way back counts right */
                const double pd = (double)h_k0 + (double)(s_mm_pos_h - h_p0) * step;   /* delivered presses only */
                if (pd > hi + step * 0.5) { const int ex = (int)((pd - hi) / step + 0.5); h_p0 += ex; pred -= ex * step; }
                if (pd < lo - step * 0.5) { const int ex = (int)((lo - pd) / step + 0.5); h_p0 -= ex; pred += ex * step; }
                if (zone == 0) end_done = 0;
            }
            if (pred < lo) pred = lo; if (pred > hi) pred = hi;
            const double e = (double)g_tgt - pred;
            int need = (int)(e / step + (e >= 0 ? 0.5 : -0.5));
            /* pointer at an end: one extra press (once) so the true min / max is reached even if the step is a bit off */
            if (zone && end_done != zone && (need == 0 || (need > 0) == (zone > 0)) && mm_pending(0) * zone >= 0) { need += zone; end_done = zone; }
            if (need != 0) {
                if (ph != 0 && ((need > 0) != (ph > 0))) s_mm_flush = 1;
                else { mm_push(need > 0 ? CELL_PAD_CTRL_RIGHT : CELL_PAD_CTRL_LEFT, need > 0 ? need : -need); t_act = now; }
            }
        }
    }
    if (arrow_dir) {   /* one step per click; held: repeats after 320 ms every 80 ms */
        const int ready = fresh && sel == arrow_row && !bx_wait && mm_pending(1) == 0 && !s_mm_out && s_mm_qh == s_mm_qt;
        if (ready && !arrow_sent) { mm_push(arrow_dir < 0 ? CELL_PAD_CTRL_LEFT : CELL_PAD_CTRL_RIGHT, 1); arrow_sent = 1; t_arrow = now; t_act = now; }
        else if (ready && arrow_held && now - t_arrow >= (arrow_first ? 320u : 80u)) {
            mm_push(arrow_dir < 0 ? CELL_PAD_CTRL_LEFT : CELL_PAD_CTRL_RIGHT, 1); arrow_first = 0; t_arrow = now; t_act = now; }
        if (!arrow_held && (arrow_sent || now - t_arrow > 1500)) arrow_dir = 0;
    }
    (void)want_cross;
    { const int mv = s_mm_out || s_mm_qh != s_mm_qt || s_mm_flush;
      if (!drag && click_row < 0 && val_row < 0 && !arrow_dir && !mv && now - s_mm_t_edge > 30) s_mm_drive = 0;
      else if (mv) s_mm_drive = 1; }
    /* v1107Z5 walking = a highlight or value is on its way to the target (not a drag / held bar) */
    { const int mv = s_mm_out || s_mm_qh != s_mm_qt;
      const int dragging = drag || (val_row >= 0 && lmb) || arrow_held;
      const int w = !dragging && fresh && n > 0 &&
                    ((g_row >= 0 && g_row < n && sel != g_row) || bx_wait || val_busy || (mv && !box));
      /* v1107Z7 (user 20:58: the picture "skips to the frame where the cursor is on what I selected" on every move): the hidden
       * walk only for walks of >= PAD_MENU_TURBO_MIN_STEPS (2) rows / values -- a 1-row hover move shows the game's own fade */
      static int w_prev = 0, w_tb = 0, min_steps = -1;
      if (min_steps < 0) min_steps = pad_envi("PAD_MENU_TURBO_MIN_STEPS", 2);
      if (w && !w_prev) {
          int dist = 0;
          if (g_row >= 0 && g_row < n && sel >= 0) dist = abs(g_row - sel);
          if (val_row >= 0 && val_row < n && vsel[val_row] >= 0) { const int dv = abs(val_idx - vsel[val_row]); if (dv > dist) dist = dv; }
          w_tb = dist >= min_steps; s_mm_t_walk0 = now;
      }
      w_prev = w;
      s_mm_walk = w && w_tb && box;   /* v1107Z8: picture hold only on boxed lists */
      if (w) s_mm_drive = 1; }
    g_gf_menu_fast = drag || moving || s_mm_drive || now - t_act < 600 || now - t_moved < 600;
    { static int lh = -2; if (edge || (hit != lh && hit >= 0)) { lh = hit;
        printf("[cellPad] v1107Z9 menu mouse: ptr %d,%d -> item %d%s%s (menu %d%s, highlight %d, model %s %d, sliders %d, step %.1f)%s\n", px, py, hit,
               on_track ? " track" : on_arrow ? " arrow" : "", on_value >= 0 ? " value" : "", n, box ? " boxed" : "", sel, v_ok ? "ok" : "--", v_off + s_mm_pos_v, nsl, step, edge ? " CLICK" : ""); fflush(stdout); } }
    { static int lsync = -99; if (h_ok && drag && h_k0 != lsync) { lsync = h_k0;
        printf("[cellPad] v1107Z slider row %d: knob %d (target %d, step %.1f)\n", h_row, h_k0, g_tgt, step); fflush(stdout); } }
}
/* v1107Z (user: "make it Q/E to change the tabs instead of the mouse buttons"): in menus Q = L1, E = R1 (tabs: GAME /
 * AUDIO / CONTROLS ...); rsx_d3d12_backend.c shows the menu glyph variant (L1 / R1 drawn as Q / E keys) while
 * g_gf_menu_glyphs is set. PAD_MENU_QE=0 = old binds. */
volatile int g_gf_menu_glyphs = 0;
static void pad_poll_kbm_generic_reborn(int merge)   /* v1130P "Reborn" scheme (unchanged) */
{
    PadHostState* hs = &s_host_state[0];
    static int s_walk = -1, s_dz = 30, s_inv = 0; static float s_speed = 1200.0f, s_tau = 0.060f;
    if (s_walk < 0) {
        /* v1115W2 (user: "can't walk with it toggled"): 64 is in Godfather's turn-on-the-spot band (0 m/s, gf_smoke.c
         * 86_t6_sweep); 90 = the ~1.6 m/s walk the smoking cap uses. */
        s_walk = pad_envi("PAD_KBM_WALK", 90); s_dz = pad_envi("PAD_MOUSE_DZ", 30); s_inv = pad_envi("PAD_MOUSE_INVERT_Y", 0);
        const char* e = getenv("PAD_MOUSE_SPEED"); if (e && *e) s_speed = (float)atof(e); if (s_speed < 50.0f) s_speed = 50.0f;
        s_tau = (float)pad_envi("PAD_MOUSE_SMOOTH_MS", 60) / 1000.0f; if (s_tau < 0.005f) s_tau = 0.005f;
    }
    static float s_vx = 0.0f, s_vy = 0.0f;        /* smoothed mouse velocity, px/s */
    static LARGE_INTEGER s_f, s_t; LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!s_f.QuadPart) { QueryPerformanceFrequency(&s_f); s_t = now; }
    double dt = (double)(now.QuadPart - s_t.QuadPart) / (double)s_f.QuadPart; s_t = now;
    if (dt <= 0.0) dt = 0.001; if (dt > 0.1) dt = 0.1;
    const long raw_dx = InterlockedExchange(&g_pad_raw_mdx, 0), raw_dy = InterlockedExchange(&g_pad_raw_mdy, 0);
    const long raw_wheel = InterlockedExchange(&g_pad_raw_wheel, 0);   /* v1107U: menu scroll */
    { const int foc = pad_host_window_focused();   /* v1043T-2: the typing queue also runs unfocused (soaks) */
      if (pad_namekb_poll(hs, merge, foc)) { g_pad_mouse_captured = 0; return; } }
    if (!pad_host_window_focused()) {
        g_pad_mouse_captured = 0; s_vx = s_vy = 0.0f;
        if (!merge) {
            hs->buttons = 0; hs->analog_lx = hs->analog_ly = hs->analog_rx = hs->analog_ry = 128;
            hs->trigger_l2 = hs->trigger_r2 = 0; hs->connected = 1;
        }
        return;
    }
#define KD(vk) ((GetAsyncKeyState(vk) & 0x8000) != 0)
    /* Esc toggles a free pointer (and is START); a click inside the window grabs the mouse again. */
    static int s_free = 0, s_esc_prev = 0, s_lmb_block = 0;
    { const int esc = KD(VK_ESCAPE);
      extern volatile int g_uiov_menu_open;
      if (esc && !s_esc_prev && !g_uiov_menu_open) s_free = !s_free;
      s_esc_prev = esc;
      if (s_free && KD(VK_LBUTTON)) {
          HWND fw = GetForegroundWindow(); POINT cp; RECT cr;
          if (fw && GetCursorPos(&cp) && ScreenToClient(fw, &cp) && GetClientRect(fw, &cr) &&
              cp.x >= 0 && cp.y >= 0 && cp.x < cr.right && cp.y < cr.bottom) { s_free = 0; s_lmb_block = 1; }
      }
      if (s_lmb_block && !KD(VK_LBUTTON)) s_lmb_block = 0; }
    /* v1110V (user: "I clicked on a cigar in my inventory and instead of smoking the shopkeeper thought I intimidated him" -- the log:
     * every click in the port's own pages (inventory / store / settings / trainer: g_uiov_menu_open) ALSO went to the game as R1 =
     * punches, the smoke stopped on the held button, and the shopkeeper next to him got beaten). While such a page is open the mouse
     * buttons do not reach the game, and a button held when it closes stays blocked until it is released. PAD_OV_MOUSE_BLOCK=0 = old. */
    static int s_ovb = 0; int ov_mouse = 0;
    { extern volatile int g_uiov_menu_open; static int on = -1; if (on < 0) on = pad_envi("PAD_OV_MOUSE_BLOCK", 1);
      const int anyb = KD(VK_LBUTTON) || KD(VK_RBUTTON) || KD(VK_MBUTTON);
      if (on) { if (g_uiov_menu_open && anyb) s_ovb = 1; if (!anyb) s_ovb = 0; ov_mouse = g_uiov_menu_open || s_ovb; } }
    /* v1042M motion key (PAD_MOTION_KEY, default X): hold it and tap W forward / S lift / A,D swing left,right /
     * Space slam down / E twist, or flick the mouse (forward = up, lift = down, left / right). While it is held
     * those keys and the mouse stop driving movement, the camera, Cross and Triangle. */
    static int s_mot_vk = -1; static int s_mprev[7];
    if (s_mot_vk < 0) { const char* e = getenv("PAD_MOTION_KEY"); s_mot_vk = (e && *e) ? kbm_key_vk(e) : 'X'; if (!s_mot_vk) s_mot_vk = 'X'; }
    const int mot = KD(s_mot_vk);
    u16 btns = 0;
    /* v1042X PC melee layout -- only with fists / a melee weapon in hand AND lock-on (RMB = L1) held:
     *   F      tap = punch (right stick up; with A / D held: up-left / up-right = hooks), hold (PAD_KBM_BLOCK_MS,
     *          180) = block (Square, F's normal button)
     *   R      hold = charge (right stick down), release = power attack / throw an explosive (right stick up)
     *   LMB    = R1 as always, so RMB+LMB = grab; while grabbing F = grab attacks,
     *   Q / E  (while grabbing) = swing left / right: the right stick held left / right plus the SIXAXIS swing
     *          gesture on the press (PAD_KBM_QE_MOTION=0: stick only)
     * While locked on the mouse does not drive the right stick (no accidental flicks). Gun in hand, no lock-on, or no
     * player (menus) = the normal mapping. PAD_KBM_COMBAT=0 (gf_combat.c) = off. The prompts say the same with the
     * KBM glyphs ([RDLeft]=F, [RSBottom]=R, [LSBottom]=Q, [RDUp]=E; gf_prompts_table.h). */
    const int cmb_gun = g_pad_weapon_fn ? g_pad_weapon_fn() : -1;
    /* v1107U menu pointer: in the game's menus (no player: frontend, creator, loading; or the pause menu) with the keyboard /
     * mouse in use, the mouse is not captured, so the pointer shows. LMB = Cross, RMB = Circle, wheel = d-pad up / down. */
    static int s_mcur_on = -1; if (s_mcur_on < 0) s_mcur_on = pad_envi("PAD_MENU_CURSOR", 1);
    static int s_menu_qe = -1; if (s_menu_qe < 0) s_menu_qe = pad_envi("PAD_MENU_QE", 1);
    const int mcur = s_mcur_on && g_pad_kbm_last && !s_free &&
                     ((g_pad_weapon_fn && cmb_gun < 0) || (g_pad_menu_fn && g_pad_menu_fn())) &&
                     !(g_pad_cutscene_fn && g_pad_cutscene_fn());
    { static int was = -1; if (mcur != was) { was = mcur; printf("[cellPad] v1107U menu pointer %s\n", mcur ? "ON" : "off"); fflush(stdout); } }
    /* v1044A (user: "free aim and aim assist like a Rockstar game", step 1): gun in hand -> hold RMB = the game's free-aim
     * crosshair (R3 toggles it; one R3 pulse on RMB press, one on release), mouse = crosshair, LMB = fire. The old lock-on
     * (L1) moves to T. Fists / melee keep RMB = lock-on (combat layout). PAD_KBM_FREEAIM=0 = old RMB = L1. */
    static int fa_on = -1; if (fa_on < 0) fa_on = pad_envi("PAD_KBM_FREEAIM", 1);
    /* v1119L (user: Monk's lesson in "Sleeping with the Fishes" can't be beaten -- "LOCK-ON: Hold L1" waits for the game's own
     * lock-on, s1_TLN_lock_shoot2, and RMB = free aim never gives it): while the lesson runs (g_gf_classic_aim, set from its
     * script events in gf_coop_mission.c) the PC free aim steps aside and the classic layout applies: RMB = L1 lock-on, the
     * mouse = right stick (adjusts the aim while locked: "Disarming"), LMB = R1, MMB / Z = R3 (the free-aim step). Switches
     * only with RMB up and free aim off, so the R3 toggle never desyncs. Log [aim] v1119L. */
    static int s_classic_eff = 0;
    if (g_gf_classic_aim != s_classic_eff && !s_fa_busy_pub && !KD(VK_RBUTTON)) {
        s_classic_eff = g_gf_classic_aim;
        printf("[aim] v1119L classic aiming %s (Monk's lesson)\n", s_classic_eff ? "ON: RMB = lock-on, MMB / Z = free aim" : "off: back to PC free aim"); fflush(stdout);
    }
    const int fa_now = fa_on && !s_classic_eff;
    const int fa_gun = fa_now && cmb_gun == 1 && !s_free;
    /* v1053M: gun selected but holstered + RMB = draw it (d-pad UP) and free-aim, never the game's L1 lock-on aim.
     * PAD_KBM_RMB_DRAW=0 = old (RMB = melee lock-on while the gun is away). */
    static int rmb_draw = -1; if (rmb_draw < 0) rmb_draw = pad_envi("PAD_KBM_RMB_DRAW", 1);
    /* v1053N2: only a gun the player is pulling out RIGHT NOW counts -- selected in the wheel within GF_GUN_INTENT_MS
     * (gf_combat.c returns 2) or a draw requested (H, d-pad UP keys) within PAD_KBM_DRAW_INTENT_MS (1500). A gun that
     * stays holstered keeps RMB = the melee lock-on. */
    static int draw_win = -1; if (draw_win < 0) draw_win = pad_envi("PAD_KBM_DRAW_INTENT_MS", 1500);
    const int hol = (rmb_draw && fa_now && !fa_gun && !s_free && !mot && g_pad_gun_holstered_fn) ? g_pad_gun_holstered_fn() : 0;
    if (hol && !KD(VK_RBUTTON) && (KD(VK_UP) || KD('1'))) s_draw_req_ms = pad_now_ms();
    static int s_gh_hold = 0;   /* keeps the decision for the whole RMB hold */
    if (!KD(VK_RBUTTON)) s_gh_hold = 0;
    else if (!s_gh_hold && hol) s_gh_hold = (hol == 2 || pad_now_ms() - s_draw_req_ms < (unsigned long long)draw_win) ? 1 : 2;
    const int gun_hol = hol && s_gh_hold == 1;
    const int cmb_fist = cmb_gun == 0 && !s_free && !mot && KD(VK_RBUTTON) && !gun_hol && !ov_mouse;   /* v1053M: not with a holstered gun; v1110V not in our pages */
    const int cmb_grab = cmb_fist && KD(VK_LBUTTON) && !s_lmb_block;
    static int s_fa_act = 0;   /* v1044B: free aim currently on (mouse sensitivity below) */
    static int s_lock_act = 0; /* v1044K: RMB lock-on phase (mouse does not drive the stick) */
    { extern volatile int g_pad_fa_act; g_pad_fa_act = s_fa_act; }   /* v1044E: ppu_loader [getstick-chain] tag */
    for (int i = 0; i < s_kbm_nbinds; i++) {
        const int vk = s_kbm_binds[i].vk;
        if (cmb_fist && (vk == 'F' || vk == 'R')) continue;
        if (cmb_grab && (vk == 'Q' || vk == 'E')) continue;
        if (mot && (vk == VK_SPACE || vk == 'E' || vk == 'W' || vk == 'A' || vk == 'S' || vk == 'D')) continue;
        if ((vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2) &&
            (s_free || mcur || ov_mouse || (vk == VK_LBUTTON && s_lmb_block))) continue;   /* v1110V ov_mouse */
        if ((fa_gun || gun_hol) && vk == VK_RBUTTON) continue;   /* v1044A: RMB = free aim with a gun (v1053M: also a holstered one) */
        if (vk == 'V' && g_pad_camcycle_fn) continue;   /* v1046V: V = camera view (tap) / L3 (hold), below */
        if (mcur && s_menu_qe && (vk == 'Q' || vk == 'E')) continue;   /* v1107Z: Q / E = tabs in menus, below */
        if (KD(vk)) btns |= s_kbm_binds[i].btn;
    }
    { extern volatile int g_gf_menu_want; g_gf_menu_want = mcur; }   /* v1107V: frame probe finds the menu */
    g_gf_menu_glyphs = mcur && s_menu_qe;
    if (mcur && s_menu_qe) { if (KD('Q')) btns |= CELL_PAD_CTRL_L1; if (KD('E')) btns |= CELL_PAD_CTRL_R1; }   /* v1107Z tabs */
    if (mcur) {   /* v1107U menu clicks; v1107V point-and-click */
        pad_menu_mouse(KD(VK_LBUTTON));   /* v1107X: pulses go out through cellPadGetData (pad_menu_getdata_step) */
        if (KD(VK_LBUTTON)) s_lmb_block = 1;   /* a click that closes the menu is not a shot */
        if (KD(VK_RBUTTON)) btns |= CELL_PAD_CTRL_CIRCLE;
        /* v1107X wheel: one d-pad pulse per notch through the same paced queue (was a 70 ms hold) */
        if (raw_wheel && !s_mm_drive) { const int notches = (int)((raw_wheel < 0 ? -raw_wheel : raw_wheel) + 119) / 120;
            mm_push(raw_wheel > 0 ? CELL_PAD_CTRL_UP : CELL_PAD_CTRL_DOWN, notches < 1 ? 1 : notches); }
    } else pad_menu_reset();
    if (gun_hol && KD(VK_RBUTTON) && !ov_mouse) {   /* v1053M: draw the selected gun; free aim follows (v1053L latch gap) */
        btns |= CELL_PAD_CTRL_UP;
        static unsigned long long nd = 0; const unsigned long long t = pad_now_ms();
        if (t - nd > 1000ull) { printf("[aim] v1053M RMB with a holstered gun -> drawing it (no lock-on), free aim follows\n"); fflush(stdout); }
        nd = t;
    }
    { static int vprev = 0, vhold = 0, hold_ms = -1; static unsigned long long vt = 0;
      if (hold_ms < 0) hold_ms = pad_envi("PAD_KBM_V_HOLD_MS", 300);
      if (g_pad_camcycle_fn) {
          const int v = KD('V'); const unsigned long long vn = pad_now_ms();
          if (v && !vprev) { vt = vn; vhold = 0; }
          if (v && !vhold && vn - vt >= (unsigned long long)hold_ms) vhold = 1;
          if (v && vhold) btns |= CELL_PAD_CTRL_L3;
          if (!v && vprev && !vhold) g_pad_camcycle_fn();
          vprev = v;
      } }
    { static int fa_state = 0; static unsigned long long fa_pulse_end = 0, fa_last = 0;
      /* v1044K (user: "a combination of the two original modes"): RMB = the game's own lock-on (L1 held) -- it snaps
       * to an NPC exactly as before; as soon as the mouse moves PAD_KBM_LOCK_BREAK (30) counts, or no target appears
       * within PAD_KBM_LOCK_NOTARGET_MS (200), L1 is released and free aim starts (R3) facing the lock target
       * (gf_combat.c aligns the free-aim camera); RMB release ends either. While locked the mouse does not move the
       * stick (no zone-target flicks). PAD_KBM_ASSIST=0 = straight to free aim (v1044B). */
      /* v1044L (user: "doesn't snap reliably"): the mouse broke the lock before the game had picked a target. Now the
       * lock-on gets PAD_KBM_LOCK_MIN_MS (250) untouched; mouse movement only counts after that (break 40 counts);
       * no target -> free aim after PAD_KBM_LOCK_NOTARGET_MS (350). */
      /* v1044Q (user: "lock to the NPC like originally, but WAY easier to free aim from there; no one to aim at -> free
       * aim"): RMB holds the game's lock-on (L1). gf_combat.c clears the stale lock-target pointer on press, so "has a
       * target" is real now (v1044K-N saw the last NPC forever -> RMB did nothing without anyone around). No target within
       * PAD_KBM_LOCK_NOTARGET_MS (150) -> L1 is released and free aim starts a beat later (mode 4 -> 2). Locked: counted
       * from PAD_KBM_LOCK_MIN_MS (80) after the target appeared, PAD_KBM_LOCK_BREAK (12) mouse counts break to free aim
       * on that target (v1044M R3-while-locked handoff). PAD_KBM_ASSIST=0 = straight to free aim. */
      static int mode = 0, brk = -1, nt_ms = 150, asst_on = 1, min_ms = 80; static unsigned long long lock_t0 = 0, has_t = 0, gap_end = 0, lost_t = 0; static long acc = 0;
      static int hand_ms = -1, retries = 0, retry_on = 0, gap_ms = -1; static unsigned long long handoff_end = 0, verify_at = 0;
      if (hand_ms < 0) hand_ms = pad_envi("PAD_KBM_LOCK_HANDOFF_MS", 150);
      if (gap_ms < 0) gap_ms = pad_envi("PAD_KBM_LOCK_GAP_MS", 120);
      if (brk < 0) { brk = pad_envi("PAD_KBM_LOCK_BREAK", 12); nt_ms = pad_envi("PAD_KBM_LOCK_NOTARGET_MS", 150);
                     min_ms = pad_envi("PAD_KBM_LOCK_MIN_MS", 80);
                     /* v1044N (user: "nothing happens unless there's an NPC around", "free aim even more unreliable"):
                      * the lock-first hybrid held L1 with a dead mouse until a target / 350 ms / 40 counts -- with no NPC
                      * (or a stale target in player+0x1544) the press did nothing. And the R3 retry fired while free aim
                      * WAS on (its camera just not found yet), toggling the game OFF while we thought it was ON, so every
                      * later R3 pulse was inverted. Now: RMB = free aim at once (the v1044F mouse path the user liked),
                      * no retry. PAD_KBM_ASSIST=1 brings the lock-first hybrid back, PAD_KBM_FA_RETRY=1 the retry. */
                     asst_on = pad_envi("PAD_KBM_ASSIST", 0);   /* v1044T: RMB = free aim; the lock-on is done inside free aim by gf_combat.c */ retry_on = pad_envi("PAD_KBM_FA_RETRY", 0);
                     printf("[aim] v1044T RMB = %s, R3 retry %s\n", asst_on ? "game lock-on then free aim (PAD_KBM_ASSIST=1)" : "free aim + lock on the NPC at the crosshair",
                            retry_on ? "on" : "off"); fflush(stdout); }
      const unsigned long long ft = pad_now_ms();
      /* v1049F (user: "melee lock-on, then switching to a gun messes up free aim"): an RMB hold that began without a gun
       * (melee = the game's L1 lock-on) must not start free aim when the gun comes out mid-hold -- the R3 then lands
       * while the game is still locked on, cancels instead of starting the aim, and fa_state is inverted from then on
       * (aims on release). Such a hold is ignored until RMB is released; the next press is clean. PAD_KBM_FA_LATCH=0 off. */
      static int rmb_latch = 0, latch_on = -1; if (latch_on < 0) latch_on = pad_envi("PAD_KBM_FA_LATCH", 1);
      /* v1053L (user: "target someone in melee, then pull out a pistol and free aim -> it doesn't go over to free aim, I
       * have to press RMB again"): once the gun is out the RMB -> L1 bind is skipped (fa_gun), so the melee lock-on is
       * already let go; after PAD_KBM_FA_LATCH_GAP_MS (250) without L1 the latch clears and the held RMB starts free aim
       * (the same L1-off -> gap -> R3 order as the v1044S lock -> free-aim switch). 0 = old: press RMB again. */
      static unsigned long long latch_gun_t = 0; static int latch_gap = -1;
      if (latch_gap < 0) latch_gap = pad_envi("PAD_KBM_FA_LATCH_GAP_MS", 250);
      if (!KD(VK_RBUTTON)) { rmb_latch = 0; latch_gun_t = 0; }
      else if (latch_on && !fa_gun && !rmb_latch) { rmb_latch = 1; }
      if (rmb_latch && fa_gun && latch_gap > 0) {
          if (!latch_gun_t) { latch_gun_t = ft; printf("[aim] v1053L gun out during a melee RMB hold -- free aim in %d ms\n", latch_gap); fflush(stdout); }
          else if (ft - latch_gun_t >= (unsigned long long)latch_gap) { rmb_latch = 0; latch_gun_t = 0; }
      } else if (!rmb_latch) latch_gun_t = 0;
      { static int lp = 0; if (rmb_latch && fa_gun && !lp && latch_gap <= 0) { printf("[aim] v1049F RMB held from melee -- release and press again for free aim\n"); fflush(stdout); } lp = rmb_latch && fa_gun; }
      /* v1049S (user: "aim, run, let go of RMB, stop running -> still aiming"; "no free aim unless I hold RMB"): sprinting
       * out of free aim desynced the R3 toggle (the game pauses free aim while sprinting and ignores R3 then). Now Shift
       * (sprint) while RMB is held first ENDS free aim (R3, run withheld until the pulse is through), then sprints;
       * releasing Shift with RMB still held aims again. Free aim = RMB held and not sprinting. PAD_KBM_FA_SPRINT=0 = off. */
      static int sp_on = -1; if (sp_on < 0) sp_on = pad_envi("PAD_KBM_FA_SPRINT", 1);
      const int fa_sprint = sp_on && (KD(VK_LSHIFT) || KD(VK_RSHIFT));
      const int rmb = fa_gun && KD(VK_RBUTTON) && !rmb_latch && !fa_sprint && !ov_mouse;   /* v1110V */
      s_fa_busy_pub = fa_state;   /* v1119L (last poll's state) */
      if (!fa_now || cmb_gun != 1) { fa_state = 0; mode = 0; }  /* gun put away / swapped: the game leaves free aim itself */
      else {
          if (mode == 0 && rmb) {
              if (asst_on && g_pad_aim_acq_fn) { mode = 1; lock_t0 = ft; has_t = 0; lost_t = 0; acc = 0; g_pad_aim_acq_fn(1);
                  printf("[aim] v1044K RMB: lock-on\n"); fflush(stdout); }
              else { mode = 2; if (g_pad_aim_acq_fn) { g_pad_aim_acq_fn(1); g_pad_aim_acq_fn(2); } }   /* v1044N: clear any old target first */
          }
          if (mode == 1) {
              if (!rmb) mode = 0;
              else {
                  const int has = g_pad_aim_acq_fn ? g_pad_aim_acq_fn(0) : 0;
                  if (has && !has_t) { has_t = ft; printf("[aim] v1044Q locked on after %llu ms\n", ft - lock_t0); fflush(stdout); }
                  /* v1044R: once locked, the mouse keeps counting even if the target drops (dies / leaves range) --
                   * v1044Q stopped counting then and the player was stuck in lock mode with a dead mouse */
                  const int settled = has_t && ft - has_t >= (unsigned long long)min_ms;
                  if (has) lost_t = 0; else if (has_t && !lost_t) lost_t = ft;
                  if (settled) acc += (raw_dx < 0 ? -raw_dx : raw_dx) + (raw_dy < 0 ? -raw_dy : raw_dy); else acc = 0;
                  if (settled && acc >= brk) {
                      /* v1044S (user: "locked on, try to free aim -> he just stops aiming; let go of RMB -> he starts
                       * aiming until I press Z, reversed"): R3 pressed WHILE L1 is held cancels the aim in this game, but
                       * fa_state was set to 1, so the release pulse then turned free aim ON. Now the lock -> free aim
                       * switch is the same sequence as the no-target case, which the logs show working: let go of L1,
                       * wait PAD_KBM_LOCK_GAP_MS (120), then the normal R3 pulse (mode 4 -> 2). The snap still aims the
                       * new free-aim camera at the locked target (g_pad_aim_acq_fn(2) keeps it). */
                      mode = 4; gap_end = ft + (unsigned long long)gap_ms; g_pad_aim_acq_fn(2);
                      printf("[aim] v1044S -> free aim from the lock (mouse moved): L1 off, R3 in %d ms\n", gap_ms); fflush(stdout);
                  } else if ((!has_t && ft - lock_t0 >= (unsigned long long)nt_ms) ||
                             (lost_t && ft - lost_t >= (unsigned long long)nt_ms)) {   /* v1044R: lock lost -> free aim too */
                      /* nobody to lock on to: let go of L1, free aim a beat later (mode 4 -> 2) */
                      mode = 4; gap_end = ft + (unsigned long long)gap_ms; g_pad_aim_acq_fn(2);
                      printf("[aim] v1044Q no target -> free aim\n"); fflush(stdout);
                  }
              }
          }
          if (mode == 4) {
              if (!rmb) mode = 0;
              else if (ft >= gap_end) mode = 2;
          }
          if (mode == 3) {
              if (!rmb) mode = 0;
              else if (ft >= handoff_end) { mode = 2; verify_at = ft + 400ull; retries = 0; }
          }
          /* v1044M: free aim not up 400 ms after the handoff (no free-aim camera) -> the R3 did not take: press it once more */
          if (retry_on && mode == 2 && rmb && fa_state && retries < 1 && ft >= verify_at && g_pad_aim_acq_fn && !g_pad_aim_acq_fn(3) &&
              ft - fa_last >= 180ull) {
              retries++; fa_last = ft; fa_pulse_end = ft + 90ull; verify_at = ft + 400ull;
              printf("[aim] v1044M free aim did not start -> R3 again\n"); fflush(stdout);
          }
          if (mode == 2 && !rmb) mode = 0;
          const int want = (mode == 2 || mode == 3);   /* mode 4 (gap after a no-target L1) keeps the current state */
          if (want != fa_state && ft - fa_last >= 180ull) {   /* toggle the game's free aim with a short R3 pulse */
              fa_state = want; fa_last = ft; fa_pulse_end = ft + 90ull;
              printf("[aim] v1044A free aim %s (R3 pulse)%s\n", want ? "ON" : "OFF", (!want && fa_sprint) ? " -- sprint" : ""); fflush(stdout);
          }
          /* v1049S: no run until the game has left free aim (R3 through + 150 ms) */
          if (fa_sprint && (fa_state || ft < fa_pulse_end + 150ull)) btns &= ~CELL_PAD_CTRL_CROSS;
      }
      if (mode == 1 || mode == 3) btns |= CELL_PAD_CTRL_L1;
      s_lock_act = (mode == 1 || mode == 3 || mode == 4);   /* v1044S: no camera turn during the L1 -> R3 gap either */
      if (ft < fa_pulse_end) btns |= CELL_PAD_CTRL_R3;
      { static int gl = -1; if (gl < 0) gl = pad_envi("PAD_KBM_GUN_LOCK", 0);   /* v1053F (user): no game lock-on with a gun */
        if (gl && fa_now && cmb_gun == 1 && KD('T')) btns |= CELL_PAD_CTRL_L1; }   /* v1044A: T = old lock-on (PAD_KBM_GUN_LOCK=1) */
      s_fa_act = fa_state; }
    int cmb_rs = 0; float cmb_rx = 0.0f, cmb_ry = 0.0f;
    { static int block_ms = -1, flick_ms = 90, power_ms = 140, qe_mot = 1;
      if (block_ms < 0) { block_ms = pad_envi("PAD_KBM_BLOCK_MS", 180); flick_ms = pad_envi("PAD_KBM_FLICK_MS", 90);
                          power_ms = pad_envi("PAD_KBM_POWER_MS", 140); qe_mot = pad_envi("PAD_KBM_QE_MOTION", 1); }
      static int fst = 0, rst = 0, pq = 0, pe = 0;   /* fst: 0 idle, 1 F down (punch or block?), 2 blocking; rst: R charging */
      static unsigned long long f_t0 = 0, fl_end = 0; static float fl_x = 0.0f, fl_y = 0.0f, p_x = 0.0f, p_y = -127.0f;
      const unsigned long long t = pad_now_ms();
      if (cmb_fist) {
          static int qe_keys = -1; if (qe_keys < 0) qe_keys = pad_envi("PAD_KBM_QE", 0);   /* v1043K: swings = mouse drag */
          const int f = KD('F'), r = KD('R'), q = qe_keys && cmb_grab && KD('Q'), e = qe_keys && cmb_grab && KD('E');
          const int kl = KD('A'), kr = KD('D');
          if (f && fst == 0) { fst = 1; f_t0 = t; const int side = kl && !kr ? -1 : kr && !kl ? 1 : 0;
                               p_x = side ? (float)side * 90.0f : 0.0f; p_y = side ? -90.0f : -127.0f; }
          if (fst == 1 && f && t - f_t0 >= (unsigned long long)block_ms) fst = 2;
          if (fst == 1 && !f) { fl_end = t + (unsigned long long)flick_ms; fl_x = p_x; fl_y = p_y; fst = 0; }
          if (fst == 2) { if (f) btns |= CELL_PAD_CTRL_SQUARE; else fst = 0; }
          if (r) rst = 1;
          else if (rst) { rst = 0; fl_end = t + (unsigned long long)power_ms; fl_x = 0.0f; fl_y = -127.0f; }
          if (qe_mot && q && !pq) pad_motion_trigger(MOT_LEFT);
          if (qe_mot && e && !pe) pad_motion_trigger(MOT_RIGHT);
          pq = q; pe = e;
          if (rst) { cmb_rs = 1; cmb_rx = 0.0f; cmb_ry = 127.0f; }                     /* pull back = charge */
          else if (q != e) { cmb_rs = 1; cmb_rx = q ? -127.0f : 127.0f; cmb_ry = 0.0f; } /* swing while grabbing */
          else if (t < fl_end) { cmb_rs = 1; cmb_rx = fl_x; cmb_ry = fl_y; }            /* punch */
          else cmb_rs = 2;                                                             /* centred, mouse muted */
      } else { fst = rst = pq = pe = 0; fl_end = 0; }
    }
    /* v1042G layout (matches the rewritten prompts, gf_prompts.c): X+W forward, X+S towards you, X+A / X+D left / right,
     * X+SPACE up (lift while grabbing = LMB/R1 held, otherwise rotate up), X+CTRL down and in (with A / D: down-left /
     * down-right), X+E twist. */
    { const int cur[7] = { KD('W'), KD('S'), KD('A'), KD('D'), KD(VK_SPACE), KD('E'), KD(VK_LCONTROL) || KD(VK_RCONTROL) };
      const int grab = ((btns | (merge ? hs->buttons : 0)) & CELL_PAD_CTRL_R1) != 0;
      /* v1043K (user): while grabbing, a plain TAP of X (no other gesture key during the hold, released within
       * PAD_KBM_XLIFT_MS = 500 ms) = lift / pick up (was X+SPACE, which still works). Fired on release so X+W / X+A ...
       * do not lift first. PAD_KBM_XLIFT=0 = off. */
      { static int xl_on = -1, xl_ms = 500; static int x_prev = 0, x_used = 0, x_grab = 0; static unsigned long long x_t0 = 0;
        if (xl_on < 0) { xl_on = pad_envi("PAD_KBM_XLIFT", 1); xl_ms = pad_envi("PAD_KBM_XLIFT_MS", 500); }
        const unsigned long long xt = pad_now_ms();
        if (mot && !x_prev) { x_t0 = xt; x_used = 0; x_grab = 0; }
        if (mot) { for (int i = 0; i < 7; i++) if (cur[i]) x_used = 1; if (grab) x_grab = 1; }
        if (!mot && x_prev && xl_on && x_grab && grab && !x_used && xt - x_t0 <= (unsigned long long)xl_ms) {
            pad_motion_trigger(MOT_LIFT);
            printf("[motion] v1043K X tap while grabbing -> lift\n"); fflush(stdout);
        }
        x_prev = mot; }
      if (mot) {
          const int ctrl = cur[6];
          if (cur[0] && !s_mprev[0]) pad_motion_trigger(MOT_FORWARD);
          if (cur[1] && !s_mprev[1]) pad_motion_trigger(MOT_BACK);
          if (cur[2] && !s_mprev[2]) pad_motion_trigger(ctrl ? MOT_KNEE_L : MOT_LEFT);
          if (cur[3] && !s_mprev[3]) pad_motion_trigger(ctrl ? MOT_KNEE_R : MOT_RIGHT);
          if (cur[4] && !s_mprev[4]) pad_motion_trigger(grab ? MOT_LIFT : MOT_TILT);
          if (cur[5] && !s_mprev[5]) pad_motion_trigger(MOT_TWIST);
          if (ctrl && !s_mprev[6]) pad_motion_trigger(cur[2] ? MOT_KNEE_L : cur[3] ? MOT_KNEE_R : MOT_DOWN);
      }
      /* v1046A (user: "slams on SPACE instead of X+W"): while grabbing (R1 held), a plain SPACE press = the forward
       * gesture (wall slam / push / toss over a ledge), and SPACE's normal Cross button is withheld during the grab.
       * X+W still works. PAD_KBM_SPACE_SLAM=0 = off. */
      { static int sp_on = -1, sp_prev = 0; if (sp_on < 0) sp_on = pad_envi("PAD_KBM_SPACE_SLAM", 1);
        const int sp = KD(VK_SPACE);
        if (sp_on && !mot && grab) {
            if (sp && !sp_prev) { pad_motion_trigger(MOT_FORWARD); printf("[motion] v1046A SPACE while grabbing -> slam\n"); fflush(stdout); }
            if (sp) btns &= ~CELL_PAD_CTRL_CROSS;
        }
        sp_prev = sp; }
      /* v1049Q (user: prompt says Q/E slams to the sides, but only X+A / X+D did): while grabbing (R1 held), Q / E = the
       * left / right gesture (the same as X+A / X+D: side slam / swing), and Q's L2 (weapon wheel) / E's Triangle are
       * withheld during the grab. PAD_KBM_GRAB_QE=0 = off. */
      { static int qe_on = -1, q_prev = 0, e_prev = 0; if (qe_on < 0) qe_on = pad_envi("PAD_KBM_GRAB_QE", 1);
        const int q = KD('Q'), e = KD('E');
        if (qe_on && !mot && grab) {
            if (q && !q_prev) { pad_motion_trigger(MOT_LEFT);  printf("[motion] v1049Q Q while grabbing -> slam left\n");  fflush(stdout); }
            if (e && !e_prev) { pad_motion_trigger(MOT_RIGHT); printf("[motion] v1049Q E while grabbing -> slam right\n"); fflush(stdout); }
            if (q) btns &= ~CELL_PAD_CTRL_L2;
            if (e) btns &= ~CELL_PAD_CTRL_TRIANGLE;
        }
        q_prev = q; e_prev = e; }
      for (int i = 0; i < 7; i++) s_mprev[i] = cur[i]; }
    /* v1049H (user): H = hide / unhide the weapon, R = reload. In this game d-pad DOWN holsters, d-pad UP draws the
     * weapon -- and reloads while a gun is out. H taps DOWN when a weapon is out and UP when it is away (one 90 ms tap
     * per press); R with a gun drawn = UP (reload) instead of its R2 (R keeps R2 / melee charge otherwise). In game only
     * (not in menus, not during a motion gesture). PAD_KBM_HR=0 = off. */
    { static int hr_on = -1, h_prev = 0; static unsigned long long h_end = 0; static u16 h_btn = 0;
      if (hr_on < 0) hr_on = pad_envi("PAD_KBM_HR", 1);
      if (hr_on && !mot && !s_free && cmb_gun >= 0) {
          const unsigned long long ht = pad_now_ms();
          const int h = KD('H');
          /* v1052H (user: "H doesn't hide the gun consistently, I have to spam it 5-6 times"): the log showed every draw
           * (UP) worked at once but a 90 ms DOWN tap rarely holstered. Now the d-pad button is HELD until the game reports
           * the weapon state flipped (g_pad_drawn_fn), at least PAD_KBM_H_MIN_MS (60) and at most PAD_KBM_H_MAX_MS (700);
           * pressing H again while it is held cancels. Log "[keys] v1052H ... after N ms". */
          static int h_want = -1; static unsigned long long h_t0 = 0; static int h_min = -1, h_max = 700;
          if (h_min < 0) { h_min = pad_envi("PAD_KBM_H_MIN_MS", 60); h_max = pad_envi("PAD_KBM_H_MAX_MS", 700); }
          if (h && !h_prev) {
              const int drawn = g_pad_drawn_fn ? g_pad_drawn_fn() : (cmb_gun == 1);
              if (h_end && ht < h_end) { h_end = 0; h_want = -1; }   /* second press while holding: stop */
              else {
                  h_btn = drawn ? CELL_PAD_CTRL_DOWN : CELL_PAD_CTRL_UP; h_t0 = ht; h_end = ht + (unsigned long long)h_max;
                  if (!drawn) s_draw_req_ms = ht;   /* v1053N2: H = draw -> RMB within the window aims */
                  h_want = drawn ? 0 : 1;
                  printf("[keys] v1049H H -> %s weapon\n", drawn ? "hide" : "draw"); fflush(stdout);
              }
          }
          h_prev = h;
          if (h_end && ht < h_end) {
              const int now_drawn = g_pad_drawn_fn ? g_pad_drawn_fn() : -1;
              if (now_drawn == h_want && ht - h_t0 >= (unsigned long long)h_min) {
                  printf("[keys] v1052H weapon %s after %llu ms\n", h_want ? "drawn" : "hidden", ht - h_t0); fflush(stdout);
                  h_end = 0;
              } else btns |= h_btn;
          } else if (h_end) {
              printf("[keys] v1052H %s did not happen within %d ms\n", h_want ? "draw" : "hide", h_max); fflush(stdout);
              h_end = 0;
          }
          if (cmb_gun == 1 && KD('R')) { btns &= ~CELL_PAD_CTRL_R2; btns |= CELL_PAD_CTRL_UP; }
      } else h_prev = KD('H'); }
    /* v1053D (user: "change driving so it's actually WASD and not shift and F", "make space the drift button instead of
     * C"). Godfather's car: Cross = gas, Square = brake / reverse, Circle = handbrake. While driving: W = Cross,
     * S = Square, SPACE = Circle (its Cross is withheld), A / D steer (left stick X), W / S leave the stick Y alone.
     * Driving = a player exists, the on-foot camera (gf_footcam.c, func_00055E40) has not run for PAD_KBM_CAR_MS
     * (250 ms), and no menu / cutscene / free-aim / grab / gesture / RMB. Shift / F / C keep working. Log [drive].
     * PAD_KBM_CAR=0 = off. */
    static int s_car = 0;
    { static int car_on = -1;
      if (car_on < 0) { car_on = pad_envi("PAD_KBM_CAR", 1);   /* v1053V: real in-car flag (g_pad_incar_fn) */ }
      extern volatile int g_uiov_menu_open;
      const int cutscene = g_pad_cutscene_fn && g_pad_cutscene_fn();
      const int menu = (g_pad_menu_fn && g_pad_menu_fn()) || g_uiov_menu_open;   /* v1053V: the car flag implies a player */
      const int car = car_on && g_pad_incar_fn && g_pad_incar_fn() && !menu && !cutscene && !mot && !s_free;   /* v1053V */
      if (car != s_car) { s_car = car; printf("[drive] v1053D car controls %s (W gas, S brake, SPACE drift)\n", car ? "ON" : "off"); fflush(stdout); }
      if (car) {
          if (KD(VK_SPACE)) { btns &= ~CELL_PAD_CTRL_CROSS; btns |= CELL_PAD_CTRL_CIRCLE; }
          if (KD(VK_LSHIFT) || KD(VK_RSHIFT) || KD(VK_RETURN)) btns |= CELL_PAD_CTRL_CROSS;
          if (KD('W')) btns |= CELL_PAD_CTRL_CROSS;
          if (KD('S')) btns |= CELL_PAD_CTRL_SQUARE;
      } }
    float lx = 0.0f, ly = 0.0f;
    /* v1043Y (user): in the main menu / pause menu WASD = d-pad (discrete steps), not the left stick. Menu = no player
     * (g_pad_weapon_fn < 0: frontend, creator, loading) or the title's menu hook (pause). PAD_KBM_MENU_DPAD=0 = off. */
    static int s_menu_dpad = -1; if (s_menu_dpad < 0) s_menu_dpad = pad_envi("PAD_KBM_MENU_DPAD", 1);
    const int menu_dpad = s_menu_dpad && ((g_pad_weapon_fn && cmb_gun < 0) || (g_pad_menu_fn && g_pad_menu_fn()));
    if (!mot && menu_dpad) {
        if (KD('W')) btns |= CELL_PAD_CTRL_UP;    if (KD('S')) btns |= CELL_PAD_CTRL_DOWN;
        if (KD('A')) btns |= CELL_PAD_CTRL_LEFT;  if (KD('D')) btns |= CELL_PAD_CTRL_RIGHT;
    } else if (!mot) {
    if (KD('A')) lx -= 1.0f;
    if (KD('D')) lx += 1.0f;
    if (KD('W') && !s_car) ly -= 1.0f;   /* v1053D: W / S are gas / brake in a car */
    if (KD('S') && !s_car) ly += 1.0f;
    }
    const int moving = (lx != 0.0f || ly != 0.0f);
    /* v1115W (user: "toggle slow walk with CTRL, not hold; sprinting cancels it"): a Ctrl PRESS toggles walking
     * (PAD_KBM_WALK deflection) on / off; sprinting (Cross held while moving = Shift by default) turns it off.
     * Ctrl used for an X+Ctrl move does not toggle: no toggle while X (the motion key) is held, and a toggle is
     * undone if X goes down while that same Ctrl press is still held. Menus and the car never toggle (the car also
     * clears it). PAD_KBM_WALK_TOGGLE=0 = the old hold-Ctrl walk. Log [walk]. */
    static int s_wt_on = -1, s_wt = 0, s_ctrl_prev = 0, s_ctrl_flip = 0;
    if (s_wt_on < 0) s_wt_on = pad_envi("PAD_KBM_WALK_TOGGLE", 1);
    const int ctrl_now = KD(VK_LCONTROL) || KD(VK_RCONTROL);
    int walk_now;
    if (s_wt_on) {
        if (ctrl_now && !s_ctrl_prev && !mot && !menu_dpad && !s_car) {
            s_wt = !s_wt; s_ctrl_flip = 1;
            printf("[walk] v1115W walk %s (Ctrl)\n", s_wt ? "ON" : "off"); fflush(stdout);
        }
        if (!ctrl_now) s_ctrl_flip = 0;
        if (mot && ctrl_now && s_ctrl_flip) {
            s_wt = !s_wt; s_ctrl_flip = 0;
            printf("[walk] v1115W Ctrl was for an X+Ctrl move -> walk back %s\n", s_wt ? "ON" : "off"); fflush(stdout);
        }
        if (s_wt && ((moving && (btns & CELL_PAD_CTRL_CROSS) && !menu_dpad) || s_car)) {
            s_wt = 0;
            printf("[walk] v1115W walk off (%s)\n", s_car ? "car" : "sprint"); fflush(stdout);
        }
        walk_now = s_wt;
    } else {
        walk_now = KD(VK_LCONTROL);
    }
    s_ctrl_prev = ctrl_now;
    if (moving) {
        const float mag = walk_now ? (float)s_walk : 127.0f;
        const float n = (lx != 0.0f && ly != 0.0f) ? 0.70710678f : 1.0f;
        lx *= mag * n; ly *= mag * n;
    }
    /* mouse -> right stick: smoothed velocity, scaled so PAD_MOUSE_SPEED px/s = full deflection, plus a
     * fixed offset past the game's stick dead zone so slow moves still turn. */
    const int alt = KD(VK_MENU);
    const int clicks = KD(VK_LBUTTON) || KD(VK_RBUTTON) || KD(VK_MBUTTON);
#undef KD
    long dx = 0, dy = 0; int have_mouse = 0;
    extern volatile int g_uiov_menu_open;
    HWND fg = GetForegroundWindow();
    if (fg && !alt && !s_free && !g_uiov_menu_open && !mcur) {   /* v1107U: !mcur = free pointer in menus */
        RECT rc; POINT c, p;
        if (GetClientRect(fg, &rc) && rc.right > 8 && rc.bottom > 8 && GetCursorPos(&p)) {
            c.x = rc.right / 2; c.y = rc.bottom / 2; ClientToScreen(fg, &c);
            if (g_pad_mouse_captured) {
                if (g_pad_raw_ok) { dx = raw_dx; dy = raw_dy; }
                else { dx = p.x - c.x; dy = p.y - c.y; }
            }
            if (p.x != c.x || p.y != c.y) SetCursorPos(c.x, c.y);
            g_pad_mouse_captured = 1; have_mouse = 1;
            /* v1043K (user: "the mouse gets unfocused when I use the dragging"): the cursor is only re-centred once per
             * poll, so a fast drag (grab swings) could leave the window between polls. Confine it to the client area
             * while the mouse is captured (PAD_MOUSE_CLIP=0 = off); released below when capture ends. */
            { static int clip = -1; if (clip < 0) clip = pad_envi("PAD_MOUSE_CLIP", 1);
              DWORD fpid = 0; GetWindowThreadProcessId(fg, &fpid);
              if (clip && fpid == GetCurrentProcessId()) { POINT tl = {0, 0}, br = {rc.right, rc.bottom};
                          ClientToScreen(fg, &tl); ClientToScreen(fg, &br);
                          RECT cr = {tl.x, tl.y, br.x, br.y}, cur;
                          if (!GetClipCursor(&cur) || cur.left != cr.left || cur.top != cr.top || cur.right != cr.right || cur.bottom != cr.bottom)
                              ClipCursor(&cr);
                          s_pad_clipped = 1; } }
        }
    }
    if (!have_mouse && s_pad_clipped) { ClipCursor(NULL); s_pad_clipped = 0; }
    if (!have_mouse) { g_pad_mouse_captured = 0; dx = dy = 0; }
    { /* v1042M mouse flick while the motion key is held (PAD_MOTION_MOUSE_PX, default 60 counts) */
      static float ax = 0.0f, ay = 0.0f; static int thr = -1;
      if (thr < 0) { thr = pad_envi("PAD_MOTION_MOUSE_PX", 60); if (thr < 5) thr = 5; }
      if (mot && have_mouse) {
          const float k = (float)exp(-dt / 0.15);
          ax = ax * k + (float)dx; ay = ay * k + (float)dy;
          const float fx = ax < 0 ? -ax : ax, fy = ay < 0 ? -ay : ay;
          if (fy >= (float)thr && fy >= fx)      { pad_motion_trigger(ay < 0 ? MOT_FORWARD : MOT_BACK); ax = ay = 0.0f; }
          else if (fx >= (float)thr)             { pad_motion_trigger(ax < 0 ? MOT_LEFT : MOT_RIGHT); ax = ay = 0.0f; }
          dx = dy = 0; s_vx = s_vy = 0.0f;      /* no camera turn while gesturing */
      } else { ax = ay = 0.0f; }
    }
    /* v1044C (user: free aim "EXTREMELY slow" -- the game caps the stick turn rate): mouse X goes straight into the
     * game's free-aim yaw (degrees, PAD_KBM_AIM_DEG per mouse count, default 0.08; PAD_KBM_AIM_DIRECT=0 = stick). */
    int aim_direct = 0;
    { static int on = -1; static float dpc = 0.08f;
      if (on < 0) { on = pad_envi("PAD_KBM_AIM_DIRECT", 1); const char* e = getenv("PAD_KBM_AIM_DEG"); if (e && *e) dpc = (float)atof(e); }
      static int iy = -1, ix = 0; if (iy < 0) { iy = pad_envi("PAD_KBM_AIM_INVY", 0); ix = pad_envi("PAD_KBM_AIM_INVX", 0); }
      if (on && s_fa_act && have_mouse && !mot && g_pad_aim_fn)
          aim_direct = g_pad_aim_fn((float)dx * dpc * (ix ? -1.0f : 1.0f), (float)dy * dpc * (iy ? -1.0f : 1.0f)); }
    /* v1046L (user: on-foot mouse look "way too slow and restricted, want it like GTA"): the game's follow camera
     * (func_0006A7F8) caps the stick turn rate, clamps the yaw offset and swings back behind the player. With KBM the
     * mouse now sets that camera's angles directly (gf_footcam.c): GF_FOOTCAM_DEG (0.12) degrees per mouse count
     * (scaled by the F10 mouse speed), GF_FOOTCAM_INVX / GF_FOOTCAM_INVY = 1 flip an axis, GF_FOOTCAM=0 = old stick look.
     * Not while free-aiming, locked on, grabbing or doing a motion gesture (those keep their own mouse handling). */
    int look_direct = 0;
    { static int on = -1, ix = 0, iy = 0; static float dpc0 = 0.12f;
      if (on < 0) { on = pad_envi("GF_FOOTCAM", 1); const char* e = getenv("GF_FOOTCAM_DEG"); if (e && *e) dpc0 = (float)atof(e);
                    ix = pad_envi("GF_FOOTCAM_INVX", 0); iy = pad_envi("GF_FOOTCAM_INVY", 0); }
      float dpc = dpc0;
      { extern volatile int g_ui_mouse_speed; if (g_ui_mouse_speed > 0) dpc = dpc0 * (float)g_ui_mouse_speed / 450.0f; }
      /* v1046M (user: "can't use the weapon wheel now"): holding L2 (Q) opens the weapon wheel, picked with the right
       * stick -> while L2 is held the mouse drives the right stick again. */
      const int wheel = (btns & CELL_PAD_CTRL_L2) != 0;
      if (on && !wheel && !aim_direct && !s_fa_act && !s_lock_act && have_mouse && !mot && !cmb_rs && !cmb_grab && g_input_kbm == 1 && g_pad_look_fn)
          look_direct = g_pad_look_fn((float)dx * dpc * (ix ? -1.0f : 1.0f), (float)dy * dpc * (iy ? -1.0f : 1.0f)); }
    const float a = (float)exp(-dt / s_tau);
    s_vx = s_vx * a + (float)((double)((aim_direct || look_direct) ? 0 : dx) / dt) * (1.0f - a);
    s_vy = s_vy * a + (float)((double)((aim_direct || look_direct) ? 0 : dy) / dt) * (1.0f - a);
    if (s_inv) { /* applied below */ }
    float rx = 0.0f, ry = 0.0f;
    /* v1044B (user: "I move the camera way too slowly in free aim"): the free-aim crosshair turns slower per stick unit than
     * the normal camera, so while free aim is on the same mouse speed gives PAD_KBM_FREEAIM_SENS (default 3.0) times the
     * stick deflection. If the game's own maximum free-aim turn rate is the limit, full deflection is reached sooner but
     * cannot go faster -- then the turn rate itself needs a game-side patch. */
    static float fa_sens = -1.0f;
    if (fa_sens < 0.0f) { const char* e = getenv("PAD_KBM_FREEAIM_SENS"); fa_sens = (e && *e) ? (float)atof(e) : 3.0f; if (fa_sens < 0.1f) fa_sens = 0.1f; }
    const float spd = s_fa_act ? s_speed / fa_sens : s_speed;
    { float fx = s_vx / spd, fy = (s_inv ? -s_vy : s_vy) / spd;    /* PS3 stick: down = + */
      const float m = sqrtf(fx * fx + fy * fy);
      if (m > 0.01f) {
          const float span = 127.0f - (float)s_dz;
          float out = (float)s_dz + (m > 1.0f ? 1.0f : m) * span;
          rx = fx / m * out; ry = fy / m * out;
      } else { s_vx = s_vy = 0.0f; } }
    if (aim_direct || look_direct) { rx = 0.0f; ry = 0.0f; }   /* v1044F / v1046L: the camera angles are written directly */
    if (s_lock_act) { rx = 0.0f; ry = 0.0f; s_vx = s_vy = 0.0f; }   /* v1044K: locked on -- no zone-target flicks */
    { /* v1043K (user): while grabbing, a quick mouse DRAG left / right = the grab swing (was Q / E; PAD_KBM_QE=1 brings
       * the keys back): the right stick is held to that side for PAD_KBM_DRAG_MS (250) and the SIXAXIS swing gesture
       * fires once. Threshold PAD_KBM_DRAG_PX (60 mouse counts within ~0.15 s). Only while grabbing; the mouse still does
       * not turn the camera then. */
      /* v1043K-2 (user: "left drag is not reliable"): threshold 60 -> 40 counts, hold 250 -> 350 ms, and the swing keeps
       * holding while the drag continues the same way (like holding Q / E did), up to 1.2 s. */
      static float gx = 0.0f; static int thr = -1, hold_ms = 350, sdir = 0; static unsigned long long s_until = 0, s_start = 0;
      if (thr < 0) { thr = pad_envi("PAD_KBM_DRAG_PX", 40); if (thr < 5) thr = 5; hold_ms = pad_envi("PAD_KBM_DRAG_MS", 350); }
      const unsigned long long gt = pad_now_ms();
      if (cmb_grab && !mot && have_mouse) {
          const float k = (float)exp(-dt / 0.15);
          gx = gx * k + (float)dx;
          if (sdir && gt >= s_until) sdir = 0;
          if (sdir && (long)sdir * dx > 0 && gt - s_start < 1200ull) s_until = gt + (unsigned long long)hold_ms;   /* still dragging */
          if (!sdir && (gx >= (float)thr || gx <= -(float)thr)) {
              sdir = gx < 0.0f ? -1 : 1; s_until = gt + (unsigned long long)hold_ms; s_start = gt; gx = 0.0f;
              pad_motion_trigger(sdir < 0 ? MOT_LEFT : MOT_RIGHT);
              printf("[motion] v1043K grab swing %s (mouse drag)\n", sdir < 0 ? "left" : "right"); fflush(stdout);
          }
      } else { gx = 0.0f; sdir = 0; s_until = 0; }
      if (sdir && gt < s_until) { cmb_rs = 1; cmb_rx = (float)sdir * 127.0f; cmb_ry = 0.0f; }
    }
    if (cmb_rs) { rx = cmb_rx; ry = cmb_ry; s_vx = s_vy = 0.0f; }   /* v1042X: locked-on melee owns the right stick */
    const int looking = (rx != 0.0f || ry != 0.0f) || cmb_rs == 1;
    if (btns || moving || clicks || (raw_dx < 0 ? -raw_dx : raw_dx) + (raw_dy < 0 ? -raw_dy : raw_dy) > 3) pad_note_device(1);
    else if (g_input_kbm < 0 && !merge) pad_note_device(1);
    { static u16 kb_prev = 0; const u16 kb = (u16)(btns & MM_MASK);   /* v1114P: a new key press beats the pointer */
      if (s_mm_drive && (kb & ~kb_prev)) {
          pad_menu_reset();
          static int nl = 0; if (nl < 20) { nl++; printf("[cellPad] v1114P menu: key press -> keyboard has the menu (pointer pulses dropped)\n"); fflush(stdout); } }
      kb_prev = kb; }
    if (s_mm_drive) btns = (u16)((btns & ~MM_MASK) | s_mm_out | s_mm_hold);   /* v1107X: the menu pointer owns d-pad + Cross */
    if (merge) {
        hs->buttons |= btns;
        if (moving)  { hs->analog_lx = pad_axis(lx); hs->analog_ly = pad_axis(ly); }
        if (looking) { hs->analog_rx = pad_axis(rx); hs->analog_ry = pad_axis(ry); }
        if (btns & CELL_PAD_CTRL_L2) hs->trigger_l2 = 255;
        if (btns & CELL_PAD_CTRL_R2) hs->trigger_r2 = 255;
    } else {
        hs->buttons = btns; hs->connected = 1;
        hs->analog_lx = pad_axis(lx); hs->analog_ly = pad_axis(ly);
        hs->analog_rx = pad_axis(rx); hs->analog_ry = pad_axis(ry);
        hs->trigger_l2 = (u8)((btns & CELL_PAD_CTRL_L2) ? 255 : 0);
        hs->trigger_r2 = (u8)((btns & CELL_PAD_CTRL_R2) ? 255 : 0);
    }
    const u16 b = hs->buttons;
    hs->press_up = (b & CELL_PAD_CTRL_UP) ? 255 : 0;             hs->press_down = (b & CELL_PAD_CTRL_DOWN) ? 255 : 0;
    hs->press_left = (b & CELL_PAD_CTRL_LEFT) ? 255 : 0;         hs->press_right = (b & CELL_PAD_CTRL_RIGHT) ? 255 : 0;
    hs->press_triangle = (b & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0; hs->press_circle = (b & CELL_PAD_CTRL_CIRCLE) ? 255 : 0;
    hs->press_cross = (b & CELL_PAD_CTRL_CROSS) ? 255 : 0;       hs->press_square = (b & CELL_PAD_CTRL_SQUARE) ? 255 : 0;
    hs->press_l1 = (b & CELL_PAD_CTRL_L1) ? 255 : 0;             hs->press_r1 = (b & CELL_PAD_CTRL_R1) ? 255 : 0;
    { static int said = 0; if (!said && (btns || moving || looking)) { said = 1;
        printf("[cellPad] generic keyboard/mouse input active on port 0%s\n", merge ? " (merged with controller)" : ""); fflush(stdout); } }
}
static void pad_poll_kbm_generic(int merge)
{
    if (!kbm_classic()) { pad_poll_kbm_generic_reborn(merge); return; }   /* v1130P */
    PadHostState* hs = &s_host_state[0];
    static int s_walk = -1, s_dz = 30, s_inv = 0; static float s_speed = 1200.0f, s_tau = 0.060f;
    if (s_walk < 0) {
        /* v1115W2 (user: "can't walk with it toggled"): 64 is in Godfather's turn-on-the-spot band (0 m/s, gf_smoke.c
         * 86_t6_sweep); 90 = the ~1.6 m/s walk the smoking cap uses. */
        s_walk = pad_envi("PAD_KBM_WALK", 90); s_dz = pad_envi("PAD_MOUSE_DZ", 30); s_inv = pad_envi("PAD_MOUSE_INVERT_Y", 0);
        const char* e = getenv("PAD_MOUSE_SPEED"); if (e && *e) s_speed = (float)atof(e); if (s_speed < 50.0f) s_speed = 50.0f;
        s_tau = (float)pad_envi("PAD_MOUSE_SMOOTH_MS", 60) / 1000.0f; if (s_tau < 0.005f) s_tau = 0.005f;
    }
    static float s_vx = 0.0f, s_vy = 0.0f;        /* smoothed mouse velocity, px/s */
    static LARGE_INTEGER s_f, s_t; LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!s_f.QuadPart) { QueryPerformanceFrequency(&s_f); s_t = now; }
    double dt = (double)(now.QuadPart - s_t.QuadPart) / (double)s_f.QuadPart; s_t = now;
    if (dt <= 0.0) dt = 0.001; if (dt > 0.1) dt = 0.1;
    const long raw_dx = InterlockedExchange(&g_pad_raw_mdx, 0), raw_dy = InterlockedExchange(&g_pad_raw_mdy, 0);
    const long raw_wheel = InterlockedExchange(&g_pad_raw_wheel, 0);   /* v1107U: menu scroll */
    { const int foc = pad_host_window_focused();   /* v1043T-2: the typing queue also runs unfocused (soaks) */
      if (pad_namekb_poll(hs, merge, foc)) { g_pad_mouse_captured = 0; return; } }
    if (!pad_host_window_focused()) {
        g_pad_mouse_captured = 0; s_vx = s_vy = 0.0f;
        if (!merge) {
            hs->buttons = 0; hs->analog_lx = hs->analog_ly = hs->analog_rx = hs->analog_ry = 128;
            hs->trigger_l2 = hs->trigger_r2 = 0; hs->connected = 1;
        }
        return;
    }
#define KD(vk) ((GetAsyncKeyState(vk) & 0x8000) != 0)
    /* Esc toggles a free pointer (and is START); a click inside the window grabs the mouse again. */
    static int s_free = 0, s_esc_prev = 0, s_lmb_block = 0;
    { const int esc = KD(VK_ESCAPE);
      extern volatile int g_uiov_menu_open;
      if (esc && !s_esc_prev && !g_uiov_menu_open) s_free = !s_free;
      s_esc_prev = esc;
      if (s_free && KD(VK_LBUTTON)) {
          HWND fw = GetForegroundWindow(); POINT cp; RECT cr;
          if (fw && GetCursorPos(&cp) && ScreenToClient(fw, &cp) && GetClientRect(fw, &cr) &&
              cp.x >= 0 && cp.y >= 0 && cp.x < cr.right && cp.y < cr.bottom) { s_free = 0; s_lmb_block = 1; }
      }
      if (s_lmb_block && !KD(VK_LBUTTON)) s_lmb_block = 0; }
    /* v1110V (user: "I clicked on a cigar in my inventory and instead of smoking the shopkeeper thought I intimidated him" -- the log:
     * every click in the port's own pages (inventory / store / settings / trainer: g_uiov_menu_open) ALSO went to the game as R1 =
     * punches, the smoke stopped on the held button, and the shopkeeper next to him got beaten). While such a page is open the mouse
     * buttons do not reach the game, and a button held when it closes stays blocked until it is released. PAD_OV_MOUSE_BLOCK=0 = old. */
    static int s_ovb = 0; int ov_mouse = 0;
    { extern volatile int g_uiov_menu_open; static int on = -1; if (on < 0) on = pad_envi("PAD_OV_MOUSE_BLOCK", 1);
      const int anyb = KD(VK_LBUTTON) || KD(VK_RBUTTON) || KD(VK_MBUTTON);
      if (on) { if (g_uiov_menu_open && anyb) s_ovb = 1; if (!anyb) s_ovb = 0; ov_mouse = g_uiov_menu_open || s_ovb; } }
    /* v1042M motion key (PAD_MOTION_KEY, default X): hold it and tap W forward / S lift / A,D swing left,right /
     * Space slam down / E twist, or flick the mouse (forward = up, lift = down, left / right). While it is held
     * those keys and the mouse stop driving movement, the camera, Cross and Triangle. */
    static int s_mot_vk = -1; static int s_mprev[7];
    if (s_mot_vk < 0) { const char* e = getenv("PAD_MOTION_KEY"); s_mot_vk = (e && *e) ? kbm_key_vk(e) : 0xFF;   /* v1130F: no gesture key by default (EA-style gestures below) */ if (!s_mot_vk) s_mot_vk = 'G';   /* v1130E: gesture key G (X = strangle / L3, EA PC scheme) */ }
    const int mot = KD(s_mot_vk);
    u16 btns = 0;
    /* v1042X PC melee layout -- only with fists / a melee weapon in hand AND lock-on (RMB = L1) held:
     *   F      tap = punch (right stick up; with A / D held: up-left / up-right = hooks), hold (PAD_KBM_BLOCK_MS,
     *          180) = block (Square, F's normal button)
     *   R      hold = charge (right stick down), release = power attack / throw an explosive (right stick up)
     *   LMB    = R1 as always, so RMB+LMB = grab; while grabbing F = grab attacks,
     *   Q / E  (while grabbing) = swing left / right: the right stick held left / right plus the SIXAXIS swing
     *          gesture on the press (PAD_KBM_QE_MOTION=0: stick only)
     * While locked on the mouse does not drive the right stick (no accidental flicks). Gun in hand, no lock-on, or no
     * player (menus) = the normal mapping. PAD_KBM_COMBAT=0 (gf_combat.c) = off. The prompts say the same with the
     * KBM glyphs ([RDLeft]=F, [RSBottom]=R, [LSBottom]=Q, [RDUp]=E; gf_prompts_table.h). */
    const int cmb_gun = g_pad_weapon_fn ? g_pad_weapon_fn() : -1;
    /* v1107U menu pointer: in the game's menus (no player: frontend, creator, loading; or the pause menu) with the keyboard /
     * mouse in use, the mouse is not captured, so the pointer shows. LMB = Cross, RMB = Circle, wheel = d-pad up / down. */
    static int s_mcur_on = -1; if (s_mcur_on < 0) s_mcur_on = pad_envi("PAD_MENU_CURSOR", 1);
    static int s_menu_qe = -1; if (s_menu_qe < 0) s_menu_qe = pad_envi("PAD_MENU_QE", 1);
    const int mcur = s_mcur_on && g_pad_kbm_last && !s_free &&
                     ((g_pad_weapon_fn && cmb_gun < 0) || (g_pad_menu_fn && g_pad_menu_fn())) &&
                     !(g_pad_cutscene_fn && g_pad_cutscene_fn());
    { static int was = -1; if (mcur != was) { was = mcur; printf("[cellPad] v1107U menu pointer %s\n", mcur ? "ON" : "off"); fflush(stdout); } }
    /* v1130Q PC Classic: the weapon dealer's buy screen (gf_dialogue.c F2FShopping) works with the mouse like the EA PC port:
     * the pointer is free, a click on SALIR / EXIT (left bar) = d-pad left, on COMPRAR / BUY (right bar) = d-pad right, on the
     * quantity arrows or the wheel = up / down; Enter = buy, Backspace = exit. The mouse buttons do not reach the game there.
     * Bars at 1280x720: y 549..573, x 291..601 / 679..989; quantity arrows x ~818..836, y ~376..396. PAD_KBM_SHOP_MOUSE=0 = off. */
    { static int on = -1; if (on < 0) on = pad_envi("PAD_KBM_SHOP_MOUSE", 1);
      const int shp = g_pad_shop_fn ? g_pad_shop_fn() : 0;
      const int sh = on && shp && g_pad_kbm_last && !s_free && !mcur && !ov_mouse;
      s_shop_popup = sh && shp == 2;
      if (sh != s_shop_ptr) { printf("[cellPad] v1130Q buy screen pointer %s\n", sh ? "ON" : "off"); fflush(stdout); }
      s_shop_ptr = sh; }
    /* v1044A (user: "free aim and aim assist like a Rockstar game", step 1): gun in hand -> hold RMB = the game's free-aim
     * crosshair (R3 toggles it; one R3 pulse on RMB press, one on release), mouse = crosshair, LMB = fire. The old lock-on
     * (L1) moves to T. Fists / melee keep RMB = lock-on (combat layout). PAD_KBM_FREEAIM=0 = old RMB = L1. */
    static int fa_on = -1; if (fa_on < 0) fa_on = pad_envi("PAD_KBM_FREEAIM", 1);
    /* v1119L (user: Monk's lesson in "Sleeping with the Fishes" can't be beaten -- "LOCK-ON: Hold L1" waits for the game's own
     * lock-on, s1_TLN_lock_shoot2, and RMB = free aim never gives it): while the lesson runs (g_gf_classic_aim, set from its
     * script events in gf_coop_mission.c) the PC free aim steps aside and the classic layout applies: RMB = L1 lock-on, the
     * mouse = right stick (adjusts the aim while locked: "Disarming"), LMB = R1, MMB / Z = R3 (the free-aim step). Switches
     * only with RMB up and free aim off, so the R3 toggle never desyncs. Log [aim] v1119L. */
    static int s_classic_eff = 0;
    if (g_gf_classic_aim != s_classic_eff && !s_fa_busy_pub && !KD(VK_RBUTTON)) {
        s_classic_eff = g_gf_classic_aim;
        printf("[aim] v1119L classic aiming %s (Monk's lesson)\n", s_classic_eff ? "ON: RMB = lock-on, MMB / Z = free aim" : "off: back to PC free aim"); fflush(stdout);
    }
    const int fa_now = fa_on && !s_classic_eff;
    const int fa_gun = fa_now && cmb_gun == 1 && !s_free;
    /* v1053M: gun selected but holstered + RMB = draw it (d-pad UP) and free-aim, never the game's L1 lock-on aim.
     * PAD_KBM_RMB_DRAW=0 = old (RMB = melee lock-on while the gun is away). */
    static int rmb_draw = -1; if (rmb_draw < 0) rmb_draw = pad_envi("PAD_KBM_RMB_DRAW", 1);
    /* v1053N2: only a gun the player is pulling out RIGHT NOW counts -- selected in the wheel within GF_GUN_INTENT_MS
     * (gf_combat.c returns 2) or a draw requested (H, d-pad UP keys) within PAD_KBM_DRAW_INTENT_MS (1500). A gun that
     * stays holstered keeps RMB = the melee lock-on. */
    static int draw_win = -1; if (draw_win < 0) draw_win = pad_envi("PAD_KBM_DRAW_INTENT_MS", 1500);
    const int hol = (rmb_draw && fa_now && !fa_gun && !s_free && !mot && g_pad_gun_holstered_fn) ? g_pad_gun_holstered_fn() : 0;
    if (hol && !KD(VK_RBUTTON) && (KD(VK_UP) || KD('1'))) s_draw_req_ms = pad_now_ms();
    static int s_gh_hold = 0;   /* keeps the decision for the whole RMB hold */
    if (!KD(VK_RBUTTON)) s_gh_hold = 0;
    else if (!s_gh_hold && hol) s_gh_hold = (hol == 2 || pad_now_ms() - s_draw_req_ms < (unsigned long long)draw_win) ? 1 : 2;
    const int gun_hol = hol && s_gh_hold == 1;
    const int cmb_fist = cmb_gun == 0 && !s_free && !mot && KD(VK_RBUTTON) && !gun_hol && !ov_mouse;   /* v1053M: not with a holstered gun; v1110V not in our pages */
    const int cmb_grab = cmb_fist && KD(VK_LBUTTON) && !s_lmb_block;
    static int s_fa_act = 0;   /* v1044B: free aim currently on (mouse sensitivity below) */
    static int s_lock_act = 0; /* v1044K: RMB lock-on phase (mouse does not drive the stick) */
    { extern volatile int g_pad_fa_act; g_pad_fa_act = s_fa_act; }   /* v1044E: ppu_loader [getstick-chain] tag */
    for (int i = 0; i < s_kbm_nbinds; i++) {
        const int vk = s_kbm_binds[i].vk;
        if (cmb_fist && (vk == VK_LSHIFT || vk == VK_RSHIFT)) continue;   /* v1130E */
        if ((vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT) && cmb_gun >= 0 && !s_shop_ptr && !(g_pad_menu_fn && g_pad_menu_fn())) continue;   /* v1130G: in game the arrows are the right stick (EA PC); v1130R: d-pad on the buy screen */
        if (cmb_grab && (vk == 'Q' || vk == 'E')) continue;
        if (mot && (vk == VK_SPACE || vk == 'E' || vk == 'W' || vk == 'A' || vk == 'S' || vk == 'D' || vk == VK_LCONTROL || vk == VK_RCONTROL)) continue;
        if ((vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2) &&
            (s_free || mcur || ov_mouse || s_shop_ptr || (vk == VK_LBUTTON && s_lmb_block))) continue;   /* v1110V ov_mouse; v1130Q s_shop_ptr */
        if (s_shop_ptr && (vk == VK_RETURN || vk == VK_BACK)) continue;   /* v1130Q: buy / exit pulses below */
        if ((fa_gun || gun_hol) && vk == VK_RBUTTON) continue;   /* v1044A: RMB = free aim with a gun (v1053M: also a holstered one) */
        if (vk == 'B' && g_pad_camcycle_fn) continue;   /* v1046V: V = camera view (tap) / L3 (hold), below */
        if (mcur && s_menu_qe && (vk == 'Q' || vk == 'E')) continue;   /* v1107Z: Q / E = tabs in menus, below */
        if (KD(vk)) btns |= s_kbm_binds[i].btn;
    }
    { extern volatile int g_gf_menu_want; g_gf_menu_want = mcur; }   /* v1107V: frame probe finds the menu */
    g_gf_menu_glyphs = (mcur && s_menu_qe) || s_shop_ptr;   /* v1130Q: PC Classic menu atlas = no d-pad buttons */
    /* v1130R client coordinates (the first version used window-screenshot ones, 31 px too low). At 1280x720 client:
     * bars y 518..542, SALIR x 290..600, COMPRAR x 678..988; quantity arrows x ~825, up 340..354 / down 354..370;
     * the Yes / No pop-up (x 517..762): SI y ~330..352, NO y ~358..382 -> walk there (up / down) and confirm (right, = Enter). */
    { static unsigned long long sp_t = 0; static u16 sp_q[4]; static int sp_n = 0, sp_i = 0, sp_ph = 0; static int pl = 0, pe = 0, pb = 0;
      const unsigned long long tn = pad_now_ms();
      if (s_shop_ptr) {
          u16 hit = 0, hit2 = 0; const int l = KD(VK_LBUTTON), en = KD(VK_RETURN), bk = KD(VK_BACK);
          if (l && !pl) {
              HWND fw = GetForegroundWindow(); POINT cp; RECT cr;
              if (fw && GetCursorPos(&cp) && ScreenToClient(fw, &cp) && GetClientRect(fw, &cr) && cr.right > 0 && cr.bottom > 0) {
                  const float x = (float)cp.x / (float)cr.right * 1280.0f, y = (float)cp.y / (float)cr.bottom * 720.0f;
                  const char* what = "nothing";
                  if (y >= 508.0f && y <= 552.0f && x >= 284.0f && x < 640.0f) { hit = CELL_PAD_CTRL_LEFT; what = "exit"; }   /* v1130S: + the gold left arrow */
                  else if (y >= 508.0f && y <= 552.0f && x >= 640.0f && x <= 994.0f) { hit = CELL_PAD_CTRL_RIGHT; what = "buy"; }   /* + the gold right arrow */
                  /* v1130S quantity: the whole "VENTA: n" line and its arrows (x 800..990): upper half = +1, lower half = -1 */
                  else if (x >= 800.0f && x <= 990.0f && y >= 334.0f && y < 354.0f) { hit = CELL_PAD_CTRL_UP; what = "quantity +"; }
                  else if (x >= 800.0f && x <= 990.0f && y >= 354.0f && y <= 376.0f) { hit = CELL_PAD_CTRL_DOWN; what = "quantity -"; }
                  if (s_shop_popup && !(y >= 508.0f && y <= 552.0f)) {   /* v1130R the pop-up: walk to the line, confirm with Cross (v1130T: the bars stay clickable -- bribe and other F2F menus) */
                      hit = 0; what = "nothing";
                      if (x >= 517.0f && x <= 763.0f && y >= 326.0f && y < 355.0f) { hit = CELL_PAD_CTRL_UP; hit2 = CELL_PAD_CTRL_CROSS; what = "yes"; }
                      else if (x >= 517.0f && x <= 763.0f && y >= 355.0f && y <= 386.0f) { hit = CELL_PAD_CTRL_DOWN; hit2 = CELL_PAD_CTRL_CROSS; what = "no"; }
                  }
                  printf("[cellPad] v1130R buy screen click %.0f,%.0f -> %s\n", x, y, what); fflush(stdout);
              }
          }
          if (en && !pe) hit = s_shop_popup ? CELL_PAD_CTRL_CROSS : CELL_PAD_CTRL_RIGHT;   /* v1130R: Enter = confirm in the pop-up */
          if (bk && !pb) hit = s_shop_popup ? CELL_PAD_CTRL_CIRCLE : CELL_PAD_CTRL_LEFT;
          if (raw_wheel) hit = raw_wheel > 0 ? CELL_PAD_CTRL_UP : CELL_PAD_CTRL_DOWN;
          if (hit) { sp_q[0] = hit; sp_q[1] = hit2; sp_n = hit2 ? 2 : 1; sp_i = 0; sp_ph = 0; sp_t = tn; }
          pl = l; pe = en; pb = bk;
      } else { pl = KD(VK_LBUTTON); pe = KD(VK_RETURN); pb = KD(VK_BACK); }
      /* each queued press: 90 ms down, 90 ms up */
      if (sp_i < sp_n) {
          if (sp_ph == 0) { if (tn - sp_t < 90ull) btns |= sp_q[sp_i]; else { sp_ph = 1; sp_t = tn; } }
          else if (tn - sp_t >= 90ull) { sp_i++; sp_ph = 0; sp_t = tn; }
      } }
    if (mcur && s_menu_qe) { if (KD('Q')) btns |= CELL_PAD_CTRL_L1; if (KD('E')) btns |= CELL_PAD_CTRL_R1; }   /* v1107Z tabs */
    if (mcur) {   /* v1107U menu clicks; v1107V point-and-click */
        pad_menu_mouse(KD(VK_LBUTTON));   /* v1107X: pulses go out through cellPadGetData (pad_menu_getdata_step) */
        if (KD(VK_LBUTTON)) s_lmb_block = 1;   /* a click that closes the menu is not a shot */
        if (KD(VK_RBUTTON)) btns |= CELL_PAD_CTRL_CIRCLE;
        /* v1107X wheel: one d-pad pulse per notch through the same paced queue (was a 70 ms hold) */
        if (raw_wheel && !s_mm_drive) { const int notches = (int)((raw_wheel < 0 ? -raw_wheel : raw_wheel) + 119) / 120;
            mm_push(raw_wheel > 0 ? CELL_PAD_CTRL_UP : CELL_PAD_CTRL_DOWN, notches < 1 ? 1 : notches); }
    } else pad_menu_reset();
    if (gun_hol && KD(VK_RBUTTON) && !ov_mouse) {   /* v1053M: draw the selected gun; free aim follows (v1053L latch gap) */
        btns |= CELL_PAD_CTRL_UP;
        static unsigned long long nd = 0; const unsigned long long t = pad_now_ms();
        if (t - nd > 1000ull) { printf("[aim] v1053M RMB with a holstered gun -> drawing it (no lock-on), free aim follows\n"); fflush(stdout); }
        nd = t;
    }
    { static int vprev = 0, vhold = 0, hold_ms = -1; static unsigned long long vt = 0;
      if (hold_ms < 0) hold_ms = pad_envi("PAD_KBM_V_HOLD_MS", 300);
      if (g_pad_camcycle_fn) {
          const int v = KD('B'); const unsigned long long vn = pad_now_ms();   /* v1130E: B (V = R2 in the EA PC scheme) */
          if (v && !vprev) { vt = vn; vhold = 0; }
          if (v && !vhold && vn - vt >= (unsigned long long)hold_ms) vhold = 1;
          if (!v && vprev && !vhold) g_pad_camcycle_fn();
          vprev = v;
      } }
    { static int fa_state = 0; static unsigned long long fa_pulse_end = 0, fa_last = 0;
      /* v1044K (user: "a combination of the two original modes"): RMB = the game's own lock-on (L1 held) -- it snaps
       * to an NPC exactly as before; as soon as the mouse moves PAD_KBM_LOCK_BREAK (30) counts, or no target appears
       * within PAD_KBM_LOCK_NOTARGET_MS (200), L1 is released and free aim starts (R3) facing the lock target
       * (gf_combat.c aligns the free-aim camera); RMB release ends either. While locked the mouse does not move the
       * stick (no zone-target flicks). PAD_KBM_ASSIST=0 = straight to free aim (v1044B). */
      /* v1044L (user: "doesn't snap reliably"): the mouse broke the lock before the game had picked a target. Now the
       * lock-on gets PAD_KBM_LOCK_MIN_MS (250) untouched; mouse movement only counts after that (break 40 counts);
       * no target -> free aim after PAD_KBM_LOCK_NOTARGET_MS (350). */
      /* v1044Q (user: "lock to the NPC like originally, but WAY easier to free aim from there; no one to aim at -> free
       * aim"): RMB holds the game's lock-on (L1). gf_combat.c clears the stale lock-target pointer on press, so "has a
       * target" is real now (v1044K-N saw the last NPC forever -> RMB did nothing without anyone around). No target within
       * PAD_KBM_LOCK_NOTARGET_MS (150) -> L1 is released and free aim starts a beat later (mode 4 -> 2). Locked: counted
       * from PAD_KBM_LOCK_MIN_MS (80) after the target appeared, PAD_KBM_LOCK_BREAK (12) mouse counts break to free aim
       * on that target (v1044M R3-while-locked handoff). PAD_KBM_ASSIST=0 = straight to free aim. */
      static int mode = 0, brk = -1, nt_ms = 150, asst_on = 1, min_ms = 80; static unsigned long long lock_t0 = 0, has_t = 0, gap_end = 0, lost_t = 0; static long acc = 0;
      static int hand_ms = -1, retries = 0, retry_on = 0, gap_ms = -1; static unsigned long long handoff_end = 0, verify_at = 0;
      if (hand_ms < 0) hand_ms = pad_envi("PAD_KBM_LOCK_HANDOFF_MS", 150);
      if (gap_ms < 0) gap_ms = pad_envi("PAD_KBM_LOCK_GAP_MS", 120);
      if (brk < 0) { brk = pad_envi("PAD_KBM_LOCK_BREAK", 12); nt_ms = pad_envi("PAD_KBM_LOCK_NOTARGET_MS", 150);
                     min_ms = pad_envi("PAD_KBM_LOCK_MIN_MS", 80);
                     /* v1044N (user: "nothing happens unless there's an NPC around", "free aim even more unreliable"):
                      * the lock-first hybrid held L1 with a dead mouse until a target / 350 ms / 40 counts -- with no NPC
                      * (or a stale target in player+0x1544) the press did nothing. And the R3 retry fired while free aim
                      * WAS on (its camera just not found yet), toggling the game OFF while we thought it was ON, so every
                      * later R3 pulse was inverted. Now: RMB = free aim at once (the v1044F mouse path the user liked),
                      * no retry. PAD_KBM_ASSIST=1 brings the lock-first hybrid back, PAD_KBM_FA_RETRY=1 the retry. */
                     asst_on = pad_envi("PAD_KBM_ASSIST", 0);   /* v1044T: RMB = free aim; the lock-on is done inside free aim by gf_combat.c */ retry_on = pad_envi("PAD_KBM_FA_RETRY", 0);
                     printf("[aim] v1044T RMB = %s, R3 retry %s\n", asst_on ? "game lock-on then free aim (PAD_KBM_ASSIST=1)" : "free aim + lock on the NPC at the crosshair",
                            retry_on ? "on" : "off"); fflush(stdout); }
      const unsigned long long ft = pad_now_ms();
      /* v1049F (user: "melee lock-on, then switching to a gun messes up free aim"): an RMB hold that began without a gun
       * (melee = the game's L1 lock-on) must not start free aim when the gun comes out mid-hold -- the R3 then lands
       * while the game is still locked on, cancels instead of starting the aim, and fa_state is inverted from then on
       * (aims on release). Such a hold is ignored until RMB is released; the next press is clean. PAD_KBM_FA_LATCH=0 off. */
      static int rmb_latch = 0, latch_on = -1; if (latch_on < 0) latch_on = pad_envi("PAD_KBM_FA_LATCH", 1);
      /* v1053L (user: "target someone in melee, then pull out a pistol and free aim -> it doesn't go over to free aim, I
       * have to press RMB again"): once the gun is out the RMB -> L1 bind is skipped (fa_gun), so the melee lock-on is
       * already let go; after PAD_KBM_FA_LATCH_GAP_MS (250) without L1 the latch clears and the held RMB starts free aim
       * (the same L1-off -> gap -> R3 order as the v1044S lock -> free-aim switch). 0 = old: press RMB again. */
      static unsigned long long latch_gun_t = 0; static int latch_gap = -1;
      if (latch_gap < 0) latch_gap = pad_envi("PAD_KBM_FA_LATCH_GAP_MS", 250);
      if (!KD(VK_RBUTTON)) { rmb_latch = 0; latch_gun_t = 0; }
      else if (latch_on && !fa_gun && !rmb_latch) { rmb_latch = 1; }
      if (rmb_latch && fa_gun && latch_gap > 0) {
          if (!latch_gun_t) { latch_gun_t = ft; printf("[aim] v1053L gun out during a melee RMB hold -- free aim in %d ms\n", latch_gap); fflush(stdout); }
          else if (ft - latch_gun_t >= (unsigned long long)latch_gap) { rmb_latch = 0; latch_gun_t = 0; }
      } else if (!rmb_latch) latch_gun_t = 0;
      { static int lp = 0; if (rmb_latch && fa_gun && !lp && latch_gap <= 0) { printf("[aim] v1049F RMB held from melee -- release and press again for free aim\n"); fflush(stdout); } lp = rmb_latch && fa_gun; }
      /* v1049S (user: "aim, run, let go of RMB, stop running -> still aiming"; "no free aim unless I hold RMB"): sprinting
       * out of free aim desynced the R3 toggle (the game pauses free aim while sprinting and ignores R3 then). Now Shift
       * (sprint) while RMB is held first ENDS free aim (R3, run withheld until the pulse is through), then sprints;
       * releasing Shift with RMB still held aims again. Free aim = RMB held and not sprinting. PAD_KBM_FA_SPRINT=0 = off. */
      static int sp_on = -1; if (sp_on < 0) sp_on = pad_envi("PAD_KBM_FA_SPRINT", 1);
      const int fa_sprint = sp_on && (KD(VK_LSHIFT) || KD(VK_RSHIFT));
      const int rmb = fa_gun && KD(VK_RBUTTON) && !rmb_latch && !fa_sprint && !ov_mouse;   /* v1110V */
      s_fa_busy_pub = fa_state;   /* v1119L (last poll's state) */
      if (!fa_now || cmb_gun != 1) { fa_state = 0; mode = 0; }  /* gun put away / swapped: the game leaves free aim itself */
      else {
          if (mode == 0 && rmb) {
              if (asst_on && g_pad_aim_acq_fn) { mode = 1; lock_t0 = ft; has_t = 0; lost_t = 0; acc = 0; g_pad_aim_acq_fn(1);
                  printf("[aim] v1044K RMB: lock-on\n"); fflush(stdout); }
              else { mode = 2; if (g_pad_aim_acq_fn) { g_pad_aim_acq_fn(1); g_pad_aim_acq_fn(2); } }   /* v1044N: clear any old target first */
          }
          if (mode == 1) {
              if (!rmb) mode = 0;
              else {
                  const int has = g_pad_aim_acq_fn ? g_pad_aim_acq_fn(0) : 0;
                  if (has && !has_t) { has_t = ft; printf("[aim] v1044Q locked on after %llu ms\n", ft - lock_t0); fflush(stdout); }
                  /* v1044R: once locked, the mouse keeps counting even if the target drops (dies / leaves range) --
                   * v1044Q stopped counting then and the player was stuck in lock mode with a dead mouse */
                  const int settled = has_t && ft - has_t >= (unsigned long long)min_ms;
                  if (has) lost_t = 0; else if (has_t && !lost_t) lost_t = ft;
                  if (settled) acc += (raw_dx < 0 ? -raw_dx : raw_dx) + (raw_dy < 0 ? -raw_dy : raw_dy); else acc = 0;
                  if (settled && acc >= brk) {
                      /* v1044S (user: "locked on, try to free aim -> he just stops aiming; let go of RMB -> he starts
                       * aiming until I press Z, reversed"): R3 pressed WHILE L1 is held cancels the aim in this game, but
                       * fa_state was set to 1, so the release pulse then turned free aim ON. Now the lock -> free aim
                       * switch is the same sequence as the no-target case, which the logs show working: let go of L1,
                       * wait PAD_KBM_LOCK_GAP_MS (120), then the normal R3 pulse (mode 4 -> 2). The snap still aims the
                       * new free-aim camera at the locked target (g_pad_aim_acq_fn(2) keeps it). */
                      mode = 4; gap_end = ft + (unsigned long long)gap_ms; g_pad_aim_acq_fn(2);
                      printf("[aim] v1044S -> free aim from the lock (mouse moved): L1 off, R3 in %d ms\n", gap_ms); fflush(stdout);
                  } else if ((!has_t && ft - lock_t0 >= (unsigned long long)nt_ms) ||
                             (lost_t && ft - lost_t >= (unsigned long long)nt_ms)) {   /* v1044R: lock lost -> free aim too */
                      /* nobody to lock on to: let go of L1, free aim a beat later (mode 4 -> 2) */
                      mode = 4; gap_end = ft + (unsigned long long)gap_ms; g_pad_aim_acq_fn(2);
                      printf("[aim] v1044Q no target -> free aim\n"); fflush(stdout);
                  }
              }
          }
          if (mode == 4) {
              if (!rmb) mode = 0;
              else if (ft >= gap_end) mode = 2;
          }
          if (mode == 3) {
              if (!rmb) mode = 0;
              else if (ft >= handoff_end) { mode = 2; verify_at = ft + 400ull; retries = 0; }
          }
          /* v1044M: free aim not up 400 ms after the handoff (no free-aim camera) -> the R3 did not take: press it once more */
          if (retry_on && mode == 2 && rmb && fa_state && retries < 1 && ft >= verify_at && g_pad_aim_acq_fn && !g_pad_aim_acq_fn(3) &&
              ft - fa_last >= 180ull) {
              retries++; fa_last = ft; fa_pulse_end = ft + 90ull; verify_at = ft + 400ull;
              printf("[aim] v1044M free aim did not start -> R3 again\n"); fflush(stdout);
          }
          if (mode == 2 && !rmb) mode = 0;
          const int want = (mode == 2 || mode == 3);   /* mode 4 (gap after a no-target L1) keeps the current state */
          if (want != fa_state && ft - fa_last >= 180ull) {   /* toggle the game's free aim with a short R3 pulse */
              fa_state = want; fa_last = ft; fa_pulse_end = ft + 90ull;
              printf("[aim] v1044A free aim %s (R3 pulse)%s\n", want ? "ON" : "OFF", (!want && fa_sprint) ? " -- sprint" : ""); fflush(stdout);
          }
          /* v1049S: no run until the game has left free aim (R3 through + 150 ms) */
          if (fa_sprint && (fa_state || ft < fa_pulse_end + 150ull)) btns &= ~CELL_PAD_CTRL_CROSS;
      }
      if (mode == 1 || mode == 3) btns |= CELL_PAD_CTRL_L1;
      s_lock_act = (mode == 1 || mode == 3 || mode == 4);   /* v1044S: no camera turn during the L1 -> R3 gap either */
      if (ft < fa_pulse_end) btns |= CELL_PAD_CTRL_R3;
      { static int gl = -1; if (gl < 0) gl = pad_envi("PAD_KBM_GUN_LOCK", 0);   /* v1053F (user): no game lock-on with a gun */
        if (gl && fa_now && cmb_gun == 1 && KD('T')) btns |= CELL_PAD_CTRL_L1; }   /* v1044A: T = old lock-on (PAD_KBM_GUN_LOCK=1) */
      s_fa_act = fa_state; }
    int cmb_rs = 0; float cmb_rx = 0.0f, cmb_ry = 0.0f;
    /* v1130E EA PC combat (the original PC port's prompts): with the target locked (RMB):
     *   mouse forward = quick attack (stick up); mouse back, then forward = power attack; Q / E = quick attack (hooks);
     *   Shift + Q / E (held, release) = power attack; Caps Lock = block; X = strangle (L3); LMB + RMB = grab;
     *   grabbed: mouse forward / Q / E = grab attack, mouse left / right = shake + throw, arrow keys = drag (right stick).
     * Gesture size PAD_KBM_GESTURE_PX (60 px within 150 ms gaps), flick length PAD_KBM_FLICK_MS (90), power PAD_KBM_POWER_MS (140). */
    { static int gpx = -1, flick_ms = 90, power_ms = 140;
      if (gpx < 0) { gpx = pad_envi("PAD_KBM_GESTURE_PX", 60); flick_ms = pad_envi("PAD_KBM_FLICK_MS", 90); power_ms = pad_envi("PAD_KBM_POWER_MS", 140); }
      static float acx = 0.0f, acy = 0.0f; static unsigned long long m_t = 0, ch_until = 0, fl_end = 0; static float fl_x = 0.0f, fl_y = 0.0f;
      static int pq = 0, pe = 0, rst = 0; static float rst_x = 0.0f; static unsigned long long rst_t0 = 0; static int down_ms = -1;
      if (down_ms < 0) down_ms = pad_envi("PAD_KBM_POWER_DOWN_MS", 300);   /* v1130N: minimum charge (stick down) of a Shift+Q/E power attack */
      const unsigned long long t = pad_now_ms();
      if (cmb_fist) {
          /* v1130K EA PC (StrategyWiki controls page): Q / E / Numpad 7 8 9 = quick attack up-left / up / up-right, + Shift = power */
          const int n8 = KD(VK_NUMPAD8);
          const int shift = KD(VK_LSHIFT) || KD(VK_RSHIFT), q = KD('Q') || KD(VK_NUMPAD7) || n8, e = KD('E') || KD(VK_NUMPAD9) || n8;
          if (raw_dx || raw_dy) { if (t - m_t > 150ull) acx = acy = 0.0f; m_t = t; acx += (float)raw_dx; acy += (s_inv ? -(float)raw_dy : (float)raw_dy); }
          if (acy < -(float)gpx) {        /* forward */
              const int pw = t < ch_until && !cmb_grab;
              fl_end = t + (unsigned long long)(pw ? power_ms : flick_ms); fl_x = 0.0f; fl_y = -127.0f; ch_until = 0; acx = acy = 0.0f;
          } else if (acy > (float)gpx && !cmb_grab) { ch_until = t + 600ull; acx = acy = 0.0f; }   /* pull back = charge */
          else if (acy > (float)gpx && cmb_grab) { ch_until = t + 600ull; acx = acy = 0.0f; }   /* v1130J: towards you while grabbing = threaten (stick back), EA PC */
          else if (cmb_grab && (acx < -(float)gpx || acx > (float)gpx)) {   /* shake / throw */
              const int left = acx < 0.0f; if (left) pad_motion_trigger(MOT_LEFT); else pad_motion_trigger(MOT_RIGHT);
              fl_end = t + 250ull; fl_x = left ? -127.0f : 127.0f; fl_y = 0.0f; acx = acy = 0.0f;
          }
          if (cmb_grab && !rst && shift && ((q && !pq) || (e && !pe))) { rst = 1; rst_t0 = t; rst_x = q && !e ? 90.0f : (e && !q ? -90.0f : 0.0f); }   /* v1130T: Shift+Q/E grabbing = threaten (stick back) ...; v1130U Q left fist (up-right), E right fist (up-left) */
          else if (cmb_grab && rst) {
              if (!(q || e) || !shift) { rst = 0; fl_end = t + (unsigned long long)power_ms; fl_x = rst_x; fl_y = rst_x != 0.0f ? -90.0f : -127.0f;   /* ... punch on release of Q/E or Shift (v1130V: diagonal on the fist's side) */
                  printf("[combat] v1130T grab power punch %s (%llu ms threaten)\n", rst_x > 0.0f ? "left fist" : rst_x < 0.0f ? "right fist" : "both", t - rst_t0); fflush(stdout); }
          } else if (cmb_grab) {
              /* v1130O: Q / E while grabbing = up-left / up-right too (the grab attacks / hooks the tutorial asks for) */
              if (q && !pq) { fl_end = t + (unsigned long long)flick_ms; fl_x = e ? 0.0f : 90.0f; fl_y = -90.0f; printf("[combat] v1130O grab attack Q\n"); fflush(stdout); }
              if (e && !pe) { fl_end = t + (unsigned long long)flick_ms; fl_x = q ? 0.0f : -90.0f; fl_y = -90.0f; printf("[combat] v1130O grab attack E\n"); fflush(stdout); }
          } else if (!rst && shift && ((q && !pq) || (e && !pe))) { rst = 1; rst_t0 = t; rst_x = q && !e ? 90.0f : (e && !q ? -90.0f : 0.0f); }   /* v1130U: Q = left fist (up-right = left hook in this game), E = right; v1130N: Shift+Q/E press = power attack: stick down (charge) ... */
          else if (rst && (!(q || e) || !shift) && t - rst_t0 >= (unsigned long long)down_ms) {   /* v1130T: releasing Shift fires too */   /* ... held at least PAD_KBM_POWER_DOWN_MS (longer while the key is held = charge), then up */
              rst = 0; fl_end = t + (unsigned long long)power_ms; fl_x = rst_x; fl_y = -127.0f;
              printf("[combat] v1130N power attack (%llu ms charge)\n", t - rst_t0); fflush(stdout); }
          else if ((q && !pq) || (e && !pe)) { fl_end = t + (unsigned long long)flick_ms; fl_x = q && !e ? 90.0f : (e && !q ? -90.0f : 0.0f); fl_y = q && e ? -127.0f : -90.0f;
              printf("[combat] v1130O quick attack %s\n", q && !e ? "Q (up-right, left fist)" : e && !q ? "E (up-left, right fist)" : "up"); fflush(stdout); }
          pq = q; pe = e;
          const int ku = KD(VK_UP), kd = KD(VK_DOWN), kl = KD(VK_LEFT), kr = KD(VK_RIGHT);
          if (ku || kd || kl || kr) { cmb_rs = 1; cmb_rx = (float)(kr - kl) * 127.0f; cmb_ry = (float)(kd - ku) * 127.0f; }   /* arrows = right stick */
          else if (rst || t < ch_until) { cmb_rs = 1; static int gtx = -1, gty = 64; if (gtx < 0) { gtx = pad_envi("PAD_KBM_GRAB_TX", 110); gty = pad_envi("PAD_KBM_GRAB_TY", 64); }   /* v1130Y: the threaten stick decides the raised fist; 45 deg read as plain 'down' */
              const int gs = rst && cmb_grab && rst_x != 0.0f;
              cmb_rx = gs ? (rst_x > 0.0f ? (float)gtx : -(float)gtx) : 0.0f; cmb_ry = gs ? (float)gty : 127.0f; }   /* v1130V: grab threaten pulled back to the fist's side */
          else if (t < fl_end) { cmb_rs = 1; cmb_rx = fl_x; cmb_ry = fl_y; }
          else cmb_rs = 2;                                                              /* centred, mouse muted */
      } else { pq = pe = rst = 0; fl_end = ch_until = 0; acx = acy = 0.0f; }
    }
    /* v1042G layout (matches the rewritten prompts, gf_prompts.c): X+W forward, X+S towards you, X+A / X+D left / right,
     * X+SPACE up (lift while grabbing = LMB/R1 held, otherwise rotate up), X+CTRL down and in (with A / D: down-left /
     * down-right), X+E twist. */
    { const int cur[7] = { KD('W'), KD('S'), KD('A'), KD('D'), KD(VK_SPACE), KD('E'), KD(VK_LCONTROL) || KD(VK_RCONTROL) };
      const int grab = ((btns | (merge ? hs->buttons : 0)) & CELL_PAD_CTRL_R1) != 0;
      /* v1043K (user): while grabbing, a plain TAP of X (no other gesture key during the hold, released within
       * PAD_KBM_XLIFT_MS = 500 ms) = lift / pick up (was X+SPACE, which still works). Fired on release so X+W / X+A ...
       * do not lift first. PAD_KBM_XLIFT=0 = off. */
      { static int xl_on = -1, xl_ms = 500; static int x_prev = 0, x_used = 0, x_grab = 0; static unsigned long long x_t0 = 0;
        if (xl_on < 0) { xl_on = pad_envi("PAD_KBM_XLIFT", 1); xl_ms = pad_envi("PAD_KBM_XLIFT_MS", 500); }
        const unsigned long long xt = pad_now_ms();
        if (mot && !x_prev) { x_t0 = xt; x_used = 0; x_grab = 0; }
        if (mot) { for (int i = 0; i < 7; i++) if (cur[i]) x_used = 1; if (grab) x_grab = 1; }
        if (!mot && x_prev && xl_on && x_grab && grab && !x_used && xt - x_t0 <= (unsigned long long)xl_ms) {
            pad_motion_trigger(MOT_LIFT);
            printf("[motion] v1043K X tap while grabbing -> lift\n"); fflush(stdout);
        }
        x_prev = mot; }
      if (mot) {
          const int ctrl = cur[6];
          if (cur[0] && !s_mprev[0]) pad_motion_trigger(MOT_FORWARD);
          if (cur[1] && !s_mprev[1]) pad_motion_trigger(MOT_BACK);
          if (cur[2] && !s_mprev[2]) pad_motion_trigger(ctrl ? MOT_KNEE_L : MOT_LEFT);
          if (cur[3] && !s_mprev[3]) pad_motion_trigger(ctrl ? MOT_KNEE_R : MOT_RIGHT);
          if (cur[4] && !s_mprev[4]) pad_motion_trigger(grab ? MOT_LIFT : MOT_TILT);
          if (cur[5] && !s_mprev[5]) pad_motion_trigger(MOT_TWIST);
          if (ctrl && !s_mprev[6]) pad_motion_trigger(cur[2] ? MOT_KNEE_L : cur[3] ? MOT_KNEE_R : MOT_DOWN);
      }
      /* v1046A (user: "slams on SPACE instead of X+W"): while grabbing (R1 held), a plain SPACE press = the forward
       * gesture (wall slam / push / toss over a ledge), and SPACE's normal Cross button is withheld during the grab.
       * X+W still works. PAD_KBM_SPACE_SLAM=0 = off. */
      { static int sp_on = -1, sp_prev = 0; if (sp_on < 0) sp_on = pad_envi("PAD_KBM_SPACE_SLAM", 0);   /* v1130H: off -- EA PC slams with W */
        const int sp = KD(VK_SPACE);
        if (sp_on && !mot && grab) {
            if (sp && !sp_prev) { pad_motion_trigger(MOT_FORWARD); printf("[motion] v1046A SPACE while grabbing -> slam\n"); fflush(stdout); }
            if (sp) btns &= ~(CELL_PAD_CTRL_CROSS | CELL_PAD_CTRL_TRIANGLE);   /* v1130E: Space = Triangle */
        }
        sp_prev = sp; }
      /* v1049Q (user: prompt says Q/E slams to the sides, but only X+A / X+D did): while grabbing (R1 held), Q / E = the
       * left / right gesture (the same as X+A / X+D: side slam / swing), and Q's L2 (weapon wheel) / E's Triangle are
       * withheld during the grab. PAD_KBM_GRAB_QE=0 = off. */
      { static int qe_on = -1, q_prev = 0, e_prev = 0; if (qe_on < 0) qe_on = pad_envi("PAD_KBM_GRAB_QE", 0);   /* v1130E: Q / E while grabbing = grab attack (EA PC) */
        const int q = KD('Q'), e = KD('E');
        if (qe_on && !mot && grab) {
            if (q && !q_prev) { pad_motion_trigger(MOT_LEFT);  printf("[motion] v1049Q Q while grabbing -> slam left\n");  fflush(stdout); }
            if (e && !e_prev) { pad_motion_trigger(MOT_RIGHT); printf("[motion] v1049Q E while grabbing -> slam right\n"); fflush(stdout); }
            if (q) btns &= ~CELL_PAD_CTRL_L2;
            if (e) btns &= ~CELL_PAD_CTRL_TRIANGLE;
        }
        q_prev = q; e_prev = e; }
      for (int i = 0; i < 7; i++) s_mprev[i] = cur[i]; }
    /* v1130F EA-style gestures (no modifier key; the EA PC port has no SIXAXIS, these moves map onto its keys):
     *   grabbing (RMB + LMB): S = lift, W = forward (wall slam / over a ledge), mouse towards you = back (in the mouse block
     *   above), mouse left / right = swing / throw (above), Ctrl = knee (Ctrl+A / Ctrl+D = knee left / right),
     *   Y = twist (garrote neck snap), S tap = lift, mouse back = threaten. WASD keep moving the player while grabbing.
     *   locked on (RMB) without a grab: Ctrl = down; with a gun drawn also Q / E = left / right (pistol whip), Space = tilt.
     * An optional gesture key (PAD_MOTION_KEY=G, off by default) still works on top. PAD_KBM_EA_GESTURES=0 = off. Log [motion] v1130F. */
    static int s_ea_grab = 0;
    { static int on = -1; static int pw = 0, ps = 0, pc = 0, pe = 0, pq = 0, psp = 0, pa = 0, pd = 0;
      if (on < 0) on = pad_envi("PAD_KBM_EA_GESTURES", 1);
      const int rmb = KD(VK_RBUTTON) && !s_free && !ov_mouse && !mot;
      const int w = KD('W'), s = KD('S'), c = KD(VK_LCONTROL) || KD(VK_RCONTROL), e = KD('E'), q = KD('Q'), sp = KD(VK_SPACE);
      s_ea_grab = on && cmb_grab;
      if (on && rmb) {
          const char* what = 0;
          if (cmb_grab) {
              { static unsigned long long s_t0 = 0; static int s_hold = 0;   /* v1130J: S TAP = lift (holding S walks back: no headbutt) */
                const unsigned long long tn = pad_now_ms();
                if (s && !ps) { s_t0 = tn; s_hold = 1; }
                if (!s && ps && s_hold && tn - s_t0 <= 200ull) { pad_motion_trigger(MOT_LIFT); what = "S tap -> lift"; }
                if (!s) s_hold = 0; }
              if (w && !pw) { pad_motion_trigger(MOT_FORWARD); what = "W -> forward"; }
              { static int py = 0; const int y = KD('Y');   /* v1130J: Y = twist (garrote neck snap); E stays the grab attack */
                if (y && !py) { pad_motion_trigger(MOT_TWIST); what = "Y -> twist"; } py = y; }
              /* v1130AA (user: "A / D while grabbing should move"): no side gesture -- if (!c && KD('A') && !pa) { pad_motion_trigger(MOT_LEFT); what = "A -> left (slam into low objects)"; }   (EA: A / D slam) */
              /* v1130AA (user: "A / D while grabbing should move"): no side gesture -- if (!c && KD('D') && !pd) { pad_motion_trigger(MOT_RIGHT); what = "D -> right (slam into low objects)"; } */
          }
          if (c && !pc) { const int a = KD('A'), d = KD('D');
              pad_motion_trigger(cmb_grab && a && !d ? MOT_KNEE_L : cmb_grab && d && !a ? MOT_KNEE_R : MOT_DOWN); what = "Ctrl -> down / knee"; }
          if (cmb_grab && (KD('X') || KD(VK_MULTIPLY))) btns |= CELL_PAD_CTRL_L3 | CELL_PAD_CTRL_R3;   /* v1130J: X / Num* = strangle (PS3: hold L3 + R3) */
          if (cmb_grab) { static int pl = 0, pr = 0;   /* v1130K EA PC: swing = Insert / Num4 (left), Page Up / Num6 (right) */
              const int l = KD(VK_INSERT) || KD(VK_NUMPAD4), rr = KD(VK_PRIOR) || KD(VK_NUMPAD6);
              if (l && !pl) { pad_motion_trigger(MOT_LEFT); what = "Insert/Num4 -> swing left"; }
              if (rr && !pr) { pad_motion_trigger(MOT_RIGHT); what = "PgUp/Num6 -> swing right"; }
              pl = l; pr = rr; }
          if (!cmb_grab && cmb_gun == 1) {
              if (q && !pq) { pad_motion_trigger(MOT_LEFT); what = "Q -> left"; }
              if (e && !pe) { pad_motion_trigger(MOT_RIGHT); what = "E -> right"; }
              if (sp && !psp) { pad_motion_trigger(MOT_TILT); what = "Space -> tilt"; }
          }
          if (what) { printf("[motion] v1130F %s\n", what); fflush(stdout); }
      }
      pw = w; ps = s; pc = c; pe = e; pq = q; psp = sp; pa = KD('A'); pd = KD('D'); }
    /* v1049H (user): H = hide / unhide the weapon, R = reload. In this game d-pad DOWN holsters, d-pad UP draws the
     * weapon -- and reloads while a gun is out. H taps DOWN when a weapon is out and UP when it is away (one 90 ms tap
     * per press); R with a gun drawn = UP (reload) instead of its R2 (R keeps R2 / melee charge otherwise). In game only
     * (not in menus, not during a motion gesture). PAD_KBM_HR=0 = off. */
    { static int hr_on = -1, h_prev = 0; static unsigned long long h_end = 0; static u16 h_btn = 0;
      if (hr_on < 0) hr_on = pad_envi("PAD_KBM_HR", 1);
      if (hr_on && !mot && !s_free && cmb_gun >= 0) {
          const unsigned long long ht = pad_now_ms();
          const int h = KD('H');
          /* v1052H (user: "H doesn't hide the gun consistently, I have to spam it 5-6 times"): the log showed every draw
           * (UP) worked at once but a 90 ms DOWN tap rarely holstered. Now the d-pad button is HELD until the game reports
           * the weapon state flipped (g_pad_drawn_fn), at least PAD_KBM_H_MIN_MS (60) and at most PAD_KBM_H_MAX_MS (700);
           * pressing H again while it is held cancels. Log "[keys] v1052H ... after N ms". */
          static int h_want = -1; static unsigned long long h_t0 = 0; static int h_min = -1, h_max = 700;
          if (h_min < 0) { h_min = pad_envi("PAD_KBM_H_MIN_MS", 60); h_max = pad_envi("PAD_KBM_H_MAX_MS", 700); }
          if (h && !h_prev) {
              const int drawn = g_pad_drawn_fn ? g_pad_drawn_fn() : (cmb_gun == 1);
              if (h_end && ht < h_end) { h_end = 0; h_want = -1; }   /* second press while holding: stop */
              else {
                  h_btn = drawn ? CELL_PAD_CTRL_DOWN : CELL_PAD_CTRL_UP; h_t0 = ht; h_end = ht + (unsigned long long)h_max;
                  if (!drawn) s_draw_req_ms = ht;   /* v1053N2: H = draw -> RMB within the window aims */
                  h_want = drawn ? 0 : 1;
                  printf("[keys] v1049H H -> %s weapon\n", drawn ? "hide" : "draw"); fflush(stdout);
              }
          }
          h_prev = h;
          if (h_end && ht < h_end) {
              const int now_drawn = g_pad_drawn_fn ? g_pad_drawn_fn() : -1;
              if (now_drawn == h_want && ht - h_t0 >= (unsigned long long)h_min) {
                  printf("[keys] v1052H weapon %s after %llu ms\n", h_want ? "drawn" : "hidden", ht - h_t0); fflush(stdout);
                  h_end = 0;
              } else btns |= h_btn;
          } else if (h_end) {
              printf("[keys] v1052H %s did not happen within %d ms\n", h_want ? "draw" : "hide", h_max); fflush(stdout);
              h_end = 0;
          }
          if (cmb_gun == 1 && KD('R')) { btns &= ~CELL_PAD_CTRL_R2; btns |= CELL_PAD_CTRL_UP; }
      } else h_prev = KD('H'); }
    /* v1053D (user: "change driving so it's actually WASD and not shift and F", "make space the drift button instead of
     * C"). Godfather's car: Cross = gas, Square = brake / reverse, Circle = handbrake. While driving: W = Cross,
     * S = Square, SPACE = Circle (its Cross is withheld), A / D steer (left stick X), W / S leave the stick Y alone.
     * Driving = a player exists, the on-foot camera (gf_footcam.c, func_00055E40) has not run for PAD_KBM_CAR_MS
     * (250 ms), and no menu / cutscene / free-aim / grab / gesture / RMB. Shift / F / C keep working. Log [drive].
     * PAD_KBM_CAR=0 = off. */
    static int s_car = 0;
    { static int car_on = -1;
      if (car_on < 0) { car_on = pad_envi("PAD_KBM_CAR", 1);   /* v1053V: real in-car flag (g_pad_incar_fn) */ }
      extern volatile int g_uiov_menu_open;
      const int cutscene = g_pad_cutscene_fn && g_pad_cutscene_fn();
      const int menu = (g_pad_menu_fn && g_pad_menu_fn()) || g_uiov_menu_open;   /* v1053V: the car flag implies a player */
      const int car = car_on && g_pad_incar_fn && g_pad_incar_fn() && !menu && !cutscene && !mot && !s_free;   /* v1053V */
      if (car != s_car) { s_car = car; printf("[drive] v1053D car controls %s (W gas, S brake, SPACE drift)\n", car ? "ON" : "off"); fflush(stdout); }
      if (car) {
          /* v1130K EA PC: Space = enter / exit (Triangle, its normal bind); handbrake = Ctrl / C / RMB; LAlt = change camera */
          if (KD(VK_LMENU)) { btns &= ~CELL_PAD_CTRL_L2; btns |= CELL_PAD_CTRL_UP; }
          if (KD(VK_LSHIFT) || KD(VK_RSHIFT) || KD(VK_RETURN)) btns |= CELL_PAD_CTRL_CROSS;
          if (KD('W')) btns |= CELL_PAD_CTRL_CROSS;
          if (KD('S')) btns |= CELL_PAD_CTRL_SQUARE;
          /* v1130G EA PC car (D_ bindings): Up = gas, Down = brake, RMB / Ctrl = handbrake, LMB = L3, MMB = R3 */
          if (KD(VK_UP)) btns |= CELL_PAD_CTRL_CROSS;
          if (KD(VK_DOWN)) btns |= CELL_PAD_CTRL_SQUARE;
          if (KD(VK_RBUTTON) || KD(VK_LCONTROL)) { btns &= ~CELL_PAD_CTRL_L1; btns |= CELL_PAD_CTRL_CIRCLE; }
          if (KD('V')) { btns &= ~CELL_PAD_CTRL_R2; btns |= CELL_PAD_CTRL_R3; }   /* v1130X EA PC car: V = look back (R3) ... */
          if (KD('F')) { btns &= ~CELL_PAD_CTRL_R3; btns |= CELL_PAD_CTRL_R2; }   /* ... F = shoot from the car (R2); glyphs R2 = V/F, R3 = F/V */
          { extern float (*volatile g_pad_car_speed_fn)(void);   /* v1130T: Space gets out only when the car is (almost) stopped */
            static int sp_log = 0; const float v = g_pad_car_speed_fn ? g_pad_car_speed_fn() : -1.0f;
            if (KD(VK_SPACE) && !KD('T') && v > 1.5f) { btns &= ~CELL_PAD_CTRL_TRIANGLE; if (!sp_log) { sp_log = 1; printf("[drive] v1130T Space ignored while moving (%.1f m/s)\n", v); fflush(stdout); } }
            if (!KD(VK_SPACE)) sp_log = 0; }
          if (KD(VK_LBUTTON)) { btns &= ~CELL_PAD_CTRL_R1; btns |= CELL_PAD_CTRL_L3; }
          if (KD(VK_MBUTTON)) { btns &= ~CELL_PAD_CTRL_DOWN; btns |= CELL_PAD_CTRL_R3; }
      } }
    float lx = 0.0f, ly = 0.0f;
    /* v1043Y (user): in the main menu / pause menu WASD = d-pad (discrete steps), not the left stick. Menu = no player
     * (g_pad_weapon_fn < 0: frontend, creator, loading) or the title's menu hook (pause). PAD_KBM_MENU_DPAD=0 = off. */
    static int s_menu_dpad = -1; if (s_menu_dpad < 0) s_menu_dpad = pad_envi("PAD_KBM_MENU_DPAD", 1);
    const int menu_dpad = s_menu_dpad && ((g_pad_weapon_fn && cmb_gun < 0) || (g_pad_menu_fn && g_pad_menu_fn()));
    if (!mot && menu_dpad) {
        if (KD('W')) btns |= CELL_PAD_CTRL_UP;    if (KD('S')) btns |= CELL_PAD_CTRL_DOWN;
        if (KD('A')) btns |= CELL_PAD_CTRL_LEFT;  if (KD('D')) btns |= CELL_PAD_CTRL_RIGHT;
    } else if (!mot) {
    if (KD('A')) lx -= 1.0f;   /* v1130I: A / D still move while grabbing (the press also fires the side slam) */
    if (KD('D')) lx += 1.0f;
    if (KD(VK_DELETE) && !KD('A')) lx -= 1.0f;   /* v1130G EA PC: Home / End / Delete / Page Down = left stick too */
    if (KD(VK_NEXT) && !KD('D')) lx += 1.0f;
    if (KD(VK_HOME) && !KD('W') && !s_car) ly -= 1.0f;
    if (KD(VK_END) && !KD('S') && !s_car) ly += 1.0f;
    if (s_car && KD(VK_LEFT) && !KD('A')) lx -= 1.0f;    /* v1130G EA PC: arrows steer in a car */
    if (s_car && KD(VK_RIGHT) && !KD('D')) lx += 1.0f;
    if (KD('W') && !s_car) ly -= 1.0f;   /* v1053D: W / S are gas / brake in a car */
    if (KD('S') && !s_car) ly += 1.0f;
    }
    const int moving = (lx != 0.0f || ly != 0.0f);
    /* v1115W (user: "toggle slow walk with CTRL, not hold; sprinting cancels it"): a Ctrl PRESS toggles walking
     * (PAD_KBM_WALK deflection) on / off; sprinting (Cross held while moving = Shift by default) turns it off.
     * Ctrl used for an X+Ctrl move does not toggle: no toggle while X (the motion key) is held, and a toggle is
     * undone if X goes down while that same Ctrl press is still held. Menus and the car never toggle (the car also
     * clears it). PAD_KBM_WALK_TOGGLE=0 = the old hold-Ctrl walk. Log [walk]. */
    static int s_wt_on = -1, s_wt = 0, s_ctrl_prev = 0, s_ctrl_flip = 0;
    if (s_wt_on < 0) s_wt_on = pad_envi("PAD_KBM_WALK_TOGGLE", 1);
    const int ctrl_now = KD(VK_TAB);   /* v1130E EA PC scheme: Tab toggles walking (auto-run otherwise) */
    int walk_now;
    if (s_wt_on) {
        if (ctrl_now && !s_ctrl_prev && !mot && !menu_dpad && !s_car) {
            s_wt = !s_wt; s_ctrl_flip = 1;
            printf("[walk] v1115W walk %s (Ctrl)\n", s_wt ? "ON" : "off"); fflush(stdout);
        }
        if (!ctrl_now) s_ctrl_flip = 0;
        if (mot && ctrl_now && s_ctrl_flip) {
            s_wt = !s_wt; s_ctrl_flip = 0;
            printf("[walk] v1115W Ctrl was for an X+Ctrl move -> walk back %s\n", s_wt ? "ON" : "off"); fflush(stdout);
        }
        if (s_wt && ((moving && (btns & CELL_PAD_CTRL_CROSS) && !menu_dpad) || s_car)) {
            s_wt = 0;
            printf("[walk] v1115W walk off (%s)\n", s_car ? "car" : "sprint"); fflush(stdout);
        }
        walk_now = s_wt;
    } else {
        walk_now = KD(VK_TAB);
    }
    s_ctrl_prev = ctrl_now;
    if (moving) {
        const float mag = walk_now ? (float)s_walk : 127.0f;
        const float n = (lx != 0.0f && ly != 0.0f) ? 0.70710678f : 1.0f;
        lx *= mag * n; ly *= mag * n;
    }
    /* mouse -> right stick: smoothed velocity, scaled so PAD_MOUSE_SPEED px/s = full deflection, plus a
     * fixed offset past the game's stick dead zone so slow moves still turn. */
    const int alt = KD(VK_MENU) && (KD(VK_TAB) || KD(VK_F4) || KD(VK_ESCAPE));   /* v1130L: Alt alone = L2 (weapon wheel, EA PC scheme); only Alt+Tab / F4 / Esc free the mouse */
    const int clicks = KD(VK_LBUTTON) || KD(VK_RBUTTON) || KD(VK_MBUTTON);
#undef KD
    long dx = 0, dy = 0; int have_mouse = 0;
    extern volatile int g_uiov_menu_open;
    HWND fg = GetForegroundWindow();
    if (fg && !alt && !s_free && !g_uiov_menu_open && !mcur && !s_shop_ptr) {   /* v1107U: !mcur = free pointer in menus; v1130Q buy screen */
        RECT rc; POINT c, p;
        if (GetClientRect(fg, &rc) && rc.right > 8 && rc.bottom > 8 && GetCursorPos(&p)) {
            c.x = rc.right / 2; c.y = rc.bottom / 2; ClientToScreen(fg, &c);
            if (g_pad_mouse_captured) {
                if (g_pad_raw_ok) { dx = raw_dx; dy = raw_dy; }
                else { dx = p.x - c.x; dy = p.y - c.y; }
            }
            if (p.x != c.x || p.y != c.y) SetCursorPos(c.x, c.y);
            g_pad_mouse_captured = 1; have_mouse = 1;
            /* v1043K (user: "the mouse gets unfocused when I use the dragging"): the cursor is only re-centred once per
             * poll, so a fast drag (grab swings) could leave the window between polls. Confine it to the client area
             * while the mouse is captured (PAD_MOUSE_CLIP=0 = off); released below when capture ends. */
            { static int clip = -1; if (clip < 0) clip = pad_envi("PAD_MOUSE_CLIP", 1);
              DWORD fpid = 0; GetWindowThreadProcessId(fg, &fpid);
              if (clip && fpid == GetCurrentProcessId()) { POINT tl = {0, 0}, br = {rc.right, rc.bottom};
                          ClientToScreen(fg, &tl); ClientToScreen(fg, &br);
                          RECT cr = {tl.x, tl.y, br.x, br.y}, cur;
                          if (!GetClipCursor(&cur) || cur.left != cr.left || cur.top != cr.top || cur.right != cr.right || cur.bottom != cr.bottom)
                              ClipCursor(&cr);
                          s_pad_clipped = 1; } }
        }
    }
    if (!have_mouse && s_pad_clipped) { ClipCursor(NULL); s_pad_clipped = 0; }
    if (!have_mouse) { g_pad_mouse_captured = 0; dx = dy = 0; }
    { /* v1042M mouse flick while the motion key is held (PAD_MOTION_MOUSE_PX, default 60 counts) */
      static float ax = 0.0f, ay = 0.0f; static int thr = -1;
      if (thr < 0) { thr = pad_envi("PAD_MOTION_MOUSE_PX", 60); if (thr < 5) thr = 5; }
      if (mot && have_mouse) {
          const float k = (float)exp(-dt / 0.15);
          ax = ax * k + (float)dx; ay = ay * k + (float)dy;
          const float fx = ax < 0 ? -ax : ax, fy = ay < 0 ? -ay : ay;
          if (fy >= (float)thr && fy >= fx)      { pad_motion_trigger(ay < 0 ? MOT_FORWARD : MOT_BACK); ax = ay = 0.0f; }
          else if (fx >= (float)thr)             { pad_motion_trigger(ax < 0 ? MOT_LEFT : MOT_RIGHT); ax = ay = 0.0f; }
          dx = dy = 0; s_vx = s_vy = 0.0f;      /* no camera turn while gesturing */
      } else { ax = ay = 0.0f; }
    }
    /* v1044C (user: free aim "EXTREMELY slow" -- the game caps the stick turn rate): mouse X goes straight into the
     * game's free-aim yaw (degrees, PAD_KBM_AIM_DEG per mouse count, default 0.08; PAD_KBM_AIM_DIRECT=0 = stick). */
    int aim_direct = 0;
    { static int on = -1; static float dpc = 0.08f;
      if (on < 0) { on = pad_envi("PAD_KBM_AIM_DIRECT", 1); const char* e = getenv("PAD_KBM_AIM_DEG"); if (e && *e) dpc = (float)atof(e); }
      static int iy = -1, ix = 0; if (iy < 0) { iy = pad_envi("PAD_KBM_AIM_INVY", 0); ix = pad_envi("PAD_KBM_AIM_INVX", 0); }
      if (on && s_fa_act && have_mouse && !mot && g_pad_aim_fn)
          aim_direct = g_pad_aim_fn((float)dx * dpc * (ix ? -1.0f : 1.0f), (float)dy * dpc * (iy ? -1.0f : 1.0f)); }
    /* v1046L (user: on-foot mouse look "way too slow and restricted, want it like GTA"): the game's follow camera
     * (func_0006A7F8) caps the stick turn rate, clamps the yaw offset and swings back behind the player. With KBM the
     * mouse now sets that camera's angles directly (gf_footcam.c): GF_FOOTCAM_DEG (0.12) degrees per mouse count
     * (scaled by the F10 mouse speed), GF_FOOTCAM_INVX / GF_FOOTCAM_INVY = 1 flip an axis, GF_FOOTCAM=0 = old stick look.
     * Not while free-aiming, locked on, grabbing or doing a motion gesture (those keep their own mouse handling). */
    int look_direct = 0;
    { static int on = -1, ix = 0, iy = 0; static float dpc0 = 0.12f;
      if (on < 0) { on = pad_envi("GF_FOOTCAM", 1); const char* e = getenv("GF_FOOTCAM_DEG"); if (e && *e) dpc0 = (float)atof(e);
                    ix = pad_envi("GF_FOOTCAM_INVX", 0); iy = pad_envi("GF_FOOTCAM_INVY", 0); }
      float dpc = dpc0;
      { extern volatile int g_ui_mouse_speed; if (g_ui_mouse_speed > 0) dpc = dpc0 * (float)g_ui_mouse_speed / 450.0f; }
      /* v1046M (user: "can't use the weapon wheel now"): holding L2 (Q) opens the weapon wheel, picked with the right
       * stick -> while L2 is held the mouse drives the right stick again. */
      const int wheel = (btns & CELL_PAD_CTRL_L2) != 0;
      if (on && !wheel && !aim_direct && !s_fa_act && !s_lock_act && have_mouse && !mot && !cmb_rs && !cmb_grab && g_input_kbm == 1 && g_pad_look_fn)
          look_direct = g_pad_look_fn((float)dx * dpc * (ix ? -1.0f : 1.0f), (float)dy * dpc * (iy ? -1.0f : 1.0f)); }
    const float a = (float)exp(-dt / s_tau);
    s_vx = s_vx * a + (float)((double)((aim_direct || look_direct) ? 0 : dx) / dt) * (1.0f - a);
    s_vy = s_vy * a + (float)((double)((aim_direct || look_direct) ? 0 : dy) / dt) * (1.0f - a);
    if (s_inv) { /* applied below */ }
    float rx = 0.0f, ry = 0.0f;
    /* v1044B (user: "I move the camera way too slowly in free aim"): the free-aim crosshair turns slower per stick unit than
     * the normal camera, so while free aim is on the same mouse speed gives PAD_KBM_FREEAIM_SENS (default 3.0) times the
     * stick deflection. If the game's own maximum free-aim turn rate is the limit, full deflection is reached sooner but
     * cannot go faster -- then the turn rate itself needs a game-side patch. */
    static float fa_sens = -1.0f;
    if (fa_sens < 0.0f) { const char* e = getenv("PAD_KBM_FREEAIM_SENS"); fa_sens = (e && *e) ? (float)atof(e) : 3.0f; if (fa_sens < 0.1f) fa_sens = 0.1f; }
    const float spd = s_fa_act ? s_speed / fa_sens : s_speed;
    { float fx = s_vx / spd, fy = (s_inv ? -s_vy : s_vy) / spd;    /* PS3 stick: down = + */
      const float m = sqrtf(fx * fx + fy * fy);
      if (m > 0.01f) {
          const float span = 127.0f - (float)s_dz;
          float out = (float)s_dz + (m > 1.0f ? 1.0f : m) * span;
          rx = fx / m * out; ry = fy / m * out;
      } else { s_vx = s_vy = 0.0f; } }
    if (aim_direct || look_direct) { rx = 0.0f; ry = 0.0f; }   /* v1044F / v1046L: the camera angles are written directly */
    if (s_lock_act) { rx = 0.0f; ry = 0.0f; s_vx = s_vy = 0.0f; }   /* v1044K: locked on -- no zone-target flicks */
    { /* v1043K (user): while grabbing, a quick mouse DRAG left / right = the grab swing (was Q / E; PAD_KBM_QE=1 brings
       * the keys back): the right stick is held to that side for PAD_KBM_DRAG_MS (250) and the SIXAXIS swing gesture
       * fires once. Threshold PAD_KBM_DRAG_PX (60 mouse counts within ~0.15 s). Only while grabbing; the mouse still does
       * not turn the camera then. */
      /* v1043K-2 (user: "left drag is not reliable"): threshold 60 -> 40 counts, hold 250 -> 350 ms, and the swing keeps
       * holding while the drag continues the same way (like holding Q / E did), up to 1.2 s. */
      static float gx = 0.0f; static int thr = -1, hold_ms = 350, sdir = 0; static unsigned long long s_until = 0, s_start = 0;
      if (thr < 0) { thr = pad_envi("PAD_KBM_DRAG_PX", 40); if (thr < 5) thr = 5; hold_ms = pad_envi("PAD_KBM_DRAG_MS", 350); }
      const unsigned long long gt = pad_now_ms();
      if (cmb_grab && !mot && have_mouse) {
          const float k = (float)exp(-dt / 0.15);
          gx = gx * k + (float)dx;
          if (sdir && gt >= s_until) sdir = 0;
          if (sdir && (long)sdir * dx > 0 && gt - s_start < 1200ull) s_until = gt + (unsigned long long)hold_ms;   /* still dragging */
          if (!sdir && (gx >= (float)thr || gx <= -(float)thr)) {
              sdir = gx < 0.0f ? -1 : 1; s_until = gt + (unsigned long long)hold_ms; s_start = gt; gx = 0.0f;
              pad_motion_trigger(sdir < 0 ? MOT_LEFT : MOT_RIGHT);
              printf("[motion] v1043K grab swing %s (mouse drag)\n", sdir < 0 ? "left" : "right"); fflush(stdout);
          }
      } else { gx = 0.0f; sdir = 0; s_until = 0; }
      if (sdir && gt < s_until) { cmb_rs = 1; cmb_rx = (float)sdir * 127.0f; cmb_ry = 0.0f; }
    }
    /* v1130G EA PC: the arrow keys are the right stick in game (camera; drag / gestures while locked on, above). Not in
     * menus (d-pad there) or in a car (gas / brake / steer, below). */
    if (!cmb_rs && !menu_dpad && !s_car && !s_free && !s_shop_ptr && cmb_gun >= 0) {   /* v1130R: not on the buy screen (arrows = d-pad there) */
        const int ku = (GetAsyncKeyState(VK_UP) & 0x8000) != 0, kd = (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0, kl = (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0, kr = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
        if (ku || kd || kl || kr) { cmb_rs = 1; cmb_rx = (float)(kr - kl) * 127.0f; cmb_ry = (float)(kd - ku) * 127.0f; }
    }
    if (cmb_rs) { rx = cmb_rx; ry = cmb_ry; s_vx = s_vy = 0.0f; }   /* v1042X: locked-on melee owns the right stick */
    const int looking = (rx != 0.0f || ry != 0.0f) || cmb_rs == 1;
    if (btns || moving || clicks || (raw_dx < 0 ? -raw_dx : raw_dx) + (raw_dy < 0 ? -raw_dy : raw_dy) > 3) pad_note_device(1);
    else if (g_input_kbm < 0 && !merge) pad_note_device(1);
    { static u16 kb_prev = 0; const u16 kb = (u16)(btns & MM_MASK);   /* v1114P: a new key press beats the pointer */
      if (s_mm_drive && (kb & ~kb_prev)) {
          pad_menu_reset();
          static int nl = 0; if (nl < 20) { nl++; printf("[cellPad] v1114P menu: key press -> keyboard has the menu (pointer pulses dropped)\n"); fflush(stdout); } }
      kb_prev = kb; }
    if (s_mm_drive) btns = (u16)((btns & ~MM_MASK) | s_mm_out | s_mm_hold);   /* v1107X: the menu pointer owns d-pad + Cross */
    if (merge) {
        hs->buttons |= btns;
        if (moving)  { hs->analog_lx = pad_axis(lx); hs->analog_ly = pad_axis(ly); }
        if (looking) { hs->analog_rx = pad_axis(rx); hs->analog_ry = pad_axis(ry); }
        if (btns & CELL_PAD_CTRL_L2) hs->trigger_l2 = 255;
        if (btns & CELL_PAD_CTRL_R2) hs->trigger_r2 = 255;
    } else {
        hs->buttons = btns; hs->connected = 1;
        hs->analog_lx = pad_axis(lx); hs->analog_ly = pad_axis(ly);
        hs->analog_rx = pad_axis(rx); hs->analog_ry = pad_axis(ry);
        hs->trigger_l2 = (u8)((btns & CELL_PAD_CTRL_L2) ? 255 : 0);
        hs->trigger_r2 = (u8)((btns & CELL_PAD_CTRL_R2) ? 255 : 0);
    }
    const u16 b = hs->buttons;
    hs->press_up = (b & CELL_PAD_CTRL_UP) ? 255 : 0;             hs->press_down = (b & CELL_PAD_CTRL_DOWN) ? 255 : 0;
    hs->press_left = (b & CELL_PAD_CTRL_LEFT) ? 255 : 0;         hs->press_right = (b & CELL_PAD_CTRL_RIGHT) ? 255 : 0;
    hs->press_triangle = (b & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0; hs->press_circle = (b & CELL_PAD_CTRL_CIRCLE) ? 255 : 0;
    hs->press_cross = (b & CELL_PAD_CTRL_CROSS) ? 255 : 0;       hs->press_square = (b & CELL_PAD_CTRL_SQUARE) ? 255 : 0;
    hs->press_l1 = (b & CELL_PAD_CTRL_L1) ? 255 : 0;             hs->press_r1 = (b & CELL_PAD_CTRL_R1) ? 255 : 0;
    { static int said = 0; if (!said && (btns || moving || looking)) { said = 1;
        printf("[cellPad] generic keyboard/mouse input active on port 0%s\n", merge ? " (merged with controller)" : ""); fflush(stdout); } }
}

static void pad_poll_keyboard(int merge)
{
    static int off = -1;
    if (off < 0) off = getenv("PAD_NO_KEYBOARD") ? 1 : 0;
    if (off) return;
    if (pad_host_window_focused()) {   /* v1107U: any key, click, wheel or mouse motion = keyboard / mouse in use */
        int act = g_pad_raw_mdx != 0 || g_pad_raw_mdy != 0 || g_pad_raw_wheel != 0;
        for (int k = 1; k < 0xFF && !act; k++) {
            if (k >= 0xC3 && k <= 0xDA) continue;   /* VK_GAMEPAD_* */
            if (GetAsyncKeyState(k) & 0x8000) act = 1; }
        if (act) pad_set_kbm_last(1);
    }
    if (pad_kbm_on() && pad_kbm_generic()) { pad_poll_kbm_generic(merge); return; }
    if (pad_kbm_on()) { pad_poll_kbm(merge); return; }

    PadHostState* hs = &s_host_state[0];
    if (merge && !pad_host_window_focused()) return;
    if (!pad_host_window_focused()) {
        /* Release everything on focus loss, or a key held while alt-tabbing
         * would stay down forever. */
        hs->buttons = 0;
        hs->analog_lx = hs->analog_ly = 128;
        hs->analog_rx = hs->analog_ry = 128;
        hs->connected = 1;
        return;
    }

    static const struct { int vk; u16 btn; } map[] = {
        { VK_UP,     CELL_PAD_CTRL_UP },      { VK_DOWN,  CELL_PAD_CTRL_DOWN },
        { VK_LEFT,   CELL_PAD_CTRL_LEFT },    { VK_RIGHT, CELL_PAD_CTRL_RIGHT },
        { 'Z',       CELL_PAD_CTRL_CROSS },   { 'X',      CELL_PAD_CTRL_CIRCLE },
        { 'A',       CELL_PAD_CTRL_SQUARE },  { 'S',      CELL_PAD_CTRL_TRIANGLE },
        { 'Q',       CELL_PAD_CTRL_L1 },      { 'W',      CELL_PAD_CTRL_R1 },
        { '1',       CELL_PAD_CTRL_L2 },      { '2',      CELL_PAD_CTRL_R2 },
        { VK_RETURN, CELL_PAD_CTRL_START },   { VK_TAB,   CELL_PAD_CTRL_SELECT },
    };

    u16 btns = 0;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (GetAsyncKeyState(map[i].vk) & 0x8000) btns |= map[i].btn;

    if (merge) {
        /* A controller (or a virtual one: Steam Input, DS4Windows...) owns port 0;
         * OR the keys in instead of ignoring them -- the user pressed Enter on
         * inFamous and nothing happened because XInput reported a device. */
        hs->buttons |= btns;
        if (btns) { static int said2 = 0; if (!said2) { said2 = 1;
            printf("[cellPad] keyboard merged into XInput port 0 (first key 0x%04X)\n", btns); fflush(stdout); } }
        return;
    }
    hs->buttons   = btns;
    hs->connected = 1;
    hs->analog_lx = (u8)((btns & CELL_PAD_CTRL_LEFT) ? 0 :
                         (btns & CELL_PAD_CTRL_RIGHT) ? 255 : 128);
    hs->analog_ly = (u8)((btns & CELL_PAD_CTRL_UP) ? 0 :
                         (btns & CELL_PAD_CTRL_DOWN) ? 255 : 128);
    hs->analog_rx = hs->analog_ry = 128;
    hs->trigger_l2 = (u8)((btns & CELL_PAD_CTRL_L2) ? 255 : 0);
    hs->trigger_r2 = (u8)((btns & CELL_PAD_CTRL_R2) ? 255 : 0);

    { static int said = 0;
      if (!said && btns) { said = 1;
          printf("[cellPad] keyboard fallback active on port 0 (no XInput device)\n");
          fflush(stdout); } }
}
#endif

/* v221B PAD_XI_REPROBE_MS (default 1000; 0 = old): XInputGetState on an EMPTY slot is a device enumeration
 * (tens to hundreds of us each -- Microsoft's guidance is to re-check empty slots only every few seconds), and the
 * 250 Hz poll thread probed all 4 slots every 4 ms: pad_thread_main used ~21 % of a P-core in the b027 bench.
 * A slot whose last XInputGetState failed is now re-probed only every PAD_XI_REPROBE_MS; in between it reports
 * "not connected" exactly as a failed probe does. Connected slots are read every poll as before. Only effect:
 * a controller plugged in mid-game is picked up up to 1 s later. ABX 0x100 = old (probe every poll). */
static unsigned long long pad_now_ms(void);
extern volatile int g_abx;
static void pad_poll_xinput(void)
{
    static int reprobe_ms = -1;
    static unsigned long long next_probe[PAD_MAX_HOST_PORTS];
    if (reprobe_ms < 0) {
        const char* e = getenv("PAD_XI_REPROBE_MS");
        reprobe_ms = (e && *e) ? atoi(e) : 1000;
        if (reprobe_ms < 0) reprobe_ms = 0;
        printf("[cellPad] v221B empty XInput slots re-probed every %d ms (PAD_XI_REPROBE_MS; 0 = every poll; ABX 0x100 = every poll)\n", reprobe_ms);
        fflush(stdout);
    }
    const int backoff = reprobe_ms > 0 && !(g_abx & 0x100);
    const unsigned long long now = backoff ? pad_now_ms() : 0;
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        XINPUT_STATE state;
        memset(&state, 0, sizeof(state));

        if (backoff && next_probe[i] && now < next_probe[i]) {   /* v221B: empty slot, not due yet */
            s_host_state[i].connected = 0;
            continue;
        }
        DWORD result = XInputGetState((DWORD)i, &state);
        if (result != ERROR_SUCCESS) {
            s_host_state[i].connected = 0;
            next_probe[i] = backoff ? now + (unsigned long long)reprobe_ms : 0;   /* v221B */
            continue;
        }
        /* v1058J PAD_XI_FOCUS_ONLY=1 (co-op on one PC): a controller only drives the focused game window -- the other
         * instance sees a connected pad at rest. Default 0 = old (every instance reads the pad). */
        { static int fo = -1; if (fo < 0) { const char* e = getenv("PAD_XI_FOCUS_ONLY"); const char* e2 = getenv("PAD_FOCUS_ONLY"); fo = ((e && *e == '1') || (e2 && *e2 == '1')) ? 1 : 0;   /* v1059E PAD_FOCUS_ONLY alias */
              if (fo) { printf("[cellPad] v1058J controller input only while this window has focus (PAD_XI_FOCUS_ONLY)\n"); fflush(stdout); } }
          if (fo && !pad_host_window_focused()) memset(&state.Gamepad, 0, sizeof(state.Gamepad)); }
        next_probe[i] = 0;   /* v221B: connected -> read every poll */

        s_host_state[i].connected = 1;
        XINPUT_GAMEPAD* gp = &state.Gamepad;

        /* Map XInput buttons to PS3 CELL_PAD_CTRL_* */
        u16 btns = 0;
        if (gp->wButtons & XINPUT_GAMEPAD_BACK)           btns |= CELL_PAD_CTRL_SELECT;
        if (gp->wButtons & XINPUT_GAMEPAD_LEFT_THUMB)     btns |= CELL_PAD_CTRL_L3;
        if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)    btns |= CELL_PAD_CTRL_R3;
        if (gp->wButtons & XINPUT_GAMEPAD_START)          btns |= CELL_PAD_CTRL_START;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_UP)        btns |= CELL_PAD_CTRL_UP;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)     btns |= CELL_PAD_CTRL_RIGHT;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_DOWN)      btns |= CELL_PAD_CTRL_DOWN;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_LEFT)      btns |= CELL_PAD_CTRL_LEFT;
        if (gp->bLeftTrigger > PAD_TRIGGER_THRESHOLD)     btns |= CELL_PAD_CTRL_L2;
        if (gp->bRightTrigger > PAD_TRIGGER_THRESHOLD)    btns |= CELL_PAD_CTRL_R2;
        if (gp->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  btns |= CELL_PAD_CTRL_L1;
        if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) btns |= CELL_PAD_CTRL_R1;
        if (gp->wButtons & XINPUT_GAMEPAD_Y)              btns |= CELL_PAD_CTRL_TRIANGLE;
        if (gp->wButtons & XINPUT_GAMEPAD_B)              btns |= CELL_PAD_CTRL_CIRCLE;
        if (gp->wButtons & XINPUT_GAMEPAD_A)              btns |= CELL_PAD_CTRL_CROSS;
        if (gp->wButtons & XINPUT_GAMEPAD_X)              btns |= CELL_PAD_CTRL_SQUARE;

        s_host_state[i].buttons = btns;
        /* v193: controller activity -> controller prompts (sticks past ~37 %, so drift does not flip it) */
        { const int st = (gp->sThumbLX > 12000 || gp->sThumbLX < -12000 || gp->sThumbLY > 12000 || gp->sThumbLY < -12000 ||
                          gp->sThumbRX > 12000 || gp->sThumbRX < -12000 || gp->sThumbRY > 12000 || gp->sThumbRY < -12000);
          if (btns || st) pad_note_device(0);
          else if (g_input_kbm < 0) pad_note_device(0); }

        /* Analog sticks. PS3 Y axis is inverted vs XInput (up = 0). Reflect about
         * 128 (256 - x), NOT 255 - x: the latter turns a centered stick into 127,
         * whose bits (0x7F) alias SELECT+START and can make the guest self-exit.
         * (via sagemono, PR #42) */
        s_host_state[i].analog_lx = pad_xinput_stick_to_u8(gp->sThumbLX, PAD_STICK_DEADZONE);
        s_host_state[i].analog_ly = (u8)(256 - pad_xinput_stick_to_u8(gp->sThumbLY, PAD_STICK_DEADZONE));
        s_host_state[i].analog_rx = pad_xinput_stick_to_u8(gp->sThumbRX, PAD_STICK_DEADZONE);
        s_host_state[i].analog_ry = (u8)(256 - pad_xinput_stick_to_u8(gp->sThumbRY, PAD_STICK_DEADZONE));

        /* Triggers */
        s_host_state[i].trigger_l2 = gp->bLeftTrigger;
        s_host_state[i].trigger_r2 = gp->bRightTrigger;

        /* Pressure-sensitive buttons: XInput has digital only, so 0 or 255 */
        s_host_state[i].press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
        s_host_state[i].press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
        s_host_state[i].press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
        s_host_state[i].press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
        s_host_state[i].press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
        s_host_state[i].press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
        s_host_state[i].press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
        s_host_state[i].press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
        s_host_state[i].press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
        s_host_state[i].press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
    }
}

static void pad_init_backend(void)
{
    /* XInput needs no explicit init */
}

static void pad_shutdown_backend(void)
{
    /* XInput needs no explicit shutdown */
}

/* v1042G controller gestures (matches the rewritten prompts, gf_prompts.c). Gesture mode = L1 held, i.e. while
 * grabbing (L1+R1) or locked on (L1):
 *   flick the right stick up = forward, down = towards you, left / right; rotate it clockwise = twist (neck snap)
 *   click R3 = up (lift while grabbing, rotate up while only locked on); click L3 = down and in; hold L3 + flick
 *   left / right = down-left / down-right.
 * While grabbing the right stick never turns the camera. While only locked on, slow stick movement still reaches
 * the game (fine aim) and only fast flicks (centre -> edge within PAD_MOTION_FLICK_MS, default 150) are gestures.
 * L3 / R3 are hidden from the game while L1 is held. PAD_MOTION_PAD=NONE disables, L1 (default) or R3 / SELECT
 * picks another hold button. */
static void pad_motion_controller(void)
{
    PadHostState* hs = &s_host_state[0];
    static int mod = -1, flick_ms = 150;
    if (mod < 0) { const char* e = getenv("PAD_MOTION_PAD"); u16 b = CELL_PAD_CTRL_L1;
                   if (e && *e && _stricmp(e, "R3") && !kbm_btn(e, &b)) b = CELL_PAD_CTRL_L1;   /* old R3 default -> L1 */
                   mod = b; flick_ms = pad_envi("PAD_MOTION_FLICK_MS", 150); }
    static int armed = 1, l3_used = 0, rot_on = 0; static float rot_last = 0.0f, rot_acc = 0.0f;
    static unsigned long long t_center = 0, hide_until = 0; static u16 prevb = 0;
    if (!mod || !hs->connected) { prevb = 0; return; }
    const unsigned long long now = pad_now_ms();
    const u16 b = hs->buttons;
    const int dx = (int)hs->analog_rx - 128, dy = (int)hs->analog_ry - 128;
    const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, m = ax > ay ? ax : ay;
    if (m < 40) { t_center = now; armed = 1; }
    if (!(b & (u16)mod)) { prevb = b; rot_on = 0; l3_used = 0; return; }
    const int grab = (b & CELL_PAD_CTRL_R1) != 0;
    const int l3 = (b & CELL_PAD_CTRL_L3) != 0;
    int hide = grab;
    if (m > 96 && armed) {
        const int fast = now - t_center <= (unsigned long long)flick_ms;
        if (grab || fast) {
            armed = 0; hide_until = now + 250;
            int g = ay >= ax ? (dy < 0 ? MOT_FORWARD : MOT_BACK) : (dx < 0 ? MOT_LEFT : MOT_RIGHT);
            if (l3 && (g == MOT_LEFT || g == MOT_RIGHT)) { g = g == MOT_LEFT ? MOT_KNEE_L : MOT_KNEE_R; l3_used = 1; }
            pad_motion_trigger(g);
        }
    }
    /* clockwise rotation (PS3 stick bytes: y grows downward, so clockwise = increasing atan2) */
    if (m > 80) {
        const float ang = atan2f((float)dy, (float)dx);
        if (rot_on) {
            float d = ang - rot_last;
            if (d > 3.14159265f) d -= 6.2831853f; else if (d < -3.14159265f) d += 6.2831853f;
            rot_acc += d;
            if (rot_acc > 4.4f) { pad_motion_trigger(MOT_TWIST); rot_acc = 0.0f; hide_until = now + 250; }
            if (rot_acc < -1.0f) rot_acc = -1.0f;
        } else { rot_on = 1; rot_acc = 0.0f; }
        rot_last = ang;
    } else rot_on = 0;
    if ((b & CELL_PAD_CTRL_R3) && !(prevb & CELL_PAD_CTRL_R3)) pad_motion_trigger(grab ? MOT_LIFT : MOT_TILT);
    if (l3 && !(prevb & CELL_PAD_CTRL_L3)) l3_used = 0;
    if (!l3 && (prevb & CELL_PAD_CTRL_L3) && !l3_used) pad_motion_trigger(MOT_DOWN);   /* plain L3 click */
    prevb = b;
    if (now < hide_until) hide = 1;
    hs->buttons &= (u16)~(CELL_PAD_CTRL_L3 | CELL_PAD_CTRL_R3);
    if (hide) hs->analog_rx = hs->analog_ry = 128;
}
#endif /* PAD_BACKEND_XINPUT */

#if defined(PS3RECOMP_SDL3) && PAD_BACKEND_XINPUT
/* PlayStation / Steam controllers (pad_sdl3.c) take the host ports XInput left empty, in connection order.
 * s_sdl_slot[port] = their pad_sdl3 index, for rumble (-1 = not an SDL pad). */
static int s_sdl_slot[PAD_MAX_HOST_PORTS] = { -1, -1, -1, -1 };
static void pad_merge_sdl3(void)
{
    PadSdlPad sp[PAD_MAX_HOST_PORTS];
    const int ns = pad_sdl3_get(sp, PAD_MAX_HOST_PORTS);
    int k = 0;
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        s_sdl_slot[i] = -1;
        if (s_host_state[i].connected || k >= ns) continue;
        /* v1059E PAD_XI_FOCUS_ONLY=1 also gates PlayStation/SDL3 pads: before, a DualSense/DS4 drove BOTH co-op windows on
         * one PC (both players walked in lockstep, so each partner puppet looked like it copied you). Unfocused = pad at rest. */
        static int fo3 = -1; if (fo3 < 0) { const char* e = getenv("PAD_XI_FOCUS_ONLY"); const char* e2 = getenv("PAD_FOCUS_ONLY");
            fo3 = ((e && *e == '1') || (e2 && *e2 == '1')) ? 1 : 0;
            if (fo3) { printf("[cellPad] v1059E SDL3 (PlayStation) controller input only while this window has focus\n"); fflush(stdout); } }
        PadSdlPad rest; const PadSdlPad* p = &sp[k];
        if (fo3 && !pad_host_window_focused()) { memset(&rest, 0, sizeof rest); rest.lx = rest.ly = rest.rx = rest.ry = 128; p = &rest; }
        PadHostState* h = &s_host_state[i];
        const u16 b = p->buttons;
        h->connected = 1; h->buttons = b;
        h->analog_lx = p->lx; h->analog_ly = p->ly; h->analog_rx = p->rx; h->analog_ry = p->ry;
        h->trigger_l2 = p->l2; h->trigger_r2 = p->r2;
        h->press_up = (b & CELL_PAD_CTRL_UP) ? 255 : 0;             h->press_down = (b & CELL_PAD_CTRL_DOWN) ? 255 : 0;
        h->press_left = (b & CELL_PAD_CTRL_LEFT) ? 255 : 0;         h->press_right = (b & CELL_PAD_CTRL_RIGHT) ? 255 : 0;
        h->press_triangle = (b & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0; h->press_circle = (b & CELL_PAD_CTRL_CIRCLE) ? 255 : 0;
        h->press_cross = (b & CELL_PAD_CTRL_CROSS) ? 255 : 0;       h->press_square = (b & CELL_PAD_CTRL_SQUARE) ? 255 : 0;
        h->press_l1 = (b & CELL_PAD_CTRL_L1) ? 255 : 0;             h->press_r1 = (b & CELL_PAD_CTRL_R1) ? 255 : 0;
        /* controller activity -> controller prompts (sticks past ~37 %, like the XInput path) */
        const int moved = p->lx < 80 || p->lx > 176 || p->ly < 80 || p->ly > 176 || p->rx < 80 || p->rx > 176 || p->ry < 80 || p->ry > 176;
        if (b || moved) pad_note_device(0);
        else if (g_input_kbm < 0) pad_note_device(0);
        s_sdl_slot[i] = k++;
    }
}
#endif

/* ---------------------------------------------------------------------------
 * SDL2 backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_SDL2

static u8 pad_sdl_axis_to_u8(int raw)
{
    /* SDL axis: -32768..32767 -> 0..255 with center at 128 */
    int val = ((raw + 32768) * 255) / 65535;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

static u8 pad_sdl_trigger_to_u8(int raw)
{
    /* SDL trigger: 0..32767 -> 0..255 */
    int val = (raw * 255) / 32767;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

static void pad_poll_sdl2(void)
{
    SDL_GameControllerUpdate();

    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (!s_sdl_controllers[i]) {
            /* Try to open newly connected controllers */
            if (SDL_IsGameController(i)) {
                s_sdl_controllers[i] = SDL_GameControllerOpen(i);
            }
        }

        SDL_GameController* gc = s_sdl_controllers[i];
        if (!gc || !SDL_GameControllerGetAttached(gc)) {
            s_host_state[i].connected = 0;
            if (gc) {
                SDL_GameControllerClose(gc);
                s_sdl_controllers[i] = NULL;
            }
            continue;
        }

        s_host_state[i].connected = 1;

        u16 btns = 0;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK))          btns |= CELL_PAD_CTRL_SELECT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK))     btns |= CELL_PAD_CTRL_L3;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK))    btns |= CELL_PAD_CTRL_R3;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START))         btns |= CELL_PAD_CTRL_START;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))       btns |= CELL_PAD_CTRL_UP;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))    btns |= CELL_PAD_CTRL_RIGHT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))     btns |= CELL_PAD_CTRL_DOWN;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))     btns |= CELL_PAD_CTRL_LEFT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  btns |= CELL_PAD_CTRL_L1;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) btns |= CELL_PAD_CTRL_R1;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y))             btns |= CELL_PAD_CTRL_TRIANGLE;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B))             btns |= CELL_PAD_CTRL_CIRCLE;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A))             btns |= CELL_PAD_CTRL_CROSS;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X))             btns |= CELL_PAD_CTRL_SQUARE;

        /* Triggers via axis */
        int lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (lt > 3000) btns |= CELL_PAD_CTRL_L2;
        if (rt > 3000) btns |= CELL_PAD_CTRL_R2;

        s_host_state[i].buttons = btns;

        /* Analog sticks */
        s_host_state[i].analog_lx = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX));
        s_host_state[i].analog_ly = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY));
        s_host_state[i].analog_rx = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX));
        s_host_state[i].analog_ry = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY));

        /* Triggers */
        s_host_state[i].trigger_l2 = pad_sdl_trigger_to_u8(lt);
        s_host_state[i].trigger_r2 = pad_sdl_trigger_to_u8(rt);

        /* Pressure: SDL has digital buttons, so 0 or 255 */
        s_host_state[i].press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
        s_host_state[i].press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
        s_host_state[i].press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
        s_host_state[i].press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
        s_host_state[i].press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
        s_host_state[i].press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
        s_host_state[i].press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
        s_host_state[i].press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
        s_host_state[i].press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
        s_host_state[i].press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
    }
}

static void pad_init_backend(void)
{
    if (!s_sdl_inited) {
        if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
            SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
        }
        s_sdl_inited = 1;
    }
    memset(s_sdl_controllers, 0, sizeof(s_sdl_controllers));

    /* Open any controllers already connected */
    int num = SDL_NumJoysticks();
    for (int i = 0; i < num && i < PAD_MAX_HOST_PORTS; i++) {
        if (SDL_IsGameController(i)) {
            s_sdl_controllers[i] = SDL_GameControllerOpen(i);
        }
    }
}

static void pad_shutdown_backend(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (s_sdl_controllers[i]) {
            SDL_GameControllerClose(s_sdl_controllers[i]);
            s_sdl_controllers[i] = NULL;
        }
    }
}

#endif /* PAD_BACKEND_SDL2 */

/* ---------------------------------------------------------------------------
 * Poll dispatcher
 * -----------------------------------------------------------------------*/

/* Re-poll the host at most every PAD_POLL_INTERVAL_MS; serve cached state in
 * between.
 *
 * XInputGetState is a driver call, and on a slot with nothing plugged in it is
 * an expensive one: measured at 97.6 us for a sweep of the 7 host ports on a
 * machine with no controller attached. cellPadGetData ran that sweep on EVERY
 * call, and a guest that polls the pad from a spin loop -- The Simpsons Arcade
 * Game does, in its input stage -- therefore spends its whole main thread
 * inside the input driver.
 *
 * 4 ms is 250 Hz, well above the 60 Hz a title can actually observe (and above
 * a real DualShock 3's own report rate), so no game can tell the difference,
 * including one sampling for button-press edges. */
#define PAD_POLL_INTERVAL_MS 4

static unsigned long long pad_now_ms(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}

/* v179 (FPS): when the host poll thread runs, guest calls (cellPadGetData & co., on the game's MAIN thread) only read
 * the state it keeps fresh -- the XInput/keyboard sweep cost the main thread ~8 % of its time (perf179 profile:
 * pad_poll_backend under the frame loop). PAD_GUEST_POLL=1 = old behaviour (guest calls sweep too). */
static volatile long s_pad_thread_live = 0;
static void pad_poll_backend_do(void);
static void pad_poll_backend(void)
{
    static int gp = -1;
    if (gp < 0) { const char* e = getenv("PAD_GUEST_POLL"); gp = (e && *e == '1') ? 1 : 0; }
    if (s_pad_thread_live && !gp) return;
    pad_poll_backend_do();
}
static void pad_poll_backend_do(void)
{
    /* Benign race: two threads may poll in the same tick. That costs one extra
     * sweep, never a missed or stale-forever read. */
    static unsigned long long s_last_ms = 0;
    unsigned long long now = pad_now_ms();
    if (s_last_ms && now - s_last_ms < PAD_POLL_INTERVAL_MS) return;
    s_last_ms = now;
    /* A real pad streams reports at roughly this rate whether or not anything
     * changed, and libpad buffers them; a fresh sweep is a fresh report for
     * every port. cellPadGetData hands each one out exactly once. */
    for (int _i = 0; _i < PAD_MAX_HOST_PORTS; _i++) s_data_fresh[_i] = 1;

#ifdef _WIN32
    /* v221B PAD_COST=1: [PADCOST] every 10 s = polls/s and us per poll in the controller sweep and the keyboard/
     * mouse part (host poll thread cost; bench diagnostics). */
    static int cost = -1; static LARGE_INTEGER cf, c0; static unsigned long long c_n, c_xi, c_kb;
    if (cost < 0) { const char* e = getenv("PAD_COST"); cost = (e && *e == '1'); QueryPerformanceFrequency(&cf); QueryPerformanceCounter(&c0); }
    LARGE_INTEGER t0, t1, t2; if (cost) QueryPerformanceCounter(&t0);
#endif
    rr_tick_begin();   /* v1129RR record / replay: raw keyboard, cursor, focus, mouse for this tick */
#if PAD_BACKEND_XINPUT
    if (s_rr_mode != 2) {   /* v1129RR replay: recorded controllers, the real ones are not read */
    pad_poll_xinput();
#ifdef PS3RECOMP_SDL3
    pad_merge_sdl3();   /* PlayStation / Steam controllers into the ports XInput left empty */
#endif
    }
#elif PAD_BACKEND_SDL2
    if (s_rr_mode != 2) pad_poll_sdl2();
#endif
    rr_tick_gamepad();   /* v1129RR: controller states (record) / recorded ones (replay), drift report */
#ifdef _WIN32
    if (cost) QueryPerformanceCounter(&t1);
    s_pad_xinput_merged = s_host_state[0].connected;
    { const PadHostState* h = &s_host_state[0];   /* v1107U: controller in use (before the keyboard merge) */
      if (h->connected && (h->buttons || h->analog_lx < 80 || h->analog_lx > 176 || h->analog_ly < 80 || h->analog_ly > 176 ||
                           h->analog_rx < 80 || h->analog_rx > 176 || h->analog_ry < 80 || h->analog_ry > 176 ||
                           h->trigger_l2 > 60 || h->trigger_r2 > 60)) pad_set_kbm_last(0); }
#if PAD_BACKEND_XINPUT
    pad_motion_controller();   /* v1042M: before the keyboard merge, so keyboard R3 (Z / MMB) stays a plain R3 */
#endif
    pad_poll_keyboard(s_host_state[0].connected);
    { /* v1042S hold-to-skip (see g_pad_cutscene_fn) */
      static int on = -1, hold_ms = 1000, test_ms = 0; static unsigned long long t_down = 0, show_until = 0, ff_t0 = 0, cs_t0 = 0; static u16 prev = 0;
      if (on < 0) { const char* e = getenv("PAD_SKIP"); on = !(e && *e == '0'); hold_ms = pad_envi("PAD_SKIP_HOLD_MS", 1000);
                    if (hold_ms < 200) hold_ms = 200;
                    test_ms = pad_envi("PAD_SKIP_TEST", 0);   /* test: act as if Cross is held from <ms> into each cutscene */
                    if (test_ms) { printf("[skip] PAD_SKIP_TEST: simulated hold %d ms into each cutscene\n", test_ms); fflush(stdout); } }
      const unsigned long long now = pad_now_ms();
      const int cs = on && g_pad_cutscene_fn ? g_pad_cutscene_fn() : 0;
      PadHostState* h = &s_host_state[0];
      u16 b = h->buttons;
      if (cs && !cs_t0) cs_t0 = now;
      if (!cs) cs_t0 = 0;
      if (test_ms && cs && now - cs_t0 >= (unsigned long long)test_ms) b |= CELL_PAD_CTRL_CROSS;
      if (cs && kbm_classic() && pad_host_window_focused() && (GetAsyncKeyState(VK_SPACE) & 0x8000)) b |= CELL_PAD_CTRL_CROSS;   /* v1130M: Space = hold to skip (Space is Triangle in the EA PC scheme) */
      if (!cs) {
          g_gf_skip_request = 0;   /* v1073H: a request without a cutscene is dropped */
          if (g_gf_ffwd) { printf("[skip] v1042V cutscene skipped, back in the game after %.1f s\n", (now - ff_t0) / 1000.0); fflush(stdout); }
          g_gf_ffwd = 0; g_skip_visible = 0; g_skip_progress = 0.0f; t_down = 0; show_until = 0;
      } else if (g_gf_ffwd) {
          g_skip_visible = 2; g_skip_progress = 1.0f;
          /* v1042V PAD_SKIP_WARP (default 0): the timebase warp below pushed the PS3 clock ahead permanently (about
           * 20 s for a 0.5 s skip at speed 40), which left the game's audio out of step with its clock after every
           * skip. The skip now jumps the cutscene to its end (gf_cutscene.c) and needs no clock warp; =1 = old drain. */
          static int warp = -1; if (warp < 0) warp = pad_envi("PAD_SKIP_WARP", 0) ? 1 : 0;
          if (warp) { /* Run the hidden script drain quickly; no intermediate video or audio reaches the user. */
            extern volatile int64_t g_ps3_tb_warp;
            static int smax = 0; static unsigned long long last = 0;
            if (!smax) { smax = pad_envi("PAD_SKIP_SPEED", 40); if (smax < 2) smax = 2; if (smax > 40) smax = 40; }
            int speed = g_gf_skip_speed < smax ? g_gf_skip_speed : smax; if (speed < 2) speed = 2;
            if (!last || now - ff_t0 < 20 || now - last > 200) last = now;   /* new skip / stall: no jump */
            if (now > last) g_ps3_tb_warp += (int64_t)((now - last) * (unsigned long long)(speed - 1) * 79800ull);   /* 79.8 MHz timebase */
            last = now; }
          if (now - ff_t0 > 45000ull) {   /* safety: never drain a broken cutscene forever */
              g_gf_ffwd = 0; g_skip_visible = 0; t_down = 0;
              printf("[skip] cutscene drain stopped by the 45 s safety limit\n"); fflush(stdout);
          }
      } else if (g_gf_skip_request) {   /* v1073H: remote skip (co-op host) */
          g_gf_skip_request = 0; g_gf_ffwd = 1; ff_t0 = now; g_skip_visible = 2; t_down = 0;
          printf("[skip] v1073H cutscene skip requested by the co-op host\n"); fflush(stdout);
      } else if (g_gf_skip_block) {   /* v1073H: the host decides; no prompt, no hold */
          t_down = 0; g_skip_progress = 0.0f; g_skip_visible = 0;
      } else {
          if ((b & ~prev) || h->analog_lx < 64 || h->analog_lx > 192 || h->analog_ly < 64 || h->analog_ly > 192)
              show_until = now + 3000;                       /* any input during a cutscene: show the prompt */
          if (b & CELL_PAD_CTRL_CROSS) {
              if (!t_down) t_down = now;
              const float k = (float)(now - t_down) / (float)hold_ms;
              g_skip_progress = k > 1.0f ? 1.0f : k;
              show_until = now + 3000;
              if (k >= 1.0f) {
                  g_gf_ffwd = 1; ff_t0 = now; g_skip_visible = 2;
                  printf("[skip] v1042V cutscene skip requested; jumping to the end of the cutscene (screen black meanwhile)\n"); fflush(stdout);
              }
          } else { t_down = 0; g_skip_progress = 0.0f; }
          if (!g_gf_ffwd) g_skip_visible = now < show_until ? 1 : 0;
      }
      if (cs) h->buttons &= (u16)~CELL_PAD_CTRL_CROSS;      /* the hold is ours, not the game's */
      prev = b; }
    /* PAD_START_ON_ANY_INPUT=1: one-shot compatibility bridge for titles that
     * put up a DualShock/SIXAXIS reconnect screen and insist specifically on
     * START before accepting their otherwise working virtual pad.  Once the
     * game window is focused, translate the first real Windows input event
     * (any key, mouse movement/click/wheel, or mapped controller activity) to
     * one latched START press.  The bridge permanently disarms after firing,
     * so normal gameplay input can never become an accidental pause button.
     *
     * GetLastInputInfo lets "any input" mean exactly that instead of only the
     * small keyboard subset mapped by pad_poll_keyboard().  Focus keeps input
     * aimed at another application from advancing the game. */
    { static int any_start = -1, any_start_fired = 0;
      static DWORD last_input_tick = 0;
      static ULONGLONG focus_since = 0;
      if (any_start < 0) {
          const char* e = getenv("PAD_START_ON_ANY_INPUT");
          any_start = (e && *e && *e != '0') ? 1 : 0;
          LASTINPUTINFO li = { sizeof(li), 0 };
          if (GetLastInputInfo(&li)) last_input_tick = li.dwTime;
          if (any_start) {
              printf("[cellPad] first focused host input will emit one START press (PAD_START_ON_ANY_INPUT=1)\n");
              fflush(stdout);
          }
      }
      if (any_start && !any_start_fired && pad_host_window_focused()) {
          LASTINPUTINFO li = { sizeof(li), 0 };
          ULONGLONG now_tick = GetTickCount64();
          /* Creating/activating the game window itself produces Windows input
           * timestamps.  Baseline those launch events and wait for focus to be
           * stable before accepting the user's NEXT action; otherwise the
           * reconnect handshake fires several seconds before the dialog is
           * constructed and is inevitably discarded. */
          if (!focus_since) {
              focus_since = now_tick;
              if (GetLastInputInfo(&li)) last_input_tick = li.dwTime;
          } else if (now_tick - focus_since < 1000) {
              if (GetLastInputInfo(&li)) last_input_tick = li.dwTime;
          } else {
          int host_activity = GetLastInputInfo(&li) && li.dwTime != last_input_tick;
          PadHostState* h = &s_host_state[0];
          int pad_activity = h->buttons || h->trigger_l2 || h->trigger_r2 ||
              h->analog_lx != 128 || h->analog_ly != 128 ||
              h->analog_rx != 128 || h->analog_ry != 128;
          if (host_activity || pad_activity) {
              /* Keep START present for several delivered reports.  Do not
               * synthesize ASSIGN_CHANGES here: Godfather imports the legacy
               * CellPadInfo API, whose firmware contract strips that bit. */
              InterlockedExchange(&s_any_input_start_reports, 64);
              PAD_LATCH_OR(0, CELL_PAD_CTRL_START);
              any_start_fired = 1;
              printf("[cellPad] first focused host input -> held START handshake\n");
              fflush(stdout);
          }
          if (GetLastInputInfo(&li)) last_input_tick = li.dwTime;
          }
      } else if (any_start && !any_start_fired) {
          focus_since = 0;
      }
    }
    if (cost) {
        QueryPerformanceCounter(&t2);
        c_n++; c_xi += (unsigned long long)(t1.QuadPart - t0.QuadPart); c_kb += (unsigned long long)(t2.QuadPart - t1.QuadPart);
        const double win = (double)(t2.QuadPart - c0.QuadPart) / (double)cf.QuadPart;
        if (win >= 10.0) {
            const double us = 1e6 / (double)cf.QuadPart;
            fprintf(stderr, "[PADCOST] v221B %.0f polls/s; controller sweep %.1f us/poll, keyboard+mouse %.1f us/poll = %.2f %% of a core (abx 0x%X)\n",
                    c_n / win, c_xi * us / (double)c_n, c_kb * us / (double)c_n, 100.0 * (double)(c_xi + c_kb) / (double)cf.QuadPart / win, (unsigned)g_abx);
            c_n = c_xi = c_kb = 0; c0 = t2;
        }
    }
#endif
    for (int _i = 0; _i < PAD_MAX_HOST_PORTS; _i++)
        if (s_host_state[_i].buttons) PAD_LATCH_OR(_i, s_host_state[_i].buttons);
}

/* Host-side poller: samples the controller/keyboard every PAD_POLL_INTERVAL_MS
 * independently of how often the guest reads, feeding the press latch.
 * PAD_NO_THREAD=1 disables it (polling then happens only inside GetData). */
#ifdef _WIN32
static volatile LONG s_pad_thread_run = 0;
static HANDLE s_pad_thread = NULL;
static DWORD WINAPI pad_thread_main(LPVOID arg)
{
    (void)arg;
    InterlockedExchange(&s_pad_thread_live, 1);   /* v179 */
    while (s_pad_thread_run) {
#ifdef PS3RECOMP_SDL3
        pad_sdl3_update();   /* SDL3 gamepads: every SDL call happens on this thread */
#endif
        pad_poll_backend_do();
        Sleep(PAD_POLL_INTERVAL_MS);
    }
    return 0;
}
static void pad_thread_start(void)
{
    if (s_pad_thread || getenv("PAD_NO_THREAD")) return;
    s_pad_thread_run = 1;
    s_pad_thread = CreateThread(NULL, 0, pad_thread_main, NULL, 0, NULL);
    printf("[cellPad] host poll thread %s (every %d ms, presses latched until read)\n",
           s_pad_thread ? "started" : "FAILED", PAD_POLL_INTERVAL_MS);
    fflush(stdout);
}
static void pad_thread_stop(void)
{
    if (!s_pad_thread) return;
    s_pad_thread_run = 0;
    InterlockedExchange(&s_pad_thread_live, 0);   /* v179 */
    WaitForSingleObject(s_pad_thread, 200);
    CloseHandle(s_pad_thread);
    s_pad_thread = NULL;
}
#else
static void pad_thread_start(void) {}
static void pad_thread_stop(void) {}
#endif

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellPadInit(u32 max_connect)
{
    printf("[cellPad] Init(max_connect=%u)\n", max_connect);

    if (s_pad_initialized)
        return CELL_PAD_ERROR_ALREADY_OPENED;

    /* The bound is the 127-slot pad table, not the 7 ports: larger requests
     * are accepted and clamped. Resistance: Fall of Man asks for 8; rejected,
     * the library stayed closed, every GetInfo failed and the title never
     * polled the pad -- stuck on "Press the START button". */
    if (max_connect == 0 || max_connect > 127)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_pad_initialized = 1;
    rr_init();   /* v1129RR PAD_RECORD / PAD_REPLAY */
    s_max_connect = max_connect > CELL_PAD_MAX_PORT_NUM ? CELL_PAD_MAX_PORT_NUM : max_connect;
    memset(s_port_setting, 0, sizeof(s_port_setting));
    memset(s_host_state, 0, sizeof(s_host_state));
    pad_assignment_reset();

    pad_init_backend();

    /* Do an initial poll to detect connected controllers */
    pad_poll_backend();
    pad_thread_start();

    return CELL_OK;
}

s32 cellPadEnd(void)
{
    printf("[cellPad] End()\n");

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    pad_thread_stop();
    pad_shutdown_backend();

    s_pad_initialized = 0;
    s_max_connect = 0;
    return CELL_OK;
}

void cellPad_poll(void)
{
    if (s_pad_initialized) {
        pad_poll_backend();
    }
}

/* HLE args are GUEST effective addresses; guest structs are BIG-ENDIAN. The
 * vm_write helpers from ppu_memory.h translate + byte-swap. (cellPad was
 * written for host pointers and was never actually invoked until its NIDs
 * were fixed.) These used to be redeclared here with a 64-bit address
 * parameter, which conflicts with the header now that it is included. */

#ifdef _WIN32
/* v219 MEMCMD=<path> -- bench/debug memory tool (main thread, between game frames, checked every 100 ms).
 * The file holds ONE command; it is executed, its output appended to <path>.out (and stdout), and the file emptied.
 * Used to find Cole's position in guest RAM and teleport him (city bench), Cheat-Engine style:
 *   init <lo> <hi>          candidates = every 4-aligned big-endian float in [lo,hi] (|v| >= 0.5) in committed guest RAM
 *   changed | same | inc | dec   keep candidates whose value changed / stayed / grew / shrank since the last step
 *   list <n>                print the count and the first n candidates (addr value)
 *   nbr <addr> [n]          dump n (default 16) floats starting at addr
 *   find3 <x> <y> <z> <eps> every address holding 3 consecutive floats within eps of x y z (max 256)
 *   poke <addr> <v>         write a float;  poke3 <addr> <x> <y> <z>
 *   tp <x> <y> <z> <eps> <tx> <ty> <tz> [frames]   find3 + write tx ty tz to every hit, re-written for [frames] polls
 *   tpd <x> <y> <z> <eps> <dx> <dy> <dz> [frames]  same, target = found position + d (keeps the other copies consistent) */
static u32*  s_mc_addr = NULL; static float* s_mc_val = NULL; static size_t s_mc_n = 0, s_mc_cap = 0;
static u32   s_mc_hold_addr[256]; static float s_mc_hold_v[256][3]; static int s_mc_hold_n = 0, s_mc_hold_frames = 0;
static u32   s_mc_hf_addr = 0; static float s_mc_hf_v = 0; static int s_mc_hf_polls = 0;   /* trainer hunt: holdf */
static FILE* s_mc_out = NULL;
static void mc_log(const char* fmt, ...)
{
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fputs(b, stdout); fflush(stdout);
    if (s_mc_out) { fputs(b, s_mc_out); fflush(s_mc_out); }
}
static float mc_rd(u32 a) { u32 r; memcpy(&r, vm_base + a, 4); r = _byteswap_ulong(r); float f; memcpy(&f, &r, 4); return f; }
static void  mc_wr(u32 a, float f) { u32 r; memcpy(&r, &f, 4); vm_write32(a, r); }
/* walk committed, readable guest pages in [0x00010000, 0xC0000000) */
typedef void (*mc_range_fn)(u32 lo, u32 hi, void* ctx);
static void mc_ranges(mc_range_fn fn, void* ctx)
{
    unsigned long long a = 0x00010000ull;
    while (a < 0xC0000000ull) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vm_base + a, &mi, sizeof mi)) break;
        unsigned long long end = (unsigned long long)((uint8_t*)mi.BaseAddress - vm_base) + mi.RegionSize;
        if (end > 0xC0000000ull) end = 0xC0000000ull;
        const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
        if (mi.State == MEM_COMMIT && (mi.Protect & ok) && !(mi.Protect & PAGE_GUARD)) fn((u32)a, (u32)(end - 1), ctx);
        if (end <= a) break;
        a = end;
    }
}
typedef struct { float lo, hi; } mc_init_ctx;
static void mc_push(u32 a, float v)
{
    if (s_mc_n == s_mc_cap) {
        size_t nc = s_mc_cap ? s_mc_cap * 2 : (1u << 20);
        if (nc > (size_t)96 << 20) return;   /* cap 96M candidates */
        u32* na = (u32*)realloc(s_mc_addr, nc * 4); float* nv = (float*)realloc(s_mc_val, nc * 4);
        if (!na || !nv) return;
        s_mc_addr = na; s_mc_val = nv; s_mc_cap = nc;
    }
    s_mc_addr[s_mc_n] = a; s_mc_val[s_mc_n] = v; s_mc_n++;
}
static void mc_init_fn(u32 lo, u32 hi, void* c)
{
    mc_init_ctx* x = (mc_init_ctx*)c;
    for (unsigned long long a = lo; a + 3 <= hi; a += 4) {
        float v = mc_rd((u32)a);
        if (v == v && v >= x->lo && v <= x->hi && fabsf(v) >= 0.5f) mc_push((u32)a, v);
    }
}
typedef struct { float x, y, z, eps; u32 hits[256]; int n; } mc_find_ctx;
static void mc_find_fn(u32 lo, u32 hi, void* c)
{
    mc_find_ctx* x = (mc_find_ctx*)c;
    for (unsigned long long a = lo; a + 11 <= hi; a += 4) {
        float v = mc_rd((u32)a);
        if (!(fabsf(v - x->x) <= x->eps)) continue;   /* NaN-safe: NaN never matches */
        if (!(fabsf(mc_rd((u32)a + 4) - x->y) <= x->eps) || !(fabsf(mc_rd((u32)a + 8) - x->z) <= x->eps)) continue;
        if (x->n < 256) x->hits[x->n++] = (u32)a;
    }
}
/* Skate mod research: "joints <radius>" lists blocks of bone matrices (orthonormal 3x3 + translation within
 * <radius> cm of Cole's position) -- 4x4 with the translation in floats 12..14 (row-vector) or 3,7,11 (column),
 * and 3x4 rows (translation in 3,7,11). Consecutive hits at a fixed stride = one skeleton's pose buffer. */
static volatile u32 s_mc_jflat_addr; static volatile int s_mc_jflat_n; static volatile float s_mc_jflat_z;
static DWORD WINAPI mc_jflat_thread(LPVOID unused)
{   /* research: overwrite continuously so the write wins whatever the producer/consumer timing is */
    (void)unused;
    for (;;) {
        const int n = s_mc_jflat_n; const u32 a = s_mc_jflat_addr; const float z = s_mc_jflat_z;
        if (n <= 0) { Sleep(5); continue; }
        for (int jf = 0; jf < n; jf++) mc_wr(a + 64u * (u32)jf + 56u, z);
    }
    return 0;
}
typedef struct { float px, py, pz, r; u32 hits[2048]; u8 kind[2048]; int n; } mc_joint_ctx;
static int mc_unit3(float a, float b, float c) { const float l = a * a + b * b + c * c; return l > 0.81f && l < 1.21f; }
static void mc_joint_fn(u32 lo, u32 hi, void* cv)
{
    mc_joint_ctx* x = (mc_joint_ctx*)cv;
    for (unsigned long long a = (lo + 15ull) & ~15ull; a + 64 <= hi; a += 16) {
        float m[16];
        const float t0 = mc_rd((u32)a + 48), t1 = mc_rd((u32)a + 52), t2 = mc_rd((u32)a + 56);
        const float c0 = mc_rd((u32)a + 12), c1 = mc_rd((u32)a + 28), c2 = mc_rd((u32)a + 44);
        int kind = 0;
        if (fabsf(t0 - x->px) <= x->r && fabsf(t1 - x->py) <= x->r && fabsf(t2 - x->pz) <= x->r) kind = 1;
        else if (fabsf(c0 - x->px) <= x->r && fabsf(c1 - x->py) <= x->r && fabsf(c2 - x->pz) <= x->r) kind = 2;
        if (!kind) continue;
        for (int i = 0; i < 12; i++) m[i] = mc_rd((u32)a + 4 * i);
        if (!mc_unit3(m[0], m[1], m[2]) || !mc_unit3(m[4], m[5], m[6]) || !mc_unit3(m[8], m[9], m[10])) continue;
        if (fabsf(m[0] * m[4] + m[1] * m[5] + m[2] * m[6]) > 0.1f) continue;
        if (x->n < 2048) { x->hits[x->n] = (u32)a; x->kind[x->n] = (u8)kind; x->n++; }
    }
}
static int mc_still_readable(u32 a)
{
    MEMORY_BASIC_INFORMATION mi;
    return VirtualQuery(vm_base + a, &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

/* v219 MEMCMD goto autopilot: "goto <tx> <ty> <tol> <timeout_s> [zmax]" steers Cole's left stick toward a world point,
 * closed-loop on the player position global (default 0x00915430 = x y z, cm, Z up; "posaddr <a>" to change).
 * The stick is camera-relative, so the camera yaw is estimated online from the measured velocity vs the stick
 * direction (starts at 0 = camera facing +y/north, the hideout spawn view). Stalled > 1 s -> CROSS (jump/climb),
 * every 3rd stall CIRCLE (let go of a ledge). At the target but above zmax (roof, pillar): CIRCLE until below.
 * "stop" ends it. Never touches the right stick (camera). */
static u32   s_gt_pos = 0x00915430u;
static int   s_gt_on = 0; static float s_gt_tx, s_gt_ty, s_gt_tol, s_gt_zmax = 1e9f; static unsigned long long s_gt_end = 0;
static float s_gt_theta = 0.0f, s_gt_phis = 0.0f; static float s_gt_px = 0, s_gt_py = 0;
static unsigned long long s_gt_ts = 0, s_gt_stall = 0, s_gt_log = 0, s_gt_btn_until = 0, s_gt_steady = 0;
static int   s_gt_nstall = 0; static unsigned s_mc_btn = 0;
static float mc_wrap(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }
static void mc_goto_tick(void)
{
    if (!s_gt_on) return;
    const unsigned long long now = GetTickCount64();
    const float x = mc_rd(s_gt_pos), y = mc_rd(s_gt_pos + 4), z = mc_rd(s_gt_pos + 8);
    const float dx = s_gt_tx - x, dy = s_gt_ty - y, dist = sqrtf(dx * dx + dy * dy);
    s_mc_btn = now < s_gt_btn_until ? s_mc_btn : 0;
    if (dist <= s_gt_tol && z > s_gt_zmax && now < s_gt_end && x == x) {   /* above the target (roof / pillar): let go */
        g_pf_stick[0] = g_pf_stick[1] = 128; g_pf_stick_on = 1;
        if (now >= s_gt_btn_until + 600) { s_mc_btn = 0x2000u; s_gt_btn_until = now + 200; }
        if (now - s_gt_log >= 1000) { s_gt_log = now; mc_log("[goto] over target at z %.0f > %.0f -> CIRCLE\n", z, s_gt_zmax); }
        return;
    }
    if (dist <= s_gt_tol || now >= s_gt_end || !(x == x)) {
        s_gt_on = 0; s_mc_btn = 0;
        g_pf_stick[0] = g_pf_stick[1] = 128; g_pf_stick_on = 0;   /* release to the real pad (was stuck at centre) */
        mc_log("[goto] %s at %.0f %.0f %.0f (dist %.0f, theta %.0f deg)\n", dist <= s_gt_tol ? "ARRIVED" : "TIMEOUT", x, y, z, dist, s_gt_theta * 57.2958f);
        return;
    }
    if (!s_gt_ts) { s_gt_ts = now; s_gt_px = x; s_gt_py = y; s_gt_stall = now; s_gt_steady = now; }
    if (now - s_gt_ts >= 250) {
        const float dt = (float)(now - s_gt_ts) / 1000.0f, vx = (x - s_gt_px) / dt, vy = (y - s_gt_py) / dt;
        const float sp = sqrtf(vx * vx + vy * vy);
        if (sp > 150.0f && now - s_gt_steady >= 250 && now >= s_gt_btn_until) {     /* learn the camera yaw */
            const float th = mc_wrap(atan2f(vy, vx) - s_gt_phis);
            s_gt_theta = mc_wrap(s_gt_theta + 0.5f * mc_wrap(th - s_gt_theta));
        }
        if (sp > 60.0f) s_gt_stall = now;
        else if (now - s_gt_stall > 1000) {
            s_gt_nstall++; s_gt_stall = now;
            s_mc_btn = (s_gt_nstall % 3 == 0) ? 0x2000u : 0x4000u; s_gt_btn_until = now + 200;
            mc_log("[goto] stalled at %.0f %.0f %.0f -> %s\n", x, y, z, s_mc_btn == 0x2000u ? "CIRCLE" : "CROSS");
        }
        s_gt_ts = now; s_gt_px = x; s_gt_py = y;
    }
    const float phis = mc_wrap(atan2f(dy, dx) - s_gt_theta);
    if (fabsf(mc_wrap(phis - s_gt_phis)) > 0.35f) s_gt_steady = now;   /* big steer change: wait before learning */
    s_gt_phis = phis;
    int lx = 128 + (int)(127.0f * cosf(phis)), ly = 128 - (int)(127.0f * sinf(phis));
    g_pf_stick[0] = (unsigned char)(lx < 0 ? 0 : lx > 255 ? 255 : lx);
    g_pf_stick[1] = (unsigned char)(ly < 0 ? 0 : ly > 255 ? 255 : ly);
    g_pf_stick[2] = g_pf_stick[3] = 128; g_pf_stick_on = 1;
    if (now - s_gt_log >= 1000) { s_gt_log = now;
        mc_log("[goto] pos %.0f %.0f %.0f dist %.0f theta %.0f stick %d %d\n", x, y, z, dist, s_gt_theta * 57.2958f, lx, ly); }
}
static void mc_exec(char* line)
{
    char cmd[16] = {0}; double p[8] = {0}; unsigned addr = 0;
    int n = sscanf(line, "%15s", cmd);
    if (n < 1) return;
    mc_log("[memcmd] > %s", line); if (!strchr(line, '\n')) mc_log("\n");
    if (!strcmp(cmd, "init")) {
        mc_init_ctx c; c.lo = -1e9f; c.hi = 1e9f;
        sscanf(line, "%*s %lf %lf", &p[0], &p[1]); c.lo = (float)p[0]; c.hi = (float)p[1];
        s_mc_n = 0; mc_ranges(mc_init_fn, &c);
        mc_log("[memcmd] init: %zu candidates\n", s_mc_n);
    } else if (!strcmp(cmd, "changed") || !strcmp(cmd, "same") || !strcmp(cmd, "inc") || !strcmp(cmd, "dec")) {
        const int m = cmd[0] == 'c' ? 0 : cmd[0] == 's' ? 1 : cmd[0] == 'i' ? 2 : 3;
        size_t k = 0;
        for (size_t i = 0; i < s_mc_n; i++) {
            const u32 a = s_mc_addr[i];
            if (((i & 4095) == 0) && !mc_still_readable(a)) continue;
            const float v = mc_rd(a), o = s_mc_val[i];
            const int keep = m == 0 ? (v != o) : m == 1 ? (v == o) : m == 2 ? (v > o) : (v < o);
            if (keep && v == v) { s_mc_addr[k] = a; s_mc_val[k] = v; k++; }
        }
        s_mc_n = k; mc_log("[memcmd] %s: %zu candidates\n", cmd, s_mc_n);
    } else if (!strcmp(cmd, "list")) {
        int cnt = 32; sscanf(line, "%*s %d", &cnt);
        mc_log("[memcmd] %zu candidates\n", s_mc_n);
        for (size_t i = 0; i < s_mc_n && (int)i < cnt; i++) mc_log("[memcmd]   0x%08X %.4f\n", s_mc_addr[i], mc_rd(s_mc_addr[i]));
    } else if (!strcmp(cmd, "nbr")) {
        int cnt = 16; sscanf(line, "%*s %x %d", (int*)&addr, &cnt);
        addr &= ~3u;
        for (int i = 0; i < cnt; i++) mc_log("[memcmd]   0x%08X +%03X %.4f (0x%08X)\n", addr + 4 * i, 4 * i, mc_rd(addr + 4 * i), vm_read32(addr + 4 * i));
    } else if (!strcmp(cmd, "poke")) {
        sscanf(line, "%*s %x %lf", (int*)&addr, &p[0]); mc_wr(addr, (float)p[0]);
        mc_log("[memcmd] poke 0x%08X = %.4f\n", addr, mc_rd(addr));
    } else if (!strcmp(cmd, "holdf")) {   /* holdf <hexEA> <f32> <polls>: write one float now and on every pad poll (= every game frame)
                                            * for <polls> more frames; "holdf 0 0 0" stops. Trainer velocity test (+0xE20..) */
        int n = 0; sscanf(line, "%*s %x %lf %d", (int*)&addr, &p[0], &n);
        s_mc_hf_addr = addr; s_mc_hf_v = (float)p[0]; s_mc_hf_polls = addr ? n : 0;
        if (addr) mc_wr(addr, s_mc_hf_v);
        mc_log("[memcmd] holdf 0x%08X = %.4f for %d frames\n", addr, addr ? mc_rd(addr) : 0.0f, s_mc_hf_polls);
    } else if (!strcmp(cmd, "poke3")) {
        sscanf(line, "%*s %x %lf %lf %lf", (int*)&addr, &p[0], &p[1], &p[2]);
        for (int k = 0; k < 3; k++) mc_wr(addr + 4 * k, (float)p[k]);
        mc_log("[memcmd] poke3 0x%08X = %.3f %.3f %.3f\n", addr, mc_rd(addr), mc_rd(addr + 4), mc_rd(addr + 8));
    } else if (!strcmp(cmd, "find3") || !strcmp(cmd, "tp") || !strcmp(cmd, "tpd")) {
        int frames = 1;
        int got = sscanf(line, "%*s %lf %lf %lf %lf %lf %lf %lf %d", &p[0], &p[1], &p[2], &p[3], &p[4], &p[5], &p[6], &frames);
        static mc_find_ctx c; memset(&c, 0, sizeof c);
        c.x = (float)p[0]; c.y = (float)p[1]; c.z = (float)p[2]; c.eps = got >= 4 ? (float)p[3] : 0.01f;
        mc_ranges(mc_find_fn, &c);
        mc_log("[memcmd] find3 %.3f %.3f %.3f eps %.3f: %d hits\n", c.x, c.y, c.z, c.eps, c.n);
        s_mc_hold_n = 0;
        for (int i = 0; i < c.n; i++) {
            const u32 a = c.hits[i];
            float v[3] = { mc_rd(a), mc_rd(a + 4), mc_rd(a + 8) };
            mc_log("[memcmd]   0x%08X %.3f %.3f %.3f\n", a, v[0], v[1], v[2]);
            if (cmd[0] == 't' && got >= 7) {
                float t[3];
                for (int k = 0; k < 3; k++) t[k] = cmd[2] == 'd' ? v[k] + (float)p[4 + k] : (float)p[4 + k];
                for (int k = 0; k < 3; k++) mc_wr(a + 4 * k, t[k]);
                s_mc_hold_addr[s_mc_hold_n] = a; memcpy(s_mc_hold_v[s_mc_hold_n], t, sizeof t); s_mc_hold_n++;
            }
        }
        s_mc_hold_frames = cmd[0] == 't' ? (frames > 0 ? frames : 1) - 1 : 0;
        if (cmd[0] == 't') mc_log("[memcmd] teleported %d copies (held %d more polls)\n", s_mc_hold_n, s_mc_hold_frames);
    } else if (!strcmp(cmd, "joints")) {
        static mc_joint_ctx c; memset(&c, 0, sizeof c);
        double r = 250; sscanf(line, "%*s %lf", &r);
        c.px = mc_rd(0x00915430u); c.py = mc_rd(0x00915434u); c.pz = mc_rd(0x00915438u); c.r = (float)r;
        mc_ranges(mc_joint_fn, &c);
        mc_log("[memcmd] joints near %.1f %.1f %.1f r %.0f: %d hits\n", c.px, c.py, c.pz, c.r, c.n);
        for (int i = 0; i < c.n; i++) {
            const u32 a = c.hits[i]; const int k = c.kind[i];
            const float tx = k == 1 ? mc_rd(a + 48) : mc_rd(a + 12), ty = k == 1 ? mc_rd(a + 52) : mc_rd(a + 28), tz = k == 1 ? mc_rd(a + 56) : mc_rd(a + 44);
            mc_log("[memcmd]   0x%08X %s d=(%.1f %.1f %.1f) r0=(%.3f %.3f %.3f)\n", a, k == 1 ? "rowT" : "colT",
                   tx - c.px, ty - c.py, tz - c.pz, mc_rd(a), mc_rd(a + 4), mc_rd(a + 8));
        }
    } else if (!strcmp(cmd, "jflat")) {   /* jflat <addr> <count> <z>: every poll, force each 64-byte matrix's z */
        double zv = 0; s_mc_jflat_n = 0;
        { unsigned ja = 0; int jn = 0; sscanf(line, "%*s %x %d %lf", &ja, &jn, &zv); s_mc_jflat_n = 0; s_mc_jflat_addr = ja; s_mc_jflat_z = (float)zv; s_mc_jflat_n = jn; }
        { static int started = 0; if (!started) { started = 1; CreateThread(NULL, 0, mc_jflat_thread, NULL, 0, NULL); } }
        mc_log("[memcmd] jflat 0x%08X x%d z=%.1f\n", s_mc_jflat_addr, s_mc_jflat_n, s_mc_jflat_z);
    } else if (!strcmp(cmd, "dump")) {   /* dump <addr> <bytes> <file>: raw guest bytes (big-endian) to a file */
        unsigned n = 0; char fn[400] = {0};
        sscanf(line, "%*s %x %x %399s", (int*)&addr, &n, fn);
        FILE* f = fn[0] ? fopen(fn, "wb") : NULL;
        if (f && n && n <= (64u << 20)) { fwrite(vm_base + addr, 1, n, f); fclose(f); mc_log("[memcmd] dump 0x%08X +0x%X -> %s\n", addr, n, fn); }
        else { if (f) fclose(f); mc_log("[memcmd] dump failed\n"); }
    } else if (!strcmp(cmd, "dumpall")) {   /* dumpall <prefix>: every committed guest range except RSX VRAM ->
                                              * <prefix>.idx (start size per line) + <prefix>.bin (concatenated) */
        char pre[400] = {0};
        sscanf(line, "%*s %399s", pre);
        if (!pre[0]) { mc_log("[memcmd] dumpall needs a prefix\n"); return; }
        char fn[420]; snprintf(fn, sizeof fn, "%s.bin", pre);
        FILE* bin = fopen(fn, "wb");
        snprintf(fn, sizeof fn, "%s.idx", pre);
        FILE* idx = fopen(fn, "w");
        unsigned long long total = 0; int nr = 0;
        if (bin && idx) {
            unsigned long long a = 0x00010000ull;
            while (a < 0xF0000000ull) {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery(vm_base + a, &mi, sizeof mi)) break;
                unsigned long long end = (unsigned long long)((uint8_t*)mi.BaseAddress - vm_base) + mi.RegionSize;
                if (end > 0xF0000000ull) end = 0xF0000000ull;
                const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
                const int vram = a >= 0x10000000ull && a < 0x20000000ull;
                if (mi.State == MEM_COMMIT && (mi.Protect & ok) && !(mi.Protect & PAGE_GUARD) && !vram) {
                    fwrite(vm_base + a, 1, (size_t)(end - a), bin);
                    fprintf(idx, "%08llX %llX\n", a, end - a);
                    total += end - a; nr++;
                }
                if (end <= a) break;
                a = end;
            }
        }
        if (bin) fclose(bin);
        if (idx) fclose(idx);
        mc_log("[memcmd] dumpall %s: %d ranges, %llu MB\n", pre, nr, total >> 20);
    } else if (!strcmp(cmd, "wwatch")) {   /* wwatch <hexEA> [hexlen]: aim the PPU_WWATCH store watch live (needs PPU_WWATCH
                                             * set at launch); hits go to stderr as [ww] with the writing guest function */
        extern volatile uint32_t g_ww_live, g_ww_live_len; extern volatile long g_ww_live_gen; extern uint32_t g_ww_lo, g_ww_hi;
        unsigned len = 0x20; sscanf(line, "%*s %x %x", (int*)&addr, &len);
        if (len < 0x20) len = 0x20;
        g_ww_live_len = len; g_ww_live = addr; g_ww_lo = addr & ~15u; g_ww_hi = (addr & ~15u) + len; g_ww_live_gen++;
        mc_log("[memcmd] wwatch 0x%08X +0x%X%s\n", addr, len, getenv("PPU_WWATCH") ? "" : "  (PPU_WWATCH was not set at launch: lifted stores bypass the watch!)");
    } else if (!strcmp(cmd, "icnmark")) {   /* coop v8: icnmark <tag> -- start a new ICNEW phase (ppu_loader.cpp) */
        extern void ps3_icn_mark(const char* tag);
        char tag[48] = {0}; sscanf(line, "%*s %47s", tag);
        ps3_icn_mark(tag);
        { extern void rsx_fxprobe_mark(const char* tag); rsx_fxprobe_mark(tag); }   /* coop v8 FXPROBE (rsx_d3d12_backend.c) */
        mc_log("[memcmd] icnmark %s\n", tag);
    } else if (!strcmp(cmd, "goto")) {
        double t[5] = {0, 0, 150, 60, 1e9};
        sscanf(line, "%*s %lf %lf %lf %lf %lf", &t[0], &t[1], &t[2], &t[3], &t[4]);
        s_gt_zmax = (float)t[4];
        s_gt_tx = (float)t[0]; s_gt_ty = (float)t[1]; s_gt_tol = (float)t[2];
        s_gt_end = GetTickCount64() + (unsigned long long)(t[3] * 1000.0); s_gt_ts = 0; s_gt_nstall = 0; s_gt_on = 1;
        mc_log("[goto] -> %.0f %.0f (tol %.0f, %.0f s) from %.0f %.0f %.0f\n", s_gt_tx, s_gt_ty, s_gt_tol, t[3], mc_rd(s_gt_pos), mc_rd(s_gt_pos + 4), mc_rd(s_gt_pos + 8));
    } else if (!strcmp(cmd, "theta")) {
        double t = 0; sscanf(line, "%*s %lf", &t); s_gt_theta = (float)(t / 57.2958); mc_log("[goto] theta = %.0f deg\n", t);
    } else if (!strcmp(cmd, "stop")) {
        s_gt_on = 0; s_mc_btn = 0; g_pf_stick[0] = g_pf_stick[1] = 128; g_pf_stick_on = 0; mc_log("[goto] stopped, sticks released\n");
    } else if (!strcmp(cmd, "posaddr")) {
        sscanf(line, "%*s %x", &s_gt_pos); mc_log("[goto] position at 0x%08X\n", s_gt_pos);
    } else if (!strcmp(cmd, "pos")) {
        mc_log("[memcmd] pos %.1f %.1f %.1f\n", mc_rd(s_gt_pos), mc_rd(s_gt_pos + 4), mc_rd(s_gt_pos + 8));
    } else mc_log("[memcmd] unknown command\n");
}
static void pad_memcmd_poll(void)
{
    static int on = -1; static char path[512]; static unsigned long long next = 0;
    if (on < 0) { const char* e = getenv("MEMCMD"); on = (e && *e) ? 1 : 0;
                  if (on) { snprintf(path, sizeof path, "%s", e); char op[520]; snprintf(op, sizeof op, "%s.out", e); s_mc_out = fopen(op, "ab");
                            printf("[memcmd] v219 memory tool on (%s)\n", path); fflush(stdout); } }
    if (!on) return;
    if (s_mc_hf_polls > 0) { s_mc_hf_polls--; mc_wr(s_mc_hf_addr, s_mc_hf_v); }
    if (s_mc_hold_frames > 0) { s_mc_hold_frames--;
        for (int i = 0; i < s_mc_hold_n; i++) for (int k = 0; k < 3; k++) mc_wr(s_mc_hold_addr[i] + 4 * k, s_mc_hold_v[i][k]); }
    mc_goto_tick();
    const unsigned long long now = GetTickCount64();
    if (now < next) return;
    next = now + 100;
    FILE* f = fopen(path, "rb");
    if (!f) return;
    char buf[256]; buf[0] = 0;
    const int got = fgets(buf, sizeof buf, f) != NULL;
    fclose(f);
    if (!got || !buf[0] || buf[0] == '\n' || buf[0] == '\r') return;
    { FILE* w = fopen(path, "wb"); if (w) fclose(w); }
    for (char* q = buf; *q; q++) if (*q == '\r') *q = ' ';
    mc_exec(buf);
}
#else
static void pad_memcmd_poll(void) {}
static unsigned s_mc_btn = 0;
#endif

/* rsx_ui_trainer.h: set by the title (e.g. the inFamous trainer), NULL = nothing. */
void (*volatile g_pad_title_tick_fn)(void) = 0;
/* rsx_ui_trainer.h: port 0 as the game last received it (left stick x,y 0..255; digital DIGITAL2<<8 | DIGITAL1). */
volatile unsigned char g_pad_title_lstick[2] = {128, 128};
volatile unsigned g_pad_title_digital = 0;

s32 cellPadGetData(u32 port_no, CellPadData* data_guest)
{
    { static int _once = 0;
      if (!_once++) printf("[cellPad] GetData polling begins (port %u)\n", port_no); }
    { static unsigned long long _hb_n = 0, _hb_t = 0; _hb_n++;
      unsigned long long _now = pad_now_ms();
      if (!_hb_t) _hb_t = _now;
      if (_now - _hb_t >= 10000) { _hb_t = _now;
          printf("[cellPad] heartbeat: %llu GetData calls so far, port0 buttons=0x%04X connected=%d\n",
                 _hb_n, (unsigned)s_host_state[0].buttons, s_host_state[0].connected); fflush(stdout); } }
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= s_max_connect || !data_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* v1042Z GF_MOUSE_PROBE=1: Godfather's pad driver (func_004A4424, call site lr 0x004A45C8) keeps its controller
    * object in r31; its stick floats live at +0x28 (left x/y) and +0x30 (right x/y). Point the runtime read-watch at
    * them once (needs PPU_RWATCH set so the checked accessors run) -> [rwatch-fn] lists the camera's readers. */
    { static int pr = -1;
      if (pr < 0) { const char* e = getenv("GF_MOUSE_PROBE"); pr = (e && *e == '1') ? 1 : 0; }
      if (pr == 1) { extern uint32_t ppu_cur_lr(void); extern uint64_t ppu_cur_gpr(int r); extern void ppu_rwatch_set(uint32_t lo, uint32_t len);
          if (ppu_cur_lr() == 0x004A45C8u) { const u32 obj = (u32)ppu_cur_gpr(31);
              printf("[mouse-probe] v1042Z pad driver object 0x%08X (right stick floats at 0x%08X)\n", obj, obj + 0x30); fflush(stdout);
              ppu_rwatch_set(obj + 0x28, 0x10); pr = 2; } } }
    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadData _d; CellPadData* data = &_d;
    memset(data, 0, sizeof(CellPadData));

    /* Poll fresh state */
    pad_poll_backend();

    /* YDKJ_INJECT_PAD: the game's wait loop polls the pad (cellPadSetActDirect /
     * GetData) -- if it's parked on a "press button" prompt after loading, no
     * host input means it waits forever. Inject a real button PULSE (CROSS+START+
     * CIRCLE) so it advances. Legit input simulation, not forged pixels. */
    /* NOTE: cache getenv() -- this runs on every poll from multiple threads, and the
     * uncached version crashed deterministically (varying fault address = race). Every
     * other probe in this tree uses the cached `static int x=-1` pattern; match it. */
    static int  s_inj   = -1;
    static u16  s_btn   = 0x0008u;   /* default START */
    static long s_delay = 3000;
    if (s_inj < 0) {
        s_inj = getenv("YDKJ_INJECT_PAD") ? 1 : 0;
        const char* be = getenv("YDKJ_PAD_BTN");   if (be) s_btn   = (u16)strtoul(be,0,0);
        const char* de = getenv("YDKJ_PAD_DELAY"); if (de) s_delay = strtol(de,0,0);
    }
    /* Gate on WALL-CLOCK, not poll count: the game polls the pad hundreds of thousands
     * of times inside an early wait loop, so any count-based delay fires during init
     * (crash: guest ctr=0x0005F8A0 r3=0 -- button handler on an unconstructed object).
     * YDKJ_PAD_DELAY is now SECONDS to wait before the first injected press. */
    if (s_inj && port_no < PAD_MAX_HOST_PORTS) {
        static long _pc = 0; _pc++;
        /* Cheap counter gate FIRST: clock() is a slow call and this poll loop is hammered
         * by the guest from multiple threads; sampling it every poll perturbs timing. Only
         * consult the clock once every 256 polls, and never after arming. */
        static int     s_armed = 0;
        static clock_t s_t0    = 0;
        if (!s_armed) {
            if ((_pc & 0xFF) == 0) {
                if (!s_t0) s_t0 = clock();
                else if ((double)(clock() - s_t0) / (double)CLOCKS_PER_SEC >= (double)s_delay) s_armed = 1;
            }
            goto skip_inject;
        }
        u16 btn = s_btn; long delay = 0;
        /* Delay past init, then a single clean pulse every ~180 calls (press 15, release 165). */
        if (_pc > delay) {
            long ph = (_pc - delay) % 180;
            if (ph < 15) s_host_state[port_no].buttons |= btn;
            else         s_host_state[port_no].buttons &= ~btn;
            s_host_state[port_no].connected = 1;
        }
    }
skip_inject: ;
    if (port_no == 0) pad_namekb_getdata_step();   /* v1043T-4 name-entry typing steps */
    if (port_no == 0) pad_menu_getdata_step();     /* v1107X menu pointer bursts */

    if (port_no >= PAD_MAX_HOST_PORTS || (!s_host_state[port_no].connected && port_no != 0)) {
        goto emit;   /* data stays zeroed -> len=0 */
    }
    /* port 0 is always a valid (virtual) pad; s_host_state[0] is neutral if no
     * physical device, which yields no-buttons-pressed data below. */

    {
    PadHostState* hs = &s_host_state[port_no];
    /* An in-game system dialog (libs/video/rsx_ui_overlay.c) owns the pad while it is up, as on
     * hardware: the title sees a neutral pad, and the press that answered the dialog (held, or
     * latched) never reaches it. */
    if (port_no == 0) { g_pad_phys_buttons = hs->buttons; g_pad_phys_ms = GetTickCount64(); }   /* v1093M */
    int pad_gate_block = 0;
    { extern int uiov_pad_gate(void);
      static PadHostState s_neutral = { 1, 0, 128, 128, 128, 128 };
      int gate = uiov_pad_gate();
      if (gate) (void)PAD_LATCH_TAKE(port_no);
      if (gate == 1) { s_neutral.connected = hs->connected; hs = &s_neutral; pad_gate_block = 1; } }
    u32 setting = s_port_setting[port_no];
    /* v1085W (user: "while smoking, the player walks instead of the normal run"): cap the left stick's deflection at the
     * walk amount (PAD_KBM_WALK, default 64 of 127) on port 0 while g_gf_force_walk is set -- WASD (full deflection = jog)
     * and a controller's stick alike. The direction is kept. */
    if (port_no == 0 && g_gf_force_walk && !pad_gate_block) {
        static int walk_env = -1; if (walk_env < 0) { walk_env = pad_envi("PAD_KBM_WALK", 64); if (walk_env < 20) walk_env = 20; if (walk_env > 127) walk_env = 127; }
        const int walk = g_gf_force_walk > 1 ? (g_gf_force_walk > 127 ? 127 : g_gf_force_walk) : walk_env;   /* v1086W: gf_smoke.c passes the cap itself (1 = PAD_KBM_WALK) */
        const float dx = (float)hs->analog_lx - 128.0f, dy = (float)hs->analog_ly - 128.0f, m = sqrtf(dx * dx + dy * dy);
        if (m > (float)walk) {
            const float k = (float)walk / m;
            hs->analog_lx = (u8)(128.0f + dx * k + 0.5f); hs->analog_ly = (u8)(128.0f + dy * k + 0.5f);
        }
    }

    /* Determine data length based on port settings */
    s32 len = CELL_PAD_LEN_CHANGE_DEFAULT;
    if (setting & CELL_PAD_SETTING_SENSOR_ON)
        len = CELL_PAD_LEN_CHANGE_SENSOR_ON;
    else if (setting & CELL_PAD_SETTING_PRESS_ON)
        len = CELL_PAD_LEN_CHANGE_PRESS_ON;

    data->len = len;
    /* button[0] is reserved and 0 on hardware; button[1] carries the pad mode
     * in its high nibble (0x7) and len/2 in its low one. GH3 drops any packet
     * whose button[0] is non-zero -- with len (8) there, every press was
     * received and thrown away, and "Press any button" never advanced. */
    data->button[0] = 0;
    data->button[1] = (u16)(0x70 | ((len / 2) & 0xF));

    /* Digital buttons. hs->buttons packs both halves into one 16-bit mask
     * (SELECT=bit0..LEFT=bit7 = DIGITAL1; L2=bit8..SQUARE=bit15 = DIGITAL2).
     * Split correctly — writing the whole value into DIGITAL1 with DIGITAL2=0
     * left every face button (cross/circle/triangle/square, L1/L2/R1/R2) dead.
     * (via sagemono, PR #42) */
    /* Held buttons plus anything pressed since the last delivered packet. The
     * latch is only consumed when this packet is actually delivered (len > 0,
     * see emit), so a press is never eaten by a len = 0 read. */
    u16 eff_btns = (u16)(hs->buttons | (pad_gate_block ? 0 : (u16)s_latch[port_no]));
    if (port_no == 0 && !pad_gate_block && s_any_input_start_reports > 0) {
        eff_btns |= CELL_PAD_CTRL_START;
        InterlockedDecrement(&s_any_input_start_reports);
    }
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] = (u16)(eff_btns & 0xFF);
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] = (u16)((eff_btns >> 8) & 0xFF);
    if (port_no == 0) { extern volatile int g_gf_cross_held; g_gf_cross_held = (eff_btns >> 8) & 0x40 ? 1 : 0; }
    if (port_no == 0) { extern volatile int g_gf_combat_btn; g_gf_combat_btn = (int)((eff_btns >> 8) & 0x8F); }   /* v1087C: L2 R2 L1 R1 SQUARE (aim / fire / lock-on / attack) held, read by gf_smoke.c */   /* v1085W: Cross = sprint (Shift / pad), read by gf_smoke.c */
    { static int said = 0;
      if (eff_btns && !hs->buttons && said < 5) { said++;
          printf("[cellPad] delivered a latched press 0x%04X (released before this read)\n", eff_btns);
          fflush(stdout); } }

    /* PAD_AUTOPRESS: headless bring-up input -- pulse CROSS then START every few
     * seconds of polling so boot screens that wait for input advance without a
     * human at the pad. Env-gated test scaffolding, off by default. */
    /* v1041R benchmark: once the gameplay world has loaded (pad_bench_world_mark), START is never pressed again
     * (in-game it opens the pause menu), CROSS only for PAD_AUTOPRESS_GRACE_MS (default 12000) to clear a
     * post-load prompt, then no synthetic input at all. PAD_AUTOPRESS_FOREVER=1 = old endless pulses. */
    { static int s_ap = -1, s_forever = 0, s_grace = 12000; static unsigned s_apn = 0;
      if (s_ap < 0) { s_ap = (getenv("PAD_AUTOPRESS") && !s_rr_mode) ? 1 : 0;   /* v1129RR: off while recording / replaying */
          const char* f = getenv("PAD_AUTOPRESS_FOREVER"); s_forever = f && *f == '1';
          const char* g = getenv("PAD_AUTOPRESS_GRACE_MS"); if (g && *g) s_grace = atoi(g); }
      /* v1043N PAD_AUTOPRESS_PAUSE=<from_s>:<to_s>: no autopress between those wall-clock seconds after the first poll
       * (lets a PAD_SCRIPT drive one screen, e.g. the name entry). */
      static double s_pf = -1.0, s_pt = -1.0; static unsigned long long s_ap_t0 = 0;
      if (s_pf < 0.0) { s_pf = 0.0; const char* pz = getenv("PAD_AUTOPRESS_PAUSE");
                        if (pz && *pz) { s_pf = atof(pz); const char* c = strchr(pz, ':'); s_pt = c ? atof(c + 1) : 0.0; } }
      if (!s_ap_t0) s_ap_t0 = GetTickCount64();
      const double ap_sec = (double)(GetTickCount64() - s_ap_t0) / 1000.0;
      if (s_ap && port_no == 0 && s_pt > 0.0 && ap_sec >= s_pf && ap_sec < s_pt) { /* paused */ }
      else if (s_ap && port_no == 0) {
          const long long wm = g_bench_world_ms;
          const long long since = wm ? (long long)GetTickCount64() - wm : -1;
          /* v1046Q: phase from the wall clock (4 s cycle, CROSS 0-200 ms, START 2000-2200 ms = the old 240-poll cycle
           * at 60 polls/s). Counting polls made the presses 3x shorter at 180 FPS and the APT menus missed them (soaks
           * Y04/Y05 sat in the main menu). PAD_AUTOPRESS_POLLS=1 = old poll counting. */
          static int s_appoll = -1; if (s_appoll < 0) { const char* q = getenv("PAD_AUTOPRESS_POLLS"); s_appoll = (q && *q == '1'); }
          unsigned ph = s_appoll ? (s_apn++ % 240) : (unsigned)(((unsigned long long)(ap_sec * 1000.0) % 4000ull) * 240ull / 4000ull);
          if (!wm || s_forever) {
              if (ph < 12)
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= 0x40;   /* CROSS */
              else if (ph >= 120 && ph < 132)
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= 0x08;   /* START */
          } else if (since < s_grace) {
              if (ph < 12) data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= 0x40;   /* CROSS only */
          } else {
              static int said = 0;
              if (!said) { said = 1; fprintf(stderr, "[BENCH] v1041R autopress stopped %lld ms after the world mark%c", since, 10); }
          }
      } }

    /* PAD_SCRIPT="<sec>:<mask>,<sec>:<mask>,..." -- press a NAMED button at a
     * given wall-clock second, each held ~250 ms. PAD_AUTOPRESS only pulses
     * CROSS and START, which is enough to clear a "press start" screen but not
     * to navigate a menu: You Don't Know Jack needs DOWN to move off its name
     * field before CROSS means anything. Masks are CELL_PAD_CTRL_* packed as
     * (DIGITAL2 << 8) | DIGITAL1: START 0x0008, UP 0x0010, DOWN 0x0040,
     * LEFT 0x0080, RIGHT 0x0020, CROSS 0x4000, CIRCLE 0x2000, TRIANGLE 0x1000.
     * Legit input simulation on the same footing as PAD_AUTOPRESS/PAD_STICK. */
    { static int s_sc = -1;
      static struct { double t; unsigned mask; } ev[32]; static int n_ev = 0;
      static unsigned long long t0 = 0;   /* not ULONGLONG: this builds on POSIX too */
      if (s_sc < 0) {
          s_sc = 0;
          const char* e = getenv("PAD_SCRIPT");
          if (e && *e && !s_rr_mode) {   /* v1129RR: off while recording / replaying */
              s_sc = 1; t0 = GetTickCount64();
              const char* p = e;
              while (*p && n_ev < 32) {
                  double t = strtod(p, (char**)&p);
                  if (*p == ':') p++;
                  unsigned m = (unsigned)strtoul(p, (char**)&p, 0);
                  ev[n_ev].t = t; ev[n_ev].mask = m; n_ev++;
                  while (*p == ',' || *p == ' ') p++;
              }
              printf("[cellPad] PAD_SCRIPT: %d event(s)\n", n_ev);
          }
      }
      if (s_sc && port_no == 0) {
          double now = (double)(GetTickCount64() - t0) / 1000.0;
          /* Latch, do not window. A fixed time window is missed whenever the
           * title stops polling the pad for longer than the window (it does
           * exactly that on a loading screen), so the press silently never
           * happens. Fire on the first poll at or after the event time and
           * hold it for a fixed number of polls, which is what a real press
           * looks like regardless of poll rate. */
          static int fired[32], held[32];
          for (int i = 0; i < n_ev; i++) {
              if (!fired[i] && now >= ev[i].t) { fired[i] = 1; held[i] = 40;
                  if (ev[i].mask & 0x10000u) {   /* v1043N: mask bit 16 = memory snapshot (GF_MEMDIFF), no press */
                      held[i] = 0; if (g_pad_snapshot_fn) g_pad_snapshot_fn();
                      printf("[cellPad] PAD_SCRIPT t=%.1fs memory snapshot\n", ev[i].t); fflush(stdout); } }
              if (held[i] > 0) {
                  held[i]--;
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(ev[i].mask & 0xFF);
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((ev[i].mask >> 8) & 0xFF);
                  static int said[32];
                  if (!said[i]) { said[i] = 1;
                      printf("[cellPad] PAD_SCRIPT t=%.1fs press 0x%04X\n", ev[i].t, ev[i].mask); }
              }
          }
      } }

    /* v1042Z GF_NET_AUTODISMISS: cellNetCtl.c schedules one CROSS ~3 s after the (canceled) network-start dialog. Offline,
     * Godfather then shows "Unable to connect to EA servers. Do you want to try again?" with NO preselected; the press
     * answers NO. Cancelled if the player presses anything first (the press can never land on the main menu after a
     * manual answer). Log [net-autodismiss]. */
    { extern volatile unsigned long long g_pad_autocross_at;
      static int held = 0;
      if (g_pad_autocross_at && port_no == 0) {
          const unsigned long long now = GetTickCount64();
          if (s_host_state[0].buttons != 0) { g_pad_autocross_at = 0; printf("[net-autodismiss] player input first -- no automatic answer\n"); }
          else if (now >= g_pad_autocross_at) { g_pad_autocross_at = 0; held = 40; printf("[net-autodismiss] CROSS (answer NO to the EA reconnect prompt)\n"); }
      }
      if (held > 0 && port_no == 0) { held--; data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)0x40; } }
    if (g_pad_inject_polls > 0 && port_no == 0) {   /* v1112B */
        g_pad_inject_polls--; data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)g_pad_inject_d1; data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)g_pad_inject_d2; }

    /* PAD_FILE=<path> -- closed-loop input. Each poll, if the file holds a
     * button mask (same encoding as PAD_SCRIPT), press it for a fixed number
     * of polls and empty the file. A driver outside the process can then look
     * at the log or a frame dump, decide which screen the title is on, and
     * write the next button -- which a fixed time schedule cannot do, because
     * a menu system with several screens wanders when driven blind.
     *
     *   echo 0x0008 > pad.txt    # START
     *   echo 0x4000 > pad.txt    # CROSS
     *   echo 0x0040 4 > pad.txt  # DOWN for 4 polls (menus auto-repeat a
     *                            # long hold; the default is 40)
     */
    if (port_no == 0) pad_memcmd_poll();   /* v219 MEMCMD bench memory tool */
    /* Title per-frame hook (rsx_ui_trainer.h): the game's main PPU thread inside an HLE call, between frames. */
    if (port_no == 0 && g_pad_title_tick_fn) g_pad_title_tick_fn();
#ifdef PS3RECOMP_COOP_MOD
    /* Co-op combat: apply queued peer damage through the game's own ModifyHealth.  This is the game's main
     * PPU thread inside an HLE call, which coop_health_game_thread_tick() requires (coop_health.h). */
    { extern void coop_health_game_thread_tick(void);
      if (port_no == 0) coop_health_game_thread_tick(); }
    /* Co-op world sync (v5): apply queued actor pose / freeze / kill / time-of-day writes (coop_world_game.h). */
    { extern void coop_world_game_thread_tick(void);
      if (port_no == 0) coop_world_game_thread_tick(); }
#endif
    { static int s_pf = -1; static char pf_path[512];
      static unsigned held_mask = 0; static int held_n = 0;
      if (s_pf < 0) { const char* e = getenv("PAD_FILE");
                      s_pf = (e && *e) ? 1 : 0;
                      if (s_pf) { strncpy(pf_path, e, sizeof pf_path - 1); pf_path[sizeof pf_path - 1] = 0; } }
      if (s_pf && port_no == 0) {
          static unsigned long long pf_next = 0; const unsigned long long pf_now = GetTickCount64();   /* v219: fopen per poll cost ~1 % of the main thread */
          if (held_n <= 0 && pf_now >= pf_next) {
              pf_next = pf_now + 50;
              FILE* f = fopen(pf_path, "rb");
              if (f) {
                  char buf[96]; buf[0] = 0; int took = 0;
                  if (fgets(buf, sizeof buf, f)) {
                      char* rest;
                      unsigned m = (unsigned)strtoul(buf, &rest, 0);
                      int n = (int)strtol(rest, &rest, 0);
                      /* v219: optional "lx ly rx ry" (0-255) after the hold count = hold the sticks there until the
                       * next stick line ("0 0 128 128 128 128" centres them). Mask 0 = sticks only. */
                      int sv[4], ns = sscanf(rest, "%d %d %d %d", &sv[0], &sv[1], &sv[2], &sv[3]);
                      if (ns == 4) { for (int k = 0; k < 4; k++) g_pf_stick[k] = (unsigned char)(sv[k] < 0 ? 0 : sv[k] > 255 ? 255 : sv[k]);
                                     g_pf_stick_on = 1; took = 1;
                                     printf("[cellPad] PAD_FILE sticks %d %d %d %d\n", sv[0], sv[1], sv[2], sv[3]); fflush(stdout); }
                      if (m) { held_mask = m; held_n = n > 0 ? n : 40; took = 1;
                               printf("[cellPad] PAD_FILE press 0x%04X\n", m); fflush(stdout); }
                  }
                  fclose(f);
                  if (took) { FILE* w = fopen(pf_path, "wb"); if (w) fclose(w); }
              }
          }
          g_pad_file_held = held_n > 0 ? held_mask : 0;   /* v219: the in-game UI overlay reads it too */
          if (held_n > 0) { held_n--;
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(held_mask & 0xFF);
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((held_mask >> 8) & 0xFF); }
      } }

    /* Analog sticks */
    /* PAD_STICK="lx,ly,rx,ry" (0-255, 128 = centred): hold the analog sticks at
     * fixed positions. Rubber Ducky drives its camera from the sticks and never
     * moves on its own, so with no host pad the view is frozen wherever it
     * started -- which can leave the subject of the demo off-screen or too far
     * away to make out. Legit input simulation, same footing as YDKJ_INJECT_PAD. */
    { static int s_st = -1;
      static unsigned char s_v[4] = {128,128,128,128};
      /* Optional 5th/6th fields: the seconds the hold starts and ends, so the menus
       * before gameplay are not driven by it (PAD_STICK="128,0,128,128,300,320"). */
      static double s_from = 0, s_until = 1e30; static unsigned long long s_t0 = 0;
      if (!s_t0) s_t0 = GetTickCount64();
      if (s_st < 0) { const char* e = getenv("PAD_STICK");
        s_st = e ? 1 : 0;
        if (e) { int a=128,b=128,c=128,d=128;
                 sscanf(e, "%d,%d,%d,%d,%lf,%lf", &a,&b,&c,&d,&s_from,&s_until);
                 s_v[0]=(unsigned char)a; s_v[1]=(unsigned char)b;
                 s_v[2]=(unsigned char)c; s_v[3]=(unsigned char)d; } }
      if (s_st && (double)(GetTickCount64() - s_t0) / 1000.0 >= s_from && (double)(GetTickCount64() - s_t0) / 1000.0 < s_until) { hs->analog_lx = s_v[0]; hs->analog_ly = s_v[1];
                  hs->analog_rx = s_v[2]; hs->analog_ry = s_v[3];
                  hs->connected = 1; }
      /* PAD_SWEEP=<seconds per step>: walk the sticks through a fixed set of
       * deflections instead of holding one. Guessing a single stick position and
       * re-running costs ~5 minutes a try; a sweep answers "can input move the
       * camera anywhere useful" in one run, and the frame dumps say which step
       * paid off. Steps: centre, each left-stick direction, then each right. */
      { static int sw = -1; static clock_t t0 = 0;
        if (sw < 0) { const char* e = getenv("PAD_SWEEP");
                      sw = e ? atoi(e) : 0; if (sw < 0) sw = 0; }
        if (sw) {
            static const unsigned char steps[9][4] = {
                {128,128,128,128},
                {128,  0,128,128}, {128,255,128,128},
                {  0,128,128,128}, {255,128,128,128},
                {128,128,128,  0}, {128,128,128,255},
                {128,128,  0,128}, {128,128,255,128},
            };
            if (!t0) t0 = clock();
            long el = (long)((double)(clock() - t0) / (double)CLOCKS_PER_SEC);
            int k = (int)((el / (sw > 0 ? sw : 1)) % 9);
            hs->analog_lx = steps[k][0]; hs->analog_ly = steps[k][1];
            hs->analog_rx = steps[k][2]; hs->analog_ry = steps[k][3];
            hs->connected = 1;
            { static int lastk = -1;
              if (k != lastk) { lastk = k;
                fprintf(stderr, "[PADSWEEP] step %d: l=(%u,%u) r=(%u,%u)%c", k,
                        steps[k][0], steps[k][1], steps[k][2], steps[k][3], 10); } }
        } } }
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = hs->analog_rx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = hs->analog_ry;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X]  = hs->analog_lx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y]  = hs->analog_ly;
    if (g_pf_stick_on && port_no == 0) {   /* v219 PAD_FILE stick hold */
        data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X]  = g_pf_stick[0];
        data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y]  = g_pf_stick[1];
        data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = g_pf_stick[2];
        data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = g_pf_stick[3];
    }
    /* v1041R BENCH_MOVE=1: from BENCH_MOVE_AFTER_MS (default 15000) after the world mark, hold the left stick
     * forward and slowly turn the camera with the right stick, reversing every BENCH_MOVE_FLIP_MS (default 6000),
     * so a benchmark exercises streaming and traversal instead of standing still. */
    if (port_no == 0 && g_bench_world_ms) {
        static int bm = -1, after = 15000, flipms = 6000;
        if (bm < 0) { const char* e = getenv("BENCH_MOVE"); bm = e && *e == '1' && !s_rr_mode;   /* v1129RR */
            const char* a = getenv("BENCH_MOVE_AFTER_MS"); if (a && *a) after = atoi(a);
            const char* f = getenv("BENCH_MOVE_FLIP_MS"); if (f && *f) flipms = atoi(f);
            if (flipms < 500) flipms = 500; }
        const long long since = (long long)GetTickCount64() - g_bench_world_ms;
        if (bm && since >= after) {
            static int said = 0;
            if (!said) { said = 1; fprintf(stderr, "[BENCH] v1041R scripted movement started%c", 10); }
            const int dir = (int)(((since - after) / flipms) & 1);
            data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X]  = 0x80;
            data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y]  = 0x00;              /* full forward */
            data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = dir ? 0x50 : 0xB0; /* gentle turn */
            data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = 0x80;
        }
    }
    if (port_no == 0 && s_mc_btn) {   /* v219 MEMCMD goto: jump / let go */
        data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(s_mc_btn & 0xFF);
        data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((s_mc_btn >> 8) & 0xFF);
    }

    if (g_pad_filter)
        g_pad_filter(port_no, data->button, data->len);
    if (port_no == 0) {
        g_pad_title_lstick[0] = (unsigned char)data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X];
        g_pad_title_lstick[1] = (unsigned char)data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y];
        g_pad_title_digital = (((unsigned)data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] & 0xFFu) << 8) |
                              ((unsigned)data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] & 0xFFu);
    }
#ifdef PS3RECOMP_COOP_MOD
    if (port_no == 0)
        g_coop_pad_digital = pad_gate_block ? 0u
            : (((unsigned)data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] & 0xFFu) << 8) |
              ((unsigned)data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] & 0xFFu);
#endif

    /* Pressure-sensitive buttons (only meaningful if PRESS_ON) */
    if (setting & CELL_PAD_SETTING_PRESS_ON) {
        data->button[CELL_PAD_BTN_OFFSET_PRESS_RIGHT]    = hs->press_right;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_LEFT]     = hs->press_left;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_UP]       = hs->press_up;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_DOWN]     = hs->press_down;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE] = hs->press_triangle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CIRCLE]   = hs->press_circle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CROSS]    = hs->press_cross;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_SQUARE]   = hs->press_square;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L1]       = hs->press_l1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R1]       = hs->press_r1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L2]       = hs->trigger_l2;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R2]       = hs->trigger_r2;
        /* A button whose digital bit is set reads full pressure. Injected
         * input (PAD_FILE/PAD_SCRIPT/PAD_AUTOPRESS, keyboard) only sets the
         * digital bits, and the host values above zeroed any pressure it had
         * written -- so a title keying its menus off the pressure words (GH3,
         * Tornado Outbreak's title screen) never saw the press. */
        static const struct { u8 word; u8 bit; u8 off; } pm[] = {
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x20, CELL_PAD_BTN_OFFSET_PRESS_RIGHT },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x80, CELL_PAD_BTN_OFFSET_PRESS_LEFT },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x10, CELL_PAD_BTN_OFFSET_PRESS_UP },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x40, CELL_PAD_BTN_OFFSET_PRESS_DOWN },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x10, CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x20, CELL_PAD_BTN_OFFSET_PRESS_CIRCLE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x40, CELL_PAD_BTN_OFFSET_PRESS_CROSS },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x80, CELL_PAD_BTN_OFFSET_PRESS_SQUARE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x04, CELL_PAD_BTN_OFFSET_PRESS_L1 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x08, CELL_PAD_BTN_OFFSET_PRESS_R1 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x01, CELL_PAD_BTN_OFFSET_PRESS_L2 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x02, CELL_PAD_BTN_OFFSET_PRESS_R2 } };
        for (unsigned k = 0; k < sizeof pm / sizeof pm[0]; k++)
            if ((data->button[pm[k].word] & pm[k].bit) && data->button[pm[k].off] < 255)
                data->button[pm[k].off] = 255;
    }

    /* Sensor data (only meaningful if SENSOR_ON) */
    if (setting & CELL_PAD_SETTING_SENSOR_ON) {
        /* Default sensor values: accelerometer at rest (512 = 1g center) */
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_X] = 512;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Y] = 399; /* gravity */
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Z] = 512;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_G] = 512;
        if (port_no == 0)   /* v1042M: synthesized SIXAXIS gesture pulse (at rest otherwise) */
            pad_motion_sample(&data->button[CELL_PAD_BTN_OFFSET_SENSOR_X], &data->button[CELL_PAD_BTN_OFFSET_SENSOR_Y],
                              &data->button[CELL_PAD_BTN_OFFSET_SENSOR_Z], &data->button[CELL_PAD_BTN_OFFSET_SENSOR_G]);
    }
    }  /* end connected block */

emit:
    /* A non-zero len means "here is a CHANGE packet" -- the constant is literally
     * named CELL_PAD_LEN_CHANGE_DEFAULT. Hardware reports len = 0 once the port
     * has nothing new, and titles DRAIN on that: The Simpsons Arcade Game's
     * input stage is
     *
     *     while (cellPadGetData(port, &d) == CELL_OK && d.len > 0) { ... }
     *
     * (func_0013CFA8, loop 0x13D648 -> 0x13D040). Reporting a change on every
     * call made that loop infinite: the main thread pegged one core inside
     * libpad and the title never got past its first rendered screen.
     *
     * So hand out one packet per host report and report len = 0 for any read
     * after that until the next one arrives (pad_poll_backend sets the flag when
     * it actually re-polls, every PAD_POLL_INTERVAL_MS). That is what the
     * hardware does: a pad streams reports at a fixed rate whether or not
     * anything changed, and libpad buffers them.
     *
     * Reporting only on CHANGE instead is wrong and was tried: a HELD button
     * produces exactly one packet, so a title that reads button[] only when
     * len > 0 sees the press for a single frame and then sees nothing. In this
     * game that came out as jump being the only control that seemed to work and
     * attack firing once.
     *
     * PAD_ALWAYS_CHANGE=1 restores the old always-a-packet behaviour. */
    {
        static int always = -1;
        if (always < 0) always = getenv("PAD_ALWAYS_CHANGE") ? 1 : 0;
        if (!always && port_no < PAD_MAX_HOST_PORTS && data->len) {
            if (!s_data_fresh[port_no]) data->len = 0;   /* nothing new: end the drain */
            else                        s_data_fresh[port_no] = 0;
        }
        if (port_no < PAD_MAX_HOST_PORTS && data->len) (void)PAD_LATCH_TAKE(port_no);
        if (port_no == 0 && data->len) { s_nk_rep_btn = s_host_state[0].buttons; s_nk_rep_seq++; }   /* v1043T-3 */
    }
    {
        unsigned int ea = (unsigned int)(uintptr_t)data_guest;
        vm_write32((unsigned long long)ea + 0, (unsigned int)data->len);   /* len (s32) */
        for (int _i = 0; _i < CELL_PAD_MAX_CODES; _i++)                    /* button[] u16 */
            vm_write16((unsigned long long)ea + 4 + (unsigned int)_i * 2, data->button[_i]);
    }
    return CELL_OK;
}

/* cellPadGetInfo (v1 API, NID 0x3AAAD464) — used by PSL1GHT's ioPadGetInfo.
 * Layout (RPCS3 cellPad.h CellPadInfo, all big-endian):
 *   +0x00 u32 max_connect        +0x04 u32 now_connect   +0x08 u32 system_info
 *   +0x0C u16 vendor_id[127]     +0x10A u16 product_id[127] +0x208 u8 status[127]
 *   (CELL_MAX_PADS / PSL1GHT MAX_PADS = 127 -- NOT 7; see below)
 * Leaving this unimplemented let the guest read stack garbage as pad state and
 * run off into the weeds before ever reaching gcm init. */
s32 cellPadGetInfo(CellPadInfo2* info)
{
    uint32_t gaddr = (uint32_t)(uintptr_t)info;
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!gaddr)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    /* Present a virtual pad on port 0 when nothing physical is attached, the
     * same as cellPadGetInfo2 does.
     *
     * The two entry points disagreed: GetInfo2 carried the fallback and GetInfo
     * did not, so which one a title imports decided whether it believed a
     * controller existed. The Orange Box's chooser imports GetInfo (v1) and
     * never GetInfo2, so it saw zero pads, opened "SIXAXIS wireless controller
     * not detected", took the auto-OK, and opened it again -- forever, before
     * it ever finished booting. Both entry points describe the same hardware
     * and have to answer the same way. */
    const int hide0 = pad_port0_hidden();
    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++)
        if ((s_host_state[i].connected || i == 0) && !(i == 0 && hide0)) connected++;

    vm_write32(gaddr + 0x00, s_max_connect);
    vm_write32(gaddr + 0x04, connected);
    vm_write32(gaddr + 0x08, 0);                       /* system_info */
    /* CellPadInfo (v1) sizes its arrays by CELL_MAX_PADS = 127, not by the 7
     * ports: vendor_id[127] @0x0C, product_id[127] @0x10A, status[127] @0x208.
     * Packed for 7 ports, status landed at 0x28 and GH3 -- which reads
     * info+0x208+port -- saw port 0 disconnected and never polled the pad. */
    for (u32 i = 0; i < CELL_PAD_MAX_PORT_NUM; i++) {
        int on = (i < s_max_connect && i < PAD_MAX_HOST_PORTS &&
                  (s_host_state[i].connected || i == 0) && !(i == 0 && hide0));
        vm_write16(gaddr + 0x00C + i * 2, on ? 0x054C : 0);   /* vendor: Sony  */
        vm_write16(gaddr + 0x10A + i * 2, on ? 0x0268 : 0);   /* product: DS3  */
        /* Firmware/RPCS3 clears ASSIGN_CHANGES before copying the legacy
         * CellPadInfo status byte.  Some 2007 titles compare status for exact
         * equality with CONNECTED; returning 3 here makes a healthy pad look
         * unsupported and opens the permanent SIXAXIS reconnect overlay. */
        if (on && s_assign_change_pending[i]) s_assign_change_pending[i] = 0;
        vm_write8(gaddr + 0x208 + i, on ? (u8)CELL_PAD_STATUS_CONNECTED : 0);
    }
    return CELL_OK;
}

s32 cellPadGetInfo2(CellPadInfo2* info_guest)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Poll to get latest connection state */
    pad_poll_backend();

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadInfo2 _in; CellPadInfo2* info = &_in;
    memset(info, 0, sizeof(CellPadInfo2));
    info->max_connect = s_max_connect;

    const int hide0 = pad_port0_hidden();
    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++) {
        /* Always present a virtual pad on port 0 (standard emulator behavior) so
         * boot flows that block until a controller is connected can proceed even
         * with no physical device attached. */
        if ((s_host_state[i].connected || i == 0) && !(i == 0 && hide0)) {
            info->port_status[i]       = pad_connected_status(i);
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = pad_device_capabilities();
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED;
        }
    }
    info->now_connect = connected;

    /* copy to guest big-endian (CellPadInfo2 is all u32 fields) */
    {
        unsigned int ea = (unsigned int)(uintptr_t)info_guest;
        for (unsigned int _o = 0; _o < sizeof(CellPadInfo2); _o += 4)
            vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o));
    }
    return CELL_OK;
}

/* cellPadPeriphGetInfo -- the peripheral-class view of the pad ports (NID
 * 0x4CC9B68D). LBP polls this during boot; leaving it unresolved returned
 * CELL_OK with the caller's CellPadPeriphInfo left as uninitialised stack
 * garbage, so the game read junk port_status/pclass_type for all 7 ports.
 *
 * Layout (SDK cell/pad/pad_codes.h) -- all u32, so the whole struct marshals
 * big-endian in one sweep like cellPadGetInfo2:
 *   max_connect, now_connect, system_info,
 *   port_status[7], port_setting[7], device_capability[7],
 *   device_type[7], pclass_type[7], pclass_profile[7]
 * We report a standard DUALSHOCK-class pad (pclass_type STANDARD = 0, no
 * profile bits) mirroring the ports cellPadGetInfo2 already reports. */
s32 cellPadPeriphGetInfo(CellPadPeriphInfo* info_guest)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    CellPadPeriphInfo _in; CellPadPeriphInfo* info = &_in;
    memset(info, 0, sizeof(CellPadPeriphInfo));
    info->max_connect = s_max_connect;
    info->system_info = 0;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < CELL_PAD_MAX_PORT_NUM; i++) {
        /* Same virtual-pad-on-port-0 rule as cellPadGetInfo2, so both views of
         * the ports agree (a game that cross-checks them must see one truth). */
        if (((i < PAD_MAX_HOST_PORTS && s_host_state[i].connected) || i == 0) && !(i == 0 && pad_port0_hidden())) {
            info->port_status[i]       = pad_connected_status(i);
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = pad_device_capabilities();
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            info->pclass_type[i]       = CELL_PAD_PCLASS_TYPE_STANDARD;
            info->pclass_profile[i]    = 0;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED;
        }
    }
    info->now_connect = connected;

    { unsigned int ea = (unsigned int)(uintptr_t)info_guest;
      for (unsigned int _o = 0; _o < sizeof(CellPadPeriphInfo); _o += 4)
          vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o)); }

    { static int _n = 0;
      if ((_n < 2 && _n++ < 2))
          printf("[cellPad] PeriphGetInfo(max=%u now=%u) -> STANDARD class\n",
                 info->max_connect, connected); }
    return CELL_OK;
}

s32 cellPadSetPortSetting(u32 port_no, u32 port_setting)
{
    printf("[cellPad] SetPortSetting(port=%u, setting=0x%X)\n",
           port_no, port_setting);

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    if (!pad_sensor_enabled())
        port_setting &= ~(u32)CELL_PAD_SETTING_SENSOR_ON;
    s_port_setting[port_no] = port_setting;
    return CELL_OK;
}

/* cellPadInfoPressMode (NID 0x0E2DFAAD) / cellPadSetPressMode (NID 0xF83F8182)
 *
 * Press mode makes the digital buttons report an analog 0..255 pressure in the
 * extended part of CellPadData. A DS3 supports it, and the virtual pad reported
 * by cellPadGetInfo/GetInfo2 already advertises CELL_PAD_CAPABILITY_PRESS_MODE,
 * so these have to agree with that claim rather than fall through to the
 * unresolved-NID path.
 *
 * Info returns 1 when the port supports press mode; Set toggles it, recorded in
 * the same s_port_setting word cellPadSetPortSetting uses so a title that reads
 * the setting back sees what it asked for. */
s32 cellPadInfoPressMode(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    return 1;                                  /* supported */
}

s32 cellPadSetPressMode(u32 port_no, u32 mode)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    if (mode)
        s_port_setting[port_no] |= CELL_PAD_SETTING_PRESS_ON;
    else
        s_port_setting[port_no] &= ~(u32)CELL_PAD_SETTING_PRESS_ON;
    return CELL_OK;
}

/* cellPadInfoSensorMode (NID 0x78200559) / cellPadSetSensorMode
 * (NID 0xBE5BE3BA). Keep these consistent with the capability masks above.
 * With PAD_SENSOR=0, SetSensorMode remains a successful no-op: titles which
 * optimistically enable it keep running, but never enter a motion-input path
 * or receive extended sensor packets. */
s32 cellPadInfoSensorMode(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    return pad_sensor_enabled();
}

s32 cellPadSetSensorMode(u32 port_no, u32 mode)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    if (mode && pad_sensor_enabled())
        s_port_setting[port_no] |= CELL_PAD_SETTING_SENSOR_ON;
    else
        s_port_setting[port_no] &= ~(u32)CELL_PAD_SETTING_SENSOR_ON;
    return CELL_OK;
}

s32 cellPadGetCapabilityInfo(u32 port_no, CellPadCapabilityInfo* info)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !info)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    u32 info_ea = (u32)(uintptr_t)info;
    for (u32 i = 0; i < CELL_PAD_MAX_CODES; i++)
        vm_write32(info_ea + i * 4, 0);

    vm_write32(info_ea, pad_device_capabilities());

    return CELL_OK;
}

s32 cellPadSetActDirect(u32 port_no, CellPadActParam* param)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !param)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    param = GUEST_PTR(param, CellPadActParam*);
#if defined(PS3RECOMP_SDL3) && PAD_BACKEND_XINPUT
    if (!pad_rumble_enabled()) return CELL_OK;   /* PAD_RUMBLE=0 */
    if (port_no < PAD_MAX_HOST_PORTS && s_sdl_slot[port_no] >= 0) {   /* a PlayStation / Steam controller */
        pad_sdl3_rumble(s_sdl_slot[port_no], param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL], param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE]);
        return CELL_OK;
    }
#endif

#if PAD_BACKEND_XINPUT
    /* Map to XInput vibration */
    if (port_no < PAD_MAX_HOST_PORTS && s_host_state[port_no].connected) {
        XINPUT_VIBRATION vib;
        vib.wLeftMotorSpeed  = (WORD)(param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] * 257);
        vib.wRightMotorSpeed = (WORD)(param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] * 257);
        XInputSetState((DWORD)port_no, &vib);
    }
#endif

#if PAD_BACKEND_SDL2
    if (port_no < PAD_MAX_HOST_PORTS && s_sdl_controllers[port_no]) {
        SDL_GameControllerRumble(
            s_sdl_controllers[port_no],
            (Uint16)(param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] * 257),
            (Uint16)(param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] * 257),
            100 /* duration ms */
        );
    }
#endif

    return CELL_OK;
}

s32 cellPadClearBuf(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Nothing to clear in our implementation -- state is polled fresh */
    return CELL_OK;
}

#ifndef _WIN32
void ps3_pad_mouse_inject(u32 ctrl) { (void)ctrl; }   /* v151: KBM is Windows-only */
void ps3_pad_rstick_read(u32 site, u32 which, u32 kind) { (void)site; (void)which; (void)kind; }   /* v236 */
void ps3_pad_mouse_cam_spring(u32 cam) { (void)cam; }
int  ps3_pad_mouse_raw_turn(void) { return 0; }
#endif
