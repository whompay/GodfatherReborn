/* gf_cutscene.c -- v1042S: is one of Godfather's in-engine cutscenes (IGCs) playing right now?
 *                  v1042V: real skip -- jump the cutscene to its end instead of fast-forwarding through it.
 *
 * Story cutscenes are mission entities of class SingleEventHandRolledIgc / MultiEventHandRolledIgc (registered by
 * 0x1D884 / 0x1D8A4 with factories 0x21C0C / 0x21C5C; vtables 0x10042B18 / 0x10042B90). They are switched on and
 * off through the shared entity activator func_00080A4C(this, on), which moves the entity state at +0x1C
 * (0 idle, 2 activation requested, 1 active). The IGC's message handler (vtable +0x18, func_0008A82C) sets bit
 * 0x08000000 of +0x7C when its start message arrives and clears it on its stop / reset messages.
 *
 * ppu_recomp_000.cpp calls gf_igc_note() at the top of func_00080A4C; every IGC object seen there is tracked, and
 * gf_cutscene_active() reports a cutscene while any tracked IGC has state != 0 and the playing bit set. cellPad.c
 * uses it (g_pad_cutscene_fn) for hold-to-skip. State changes are logged as [cutscene] (stdout) so a play log
 * shows whether the detection matches what is on screen. GF_CUTSCENE_LOOSE=1: state != 0 alone counts.
 *
 * ---- v1042V: how the skip actually ends the cutscene -------------------------------------------------------------
 * The streamed story IGCs (igc_actNN_*.str: TRINITY_SEQ_MASTER + trSeq_<shot> + play_/finish_/audio_igc_ events)
 * are "SequenceInstance" entities (factory 0x21AB0, ctor 0x28A6E4, vtable 0x1004B878). Layout used here:
 *   +0xD0 shot vector begin, +0xD4 shot count (12-byte elements, word 0 = shot def, shot def +0x8 = sub-sequence
 *   count), +0xDC current element, +0x108 current sub-sequence index, +0xF8 64-bit state word (bit 63 = running),
 *   +0x130 bit 0 = "all shots done, finalize on the next iMsgRunningTick".
 * Each sub-sequence is a Trinity playback in one of 16 slots (0x10261410, stride 0xF0, +0x40 bit 63 = active,
 * +0x48 script IP, +0x4C elapsed, +0xE4 owner link = owner+0x38). The per-frame Trinity update func_005F0DF8(dt)
 * advances every slot; a finished playback reaches the owner's vtable +0x34 (func_0028ADE0, "sub-sequence done").
 * func_0028ADE0(this, 0) moves to the next sub-sequence, then to the next shot (+0xDC += 12) and, past the last shot, sets
 * +0x130 bit 0 -- on the next tick func_0028C5C0 dispatches finish_igc_<name>, which is what the mission waits for.
 *
 * The skip rides that exact path, on the game thread, with three small hooks:
 *   gf_trinity_dt()       top of func_005F0DF8 (ppu_recomp_002.cpp): while skipping, the Trinity playbacks get a
 *                         large dt (GF_SKIP_TRINITY_DT seconds per frame, default 2) so the current sub-sequence
 *                         runs its remaining script (events included) and completes within a frame or two.
 *   gf_seq_advance()      top of func_0028ADE0 (ppu_recomp_001.cpp): the finished sub-sequence ends its shot.
 *   gf_seq_jump_to_end()  in func_0028ADE0 right after +0xDC moves to the next shot (ppu_recomp_001.cpp): the cursor
 *                         jumps past the last shot, so the remaining shots are never streamed or played and the
 *                         natural "last shot finished" code runs (finish_igc_* dispatch, camera/director release).
 * The old hidden fast-forward (g_gf_ffwd: no frame limiter, game dt 83 ms, muted audio) still runs underneath as
 * a fallback for anything that is not a SequenceInstance (hand-rolled IGCs); the screen is black meanwhile.
 * GF_SKIP_JUMP=0 disables the three hooks (old fast-forward only). Log: [skip-seq].
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

extern uint8_t* vm_base;
extern int (*volatile g_pad_cutscene_fn)(void);
extern volatile int g_gf_ffwd;            /* cellPad.c: skip in progress */
extern int ps3_fs_igc_active(void);
extern void ps3_fs_igc_forget(void);   /* v1042V ppu_fs.cpp: stop counting the IGC dialogue files open now */
extern void ps3_fs_igc_end_audio(void);   /* v1042Z ppu_fs.cpp: end the cutscene's audio track (GF_SKIP_AUDIO_END) */

