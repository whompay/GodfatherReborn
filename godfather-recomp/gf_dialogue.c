/* gf_dialogue.c -- v1106D (Claude, 2026-11-07): street dialogue. User: "dialogue choices answered with fitting in-game voice lines".
 *
 * T next to a living NPC (or the one under the free-aim crosshair) opens a small menu in the game's look (rsx_ui_overlay.c
 * gfd_build): 1 SAY HELLO, 2 WHAT'S THE WORD?, 3 INTIMIDATE, 4 LEAVE. The player gestures / speaks, the NPC answers with a
 * voice line from the disc, and the game's own English subtitle is shown. After the answer the menu comes back until LEAVE,
 * T / Backspace, walking away (> 7 m), a car, a cutscene or the NPC dying.
 *
 * Lines (RE 2026-11-07, see HANDOFF v1106D): audiostreams/<dir>/dx_<type>_<speaker>_<cat>_<n>.exa.snu, 10,585 files. Their
 * subtitles sit in gf_global.en.LH2 (godfather_v4/text/310D3A42.str, RefPack chunk): 8,337 strings, keys sorted at +0xB0,
 * offsets (from +0x90) after them; key = gf_mission_hash(stream base name). 2,133 streams have one -- only those are used,
 * picked by text in tools/gf_dialogue_build.py -> assets/gf_dialogue.txt (category, speaker, path, subtitle):
 *   HELLO / GOSSIP / HURT (player health < 45 %): chat_repu of the generic street voices imgx (men) / ifgx (women) -- only lines
 *     that fit at any point of the story (the rank / mission lines of ma07 / ma10 need state we do not read yet);
 *   PL_THREAT / PL_THREAT_GUN (a gun drawn) / PL_BYE: the player's scrp_pres + chat_cmbt lines;
 *   SCARED / DEFY / GUN / GIVEUP: chat_prss (the shopkeepers' "pressed" replies) of ma01..ma20 / fm04..fm08. Each NPC keeps one
 *     of those voices (hash of the entity). The 3rd intimidation of the same NPC -> GIVEUP.
 * Audio: EA SNU header (+0x08 data offset, +0x10 codec 4 = EA-XAS v1, +0x12 rate, +0x14 samples), blocks u8 flags (0x80 last) +
 * u24 size + u32 samples, then 76-byte frames of 128 samples: 4 little-endian headers (coef index bits 0-3, shift bits 16-19,
 * two start samples in the high 12 bits of each half), then 15 x 4 bytes of nibbles (byte 0x10 + 4 j + subframe, HIGH nibble
 * first), sample = (n << 12 >> shift) + (c1 h1 + c2 h2 + 128) >> 8, coefs {0,0},{240,0},{460,-208},{392,-220}. Verified on the
 * disc: the sub-block start samples continue the decoded signal (boundary jump / inner step 1.02). Decoded here, resampled to
 * 48 kHz and mixed into the device buffer by cellAudio.c g_audio_overlay_mix (game audio ducked to 60 % while a line plays).
 * Gestures: the game's own clips on upper-body layer 0 (same call as gf_smoke.c): lw_common_respect_tiphat,
 * f2f_player_right_hand_gesture, f2f_player_brandish_fist / thump_fist_chest; NPC lw_innocent_{male,female}_respect,
 * lw_common_respect_nod, f2f_npc_hands_out_afraid, f2f_npc_talk_fear, lw_innocent_{male,female}_disrespect, lw_innocent_surrender.
 * Gender: byte npc+0x1817 (2 = man, 3 = woman; seen with the voice banks of the vocal logger).
 *
 * Main PPU thread: gf_dialogue_tick from gf_trainer.c main_tick. Env: GF_DIALOGUE=0 off, GF_TALK_KEY=<letter> (E; v1107C),
 * GF_DIALOGUE_VOL (1.0), GF_DIALOGUE_DUCK (0.6), GF_DIALOGUE_TEST=<s> (soak: open on the nearest NPC s seconds after the
 * player exists and choose 1,2,3,3,3,4 in turn). Log [dialogue]. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "recompiled/ppu_recomp.h"

extern uint8_t* vm_base;
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern int gf_cutscene_active(void);
extern uint32_t gf_aim_target_now(void);
extern uint32_t gf_nearest_npc(float maxd);
extern int gf_rag_bone_index(uint32_t ent, uint32_t anim, int b);
extern int gf_npc_name(uint32_t ent, char* out, int cap);   /* gf_ragdoll.c v1106D */
extern uint32_t gf_mission_hash(const char* s);
extern int (*volatile g_pad_weapon_fn)(void);
extern int (*volatile g_pad_drawn_fn)(void);
extern void (*volatile g_audio_overlay_mix)(float* stereo, unsigned frames);
extern const char* ppu_vfs_root;
extern void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
typedef void (*gfn)(ppu_context*);
#define TOC 0x00B4BF60u

static int ok(uint32_t ea) { return ea >= 0x10000u && ea < 0xF0000000u; }
static uint32_t rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static float rdf(uint32_t ea) { uint32_t u = rd32(ea); float f; memcpy(&f, &u, 4); return f; }
static int fok(float f) { return f == f && f > -1e7f && f < 1e7f; }
static double now_s(void) { return (double)GetTickCount64() / 1000.0; }
static int s_nlog;
#define DLOG(...) do { if (s_nlog < 3000) { s_nlog++; printf(__VA_ARGS__); fflush(stdout); } } while (0)

/* ------------------------------------------------------------------ shared with rsx_ui_overlay.c (render thread reads) */
volatile int g_gfd_menu = 0, g_gfd_sel = 0, g_gfd_nopt = 0, g_gfd_clear = 0;
volatile unsigned long long g_gfd_close_t = 0, g_gfd_sub_t1 = 0, g_gfd_prompt_t = 0;
char g_gfd_opt[5][40], g_gfd_who[48], g_gfd_sub_who[48], g_gfd_sub[200];
volatile int g_gfd_key = 'E';
volatile int g_gfd_lock = 0;   /* v1107C: the pad stays with the conversation (rsx_ui_overlay.c uiov_pad_gate) */
extern volatile int g_gfd_cam_on; extern float g_gfd_cam_eye[3], g_gfd_cam_tgt[3];   /* gf_footcam.c v1107C */
extern int gf_rag_ent_frame(uint32_t ent, float* M9, float* T3);

/* ------------------------------------------------------------------ helpers */
static uint32_t player(void)
{
    __try { uint32_t a = rd32(0x10181290u); if (!ok(a)) return 0; a = rd32(a + 4); if (!ok(a)) return 0; a = rd32(a);
        return (ok(a) && a >= 0x17FCu) ? a - 0x17FCu : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int ent_pos(uint32_t e, float* p)
{
    const uint32_t f = rd32(e + 0x68u); if (!ok(f)) return 0;
    p[0] = rdf(f + 0x40); p[1] = rdf(f + 0x44); p[2] = rdf(f + 0x48);
    return fok(p[0]) && fok(p[1]) && fok(p[2]);
}
static float ent_dist(uint32_t a, uint32_t b) { float p[3], q[3]; if (!ent_pos(a, p) || !ent_pos(b, q)) return 1e9f;
    const float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2]; return sqrtf(dx * dx + dy * dy + dz * dz); }
static int alive(uint32_t e) { const float h = rdf(e + 0x14C8u); return fok(h) && h > 0.0f; }
static int in_car(uint32_t pl) { const uint32_t car = rd32(pl + 0x1718u); return ok(car) && car == rd32(pl + 0x4Cu) && (rd32(pl + 0x5C4u) & 0x8u); }
static int is_female(uint32_t e) { return (rd32(e + 0x1814u) & 1u) != 0; }   /* v1107G: the name generator's flag (func_0012D5BC: rldicl r5,(+0x1814),0,63) */
static int is_story(uint32_t e) { return rd32(e + 0x1830u) != 0; }   /* v1107G: story / mission character (name from a text key, func_0012D5BC) */
static int fg_ours(void) { HWND w = GetForegroundWindow(); DWORD pid = 0; if (w) GetWindowThreadProcessId(w, &pid); return pid == GetCurrentProcessId(); }
static int armed(void) { return g_pad_weapon_fn && g_pad_drawn_fn && g_pad_weapon_fn() == 1 && g_pad_drawn_fn() == 1; }
static gfn lookup(uint32_t addr)
{
    for (uint64_t k = 0; k < function_table_count; k++) if ((uint32_t)function_table[k].addr == addr) return function_table[k].func;
    return 0;
}
static int gcall(gfn f, const uint64_t* a, int n, double f1, uint64_t* ret)
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
        g_active_ctx = cur; if (ret) *ret = c.gpr[3]; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_active_ctx = cur; g_trampoline_fn = 0; return 0; }
}
/* the game's clip on upper-body layer 0 (gf_smoke.c v1079S-3/4: func_0046194C blend, func_00460E0C play, lumbar mask) */
static int play_gesture_m(uint32_t ent, const char* clip, int whole);
static int play_gesture(uint32_t ent, const char* clip) { return play_gesture_m(ent, clip, 0); }
static void stop_layer(uint32_t ent)   /* func_004610F4(ent, layer): fade the layer out (gf_smoke.c) */
{ static gfn f; if (!f) f = lookup(0x004610F4u); if (!f || !ok(ent)) return; const uint64_t a[2] = { ent, 0 }; gcall(f, a, 2, 0.0, NULL); }
static uint32_t layer_hash(uint32_t ent)   /* clip on layer 0 now, FFFFFFFF = none */
{ const uint32_t anim = rd32(ent + 0xC4u); if (!ok(anim)) return 0; const uint32_t L = rd32(anim + 0x148u); return ok(L) ? rd32(L + 0x10u) : 0; }
static int play_gesture_m(uint32_t ent, const char* clip, int whole)
{
    static gfn f_blend, f_play; if (!f_play) { f_blend = lookup(0x0046194Cu); f_play = lookup(0x00460E0Cu); }
    if (!f_play || !ok(ent)) return 0;
    const uint32_t anim = rd32(ent + 0xC4u); if (!ok(anim)) return 0;
    const uint32_t h = gf_mission_hash(clip);
    const int mask = whole ? -1 : gf_rag_bone_index(ent, anim, 1);   /* v1107W: whole body = the legs stand too */
    const uint64_t b[2] = { ent, 0 }; gcall(f_blend, b, 2, 0.45, NULL);   /* v1107W: softer cross-fade (was 0.25) */
    const uint64_t a[8] = { ent, h, 0, (uint64_t)(int64_t)mask, 1, 1, 0, 1 }; uint64_t r = 0;
    const int okc = gcall(f_play, a, 8, 0.45, &r);
    DLOG("[dialogue] gesture %s on %08X: %s, returned %d\n", clip, ent, okc ? "ok" : "call failed", (int)(uint32_t)r);
    return okc && r;
}

/* ------------------------------------------------------------------ line table (assets/gf_dialogue.txt) */
enum { C_HELLO_M, C_HELLO_F, C_HURT_M, C_HURT_F, C_GOSSIP_M, C_GOSSIP_F, C_PL_THREAT, C_PL_THREAT_GUN, C_PL_BYE,
       C_SCARED_M, C_SCARED_F, C_DEFY_M, C_DEFY_F, C_GUN_M, C_GUN_F, C_GIVEUP_M, C_GIVEUP_F, C_PL_HELLO, C_PL_GOSSIP, C_PL_FLIRT, C_FLIRT_F, C_N };
static const char* const k_cat[C_N] = { "HELLO_M", "HELLO_F", "HURT_M", "HURT_F", "GOSSIP_M", "GOSSIP_F", "PL_THREAT", "PL_THREAT_GUN", "PL_BYE",
       "SCARED_M", "SCARED_F", "DEFY_M", "DEFY_F", "GUN_M", "GUN_F", "GIVEUP_M", "GIVEUP_F", "PL_HELLO", "PL_GOSSIP", "PL_FLIRT", "FLIRT_F" };   /* v1107L: player lines the user transcribed / tagged */
typedef struct { int cat; char spk[8]; char path[96]; char text[200]; } Line;
static Line* s_ln; static int s_nln;
static const char* const k_pr_m[] = { "ma01","ma02","ma03","ma04","ma05","ma06","ma07","ma08","ma11","ma12","ma13","ma15","ma20" };
static const char* const k_pr_f[] = { "fm04","fm06","fm08" };
extern const char* ui_tr(const char* en);           /* ui_i18n.c */
extern const char* ps3_fs_lang_pack_dir(void);   /* sys_fs.c: language pack (langpack/<code>), NULL = base language */
static int load_lines(void)
{
    char exe[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH); FILE* f = NULL; char path[MAX_PATH + 64];
    { const char* lp = ps3_fs_lang_pack_dir();   /* a language pack carries its own lines (that language's voices + subtitles) */
      if (lp) { snprintf(path, sizeof path, "%s\\gf_dialogue.txt", lp); f = fopen(path, "rb");
                if (f) DLOG("[dialogue] lines from the language pack: %s\n", path); } }
    if (n && n < MAX_PATH) { char* s = strrchr(exe, '\\'); if (s) *s = 0;
        static const char* rel[] = { "\\assets\\gf_dialogue.txt", "\\..\\assets\\gf_dialogue.txt", "\\..\\..\\assets\\gf_dialogue.txt" };
        for (int i = 0; i < 3 && !f; i++) { snprintf(path, sizeof path, "%s%s", exe, rel[i]); f = fopen(path, "rb"); } }
    if (!f) { DLOG("[dialogue] v1106D no assets\\gf_dialogue.txt (tools/gf_dialogue_build.py) -> dialogue off\n"); return 0; }
    int cap = 1024; s_ln = (Line*)calloc((size_t)cap, sizeof(Line)); char l[600];
    while (fgets(l, sizeof l, f)) {
        if (l[0] == '#') continue;
        char* t[4]; int k = 0; char* p = l; t[k++] = p;
        while (*p && k < 4) { if (*p == '\t') { *p = 0; t[k++] = p + 1; } p++; }
        if (k < 4) continue;
        p = t[3]; while (*p && *p != '\r' && *p != '\n') p++; *p = 0;
        int c = -1; for (int i = 0; i < C_N; i++) if (!strcmp(t[0], k_cat[i])) c = i;
        if (c < 0) continue;
        if (s_nln >= cap) { cap *= 2; s_ln = (Line*)realloc(s_ln, (size_t)cap * sizeof(Line)); }
        Line* L = &s_ln[s_nln++]; memset(L, 0, sizeof *L); L->cat = c;
        snprintf(L->spk, sizeof L->spk, "%s", t[1]); snprintf(L->path, sizeof L->path, "%s", t[2]); snprintf(L->text, sizeof L->text, "%s", t[3]);
    }
    fclose(f);
    DLOG("[dialogue] v1106D %d voice lines from %s\n", s_nln, path);
    return s_nln > 0;
}
static uint32_t s_rng = 0x1106D;
static uint32_t rnd(void) { s_rng = s_rng * 1103515245u + 12345u; return (s_rng >> 8) & 0xFFFFFF; }
static const Line* pick(int cat, const char* spk)
{
    static int recent[C_N][4];
    int cand[512], nc = 0;
    for (int pass = 0; pass < 2 && !nc; pass++)
        for (int i = 0; i < s_nln && nc < 512; i++) {
            if (s_ln[i].cat != cat) continue;
            if (pass == 0 && spk && strcmp(s_ln[i].spk, spk)) continue;
            int rec = 0; for (int r = 0; r < 4; r++) if (recent[cat][r] == i + 1) rec = 1;
            if (!rec) cand[nc++] = i;
        }
    if (!nc) for (int i = 0; i < s_nln && nc < 512; i++) if (s_ln[i].cat == cat) cand[nc++] = i;
    if (!nc) return NULL;
    const int i = cand[rnd() % (uint32_t)nc];
    memmove(&recent[cat][1], &recent[cat][0], 3 * sizeof(int)); recent[cat][0] = i + 1;
    return &s_ln[i];
}

/* ------------------------------------------------------------------ voice channel (decode on the main thread, mix on the audio thread) */
typedef struct { float* s; int n; float gain; } Pcm;
static Pcm* volatile s_cur;          /* being mixed; the mixer sets it to NULL when it ends */
static struct { Pcm* p; ULONGLONG t; } s_dead[8]; static int s_ndead;   /* retired clips, freed 200 ms later (the mixer may still be inside its block) */
static volatile LONG s_pos;
static float s_duck_cur = 1.0f, s_vol = 1.0f, s_duck = 0.6f;
/* v1113B one-shot sound effects mixed with the dialogue channel (gf_sfx_body_hit). Voices read static buffers that are never
 * freed, so the audio thread can never touch freed memory. */