#define IGC_VT_SINGLE 0x10042B18u
#define IGC_VT_MULTI  0x10042B90u
#define SEQ_VT        0x1004B878u         /* SequenceInstance (streamed story IGC) */
#define TRI_SLOTS     0x10261410u         /* Trinity playback slots */
#define TRI_SLOT_SZ   0xF0u
#define TRI_NSLOTS    16
#define GFC_MAX 64

static uint32_t rd32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t rd64(uint32_t ea) { return ((uint64_t)rd32(ea) << 32) | rd32(ea + 4); }
static void wr32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static float rdf(uint32_t ea) { uint32_t b = rd32(ea); float f; memcpy(&f, &b, 4); return f; }
static int ea_ok(uint32_t o) { return o >= 0x10000u && o < 0xF0000000u; }

/* ---- v1042V skip state ---- */
static int s_jump_on = -1;                 /* GF_SKIP_JUMP (default 1) */
static float s_tri_dt = 2.0f;              /* GF_SKIP_TRINITY_DT (default 2 s per frame; 0 = leave dt alone) */
static int s_logn = 0;
static volatile unsigned long long s_jump_ms = 0;   /* when a skip jumped a SequenceInstance past its last shot */
static unsigned long long now_ms(void)
{
    struct timespec ts; timespec_get(&ts, TIME_UTC);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}
static void skip_cfg(void)
{
    if (s_jump_on >= 0) return;
    const char* e = getenv("GF_SKIP_JUMP");
    s_jump_on = !(e && *e == '0');
    e = getenv("GF_SKIP_TRINITY_DT");
    if (e && *e) s_tri_dt = (float)atof(e);
    if (s_tri_dt < 0.0f) s_tri_dt = 0.0f;
    if (s_tri_dt > 60.0f) s_tri_dt = 60.0f;
}
#define SKIPLOG(...) do { if (s_logn < 600) { s_logn++; printf(__VA_ARGS__); fflush(stdout); } } while (0)
static int skipping(void) { skip_cfg(); return s_jump_on && g_gf_ffwd; }
static int is_seq(uint32_t o) { return ea_ok(o) && rd32(o) == SEQ_VT; }

/* Top of func_0028ADE0(this, stop, f1). With the instance running (+0xF8 bit 63) and stop == 0 it advances to the
 * next sub-sequence (stop != 0 goes to vtable +0x30, the stop path, instead). While skipping, make the finished
 * sub-sequence the last one of its shot, so this call moves on to the next shot (gf_seq_jump_to_end takes over). */
/* v1049K GF_SKIP_ALLSHOTS (default 1; 0 = v1042V jump): the jump past the remaining shots also skipped their event
 * scripts. A cutscene that switches ambient dialogue / the HUD minimap off at its start and back on in a later shot
 * left them off for good (user 2026-11-02: after skipping igc_act01_t03_03 -- 6 of 7 shots jumped -- the game never
 * opened another dialogue stream, story NPC conversations "finished instantly", minimap blank). Now every shot and
 * sub-sequence still runs, each finishing in a frame or two under the Trinity dt boost (GF_SKIP_TRINITY_DT), behind
 * the black skip screen; the audio-end trick runs when the cutscene really reaches its end. */
static int s_allshots = -1;
static unsigned long long s_prog_ms;   /* last fast-shot step (stall diagnostics) */
static int allshots(void)
{
    if (s_allshots < 0) { const char* e = getenv("GF_SKIP_ALLSHOTS");
        /* (A language pack once defaulted to the direct jump here; that skipped the event scripts and froze the
         * mission after the mobface screen, so the fast replay stays the default everywhere.) */
        extern const char* ps3_fs_lang_pack_dir(void);
        (void)ps3_fs_lang_pack_dir; s_allshots = !(e && *e == '0');
        printf("[skip-seq] v1049K skip plays every shot fast (all events fire): %s (GF_SKIP_ALLSHOTS)\n", s_allshots ? "ON" : "off"); fflush(stdout); }
    return s_allshots;
}
void gf_seq_advance(uint64_t self, uint64_t stop)
{
    if (!skipping()) return;
    const uint32_t o = (uint32_t)self;
    if (!is_seq(o) || (uint32_t)stop) return;
    if ((int64_t)rd64(o + 0xF8) >= 0) return;               /* not running: func_0028ADE0 will not advance */
    const uint32_t begin = rd32(o + 0xD0), cnt = rd32(o + 0xD4), cur = rd32(o + 0xDC);
    const uint32_t end = begin + cnt * 12u;
    if (!ea_ok(begin) || cnt > 256 || cur < begin || cur >= end) return;
    const uint32_t shot = rd32(cur);
    if (!ea_ok(shot)) return;
    const uint32_t n = rd32(shot + 8), idx = rd32(o + 0x108);
    SKIPLOG("[skip-seq] v1042V seq 0x%08X sub-sequence done: shot %u/%u, sub %u/%u\n",
            o, (cur - begin) / 12u + 1u, cnt, idx + 1u, n);
    if (allshots()) return;   /* v1049K: the next sub-sequence runs too (fast) */
    if (n && n < 4096 && idx + 1u < n) {
        wr32(o + 0x108, n - 1u);
        SKIPLOG("[skip-seq] v1042V seq 0x%08X dropping %u remaining sub-sequence(s) of this shot\n", o, n - 1u - idx);
    }
}