typedef struct { const float* s; int n; volatile LONG pos; float gain; volatile LONG on; } Sfx;
static Sfx s_sfx[6];
static void mix(float* st, unsigned fr)
{
    for (int v = 0; v < 6; v++) { Sfx* x = &s_sfx[v]; if (!x->on) continue;
        LONG p = x->pos; const float g = x->gain;
        for (unsigned i = 0; i < fr; i++) { if (p >= x->n) { x->on = 0; break; } const float y = x->s[p++] * g; st[2 * i] += y; st[2 * i + 1] += y; }
        x->pos = p; }
    Pcm* c = s_cur;
    const float target = c ? s_duck : 1.0f;
    float acc2 = 0.0f; unsigned nacc = 0;   /* v1117A mouth: RMS of the voice in this block */
    for (unsigned i = 0; i < fr; i++) {
        s_duck_cur += (target - s_duck_cur) * 0.0015f;   /* ~15 ms ramp */
        float l = st[2 * i] * s_duck_cur, r = st[2 * i + 1] * s_duck_cur;
        if (c) { const LONG p = s_pos; if (p < c->n) { const float v = c->s[p] * c->gain; l += v; r += v; s_pos = p + 1; acc2 += v * v; nacc++; }
                 else { InterlockedCompareExchangePointer((PVOID volatile*)&s_cur, NULL, c); c = NULL; } }
        st[2 * i] = l > 1.0f ? 1.0f : l < -1.0f ? -1.0f : l; st[2 * i + 1] = r > 1.0f ? 1.0f : r < -1.0f ? -1.0f : r;
    }
    { extern volatile float g_dlg_rms; g_dlg_rms = nacc ? sqrtf(acc2 / (float)nacc) : 0.0f; }   /* v1117A */
}
/* v1117A TALKING MOUTHS: who is speaking (set with every play_line) + the voice RMS from the mixer. gf_ragdoll.c mouth_post
 * (after that entity's skeleton update) asks gf_dialogue_mouth for the jaw opening 0..1 (fast attack, slower release). */
volatile float g_dlg_rms;
static volatile uint32_t s_spk_ent;
static float s_mouth_env; static double s_mouth_t;
int gf_dialogue_mouth(uint32_t ent, float* open)
{
    if (!ent || ent != s_spk_ent) return 0;
    const double t = (double)GetTickCount64() / 1000.0; float dt = (float)(t - s_mouth_t); s_mouth_t = t; if (!(dt > 0.0f) || dt > 0.1f) dt = 0.016f;
    float target = s_cur ? (g_dlg_rms - 0.012f) * 7.0f : 0.0f; target = target < 0.0f ? 0.0f : target > 1.0f ? 1.0f : target;
    const float k = target > s_mouth_env ? 1.0f - expf(-dt / 0.035f) : 1.0f - expf(-dt / 0.09f);
    s_mouth_env += (target - s_mouth_env) * k;
    if (!s_cur && s_mouth_env < 0.01f) { s_mouth_env = 0.0f; s_spk_ent = 0; return 0; }
    *open = s_mouth_env; return 1;
}
/* v1113B (user: "when I shoot ragdolls it's not the correct audio playing, it just sounds like I'm shooting the ground"):
 * corpses are our ragdolls, the game's bullet goes past them into the pavement. gf_ragdoll.c corpse_fire calls this on every
 * corpse hit. assets/gf_sfx_bodyhit.bin = the game's own bullet-into-body sound: GAM.str (GlobalFolder/audio_global, RefPack ->
 * RWS GAM.sbk, 'tads' table) sample 226, EA-XAS 12 kHz, identified by cross-correlating a recording of the final mix
 * (play-soundtest.cmd, AUDIO_WAV + GF_SND_PROBE) with every bank sample: it follows game-registered body hits 80-140 ms after the
 * shot, sample 392 = the ground hit. File: "GFSFX1\0\0", u32 rate (48000), u32 n, float[n] mono. Level from the recording:
 * x0.55 at ~4 m -> gain 0.55 * clamp(4 / d, 0.12, 1.3). GF_BODYHIT_SFX=0 off, GF_BODYHIT_VOL (1.0). */