/* func_0028ADE0, right after +0xDC moved to the next shot element. Returns 1 when it moved the cursor past the
 * last shot; the caller then reloads +0xDC and the game's own end-of-sequence path runs. */
int gf_seq_jump_to_end(uint64_t self)
{
    if (!skipping()) return 0;
    const uint32_t o = (uint32_t)self;
    if (!is_seq(o)) return 0;
    const uint32_t begin = rd32(o + 0xD0), cnt = rd32(o + 0xD4), cur = rd32(o + 0xDC);
    const uint32_t end = begin + cnt * 12u;
    if (allshots()) {   /* v1049K: no jump; end the audio once the sequence itself passes its last shot */
        if (ea_ok(begin) && cnt <= 256 && cur >= end) {
            s_jump_ms = now_ms();
            ps3_fs_igc_end_audio();
            SKIPLOG("[skip-seq] v1049K seq 0x%08X played all %u shot(s) fast -- ending\n", o, cnt);
        } else if (ea_ok(begin) && cnt <= 256 && cur >= begin) {
            s_prog_ms = now_ms();
            SKIPLOG("[skip-seq] v1049K seq 0x%08X fast shot %u/%u\n", o, (cur - begin) / 12u + 1u, cnt); }
        return 0;
    }
    if (!ea_ok(begin) || cnt > 256 || cur < begin || cur >= end) return 0;   /* already past the last shot */
    wr32(o + 0xDC, end);
    s_jump_ms = now_ms();
    ps3_fs_igc_end_audio();   /* v1042Z: the skipped cutscene's dialogue/music stops with it */
    SKIPLOG("[skip-seq] v1042V seq 0x%08X jumped to the end: %u of %u shot(s) not played\n", o, (end - cur) / 12u, cnt);
    return 1;
}

/* Top of func_005F0DF8(dt): the per-frame update of the 16 Trinity playback slots. */
double gf_trinity_dt(double dt)
{
    if (!skipping()) return dt;
    static int n = 0;
    static unsigned long long stall_t = 0; static int stall_logs = 0;   /* a fast replay that stops moving: log what is left */
    const unsigned long long nowm = now_ms();
    if (!s_prog_ms || nowm - s_prog_ms < 4000ull) stall_t = 0;
    else if (stall_logs < 12 && nowm - stall_t >= 2000ull) { stall_t = nowm; stall_logs++; n = 39;
        SKIPLOG("[skip-seq] fast replay stalled %.1f s -- playback slots:\n", (nowm - s_prog_ms) / 1000.0); }
    if (n < 40) {   /* what the skip is chewing through (first frames only) */
        n++;
        for (int i = 0; i < TRI_NSLOTS; i++) {
            const uint32_t s = TRI_SLOTS + (uint32_t)i * TRI_SLOT_SZ;
            const uint64_t st = rd64(s + 0x40);
            if ((int64_t)st >= 0) continue;
            const uint32_t link = rd32(s + 0xE4), owner = link > 0x38u ? link - 0x38u : 0u;
            SKIPLOG("[skip-seq] v1042V trinity slot %d state %016llX ip %08X t %.2f s owner 0x%08X%s\n", i,
                    (unsigned long long)st, rd32(s + 0x48), rdf(s + 0x4C), owner,
                    owner && ea_ok(owner) && rd32(owner) == SEQ_VT ? " (SequenceInstance)" : "");
        }
    }
    if (s_tri_dt > 0.0f && dt < (double)s_tri_dt) return (double)s_tri_dt;
    return dt;
}

/* Trace the retail named events, including their real subscriber objects.
 * Registration stores a handle whose event pointer is filled by the guest. */
static struct { uint32_t handle; char name[128]; } s_events[256];
static unsigned s_events_n;
extern int (*volatile g_gf_rain_poll)(void);   /* v1118B, below */
static int gf_rain_poll(void);
void gf_igc_register_event(uint64_t handle, uint64_t name)
{
    if (!g_gf_rain_poll) g_gf_rain_poll = gf_rain_poll;   /* v1118B */
    const char *n = (const char *)(vm_base + (uint32_t)name);
    if (strncmp(n, "play_igc_", 9) && strncmp(n, "finish_igc_", 11)) return;
    unsigned i;
    for (i = 0; i < s_events_n; ++i) if (s_events[i].handle == (uint32_t)handle) break;
    if (i == 256) return;
    if (i == s_events_n) ++s_events_n;
    s_events[i].handle = (uint32_t)handle;
    snprintf(s_events[i].name, sizeof(s_events[i].name), "%s", n);
    printf("[igc-event] register %s handle=%08X\n", n, (uint32_t)handle);
}

static volatile uint32_t s_obj[GFC_MAX];
static volatile int s_nobj = 0;
static int s_loose = -1;

/* v1118B weather: the rain messages (gf_coop_msgnames.h handles) and the rain scheduler singleton (constructor
 * func_00278C18 stores it at 0x101DB82C; func_00279038 / func_00279138 set flag 0x4 of the u16 at +0x74 when they send
 * iMsgStartRainFadeIn, func_002790B0 clears it with iMsgStartRainFadeOut). rsx_post.h (Wet Streets) polls
 * g_gf_rain_poll each frame: 1 = raining. */
extern int (*volatile g_gf_rain_poll)(void);
static volatile int s_rain_msg = -1;   /* -1 = no message seen yet */
static int gf_rain_poll(void)
{
    static int force = -2; if (force == -2) { const char* e = getenv("GF_RAIN_FORCE"); force = e && *e ? atoi(e) : -1; }
    if (force >= 0) return force;
    const uint32_t o = rd32(0x101DB82Cu);
    const int sched = (o >= 0x10000u && o < 0xF0000000u) ? (int)((rd32(o + 0x74) >> 16) & 4u) != 0 : 0;
    static int last = -1; const int r = s_rain_msg >= 0 ? s_rain_msg : sched;
    if (r != last) { printf("[weather] v1118B rain %s (scheduler 0x%08X flag %d, message %d)\n", r ? "ON" : "off", o, sched, s_rain_msg); fflush(stdout); last = r; }
    return r;
}
static void gf_weather_event(uint32_t message, uint32_t event)
{
    static const uint32_t h[4] = { 0x101DB80Cu, 0x101DB7FCu, 0x101DB804u, 0x101DB7F4u };   /* FadeIn, FadeInTOD, FadeOut, FadeOutTOD */
    static const char* const nm[4] = { "FadeIn", "FadeInTOD", "FadeOut", "FadeOutTOD" };
    for (int i = 0; i < 4; i++) {
        if (rd32(h[i]) != event) continue;
        const uint32_t pp = rd32(message + 4); const float prm = (pp >= 0x10000u && pp < 0xF0000000u) ? rdf(pp) : 0.0f;
        s_rain_msg = i < 2;
        printf("[weather] v1118B iMsgStartRain%s (%.2f)\n", nm[i], prm); fflush(stdout);
        break;
    }
}
void gf_igc_dispatch_event(uint64_t message)
{
    const uint32_t event = rd32((uint32_t)message);
    if (event) { if (!g_gf_rain_poll) g_gf_rain_poll = gf_rain_poll; gf_weather_event((uint32_t)message, event); }
    for (unsigned i = 0; i < s_events_n; ++i) {
        if (!event || rd32(s_events[i].handle) != event) continue;
        printf("[igc-event] dispatch %s event=%08X\n", s_events[i].name, event);
        if (g_gf_ffwd && !strncmp(s_events[i].name, "finish_igc_", 11)) {
            /* v1042V: the skipped cutscene handed control back. Its dialogue stream may still be open for a moment;
             * do not let that keep the skip (black screen, no frame limiter) running into gameplay. */
            ps3_fs_igc_forget();
            SKIPLOG("[skip-seq] v1042V %s dispatched -- cutscene finished, returning to the game\n", s_events[i].name);
        }
        uint32_t node = rd32(event);
        for (unsigned j = 0; node && j < 64; ++j) {
            uint32_t obj = rd32(node + 8);
            if (obj) printf("[igc-event] subscriber obj=%08X vt=%08X\n", obj, rd32(obj));
            node = rd32(node);
        }
        break;
    }
}
static int is_igc(uint32_t o)
{
    if (o < 0x10000u || o >= 0xF0000000u) return 0;
    const uint32_t vt = rd32(o);
    return vt == IGC_VT_SINGLE || vt == IGC_VT_MULTI;
}