static float* s_bh; static int s_bh_n = -1; static float s_bh_vol = 1.0f;
void gf_sfx_body_hit(float dist)
{
    if (s_bh_n < 0) {
        s_bh_n = 0;
        const char* e = getenv("GF_BODYHIT_SFX"); if (e && *e == '0') { DLOG("[sfx] v1113B body-hit sound off (GF_BODYHIT_SFX=0)\n"); return; }
        e = getenv("GF_BODYHIT_VOL"); if (e && *e) { s_bh_vol = (float)atof(e); if (!(s_bh_vol >= 0.0f && s_bh_vol <= 4.0f)) s_bh_vol = 1.0f; }
        char exe[MAX_PATH]; const DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH); FILE* f = NULL;
        if (n && n < MAX_PATH) { char* sl = strrchr(exe, '\\'); if (sl) *sl = 0;
            static const char* rel[] = { "\\assets\\gf_sfx_bodyhit.bin", "\\..\\assets\\gf_sfx_bodyhit.bin", "\\..\\..\\assets\\gf_sfx_bodyhit.bin" };
            for (int i = 0; i < 3 && !f; i++) { char p[MAX_PATH + 64]; snprintf(p, sizeof p, "%s%s", exe, rel[i]); f = fopen(p, "rb"); } }
        if (f) { char mg[8]; uint32_t h[2];
            if (fread(mg, 1, 8, f) == 8 && !memcmp(mg, "GFSFX1", 6) && fread(h, 4, 2, f) == 2 && h[0] == 48000u && h[1] > 0 && h[1] < 48000u * 10u) {
                s_bh = (float*)malloc(sizeof(float) * h[1]);
                if (s_bh && fread(s_bh, sizeof(float), h[1], f) == h[1]) s_bh_n = (int)h[1]; }
            fclose(f); }
        DLOG("[sfx] v1113B body-hit sound: %s (%.2f s)\n", s_bh_n > 0 ? "loaded" : "assets\\gf_sfx_bodyhit.bin missing -> off", s_bh_n / 48000.0);
    }
    if (s_bh_n <= 0) return;
    if (!g_audio_overlay_mix) g_audio_overlay_mix = mix;   /* GF_DIALOGUE=0: the mixer is not installed yet */
    float d = dist > 0.1f ? dist : 0.1f; float k = 4.0f / d; k = k < 0.12f ? 0.12f : k > 1.3f ? 1.3f : k;
    const float jit = 0.9f + 0.2f * (float)(GetTickCount64() % 101) / 100.0f;
    int v = -1; LONG best = -1; for (int i = 0; i < 6; i++) { if (!s_sfx[i].on) { v = i; break; } if (s_sfx[i].pos > best) { best = s_sfx[i].pos; v = i; } }
    Sfx* x = &s_sfx[v]; InterlockedExchange(&x->on, 0);
    x->s = s_bh; x->n = s_bh_n; x->gain = 0.55f * k * jit * s_bh_vol; x->pos = 0;
    MemoryBarrier(); InterlockedExchange(&x->on, 1);
    static int nl = 0; if (nl < 40) { nl++; DLOG("[sfx] v1113B body hit at %.1f m, gain %.2f (voice %d)\n", dist, x->gain, v); }
}
static int s16(int v) { v &= 0xFFFF; return v >= 32768 ? v - 65536 : v; }
static Pcm* decode_snu(const char* rel)
{
    char path[MAX_PATH + 160]; FILE* f = NULL;
    { const char* lp = ps3_fs_lang_pack_dir();
      if (lp) { snprintf(path, sizeof path, "%s/audiostreams/%s", lp, rel); f = fopen(path, "rb"); } }
    if (!f) { snprintf(path, sizeof path, "%s/PS3_GAME/USRDIR/audiostreams/%s", ppu_vfs_root ? ppu_vfs_root : ".", rel); f = fopen(path, "rb"); }
    if (!f) { DLOG("[dialogue] cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); const long L = ftell(f); fseek(f, 0, SEEK_SET);
    if (L < 0x30 || L > 8 * 1024 * 1024) { fclose(f); return NULL; }
    uint8_t* d = (uint8_t*)malloc((size_t)L); const size_t got = fread(d, 1, (size_t)L, f); fclose(f);
    if (got != (size_t)L) { free(d); return NULL; }
#define BE32(o) (((uint32_t)d[o] << 24) | ((uint32_t)d[(o) + 1] << 16) | ((uint32_t)d[(o) + 2] << 8) | d[(o) + 3])
    const uint32_t off = BE32(8); const int codec = d[0x10] & 0xF, rate = (d[0x12] << 8) | d[0x13]; const int ns = (int)(BE32(0x14) & 0x0FFFFFFF);
    if (codec != 4 || rate < 8000 || rate > 48000 || ns <= 0 || ns > 48000 * 120 || off >= (uint32_t)L) {
        DLOG("[dialogue] %s: unsupported (codec %d rate %d samples %d)\n", rel, codec, rate, ns); free(d); return NULL; }
    int16_t* pcm = (int16_t*)calloc((size_t)ns + 256, 2); int np = 0;
    static const int C[4][2] = { { 0, 0 }, { 240, 0 }, { 460, -208 }, { 392, -220 } };
    uint32_t p = off;
    while (p + 8 <= (uint32_t)L && np < ns) {
        const int fl = d[p]; const uint32_t sz = BE32(p) & 0xFFFFFFu; if (sz < 8 || p + sz > (uint32_t)L) break;
        const uint32_t nfr = (sz - 8) / 76;
        for (uint32_t k = 0; k < nfr && np < ns; k++) {
            const uint8_t* fr = d + p + 8 + 76 * k;
            for (int sf = 0; sf < 4; sf++) {
                const uint32_t hd = fr[4 * sf] | (fr[4 * sf + 1] << 8) | (fr[4 * sf + 2] << 16) | ((uint32_t)fr[4 * sf + 3] << 24);
                const int ci = hd & 3, sh = (hd >> 16) & 0xF; int h2 = s16(hd & 0xFFF0), h1 = s16((hd >> 16) & 0xFFF0);
                if (np < ns) pcm[np++] = (int16_t)h2; if (np < ns) pcm[np++] = (int16_t)h1;
                for (int j = 0; j < 15; j++) { const int b = fr[0x10 + sf + 4 * j];
                    for (int hi = 1; hi >= 0; hi--) { int nb = hi ? (b >> 4) & 0xF : b & 0xF; if (nb > 7) nb -= 16;
                        int s = ((nb << 12) >> sh) + ((C[ci][0] * h1 + C[ci][1] * h2 + 128) >> 8);
                        s = s > 32767 ? 32767 : s < -32768 ? -32768 : s; if (np < ns) pcm[np++] = (int16_t)s; h2 = h1; h1 = s; } }
            }
        }
        p += sz; if (fl & 0x80) break;
    }
    free(d);
#undef BE32
    Pcm* o = (Pcm*)calloc(1, sizeof(Pcm));
    const double ratio = 48000.0 / rate; o->n = (int)(np * ratio); o->s = (float*)malloc(sizeof(float) * (size_t)(o->n + 1));
    for (int i = 0; i < o->n; i++) { const double x = i / ratio; const int a = (int)x; const float t = (float)(x - a);
        const float s0 = pcm[a < np ? a : np - 1], s1 = pcm[a + 1 < np ? a + 1 : np - 1]; o->s[i] = (s0 + (s1 - s0) * t) * (1.0f / 32768.0f); }
    free(pcm); o->gain = 1.0f;
    return o;
}
static double play_line(const Line* ln, float gain)   /* returns the line's length in seconds (0 = failed) */
{
    if (!ln) return 0.0;
    Pcm* c = decode_snu(ln->path); if (!c) return 0.0;
    c->gain = gain * s_vol;
    InterlockedExchangePointer((PVOID volatile*)&s_cur, NULL);
    static Pcm* last; if (last && s_ndead < 8) { s_dead[s_ndead].p = last; s_dead[s_ndead].t = GetTickCount64(); s_ndead++; } last = c;
    s_pos = 0; InterlockedExchangePointer((PVOID volatile*)&s_cur, c);
    return c->n / 48000.0;
}


/* ------------------------------------------------------------------ v1107P face-to-face (F2F) probe
 * The game's own conversation system (Corleone members, merchants ...): PlayerF2FSM (factory func_001FE7F8 -> ctor func_001FB0C8,
 * 512 B, vtable 0x100491A8: slot 3 action handler func_001FD7B8, slot 4 condition handler func_001F765C (ids 15..45, rest ->
 * base func_001CB8E8)) and NPCF2FStateMachine (factory func_001A9024 -> ctor func_001A88F8, 140 B, vtable 0x100481B8: slot 3
 * actions func_001A851C, slot 4 conditions func_001A7178). States (playerF2FStateTable, builder func_001F4FC0): Inactive -ev33->
 * Valid -ev19-> F2FWaitingForLoad -> F2FMoveToPosition -ev1-> F2FActivate -> F2FIdleUserInput / F2FVoiceover / F2FShopping ...
 * -> F2FDeactivate -> F2FEnd -ev4-> Inactive. This probe logs every action, every condition whose result changes, and dumps
 * the SM object the first time each action id runs, so a real conversation shows where the target / camera live.
 * GF_F2F_PROBE=0 off. Log [f2f]. */
static gfn o_pf_act, o_pf_cond, o_nf_act, o_nf_cond;
static int s_f2f_nlog;
static struct { uint32_t ent; ULONGLONG t; } s_f2f_ent[8];   /* v1107H: NPCs that own the game's own conversation */
static void f2f_ent_note(uint32_t sm) { const uint32_t e = rd32(sm + 0x58u); if (!ok(e)) return; int k = -1, o = 0;
    for (int i = 0; i < 8; i++) { if (s_f2f_ent[i].ent == e) k = i; if (s_f2f_ent[i].t < s_f2f_ent[o].t) o = i; }
    if (k < 0) k = o; s_f2f_ent[k].ent = e; s_f2f_ent[k].t = GetTickCount64(); }
static int game_f2f_npc(uint32_t e) { for (int i = 0; i < 8; i++) if (s_f2f_ent[i].ent == e && GetTickCount64() - s_f2f_ent[i].t < 8000) return 1; return 0; }
static volatile int s_pf_valid = 0; static volatile unsigned long long s_pf_valid_t = 0;   /* v1107C: the game's own F2F has a target (cond 33) */
#define FLOG(...) do { if (s_f2f_nlog < 600) { s_f2f_nlog++; printf(__VA_ARGS__); fflush(stdout); } } while (0)
static void f2f_dump(const char* tag, uint32_t obj, int words)
{
    if (!ok(obj)) return;
    char b[2400]; int o = 0;
    for (int i = 0; i < words && o < 2300; i++) { const uint32_t v = rd32(obj + 4u * (uint32_t)i); if (v) o += snprintf(b + o, sizeof b - (size_t)o, " +%X=%08X", 4 * i, v); }
    FLOG("[f2f] %s dump %08X:%s\n", tag, obj, b);
}
static void hk_pf_act(ppu_context* c)
{
    const uint32_t th = (uint32_t)c->gpr[3]; const uint32_t r4 = (uint32_t)c->gpr[4], r5 = (uint32_t)c->gpr[5], r6 = (uint32_t)c->gpr[6], r7 = (uint32_t)c->gpr[7];
    static uint8_t seen[256];
    o_pf_act(c);
    __try { FLOG("[f2f] player act this=%08X r4=%X r5=%X r6=%X r7=%X -> %X\n", th, r4, r5, r6, r7, (uint32_t)c->gpr[3]);
        const uint32_t id = r6 < 256 ? r6 : (r7 < 256 ? r7 : 255); if (!seen[id]) { seen[id] = 1; char t[32]; snprintf(t, sizeof t, "player act %u", id); f2f_dump(t, th, 128); } }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static void hk_pf_cond(ppu_context* c)
{
    const uint32_t th = (uint32_t)c->gpr[3], id = (uint32_t)c->gpr[6];
    o_pf_cond(c);
    __try { static int16_t last[256]; const int r = (int)(uint32_t)c->gpr[3];
        if (id == 33) { s_pf_valid = r; s_pf_valid_t = GetTickCount64(); }
        if (id < 256 && last[id] != (int16_t)(r + 1)) { last[id] = (int16_t)(r + 1); FLOG("[f2f] player cond %u this=%08X -> %d\n", id, th, r); } }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static void hk_nf_act(ppu_context* c)
{
    const uint32_t th = (uint32_t)c->gpr[3]; const uint32_t r6 = (uint32_t)c->gpr[6], r7 = (uint32_t)c->gpr[7];
    static uint8_t seen[256];
    o_nf_act(c);
    __try { f2f_ent_note(th); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    __try { FLOG("[f2f] npc act this=%08X (ent %08X) r6=%X r7=%X -> %X\n", th, rd32(th + 0x4C), r6, r7, (uint32_t)c->gpr[3]);
        const uint32_t id = r6 < 256 ? r6 : (r7 < 256 ? r7 : 255); if (!seen[id]) { seen[id] = 1; char t[32]; snprintf(t, sizeof t, "npc act %u", id); f2f_dump(t, th, 36); } }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static void hk_nf_cond(ppu_context* c)
{
    const uint32_t th = (uint32_t)c->gpr[3], id = (uint32_t)c->gpr[6];
    o_nf_cond(c);
    __try { f2f_ent_note(th); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    __try { static struct { uint32_t th; int16_t last[64]; } S2[8]; static int n2; int k = -1;
        for (int i = 0; i < n2; i++) if (S2[i].th == th) k = i;
        if (k < 0 && n2 < 8) { k = n2++; S2[k].th = th; memset(S2[k].last, 0, sizeof S2[k].last); }
        const int r = (int)(uint32_t)c->gpr[3];
        if (k >= 0 && id < 64 && S2[k].last[id] != (int16_t)(r + 1)) { S2[k].last[id] = (int16_t)(r + 1); FLOG("[f2f] npc cond %u this=%08X (ent %08X) -> %d\n", id, th, rd32(th + 0x4C), r); } }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static void f2f_probe_init(void)
{
    const char* e = getenv("GF_F2F_PROBE"); if (e && *e == '0') return;
    o_pf_act = lookup(0x001FD7B8u); o_pf_cond = lookup(0x001F765Cu); o_nf_act = lookup(0x001A851Cu); o_nf_cond = lookup(0x001A7178u);
    if (!o_pf_act || !o_pf_cond || !o_nf_act || !o_nf_cond) { FLOG("[f2f] v1107P probe: a function is missing -- off\n"); return; }
    ppu_register_function(0x001FD7B8u, hk_pf_act); ppu_register_function(0x001F765Cu, hk_pf_cond);
    ppu_register_function(0x001A851Cu, hk_nf_act); ppu_register_function(0x001A7178u, hk_nf_cond);
    FLOG("[f2f] v1107P probe ON (player F2F act/cond, NPC F2F act/cond hooked)\n");
}


/* ------------------------------------------------------------------ v1107C dialogue mode: NPC stops + faces the player, two-shot camera */
/* v1107H: func_00234994(ent, on, r5, 0, f1) is REFERENCE COUNTED: on=1 -> ent+0x14B4 |= 0x200, timer +0x17F0 = f1, count +0x17F4 ++;
 * on=0 -> count -- and the flag only clears at 0. v1107C refreshed it every 1.5 s and "released" with another hold -> the count
 * never returned to 0 and the NPC ran in place for good (user 18:32). Now exactly one acquire and one release per talk. */
static void ai_hold_set(uint32_t ent, int on, double secs)
{
    static gfn f; if (!f) f = lookup(0x00234994u); if (!f || !ok(ent)) return;
    const uint64_t a[4] = { ent, (uint64_t)(on ? 1 : 0), 0, 0 }; gcall(f, a, 4, secs, NULL);
    DLOG("[dialogue] AI hold %s on %08X: flags %08X count %d\n", on ? "set" : "released", ent, rd32(ent + 0x14B4u), (int)rd32(ent + 0x17F4u));
}
static uint32_t vfunc(uint32_t ent, uint32_t off) { const uint32_t vt = rd32(ent); if (!ok(vt)) return 0; const uint32_t opd = rd32(vt + off); return ok(opd) ? rd32(opd) : 0; }
static float ent_fwd_angle(uint32_t ent, float* T)   /* world yaw of the model's +z (gf_ragdoll.c: model = M (w - T), the model faces +z) */
{
    float M[9]; if (!gf_rag_ent_frame(ent, M, T)) return 1e9f;
    return atan2f(M[6], M[8]);
}
/* turn ent about world up so its +z points at (tx, tz), at most maxstep rad this call. SetMatrix = vt+0x30 + position block vt+0x24
 * (as gf_ragdoll.c stumble_move). Returns the remaining error. */
static float turn_toward(uint32_t ent, float tx, float tz, float maxstep, const float* pin)   /* v1107W pin: keep x/z there */
{
    float T[3]; const float cur = ent_fwd_angle(ent, T); if (cur > 1e8f) return 0.0f;
    const float want = atan2f(tx - T[0], tz - T[2]);
    float e = want - cur; while (e > 3.14159265f) e -= 6.2831853f; while (e < -3.14159265f) e += 6.2831853f;
    const int moved = pin && ((T[0] - pin[0]) * (T[0] - pin[0]) + (T[2] - pin[2]) * (T[2] - pin[2]) > 0.0004f);
    if (fabsf(e) < 0.02f && !moved) return e;
    float st = e > maxstep ? maxstep : e < -maxstep ? -maxstep : e;
    if (fabsf(e) < 0.02f) st = 0.0f;
    const uint32_t xf = rd32(ent + 0x68); ppu_context* cur_ctx = g_active_ctx;
    gfn f30 = lookup(vfunc(ent, 0x30)), f24 = lookup(vfunc(ent, 0x24));
    if (!ok(xf) || !cur_ctx || !f30 || !f24) return e;
    const uint32_t buf = (((uint32_t)cur_ctx->gpr[1] - 0x800u) & ~0xFu);
    for (uint32_t k = 0; k < 64; k += 4) { const uint8_t* q = vm_base + xf + 0x10u + k; uint8_t* d = vm_base + buf + k; d[0] = q[0]; d[1] = q[1]; d[2] = q[2]; d[3] = q[3]; }
    if (pin) { float v[2] = { pin[0], pin[2] }; for (int q = 0; q < 2; q++) { uint32_t u; memcpy(&u, &v[q], 4); uint8_t* d = vm_base + buf + (q ? 0x38u : 0x30u); d[0] = (uint8_t)(u >> 24); d[1] = (uint8_t)(u >> 16); d[2] = (uint8_t)(u >> 8); d[3] = (uint8_t)u; } }
    /* rows or columns hold the model axes (gf_ragdoll.c convention vote): rotate whichever matches; a yaw by +st turns +z
     * (sin a, cos a) to (sin(a+st), cos(a+st)): x' = x cos + z sin, z' = -x sin + z cos */
    float R[3][3]; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R[i][j] = rdf(buf + 16u * (uint32_t)i + 4u * (uint32_t)j);
    float M[9], TT[3]; gf_rag_ent_frame(ent, M, TT);
    const int rows = fabsf(R[2][0] - M[6]) + fabsf(R[2][2] - M[8]) < 0.01f;
    const float c = cosf(st), s = sinf(st);
    for (int i = 0; i < 3; i++) {
        if (rows) { const float x = R[i][0], z = R[i][2]; R[i][0] = x * c + z * s; R[i][2] = -x * s + z * c; }
        else      { const float x = R[0][i], z = R[2][i]; R[0][i] = x * c + z * s; R[2][i] = -x * s + z * c; }
    }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { float v = R[i][j]; uint32_t u; memcpy(&u, &v, 4); uint8_t* d = vm_base + buf + 16u * (uint32_t)i + 4u * (uint32_t)j; d[0] = (uint8_t)(u >> 24); d[1] = (uint8_t)(u >> 16); d[2] = (uint8_t)(u >> 8); d[3] = (uint8_t)u; }
    const uint64_t a[2] = { ent, buf }; if (!gcall(f30, a, 2, 0.0, NULL)) return e;
    const uint32_t blk = buf + 0x40u; const float px = rdf(buf + 0x30), py = rdf(buf + 0x34), pz = rdf(buf + 0x38);
    { float v[3] = { px, py, pz }; for (int i = 0; i < 3; i++) { uint32_t u; memcpy(&u, &v[i], 4); uint8_t* d = vm_base + blk + 4u * (uint32_t)i; d[0] = (uint8_t)(u >> 24); d[1] = (uint8_t)(u >> 16); d[2] = (uint8_t)(u >> 8); d[3] = (uint8_t)u; } }
    { uint8_t* d = vm_base + blk + 12u; d[0] = 0; d[1] = 0; d[2] = 0; d[3] = 2; }
    const uint64_t b[2] = { ent, blk }; gcall(f24, b, 2, 0.0, NULL);
    return e - st;
}


/* ------------------------------------------------------------------ v1107K combatants (gangsters, thugs, cops): CombatNPCStateMachine
 * (vtable 0x10047490, action slot 3 func_00148088 -- NOTES_ragdoll_RE.md; civilians run Innocent / Pedestrian machines instead).
 * gf_ragdoll.c's hook of that handler calls gf_dialogue_combat_note(entity = sm+0x50, action id). A combatant is never scared;
 * one whose machine fires >= GF_DLG_FIGHT_ACTS (4) actions in 2 s is fighting -> no dialogue. Log the ids of the NPC we talk to
 * (and for 12 s after a hostile end) so the "attack" actions can be learned. */
static struct { uint32_t ent; ULONGLONG t_last, t_win; int nwin, nprev; } s_cmb[48];
static volatile uint32_t s_watch_ent; static volatile ULONGLONG s_watch_until;
void gf_dialogue_combat_note(uint32_t ent, uint32_t act)
{
    if (!ok(ent)) return;
    const ULONGLONG t = GetTickCount64(); int k = -1, o = 0;
    for (int i = 0; i < 48; i++) { if (s_cmb[i].ent == ent) { k = i; break; } if (s_cmb[i].t_last < s_cmb[o].t_last) o = i; }
    if (k < 0) { k = o; s_cmb[k].ent = ent; s_cmb[k].t_win = t; s_cmb[k].nwin = 0; s_cmb[k].nprev = 0; }
    if (t - s_cmb[k].t_win > 2000) { s_cmb[k].nprev = s_cmb[k].nwin; s_cmb[k].nwin = 0; s_cmb[k].t_win = t; }
    s_cmb[k].nwin++; s_cmb[k].t_last = t;
    if (ent == s_watch_ent && t < s_watch_until) { static int n; if (n < 400) { n++; printf("[dialogue] combat act %u on %08X\n", act, ent); fflush(stdout); } }
}
static int is_combatant(uint32_t e) { for (int i = 0; i < 48; i++) if (s_cmb[i].ent == e) return GetTickCount64() - s_cmb[i].t_last < 60000; return 0; }
static int in_fight(uint32_t e)
{
    static int thr = -1; if (thr < 0) { const char* v = getenv("GF_DLG_FIGHT_ACTS"); thr = v && atoi(v) > 0 ? atoi(v) : 4; }
    for (int i = 0; i < 48; i++) if (s_cmb[i].ent == e) { const ULONGLONG t = GetTickCount64();
        return t - s_cmb[i].t_last < 2500 && (s_cmb[i].nwin >= thr || (t - s_cmb[i].t_win < 2000 && s_cmb[i].nprev >= thr)); }
    return 0;
}
/* the game's living-world event on an NPC (same call as gf_ragdoll.c vt_say: context push func_0009764C, func_00239EDC(ent, id, 0,
 * ctx), pop func_0009767C; the event's cooldown cleared first). 0x11 act_threatening, 0x05 taunt. Returns the dispatcher's result. */
static uint32_t lw_event(uint32_t ent, uint32_t id)
{
    ppu_context* cur = g_active_ctx; if (!cur || !ok(ent)) return 0;
    static gfn f_push, f_say, f_pop; if (!f_say) { f_push = lookup(0x0009764Cu); f_say = lookup(0x00239EDCu); f_pop = lookup(0x0009767Cu); }
    if (!f_push || !f_say || !f_pop) return 0;
    const uint32_t tab = rd32(TOC - 0x5EACu); if (ok(tab) && id < 0xD8) { float v = -100000.0f; uint32_t u; memcpy(&u, &v, 4); uint8_t* d = vm_base + tab + 16u * id + 8u; d[0] = (uint8_t)(u >> 24); d[1] = (uint8_t)(u >> 16); d[2] = (uint8_t)(u >> 8); d[3] = (uint8_t)u; }
    const uint32_t buf = ((((uint32_t)cur->gpr[1] - 0x1000u) & ~0xFu)) - 0x40u; uint64_t r = 0;
    { const uint64_t a[3] = { buf, 0, 0 }; gcall(f_push, a, 3, 0.0, NULL); }
    { const uint64_t a[4] = { ent, id, 0, buf }; gcall(f_say, a, 4, 0.0, &r); }
    { const uint64_t a[1] = { buf }; gcall(f_pop, a, 1, 0.0, NULL); }
    return (uint32_t)r;
}

/* ------------------------------------------------------------------ conversation */
enum { D_IDLE, D_MENU, D_PLAY };
enum { A_HELLO, A_GOSSIP, A_THREAT, A_LEAVE, A_FLIRT };
static const char* const k_act_label[5] = { "Say hello", "What's the word?", "Intimidate", "Leave", "Flirt" };
static struct { int st, act, step, nthreat, female, prss; uint32_t npc; double t_step, t_menu, t_idle, t_hold, t_start, t_idlechk, t_gest; char name[48]; int nface; float pin[3], ppin[3]; int acts[5], nact, held, combatant, hostile; } D;
static double s_pl_gest_end = 0.0, s_pl_stop2 = 0.0;   /* v1107W: player gesture must not outlive its moment / the talk */
static int s_on = -1, s_key = 'E', s_prev[16]; static float s_test = 0.0f; static int s_test_step = -1; static double s_test_t;
static int edge(int i, int vk) { const int k = fg_ours() && (GetAsyncKeyState(vk) & 0x8000); const int e = k && !s_prev[i]; s_prev[i] = k; return e; }
static void set_sub(const char* who, const char* text, double secs)
{
    snprintf(g_gfd_sub_who, sizeof g_gfd_sub_who, "%s", who ? who : ""); snprintf(g_gfd_sub, sizeof g_gfd_sub, "%s", text ? text : "");
    g_gfd_sub_t1 = GetTickCount64() + (unsigned long long)(secs * 1000.0) + 600;
}
static void prime_keys(void)   /* a key already held when the menu (re)opens is not a press */
{
    static const int vk[15] = { 0, 'W', VK_UP, 'S', VK_DOWN, VK_RETURN, VK_SPACE, VK_LBUTTON, '1', '2', '3', '4', VK_BACK, VK_ESCAPE, '5' };
    for (int i = 1; i < 15; i++) s_prev[i] = fg_ours() && (GetAsyncKeyState(vk[i]) & 0x8000);
}
static void open_menu(void)
{
    prime_keys();
    /* v1107L: women get a Flirt option */
    D.nact = 0; D.acts[D.nact++] = A_HELLO; D.acts[D.nact++] = A_GOSSIP; if (D.female) D.acts[D.nact++] = A_FLIRT; D.acts[D.nact++] = A_THREAT; D.acts[D.nact++] = A_LEAVE;
    for (int i = 0; i < D.nact; i++) { extern const char* ui_tr(const char* en); snprintf(g_gfd_opt[i], 40, "%s", ui_tr(k_act_label[D.acts[i]])); }
    g_gfd_nopt = D.nact; if (g_gfd_sel < 0 || g_gfd_sel >= D.nact) g_gfd_sel = 0;
    snprintf(g_gfd_who, sizeof g_gfd_who, "%s", D.name);
    g_gfd_menu = 1; D.st = D_MENU; D.t_menu = now_s();
}
static void close_menu(void) { if (g_gfd_menu) { g_gfd_menu = 0; g_gfd_close_t = GetTickCount64(); g_gfd_clear = 1; } }
static void end_talk(const char* why)
{
    close_menu(); if (D.st != D_IDLE) DLOG("[dialogue] end (%s)\n", why);
    if (D.st != D_IDLE && ok(D.npc)) { if (D.held) ai_hold_set(D.npc, 0, 0.0); D.held = 0; stop_layer(D.npc); }   /* release (one per acquire) */
    if (D.st != D_IDLE) { const uint32_t pl = player(); if (pl) stop_layer(pl); s_pl_stop2 = now_s() + 0.5; s_pl_gest_end = 0.0; }   /* v1107W: no gesture runs on after the talk */
    if (g_gfd_lock) { g_gfd_lock = 0; g_gfd_close_t = GetTickCount64(); g_gfd_clear = 1; }
    g_gfd_cam_on = 0; D.st = D_IDLE; D.npc = 0;
}

extern int gf_rag_ray(const float* o, const float* d, float maxd, float* t, float* n);
static struct { uint32_t ent; float p[3]; double t; } s_spd[8];   /* v1107H: recent positions of nearby NPCs (speed) */
static void speed_track(uint32_t pl)
{
    static double tl; const double t = now_s(); if (t - tl < 0.1) return; tl = t;
    const uint32_t n = gf_nearest_npc(5.0f); if (!n) return;
    int k = -1, o = 0; for (int i = 0; i < 8; i++) { if (s_spd[i].ent == n) k = i; if (s_spd[i].t < s_spd[o].t) o = i; }
    if (k >= 0 && t - s_spd[k].t < 0.25) return;
    if (k < 0) k = o; s_spd[k].ent = n; ent_pos(n, s_spd[k].p); s_spd[k].t = t; (void)pl;
}
static float npc_speed(uint32_t n)
{
    float q[3]; if (!ent_pos(n, q)) return 0.0f;
    for (int i = 0; i < 8; i++) if (s_spd[i].ent == n) { const double dt = now_s() - s_spd[i].t; if (dt < 0.08 || dt > 1.5) return 0.0f;
        const float dx = q[0] - s_spd[i].p[0], dz = q[2] - s_spd[i].p[2]; return sqrtf(dx * dx + dz * dz) / (float)dt; }
    return 0.0f;
}
/* someone we may talk to: not hurt (fights), not running (fleeing / fighting), same floor, nothing solid in between */
static int talkable(uint32_t pl, uint32_t n)
{
    const float hp = rdf(n + 0x14C8u), hm = rdf(n + 0x14CCu);
    if (fok(hp) && fok(hm) && hm > 0.0f && hp < hm * 0.97f) { DLOG("[dialogue] %08X is hurt (%.0f / %.0f) -> no talk\n", n, hp, hm); return 0; }
    const float sp = npc_speed(n); if (sp > 2.6f) { DLOG("[dialogue] %08X is running (%.1f m/s) -> no talk\n", n, sp); return 0; }
    float P[3], N[3]; if (!ent_pos(pl, P) || !ent_pos(n, N)) return 0;
    if (fabsf(N[1] - P[1]) > 1.2f) { DLOG("[dialogue] %08X is on another level -> no talk\n", n); return 0; }
    const float o[3] = { P[0], P[1] + 1.35f, P[2] }; float d[3] = { N[0] - P[0], N[1] + 1.35f - o[1], N[2] - P[2] };
    const float L = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); if (L < 0.05f) return 1; for (int i = 0; i < 3; i++) d[i] /= L;
    float th = 0.0f, nn[3]; if (gf_rag_ray(o, d, L, &th, nn) && th < L - 0.35f) { DLOG("[dialogue] %08X is behind a wall (hit at %.2f of %.2f m) -> no talk\n", n, th, L); return 0; }
    return 1;
}
static void start_talk(uint32_t pl, uint32_t npc)
{
    memset(&D, 0, sizeof D); D.npc = npc; D.female = is_female(npc);
    D.prss = (int)((npc >> 4) * 2654435761u >> 8);
    D.name[0] = 0; gf_npc_name(npc, D.name, sizeof D.name); if (!D.name[0]) snprintf(D.name, sizeof D.name, "%s", D.female ? "Woman" : "Man");
    D.combatant = is_combatant(npc); s_watch_ent = npc; s_watch_until = GetTickCount64() + 600000;
    g_gfd_sel = 0; open_menu();
    g_gfd_lock = 1; D.t_start = now_s(); D.t_hold = 0.0; D.nface = 0;   /* v1107C */
    ent_pos(npc, D.pin); ent_pos(pl, D.ppin); ai_hold_set(npc, 1, 30.0); D.held = 1;
    play_gesture_m(npc, D.female ? "ambient_idle_female" : "ambient_idle_male", 1); D.t_gest = now_s();   /* v1107W: stop walking at once */
    if (s_test > 0.0f) { extern volatile int g_kd_dump_req; g_kd_dump_req = 1; }   /* soak: frames of the dialogue camera (GF_KD_DUMP) */
    DLOG("[dialogue] v1106D talk to %08X '%s' (%s, %.1f m, voice %s; +0x1814 = %08X, eth %d)\n", npc, D.name, D.female ? "woman" : "man", ent_dist(pl, npc),
         D.female ? k_pr_f[D.prss % 3] : k_pr_m[D.prss % 13], rd32(npc + 0x1814u), (int)rd32(npc + 0x153Cu));
}

/* v1110U gf_shop.c: who was intimidated (INTIMIDATE chosen) and when -- a scared shopkeeper will not sell */
static struct { uint32_t ent; ULONGLONG t; } s_thr[32]; static unsigned s_thr_i;
static void threat_note(uint32_t e) { int k = -1; for (int i = 0; i < 32; i++) if (s_thr[i].ent == e) { k = i; break; } if (k < 0) k = (int)(s_thr_i++ % 32u); s_thr[k].ent = e; s_thr[k].t = GetTickCount64(); }
long long gf_dialogue_threat_age_ms(uint32_t e) { for (int i = 0; i < 32; i++) if (s_thr[i].ent == e && e) return (long long)(GetTickCount64() - s_thr[i].t); return -1; }
static void mode_tick(uint32_t pl)   /* v1107C: every frame of a conversation */
{
    const double t = now_s();
    float P[3], N[3]; if (!ent_pos(pl, P) || !ent_pos(D.npc, N)) return;
    const float en = turn_toward(D.npc, P[0], P[2], 0.12f, D.pin);
    const float ep = turn_toward(pl, N[0], N[2], 0.12f, D.ppin);
    if (t - D.t_idlechk > 0.2 && t - D.t_gest > 0.4) { D.t_idlechk = t;   /* v1107W: the NPC stands (whole-body idle) when no gesture plays */
        if (layer_hash(D.npc) == 0xFFFFFFFFu) play_gesture_m(D.npc, D.female ? "ambient_idle_female" : "ambient_idle_male", 1); }
    if (D.nface < 8 && t - D.t_start > 0.25 * (D.nface + 1)) { D.nface++; DLOG("[dialogue] facing: npc error %.2f rad, player error %.2f rad\n", en, ep); }
    float dx = N[0] - P[0], dz = N[2] - P[2]; const float L = sqrtf(dx * dx + dz * dz); if (L < 0.05f) return; dx /= L; dz /= L;
    const float rx = dz, rz = -dx;   /* one side of the line player -> NPC */
    static float back = -1.0f, side, up, look; if (back < 0.0f) { const char* e;
        back = (e = getenv("GF_DLG_CAM_BACK")) && atof(e) > 0 ? (float)atof(e) : 1.35f; side = (e = getenv("GF_DLG_CAM_SIDE")) && *e ? (float)atof(e) : 0.75f;
        up = (e = getenv("GF_DLG_CAM_UP")) && *e ? (float)atof(e) : 1.72f; look = (e = getenv("GF_DLG_CAM_LOOK")) && *e ? (float)atof(e) : 0.62f; }
    g_gfd_cam_eye[0] = P[0] - dx * back + rx * side; g_gfd_cam_eye[1] = P[1] + up; g_gfd_cam_eye[2] = P[2] - dz * back + rz * side;
    g_gfd_cam_tgt[0] = P[0] + (N[0] - P[0]) * look; g_gfd_cam_tgt[1] = (P[1] + N[1]) * 0.5f + 1.52f; g_gfd_cam_tgt[2] = P[2] + (N[2] - P[2]) * look;
    g_gfd_cam_on = 1;
}
static void choose(uint32_t pl, int idx)   /* idx = menu row */
{
    if (idx < 0 || idx >= D.nact) return;
    const int act = D.acts[idx];
    close_menu(); D.act = act; D.st = D_PLAY; D.step = 0; D.t_step = now_s();
    DLOG("[dialogue] choice %d (%s)\n", idx + 1, k_act_label[act]);
}
static float npc_gain(uint32_t pl) { const float d = ent_dist(pl, D.npc); return d < 3.0f ? 1.0f : d > 10.0f ? 0.45f : 1.0f - (d - 3.0f) * (0.55f / 7.0f); }
/* step 0: the player; step 1: the NPC (when the player's part has ended); step 2: back to the menu when the NPC's line ended */
static void play_tick(uint32_t pl)
{
    const double t = now_s();
    if (D.step == 0) {
        double len = 0.0;
        s_spk_ent = pl;   /* v1117A: the player's line moves his jaw */
        if (D.act == A_HELLO || D.act == A_GOSSIP || D.act == A_FLIRT) {   /* v1107L: the player speaks too (lines the user transcribed / tagged) */
            play_gesture(pl, D.act == A_HELLO ? "lw_common_respect_nod" : "f2f_player_right_hand_gesture"); s_pl_gest_end = now_s() + 1.6;
            const int cat = D.act == A_HELLO ? C_PL_HELLO : D.act == A_GOSSIP ? C_PL_GOSSIP : C_PL_FLIRT;
            const int speak = D.act == A_FLIRT || (rnd() % 100) < 75u;   /* hello / gossip have one line each: not every time */
            const Line* ln = speak ? pick(cat, "plyr") : NULL; const double l = ln ? play_line(ln, 1.0f) : 0.0;
            if (ln && l > 0.0) { if (ln->text[0]) set_sub(ui_tr("You"), ln->text, l); DLOG("[dialogue] player: \"%s\" (%s, %.1f s)\n", ln->text, ln->path, l); }
            len = l > 0.0 ? l + 0.25 : 0.55; }
        else if (D.act == A_THREAT) { const int gun = armed();
            if (!gun) play_gesture(pl, (rnd() & 1) ? "f2f_player_brandish_fist" : "f2f_player_thump_fist_chest"); s_pl_gest_end = now_s() + 1.6;
            const Line* ln = pick(gun ? C_PL_THREAT_GUN : C_PL_THREAT, "plyr"); const double l = play_line(ln, 1.0f);
            if (ln && l > 0.0) { set_sub(ui_tr("You"), ln->text, l); DLOG("[dialogue] player: \"%s\" (%s, %.1f s)\n", ln->text, ln->path, l); }
            len = l > 0.0 ? l + 0.25 : 0.6; }
        else { const Line* ln = pick(C_PL_BYE, "plyr");   /* v1107W: no hat tip (user: ugly + too long) */ const double l = play_line(ln, 1.0f);
            if (ln && l > 0.0 && ln->text[0]) set_sub(ui_tr("You"), ln->text, l); len = l > 0.0 ? l : 0.5; }
        D.step = 1; D.t_step = t + len; return;
    }
    if (D.step == 1) {
        if (t < D.t_step) return;
        if (D.act == A_LEAVE) { end_talk("leave"); return; }
        const int F = D.female; const char* gvo = F ? "ifgx" : "imgx"; const char* pvo = F ? k_pr_f[D.prss % 3] : k_pr_m[D.prss % 13];
        const Line* ln = NULL; const char* anim = NULL;
        const float hp = rdf(pl + 0x14C8u), hm = rdf(pl + 0x14CCu);
        if (D.act == A_HELLO) {
            if (fok(hp) && fok(hm) && hm > 0.0f && hp / hm < 0.45f) ln = pick(F ? C_HURT_F : C_HURT_M, gvo); else ln = pick(F ? C_HELLO_F : C_HELLO_M, gvo);
            anim = F ? "lw_innocent_female_respect" : "lw_innocent_male_respect";
        } else if (D.act == A_GOSSIP) { ln = pick(F ? C_GOSSIP_F : C_GOSSIP_M, gvo); anim = "lw_common_respect_nod"; }
        else if (D.act == A_FLIRT) { ln = pick(C_FLIRT_F, "ifgx"); anim = (rnd() & 1) ? "lw_female_respect_flirt" : "lw_female_respect_blowkiss"; }
        else {
            D.nthreat++; threat_note(D.npc);   /* v1110U */
            const int gun = armed();
            if (D.combatant && !F) {   /* v1107K: a gangster / thug / cop is not scared: he gets mad, and the talk turns into a fight */
                ln = pick(C_DEFY_M, pvo); anim = (rnd() & 1) ? "lw_mobster_disrespect_upyours" : "lw_mobster_disrespect_chinflick"; D.hostile = 1; }
            else if (!F && D.nthreat >= 2 && (rnd() % 100) < 30u) {   /* a tough civilian can snap too */
                ln = pick(C_DEFY_M, pvo); anim = "lw_innocent_male_threatening"; D.hostile = 1; }
            else if (D.nthreat >= 3) { ln = pick(F ? C_GIVEUP_F : C_GIVEUP_M, pvo); anim = "lw_innocent_surrender"; }
            else if (gun) { ln = pick(F ? C_GUN_F : C_GUN_M, pvo); anim = "f2f_npc_hands_out_afraid"; }
            else if ((rnd() % 100) < (D.nthreat == 1 ? 45u : 25u)) { ln = pick(F ? C_DEFY_F : C_DEFY_M, pvo); anim = F ? "lw_innocent_female_disrespect" : "lw_innocent_male_disrespect"; }
            else { ln = pick(F ? C_SCARED_F : C_SCARED_M, pvo); anim = (rnd() & 1) ? "f2f_npc_hands_out_afraid" : "f2f_npc_talk_fear"; }
        }
        if (anim) { play_gesture_m(D.npc, anim, 1); D.t_gest = t; }
        s_spk_ent = D.npc;   /* v1117A */
        const double l = play_line(ln, npc_gain(pl));
        if (ln && l > 0.0) { set_sub(D.name, ln->text, l); DLOG("[dialogue] %s: \"%s\" (%s, %.1f s)\n", D.name, ln->text, ln->path, l); }
        D.step = 2; D.t_step = t + (l > 0.0 ? l : 1.0) + 0.35; return;
    }
    if (D.hostile && t >= D.t_step - 0.3) {   /* v1107K: he is mad -> the talk ends, the game's own threatening / taunt behaviour starts the fight */
        const uint32_t npc = D.npc; end_talk("hostile");
        const uint32_t r1 = lw_event(npc, 0x11), r2 = lw_event(npc, 0x05);
        s_watch_ent = npc; s_watch_until = GetTickCount64() + 12000;
        DLOG("[dialogue] %08X turned hostile: act_threatening -> %u, taunt -> %u (combat actions logged for 12 s)\n", npc, r1, r2);
        return; }
    if (t >= D.t_step) open_menu();
}
void gf_dialogue_tick(void)
{
    if (s_on < 0) {
        const char* e = getenv("GF_DIALOGUE"); s_on = !(e && *e == '0');
        e = getenv("GF_TALK_KEY"); if (e && isalnum((unsigned char)e[0])) s_key = toupper((unsigned char)e[0]); g_gfd_key = s_key;
        e = getenv("GF_DIALOGUE_VOL"); if (e && atof(e) > 0.0) s_vol = (float)atof(e);
        e = getenv("GF_DIALOGUE_DUCK"); if (e && atof(e) > 0.0) s_duck = (float)atof(e);
        e = getenv("GF_DIALOGUE_TEST"); if (e && atof(e) > 0.0) s_test = (float)atof(e);
        if (s_on) s_on = load_lines();
        if (s_on) g_audio_overlay_mix = mix;
        f2f_probe_init();   /* v1107P */
        DLOG("[dialogue] v1106D street dialogue %s (key %c, vol %.2f, duck %.2f)\n", s_on ? "ON" : "off", s_key, s_vol, s_duck);
    }
    if (!s_on) return;
    for (int i = 0; i < s_ndead; i++) if (GetTickCount64() - s_dead[i].t > 200 && s_dead[i].p != s_cur) {
        free(s_dead[i].p->s); free(s_dead[i].p); s_dead[i] = s_dead[--s_ndead]; i--; }
    __try {
        const uint32_t pl = player();
        if (pl && ((s_pl_gest_end > 0.0 && now_s() > s_pl_gest_end) || (s_pl_stop2 > 0.0 && now_s() > s_pl_stop2))) { stop_layer(pl); s_pl_gest_end = 0.0; s_pl_stop2 = 0.0; }
        if (!pl || !alive(pl) || in_car(pl) || gf_cutscene_active()) { if (D.st != D_IDLE) end_talk("player busy"); g_gfd_cam_on = 0; return; }
        /* soak test: open on the nearest NPC and walk through the choices */
        if (s_test > 0.0f) { if (s_test_step < 0) { s_test_step = 0; s_test_t = now_s() + s_test; }
            if (s_test_step < 7 && now_s() >= s_test_t) {
                if (D.st == D_IDLE && s_test_step == 0) { const uint32_t n = gf_nearest_npc(12.0f); if (n && !is_story(n)) { start_talk(pl, n); s_test_step = 1; } else { s_test_t = now_s() + 2.0; DLOG("[dialogue] test: no NPC within 12 m yet\n"); } }
                else if (D.st == D_MENU) { static const int seq[6] = { A_HELLO, A_GOSSIP, A_THREAT, A_THREAT, A_THREAT, A_LEAVE }; int ix = 0; for (int q = 0; q < D.nact; q++) if (D.acts[q] == seq[s_test_step - 1]) ix = q; g_gfd_sel = ix; choose(pl, ix); s_test_step++; s_test_t = now_s() + 1.0; }
            } }
        const int kt = edge(0, s_key);
        if (D.st == D_IDLE) {
            speed_track(pl);   /* v1107H */
            /* prompt when someone is close (no shop / menus checked here: the overlay hides it under its own pages) */
            /* v1107P: no "T TALK" prompt any more (user: it showed all the time) */
            if (kt) {
                if (s_pf_valid && GetTickCount64() - s_pf_valid_t < 1500) { DLOG("[dialogue] %c: the game's own conversation is available -> left to the game\n", s_key); return; }
                uint32_t n = gf_aim_target_now(); if (n && ent_dist(pl, n) > 6.0f) n = 0;
                if (!n) { n = gf_nearest_npc(2.5f);
                    if (n) { float T[3], N[3]; const float a = ent_fwd_angle(pl, T);   /* in front of the player (+-70 deg) */
                        if (a < 1e8f && ent_pos(n, N)) { float d = atan2f(N[0] - T[0], N[2] - T[2]) - a; while (d > 3.14159265f) d -= 6.2831853f; while (d < -3.14159265f) d += 6.2831853f;
                            if (fabsf(d) > 1.22f) n = 0; } } }
                if (n && is_story(n)) { DLOG("[dialogue] %c: %08X is a story / mission character -> not ours\n", s_key, n); n = 0; }
                if (n && game_f2f_npc(n)) { DLOG("[dialogue] %c: %08X has the game's own conversation -> left to the game\n", s_key, n); n = 0; }
                if (n && in_fight(n)) { DLOG("[dialogue] %c: %08X is fighting -> no talk\n", s_key, n); n = 0; }
                if (n && !talkable(pl, n)) n = 0;
                if (n && alive(n)) start_talk(pl, n);
                else DLOG("[dialogue] %c: nobody to talk to in front within 3 m\n", s_key);
            }
            return;
        }
        if (!ok(D.npc) || !alive(D.npc)) { end_talk("NPC gone"); return; }
        mode_tick(pl);   /* v1107C */
        if (ent_dist(pl, D.npc) > (s_test > 0.0f ? 40.0f : 7.0f)) { end_talk("walked away"); return; }
        if (D.st == D_MENU) {
            const int up = edge(1, 'W') | edge(2, VK_UP), dn = edge(3, 'S') | edge(4, VK_DOWN), go = edge(5, VK_RETURN) | edge(6, VK_SPACE) | edge(7, VK_LBUTTON);
            int pick_n = -1; for (int i = 0; i < 4; i++) if (edge(8 + i, '1' + i)) pick_n = i;
            if (edge(14, '5')) pick_n = 4;
            if (pick_n >= D.nact) pick_n = -1;
            if (kt || edge(12, VK_BACK) || edge(13, VK_ESCAPE)) { end_talk("closed"); return; }
            if (up) g_gfd_sel = (g_gfd_sel + D.nact - 1) % D.nact;
            if (dn) g_gfd_sel = (g_gfd_sel + 1) % D.nact;
            if (pick_n >= 0) { g_gfd_sel = pick_n; choose(pl, pick_n); }
            else if (go) choose(pl, g_gfd_sel);
            else if (now_s() - D.t_menu > 30.0) end_talk("menu timeout");
            return;
        }
        if (kt || edge(12, VK_BACK)) { end_talk("key"); return; }
        play_tick(pl);
    } __except (EXCEPTION_EXECUTE_HANDLER) { end_talk("fault"); }
}