void gf_igc_note(uint64_t obj, uint64_t on)
{
    const uint32_t o = (uint32_t)obj;
    if (!is_igc(o)) return;
    int i;
    for (i = 0; i < s_nobj; i++) if (s_obj[i] == o) break;
    if (i == s_nobj && s_nobj < GFC_MAX) { s_obj[s_nobj] = o; s_nobj = s_nobj + 1; }
    static int n = 0;
    if (n < 400) {
        n++;
        printf("[cutscene] v1042S IGC 0x%08X %s activate(%d): state %u flags 0x%08X\n", o,
               rd32(o) == IGC_VT_SINGLE ? "single" : "multi", (int)on, rd32(o + 0x1C), rd32(o + 0x7C));
        fflush(stdout);
    }
}

int gf_cutscene_active(void)
{
    if (s_loose < 0) { const char* e = getenv("GF_CUTSCENE_LOOSE"); s_loose = e && *e == '1'; }
    static int last = 0;
    static uint32_t last_obj = 0;
    int act = 0; uint32_t who = 0;
    /* Primary signal (v1042S fix): the user's first test showed story IGCs never pass through func_00080A4C with an
     * IGC vtable, but every one streams its own dialogue track (audiostreams/<dir>/igc_<name>.exa.snu) for exactly
     * as long as it plays. Reads continue normally during a skip, so this remains the authoritative end signal.
     * Do not keep a skip alive from the shot-age heuristic: that used to run the accelerated clock for another
     * 2.5 seconds after the cutscene and could visibly fast-forward the gameplay that followed.
     * v1042V: after a skip ends the cutscene, the dialogue files open at that moment no longer count. */
    int stream = ps3_fs_igc_active();
    /* v1042V: the jump hands the sequence to the game's end-of-cutscene code, which finalizes it on the next tick.
     * Not every cutscene's finish_igc_* event is registered by name (only some are seen by gf_igc_register_event),
     * and the IGC dialogue file stays open and polled afterwards (first test: gameplay chatter was already playing
     * while the skip screen stayed up for good). So 500 ms after the jump, stop treating that stream as a cutscene. */
    if (s_jump_ms && now_ms() - s_jump_ms >= 500ull) {
        s_jump_ms = 0;
        ps3_fs_igc_forget();
        SKIPLOG("[skip-seq] v1042V sequence handed back to the game; ending the skip\n");
        stream = ps3_fs_igc_active();
    }
    if (stream) { act = 1; who = 0xF5F5F5F5u; }
    for (int i = 0; !act && i < s_nobj; i++) {
        const uint32_t o = s_obj[i];
        if (!is_igc(o)) continue;                 /* freed / reused memory */
        const uint32_t st = rd32(o + 0x1C), fl = rd32(o + 0x7C);
        if (st != 0 && (s_loose || (fl & 0x08000000u))) { act = 1; who = o; break; }
    }
    if (act != last || (act && who != last_obj)) {
        static int n = 0;
        if (n < 400) { n++; printf("[cutscene] v1042S %s (%s)\n", act ? "cutscene playing" : "cutscene over", (act ? who : last_obj) == 0xF5F5F5F5u ? "igc dialogue stream" : "IGC entity"); fflush(stdout); }
        last = act; last_obj = who;
    }
    return act;
}

void gf_combat_init(void);   /* v1042W gf_combat.c: PC keyboard/mouse combat layout */
void gf_trainer_init(void);  /* v1045A gf_trainer.c: F10 trainer */
void gf_cutscene_init(void)
{
    gf_trainer_init();
    gf_combat_init();
    g_pad_cutscene_fn = gf_cutscene_active;
    skip_cfg();
    printf("[cutscene] v1042V IGC tracking ON (hold Cross / Space to skip); jump-to-end %s, Trinity skip dt %.2f s\n",
           s_jump_on ? "ON" : "OFF (GF_SKIP_JUMP=0: fast-forward only)", (double)s_tri_dt);
    fflush(stdout);
}
