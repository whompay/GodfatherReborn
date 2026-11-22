/*
 * rsx_ui_overlay.c -- in-game system-dialog overlay (cellMsgDialog) for the D3D12 backend.
 *
 * Replaces the blocking Win32 MessageBox with a prompt drawn into the game's own frame, the
 * way the PS3 system software overlays its dialogs: the title keeps running, the dialog owns
 * the controller until it is answered, and the answer reaches the title through the normal
 * sysutil callback (cellMsgDialog_tick).
 *
 * Look: the title's own HUD typeface (font_main_latin) and controller glyphs (font_icons),
 * extracted from the game data by infamous-recomp/tools/extract_ui_font.py into
 * assets/ui_font.bin -- see that script for the format. No asset -> uiov_dialog_available()
 * is 0 and cellMsgDialog keeps its MessageBox fallback.
 *
 * Threads: cellMsgDialog (PPU thread) opens/polls/closes; the render thread draws and reads
 * input (uiov_render, once per presented frame); cellPadGetData asks uiov_pad_gate() whether
 * the title may see the pad. All shared state is behind s_cs.
 *
 * Input: Cross = XInput A / Enter / Space / Z, Circle = XInput B / Esc / Backspace / X / C,
 * left/right = d-pad / left stick / arrow keys / A,D / Tab; the mouse hovers and clicks buttons.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "rsx_ui_trainer.h"
#include "rsx_ui_shop.h"   /* v1092S inventory / store pages */
#include "rsx_ui_coop.h"   /* v1119M CO-OP tab (Godfather gf_coop.c) */
const uiov_coop_api* volatile g_uiov_coop = 0;
/* rsx_ui_trainer.h: the title's trainer page (F10); NULL = no trainer. */
const uiov_trainer_api* volatile g_uiov_trainer = 0;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include "rsx_d3d12_abi.h"   /* struct-returning D3D12 calls for every Windows SDK */
#include <d3dcompiler.h>
#include <xinput.h>
#include <math.h>
#include <limits.h>

#define UIOV_VW_H      720.0f     /* virtual layout height; width follows the backbuffer aspect */
#define UIOV_MAX_VERTS 12000
#define UIOV_FRAMES    3

typedef struct { float x, y, u, v, r, g, b, a; } UVert;
typedef struct { unsigned code; short x, y, w, h, xo, yo, adv, pad; } UGlyph;

/* ---------------- font asset ---------------- */
static int      s_font_state;            /* 0 untried, 1 ok, -1 failed */
static unsigned s_aw, s_ah, s_nt, s_tline, s_ni, s_iline, s_wx, s_wy;
static UGlyph*  s_gl;                    /* s_nt text glyphs then s_ni icon glyphs */
static unsigned char* s_pix;             /* RGBA8 atlas (freed after upload) */

static int font_try(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    char magic[8]; unsigned h[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "IFUIFNT1", 8) || fread(h, 4, 8, f) != 8) { fclose(f); return 0; }
    s_aw = h[0]; s_ah = h[1]; s_nt = h[2]; s_tline = h[3]; s_ni = h[4]; s_iline = h[5]; s_wx = h[6]; s_wy = h[7];
    if (s_aw != 1024 || s_ah != 1024 || !s_nt || s_nt + s_ni > 4096) { fclose(f); return 0; }
    s_gl = (UGlyph*)calloc(s_nt + s_ni, sizeof(UGlyph));
    s_pix = (unsigned char*)malloc((size_t)s_aw * s_ah * 4);
    int ok = s_gl && s_pix;
    for (unsigned i = 0; ok && i < s_nt + s_ni; i++) {
        unsigned char r[20];
        if (fread(r, 1, 20, f) != 20) { ok = 0; break; }
        memcpy(&s_gl[i].code, r, 4); memcpy(&s_gl[i].x, r + 4, 16);
    }
    if (ok && fread(s_pix, 1, (size_t)s_aw * s_ah * 4, f) != (size_t)s_aw * s_ah * 4) ok = 0;
    fclose(f);
    if (!ok) { free(s_gl); free(s_pix); s_gl = NULL; s_pix = NULL; return 0; }
    fprintf(stderr, "[uiov] dialog font loaded: %s (%u text + %u icon glyphs)\n", path, s_nt, s_ni);
    return 1;
}

static int gfw_attach(void);   /* v1057W */
static int font_load(void)
{
    if (s_font_state) return s_font_state > 0;
    s_font_state = -1;
    const char* e = getenv("UI_FONT");
    if (e && *e && font_try(e)) { s_font_state = 1; gfw_attach(); return 1; }
    char exe[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (n && n < MAX_PATH) {
        char* sl = strrchr(exe, '\\'); if (sl) *sl = 0;
        static const char* rel[] = { "\\assets\\ui_font.bin", "\\..\\assets\\ui_font.bin", "\\..\\..\\assets\\ui_font.bin" };
        for (int i = 0; i < 3; i++) {
            char p[MAX_PATH + 64]; snprintf(p, sizeof p, "%s%s", exe, rel[i]);
            if (font_try(p)) { s_font_state = 1; gfw_attach(); return 1; }
        }
    }
    fprintf(stderr, "[uiov] no assets\\ui_font.bin (run infamous-recomp\\tools\\extract_ui_font.py) -> MessageBox dialogs\n");
    return 0;
}


/* ---------------- v1057W overlay weapon wheel ("Noir Ring") ----------------
 * assets/gf_wheel.bin (godfather-recomp/tools/gf_wheel_atlas.py): the TEX_REPLACE keys of the game's 38 weapon-wheel
 * textures (slot discs, R / d-pad markers, every weapon icon) + white icon silhouettes. The D3D12 backend tags those
 * textures, reports every draw that uses one (uiov_wheel_note: entry + HUD-space box) and skips it while g_gfw_hide;
 * gfw_build() redraws the same wheel state -- which slots exist, which is selected, which icon sits in each -- as one
 * centred ring with a dimmed world. GF_WHEEL_UI=0 = the game's own wheel. Log [gfw]. */
typedef struct { unsigned key; unsigned short idx; unsigned char kind, pad; unsigned short w, h, ax, ay, aw, ah; char name[24]; } GfwEnt;
static GfwEnt* s_gfw; static int s_gfw_n;
volatile int g_gfw_hide = 0;
static int gfw_attach(void)   /* after the font: wheel icons go below the font atlas (s_ah 1024 -> 2048) */
{
    const char* e = getenv("GF_WHEEL_UI");
    if (e && *e == '0') { fprintf(stderr, "[gfw] v1057W overlay wheel off (GF_WHEEL_UI=0)\n"); return 0; }
    char exe[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH); FILE* f = NULL;
    if (n && n < MAX_PATH) {
        char* sl = strrchr(exe, '\\'); if (sl) *sl = 0;
        static const char* rel[] = { "\\assets\\gf_wheel.bin", "\\..\\assets\\gf_wheel.bin", "\\..\\..\\assets\\gf_wheel.bin" };
        for (int i = 0; i < 3 && !f; i++) { char p[MAX_PATH + 64]; snprintf(p, sizeof p, "%s%s", exe, rel[i]); f = fopen(p, "rb"); }
    }
    if (!f) { fprintf(stderr, "[gfw] no assets\\gf_wheel.bin (tools/gf_wheel_atlas.py) -> game's own weapon wheel\n"); return 0; }
    char mg[8]; unsigned cnt = 0; int ok = 0;
    if (fread(mg, 1, 8, f) == 8 && !memcmp(mg, "GFWHEEL1", 8) && fread(&cnt, 4, 1, f) == 1 && cnt && cnt < 256) {
        s_gfw = (GfwEnt*)calloc(cnt, sizeof(GfwEnt));
        unsigned char* np = (unsigned char*)realloc(s_pix, (size_t)s_aw * 2048 * 4);
        if (s_gfw && np) {
            s_pix = np; ok = 1;
            for (unsigned i = 0; i < cnt && ok; i++) {
                unsigned char r[44];
                if (fread(r, 1, 44, f) != 44) { ok = 0; break; }
                GfwEnt* g = &s_gfw[i];
                memcpy(&g->key, r, 4); memcpy(&g->idx, r + 4, 2); g->kind = r[6];
                memcpy(&g->w, r + 8, 2); memcpy(&g->h, r + 10, 2); memcpy(&g->ax, r + 12, 2); memcpy(&g->ay, r + 14, 2);
                memcpy(&g->aw, r + 16, 2); memcpy(&g->ah, r + 18, 2); memcpy(g->name, r + 20, 24); g->name[23] = 0;
            }
            if (ok && fread(s_pix + (size_t)s_aw * 1024 * 4, 1, (size_t)1024 * 1024 * 4, f) != (size_t)1024 * 1024 * 4) ok = 0;
            if (ok) s_ah = 2048;
        }
    }
    fclose(f);
    if (!ok) { free(s_gfw); s_gfw = NULL; fprintf(stderr, "[gfw] assets\\gf_wheel.bin unreadable -> game's own weapon wheel\n"); return 0; }
    s_gfw_n = (int)cnt; g_gfw_hide = 1;
    fprintf(stderr, "[gfw] v1057W overlay weapon wheel: %d wheel textures known, game's wheel hidden\n", s_gfw_n);
    return 1;
}
int uiov_wheel_size(unsigned w, unsigned h, unsigned fmt)
{
    if (!s_gfw_n || (fmt & 0x9Fu) != 0x87u) return 0;
    return (w == 32 && h == 32) || (w == 64 && h == 32) || (w == 64 && h == 64) || (w == 128 && h == 64) || (w == 128 && h == 128);
}
int uiov_wheel_lookup(unsigned key)
{
    for (int i = 0; i < s_gfw_n; i++) if (s_gfw[i].key == key) return i + 1;
    return 0;
}
typedef struct { int ent; float x0, y0, x1, y1; int pos, vs; } GfwNote;
#define GFW_MAXN 96
static GfwNote s_wn[GFW_MAXN]; static int s_wn_n;
void uiov_wheel_note(int ent, float x0, float y0, float x1, float y1, int pos, int vs)
{
    if (ent <= 0 || ent > s_gfw_n || s_wn_n >= GFW_MAXN) return;
    GfwNote* o = &s_wn[s_wn_n++];
    o->ent = ent - 1; o->x0 = x0; o->y0 = y0; o->x1 = x1; o->y1 = y1; o->pos = pos; o->vs = vs;
}

static const UGlyph* glyph(unsigned code, int icon)
{
    unsigned a = icon ? s_nt : 0, b = icon ? s_nt + s_ni : s_nt;
    for (unsigned i = a; i < b; i++) if (s_gl[i].code == code) return &s_gl[i];
    return NULL;
}

/* ---------------- dialog state ---------------- */
static CRITICAL_SECTION s_cs;
static volatile LONG s_cs_ready;
static void lock(void)
{
    if (s_cs_ready != 2) {
        if (InterlockedCompareExchange(&s_cs_ready, 1, 0) == 0) { InitializeCriticalSection(&s_cs); s_cs_ready = 2; }
        else while (s_cs_ready != 2) Sleep(0);
    }
    EnterCriticalSection(&s_cs);
}
static void unlock(void) { LeaveCriticalSection(&s_cs); }

static struct {
    int active;               /* dialog on screen and owning input */
    int kind;                 /* 1 = OK, 2 = YES/NO */
    int sel;                  /* 0 = first button (OK/Yes), 1 = No */
    int result;               /* 0 pending, 1 OK/YES, 2 NO -- read by the PPU side */
    char msg[2048];
    ULONGLONG t_open, t_close;
    int need_clear;           /* pad latch must be dropped once after the dialog */
    int held;                 /* a dialog key is still held after the answer */
    unsigned prev;            /* input bits last frame (edge detection) */
    float fade_close;         /* alpha the dialog had when it was answered (fade-out) */
    int last_kind, last_sel; char last_msg[2048];
} D;
static volatile ULONGLONG s_last_draw_ms;

int uiov_dialog_available(void)
{
    if (!font_load()) return 0;
    ULONGLONG now = GetTickCount64();
    return s_last_draw_ms && now - s_last_draw_ms < 1500;   /* the renderer is presenting frames */
}

unsigned long long uiov_last_draw_ms(void) { return s_last_draw_ms; }

void uiov_dialog_open(const char* msg, int kind, int default_no)
{
    lock();
    memset(D.msg, 0, sizeof D.msg);
    strncpy(D.msg, msg ? msg : "", sizeof D.msg - 1);
    D.kind = kind; D.sel = (kind == 2 && default_no) ? 1 : 0; D.result = 0;
    D.t_open = GetTickCount64(); D.active = 1; D.prev = 0xFFFFFFFFu;   /* keys already down at open are ignored */
    unlock();
    fprintf(stderr, "[uiov] dialog open (%s): %s\n", kind == 2 ? "yes/no" : "ok", msg ? msg : "");
}

int uiov_dialog_result(void)
{
    lock(); int r = D.result; unlock();
    return r;
}

void uiov_dialog_close(void)
{
    lock();
    if (D.active) { D.active = 0; D.t_close = GetTickCount64(); D.need_clear = 1; D.fade_close = 1.0f;
                    memcpy(D.last_msg, D.msg, sizeof D.msg); D.last_kind = D.kind; D.last_sel = D.sel; }
    D.result = 0;
    unlock();
}

/* 1 = hide the pad from the title, 2 = drop the press latch once then deliver, 0 = normal. */
static volatile int s_menu_open = 0;            /* v194 settings menu (render thread owns it) */
static volatile ULONGLONG s_menu_close_ms = 0;
static volatile int s_menu_need_clear = 0;
volatile int g_uiov_menu_open = 0;              /* read by cellPad.c (free the mouse, no Esc toggle) */
static volatile int s_tr_open = 0;              /* F10 trainer page (render thread owns it) */
static volatile ULONGLONG s_tr_close_ms = 0;
static volatile int s_tr_need_clear = 0;
static volatile int s_sp_page_g = 0;   /* v1092S mirror of s_sp_page for the pad gate (defined later) */
static volatile ULONGLONG s_sp_close_g = 0; static volatile int s_sp_clear_g = 0;
int uiov_pad_gate(void)
{
    { extern volatile int g_gfd_menu, g_gfd_clear, g_gfd_lock; extern volatile unsigned long long g_gfd_close_t;   /* v1106D street-dialogue menu owns the pad; v1107C: the whole conversation */
      if (g_gfd_menu || g_gfd_lock) return 1;
      if (g_gfd_close_t && GetTickCount64() - g_gfd_close_t < 250) return 1;
      if (g_gfd_clear) { g_gfd_clear = 0; return 2; } }
    if (s_sp_page_g) return 1;                   /* v1092S inventory / store page owns the pad */
    if (s_sp_close_g && GetTickCount64() - s_sp_close_g < 250) return 1;
    if (s_sp_clear_g) { s_sp_clear_g = 0; return 2; }
    if (s_menu_open) return 1;                   /* v194: the settings menu owns the pad */
    if (s_tr_open) return 1;                     /* F10 trainer page owns the pad */
    if (s_tr_close_ms && GetTickCount64() - s_tr_close_ms < 250) return 1;
    if (s_tr_need_clear) { s_tr_need_clear = 0; return 2; }
    if (s_menu_close_ms && GetTickCount64() - s_menu_close_ms < 250) return 1;
    if (s_menu_need_clear) { s_menu_need_clear = 0; return 2; }
    if (s_cs_ready != 2) return 0;
    lock();
    int g = 0;
    ULONGLONG now = GetTickCount64();
    if (D.active || (D.held && now - D.t_close < 3000) || (D.t_close && now - D.t_close < 300)) g = 1;
    else if (D.need_clear) { D.need_clear = 0; g = 2; }
    unlock();
    return g;
}

/* ---------------- input ---------------- */
enum { IN_OK = 1, IN_BACK = 2, IN_LEFT = 4, IN_RIGHT = 8, IN_TAB = 16, IN_UP = 32, IN_DOWN = 64, IN_SELECT = 128 };
static unsigned read_input(HWND hwnd)
{
    unsigned b = 0;
    for (DWORD i = 0; i < 4; i++) {
        XINPUT_STATE st; memset(&st, 0, sizeof st);
        if (XInputGetState(i, &st) != ERROR_SUCCESS) continue;
        WORD w = st.Gamepad.wButtons;
        if (w & XINPUT_GAMEPAD_A) b |= IN_OK;
        if (w & XINPUT_GAMEPAD_B) b |= IN_BACK;
        if ((w & XINPUT_GAMEPAD_DPAD_LEFT) || st.Gamepad.sThumbLX < -16000) b |= IN_LEFT;
        if ((w & XINPUT_GAMEPAD_DPAD_RIGHT) || st.Gamepad.sThumbLX > 16000) b |= IN_RIGHT;
        if ((w & XINPUT_GAMEPAD_DPAD_UP) || st.Gamepad.sThumbLY > 16000) b |= IN_UP;
        if ((w & XINPUT_GAMEPAD_DPAD_DOWN) || st.Gamepad.sThumbLY < -16000) b |= IN_DOWN;
        if (w & XINPUT_GAMEPAD_BACK) b |= IN_SELECT;
    }
    { extern volatile unsigned g_pad_file_held;   /* v219: PAD_FILE (unattended bench driver), PS3 button mask */
      const unsigned m = g_pad_file_held;
      if (m & 0x4000) b |= IN_OK;     if (m & 0x2000) b |= IN_BACK;
      if (m & 0x0080) b |= IN_LEFT;   if (m & 0x0020) b |= IN_RIGHT;
      if (m & 0x0010) b |= IN_UP;     if (m & 0x0040) b |= IN_DOWN;
      if (m & 0x0001) b |= IN_SELECT; }
    { extern volatile unsigned g_pad_phys_buttons; extern volatile unsigned long long g_pad_phys_ms; extern volatile int g_input_kbm;   /* v1093M: any controller (SDL too) */
      if (g_input_kbm == 0 && GetTickCount64() - g_pad_phys_ms < 500) { const unsigned m = g_pad_phys_buttons;
          if (m & 0x4000) b |= IN_OK;     if (m & 0x2000) b |= IN_BACK;
          if (m & 0x0080) b |= IN_LEFT;   if (m & 0x0020) b |= IN_RIGHT;
          if (m & 0x0010) b |= IN_UP;     if (m & 0x0040) b |= IN_DOWN;
          if (m & 0x0C00) b |= IN_TAB; } }   /* L1 / R1 = switch page */
    if (hwnd && GetForegroundWindow() == hwnd) {
        #define K(vk) (GetAsyncKeyState(vk) & 0x8000)
        if (K(VK_RETURN) || K(VK_SPACE) || K('Z')) b |= IN_OK;
        if (K(VK_ESCAPE) || K(VK_BACK) || K('X') || K('C')) b |= IN_BACK;
        if (K(VK_LEFT) || K('A')) b |= IN_LEFT;
        if (K(VK_RIGHT) || K('D')) b |= IN_RIGHT;
        if (K(VK_TAB)) b |= IN_TAB;
        if (K(VK_UP) || K('W')) b |= IN_UP;
        if (K(VK_DOWN) || K('S')) b |= IN_DOWN;
        #undef K
    }
    return b;
}

/* ---------------- geometry ---------------- */
static UVert* s_v; static unsigned s_nv;
static float s_k, s_vw;                    /* backbuffer px per virtual px, virtual width */

static void quad_uv(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                    const float c0[4], const float c1[4], int horiz)
{
    if (s_nv + 6 > UIOV_MAX_VERTS) return;
    /* c0 at top/left, c1 at bottom/right */
    const float* tl = c0; const float* tr = horiz ? c1 : c0; const float* bl = horiz ? c0 : c1; const float* br = c1;
    UVert q[4] = {
        { x0, y0, u0, v0, tl[0], tl[1], tl[2], tl[3] }, { x1, y0, u1, v0, tr[0], tr[1], tr[2], tr[3] },
        { x0, y1, u0, v1, bl[0], bl[1], bl[2], bl[3] }, { x1, y1, u1, v1, br[0], br[1], br[2], br[3] } };
    UVert* o = s_v + s_nv;
    o[0] = q[0]; o[1] = q[1]; o[2] = q[2]; o[3] = q[2]; o[4] = q[1]; o[5] = q[3];
    s_nv += 6;
}
static void rect(float x0, float y0, float x1, float y1, const float c0[4], const float c1[4], int horiz)
{
    float u = (s_wx + 0.5f) / s_aw, v = (s_wy + 0.5f) / s_ah;
    quad_uv(x0, y0, x1, y1, u, v, u, v, c0, c1, horiz);
}
static void rgba(float o[4], float r, float g, float b, float a) { o[0] = r; o[1] = g; o[2] = b; o[3] = a; }

static unsigned utf8_next(const unsigned char** p)
{
    const unsigned char* s = *p; unsigned c = *s++;
    if (c >= 0xC0 && c < 0xE0 && (*s & 0xC0) == 0x80) { c = ((c & 0x1F) << 6) | (*s++ & 0x3F); }
    else if (c >= 0xE0 && c < 0xF0 && (s[0] & 0xC0) == 0x80 && (s[1] & 0xC0) == 0x80) { c = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F); s += 2; }
    else if (c >= 0x80) c = '?';
    *p = s; return c;
}
static unsigned map_char(unsigned c)
{
    if (c == 0x2018 || c == 0x2019) return '\'';
    if (c == 0x201C || c == 0x201D) return '"';
    if (c == 0x2013 || c == 0x2014) return '-';
    return c;
}
static float text_width(const char* s, int n, float size)
{
    float w = 0; const unsigned char* p = (const unsigned char*)s; const unsigned char* e = p + n;
    while (p < e && *p) { unsigned c = map_char(utf8_next(&p)); const UGlyph* g = glyph(c, 0); if (!g) g = glyph(' ', 0); if (g) w += g->adv; }
    return w * size / s_tline;
}
static void text_draw(const char* s, int n, float x, float y, float size, const float col[4])
{
    float sc = size / s_tline, pen = x; const unsigned char* p = (const unsigned char*)s; const unsigned char* e = p + n;
    while (p < e && *p) {
        unsigned c = map_char(utf8_next(&p)); const UGlyph* g = glyph(c, 0);
        if (!g) g = glyph(' ', 0);
        if (!g) continue;
        if (g->w > 2 && g->h > 2) {
            float x0 = pen + g->xo * sc, y0 = y + g->yo * sc;
            quad_uv(x0, y0, x0 + g->w * sc, y0 + g->h * sc,
                    (float)g->x / s_aw, (float)g->y / s_ah, (float)(g->x + g->w) / s_aw, (float)(g->y + g->h) / s_ah, col, col, 0);
        }
        pen += g->adv * sc;
    }
}
static float icon_draw(unsigned code, float x, float y, float size, float alpha)
{
    const UGlyph* g = glyph(code, 1); if (!g) return 0;
    float sc = size / s_iline, c[4]; rgba(c, 1, 1, 1, alpha);
    quad_uv(x, y, x + g->w * sc, y + g->h * sc, (float)g->x / s_aw, (float)g->y / s_ah,
            (float)(g->x + g->w) / s_aw, (float)(g->y + g->h) / s_ah, c, c, 0);
    return g->adv * sc;
}

/* word-wrap into at most 12 lines: (start, len) pairs */
static int wrap(const char* msg, float size, float maxw, int st[12], int ln[12])
{
    int n = 0, len = (int)strlen(msg), i = 0;
    while (i < len && n < 12) {
        while (i < len && msg[i] == ' ') i++;
        int start = i, last_ok = -1, j = i;
        while (j <= len) {
            if (j == len || msg[j] == ' ' || msg[j] == '\n') {
                if (last_ok >= 0 && text_width(msg + start, j - start, size) > maxw) break;
                last_ok = j;
                if (j == len || msg[j] == '\n') break;
            }
            j++;
        }
        if (last_ok < 0) last_ok = j;
        st[n] = start; ln[n] = last_ok - start; n++;
        i = last_ok + ((last_ok < len && (msg[last_ok] == '\n' || msg[last_ok] == ' ')) ? 1 : 0);
    }
    return n;
}

typedef struct { float x0, y0, x1, y1; } BtnRect;
static BtnRect s_br[2];          /* last drawn button rects (mouse hit test), render thread only */
static int s_nb;

/* Builds the dialog; returns the button hit rectangles (virtual px). */
static int build_dialog(const char* msg, int kind, int sel, float alpha, float grow, BtnRect* br)
{
    const float cx = s_vw * 0.5f, cy = UIOV_VW_H * 0.5f;
    float c0[4], c1[4];
    static const float BLUE[3] = { 0.36f, 0.72f, 1.0f };
    /* dim the scene */
    rgba(c0, 0, 0, 0, 0.45f * alpha); rect(0, 0, s_vw, UIOV_VW_H, c0, c0, 0);
    float size = 30.0f, lh = 40.0f; int st[12], ln[12];
    int nl = wrap(msg, size, 720.0f, st, ln);
    if (nl > 5) { size = 25.0f; lh = 33.0f; nl = wrap(msg, size, 760.0f, st, ln); }
    float bh = (nl * lh + 110.0f) * grow, y0 = cy - bh * 0.5f, y1 = cy + bh * 0.5f, fade = 36.0f;
    /* the band, with soft top/bottom edges */
    rgba(c0, 0, 0, 0, 0); rgba(c1, 0, 0, 0, 0.80f * alpha);
    rect(0, y0 - fade, s_vw, y0, c0, c1, 0);
    rect(0, y0, s_vw, y1, c1, c1, 0);
    rect(0, y1, s_vw, y1 + fade, c1, c0, 0);
    /* electric edge lines: glow + core, fading out toward the ends */
    float lx0 = cx - 500.0f, lx1 = cx + 500.0f, ys[2] = { y0, y1 - 2.0f };
    for (int i = 0; i < 2; i++) {
        float y = ys[i];
        rgba(c0, BLUE[0], BLUE[1], BLUE[2], 0); rgba(c1, BLUE[0], BLUE[1], BLUE[2], 0.13f * alpha);
        rect(lx0, y - 3, cx, y + 5, c0, c1, 1); rect(cx, y - 3, lx1, y + 5, c1, c0, 1);
        rgba(c1, BLUE[0], BLUE[1], BLUE[2], 0.95f * alpha);
        rect(lx0, y, cx, y + 2, c0, c1, 1); rect(cx, y, lx1, y + 2, c1, c0, 1);
    }
    if (grow < 0.999f) return 0;
    /* message */
    float ty = y0 + 28.0f; float white[4]; rgba(white, 0.96f, 0.96f, 0.96f, alpha);
    for (int i = 0; i < nl; i++) {
        float w = text_width(msg + st[i], ln[i], size);
        text_draw(msg + st[i], ln[i], cx - w * 0.5f, ty - size * 0.18f, size, white);
        ty += lh;
    }
    /* buttons */
    const float isz = 30.0f, gap = 60.0f, lsz = 26.0f;
    int nb = kind == 2 ? 2 : 1;
    const char* lab[2] = { kind == 2 ? "Yes" : "OK", "No" };
    unsigned ico[2] = { 0xF083u, 0xF080u };            /* cross, circle */
    float bw[2], tot = 0;
    for (int i = 0; i < nb; i++) { bw[i] = isz + 10.0f + text_width(lab[i], (int)strlen(lab[i]), lsz); tot += bw[i]; }
    tot += gap * (nb - 1);
    float x = cx - tot * 0.5f, by = y1 - 62.0f;
    for (int i = 0; i < nb; i++) {
        int on = (i == sel);
        if (on && nb > 1) { rgba(c0, BLUE[0], BLUE[1], BLUE[2], 0.9f * alpha); rect(x - 14, by + isz + 8, x + bw[i] + 14, by + isz + 10, c0, c0, 0); }
        icon_draw(ico[i], x, by, isz, alpha);
        float col[4]; if (on || nb == 1) rgba(col, 1, 1, 1, alpha); else rgba(col, 0.62f, 0.62f, 0.62f, alpha);
        text_draw(lab[i], (int)strlen(lab[i]), x + isz + 10.0f, by + isz * 0.5f - lsz * 0.5f - lsz * 0.12f, lsz, col);
        if (br) { br[i].x0 = x - 14; br[i].y0 = by - 6; br[i].x1 = x + bw[i] + 14; br[i].y1 = by + isz + 12; }
        x += bw[i] + gap;
    }
    return nb;
}

/* ---------------- D3D12 ---------------- */
static int s_gfx_state;                     /* 0 untried, 1 ok, -1 failed */
static ID3D12RootSignature* s_rs;
static ID3D12PipelineState* s_pso;
static ID3D12DescriptorHeap* s_heap;
static ID3D12Resource* s_tex;
static ID3D12Resource* s_tex_up;
static int s_tex_uploaded;
static ID3D12Resource* s_vb[UIOV_FRAMES];
ID3D12Resource* volatile g_uiov_bb = 0;              /* v1057Y: the backbuffer, set by the backend before uiov_render */
static ID3D12PipelineState* s_gpso;                 /* v1057Y wheel grey-out (gfw_grey) */
static ID3D12DescriptorHeap* s_gheap;
static ID3D12Resource* s_gvb; static UVert* s_gvbmap;
static UVert* s_vbmap[UIOV_FRAMES];
static unsigned s_vbi;

static int gfx_init(ID3D12Device* dev, DXGI_FORMAT rtfmt)
{
    if (s_gfx_state) return s_gfx_state > 0;
    s_gfx_state = -1;
    HRESULT hr;
    /* root signature: 4 VS constants (scale/offset), one SRV table, static linear-clamp sampler */
    D3D12_DESCRIPTOR_RANGE rg = {0};
    rg.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg.NumDescriptors = 1;
    rg.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER rp[2]; memset(rp, 0, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.Num32BitValues = 4;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &rg;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sd = {0};
    sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sd.MaxLOD = D3D12_FLOAT32_MAX; sd.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd = {0};
    rd.NumParameters = 2; rd.pParameters = rp; rd.NumStaticSamplers = 1; rd.pStaticSamplers = &sd;
    rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob *sig = NULL, *err = NULL;
    hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) { fprintf(stderr, "[uiov] root signature serialize failed\n"); if (err) err->lpVtbl->Release(err); return 0; }
    hr = dev->lpVtbl->CreateRootSignature(dev, 0, sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig),
                                         &IID_ID3D12RootSignature, (void**)&s_rs);
    sig->lpVtbl->Release(sig);
    if (FAILED(hr)) { fprintf(stderr, "[uiov] root signature failed 0x%08lX\n", hr); return 0; }

    static const char vs[] =
        "cbuffer c : register(b0) { float4 so; };\n"
        "struct I { float2 p:POSITION; float2 t:TEXCOORD; float4 c:COLOR; };\n"
        "struct O { float4 p:SV_POSITION; float2 t:TEXCOORD; float4 c:COLOR; };\n"
        "O main(I i) { O o; o.p = float4(i.p.x * so.x - 1.0, 1.0 - i.p.y * so.y, 0, 1); o.t = i.t; o.c = i.c; return o; }\n";
    static const char ps[] =
        "Texture2D tx : register(t0); SamplerState sm : register(s0);\n"
        "struct O { float4 p:SV_POSITION; float2 t:TEXCOORD; float4 c:COLOR; };\n"
        "float4 main(O i) : SV_TARGET { return tx.Sample(sm, i.t) * i.c; }\n";
    ID3DBlob *vb = NULL, *pb = NULL;
    hr = D3DCompile(vs, sizeof vs - 1, "uiov_vs", NULL, NULL, "main", "vs_5_0", 0, 0, &vb, &err);
    if (SUCCEEDED(hr)) hr = D3DCompile(ps, sizeof ps - 1, "uiov_ps", NULL, NULL, "main", "ps_5_0", 0, 0, &pb, &err);
    if (FAILED(hr)) { fprintf(stderr, "[uiov] shader compile failed: %s\n", err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?"); return 0; }
    D3D12_INPUT_ELEMENT_DESC il[3] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 8,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
    pd.pRootSignature = s_rs;
    pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb); pd.VS.BytecodeLength = vb->lpVtbl->GetBufferSize(vb);
    pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb); pd.PS.BytecodeLength = pb->lpVtbl->GetBufferSize(pb);
    pd.InputLayout.pInputElementDescs = il; pd.InputLayout.NumElements = 3;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    D3D12_RENDER_TARGET_BLEND_DESC* bd = &pd.BlendState.RenderTarget[0];
    bd->BlendEnable = TRUE; bd->SrcBlend = D3D12_BLEND_SRC_ALPHA; bd->DestBlend = D3D12_BLEND_INV_SRC_ALPHA; bd->BlendOp = D3D12_BLEND_OP_ADD;
    bd->SrcBlendAlpha = D3D12_BLEND_ONE; bd->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; bd->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    bd->LogicOp = D3D12_LOGIC_OP_NOOP; bd->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX; pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = rtfmt; pd.SampleDesc.Count = 1;
    hr = dev->lpVtbl->CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void**)&s_pso);
    { /* v1057Y wheel grey-out: same layout, opaque, PS desaturates the copied frame (c.r) and darkens it (c.g) */
      static const char gps[] =
          "Texture2D tx : register(t0); SamplerState sm : register(s0);\n"
          "struct O { float4 p:SV_POSITION; float2 t:TEXCOORD; float4 c:COLOR; };\n"
          "float4 main(O i) : SV_TARGET { float3 c = tx.Sample(sm, i.t).rgb; float l = dot(c, float3(0.299, 0.587, 0.114));\n"
          "  float3 g = lerp(c, l.xxx * float3(1.0, 0.96, 0.90), i.c.r); return float4(g * (1.0 - i.c.g), 1.0); }\n";
      ID3DBlob* gb = NULL; ID3DBlob* ge = NULL;
      if (SUCCEEDED(hr) && SUCCEEDED(D3DCompile(gps, sizeof gps - 1, "uiov_grey_ps", NULL, NULL, "main", "ps_5_0", 0, 0, &gb, &ge))) {
          D3D12_GRAPHICS_PIPELINE_STATE_DESC gd = pd;
          gd.PS.pShaderBytecode = gb->lpVtbl->GetBufferPointer(gb); gd.PS.BytecodeLength = gb->lpVtbl->GetBufferSize(gb);
          gd.BlendState.RenderTarget[0].BlendEnable = FALSE;
          if (FAILED(dev->lpVtbl->CreateGraphicsPipelineState(dev, &gd, &IID_ID3D12PipelineState, (void**)&s_gpso))) s_gpso = NULL;
          gb->lpVtbl->Release(gb);
      } else fprintf(stderr, "[gfw] grey shader compile failed: %s\n", ge ? (const char*)ge->lpVtbl->GetBufferPointer(ge) : "?");
      if (ge) ge->lpVtbl->Release(ge); }
    vb->lpVtbl->Release(vb); pb->lpVtbl->Release(pb);
    if (FAILED(hr)) { fprintf(stderr, "[uiov] PSO failed 0x%08lX\n", hr); return 0; }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->lpVtbl->CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_heap))) { fprintf(stderr, "[uiov] heap failed\n"); return 0; }

    D3D12_HEAP_PROPERTIES hpd = {0}, hpu = {0}; hpd.Type = D3D12_HEAP_TYPE_DEFAULT; hpu.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = s_aw; td.Height = s_ah; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hpd, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                    &IID_ID3D12Resource, (void**)&s_tex))) { fprintf(stderr, "[uiov] texture failed\n"); return 0; }
    D3D12_RESOURCE_DESC bdsc = {0};
    bdsc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bdsc.Width = (UINT64)s_aw * s_ah * 4; bdsc.Height = 1; bdsc.DepthOrArraySize = 1;
    bdsc.MipLevels = 1; bdsc.SampleDesc.Count = 1; bdsc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hpu, D3D12_HEAP_FLAG_NONE, &bdsc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                    &IID_ID3D12Resource, (void**)&s_tex_up))) { fprintf(stderr, "[uiov] upload failed\n"); return 0; }
    void* m = NULL; D3D12_RANGE nr = { 0, 0 };
    if (FAILED(s_tex_up->lpVtbl->Map(s_tex_up, 0, &nr, &m))) return 0;
    memcpy(m, s_pix, (size_t)s_aw * s_ah * 4);     /* 1024 * 4 = 4096-byte rows: already 256-aligned */
    s_tex_up->lpVtbl->Unmap(s_tex_up, 0, NULL);
    free(s_pix); s_pix = NULL;

    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE ch; RSX_D3D12_CPU_HEAP_START(s_heap)(s_heap, &ch);
    dev->lpVtbl->CreateShaderResourceView(dev, s_tex, &sv, ch);

    bdsc.Width = (UINT64)UIOV_MAX_VERTS * sizeof(UVert);
    for (int i = 0; i < UIOV_FRAMES; i++) {
        if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hpu, D3D12_HEAP_FLAG_NONE, &bdsc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                        &IID_ID3D12Resource, (void**)&s_vb[i]))) { fprintf(stderr, "[uiov] vb failed\n"); return 0; }
        if (FAILED(s_vb[i]->lpVtbl->Map(s_vb[i], 0, &nr, (void**)&s_vbmap[i]))) return 0;
    }
    if (s_gpso) {   /* v1057Y grey-out: own SRV heap + a small vertex ring */
        D3D12_DESCRIPTOR_HEAP_DESC gh = {0};
        gh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; gh.NumDescriptors = 1; gh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        bdsc.Width = (UINT64)UIOV_FRAMES * 6 * sizeof(UVert);
        if (FAILED(dev->lpVtbl->CreateDescriptorHeap(dev, &gh, &IID_ID3D12DescriptorHeap, (void**)&s_gheap)) ||
            FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hpu, D3D12_HEAP_FLAG_NONE, &bdsc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                        &IID_ID3D12Resource, (void**)&s_gvb)) ||
            FAILED(s_gvb->lpVtbl->Map(s_gvb, 0, &nr, (void**)&s_gvbmap))) { s_gpso = NULL; s_gvbmap = NULL; }
    }
    s_gfx_state = 1;
    fprintf(stderr, "[uiov] in-game dialog renderer ready\n");
    return 1;
}

/* ===========================================================================
 * v1057B blood decals (Godfather). gf_ragdoll.c places them on the level collision (gfd_add, any thread); the backend
 * calls gfd_render right after the frame's last 3D scene draw, with the scene's colour + depth targets still bound, the
 * scene's view-projection (VS constants c0..c3: clip = x c0 + y c1 + z c2 + c3, world space) and output epilogue.
 * Depth-tested (no write) against the game's own depth -> hidden behind walls / people, under the HUD.
 * Texture: assets/gf_blood.bin (the game's own bloodspray / bloodsplats3 / bloodsplats / blood1, 2x2 atlas, built from
 * godfather_global_global.str). GF_BLOOD=0 = off. Log [blood].
 * =========================================================================*/
typedef struct { float x, y, z, u, v, r, g, b, a; } DVert;
typedef struct { float p[3], n[3], t[3], w, h, alpha; int sprite; ULONGLONG born; float grow; } GfDecal;
#define GFD_MAX 2048   /* v1099B (user: blood should stay longer): was 192 -- every landed droplet takes a slot, the ring wrapped in seconds */
#define GFD_RING 8   /* v1090C: decal-pass ring (VBs + depth SRVs): 2 calls per frame now (decals, held prop) */
/* v1093M cigarette smoke (gf_smoke.c gfs_add): soft grey camera-facing puffs drawn in the held-prop pass (after the characters,
 * hidden where the scene is nearer). Each rises (buoyancy), slows (drag), sways, grows and fades over its life. */
#define SMK_MAX 640
typedef struct { float p[3], v[3], s0, s1, life, a0, ph; ULONGLONG born; int alive; } GfSmoke;
static GfSmoke s_smk[SMK_MAX]; static int s_smk_head; static volatile int s_smk_live;
static GfDecal s_gfd[GFD_MAX]; static int s_gfd_n, s_gfd_head;
/* v1117A blood ON bodies (gf_ragdoll.c wound_tick -> gfb_set every skeleton update of a wounded body): a splat projected onto
 * whatever is drawn within 7 cm of the wound's tangent plane (psz shader, vertex g = 2). Entries not updated for 250 ms
 * (body out of the update / wound gone) are not drawn. */
typedef struct { float p[3], n[3], t[3], size, alpha; int sprite; ULONGLONG upd; } GfBody;
#define GFB_MAX 64
static GfBody s_gfb[GFB_MAX]; static volatile ULONGLONG s_gfb_last;
static CRITICAL_SECTION s_gfd_cs; static volatile LONG s_gfd_cs_ok;
/* v1088M held props (Godfather gf_smoke.c): a small textured mesh (assets/gf_cigarette.bin, "GFMESH1": nv, ni, nv x
 * {pos3 normal3 uv2} in metres, ni x u16, w, h, RGBA) drawn in the decal pass at a world matrix the game thread sets each
 * frame with gfm_set (rows: x axis, y axis, z axis, position). Drawn before the characters -> the hand / head drawn later
 * cover it; hidden behind the level by the same scene-depth test as the blood particles. Model: "Cigarette Low Poly" by
 * pixelAlp (sketchfab), CC-BY-4.0 -- see assets/CREDITS.txt. */
#define GM_MAXV 4096
static int s_gm_nv, s_gm_ni; static float* s_gm_v; static unsigned short* s_gm_i; static unsigned char* s_gm_tex; static int s_gm_tw, s_gm_th;
static ID3D12Resource *s_gm_res, *s_gm_up; static int s_gm_up_done, s_gm_state;   /* state: 0 not tried, 1 ok, -1 failed */
static float s_gm_m[12]; static volatile int s_gm_on, s_gm_dbg;   /* v1089L: id 1 = UV debug colours */
#define GM_RING 16
static struct { unsigned tag; int used, on; float m[12]; } s_gm_r[GM_RING]; static int s_gm_rh; static volatile LONG s_gm_rlock;   /* v1093S */
int gfm_on(void) { return s_gm_on; }
void gfm_set(int id, const float* m12, int on)
{
    s_gm_dbg = (id == 1);
    if (on && m12) { for (int i = 0; i < 12; i++) if (!(m12[i] == m12[i]) || m12[i] > 1e6f || m12[i] < -1e6f) on = 0; }
    if (on) { memcpy(s_gm_m, m12, sizeof s_gm_m); }
    s_gm_on = on;
    {   /* v1093S frame sync: remember this frame's matrix under the game's flip-request count (game thread, post-skeleton) */
        extern unsigned cellGcm_flip_request_count(void);
        const unsigned tag = cellGcm_flip_request_count();
        while (InterlockedExchange(&s_gm_rlock, 1)) Sleep(0);
        int k = -1; for (int i = 0; i < GM_RING; i++) if (s_gm_r[i].used && s_gm_r[i].tag == tag) { k = i; break; }
        if (k < 0) { k = s_gm_rh; s_gm_rh = (s_gm_rh + 1) % GM_RING; }
        s_gm_r[k].used = 1; s_gm_r[k].tag = tag; s_gm_r[k].on = on; if (on) memcpy(s_gm_r[k].m, m12, sizeof s_gm_r[k].m);
        InterlockedExchange(&s_gm_rlock, 0);
    }
}
/* v1093S (user: the cigarette "doesn't move with his mouth / hand when I walk"): the game thread places it while building frame
 * F, the backend replays frame F's draws later -- the newest matrix was drawn on an older body. The renderer now takes the
 * matrix recorded for the frame it is drawing: tag == g_gcm_walked_req + GF_CIG_SYNC (default -1: the walker has passed frame
 * F's own flip when its draws are replayed). No exact match -> the newest one not newer than that. GF_CIG_SYNC=off = newest. */
static int gm_pick(float* m)
{
    static int ofs = 0x7fff; if (ofs == 0x7fff) { const char* e = getenv("GF_CIG_SYNC"); ofs = (e && *e == 'o') ? 0x7ffe : (e && *e ? atoi(e) : -1); }
    if (ofs == 0x7ffe) { memcpy(m, s_gm_m, 48); return s_gm_on; }
    extern volatile unsigned g_gcm_walked_req;
    const unsigned want = g_gcm_walked_req + (unsigned)ofs;
    int best = -1, newest = -1, on = 0;
    while (InterlockedExchange(&s_gm_rlock, 1)) Sleep(0);
    for (int i = 0; i < GM_RING; i++) { if (!s_gm_r[i].used) continue;
        if (newest < 0 || (int)(s_gm_r[i].tag - s_gm_r[newest].tag) > 0) newest = i;
        if ((int)(s_gm_r[i].tag - want) <= 0 && (best < 0 || (int)(s_gm_r[i].tag - s_gm_r[best].tag) > 0)) best = i; }
    if (best < 0) best = newest;
    if (best >= 0) { memcpy(m, s_gm_r[best].m, 48); on = s_gm_r[best].on; }
    const unsigned bt = best >= 0 ? s_gm_r[best].tag : 0, nt = newest >= 0 ? s_gm_r[newest].tag : 0;
    InterlockedExchange(&s_gm_rlock, 0);
    { static ULONGLONG t; const ULONGLONG nw = GetTickCount64(); static int nl = 0;
      if (nl < 40 && nw - t > 2000) { t = nw; nl++; fprintf(stderr, "[prop] v1093S sync: drawing frame %u (walked %u %+d), newest placed %u, used %u%c", want, g_gcm_walked_req, ofs, nt, bt, 10); } }
    return on && best >= 0;
}
static int gm_load(void)
{
    if (s_gm_state) return s_gm_state > 0;
    s_gm_state = -1;
    char exe[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH); FILE* f = NULL;
    if (n && n < MAX_PATH) { char* sl = strrchr(exe, '\\'); if (sl) *sl = 0;
        static const char* rel[] = { "\\assets\\gf_cigarette.bin", "\\..\\assets\\gf_cigarette.bin", "\\..\\..\\assets\\gf_cigarette.bin" };
        for (int i = 0; i < 3 && !f; i++) { char pth[MAX_PATH + 64]; snprintf(pth, sizeof pth, "%s%s", exe, rel[i]); f = fopen(pth, "rb"); } }
    if (!f) { fprintf(stderr, "[prop] no assets\\gf_cigarette.bin -> no cigarette mesh\n"); return 0; }
    char mg[8]; unsigned hd[2] = { 0, 0 }, td[2] = { 0, 0 }; int okr = 0;
    if (fread(mg, 1, 8, f) == 8 && !memcmp(mg, "GFMESH1", 7) && fread(hd, 4, 2, f) == 2 && hd[0] > 0 && hd[0] <= 65535 && hd[1] > 0 && hd[1] * 1u <= GM_MAXV && hd[1] % 3 == 0) {
        s_gm_v = (float*)malloc(hd[0] * 8 * sizeof(float)); s_gm_i = (unsigned short*)malloc(hd[1] * 2);
        if (s_gm_v && s_gm_i && fread(s_gm_v, 4, hd[0] * 8, f) == hd[0] * 8 && fread(s_gm_i, 2, hd[1], f) == hd[1] && fread(td, 4, 2, f) == 2 &&
            td[0] > 0 && td[0] <= 2048 && td[1] > 0 && td[1] <= 2048) {
            s_gm_tex = (unsigned char*)malloc((size_t)td[0] * td[1] * 4);
            if (s_gm_tex && fread(s_gm_tex, 4, (size_t)td[0] * td[1], f) == (size_t)td[0] * td[1]) okr = 1;
        }
    }
    fclose(f);
    if (!okr) { fprintf(stderr, "[prop] assets\\gf_cigarette.bin unreadable\n"); return 0; }
    for (unsigned i = 0; i < hd[1]; i++) if (s_gm_i[i] >= hd[0]) { fprintf(stderr, "[prop] gf_cigarette.bin: bad index\n"); return 0; }
    s_gm_nv = (int)hd[0]; s_gm_ni = (int)hd[1]; s_gm_tw = (int)td[0]; s_gm_th = (int)td[1];
    s_gm_state = 1; fprintf(stderr, "[prop] v1088M cigarette mesh: %d vertices, %d triangles, texture %dx%d\n", s_gm_nv, s_gm_ni / 3, s_gm_tw, s_gm_th);
    return 1;
}
static void gfd_lock(void)
{
    if (s_gfd_cs_ok != 2) { if (InterlockedCompareExchange(&s_gfd_cs_ok, 1, 0) == 0) { InitializeCriticalSection(&s_gfd_cs); s_gfd_cs_ok = 2; } else while (s_gfd_cs_ok != 2) Sleep(0); }
    EnterCriticalSection(&s_gfd_cs);
}
static int gfd_on(void) { static int on = -1; if (on < 0) { const char* e = getenv("GF_BLOOD"); on = (e && *e == '0') ? 0 : 1; } return on; }
/* p = centre on the surface, n = surface normal, t = the decal's long axis (in the surface plane), w x h metres,
 * sprite 0 spray / 1 droplets / 2 splat / 3 drops, grow = seconds to grow in (0 = at once). */
void gfd_add(const float* p, const float* n, const float* t, float w, float h, int sprite, float alpha, float grow)
{
    if (!gfd_on()) return;
    gfd_lock();
    GfDecal* d = &s_gfd[s_gfd_head]; s_gfd_head = (s_gfd_head + 1) % GFD_MAX; if (s_gfd_n < GFD_MAX) s_gfd_n++;
    memcpy(d->p, p, 12); memcpy(d->n, n, 12); memcpy(d->t, t, 12); d->w = w; d->h = h; d->alpha = alpha; d->sprite = sprite & 3;
    d->born = GetTickCount64(); d->grow = grow;
    LeaveCriticalSection(&s_gfd_cs);
    static int nlog = 0; if (nlog < 40) { nlog++; fprintf(stderr, "[blood] v1057B decal %d at (%.2f %.2f %.2f) n (%.2f %.2f %.2f) %.2fx%.2f m\n", sprite, p[0], p[1], p[2], n[0], n[1], n[2], w, h); }
}
void gfb_set(int i, const float* p, const float* n, const float* t, float size, float alpha, int sprite)   /* v1117A, game thread */
{
    if (!gfd_on() || i < 0 || i >= GFB_MAX) return;
    gfd_lock();
    GfBody* b = &s_gfb[i]; memcpy(b->p, p, 12); memcpy(b->n, n, 12); memcpy(b->t, t, 12); b->size = size; b->alpha = alpha; b->sprite = sprite & 3;
    b->upd = GetTickCount64(); s_gfb_last = b->upd;
    LeaveCriticalSection(&s_gfd_cs);
}
/* v1069B blood particles (spray / arterial spurts): ballistic droplets, drawn as streaks along their velocity in the
 * decal pass (same root signature / PSO; vertex colour g = -1 marks a particle (decals have g = 1): round soft blob, hidden where the scene is
 * in front). A droplet that reaches its ground height (given by the caller) leaves a small splat (gfd_add). */
typedef struct { float p[3], v[3], size, ground, life; ULONGLONG born; int alive; } GfPart;
#define GFP_MAX 1024
static GfPart s_gfp[GFP_MAX]; static int s_gfp_head, s_gfp_live;
static long s_gfp_total, s_gfp_landed;
void gfp_add(const float* p, const float* v, float size, float ground, float life)
{
    if (!gfd_on()) return;
    gfd_lock();
    GfPart* q = &s_gfp[s_gfp_head]; s_gfp_head = (s_gfp_head + 1) % GFP_MAX;
    if (!q->alive) s_gfp_live++;
    memcpy(q->p, p, 12); memcpy(q->v, v, 12); q->size = size; q->ground = ground; q->life = life > 0.1f ? life : 2.5f;
    q->born = GetTickCount64(); q->alive = 1; s_gfp_total++;
    LeaveCriticalSection(&s_gfd_cs);
}
static ID3D12RootSignature* s_dsig; static ID3D12PipelineState* s_dpso[4]; static DXGI_FORMAT s_dpso_fmt[4]; static int s_dpso_z[4];
static ID3D12DescriptorHeap* s_dheap; static ID3D12Resource *s_dtex, *s_dtex_up, *s_dvb[GFD_RING]; static DVert* s_dvbmap[GFD_RING]; static unsigned s_dvbi;
static int s_dstate, s_dtex_up_done;
int gfd_pending(void) { return ((gfd_on() && (s_gfd_n > 0 || s_gfp_live > 0 || GetTickCount64() - s_gfb_last < 500)) || s_gm_on || s_smk_live > 0) && s_dstate >= 0; }   /* v1117A + body blood */   /* v1088M: + a visible held prop; v1093M + smoke */
void gfs_add(const float* p, const float* v, float s0, float s1, float life, float alpha)   /* v1093M: game thread */
{
    gfd_lock();
    GfSmoke* q = &s_smk[s_smk_head]; s_smk_head = (s_smk_head + 1) % SMK_MAX;
    if (!q->alive) s_smk_live++;
    memcpy(q->p, p, 12); memcpy(q->v, v, 12); q->s0 = s0; q->s1 = s1; q->life = life > 0.2f ? life : 2.0f; q->a0 = alpha;
    q->ph = (float)(rand() % 6283) / 1000.0f; q->born = GetTickCount64(); q->alive = 1;
    LeaveCriticalSection(&s_gfd_cs);
}
static int gfd_init(ID3D12Device* dev)
{
    if (s_dstate) return s_dstate > 0;
    s_dstate = -1;
    char exe[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH); FILE* f = NULL;
    if (n && n < MAX_PATH) { char* sl = strrchr(exe, '\\'); if (sl) *sl = 0;
        static const char* rel[] = { "\\assets\\gf_blood.bin", "\\..\\assets\\gf_blood.bin", "\\..\\..\\assets\\gf_blood.bin" };
        for (int i = 0; i < 3 && !f; i++) { char p[MAX_PATH + 64]; snprintf(p, sizeof p, "%s%s", exe, rel[i]); f = fopen(p, "rb"); } }
    if (!f) { fprintf(stderr, "[blood] no assets\\gf_blood.bin -> no blood decals\n"); return 0; }
    unsigned char* pix = (unsigned char*)malloc(256 * 256 * 4); char mg[8];
    const int ok = pix && fread(mg, 1, 8, f) == 8 && !memcmp(mg, "GFBLOOD1", 8) && fread(pix, 1, 256 * 256 * 4, f) == 256 * 256 * 4;
    fclose(f);
    if (!ok) { free(pix); fprintf(stderr, "[blood] assets\\gf_blood.bin unreadable\n"); return 0; }
    D3D12_DESCRIPTOR_RANGE rg = {0};
    rg.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg.NumDescriptors = 1; rg.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_DESCRIPTOR_RANGE rg2 = rg; rg2.BaseShaderRegister = 1;   /* v1057B-12: t1 = the scene depth */
    D3D12_ROOT_PARAMETER rp[3]; memset(rp, 0, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.Num32BitValues = 28; rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[2].DescriptorTable.NumDescriptorRanges = 1; rp[2].DescriptorTable.pDescriptorRanges = &rg2;
    rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &rg;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sd = {0};
    sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sd.MaxLOD = D3D12_FLOAT32_MAX; sd.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd = {0};
    rd.NumParameters = 3; rd.pParameters = rp; rd.NumStaticSamplers = 1; rd.pStaticSamplers = &sd; rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob *sig = NULL, *err = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(dev->lpVtbl->CreateRootSignature(dev, 0, sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig), &IID_ID3D12RootSignature, (void**)&s_dsig))) {
        fprintf(stderr, "[blood] root signature failed\n"); free(pix); return 0; }
    sig->lpVtbl->Release(sig);
    D3D12_DESCRIPTOR_HEAP_DESC hd = {0}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 2 + GFD_RING;   /* v1088M: + the held-prop texture (last slot) */ hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D12_HEAP_PROPERTIES hpd = {0}, hpu = {0}; hpd.Type = D3D12_HEAP_TYPE_DEFAULT; hpu.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = 256; td.Height = 256; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 256 * 256 * 4; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->lpVtbl->CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_dheap)) ||
        FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hpd, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&s_dtex)) ||
        FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hpu, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&s_dtex_up))) {
        fprintf(stderr, "[blood] texture failed\n"); free(pix); return 0; }
    { void* m = NULL; D3D12_RANGE nr = { 0, 0 }; if (FAILED(s_dtex_up->lpVtbl->Map(s_dtex_up, 0, &nr, &m))) { free(pix); return 0; }
      memcpy(m, pix, 256 * 256 * 4); s_dtex_up->lpVtbl->Unmap(s_dtex_up, 0, NULL); free(pix); }
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE ch; RSX_D3D12_CPU_HEAP_START(s_dheap)(s_dheap, &ch);
    dev->lpVtbl->CreateShaderResourceView(dev, s_dtex, &sv, ch);
    if (gm_load()) {   /* v1088M: held-prop texture -> SRV in slot 1 + GFD_RING */
        D3D12_RESOURCE_DESC mt = td; mt.Width = (UINT64)s_gm_tw; mt.Height = (UINT)s_gm_th;
        D3D12_RESOURCE_DESC mb = bd; mb.Width = (UINT64)s_gm_tw * s_gm_th * 4;
        if (SUCCEEDED(dev->lpVtbl->CreateCommittedResource(dev, &hpd, D3D12_HEAP_FLAG_NONE, &mt, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&s_gm_res)) &&
            SUCCEEDED(dev->lpVtbl->CreateCommittedResource(dev, &hpu, D3D12_HEAP_FLAG_NONE, &mb, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&s_gm_up))) {
            void* m = NULL; D3D12_RANGE nr = { 0, 0 };
            if (SUCCEEDED(s_gm_up->lpVtbl->Map(s_gm_up, 0, &nr, &m))) { memcpy(m, s_gm_tex, (size_t)s_gm_tw * s_gm_th * 4); s_gm_up->lpVtbl->Unmap(s_gm_up, 0, NULL); }
            const UINT inc = dev->lpVtbl->GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_CPU_DESCRIPTOR_HANDLE mh = ch; mh.ptr += (SIZE_T)(1 + GFD_RING) * inc;
            dev->lpVtbl->CreateShaderResourceView(dev, s_gm_res, &sv, mh);
        } else { s_gm_state = -1; fprintf(stderr, "[prop] texture failed\n"); }
    }
    bd.Width = (UINT64)(GFD_MAX + GFP_MAX + GFB_MAX) * 6 * sizeof(DVert) + (UINT64)GM_MAXV * sizeof(DVert) + (UINT64)SMK_MAX * 6 * sizeof(DVert);   /* v1117A + body blood */   /* v1093M + smoke */   /* v1069B: + particles; v1088M: + held prop */
    for (int i = 0; i < GFD_RING; i++) {
        D3D12_RANGE nr = { 0, 0 };
        if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hpu, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&s_dvb[i])) ||
            FAILED(s_dvb[i]->lpVtbl->Map(s_dvb[i], 0, &nr, (void**)&s_dvbmap[i]))) { fprintf(stderr, "[blood] vb failed\n"); return 0; }
    }
    s_dstate = 1; fprintf(stderr, "[blood] v1057B blood decal renderer ready\n");
    return 1;
}
static ID3D12PipelineState* gfd_pso(ID3D12Device* dev, DXGI_FORMAT rtfmt, int zfunc)
{
    for (int i = 0; i < 4; i++) if (s_dpso[i] && s_dpso_fmt[i] == rtfmt && s_dpso_z[i] == zfunc) return s_dpso[i];
    int slot = -1; for (int i = 0; i < 4; i++) if (!s_dpso[i]) { slot = i; break; }
    if (slot < 0) return NULL;
    static const char vs[] =
        "cbuffer c : register(b0) { float4 c0, c1, c2, c3, ps, po, kz; };\n"
        "struct I { float3 p:POSITION; float2 t:TEXCOORD; float4 c:COLOR; };\n"
        "struct O { float4 p:SV_POSITION; float2 t:TEXCOORD; float4 c:COLOR; float w:TEXCOORD1; float3 wp:TEXCOORD2; };\n"
        "O main(I i) { O o; float4 k = i.p.x * c0 + i.p.y * c1 + i.p.z * c2 + c3;\n"
        "  o.p = float4(k.xyz * ps.xyz + k.w * po.xyz, k.w); o.t = i.t; o.c = i.c; o.w = k.w; o.wp = i.p; return o; }\n";
    static const char ps[] =
        "Texture2D tx : register(t0); SamplerState sm : register(s0);\n"
        "struct O { float4 p:SV_POSITION; float2 t:TEXCOORD; float4 c:COLOR; float w:TEXCOORD1; float3 wp:TEXCOORD2; };\n"
        "float4 main(O i) : SV_TARGET { if (i.c.g < -2.5) { float r = length(i.t); if (r > 1.0) discard; return float4(i.c.r, i.c.r, i.c.r * 1.02, i.c.a * (1.0 - smoothstep(0.15, 1.0, r))); }\n"   /* v1093M smoke */
        "  if (i.c.g < -1.5) { if (i.c.b > 0.5) return float4(frac(i.t), 0.0, 1.0); float4 t = tx.Sample(sm, i.t); float3 g3 = dot(t.rgb, float3(0.3, 0.59, 0.11)); return float4(lerp(g3, t.rgb, 0.8) * i.c.r, 1.0); }\n"   /* v1088M held prop */
        "  if (i.c.g < -0.5) { float r = length(i.t); if (r > 1.0) discard;\n"
        "    return float4(0.20 + 0.10 * (1.0 - r), 0.005, 0.005, i.c.a * smoothstep(1.0, 0.35, r)); }\n"
        "  float4 t = tx.Sample(sm, i.t); return float4(t.rgb * i.c.rgb, t.a * i.c.a); }\n";
    /* v1057B-12 depth-projected decals: keep a pixel only where the drawn scene surface is within c.r metres (along the
     * view ray) of the decal plane -- no depth test, so no z-fighting, and a body lying on the floor (its pixels are
     * nearer than the floor) or a kerb edge the decal hangs over (farther) drops out. kz = (A, B): depth d = A + B / w. */
    /* v1069B-3 (user screenshots: the small landing drops blink with camera distance): projected decals the way deferred
     * decals work -- keep a pixel where the DRAWN surface lies within kz.z (10 cm) of the decal plane (the level's collision
     * sits a few cm off the drawn roofs / streets) AND runs parallel to it (the plane distance hardly changes from pixel to
     * pixel: rejects legs, walls, kerb faces). Soft edges on both tests. GFD_DEBUG: magenta pass, green scene above, blue
     * below, cyan not parallel, yellow no depth. */
    static const char psz[] =
        "Texture2D tx : register(t0); Texture2D<float> dz : register(t1); SamplerState sm : register(s0);\n"
        "cbuffer c : register(b0) { float4 c0, c1, c2, c3, ps, po, kz; };\n"
        "struct O { float4 p:SV_POSITION; float2 t:TEXCOORD; float4 c:COLOR; float w:TEXCOORD1; float3 wp:TEXCOORD2; };\n"
        "float4 main(O i) : SV_TARGET { float ds = dz.Load(int3(i.p.xy, 0));\n"
        "  float den = ds - kz.x; float vd = (ds < 0.999999 && abs(den) > 1e-9) ? 1.0 : 0.0;\n"
        "  float ws = vd > 0.5 ? kz.y / den : i.w; float d = (ws / i.w - 1.0) * i.c.r;\n"
        /* v1108B (user 22:33: blood "invisible when looked at from different angles", flicker): the parallel test allowed a fixed
         * change of d per pixel (0.6 x 0.0016 w = a head-on pixel); from a low angle a pixel covers several times more ground, so the
         * small tilt between the collision plane the decal sits on and the drawn street failed it (and blinked at the limit). Now
         * TILT = change of d per pixel / metres of the decal plane per pixel (ddx / ddy of the world position) -- the real slope
         * between drawn surface and decal, independent of the view angle; pass below 0.35 (~19 deg), soft from 0.2. */
        "  float gr = abs(ddx(d)) + abs(ddy(d)); float fpw = length(ddx(i.wp)) + length(ddy(i.wp)); float tilt = gr / max(fpw, 1e-5); float sl = 0.35;\n"
        "  if (i.c.g < -2.5) { float r = length(i.t); if (r > 1.0) discard; if (vd > 0.5 && ws < i.w * 0.99) discard; return float4(i.c.r, i.c.r, i.c.r * 1.02, i.c.a * (1.0 - smoothstep(0.15, 1.0, r))); }\n"   /* v1093M smoke */
        "  if (i.c.g > 1.5) { if (vd < 0.5 || abs(d) > 0.07) discard; float4 tb = tx.Sample(sm, i.t);\n"   /* v1117A blood on a body: whatever is drawn near the wound plane */
        "    return float4(tb.rgb * 0.72, tb.a * i.c.a * saturate((0.07 - abs(d)) / 0.03)); }\n"
        "  if (i.c.g < -1.5) { if (vd > 0.5 && ws < i.w * 0.997) discard; if (i.c.b > 0.5) return float4(frac(i.t), 0.0, 1.0); float4 tm = tx.Sample(sm, i.t); float3 g3 = dot(tm.rgb, float3(0.3, 0.59, 0.11)); return float4(lerp(g3, tm.rgb, 0.8) * i.c.r, 1.0); }\n"   /* v1088M held prop (g = -2) */
        "  if (i.c.g < -0.5) { float r = length(i.t); if (r > 1.0) discard;\n"   /* particle (g = -1): hidden where the scene is nearer */
        "    if (vd > 0.5 && ws < i.w * 0.994) discard;\n"
        "    return float4(0.20 + 0.10 * (1.0 - r), 0.005, 0.005, i.c.a * smoothstep(1.0, 0.35, r)); }\n"
        "  if (kz.w > 0.5) { if (vd < 0.5) return float4(1, 1, 0, 0.6); if (d > kz.z) return float4(0, 1, 0, 0.6); if (d < -kz.z) return float4(0, 0.3, 1, 0.6);\n"
        "    if (tilt > sl) return float4(0, 1, 1, 0.6); float4 t0 = tx.Sample(sm, i.t); return float4(lerp(float3(1, 0, 1), t0.rgb, 0.3), 0.8); }\n"
        "  if (vd < 0.5 || abs(d) > kz.z || tilt > sl) discard;\n"
        "  float4 t = tx.Sample(sm, i.t);\n"
        "  return float4(t.rgb, t.a * i.c.a * saturate((kz.z - abs(d)) / (0.5 * kz.z)) * saturate((sl - tilt) / (sl - 0.2))); }\n";
    const int zmode = (zfunc < 0);
    ID3DBlob *vb = NULL, *pb = NULL, *err = NULL;
    if (FAILED(D3DCompile(vs, sizeof vs - 1, "gfd_vs", NULL, NULL, "main", "vs_5_0", 0, 0, &vb, &err)) ||
        FAILED(zmode ? D3DCompile(psz, sizeof psz - 1, "gfd_psz", NULL, NULL, "main", "ps_5_0", 0, 0, &pb, &err)
                     : D3DCompile(ps, sizeof ps - 1, "gfd_ps", NULL, NULL, "main", "ps_5_0", 0, 0, &pb, &err))) {
        fprintf(stderr, "[blood] shader compile failed: %s\n", err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?"); return NULL; }
    D3D12_INPUT_ELEMENT_DESC il[3] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
    pd.pRootSignature = s_dsig;
    pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb); pd.VS.BytecodeLength = vb->lpVtbl->GetBufferSize(vb);
    pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb); pd.PS.BytecodeLength = pb->lpVtbl->GetBufferSize(pb);
    pd.InputLayout.pInputElementDescs = il; pd.InputLayout.NumElements = 3;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
    D3D12_RENDER_TARGET_BLEND_DESC* b = &pd.BlendState.RenderTarget[0];
    b->BlendEnable = TRUE; b->SrcBlend = D3D12_BLEND_SRC_ALPHA; b->DestBlend = D3D12_BLEND_INV_SRC_ALPHA; b->BlendOp = D3D12_BLEND_OP_ADD;
    b->SrcBlendAlpha = D3D12_BLEND_ZERO; b->DestBlendAlpha = D3D12_BLEND_ONE; b->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b->LogicOp = D3D12_LOGIC_OP_NOOP; b->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = TRUE; pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    { const int rev = (zfunc == 4 || zfunc == 6); pd.RasterizerState.DepthBias = 0; pd.RasterizerState.SlopeScaledDepthBias = 0.0f;   /* v1057B-3: slope bias pulled ground decals over legs / bumpers */ pd.RasterizerState.DepthBiasClamp = 0.0f; }   /* v1057B-2: pull decals in front of their surface (flicker) */
    pd.DepthStencilState.DepthFunc = (zfunc == 4 || zfunc == 6) ? D3D12_COMPARISON_FUNC_GREATER_EQUAL : D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    if (zmode) { pd.DepthStencilState.DepthEnable = FALSE; pd.DSVFormat = DXGI_FORMAT_UNKNOWN; }
    pd.SampleMask = UINT_MAX; pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = rtfmt; pd.SampleDesc.Count = 1;
    HRESULT hr = dev->lpVtbl->CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void**)&s_dpso[slot]);
    vb->lpVtbl->Release(vb); pb->lpVtbl->Release(pb);
    if (FAILED(hr)) { s_dpso[slot] = NULL; fprintf(stderr, "[blood] PSO failed 0x%08lX (rt fmt %d)\n", hr, (int)rtfmt); return NULL; }
    s_dpso_fmt[slot] = rtfmt; s_dpso_z[slot] = zfunc;
    return s_dpso[slot];
}
/* Draws every live decal into the bound target. c: 16 floats (c0..c3), ep: posscale(4) + posoffset(4). Returns 1 when
 * it changed command-list state (the caller rebinds its own). */
/* ---------------- v1095G ground probes: the floor height as DRAWN (gf_footik.c) ----------------
 * User: the leg IK read gf_collision.bin (the game's Havok shapes: kerbs / stairs as ramps, holes indoors) and the feet did not
 * match what is on screen. Now the game thread asks for up to GPB_N vertical columns (x, z, y_top, y_bottom) per frame
 * (gpb_request); the backend runs gpb_run right after the main pass's WORLD draws (before the first character, so legs and
 * people do not count) and a compute shader walks each column top -> bottom: a point is "occluded" when the drawn depth at
 * its pixel is nearer than the point. The first visible -> occluded step is the drawn surface (GPB_S samples). Results come
 * back through a readback ring (GPB_RING frames later) as (y, flag): 1 found, 2 hidden (occluded from the top: under a
 * wall / ledge / off view), 3 all visible (nothing within the range), 0 off screen. gpb_result() returns the newest. */
#define GPB_N 8
#define GPB_S 40
#define GPB_RING 4
static float s_gpb_req[GPB_N][4]; static int s_gpb_nreq; static unsigned s_gpb_req_seq; static volatile LONG s_gpb_lock;
static float s_gpb_res[GPB_N][2]; static unsigned s_gpb_res_seq[GPB_N]; static ULONGLONG s_gpb_req_t;
static ID3D12RootSignature* s_gpb_sig; static ID3D12PipelineState* s_gpb_pso; static ID3D12DescriptorHeap* s_gpb_heap;
static ID3D12Resource* s_gpb_uav; static ID3D12Resource* s_gpb_rb[GPB_RING]; static float* s_gpb_rbm[GPB_RING];
static unsigned s_gpb_rb_seq[GPB_RING]; static int s_gpb_rb_n[GPB_RING], s_gpb_rb_used[GPB_RING]; static int s_gpb_ri, s_gpb_state;
void gpb_request(const float* cols, int n)   /* game thread: n columns of (x, z, y_top, y_bottom) */
{
    if (n > GPB_N) n = GPB_N; if (n < 0) n = 0;
    while (InterlockedExchange(&s_gpb_lock, 1)) Sleep(0);
    memcpy(s_gpb_req, cols, (size_t)n * 16); s_gpb_nreq = n; s_gpb_req_seq++; s_gpb_req_t = GetTickCount64();
    InterlockedExchange(&s_gpb_lock, 0);
}
/* newest result for column i: 1 + flag (0 = none yet); *age = requests made since the one it answers */
int gpb_result(int i, float* y, unsigned* age)
{
    if (i < 0 || i >= GPB_N) return 0;
    while (InterlockedExchange(&s_gpb_lock, 1)) Sleep(0);
    const unsigned rs = s_gpb_res_seq[i]; const float yy = s_gpb_res[i][0], fl = s_gpb_res[i][1]; const unsigned cur = s_gpb_req_seq;
    InterlockedExchange(&s_gpb_lock, 0);
    if (!rs) return 0;
    *y = yy; *age = cur - rs; return 1 + (int)(fl + 0.5f);
}
int gpb_wanted(void) { return s_gpb_nreq > 0 && GetTickCount64() - s_gpb_req_t < 500 && s_gpb_state >= 0; }
static int gpb_init(ID3D12Device* dev)
{
    if (s_gpb_state) return s_gpb_state > 0;
    s_gpb_state = -1;
    static const char cs[] =
        "cbuffer c : register(b0) { float4 c0, c1, c2, c3, ps, po, kz; float4 col[8]; };\n"
        "Texture2D<float> dz : register(t0); RWStructuredBuffer<float4> ob : register(u0);\n"
        /* v1108S-2: the column walk as a function; up to 3 candidate spots (beside the body away from it, the other side, towards
         * the camera) -- the first that finds the ground wins (m19 soak: one side was over a drop / in a wall, "none" 600/600) */
        "float4 walkcol(float2 xz, float ytop, float ybot) {\n"
        "  uint W, H; dz.GetDimensions(W, H);\n"
        "  const int S = 40; float stp = (ytop - ybot) / (S - 1); int vis = 0, off = 0; float found = -1.0e9; int got = 0;\n"
        "  [loop] for (int k = 0; k < S; k++) { float y = ytop - stp * k;\n"
        "    float4 kk = xz.x * c0 + y * c1 + xz.y * c2 + c3; float4 o = float4(kk.xyz * ps.xyz + kk.w * po.xyz, kk.w);\n"
        "    if (o.w <= 0.05) { off++; continue; } float3 nd = o.xyz / o.w;\n"
        "    if (abs(nd.x) >= 0.999 || abs(nd.y) >= 0.999) { off++; continue; }\n"
        "    int2 px = int2((nd.x * 0.5 + 0.5) * W, (0.5 - nd.y * 0.5) * H); float ds = dz.Load(int3(px, 0));\n"
        "    float den = ds - kz.x; bool vd = ds < 0.999999 && abs(den) > 1e-9; float ws = vd ? kz.y / den : 1.0e9;\n"
        "    bool occ = ws < o.w * 0.997 - 0.004;\n"
        "    if (!occ) vis++; else { if (vis > 0 && !got) { got = 1; found = y + stp * 0.5; } } }\n"
        "  if (got) { float y0 = found + stp * 0.5; float lo = y0, hi = y0 - stp;\n"
        "    [loop] for (int r = 0; r < 4; r++) { float ym = (lo + hi) * 0.5; float4 kk = xz.x * c0 + ym * c1 + xz.y * c2 + c3; float4 o = float4(kk.xyz * ps.xyz + kk.w * po.xyz, kk.w);\n"
        "      float3 nd = o.xyz / max(o.w, 1e-4); int2 px = int2((nd.x * 0.5 + 0.5) * W, (0.5 - nd.y * 0.5) * H); float ds = dz.Load(int3(clamp(px, int2(0, 0), int2(W - 1, H - 1)), 0));\n"
        "      float den = ds - kz.x; float ws = (ds < 0.999999 && abs(den) > 1e-9) ? kz.y / den : 1.0e9; if (ws < o.w * 0.997 - 0.004) hi = ym; else lo = ym; }\n"
        "    found = (lo + hi) * 0.5;\n"
        /* v1108T (user: "one foot on the sidewalk, the foot on the road gets pushed up in the air"; stairs going down): the
         * first visible -> hidden step can be a NEARER object (the kerb / the step edge between the camera and the column)
         * instead of the surface at the column. Just below the found height the drawn depth must be the surface itself: if
         * what is drawn there is > GPB_OCC (0.08 m) nearer than the point, it was an occluder -> not ground (hidden). */
        "    { float yv = found - 0.012; float4 kk = xz.x * c0 + yv * c1 + xz.y * c2 + c3; float4 o = float4(kk.xyz * ps.xyz + kk.w * po.xyz, kk.w);\n"
        "      float3 nd = o.xyz / max(o.w, 1e-4); int2 px = int2((nd.x * 0.5 + 0.5) * W, (0.5 - nd.y * 0.5) * H); float ds = dz.Load(int3(clamp(px, int2(0, 0), int2(W - 1, H - 1)), 0));\n"
        "      float den = ds - kz.x; float ws = (ds < 0.999999 && abs(den) > 1e-9) ? kz.y / den : 1.0e9;\n"
        "      if (ws < o.w - col[7].w) { got = 0; vis = 0; } } }\n"
        "  float flag = got ? 1.0 : (off > S / 2 ? 0.0 : (vis == 0 ? 2.0 : 3.0));\n"
        "  return float4(found, flag, (float)vis, (float)off); }\n"
        "[numthreads(8, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {\n"
        "  uint i = id.x; if (i >= (uint)kz.z) return;\n"
        "  float4 cl = col[i]; float2 tc = col[7].xy - cl.xy; float tl = length(tc); float amt = (i == 4 ? 0.35 : kz.w);\n"
        "  float2 cand[3]; int nc = 1; cand[0] = cl.xy;\n"
        "  if (tl > 0.01) { float2 dir = tc / tl;\n"
        "    if (col[7].z > 0.5) { float2 pp = float2(-dir.y, dir.x); float2 rel = cl.xy - col[4].xy; float sg = (i == 4 || dot(pp, rel) >= 0.0) ? 1.0 : -1.0;\n"
        "      cand[0] = cl.xy + pp * (sg * amt); cand[1] = cl.xy - pp * (sg * amt); cand[2] = cl.xy + dir * amt; nc = 3; }\n"   /* v1108S: sideways first (same stair tread) */
        "    else { cand[0] = cl.xy + dir * amt; nc = 1; } }\n"
        "  float4 r = walkcol(cand[0], cl.z, cl.w);\n"
        "  [loop] for (int q = 1; q < nc && r.y != 1.0; q++) { float4 r2 = walkcol(cand[q], cl.z, cl.w); if (r2.y == 1.0 || r.y == 0.0) r = r2; }\n"
        "  ob[i] = r;\n"
        "  if (i == 4) { uint W, H; dz.GetDimensions(W, H); float y = (cl.z + cl.w) * 0.5; float4 kk = cl.x * c0 + y * c1 + cl.y * c2 + c3; float4 o = float4(kk.xyz * ps.xyz + kk.w * po.xyz, kk.w);\n"
        "    float3 nd = o.xyz / max(o.w, 1e-4); int2 px = int2((nd.x * 0.5 + 0.5) * W, (0.5 - nd.y * 0.5) * H); float ds = dz.Load(int3(clamp(px, int2(0, 0), int2(W - 1, H - 1)), 0));\n"
        "    ob[6] = float4((float)W, (float)H, nd.x, nd.y); ob[7] = float4(ds, o.w, kz.y / (ds - kz.x), nd.z); } }\n";
    ID3DBlob *b = NULL, *err = NULL;
    if (FAILED(D3DCompile(cs, sizeof cs - 1, "gpb_cs", NULL, NULL, "main", "cs_5_0", 0, 0, &b, &err))) {
        fprintf(stderr, "[gpb] compute shader failed: %s\n", err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?"); return 0; }
    D3D12_DESCRIPTOR_RANGE rg[2]; memset(rg, 0, sizeof rg);
    rg[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg[0].NumDescriptors = 1; rg[0].BaseShaderRegister = 0; rg[0].OffsetInDescriptorsFromTableStart = 0;
    rg[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; rg[1].NumDescriptors = 1; rg[1].BaseShaderRegister = 0; rg[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER rp[2]; memset(rp, 0, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.Num32BitValues = 60; rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable.NumDescriptorRanges = 2; rp[1].DescriptorTable.pDescriptorRanges = rg;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd = {0}; rd.NumParameters = 2; rd.pParameters = rp;
    ID3DBlob* sig = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(dev->lpVtbl->CreateRootSignature(dev, 0, sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig), &IID_ID3D12RootSignature, (void**)&s_gpb_sig))) {
        fprintf(stderr, "[gpb] root signature failed\n"); return 0; }
    sig->lpVtbl->Release(sig);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
    pd.pRootSignature = s_gpb_sig; pd.CS.pShaderBytecode = b->lpVtbl->GetBufferPointer(b); pd.CS.BytecodeLength = b->lpVtbl->GetBufferSize(b);
    if (FAILED(dev->lpVtbl->CreateComputePipelineState(dev, &pd, &IID_ID3D12PipelineState, (void**)&s_gpb_pso))) { fprintf(stderr, "[gpb] PSO failed\n"); return 0; }
    b->lpVtbl->Release(b);
    D3D12_DESCRIPTOR_HEAP_DESC hd = {0}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 2 * GPB_RING; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->lpVtbl->CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_gpb_heap))) { fprintf(stderr, "[gpb] heap failed\n"); return 0; }
    D3D12_HEAP_PROPERTIES hpd = {0}, hpr = {0}; hpd.Type = D3D12_HEAP_TYPE_DEFAULT; hpr.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = GPB_N * 16; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hpd, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void**)&s_gpb_uav))) {
        fprintf(stderr, "[gpb] uav failed\n"); return 0; }
    bd.Flags = D3D12_RESOURCE_FLAG_NONE;
    for (int i = 0; i < GPB_RING; i++) {
        D3D12_RANGE nr = { 0, 0 };
        if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hpr, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&s_gpb_rb[i])) ||
            FAILED(s_gpb_rb[i]->lpVtbl->Map(s_gpb_rb[i], 0, &nr, (void**)&s_gpb_rbm[i]))) { fprintf(stderr, "[gpb] readback failed\n"); return 0; }
    }
    s_gpb_state = 1; fprintf(stderr, "[gpb] v1095G ground probes ready (%d columns x %d samples)\n", GPB_N, GPB_S);
    return 1;
}
/* backend: after the main pass's world draws. zres = the scene depth (already in a shader-readable state), c = camera rows
 * c0..c3 + epilogue ps / po (24 floats). Returns 1 when it changed command-list state. */
int gpb_run(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, const float* c, ID3D12Resource* zres)
{
    if (!zres || !gpb_wanted() || !gpb_init(dev)) return 0;
    /* collect the readback slot that is GPB_RING-1 runs old (its copy has long finished) */
    { const int ri = s_gpb_ri; if (s_gpb_rb_used[ri]) {
          const float* r = s_gpb_rbm[ri]; const int n = s_gpb_rb_n[ri];
          while (InterlockedExchange(&s_gpb_lock, 1)) Sleep(0);
          for (int i = 0; i < n; i++) if (r[i * 4 + 1] == r[i * 4 + 1]) { s_gpb_res[i][0] = r[i * 4]; s_gpb_res[i][1] = r[i * 4 + 1]; s_gpb_res_seq[i] = s_gpb_rb_seq[ri]; }
          InterlockedExchange(&s_gpb_lock, 0);
          {   /* v1095G-3: flag histogram per column, every 600 results (1 ground, 2 hidden, 3 nothing in range, 0 off screen) */
              static unsigned hist[5][4], hn = 0;
              for (int i = 0; i < n && i < 5; i++) { const int f = (int)r[i * 4 + 1]; if (f >= 0 && f < 4) hist[i][f]++; }
              if (++hn >= 600) { fprintf(stderr, "[gpb] flags (ground/hidden/none/off): L heel %u/%u/%u/%u  L ball %u/%u/%u/%u  R heel %u/%u/%u/%u  R ball %u/%u/%u/%u  root %u/%u/%u/%u%c",
                  hist[0][1], hist[0][2], hist[0][3], hist[0][0], hist[1][1], hist[1][2], hist[1][3], hist[1][0], hist[2][1], hist[2][2], hist[2][3], hist[2][0],
                  hist[3][1], hist[3][2], hist[3][3], hist[3][0], hist[4][1], hist[4][2], hist[4][3], hist[4][0], 10); memset(hist, 0, sizeof hist); hn = 0;
                  if (n > 4) fprintf(stderr, "[gpb]   now: root col (%.2f %.2f y %.2f..%.2f) mid ndc (%.3f %.3f %.4f) depth %.6f point w %.3f scene w %.3f, root vis %.0f off %.0f%c",
                      s_gpb_req[4][0], s_gpb_req[4][1], s_gpb_req[4][2], s_gpb_req[4][3], r[26], r[27], r[31], r[28], r[29], r[30], r[18], r[19], 10); } }
          static int nl = 0; if (nl < 12) { nl++; fprintf(stderr, "[gpb] result: %d columns, col0 y %.3f flag %.0f (vis %.0f off %.0f), col4 y %.3f flag %.0f (vis %.0f off %.0f) | depth %.0fx%.0f, root mid ndc (%.3f %.3f %.4f) depth %.6f point w %.3f scene w %.3f%c",
              n, r[0], r[1], r[2], r[3], r[16], r[17], r[18], r[19], r[24], r[25], r[26], r[27], r[31], r[28], r[29], r[30], 10); } } }
    float k[60]; memset(k, 0, sizeof k);
    memcpy(k, c, 64); memcpy(k + 16, c + 16, 32);
    {   /* depth -> clip w: d = A + B / w (same fit as the decal pass) */
        const double ww = (double)c[3] * c[3] + (double)c[7] * c[7] + (double)c[11] * c[11];
        const double a = ww > 1e-12 ? ((double)c[2] * c[3] + (double)c[6] * c[7] + (double)c[10] * c[11]) / ww : 0.0, bb = c[14] - a * c[15];
        k[24] = (float)(c[18] * a + c[22]); k[25] = (float)(c[18] * bb);
        if (!(fabsf(k[25]) > 1e-6f) || !(k[24] == k[24])) return 0;
        static int nk = 0; if (nk < 3) { nk++; fprintf(stderr, "[gpb] camera w col (%.4g %.4g %.4g %.4g), z col (%.4g %.4g %.4g %.4g), ps (%.4g %.4g %.4g) po (%.4g %.4g %.4g), kz A %.7f B %.7f%c",
            c[3], c[7], c[11], c[15], c[2], c[6], c[10], c[14], c[16], c[17], c[18], c[20], c[21], c[22], k[24], k[25], 10); }
    }
    int n;
    while (InterlockedExchange(&s_gpb_lock, 1)) Sleep(0);
    n = s_gpb_nreq; memcpy(k + 28, s_gpb_req, (size_t)n * 16); const unsigned seq = s_gpb_req_seq;
    InterlockedExchange(&s_gpb_lock, 0);
    if (n <= 0) return 0;
    k[26] = (float)n; if (n > 7) n = 7, k[26] = 7.0f;
    {   /* v1095G-2: camera position (clip x, y, w = 0) -> col[7].xy; the columns move GPB_SIDE (0.15 m) towards it */
        const double A[3][3] = { { c[0], c[4], c[8] }, { c[1], c[5], c[9] }, { c[3], c[7], c[11] } }, bb3[3] = { -c[12], -c[13], -c[15] };
        const double det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) + A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
        if (fabs(det) < 1e-12) return 0;
        float cam[3]; for (int q = 0; q < 3; q++) { double M[3][3]; memcpy(M, A, sizeof M); for (int r = 0; r < 3; r++) M[r][q] = bb3[r];
            cam[q] = (float)((M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0])) / det); }
        k[56] = cam[0]; k[57] = cam[2];
        /* v1108S (user: legs on stairs): the 0.15 m shift TOWARDS the camera put a foot's column on the previous tread when
         * the camera is behind on stairs -> now SIDEWAYS (perpendicular to the view, away from the root column): along the
         * step edge, same tread. GPB_SHIFT=cam = old. */
        static int mode = -1; if (mode < 0) { const char* e = getenv("GPB_SHIFT"); mode = (e && *e == 'c') ? 0 : 1; } k[58] = (float)mode;
        static float occ = -1.0f; if (occ < 0.0f) { const char* e = getenv("GPB_OCC"); occ = e && *e ? (float)atof(e) : 0.08f; if (occ <= 0.0f) occ = 1000.0f; } k[59] = occ;   /* v1108T occluder test (GPB_OCC=0 off) */
        static float side = -1.0f; if (side < 0.0f) { const char* e = getenv("GPB_SIDE"); side = e && *e ? (float)atof(e) : 0.15f; } k[27] = side;
    }
    const int ri = s_gpb_ri; s_gpb_ri = (s_gpb_ri + 1) % GPB_RING;
    const UINT inc = dev->lpVtbl->GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE ch; RSX_D3D12_CPU_HEAP_START(s_gpb_heap)(s_gpb_heap, &ch); ch.ptr += (SIZE_T)(2 * ri) * inc;
    D3D12_GPU_DESCRIPTOR_HANDLE gh; RSX_D3D12_GPU_HEAP_START(s_gpb_heap)(s_gpb_heap, &gh); gh.ptr += (UINT64)(2 * ri) * inc;
    { D3D12_SHADER_RESOURCE_VIEW_DESC zv; memset(&zv, 0, sizeof zv);
      zv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; zv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      zv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; zv.Texture2D.MipLevels = 1;
      dev->lpVtbl->CreateShaderResourceView(dev, zres, &zv, ch); }
    { D3D12_UNORDERED_ACCESS_VIEW_DESC uv; memset(&uv, 0, sizeof uv);
      uv.Format = DXGI_FORMAT_UNKNOWN; uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; uv.Buffer.NumElements = GPB_N; uv.Buffer.StructureByteStride = 16;
      D3D12_CPU_DESCRIPTOR_HANDLE c2 = ch; c2.ptr += inc;
      dev->lpVtbl->CreateUnorderedAccessView(dev, s_gpb_uav, NULL, &uv, c2); }
    cl->lpVtbl->SetComputeRootSignature(cl, s_gpb_sig);
    cl->lpVtbl->SetPipelineState(cl, s_gpb_pso);
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, &s_gpb_heap);
    cl->lpVtbl->SetComputeRoot32BitConstants(cl, 0, 60, k, 0);
    cl->lpVtbl->SetComputeRootDescriptorTable(cl, 1, gh);
    cl->lpVtbl->Dispatch(cl, 1, 1, 1);
    D3D12_RESOURCE_BARRIER br; memset(&br, 0, sizeof br);
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition.pResource = s_gpb_uav; br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; br.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->lpVtbl->ResourceBarrier(cl, 1, &br);
    cl->lpVtbl->CopyBufferRegion(cl, s_gpb_rb[ri], 0, s_gpb_uav, 0, GPB_N * 16);
    br.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; br.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cl->lpVtbl->ResourceBarrier(cl, 1, &br);
    s_gpb_rb_seq[ri] = seq; s_gpb_rb_n[ri] = n; s_gpb_rb_used[ri] = 1;
    static int nl = 0; if (nl < 3) { nl++; fprintf(stderr, "[gpb] dispatched %d columns (req %u)%c", n, seq, 10); }
    return 1;
}

volatile int g_gfd_mode = 3;   /* v1090C: set by the backend per call: 1 = decals + particles, 2 = the held prop */
int gfd_render(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, const float* c, const float* ep, DXGI_FORMAT rtfmt, int zfunc, ID3D12Resource* zres)
{
    const int want_dec = (g_gfd_mode & 1) != 0, want_gm = (g_gfd_mode & 2) != 0;
    if (((!gfd_on() || (!s_gfd_n && !s_gfp_live)) && !s_gm_on && !s_smk_live) || !gfd_init(dev)) return 0;   /* v1093M: + smoke */   /* v1088M: or a held prop */
    /* v1057B-12: zres = the scene depth (R24G8 typeless, in PIXEL_SHADER_RESOURCE, no DSV bound) -> depth-projected mode */
    float kzA = 0, kzB = 0;
    { static int zm = -1; if (zm < 0) { const char* e = getenv("GFD_ZMODE"); zm = (e && *e == '0') ? 0 : 1; } if (!zm) zres = NULL; }
    if (zres) {   /* clip z ~= a w + b (least squares over the x/y/z rows), then the epilogue: d = A + B / w */
        const double ww = (double)c[3] * c[3] + (double)c[7] * c[7] + (double)c[11] * c[11];
        const double a = ww > 1e-12 ? ((double)c[2] * c[3] + (double)c[6] * c[7] + (double)c[10] * c[11]) / ww : 0.0, b = c[14] - a * c[15];
        kzA = (float)(ep[2] * a + ep[6]); kzB = (float)(ep[2] * b);
        if (!(fabsf(kzB) > 1e-6f) || !(kzA == kzA)) zres = NULL;
    }
    if (zres) zfunc = -1;
    ID3D12PipelineState* pso = gfd_pso(dev, rtfmt, zfunc);
    if (!pso) return 0;
    s_dvbi = (s_dvbi + 1) % GFD_RING;
    DVert* v = s_dvbmap[s_dvbi]; unsigned nv = 0;
    const ULONGLONG now = GetTickCount64();
    static const float life = 240.0f, fade = 30.0f;
    /* v1057B-4 (user: every decal flickers): the drawn street sits a few cm off the collision surface, so a decal 2.5 cm
     * off the collision plane z-fought with it. Pull every corner GFD_PULL (6 cm) toward the camera along its view ray:
     * same place on screen, always in front of the surface. Camera = the point where clip x, y and w are all 0. */
    float cam[3] = { 0, 0, 0 }; int camok = 0;
    {   /* row-vector: clip_j = x c0[j] + y c1[j] + z c2[j] + c3[j]; solve for j = x, y, w */
        const double A[3][3] = { { c[0], c[4], c[8] }, { c[1], c[5], c[9] }, { c[3], c[7], c[11] } };
        const double b[3] = { -c[12], -c[13], -c[15] };
        const double det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) + A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
        if (fabs(det) > 1e-12) {
            for (int k = 0; k < 3; k++) {
                double M[3][3]; memcpy(M, A, sizeof M); for (int r = 0; r < 3; r++) M[r][k] = b[r];
                cam[k] = (float)((M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0])) / det);
            }
            camok = cam[0] == cam[0] && fabsf(cam[0]) < 1e6f && fabsf(cam[1]) < 1e6f && fabsf(cam[2]) < 1e6f;
        }
    }
    static float pull = -1.0f; if (pull < 0.0f) { const char* e = getenv("GFD_PULL"); pull = e && *e ? (float)atof(e) : 0.10f; }   /* v1057B-9: 10 cm (small sidewalk spots still flickered at 6) */
    static float ztol = -1.0f; if (ztol < 0.0f) { const char* e = getenv("GFD_TOL"); ztol = e && *e ? (float)atof(e) : 0.12f; }   /* metres off the decal plane (v1069B-3: 10 cm slab + parallel test; v1108B 12 cm) */
    gfd_lock();
    for (int i = 0; want_dec && i < s_gfd_n; i++) {
        const GfDecal* d = &s_gfd[i];
        const float age = (float)(now - d->born) / 1000.0f;
        float a = d->alpha; if (age > life - fade) a *= (life - age) / fade; if (a <= 0.0f) continue;
        float sc = 1.0f; if (d->grow > 0.0f && age < d->grow) { const float t = age / d->grow; sc = 0.45f + 0.55f * (1.0f - (1.0f - t) * (1.0f - t)); }
        const float hw = d->w * 0.5f * sc, hh = d->h * 0.5f * sc;
        const float* n = d->n; const float* t = d->t;
        /* v1057B-14 (user: blood floating around a body at a low camera): the old along-the-ray tolerance grew to 0.5 m
         * at grazing angles and painted the body's edges. Now the scene point's height over the decal plane is exact:
         * P_scene - P = (w_scene / w - 1) (P - cam), so its normal distance = (w_scene / w - 1) * dot(P - cam, n). */
        if (zres && !camok) continue;
        /* v1119B (user screenshot: blood visible through a wall): the depth test keeps a pixel wherever the drawn surface is
         * within 12 cm of the decal plane, so a splat on the far side of a thin wall passed on the near side. The decal's
         * normal faces the side the blood landed on -- seen from behind (camera on the other side) it is not drawn.
         * GFD_BACKFACE=1 = old. */
        { static int bf = -1; if (bf < 0) { const char* e = getenv("GFD_BACKFACE"); bf = (e && *e == '1') ? 1 : 0; }
          if (!bf && camok && (cam[0] - d->p[0]) * n[0] + (cam[1] - d->p[1]) * n[1] + (cam[2] - d->p[2]) * n[2] < 0.0f) continue; }
        const float bx = n[1] * t[2] - n[2] * t[1], by = n[2] * t[0] - n[0] * t[2], bz = n[0] * t[1] - n[1] * t[0];
        const float lift = zres ? 0.0f : 0.025f;   /* v1057B-9: 2.5 cm off the collision surface (the drawn kerbs sit higher) */
        const float ox = d->p[0] + n[0] * lift, oy = d->p[1] + n[1] * lift, oz = d->p[2] + n[2] * lift;
        const float u0 = (d->sprite & 1) * 0.5f + 0.004f, v0 = (d->sprite >> 1) * 0.5f + 0.004f, u1 = u0 + 0.492f, v1 = v0 + 0.492f;
        DVert q[4];
        const float sx[4] = { -1, 1, -1, 1 }, sy[4] = { -1, -1, 1, 1 };
        for (int k = 0; k < 4; k++) {
            q[k].x = ox + t[0] * hw * sx[k] + bx * hh * sy[k]; q[k].y = oy + t[1] * hw * sx[k] + by * hh * sy[k]; q[k].z = oz + t[2] * hw * sx[k] + bz * hh * sy[k];
            if (camok && !zres) { const float vx = cam[0] - q[k].x, vy = cam[1] - q[k].y, vz = cam[2] - q[k].z, L = sqrtf(vx * vx + vy * vy + vz * vz);
                         if (L > pull * 4.0f) { q[k].x += vx / L * pull; q[k].y += vy / L * pull; q[k].z += vz / L * pull; } }
            q[k].u = sx[k] < 0 ? u0 : u1; q[k].v = sy[k] < 0 ? v0 : v1; q[k].r = q[k].g = q[k].b = 1.0f; q[k].a = a;
            if (zres) q[k].r = (q[k].x - cam[0]) * n[0] + (q[k].y - cam[1]) * n[1] + (q[k].z - cam[2]) * n[2];
        }
        v[nv++] = q[0]; v[nv++] = q[1]; v[nv++] = q[2]; v[nv++] = q[2]; v[nv++] = q[1]; v[nv++] = q[3];
    }
    if (want_dec && zres && camok) for (int i = 0; i < GFB_MAX; i++) {   /* v1117A blood on bodies (depth-projected only) */
        const GfBody* b = &s_gfb[i];
        if (now - b->upd > 250 || b->alpha <= 0.0f || nv + 6 > (unsigned)(GFD_MAX + GFP_MAX + GFB_MAX) * 6u) continue;
        const float* n = b->n; const float* t = b->t; const float hw = b->size * 0.5f;
        const float bx = n[1] * t[2] - n[2] * t[1], by = n[2] * t[0] - n[0] * t[2], bz = n[0] * t[1] - n[1] * t[0];
        const float u0 = (b->sprite & 1) * 0.5f + 0.004f, v0 = (b->sprite >> 1) * 0.5f + 0.004f, u1 = u0 + 0.492f, v1 = v0 + 0.492f;
        DVert q[4]; const float sx[4] = { -1, 1, -1, 1 }, sy[4] = { -1, -1, 1, 1 };
        for (int k = 0; k < 4; k++) {
            q[k].x = b->p[0] + t[0] * hw * sx[k] + bx * hw * sy[k]; q[k].y = b->p[1] + t[1] * hw * sx[k] + by * hw * sy[k]; q[k].z = b->p[2] + t[2] * hw * sx[k] + bz * hw * sy[k];
            q[k].u = sx[k] < 0 ? u0 : u1; q[k].v = sy[k] < 0 ? v0 : v1; q[k].g = 2.0f; q[k].b = 1.0f; q[k].a = b->alpha;
            q[k].r = (q[k].x - cam[0]) * n[0] + (q[k].y - cam[1]) * n[1] + (q[k].z - cam[2]) * n[2];
        }
        v[nv++] = q[0]; v[nv++] = q[1]; v[nv++] = q[2]; v[nv++] = q[2]; v[nv++] = q[1]; v[nv++] = q[3];
    }
    /* v1069B particles: p(a) = p0 + v0 a + g a^2 / 2 with light air drag; streak along the velocity, facing the camera */
    float land[24][4]; int nland = 0;
    if (camok && want_dec) for (int i = 0; i < GFP_MAX; i++) {
        GfPart* q = &s_gfp[i]; if (!q->alive) continue;
        const float a = (float)(now - q->born) / 1000.0f;
        const float dr = 0.35f, fe = (1.0f - expf(-dr * a)) / dr;   /* drag on the horizontal travel */
        float P[3] = { q->p[0] + q->v[0] * fe, q->p[1] + q->v[1] * a - 4.905f * a * a, q->p[2] + q->v[2] * fe };
        const float V3[3] = { q->v[0] * expf(-dr * a), q->v[1] - 9.81f * a, q->v[2] * expf(-dr * a) };
        if (P[1] <= q->ground || a > q->life) {
            if (P[1] <= q->ground && a <= q->life && nland < 24) { land[nland][0] = P[0]; land[nland][1] = q->ground; land[nland][2] = P[2]; land[nland][3] = q->size; nland++; }
            q->alive = 0; s_gfp_live--; s_gfp_landed++; continue;
        }
        float sp = sqrtf(V3[0] * V3[0] + V3[1] * V3[1] + V3[2] * V3[2]); if (sp < 1e-3f) sp = 1e-3f;
        const float al[3] = { V3[0] / sp, V3[1] / sp, V3[2] / sp };
        const float vc[3] = { cam[0] - P[0], cam[1] - P[1], cam[2] - P[2] };
        float sd[3] = { al[1] * vc[2] - al[2] * vc[1], al[2] * vc[0] - al[0] * vc[2], al[0] * vc[1] - al[1] * vc[0] };
        const float sl = sqrtf(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]); if (sl < 1e-5f) continue;
        sd[0] /= sl; sd[1] /= sl; sd[2] /= sl;
        const float hl = q->size * 0.6f + sp * 0.010f, hw = q->size * 0.5f;
        const float fade = a > q->life - 0.3f ? (q->life - a) / 0.3f : 1.0f;
        DVert c4[4]; const float sx[4] = { -1, 1, -1, 1 }, sy[4] = { -1, -1, 1, 1 };
        for (int k = 0; k < 4; k++) {
            c4[k].x = P[0] + al[0] * hl * sx[k] + sd[0] * hw * sy[k]; c4[k].y = P[1] + al[1] * hl * sx[k] + sd[1] * hw * sy[k]; c4[k].z = P[2] + al[2] * hl * sx[k] + sd[2] * hw * sy[k];
            c4[k].u = sx[k]; c4[k].v = sy[k]; c4[k].r = 0.0f; c4[k].g = -1.0f; c4[k].b = 0.0f; c4[k].a = 0.92f * fade;
        }
        if (nv + 6 > (unsigned)(GFD_MAX + GFP_MAX) * 6u) break;
        v[nv++] = c4[0]; v[nv++] = c4[1]; v[nv++] = c4[2]; v[nv++] = c4[2]; v[nv++] = c4[1]; v[nv++] = c4[3];
    }
    LeaveCriticalSection(&s_gfd_cs);
    for (int i = 0; i < nland; i++) {   /* where droplets land: a small splat (sprite 3 drops / 1 droplets) */
        static unsigned rs = 12345u; rs = rs * 1103515245u + 12345u; const float ang = (float)(rs >> 8) / 16777216.0f * 6.2831853f;
        const float n[3] = { 0, 1, 0 }, t[3] = { cosf(ang), 0, sinf(ang) }, sz = 0.05f + land[i][3] * 2.2f;
        const float pp[3] = { land[i][0], land[i][1] + 0.005f, land[i][2] };
        gfd_add(pp, n, t, sz, sz, (rs >> 4) & 1 ? 3 : 1, 0.85f, 0.0f);
    }
    { static ULONGLONG lt = 0; if (now - lt > 10000 && s_gfp_total) { lt = now; fprintf(stderr, "[blood] v1069B particles: %ld spawned, %ld ended, %d live%c", s_gfp_total, s_gfp_landed, s_gfp_live, 10); } }
    /* v1088M held prop: model -> world on the CPU (x axis, y axis, z axis, position), simple sun + ambient in the colour */
    unsigned nv_dec = nv, nv_gm = 0;
    float gm_mm[12];
    if (want_gm && s_gm_state > 0 && s_gm_res && nv + (unsigned)s_gm_ni <= (unsigned)(GFD_MAX + GFP_MAX) * 6u + GM_MAXV && gm_pick(gm_mm)) {   /* v1093S: this frame's matrix */
        float m[12]; memcpy(m, gm_mm, sizeof m);
        const float L[3] = { 0.35f, 0.80f, 0.48f };
        for (int i = 0; i < s_gm_ni; i++) {
            const float* q = s_gm_v + (size_t)s_gm_i[i] * 8;
            DVert o;
            o.x = m[9] + m[0] * q[0] + m[3] * q[1] + m[6] * q[2];
            o.y = m[10] + m[1] * q[0] + m[4] * q[1] + m[7] * q[2];
            o.z = m[11] + m[2] * q[0] + m[5] * q[1] + m[8] * q[2];
            const float nx = m[0] * q[3] + m[3] * q[4] + m[6] * q[5], ny = m[1] * q[3] + m[4] * q[4] + m[7] * q[5], nz = m[2] * q[3] + m[5] * q[4] + m[8] * q[5];
            const float nn = sqrtf(nx * nx + ny * ny + nz * nz) + 1e-6f, dl = (nx * L[0] + ny * L[1] + nz * L[2]) / nn;
            o.u = q[6]; o.v = q[7]; o.r = 0.42f + 0.33f * (dl > 0.0f ? dl : 0.0f); o.g = -2.0f; o.b = s_gm_dbg ? 1.0f : 0.0f; o.a = 1.0f;   /* v1089L: flatter, darker (closer to the game's shading) */
            v[nv++] = o;
        }
        nv_gm = nv - nv_dec;
    }
    unsigned nv_smk = 0;   /* v1093M smoke: after the prop, same pass */
    if (want_gm && s_smk_live > 0 && camok) {
        const unsigned base = nv; const ULONGLONG nowm = GetTickCount64();
        const unsigned cap = (unsigned)(GFD_MAX + GFP_MAX) * 6u + GM_MAXV + (unsigned)SMK_MAX * 6u;
        gfd_lock();
        for (int i = 0; i < SMK_MAX; i++) {
            GfSmoke* q = &s_smk[i]; if (!q->alive) continue;
            const float a = (float)(nowm - q->born) / 1000.0f;
            if (a > q->life) { q->alive = 0; s_smk_live--; continue; }
            if (nv + 6 > cap) break;
            const float dr = 1.6f, fe = (1.0f - expf(-dr * a)) / dr;   /* drag on the launch speed */
            const float sw = 0.035f * a;                                /* lazy sway grows with age */
            const float P[3] = { q->p[0] + q->v[0] * fe + sw * sinf(q->ph + a * 1.7f), q->p[1] + q->v[1] * fe + 0.11f * a, q->p[2] + q->v[2] * fe + sw * cosf(q->ph * 1.3f + a * 1.3f) };
            const float sz = q->s0 + (q->s1 - q->s0) * (1.0f - expf(-a / (0.35f * q->life)));
            float al = q->a0 * (a < 0.12f ? a / 0.12f : 1.0f) * (1.0f - a / q->life); al *= al > 0 ? 1.0f : 0.0f;
            const float vc[3] = { cam[0] - P[0], cam[1] - P[1], cam[2] - P[2] }; const float vl = sqrtf(vc[0] * vc[0] + vc[1] * vc[1] + vc[2] * vc[2]); if (vl < 0.05f) continue;
            const float f[3] = { vc[0] / vl, vc[1] / vl, vc[2] / vl };
            float rt[3] = { f[2], 0.0f, -f[0] }; float rl = sqrtf(rt[0] * rt[0] + rt[2] * rt[2]); if (rl < 1e-4f) { rt[0] = 1; rt[2] = 0; rl = 1; } rt[0] /= rl; rt[2] /= rl;
            const float up[3] = { f[1] * rt[2] - f[2] * rt[1], f[2] * rt[0] - f[0] * rt[2], f[0] * rt[1] - f[1] * rt[0] };
            DVert c4[4]; const float sx[4] = { -1, 1, -1, 1 }, sy[4] = { -1, -1, 1, 1 };
            const float grey = 0.62f + 0.12f * sinf(q->ph * 3.0f);
            for (int k = 0; k < 4; k++) {
                c4[k].x = P[0] + (rt[0] * sx[k] + up[0] * sy[k]) * sz; c4[k].y = P[1] + (rt[1] * sx[k] + up[1] * sy[k]) * sz; c4[k].z = P[2] + (rt[2] * sx[k] + up[2] * sy[k]) * sz;
                c4[k].u = sx[k]; c4[k].v = sy[k]; c4[k].r = grey; c4[k].g = -3.0f; c4[k].b = 0.0f; c4[k].a = al;
            }
            v[nv++] = c4[0]; v[nv++] = c4[1]; v[nv++] = c4[2]; v[nv++] = c4[2]; v[nv++] = c4[1]; v[nv++] = c4[3];
        }
        LeaveCriticalSection(&s_gfd_cs);
        nv_smk = nv - base;
    }
    if (!nv) return 0;
    if (nv_gm && !s_gm_up_done) {
        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = s_gm_res; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = s_gm_up; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM; src.PlacedFootprint.Footprint.Width = (UINT)s_gm_tw;
        src.PlacedFootprint.Footprint.Height = (UINT)s_gm_th; src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = (UINT)s_gm_tw * 4;
        cl->lpVtbl->CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
        D3D12_RESOURCE_BARRIER br; memset(&br, 0, sizeof br);
        br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition.pResource = s_gm_res; br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        br.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; br.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cl->lpVtbl->ResourceBarrier(cl, 1, &br);
        s_gm_up_done = 1;
    }
    if (!s_dtex_up_done) {
        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = s_dtex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = s_dtex_up; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM; src.PlacedFootprint.Footprint.Width = 256;
        src.PlacedFootprint.Footprint.Height = 256; src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = 1024;
        cl->lpVtbl->CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
        D3D12_RESOURCE_BARRIER br; memset(&br, 0, sizeof br);
        br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition.pResource = s_dtex; br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        br.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; br.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cl->lpVtbl->ResourceBarrier(cl, 1, &br);
        s_dtex_up_done = 1;
    }
    float k[28]; memcpy(k, c, 64); memcpy(k + 16, ep, 32); k[24] = kzA; k[25] = kzB; k[26] = ztol; k[27] = 0;
    { static int dbg = -1; if (dbg < 0) { const char* e = getenv("GFD_DEBUG"); dbg = (e && *e == '1') ? 1 : 0; } k[27] = (float)dbg; }   /* v1069B: colour the depth test */
    { static int nf = 0; if (k[27] > 0.5f && nf < 400) { nf++; fprintf(stderr, "[blood] dbg f%d kz %.7f %.7f cam (%.2f %.2f %.2f) c15 %.3f c14 %.5f decals %d parts %d%c", nf, kzA, kzB, cam[0], cam[1], cam[2], c[15], c[14], s_gfd_n, s_gfp_live, 10); } }
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_dsig);
    cl->lpVtbl->SetPipelineState(cl, pso);
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, &s_dheap);
    D3D12_GPU_DESCRIPTOR_HANDLE gh; RSX_D3D12_GPU_HEAP_START(s_dheap)(s_dheap, &gh);
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 1, gh);
    if (zres) {   /* this frame's depth SRV in slot 1 + ring index (an older frame's may still be read by the GPU) */
        D3D12_SHADER_RESOURCE_VIEW_DESC zv; memset(&zv, 0, sizeof zv);
        zv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; zv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        zv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; zv.Texture2D.MipLevels = 1;
        const UINT inc = dev->lpVtbl->GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE zc; RSX_D3D12_CPU_HEAP_START(s_dheap)(s_dheap, &zc); zc.ptr += (SIZE_T)(1 + s_dvbi) * inc;
        dev->lpVtbl->CreateShaderResourceView(dev, zres, &zv, zc);
        D3D12_GPU_DESCRIPTOR_HANDLE zg = gh; zg.ptr += (UINT64)(1 + s_dvbi) * inc;
        cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 2, zg);
    }
    cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 0, 28, k, 0);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv;
    vbv.BufferLocation = s_dvb[s_dvbi]->lpVtbl->GetGPUVirtualAddress(s_dvb[s_dvbi]); vbv.SizeInBytes = nv * sizeof(DVert); vbv.StrideInBytes = sizeof(DVert);
    cl->lpVtbl->IASetVertexBuffers(cl, 0, 1, &vbv);
    if (nv_dec) cl->lpVtbl->DrawInstanced(cl, nv_dec, 1, 0, 0);
    if (nv_gm) {   /* v1088M: the held prop with its own texture (slot 1 + GFD_RING) */
        const UINT inc2 = dev->lpVtbl->GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_GPU_DESCRIPTOR_HANDLE mg = gh; mg.ptr += (UINT64)(1 + GFD_RING) * inc2;
        cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 1, mg);
        cl->lpVtbl->DrawInstanced(cl, nv_gm, 1, nv_dec, 0);
        static int nlm = 0; if (nlm < 3) { nlm++; fprintf(stderr, "[prop] v1088M drew the held prop: %u vertices at (%.2f %.2f %.2f)%s\n", nv_gm, s_gm_m[9], s_gm_m[10], s_gm_m[11], zres ? " (depth-projected)" : ""); }
    }
    if (nv_smk) { cl->lpVtbl->DrawInstanced(cl, nv_smk, 1, nv - nv_smk, 0);   /* v1093M smoke (the shader ignores the texture) */
        static int nls = 0; if (nls < 3) { nls++; fprintf(stderr, "[smoke-fx] v1093M drew %u smoke puffs%c", nv_smk / 6, 10); } }
    static int nlog = 0; if (nlog < 3) { nlog++; fprintf(stderr, "[blood] drawing %u decal vertices (rt fmt %d, zfunc %d), camera %s (%.1f %.1f %.1f), %s\n", nv, (int)rtfmt, zfunc, camok ? "found" : "NOT found", cam[0], cam[1], cam[2],
        zres ? "v1057B-12 depth-projected" : "depth-tested + pulled"); if (zres) fprintf(stderr, "[blood]   depth d = %.6f + %.6f / w, tolerance %.2f m\n", kzA, kzB, ztol); }
    return 1;
}

/* Called by the D3D12 backend once per presented frame, with the backbuffer bound-able as RTV
 * and in RENDER_TARGET state. Leaves the command list with our root signature/heap bound;
 * the backend only copies/transitions afterwards and re-binds everything next frame. */

/* ===========================================================================
 * v194 SETTINGS MENU (port overlay, same look as the system dialogs).
 * Open: F10 any time; while the game is paused (g_uiov_paused_fn, stubs.cpp: the game's world clock stopped while
 * its real clock runs = pause menu) a hint "F10 / Back  Settings & Quit" is shown -- controller Back or a click on
 * the hint opens it too. Up/Down select, Left/Right (or click / right click) change, Enter/A on a row, Esc/B close.
 * Values live in settings.ini (current directory = infamous-recomp\ for every launcher); main.cpp loads it into the
 * environment at startup (it overrides the launcher). Live settings apply at once through the g_* hooks below;
 * the ones marked "restart" are read once at startup.
 * =========================================================================*/
volatile int g_ui_fps_cap = -2;      /* -2 not set (launcher F7 behaviour), 0 off, 1 auto (v209), else fps -- infamous_clock_probe.h */
volatile int g_ui_kbm_glyphs = -1;   /* -1 env, 0 controller, 1 auto, 2 keyboard/mouse -- rsx_d3d12_backend.c */
volatile int g_ui_mouse_speed = 0;   /* 0 env, else px/s for a full stick -- cellPad.c */
volatile int g_set_vsync = -1;       /* -1 env, 0/1 -- rsx_d3d12_backend.c present_sync_interval */
extern volatile int g_win_fullscreen;   /* rsx_d3d12_backend.c window proc (F11) */
extern volatile int g_fs_cutscene_open; /* runtime/ppu/ppu_fs.cpp: story-cutscene movies open right now */
int (*volatile g_uiov_paused_fn)(void) = 0;
/* v195: set by the native pause-menu entries (stubs.cpp, script natives on the PPU thread): 1 = Settings, 2 = Quit Game */
volatile int g_uiov_request = 0;
/* v243K: val/name 8 -> 12 (Language has 12 choices) */
typedef struct { const char* key; const char* label; int restart; int n; const char* val[12]; const char* name[12]; int cur, saved; } MItem;
static MItem s_mi[] = {
    { "FULLSCREEN",      "Display Mode",       0, 2, { "0", "1" }, { "Windowed", "Fullscreen" } },
    { "PRESENT_VSYNC",   "VSync",              0, 2, { "0", "1" }, { "Off", "On" } },
    { "UI_FPS_CAP",      "Frame Rate Limit",   0, 6, { "0", "60", "50", "45", "40", "30" }, { "Off", "60 FPS", "50 FPS", "45 FPS", "40 FPS", "30 FPS" } },   /* v209b: Auto removed */
    { "KBM_GLYPHS",      "Button Prompts",     0, 3, { "1", "0", "2" }, { "Automatic", "Controller", "Keyboard & Mouse" } },
    { "PAD_MOUSE_SPEED", "Mouse Speed",        0, 5, { "900", "650", "450", "325", "225" }, { "1 (Slow)", "2", "3", "4", "5 (Fast)" } },
    { "RES_SCALE",       "Resolution",         1, 5, { "1", "1.5", "2", "2.5", "3" }, { "720p", "1080p", "1440p", "1800p", "2160p (4K)" } },
    { "SHADOWS",         "Shadows",            1, 2, { "1", "0" }, { "On", "Off" } },
    /* v1115B: shadow map resolution (rsx_d3d12_backend.c res_scale_dims / shres_register) */
    { "SHADOW_RES",      "Shadow Quality",     1, 3, { "1024", "2048", "4096" }, { "Original", "High (2x)", "Ultra (4x)" } },
    { "TEX_ANISO",       "Texture Filtering",  1, 5, { "-1", "0", "4", "8", "16" }, { "Game Default", "Bilinear", "4x Anisotropic", "8x Anisotropic", "16x Anisotropic" } },
    { "TEX_MIPS",        "Mipmaps",            1, 2, { "1", "0" }, { "On", "Off" } },
    /* v1114T: AI-upscaled texture packs (texpack\gf_texpack_2x.bin / _4x.bin, tools/gf_texpack.py); live, rsx_d3d12_backend.c */
    { "TEX_UPSCALE",     "Texture Upscaling",  0, 3, { "0", "2", "4" }, { "Off", "2x (AI)", "4x (AI)" } },
    /* v1116A lighting pack (rsx_post.h), live */
    { "GF_POST_AO",      "Ambient Occlusion",  0, 3, { "1", "2", "0" }, { "On", "Strong", "Off" } },
    { "GF_POST_BLOOM",   "Bloom",              0, 3, { "1", "2", "0" }, { "Low", "High", "Off" } },
    { "GF_POST_GRADE",   "Color Grading",      0, 3, { "1", "2", "0" }, { "Film", "Contrast", "Off" } },   /* v1116E: + Contrast */
    { "GF_POST_SHARPEN", "Sharpening",         0, 3, { "1", "2", "0" }, { "Low", "High", "Off" } },   /* v1117F CAS */
    { "GF_POST_SSR",     "Wet Streets",        0, 4, { "2", "1", "3", "0" }, { "When Raining", "When Raining (Subtle)", "Always", "Off" } },   /* v1118A reflections; v1118B rain only */
    /* v243K (from PR #25): DLSS / FSR upscaling and FSR frame generation -- rows exist only in a build made with the
     * NVIDIA DLSS SDK (PS3RECOMP_NGX) and/or the AMD FSR SDK (PS3RECOMP_FFX); rsx_dlss.cpp reads UPSCALER / FSR_FG. */
#if defined(PS3RECOMP_NGX) && defined(PS3RECOMP_FFX)
    { "UPSCALER",        "Upscaling",          1, 9, { "off", "dlaa", "dlss_quality", "dlss_balanced", "dlss_performance", "fsr_native", "fsr_quality", "fsr_balanced", "fsr_performance" },
                                                     { "Off", "NVIDIA DLAA", "DLSS Quality", "DLSS Balanced", "DLSS Performance", "FSR Native AA", "FSR Quality", "FSR Balanced", "FSR Performance" } },
#elif defined(PS3RECOMP_NGX)
    { "UPSCALER",        "Upscaling",          1, 5, { "off", "dlaa", "dlss_quality", "dlss_balanced", "dlss_performance" },
                                                     { "Off", "NVIDIA DLAA", "DLSS Quality", "DLSS Balanced", "DLSS Performance" } },
#elif defined(PS3RECOMP_FFX)
    { "UPSCALER",        "Upscaling",          1, 5, { "off", "fsr_native", "fsr_quality", "fsr_balanced", "fsr_performance" },
                                                     { "Off", "FSR Native AA", "FSR Quality", "FSR Balanced", "FSR Performance" } },
#endif
#ifdef PS3RECOMP_FFX
    { "FSR_FG",          "Frame Generation",   1, 2, { "0", "1" }, { "Off", "On (FSR)" } },
#endif
    /* v243K (from PR #25): game language (dialogue, subtitles, HUD); cellSysutil.c + infamous_lang.h read PS3_LANG */
    { "PS3_LANG",        "Language",           1, 12, { "english", "british", "french", "german", "italian", "spanish", "dutch", "polish", "portuguese", "russian", "korean", "chinese_t" },
                                                      { "English (US)", "English (UK)", "French", "German", "Italian", "Spanish", "Dutch", "Polish", "Portuguese", "Russian", "Korean (text)", "Chinese (text)" } },
};
#define MI_N ((int)(sizeof s_mi / sizeof s_mi[0]))
enum { MR_RESUME = MI_N, MR_QUIT = MI_N + 1, MR_N = MI_N + 2 };
static int s_menu_inited, s_menu_sel, s_menu_confirm, s_menu_csel, s_restart_note;
static ULONGLONG s_menu_t_open;
static unsigned s_menu_prev;
static float s_row_y0[MR_N], s_row_y1[MR_N], s_row_x0, s_row_x1, s_hint_r[4];
static int s_hint_on, s_menu_quitonly;
static int s_wm_on; static ULONGLONG s_wm_last;   /* v238 pause watermark */
static float s_cbtn[2][4];   /* v195: confirm buttons (mouse) */
static int mi_match(const MItem* m, const char* v)
{
    if (!v || !*v) return -1;
    for (int i = 0; i < m->n; i++) {
        if (!strcmp(m->val[i], v)) return i;
        if (!strcmp(m->key, "RES_SCALE") && fabs(atof(m->val[i]) - atof(v)) < 0.01) return i;
    }
    return -1;
}
extern volatile int g_post_ao, g_post_bloom, g_post_grade, g_post_sharpen, g_post_ssr;   /* v1116A rsx_post.h; v1117F + sharpen; v1118A + wet */
static void menu_apply(int i, HWND hwnd)
{
    const MItem* m = &s_mi[i]; const char* v = m->val[m->cur];
    _putenv_s(m->key, v);
    if (!strcmp(m->key, "FULLSCREEN")) { if (hwnd && (atoi(v) != 0) != (g_win_fullscreen != 0)) PostMessageA(hwnd, WM_KEYDOWN, VK_F11, 0); }
    else if (!strcmp(m->key, "PRESENT_VSYNC")) g_set_vsync = atoi(v);
    else if (!strcmp(m->key, "UI_FPS_CAP")) g_ui_fps_cap = atoi(v);
    else if (!strcmp(m->key, "KBM_GLYPHS")) g_ui_kbm_glyphs = atoi(v);
    else if (!strcmp(m->key, "PAD_MOUSE_SPEED")) g_ui_mouse_speed = atoi(v);
    else if (!strcmp(m->key, "TEX_UPSCALE")) { extern volatile int g_set_texup; g_set_texup = atoi(v); }   /* v1114T */
    else if (!strcmp(m->key, "GF_POST_AO")) g_post_ao = atoi(v);          /* v1116A */
    else if (!strcmp(m->key, "GF_POST_BLOOM")) g_post_bloom = atoi(v);
    else if (!strcmp(m->key, "GF_POST_GRADE")) g_post_grade = atoi(v);
    else if (!strcmp(m->key, "GF_POST_SHARPEN")) g_post_sharpen = atoi(v);   /* v1117F */
    else if (!strcmp(m->key, "GF_POST_SSR")) g_post_ssr = atoi(v);   /* v1118A */
}
/* v1130M keep the settings.ini keys this menu does not own (the launcher's mod switches GF_SHOP / GF_TAILOR / GF_DIALOGUE, ...) */
static char s_ini_keep[48][256]; static int s_ini_keep_n;
static void menu_keep_read(void)
{
    FILE* f = fopen("settings.ini", "r"); char line[256]; s_ini_keep_n = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f) && s_ini_keep_n < 48) {
        char k[96]; const char* eq = strchr(line, '='); size_t n; int mine = 0;
        if (line[0] == ';' || line[0] == '#' || !eq) continue;
        n = (size_t)(eq - line); while (n && (line[n - 1] == ' ' || line[n - 1] == '\t')) n--;
        if (!n || n >= sizeof k) continue;
        memcpy(k, line, n); k[n] = 0;
        for (int i = 0; i < MI_N && !mine; i++) if (!strcmp(k, s_mi[i].key)) mine = 1;
        if (!strcmp(k, "COOP_NAME") || !strcmp(k, "COOP_ADDR")) mine = 1;
        if (mine) continue;
        n = strlen(line); while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        snprintf(s_ini_keep[s_ini_keep_n++], sizeof s_ini_keep[0], "%s", line);
    }
    fclose(f);
}
static void menu_save(void)
{
    menu_keep_read();   /* v1130M */
    FILE* f = fopen("settings.ini", "w");
    if (!f) { fprintf(stderr, "[settings] could not write settings.ini\n"); return; }
    fprintf(f, "; inFamous recomp settings (written by the in-game Settings menu, pause menu > Settings). Loaded at startup; overrides the launcher.\n");
    for (int i = 0; i < MI_N; i++) fprintf(f, "%s=%s\n", s_mi[i].key, s_mi[i].val[s_mi[i].cur]);
    { const uiov_coop_api* c = g_uiov_coop;   /* v1119M: the CO-OP tab's player name + last join address */
      if (c) { const char* n = c->name ? c->name() : NULL; const char* a = c->address ? c->address() : NULL;
          if (n && *n) fprintf(f, "COOP_NAME=%s\n", n);
          if (a && *a) fprintf(f, "COOP_ADDR=%s\n", a); } }
    for (int i = 0; i < s_ini_keep_n; i++) fprintf(f, "%s\n", s_ini_keep[i]);   /* v1130M */
    fclose(f);
    fprintf(stderr, "[settings] saved settings.ini\n");
}
static void menu_init(HWND hwnd)
{
    s_menu_inited = 1;
    for (int i = 0; i < MI_N; i++) {
        MItem* m = &s_mi[i];
        int k = mi_match(m, getenv(m->key));
        if (k < 0) {   /* not set: show the built-in default */
            if (!strcmp(m->key, "PAD_MOUSE_SPEED")) k = 2;
            else if (!strcmp(m->key, "TEX_ANISO")) k = 0;
            else k = 0;
        }
        m->cur = m->saved = k;
    }
    /* live settings from settings.ini take effect now (restart ones were read at startup) */
    { FILE* f = fopen("settings.ini", "r");
      if (f) { fclose(f);
          for (int i = 0; i < MI_N; i++) if (!s_mi[i].restart) menu_apply(i, hwnd); } }
}
static int s_menu_tab = 1;   /* v1119M: 0 = CO-OP, 1 = PC SETTINGS */
static int s_coop_sel, s_coop_dirty, s_edit; static char s_ebuf[64]; static ULONGLONG s_edit_t;
static void menu_open(void)
{
    s_menu_tab = g_uiov_coop ? 0 : 1; s_coop_sel = 0; s_edit = 0;
    s_menu_open = 1; g_uiov_menu_open = 1; s_menu_sel = 0; s_menu_confirm = 0; s_menu_t_open = GetTickCount64();
    s_menu_prev = 0xFFFFFFFFu;
    fprintf(stderr, "[settings] menu open\n");
}
static void menu_close(void)
{
    s_menu_open = 0; g_uiov_menu_open = 0; s_menu_close_ms = GetTickCount64(); s_menu_need_clear = 1; s_menu_quitonly = 0;
    int dirty = s_coop_dirty; s_coop_dirty = 0; s_edit = 0; for (int i = 0; i < MI_N; i++) if (s_mi[i].cur != s_mi[i].saved) dirty = 1;
    if (dirty) { menu_save(); for (int i = 0; i < MI_N; i++) s_mi[i].saved = s_mi[i].cur; }
    fprintf(stderr, "[settings] menu closed\n");
}
static void menu_change(int i, int d, HWND hwnd)
{
    MItem* m = &s_mi[i];
    m->cur = (m->cur + d + m->n) % m->n;
    menu_apply(i, hwnd);
    if (m->restart) s_restart_note = 1;
    fprintf(stderr, "[settings] %s = %s%s\n", m->key, m->val[m->cur], m->restart ? " (after restart)" : "");
}
/* ===== v1119M CO-OP tab (user 20:37: "get the co-op mechanic somewhere in the menu so players can join each other").
 * Rows: player name (text), host, join address (text), join, leave, back + three status lines from the title (rsx_ui_coop.h).
 * Text fields: Enter / A / click = edit; typing goes through ToUnicodeEx with the window's keyboard layout (Danish ':' =
 * Shift+.), Ctrl+V pastes, Enter keeps, Esc cancels. While a field is edited the menu keys (W/A/S/D/Z/X/C...) type instead. */
enum { CR_NAME, CR_HOST, CR_ADDR, CR_JOIN, CR_LEAVE, CR_BACK, CR_N };
static float s_tab_r[2][4];
static int coop_row_enabled(int r, int st)
{
    switch (r) {
    case CR_NAME: case CR_HOST: case CR_ADDR: case CR_JOIN: return st == UIOV_COOP_OFF;
    case CR_LEAVE: return st == UIOV_COOP_HOSTING || st == UIOV_COOP_CONNECTING || st == UIOV_COOP_CONNECTED;
    default: return 1;
    }
}
static int coop_state(void)
{
    const uiov_coop_api* c = g_uiov_coop; char a[4], b[4], d[4];
    return (c && c->status) ? c->status(a, 0, b, 0, d, 0) : UIOV_COOP_OFF;
}
static int edit_char_ok(int field, unsigned ch)
{
    if (ch < 32 || ch > 126) return 0;
    if (field == 1) return ch != '=' && ch != ';';   /* name */
    return isalnum((int)ch) || ch == '.' || ch == ':' || ch == '-' || ch == '_' || ch == '[' || ch == ']';   /* address / host name */
}
static void edit_begin(int field)
{
    const uiov_coop_api* c = g_uiov_coop; const char* v = NULL;
    if (!c) return;
    v = field == 1 ? (c->name ? c->name() : "") : (c->address ? c->address() : "");
    snprintf(s_ebuf, sizeof s_ebuf, "%s", v ? v : "");
    s_edit = field; s_edit_t = GetTickCount64();
}
static void edit_end(int keep)
{
    const uiov_coop_api* c = g_uiov_coop;
    if (keep && c) {
        if (s_edit == 1 && c->set_name && s_ebuf[0]) { c->set_name(s_ebuf); s_coop_dirty = 1; }
        if (s_edit == 2 && c->set_address) { c->set_address(s_ebuf); s_coop_dirty = 1; }
    }
    s_edit = 0; s_menu_prev = 0xFFFFFFFFu;   /* keys held now (Enter / Esc) must not act on the menu */
}
static void edit_tick(HWND hwnd)
{
    static unsigned char prev[256]; static int primed;
    const int fg = hwnd && GetForegroundWindow() == hwnd;
    unsigned char cur[256]; memset(cur, 0, sizeof cur);
    if (fg) for (int vk = 8; vk < 256; vk++) if (GetAsyncKeyState(vk) & 0x8000) cur[vk] = 1;
    if (!primed || GetTickCount64() - s_edit_t < 120) { memcpy(prev, cur, sizeof prev); primed = 1; return; }   /* the key that opened the field */
    const int maxlen = s_edit == 1 ? 20 : 63;
    size_t len = strlen(s_ebuf);
    const int ctrl = cur[VK_CONTROL] || cur[VK_LCONTROL] || cur[VK_RCONTROL];
    for (int vk = 8; vk < 256; vk++) {
        if (!cur[vk] || prev[vk]) continue;
        if (vk == VK_RETURN) { memcpy(prev, cur, sizeof prev); edit_end(1); return; }
        if (vk == VK_ESCAPE) { memcpy(prev, cur, sizeof prev); edit_end(0); return; }
        if (vk == VK_BACK) { if (len) s_ebuf[--len] = 0; continue; }
        if (ctrl && vk == 'V') {   /* paste */
            if (OpenClipboard(hwnd)) { HANDLE h = GetClipboardData(CF_TEXT); const char* t = h ? (const char*)GlobalLock(h) : NULL;
                if (t) { for (int i = 0; t[i] && (int)len < maxlen; i++) if (edit_char_ok(s_edit, (unsigned char)t[i])) { s_ebuf[len++] = t[i]; s_ebuf[len] = 0; } GlobalUnlock(h); }
                CloseClipboard(); }
            continue; }
        if (ctrl) continue;
        if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
            vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_TAB || (vk >= VK_LBUTTON && vk <= VK_XBUTTON2)) continue;
        {   BYTE ks[256]; WCHAR w[4]; memset(ks, 0, sizeof ks);
            if (cur[VK_SHIFT] || cur[VK_LSHIFT] || cur[VK_RSHIFT]) ks[VK_SHIFT] = 0x80;
            if (cur[VK_RMENU]) { ks[VK_CONTROL] = 0x80; ks[VK_MENU] = 0x80; }   /* AltGr */
            if (GetKeyState(VK_CAPITAL) & 1) ks[VK_CAPITAL] = 1;
            const HKL kl = GetKeyboardLayout(GetWindowThreadProcessId(hwnd, NULL));
            const int r = ToUnicodeEx((UINT)vk, MapVirtualKeyExA((UINT)vk, 0, kl), ks, w, 4, 4 /* do not change the keyboard state */, kl);
            if (r == 1 && w[0] < 127 && edit_char_ok(s_edit, (unsigned)w[0]) && (int)len < maxlen) { s_ebuf[len++] = (char)w[0]; s_ebuf[len] = 0; } }
    }
    memcpy(prev, cur, sizeof prev);
}
/* returns 1 if the menu closed */
static int coop_activate(int r, HWND hwnd)
{
    const uiov_coop_api* c = g_uiov_coop; const int st = coop_state();
    (void)hwnd;
    if (!c || !coop_row_enabled(r, st)) return 0;
    switch (r) {
    case CR_NAME: edit_begin(1); break;
    case CR_ADDR: edit_begin(2); break;
    case CR_HOST: if (c->host) c->host(); break;
    case CR_JOIN: { const char* a = c->address ? c->address() : NULL; if (a && *a) { if (c->join) c->join(a); s_coop_dirty = 1; } else edit_begin(2); } break;
    case CR_LEAVE: if (c->leave) c->leave(); break;
    case CR_BACK: menu_close(); return 1;
    }
    return 0;
}
/* virtual-px mouse position; 0 if the cursor is not over the game window */
static int mouse_virt(HWND hwnd, unsigned bbw, unsigned bbh, float* vx, float* vy)
{
    POINT pt; RECT rc;
    if (!hwnd || GetForegroundWindow() != hwnd || !GetCursorPos(&pt) || !ScreenToClient(hwnd, &pt) || !GetClientRect(hwnd, &rc) || rc.bottom <= 0) return 0;
    if (pt.x < 0 || pt.y < 0 || pt.x >= rc.right || pt.y >= rc.bottom) return 0;
    *vx = pt.x * (UIOV_VW_H / (float)rc.bottom) * ((float)rc.bottom / rc.right) * ((float)bbw / bbh);
    *vy = pt.y * (UIOV_VW_H / (float)rc.bottom);
    return 1;
}
/* returns 1 when the menu (or the hint) wants to draw this frame */
static int menu_tick(HWND hwnd, unsigned bbw, unsigned bbh)
{
    ULONGLONG now = GetTickCount64();
    if (!s_menu_inited) menu_init(hwnd);
    const int fg = hwnd && GetForegroundWindow() == hwnd;
    static int f10_was, sel_was, lb_was, rb_was;
    const int f10 = 0;   /* F10 = trainer page now; Settings opens from the pause menu (g_uiov_request) */
    const int paused = g_uiov_paused_fn ? g_uiov_paused_fn() : 0;
    if (!s_menu_open) {
        int open = f10 && !f10_was;
        /* v195: the pause menu's own "Settings" / "Quit Game" entries (native, stubs.cpp) ask for us */
        const int req = g_uiov_request;
        if (req) { g_uiov_request = 0; if (!D.active) open = req; }
        /* v195: the old "F10 / Back" hint is gone (UI_PAUSE_HINT=1 brings it back) */
        static int hint_env = -1;
        if (hint_env < 0) { const char* e = getenv("UI_PAUSE_HINT"); hint_env = e && *e == '1'; }
        s_hint_on = hint_env && paused && !D.active;
        s_wm_on = paused && !D.active && !g_fs_cutscene_open;   /* v238 pause-screen watermark; not in cutscenes */
        if (s_hint_on) {
            unsigned in = read_input(hwnd);   /* only polled while paused (XInput on empty slots is slow) */
            if ((in & IN_SELECT) && !sel_was) open = 1;
            sel_was = (in & IN_SELECT) != 0;
            const int lb = fg && (GetAsyncKeyState(VK_LBUTTON) & 0x8000);
            float vx, vy;
            if (lb && !lb_was && mouse_virt(hwnd, bbw, bbh, &vx, &vy) &&
                vx >= s_hint_r[0] && vx <= s_hint_r[2] && vy >= s_hint_r[1] && vy <= s_hint_r[3]) open = 1;
            lb_was = lb;
        } else sel_was = 0;
        f10_was = f10;
        if (open && !D.active) {
            menu_open();
            if (open == 2) { s_menu_quitonly = 1; s_menu_confirm = 1; s_menu_csel = 1; }
        }
        else return s_hint_on || s_wm_on;
    }
    /* ---- menu input ---- */
    if (s_edit) { edit_tick(hwnd); return 1; }   /* v1119M: a CO-OP text field owns the keyboard */
    unsigned in = read_input(hwnd);
    if (s_menu_prev == 0xFFFFFFFFu) s_menu_prev = in;   /* keys held at open are ignored */
    unsigned dn = in & ~s_menu_prev; s_menu_prev = in;
    if (f10 && !f10_was) { f10_was = f10; menu_close(); return 0; }
    f10_was = f10;
    if (now - s_menu_t_open < 150) dn = 0;
    if (s_menu_confirm) {
        if (dn & (IN_LEFT | IN_RIGHT | IN_UP | IN_DOWN | IN_TAB)) s_menu_csel ^= 1;
        if (dn & IN_BACK) { s_menu_confirm = 0; if (s_menu_quitonly) { menu_close(); return 0; } }
        { /* v195: mouse on the confirm buttons: hover selects, click activates */
          static int clb_was; float vx, vy;
          const int lb = fg && (GetAsyncKeyState(VK_LBUTTON) & 0x8000);
          if (mouse_virt(hwnd, bbw, bbh, &vx, &vy))
              for (int i = 0; i < 2; i++)
                  if (vx >= s_cbtn[i][0] && vx <= s_cbtn[i][2] && vy >= s_cbtn[i][1] && vy <= s_cbtn[i][3]) {
                      s_menu_csel = i; if (lb && !clb_was) dn |= IN_OK; }
          clb_was = lb; }
        if (dn & IN_OK) {
            if (s_menu_csel == 0) {   /* Quit: the v189c window-close path ends the process */
                fprintf(stderr, "[settings] Quit Game\n");
                menu_close();
                if (hwnd) PostMessageA(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            s_menu_confirm = 0;
            if (s_menu_quitonly) { menu_close(); return 0; }
        }
    } else if (s_menu_tab == 0 && g_uiov_coop) {   /* v1119M CO-OP tab */
        if (dn & IN_TAB) { s_menu_tab = 1; s_menu_sel = 0; }
        else {
            if (dn & IN_UP)   s_coop_sel = (s_coop_sel + CR_N - 1) % CR_N;
            if (dn & IN_DOWN) s_coop_sel = (s_coop_sel + 1) % CR_N;
            if ((dn & IN_OK) && coop_activate(s_coop_sel, hwnd)) return 0;
            if (dn & (IN_BACK | IN_SELECT)) { menu_close(); return 0; }
        }
    } else {
        if ((dn & IN_TAB) && g_uiov_coop) { s_menu_tab = 0; dn = 0; }   /* v1119M */
        if (dn & IN_UP)   s_menu_sel = (s_menu_sel + MR_N - 1) % MR_N;
        if (dn & IN_DOWN) s_menu_sel = (s_menu_sel + 1) % MR_N;
        if (s_menu_sel < MI_N) {
            if (dn & IN_LEFT)  menu_change(s_menu_sel, -1, hwnd);
            if (dn & (IN_RIGHT | IN_OK)) menu_change(s_menu_sel, +1, hwnd);
        } else if (dn & IN_OK) {
            if (s_menu_sel == MR_RESUME) { menu_close(); return 0; }
            if (s_menu_sel == MR_QUIT) { s_menu_confirm = 1; s_menu_csel = 1; }
        }
        if (dn & (IN_BACK | IN_SELECT)) { menu_close(); return 0; }
    }
    /* mouse: hover selects, left click = next value / activate, right click = previous value */
    { float vx, vy; static POINT s_last;
      const int lb = fg && (GetAsyncKeyState(VK_LBUTTON) & 0x8000), rb = fg && (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
      if (!s_menu_confirm && g_uiov_coop && lb && !lb_was && mouse_virt(hwnd, bbw, bbh, &vx, &vy)) {   /* v1119M tab headers */
          for (int t = 0; t < 2; t++) if (vx >= s_tab_r[t][0] && vx <= s_tab_r[t][2] && vy >= s_tab_r[t][1] && vy <= s_tab_r[t][3] && t != s_menu_tab) {
              s_menu_tab = t; s_menu_sel = 0; s_coop_sel = 0; lb_was = lb; rb_was = rb; return 1; } }
      if (!s_menu_confirm && s_menu_tab == 0 && g_uiov_coop) {   /* v1119M CO-OP rows: hover selects, click activates */
          if (mouse_virt(hwnd, bbw, bbh, &vx, &vy) && vx >= s_row_x0 && vx <= s_row_x1) {
              POINT pt; GetCursorPos(&pt);
              for (int r = 0; r < CR_N; r++) if (vy >= s_row_y0[r] && vy <= s_row_y1[r]) {
                  if (pt.x != s_last.x || pt.y != s_last.y) s_coop_sel = r;
                  if (lb && !lb_was) { s_last = pt; lb_was = lb; rb_was = rb; if (coop_activate(r, hwnd)) return 0; return 1; } }
              s_last = pt; }
      } else
      if (!s_menu_confirm && mouse_virt(hwnd, bbw, bbh, &vx, &vy) && vx >= s_row_x0 && vx <= s_row_x1) {
          POINT pt; GetCursorPos(&pt);
          for (int r = 0; r < MR_N; r++) if (vy >= s_row_y0[r] && vy <= s_row_y1[r]) {
              if (pt.x != s_last.x || pt.y != s_last.y) s_menu_sel = r;
              if (lb && !lb_was) { if (r < MI_N) menu_change(r, +1, hwnd);
                                   else if (r == MR_RESUME) { s_last = pt; lb_was = lb; menu_close(); return 0; }
                                   else { s_menu_confirm = 1; s_menu_csel = 1; } }
              if (rb && !rb_was && r < MI_N) menu_change(r, -1, hwnd);
          }
          s_last = pt;
      } else if (s_menu_confirm && lb && !lb_was) { s_menu_csel ^= 0; }
      lb_was = lb; rb_was = rb; }
    return 1;
}
/* v238 PAUSE WATERMARK: faint centred title in the pause screen's empty top black bar (the game draws nothing there).
 * UI_WATERMARK=<text> overrides it, UI_WATERMARK=0 turns it off; UI_WATERMARK_ALPHA (default 0.22). Fades in 0.3 s. */
static void build_watermark(void)
{
    static const char* txt = NULL; static float a_max = 0.22f;
    if (!txt) { const char* e = getenv("UI_WATERMARK"); txt = (e && *e) ? e : "Infamous Reborn";
                if (!strcmp(txt, "0")) txt = "";
                e = getenv("UI_WATERMARK_ALPHA"); if (e && *e) a_max = (float)atof(e);
                if (a_max < 0.0f) a_max = 0.0f; if (a_max > 1.0f) a_max = 1.0f; }
    if (!*txt) return;
    static ULONGLONG t_on = 0; const ULONGLONG now = GetTickCount64();
    if (!t_on || now - s_wm_last > 200) t_on = now;   /* new pause -> restart the fade */
    s_wm_last = now;
    float k = (float)(now - t_on) / 300.0f; if (k > 1.0f) k = 1.0f;
    const float sz = 34.0f, w = text_width(txt, (int)strlen(txt), sz);
    float c[4]; rgba(c, 1.0f, 1.0f, 1.0f, a_max * k);
    text_draw(txt, (int)strlen(txt), (s_vw - w) * 0.5f, 42.0f - sz * 0.62f, sz, c);   /* bar = top ~80 of 720 */
}
static void build_hint(void)
{
    const char* t = "Back   Settings & Quit";
    float sz = 22.0f, w = text_width(t, (int)strlen(t), sz), x1 = s_vw - 40.0f, x0 = x1 - w - 32.0f, y0 = 30.0f, y1 = y0 + 40.0f;   /* top-right: the game's pause screen keeps that black bar empty */
    float c0[4], c1[4], wt[4];
    rgba(c0, 0, 0, 0, 0.55f); rect(x0, y0, x1, y1, c0, c0, 0);
    rgba(c1, 0.36f, 0.72f, 1.0f, 0.9f); rect(x0, y1 - 2, x1, y1, c1, c1, 0);
    rgba(wt, 0.95f, 0.95f, 0.95f, 1.0f);
    text_draw(t, (int)strlen(t), x0 + 16.0f, y0 + 20.0f - sz * 0.62f, sz, wt);
    s_hint_r[0] = x0; s_hint_r[1] = y0; s_hint_r[2] = x1; s_hint_r[3] = y1;
}
/* ===== v1045G Godfather pause-menu look (from the user's screenshot of the game's own PAUSE MENU): dark grey outer
 * frame, light grey title bar with dark letter-spaced caps, black body inside a thin light grey border, light grey caps
 * for items, the selected item in the game's red, button prompts under the box. Shared by PC Settings and the trainer.
 * Text: assets/ui_font.bin (Nimbus Sans Narrow Bold ~ the game's Trade Gothic Cn); the game's wide Magnesium caps are
 * approximated with letter spacing. */
static float GF_RED[4] = { 0.70f, 0.09f, 0.08f, 1.0f }, GF_ITEM[4] = { 0.80f, 0.80f, 0.80f, 1.0f },
             GF_DIM[4] = { 0.50f, 0.50f, 0.50f, 1.0f }, GF_TITLE[4] = { 0.09f, 0.09f, 0.09f, 1.0f },
             GF_GOLD[4] = { 0.86f, 0.70f, 0.34f, 1.0f }, GF_PROMPT[4] = { 0.86f, 0.86f, 0.86f, 1.0f };
static void gf_upper(char* d, size_t n, const char* s) { size_t i = 0; for (; s[i] && i + 1 < n; i++) d[i] = (char)toupper((unsigned char)s[i]); d[i] = 0; }
static float gf_sp_w(const char* s, float size, float sp)
{
    float w = 0.0f; int n = (int)strlen(s);
    for (int i = 0; i < n; i++) w += text_width(s + i, 1, size) + (i + 1 < n ? sp : 0.0f);
    return w;
}
static void gf_sp(const char* s, float x, float y, float size, float sp, const float c[4])
{
    for (int i = 0; s[i]; i++) { text_draw(s + i, 1, x, y, size, c); x += text_width(s + i, 1, size) + sp; }
}
static void gf_sp_c(const char* s, float cx, float y, float size, float sp, const float c[4]) { gf_sp(s, cx - gf_sp_w(s, size, sp) * 0.5f, y, size, sp, c); }
static void gf_sp_r(const char* s, float xr, float y, float size, float sp, const float c[4]) { gf_sp(s, xr - gf_sp_w(s, size, sp), y, size, sp, c); }
/* the box; returns the inner (black) body rectangle */
static void gf_box(float x0, float y0, float x1, float y1, const char* title, float* bx0, float* by0, float* bx1, float* by1)
{
    float c0[4], c1[4];
    rgba(c0, 0.24f, 0.24f, 0.24f, 1); rect(x0, y0, x1, y1, c0, c0, 0);                       /* outer frame */
    const float tb = 40.0f, fx0 = x0 + 12.0f, fx1 = x1 - 12.0f, ty0 = y0 + 10.0f, ty1 = ty0 + tb;
    rgba(c0, 0.66f, 0.66f, 0.66f, 1); rgba(c1, 0.58f, 0.58f, 0.58f, 1); rect(fx0, ty0, fx1, ty1, c0, c1, 0);   /* title bar */
    rgba(c0, 0.74f, 0.74f, 0.74f, 1); rect(fx0, ty1, fx1, y1 - 10.0f, c0, c0, 0);             /* light border */
    rgba(c0, 0, 0, 0, 1); rect(fx0 + 5.0f, ty1, fx1 - 5.0f, y1 - 15.0f, c0, c0, 0);           /* black body */
    if (title) { char t[64]; gf_upper(t, sizeof t, title); gf_sp_c(t, (x0 + x1) * 0.5f, ty0 + 9.0f, 25.0f, 4.0f, GF_TITLE); }
    *bx0 = fx0 + 5.0f; *by0 = ty1; *bx1 = fx1 - 5.0f; *by1 = y1 - 15.0f;
}
static void build_menu(void)
{
    const float cx = s_vw * 0.5f;
    float c0[4];
    if (!s_menu_quitonly) {
        rgba(c0, 0, 0, 0, 0.80f); rect(0, 0, s_vw, UIOV_VW_H, c0, c0, 0);   /* the game's pause screen behind goes dark */
        const float x0 = cx - 470.0f, x1 = cx + 470.0f, y0 = 46.0f, y1 = 610.0f;
        float bx0, by0, bx1, by1;
        gf_box(x0, y0, x1, y1, g_uiov_coop ? NULL : "PC Settings", &bx0, &by0, &bx1, &by1);
        if (g_uiov_coop) {   /* v1119M tabs in the title bar: CO-OP | PC SETTINGS */
            static const char* const tn[2] = { "CO-OP", "PC SETTINGS" };
            const float tsz = 25.0f, gap = 70.0f, w0 = gf_sp_w(tn[0], tsz, 4.0f), w1 = gf_sp_w(tn[1], tsz, 4.0f), tx0 = cx - (w0 + gap + w1) * 0.5f, ty = y0 + 19.0f;
            float dim[4] = { 0.33f, 0.33f, 0.33f, 1.0f };
            for (int t = 0; t < 2; t++) {
                const float tx = t ? tx0 + w0 + gap : tx0, tw = t ? w1 : w0;
                gf_sp(tn[t], tx, ty, tsz, 4.0f, t == s_menu_tab ? GF_TITLE : dim);
                if (t == s_menu_tab) { float u[4]; rgba(u, 0.70f, 0.09f, 0.08f, 1.0f); rect(tx, ty + 27.0f, tx + tw, ty + 30.0f, u, u, 0); }
                s_tab_r[t][0] = tx - 12.0f; s_tab_r[t][1] = y0 + 8.0f; s_tab_r[t][2] = tx + tw + 12.0f; s_tab_r[t][3] = y0 + 52.0f;
            }
            gf_sp("TAB", x0 + 30.0f, ty + 4.0f, 16.0f, 1.0f, dim); gf_sp_r("LB / RB", x1 - 30.0f, ty + 4.0f, 16.0f, 1.0f, dim);
        }
        if (s_menu_tab == 0 && g_uiov_coop) {   /* v1119M CO-OP body */
            const uiov_coop_api* c = g_uiov_coop;
            char l1[160], l2[160], l3[160], up[200];
            const int st = c->status ? c->status(l1, sizeof l1, l2, sizeof l2, l3, sizeof l3) : UIOV_COOP_OFF;
            const float rh = 40.0f, lsz = rh * 0.62f, lx = bx0 + 46.0f, rx = bx1 - 46.0f;
            s_row_x0 = bx0; s_row_x1 = bx1;
            float y = by0 + 26.0f;
            static const char* const lab[CR_N] = { "Player Name", "Host a Game", "Join Address", "Join Game", "Leave Co-op", "Back" };
            for (int r = 0; r < CR_N; r++) {
                if (r == CR_ADDR || r == CR_LEAVE || r == CR_BACK) y += 12.0f;
                const int en = coop_row_enabled(r, st), on = r == s_coop_sel;
                const float* col = !en ? GF_DIM : on ? GF_RED : GF_ITEM;
                s_row_y0[r] = y - 4.0f; s_row_y1[r] = y + rh - 4.0f;
                gf_upper(up, sizeof up, lab[r]);
                if (r == CR_BACK) gf_sp_c(up, cx, y, lsz, 2.5f, col); else gf_sp(up, lx, y, lsz, 2.0f, col);
                if (r == CR_NAME || r == CR_ADDR) {   /* value / text field */
                    const int ed = s_edit == (r == CR_NAME ? 1 : 2);
                    const char* v = ed ? s_ebuf : (r == CR_NAME ? (c->name ? c->name() : "") : (c->address ? c->address() : ""));
                    char vb[80]; snprintf(vb, sizeof vb, "%s", (v && *v) ? v : (r == CR_ADDR ? "(type the host's IP address)" : ""));
                    const float vw = text_width(vb, (int)strlen(vb), lsz);
                    if (ed) {   /* field box + blinking caret */
                        float fb[4], fl[4]; rgba(fb, 0.10f, 0.10f, 0.10f, 1); rgba(fl, 0.70f, 0.09f, 0.08f, 1);
                        const float fx0 = rx - 430.0f; rect(fx0 - 10.0f, y - 6.0f, rx + 10.0f, y + rh - 6.0f, fl, fl, 0); rect(fx0 - 8.0f, y - 4.0f, rx + 8.0f, y + rh - 8.0f, fb, fb, 0);
                        text_draw(vb, (int)strlen(vb), rx - vw - 10.0f, y, lsz, GF_PROMPT);
                        if ((GetTickCount64() / 450) & 1) { float cc[4]; rgba(cc, 0.95f, 0.95f, 0.95f, 1); rect(rx - 6.0f, y + 2.0f, rx - 4.0f, y + lsz + 4.0f, cc, cc, 0); }
                    } else text_draw(vb, (int)strlen(vb), rx - vw, y, lsz, (v && *v) ? col : GF_DIM);
                }
                y += rh;
            }
            /* status block */
            {   float sy = by1 - 118.0f; float sep[4]; rgba(sep, 0.30f, 0.30f, 0.30f, 1); rect(bx0 + 30.0f, sy - 14.0f, bx1 - 30.0f, sy - 12.0f, sep, sep, 0);
                const float* sc = st == UIOV_COOP_CONNECTED || st == UIOV_COOP_HOSTING ? GF_GOLD : GF_ITEM;
                gf_upper(up, sizeof up, l1); gf_sp_c(up, cx, sy, 21.0f, 1.2f, sc);
                gf_upper(up, sizeof up, l2); gf_sp_c(up, cx, sy + 32.0f, 17.0f, 1.0f, GF_ITEM);
                gf_upper(up, sizeof up, l3); gf_sp_c(up, cx, sy + 58.0f, 15.0f, 1.0f, GF_DIM); }
            {   static const char* const pr0[3] = { "Up/Down  Select", "Enter  Choose", "Esc  Back" };
                static const char* const pr1[3] = { "Type  Edit", "Enter  Keep", "Esc  Cancel" };
                const char* const* pr = s_edit ? pr1 : pr0;
                for (int i = 0; i < 3; i++) gf_sp_c(pr[i], cx + 380.0f * (float)(i - 1), y1 + 26.0f, 21.0f, 0.5f, GF_PROMPT); }
            return;
        }
        float rh = (by1 - by0 - 70.0f) / (float)MR_N; if (rh > 38.0f) rh = 38.0f;
        const float lsz = rh * 0.62f, lx = bx0 + 46.0f, rx = bx1 - 46.0f;
        s_row_x0 = bx0; s_row_x1 = bx1;
        float y = by0 + 22.0f;
        for (int r = 0; r < MR_N; r++) {
            if (r == MR_RESUME) y += 10.0f;
            const int on = (r == s_menu_sel) && !s_menu_confirm;
            s_row_y0[r] = y - 4.0f; s_row_y1[r] = y + rh - 4.0f;
            const float* col = on ? GF_RED : GF_ITEM;
            char lab[64];
            if (r < MI_N) {
                const MItem* m = &s_mi[r];
                gf_upper(lab, sizeof lab, m->label); gf_sp(lab, lx, y, lsz, 2.0f, col);
                if (m->restart) gf_sp("(RESTART)", lx + gf_sp_w(lab, lsz, 2.0f) + 12.0f, y + lsz * 0.28f, lsz * 0.62f, 1.0f, GF_DIM);
                char v[64]; gf_upper(v, sizeof v, m->name[m->cur]);
                if (on) {
                    const float aw = text_width(">", 1, lsz), vw = gf_sp_w(v, lsz, 1.5f);
                    text_draw(">", 1, rx - aw, y, lsz, GF_RED);
                    gf_sp(v, rx - aw - 14.0f - vw, y, lsz, 1.5f, GF_RED);
                    text_draw("<", 1, rx - aw - 28.0f - vw - aw, y, lsz, GF_RED);
                } else gf_sp_r(v, rx, y, lsz, 1.5f, GF_ITEM);
            } else {
                gf_upper(lab, sizeof lab, r == MR_RESUME ? "Back" : "Quit Game");
                gf_sp_c(lab, cx, y, lsz, 2.5f, col);
            }
            y += rh;
        }
        if (s_restart_note)
            gf_sp_c("SETTINGS MARKED (RESTART) APPLY THE NEXT TIME THE GAME STARTS.", cx, by1 - 30.0f, 15.0f, 1.0f, GF_DIM);
        if (!s_menu_confirm) {
            static const char* const pr[3] = { "Up/Down  Scroll", "Left/Right  Change", "Esc  Back" };
            for (int i = 0; i < 3; i++) gf_sp_c(pr[i], cx + 380.0f * (float)(i - 1), y1 + 26.0f, 21.0f, 0.5f, GF_PROMPT);
        }
    }
    if (s_menu_confirm) {   /* quit confirmation in the same style */
        rgba(c0, 0, 0, 0, 0.6f); rect(0, 0, s_vw, UIOV_VW_H, c0, c0, 0);
        const float x0 = cx - 330.0f, x1 = cx + 330.0f, y0 = UIOV_VW_H * 0.5f - 110.0f, y1 = UIOV_VW_H * 0.5f + 100.0f;
        float bx0, by0, bx1, by1;
        gf_box(x0, y0, x1, y1, "Quit Game", &bx0, &by0, &bx1, &by1);
        gf_sp_c("QUIT THE GAME? UNSAVED PROGRESS WILL BE LOST.", cx, by0 + 30.0f, 19.0f, 1.0f, GF_ITEM);
        const char* yn[2] = { "QUIT", "CANCEL" };
        for (int i = 0; i < 2; i++) {
            const float w = gf_sp_w(yn[i], 26.0f, 3.0f), bx = i ? cx + 70.0f : cx - 70.0f - w, by = by1 - 62.0f;
            gf_sp(yn[i], bx, by, 26.0f, 3.0f, i == s_menu_csel ? GF_RED : GF_ITEM);
            s_cbtn[i][0] = bx - 16; s_cbtn[i][1] = by - 8.0f; s_cbtn[i][2] = bx + w + 16; s_cbtn[i][3] = by + 34.0f;
        }
    }
}
/* ---------------- F10 trainer page (rsx_ui_trainer.h) ----------------
 * The title supplies the rows (g_uiov_trainer); this draws them in a left-side panel so the game stays visible, owns
 * the pad while open (uiov_pad_gate) and turns input into change() calls.  While closed it draws the title's short
 * "active cheats" line (hud) at the top centre, except on the pause screen. */
#define TR_MAX_ROWS 64
static int s_tr_sel = -1, s_tr_scroll;
static unsigned s_tr_prev, s_tr_rep_key;
static ULONGLONG s_tr_t_open, s_tr_rep_t;
static float s_tr_y0[TR_MAX_ROWS], s_tr_y1[TR_MAX_ROWS], s_tr_x0, s_tr_x1;   /* last layout, for the mouse */
static int s_tr_hud_on; static char s_tr_hud[128];
static int tr_row(const uiov_trainer_api* t, int i, uiov_trainer_row* r)
{
    memset(r, 0, sizeof *r);
    return t->row(i, r);
}
static int tr_selectable(const uiov_trainer_api* t, int i)
{
    uiov_trainer_row r;
    return tr_row(t, i, &r) && r.kind != UIOV_TR_HEADER;
}
static void tr_move(const uiov_trainer_api* t, int n, int d)
{
    if (n <= 0) { s_tr_sel = -1; return; }
    int i = s_tr_sel < 0 ? (d > 0 ? -1 : 0) : s_tr_sel;
    for (int k = 0; k < n; k++) { i = (i + d + n) % n; if (tr_selectable(t, i)) { s_tr_sel = i; return; } }
    s_tr_sel = -1;
}
static void tr_open(void)
{
    s_tr_open = 1; g_uiov_menu_open = 1; s_tr_t_open = GetTickCount64(); s_tr_prev = 0xFFFFFFFFu; s_tr_rep_key = 0;
    fprintf(stderr, "[trainer] open\n");
}
static void tr_close(void)
{
    s_tr_open = 0; g_uiov_menu_open = 0; s_tr_close_ms = GetTickCount64(); s_tr_need_clear = 1;
    fprintf(stderr, "[trainer] closed\n");
}
/* returns 1 when the page (or the active-cheat line) wants to draw this frame */
static int tr_tick(HWND hwnd, unsigned bbw, unsigned bbh)
{
    const uiov_trainer_api* t = g_uiov_trainer;
    static int f10_was, lb_was, rb_was;
    if (!t) { s_tr_hud_on = 0; if (s_tr_open) tr_close(); return 0; }
    const int fg = hwnd && GetForegroundWindow() == hwnd;
    const int f10 = fg && (GetAsyncKeyState(VK_F10) & 0x8000);
    const int press = f10 && !f10_was;
    f10_was = f10;
    if (!s_tr_open) {
        if (press && !D.active && !s_menu_open) tr_open();
        else { s_tr_hud_on = t->hud ? t->hud(s_tr_hud, sizeof s_tr_hud) : 0; return s_tr_hud_on; }
    } else if (press) { tr_close(); return 0; }
    s_tr_hud_on = 0;
    int n = t->count ? t->count() : 0; if (n > TR_MAX_ROWS) n = TR_MAX_ROWS;
    if (s_tr_sel < 0 || s_tr_sel >= n || !tr_selectable(t, s_tr_sel)) { s_tr_sel = -1; tr_move(t, n, +1); }
    /* keys / pad */
    const ULONGLONG now = GetTickCount64();
    unsigned in = read_input(hwnd);
    if (s_tr_prev == 0xFFFFFFFFu) s_tr_prev = in;   /* keys held at open are ignored */
    unsigned dn = in & ~s_tr_prev; s_tr_prev = in;
    if (now - s_tr_t_open < 150) dn = 0;
    { const unsigned dirs = in & (IN_UP | IN_DOWN | IN_LEFT | IN_RIGHT);   /* held direction auto-repeat */
      if (dn & dirs) { s_tr_rep_key = dn & dirs; s_tr_rep_t = now + 350; }
      else if (dirs & s_tr_rep_key) { if (now >= s_tr_rep_t) { dn |= dirs & s_tr_rep_key; s_tr_rep_t = now + 80; } }
      else s_tr_rep_key = 0; }
    if (dn & IN_UP)   tr_move(t, n, -1);
    if (dn & IN_DOWN) tr_move(t, n, +1);
    if (s_tr_sel >= 0) {
        uiov_trainer_row r;
        if (tr_row(t, s_tr_sel, &r)) {
            if (r.kind == UIOV_TR_CHOICE) {
                if (dn & IN_LEFT) t->change(s_tr_sel, -1);
                if (dn & (IN_RIGHT | IN_OK)) t->change(s_tr_sel, +1);
            } else if (r.kind == UIOV_TR_ACTION && (dn & IN_OK)) t->change(s_tr_sel, 0);
        }
    }
    if (dn & (IN_BACK | IN_SELECT)) { tr_close(); return 0; }
    /* mouse: hover selects, left click = next value / run, right click = previous value */
    { float vx, vy; static POINT s_last;
      const int lb = fg && (GetAsyncKeyState(VK_LBUTTON) & 0x8000), rb = fg && (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
      if (mouse_virt(hwnd, bbw, bbh, &vx, &vy) && vx >= s_tr_x0 && vx <= s_tr_x1) {
          POINT pt; GetCursorPos(&pt);
          for (int i = 0; i < n; i++) {
              if (!(s_tr_y1[i] > s_tr_y0[i] && vy >= s_tr_y0[i] && vy <= s_tr_y1[i])) continue;
              uiov_trainer_row r;
              if (!tr_row(t, i, &r) || r.kind == UIOV_TR_HEADER) continue;
              if (pt.x != s_last.x || pt.y != s_last.y) s_tr_sel = i;
              if (lb && !lb_was) { if (r.kind == UIOV_TR_CHOICE) t->change(i, +1); else if (r.kind == UIOV_TR_ACTION) t->change(i, 0); }
              if (rb && !rb_was && r.kind == UIOV_TR_CHOICE) t->change(i, -1);
          }
          s_last = pt;
      }
      lb_was = lb; rb_was = rb; }
    return 1;
}
static void build_trainer(void)
{
    const uiov_trainer_api* t = g_uiov_trainer;
    if (!t) return;
    const float px0 = 36.0f, px1 = px0 + 560.0f, top = 34.0f, bot = UIOV_VW_H - 82.0f;
    float bx0, by0, bx1, by1;
    gf_box(px0, top, px1, bot, t->title ? t->title : "Trainer", &bx0, &by0, &bx1, &by1);
    const float x0 = bx0 + 28.0f, x1 = bx1 - 28.0f;
    float y = by0 + 12.0f;
    { char st[128]; if (t->status && t->status(st, sizeof st)) { char u[128]; gf_upper(u, sizeof u, st); gf_sp(u, x0, y, 15.0f, 1.0f, GF_DIM); } }
    y += 30.0f;
    const float rh = 29.0f, lsz = 19.0f, list_bot = by1 - 8.0f;
    s_tr_x0 = px0; s_tr_x1 = px1;
    int n = t->count ? t->count() : 0; if (n > TR_MAX_ROWS) n = TR_MAX_ROWS;
    int vis = (int)((list_bot - y) / rh); if (vis < 1) vis = 1;
    if (s_tr_sel >= 0) {
        if (s_tr_sel < s_tr_scroll) s_tr_scroll = s_tr_sel;
        if (s_tr_sel >= s_tr_scroll + vis) s_tr_scroll = s_tr_sel - vis + 1;
        if (s_tr_scroll > 0 && s_tr_sel == s_tr_scroll) {
            uiov_trainer_row h; if (tr_row(t, s_tr_scroll - 1, &h) && h.kind == UIOV_TR_HEADER) s_tr_scroll--; }
    }
    if (s_tr_scroll > n - vis) s_tr_scroll = n - vis > 0 ? n - vis : 0;
    for (int i = 0; i < TR_MAX_ROWS; i++) s_tr_y0[i] = s_tr_y1[i] = 0.0f;
    if (s_tr_scroll > 0) gf_sp_r("...", x1, y - 22.0f, lsz, 1.0f, GF_DIM);
    for (int i = s_tr_scroll; i < n && i < s_tr_scroll + vis; i++, y += rh) {
        uiov_trainer_row r;
        if (!tr_row(t, i, &r)) continue;
        char u[64];
        if (r.kind == UIOV_TR_HEADER) {   /* section: small grey caps over a grey rule */
            gf_upper(u, sizeof u, r.label);
            const float w = gf_sp_w(u, 14.0f, 3.0f);
            gf_sp(u, x0, y + 9.0f, 14.0f, 3.0f, GF_DIM);
            float c0[4]; rgba(c0, 0.40f, 0.40f, 0.40f, 1); rect(x0 + w + 12.0f, y + 17.0f, x1, y + 18.0f, c0, c0, 0);
            continue;
        }
        const int on = i == s_tr_sel, locked = r.kind == UIOV_TR_LOCKED;
        s_tr_y0[i] = y - 4.0f; s_tr_y1[i] = y + rh - 4.0f;
        gf_upper(u, sizeof u, r.label);
        gf_sp(u, x0, y, lsz, 1.5f, locked ? GF_DIM : on ? GF_RED : GF_ITEM);
        if (r.value[0]) {
            const float vsz = lsz * 0.9f, vy = y + (lsz - vsz) * 0.5f;
            const int act = r.kind == UIOV_TR_CHOICE && r.on;
            const float* vc = locked ? GF_DIM : act ? GF_GOLD : on ? GF_RED : GF_DIM;
            const int vl = (int)strlen(r.value);
            float vx = x1 - text_width(r.value, vl, vsz);
            if (r.kind == UIOV_TR_CHOICE && on) {
                const float aw = text_width(">", 1, vsz);
                text_draw(">", 1, x1 - aw, vy, vsz, GF_RED);
                vx -= aw + 10.0f;
                text_draw("<", 1, vx - aw - 10.0f, vy, vsz, GF_RED);
            }
            text_draw(r.value, vl, vx, vy, vsz, vc);
        }
    }
    if (s_tr_scroll + vis < n) gf_sp_r("...", x1, y - 6.0f, lsz, 1.0f, GF_DIM);
    static const char* const pr[4] = { "Up/Down  Scroll", "Left/Right  Change", "Enter  Select", "F10  Back" };
    for (int i = 0; i < 4; i++) gf_sp(pr[i], px0 + 4.0f + 142.0f * (float)i, bot + 22.0f, 17.0f, 0.4f, GF_PROMPT);
}
static void tr_hud_build(void)
{
    if (!s_tr_hud_on || !s_tr_hud[0]) return;
    const int len = (int)strlen(s_tr_hud);
    const float sz = 17.0f, w = text_width(s_tr_hud, len, sz), x0 = (s_vw - w) * 0.5f - 12.0f, y0 = 6.0f;
    float bg[4], fg[4];
    rgba(bg, 0, 0, 0, 0.45f); rgba(fg, 0.55f, 1.0f, 0.55f, 0.92f);
    rect(x0, y0, x0 + w + 24.0f, y0 + 28.0f, bg, bg, 0);
    text_draw(s_tr_hud, len, x0 + 12.0f, y0 + 14.0f - sz * 0.62f, sz, fg);
}
/* ---------------- v1092S inventory / store pages (rsx_ui_shop.h) ----------------
 * Same look as the game's pause menu (gf_box): grey title bar, black body, light grey caps, the selection in the game's red,
 * cash in gold on the heading line, the selected item's description in a tip strip at the bottom of the box (like the
 * pause menu's hint strip, red arrows either side), button prompts under the box. I = inventory, B = store (when the title
 * says one is here), Tab switches between them, Enter / click = use / buy, Esc / the same key = close.
 * While closed: the title's store prompt ("[B] SHOP - TOBACCONIST") at the top centre in the style of the game's
 * "[E] OPEN DOOR", and short toasts ("BOUGHT CIGARETTES  -$15") under it. */
const uiov_shop_api* volatile g_uiov_shop = 0;
volatile int g_uiov_shop_solo = 0;   /* v1112J: the title's store page stands alone (tailor): no Tab / I to the pockets, no TAB hint */
#define SP_MAX_ROWS 40
static volatile int s_sp_page = 0;               /* 0 closed, 1 inventory, 2 store */
static volatile ULONGLONG s_sp_close_ms = 0; static volatile int s_sp_need_clear = 0;
static int s_sp_sel[3], s_sp_scroll[3];
static unsigned s_sp_prev, s_sp_rep_key; static ULONGLONG s_sp_t_open, s_sp_rep_t;
static float s_sp_y0[SP_MAX_ROWS], s_sp_y1[SP_MAX_ROWS], s_sp_x0, s_sp_x1;
static int s_sp_prompt_on; static char s_sp_prompt[96];
static char s_sp_toast[128]; static ULONGLONG s_sp_toast_t;
static int sp_key(int vk_env_default, const char* env)
{
    static int cache[2] = { -1, -1 }; const int slot = vk_env_default == 'I' ? 0 : 1;
    if (cache[slot] < 0) { const char* e = getenv(env); cache[slot] = (e && isalnum((unsigned char)e[0])) ? toupper((unsigned char)e[0]) : vk_env_default; }
    return cache[slot];
}
static void sp_open(int page)
{
    s_sp_page = page; s_sp_page_g = page; g_uiov_menu_open = 1; s_sp_t_open = GetTickCount64(); s_sp_prev = 0xFFFFFFFFu; s_sp_rep_key = 0;
    fprintf(stderr, "[shop-ui] open page %d\n", page);
}
static void sp_close(void)
{
    s_sp_page = 0; s_sp_page_g = 0; g_uiov_menu_open = 0; s_sp_close_ms = GetTickCount64(); s_sp_need_clear = 1; s_sp_close_g = s_sp_close_ms; s_sp_clear_g = 1;
    fprintf(stderr, "[shop-ui] closed\n");
}
static void sp_toast_poll(void)
{
    const uiov_shop_api* t = g_uiov_shop; char m[128];
    if (t && t->toast && t->toast(m, sizeof m) && m[0]) { snprintf(s_sp_toast, sizeof s_sp_toast, "%s", m); s_sp_toast_t = GetTickCount64(); }
}
static int sp_tick(HWND hwnd, unsigned bbw, unsigned bbh)
{
    const uiov_shop_api* t = g_uiov_shop;
    static int ki_was, kb_was, tab_was, lb_was;
    if (!t) { if (s_sp_page) sp_close(); return 0; }
    sp_toast_poll();
    { const int rq = t->request ? t->request() : 0;   /* the title opens / closes a page (tests, talking to a shopkeeper) */
      if (rq > 0 && rq != s_sp_page) { if (!s_sp_page) sp_open(rq); else s_sp_page = rq; } else if (rq < 0 && s_sp_page) { sp_close(); return 0; } }
    const int fg = hwnd && GetForegroundWindow() == hwnd;
    const int ki = fg && (GetAsyncKeyState(sp_key('I', "GF_INV_KEY")) & 0x8000), kb = fg && (GetAsyncKeyState(sp_key('B', "GF_SHOP_KEY")) & 0x8000);
    int pi = ki && !ki_was, pb = kb && !kb_was; ki_was = ki; kb_was = kb;
    { /* v1093M controller: HOLD d-pad left (GF_INV_HOLD_MS, 450) = inventory, hold d-pad right = store (when one is here) */
      extern volatile unsigned g_pad_phys_buttons; extern volatile unsigned long long g_pad_phys_ms;
      static ULONGLONG tl, tr; static int fl, fr; static int hold_ms = -1;
      if (hold_ms < 0) { const char* e = getenv("GF_INV_HOLD_MS"); hold_ms = e && *e ? atoi(e) : 450; }
      const ULONGLONG nw = GetTickCount64(); const int fresh = nw - g_pad_phys_ms < 500;
      const int hl = fresh && (g_pad_phys_buttons & 0x0080), hr = fresh && (g_pad_phys_buttons & 0x0020);
      if (hl) { if (!tl) tl = nw; if (!fl && nw - tl >= (ULONGLONG)hold_ms) { fl = 1; pi = 1; } } else { tl = 0; fl = 0; }
      if (hr) { if (!tr) tr = nw; if (!fr && nw - tr >= (ULONGLONG)hold_ms) { fr = 1; pb = 1; } } else { tr = 0; fr = 0; } }
    s_sp_prompt_on = (!s_sp_page && t->prompt) ? t->prompt(s_sp_prompt, sizeof s_sp_prompt) : 0;
    const ULONGLONG now = GetTickCount64();
    if (!s_sp_page) {
        const int ok_now = !D.active && !s_menu_open && !s_tr_open && (!t->can_open || t->can_open());
        if (pi && ok_now) sp_open(1);
        else if (pb && ok_now && s_sp_prompt_on) sp_open(2);
        else return s_sp_prompt_on || (s_sp_toast[0] && now - s_sp_toast_t < 2600);
    } else if ((pi && s_sp_page == 1) || (pb && s_sp_page == 2)) { sp_close(); return 0; }
    else if (pi && !(g_uiov_shop_solo && s_sp_page == 2)) s_sp_page = 1;
    else if (pb && t->prompt && t->prompt(s_sp_prompt, sizeof s_sp_prompt)) s_sp_page = 2;
    const int pg = s_sp_page;
    int n = t->count ? t->count(pg) : 0; if (n > SP_MAX_ROWS) n = SP_MAX_ROWS;
    if (s_sp_sel[pg] >= n) s_sp_sel[pg] = n - 1;
    if (s_sp_sel[pg] < 0) s_sp_sel[pg] = 0;
    unsigned in = read_input(hwnd);
    if (s_sp_prev == 0xFFFFFFFFu) s_sp_prev = in;
    unsigned dn = in & ~s_sp_prev; s_sp_prev = in;
    if (now - s_sp_t_open < 150) dn = 0;
    { const unsigned dirs = in & (IN_UP | IN_DOWN | (t->adjust ? (IN_LEFT | IN_RIGHT) : 0u));   /* v1116B: + Left / Right repeat for adjust */
      if (dn & dirs) { s_sp_rep_key = dn & dirs; s_sp_rep_t = now + 350; }
      else if (dirs & s_sp_rep_key) { if (now >= s_sp_rep_t) { dn |= dirs & s_sp_rep_key; s_sp_rep_t = now + 90; } }
      else s_sp_rep_key = 0; }
    if (n > 0) {
        if (dn & IN_UP) s_sp_sel[pg] = (s_sp_sel[pg] + n - 1) % n;
        if (dn & IN_DOWN) s_sp_sel[pg] = (s_sp_sel[pg] + 1) % n;
        if (dn & IN_OK) t->activate(pg, s_sp_sel[pg]);
        if (t->adjust && (dn & IN_LEFT)) t->adjust(pg, s_sp_sel[pg], -1);    /* v1116B */
        if (t->adjust && (dn & IN_RIGHT)) t->adjust(pg, s_sp_sel[pg], +1);
    }
    if (t->selected && n > 0) { const int ns = t->selected(pg, s_sp_sel[pg]); if (ns >= 0 && ns < n) s_sp_sel[pg] = ns; }   /* v1112A tailor try-on */
    if ((dn & IN_TAB) && !(g_uiov_shop_solo && pg == 2)) { if (pg == 2) s_sp_page = 1; else if (t->prompt && t->prompt(s_sp_prompt, sizeof s_sp_prompt)) s_sp_page = 2; }   /* Tab / L1 / R1 */
    (void)tab_was;
    if (dn & IN_BACK) { if (!(t->back && t->back(pg))) { sp_close(); return 0; } }   /* v1112A tailor: Esc = up one level */
    { float vx, vy; static POINT s_last;   /* mouse: hover selects, click = use / buy */
      const int lb = fg && (GetAsyncKeyState(VK_LBUTTON) & 0x8000);
      if (mouse_virt(hwnd, bbw, bbh, &vx, &vy) && vx >= s_sp_x0 && vx <= s_sp_x1) {
          POINT pt; GetCursorPos(&pt);
          for (int i = 0; i < n; i++) {
              if (!(s_sp_y1[i] > s_sp_y0[i] && vy >= s_sp_y0[i] && vy <= s_sp_y1[i])) continue;
              if (pt.x != s_last.x || pt.y != s_last.y) s_sp_sel[pg] = i;
              if (lb && !lb_was && now - s_sp_t_open > 200) t->activate(pg, i);
          }
          s_last = pt;
      }
      lb_was = lb; }
    return 1;
}
static void sp_money_fmt(const char* in, char* out, size_t n) { gf_upper(out, n, in); }
/* ---- v1098S store / inventory as a modern side sheet (user picked mockup "E"): translucent warm-black sheet down the left
 * edge with a thin gold edge, gold letter-spaced eyebrow, big serif title (P052 Bold, atlas codes 0xF100+c), gold rule with
 * a small diamond, rows with a soft crimson fill on the selection, the selection's description in serif italic (0xF200+c)
 * at the bottom, cash in gold, outlined key caps. GF_SHOP_UI=classic = the v1092S pause-menu box. */
static int sf_ok(void) { return glyph(0xF100u + 'A', 0) && glyph(0xF200u + 'a', 0); }
static void sf_str(const char* s, int style, char* o, size_t cap)
{
    size_t j = 0; const unsigned base = style == 2 ? 0xF200u : 0xF100u;
    for (const unsigned char* p = (const unsigned char*)s; *p && j + 4 < cap; p++) {
        if (*p >= 32 && *p < 127) { const unsigned cp = base + *p; o[j++] = (char)(0xE0 | (cp >> 12)); o[j++] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[j++] = (char)(0x80 | (cp & 0x3F)); }
        else o[j++] = (char)*p;
    }
    o[j] = 0;
}
static float sf_w(const char* s, int style, float size)
{
    if (!sf_ok()) return text_width(s, (int)strlen(s), size);
    char b[720]; sf_str(s, style, b, sizeof b); return text_width(b, (int)strlen(b), size);
}
static void sf_draw(const char* s, int style, float x, float y, float size, const float c[4])
{
    if (!sf_ok()) { text_draw(s, (int)strlen(s), x, y, size, c); return; }
    char b[720]; sf_str(s, style, b, sizeof b); text_draw(b, (int)strlen(b), x, y, size, c);
}
static void ss_outline(float x0, float y0, float x1, float y1, const float c[4])
{
    rect(x0, y0, x1, y0 + 1.0f, c, c, 0); rect(x0, y1 - 1.0f, x1, y1, c, c, 0);
    rect(x0, y0, x0 + 1.0f, y1, c, c, 0); rect(x1 - 1.0f, y0, x1, y1, c, c, 0);
}
static float ss_key(float x, float y, const char* key, const char* label)
{
    static const float KEYL[4] = { 0.43f, 0.40f, 0.36f, 1.0f }, CRM[4] = { 0.937f, 0.902f, 0.824f, 1.0f }, MUT[4] = { 0.56f, 0.54f, 0.50f, 1.0f };
    const float ks = 14.0f, kw = text_width(key, (int)strlen(key), ks) + 12.0f;
    ss_outline(x, y, x + kw, y + 20.0f, KEYL);
    text_draw(key, (int)strlen(key), x + 6.0f, y + 2.0f, ks, CRM);
    char u[32]; gf_upper(u, sizeof u, label);
    gf_sp(u, x + kw + 7.0f, y + 3.0f, 13.0f, 2.0f, MUT);
    return kw + 7.0f + gf_sp_w(u, 13.0f, 2.0f) + 20.0f;
}
static void build_shop_sheet(void)
{
    const uiov_shop_api* t = g_uiov_shop; if (!t) return;
    const int pg = s_sp_page;
    static const float CRM[4] = { 0.937f, 0.902f, 0.824f, 1.0f }, GLD[4] = { 0.788f, 0.639f, 0.353f, 1.0f }, GLDD[4] = { 0.66f, 0.54f, 0.30f, 1.0f },
                       MUT[4] = { 0.56f, 0.54f, 0.50f, 1.0f }, DIMI[4] = { 0.72f, 0.69f, 0.63f, 1.0f }, QUO[4] = { 0.84f, 0.80f, 0.73f, 1.0f },
                       OFF[4] = { 0.42f, 0.40f, 0.37f, 1.0f };
    float c0[4], c1[4];
    const float X1 = 410.0f, lx = 40.0f, rx = X1 - 40.0f, H = UIOV_VW_H;
    rgba(c0, 0, 0, 0, 0.18f); rect(X1, 0, s_vw, H, c0, c0, 0);                          /* the scene stays visible, slightly dimmed */
    rgba(c0, 0.031f, 0.027f, 0.024f, 0.96f); rect(0, 0, X1, H, c0, c0, 0);             /* the sheet (0.96: the HUD cash showed through at 0.88) */
    rgba(c0, 0.788f, 0.639f, 0.353f, 0.45f); rect(X1, 0, X1 + 1.0f, H, c0, c0, 0);     /* gold edge */
    float y = 50.0f;
    gf_sp(pg == 2 ? "FOR SALE" : "YOUR POCKETS", lx, y, 13.0f, 4.5f, GLD);
    y += 24.0f;
    { const char* ttl = t->title ? t->title(pg) : (pg == 2 ? "Store" : "Inventory");
      float ts = 46.0f; while (ts > 26.0f && sf_w(ttl, 1, ts) > rx - lx) ts -= 2.0f;
      sf_draw(ttl, 1, lx, y, ts, CRM); y += ts * 1.18f; }
    { const float my = y + 6.0f, cx = (lx + rx) * 0.5f;                                  /* gold rule + diamond */
      rgba(c0, 0.788f, 0.639f, 0.353f, 0.45f);
      rect(lx, my, cx - 10.0f, my + 1.0f, c0, c0, 0); rect(cx + 10.0f, my, rx, my + 1.0f, c0, c0, 0);
      for (int k = -4; k <= 4; k++) { const float hw = (float)(4 - (k < 0 ? -k : k)) + 0.5f; rect(cx - hw, my + k, cx + hw, my + k + 1.0f, GLD, GLD, 0); }
      y = my + 26.0f; }
    /* bottom block, from the bottom up: key caps, cash, toast, description */
    const float hy = H - 46.0f, cy = hy - 50.0f, sep = cy - 8.0f;
    { extern volatile int g_input_kbm; const int pad = g_input_kbm == 0;
      const int store_here = pg == 2 || (t->prompt && s_sp_prompt[0]);
      float kx = lx;
      kx += ss_key(kx, hy, pad ? "X" : "ENTER", pg == 2 ? "Buy" : "Use");
      if (store_here && !(g_uiov_shop_solo && pg == 2)) kx += ss_key(kx, hy, pad ? "R1" : "TAB", pg == 2 ? "Pockets" : "Store");
      ss_key(kx, hy, pad ? "O" : "ESC", "Leave"); }
    rgba(c0, 0.937f, 0.902f, 0.824f, 0.12f); rect(lx, sep, rx, sep + 1.0f, c0, c0, 0);
    gf_sp(pg == 2 ? "YOUR CASH" : "CASH", lx, cy + 14.0f, 13.0f, 3.0f, MUT);
    { char st[64]; if (t->status && t->status(pg, st, sizeof st)) { const char* d = strchr(st, '$'); const char* v = d ? d : st;
        text_draw(v, (int)strlen(v), rx - text_width(v, (int)strlen(v), 26.0f), cy + 5.0f, 26.0f, GLD); } }
    float qb = sep - 16.0f;
    if (s_sp_toast[0] && GetTickCount64() - s_sp_toast_t < 2600) {
        char u[128]; gf_upper(u, sizeof u, s_sp_toast); gf_sp(u, lx, qb - 18.0f, 15.0f, 1.5f, GLD); qb -= 32.0f; }
    /* the list's rows (needed for the description too) */
    int n = t->count ? t->count(pg) : 0; if (n > SP_MAX_ROWS) n = SP_MAX_ROWS;
    int* sel = &s_sp_sel[pg]; int* scr = &s_sp_scroll[pg];
    uiov_shop_row selrow; memset(&selrow, 0, sizeof selrow); int have_sel = 0;
    if (n > 0 && t->row(pg, *sel, &selrow)) have_sel = 1;
    /* description: serif italic, wrapped, bottom-aligned */
    float qtop = qb;
    if (have_sel && selrow.desc[0]) {
        const float qs = 19.0f, lh = 25.0f, maxw = rx - lx; int st[8], ln[8], nl = 0; const char* d = selrow.desc; int L = (int)strlen(d), i = 0;
        while (i < L && nl < 6) {
            while (i < L && d[i] == ' ') i++;
            int best = -1;
            for (int k = i; k <= L; k++) if (k == L || d[k] == ' ') { char tmp[200]; int m = k - i; if (m > 190) m = 190; memcpy(tmp, d + i, (size_t)m); tmp[m] = 0;
                if (sf_w(tmp, 2, qs) <= maxw) best = k; else break; }
            if (best < 0) { best = i + 1; while (best < L && d[best] != ' ') best++; }
            st[nl] = i; ln[nl] = best - i; nl++; i = best;
        }
        qtop = qb - nl * lh;
        for (int k = 0; k < nl; k++) { char tmp[200]; int m = ln[k] > 190 ? 190 : ln[k]; memcpy(tmp, d + st[k], (size_t)m); tmp[m] = 0;
            sf_draw(tmp, 2, lx, qtop + k * lh, qs, QUO); }
    }
    /* rows */
    const float rh = 46.0f, list_bot = qtop - 22.0f, ns = 22.0f;
    int vis = (int)((list_bot - y) / rh); if (vis < 1) vis = 1;
    if (*sel < *scr) *scr = *sel;
    if (*sel >= *scr + vis) *scr = *sel - vis + 1;
    if (*scr > n - vis) *scr = n - vis > 0 ? n - vis : 0;
    for (int i = 0; i < SP_MAX_ROWS; i++) s_sp_y0[i] = s_sp_y1[i] = 0.0f;
    s_sp_x0 = 0; s_sp_x1 = X1;
    if (!n) gf_sp(pg == 2 ? "NOTHING FOR SALE HERE" : "YOUR POCKETS ARE EMPTY", lx, y + 8.0f, 15.0f, 2.0f, MUT);
    if (*scr > 0) gf_sp_r("...", rx, y - 20.0f, 15.0f, 1.0f, MUT);
    for (int i = *scr; i < n && i < *scr + vis; i++, y += rh) {
        uiov_shop_row r; memset(&r, 0, sizeof r);
        if (!t->row(pg, i, &r)) continue;
        const int on = i == *sel;
        const float ry0 = y, ry1 = y + rh - 6.0f, ty = (ry0 + ry1) * 0.5f - ns * 0.62f;
        if (on) { rgba(c0, 0.62f, 0.106f, 0.106f, 0.30f); rgba(c1, 0.62f, 0.106f, 0.106f, 0.12f); rect(lx - 14.0f, ry0, rx + 14.0f, ry1, c0, c1, 1); }
        s_sp_y0[i] = ry0; s_sp_y1[i] = ry1;
        const float* nc = !r.enabled ? OFF : on ? CRM : DIMI;
        text_draw(r.name, (int)strlen(r.name), lx, ty, ns, nc);
        float nx = lx + text_width(r.name, (int)strlen(r.name), ns) + 10.0f;
        if (r.sub[0]) text_draw(r.sub, (int)strlen(r.sub), nx, ty + ns * 0.18f, ns * 0.72f, MUT);
        if (r.right[0]) { const float* vc = !r.enabled ? OFF : pg == 2 ? (on ? GLD : GLDD) : (on ? CRM : DIMI);
            text_draw(r.right, (int)strlen(r.right), rx - text_width(r.right, (int)strlen(r.right), ns), ty, ns, vc); }
    }
    if (*scr + vis < n) gf_sp_r("...", rx, list_bot - 6.0f, 15.0f, 1.0f, MUT);
}
static void build_shop(void)
{
    { static int classic = -1; if (classic < 0) { const char* e = getenv("GF_SHOP_UI"); classic = (e && (e[0] == 'c' || e[0] == 'C')) ? 1 : 0; }
      if (!classic) { build_shop_sheet(); return; } }
    const uiov_shop_api* t = g_uiov_shop; if (!t) return;
    const int pg = s_sp_page;
    float c0[4], c1[4];
    rgba(c0, 0, 0, 0, 0.55f); rect(0, 0, s_vw, UIOV_VW_H, c0, c0, 0);   /* the game behind dims, like its own menus */
    const float cx = s_vw * 0.5f, x0 = cx - 380.0f, x1 = cx + 380.0f, y0 = 70.0f, y1 = 610.0f;
    float bx0, by0, bx1, by1;
    const char* ttl = t->title ? t->title(pg) : (pg == 2 ? "Store" : "Inventory");
    gf_box(x0, y0, x1, y1, ttl, &bx0, &by0, &bx1, &by1);
    const float lx = bx0 + 40.0f, rx = bx1 - 40.0f;
    float y = by0 + 16.0f;
    /* heading line: column captions left, cash right (gold) */
    gf_sp(pg == 2 ? "FOR SALE" : "CARRYING", lx, y, 14.0f, 3.0f, GF_DIM);
    { char st[64], u[64]; if (t->status && t->status(pg, st, sizeof st)) { sp_money_fmt(st, u, sizeof u); gf_sp_r(u, rx, y - 3.0f, 19.0f, 1.5f, GF_GOLD); } }
    y += 24.0f;
    rgba(c0, 0.40f, 0.40f, 0.40f, 1); rect(lx, y, rx, y + 1.0f, c0, c0, 0);
    y += 14.0f;
    const float strip_h = 64.0f, list_bot = by1 - strip_h - 14.0f, rh = 34.0f, lsz = 21.0f;
    int n = t->count ? t->count(pg) : 0; if (n > SP_MAX_ROWS) n = SP_MAX_ROWS;
    int vis = (int)((list_bot - y) / rh); if (vis < 1) vis = 1;
    int* sel = &s_sp_sel[pg]; int* scr = &s_sp_scroll[pg];
    if (*sel < *scr) *scr = *sel;
    if (*sel >= *scr + vis) *scr = *sel - vis + 1;
    if (*scr > n - vis) *scr = n - vis > 0 ? n - vis : 0;
    for (int i = 0; i < SP_MAX_ROWS; i++) s_sp_y0[i] = s_sp_y1[i] = 0.0f;
    s_sp_x0 = x0; s_sp_x1 = x1;
    uiov_shop_row selrow; memset(&selrow, 0, sizeof selrow); int have_sel = 0;
    if (!n) gf_sp_c(pg == 2 ? "NOTHING FOR SALE HERE" : "YOU ARE NOT CARRYING ANYTHING", cx, y + 40.0f, 18.0f, 2.0f, GF_DIM);
    if (*scr > 0) gf_sp_r("...", rx, y - 18.0f, 16.0f, 1.0f, GF_DIM);
    for (int i = *scr; i < n && i < *scr + vis; i++, y += rh) {
        uiov_shop_row r; memset(&r, 0, sizeof r);
        if (!t->row(pg, i, &r)) continue;
        const int on = i == *sel;
        if (on) { selrow = r; have_sel = 1;
            rgba(c0, 0.20f, 0.03f, 0.03f, 0.85f); rgba(c1, 0.10f, 0.02f, 0.02f, 0.0f); rect(lx - 18.0f, y - 6.0f, rx + 18.0f, y + rh - 8.0f, c0, c1, 1);   /* faint red bar, fades right */
            text_draw(">", 1, lx - 16.0f, y, lsz, GF_RED); }
        s_sp_y0[i] = y - 6.0f; s_sp_y1[i] = y + rh - 8.0f;
        char u[64]; gf_upper(u, sizeof u, r.name);
        const float* col = !r.enabled ? GF_DIM : on ? GF_RED : GF_ITEM;
        gf_sp(u, lx + 6.0f, y, lsz, 2.0f, col);
        float rxx = rx;
        if (r.right[0]) { char v[32]; gf_upper(v, sizeof v, r.right); const float* vc = !r.enabled ? GF_DIM : pg == 2 ? GF_GOLD : (on ? GF_RED : GF_ITEM);
            gf_sp_r(v, rxx, y, lsz, 1.5f, vc); rxx -= gf_sp_w(v, lsz, 1.5f) + 26.0f; }
        if (r.sub[0]) { char v[32]; gf_upper(v, sizeof v, r.sub); gf_sp_r(v, rxx, y + lsz * 0.22f, lsz * 0.66f, 1.0f, GF_DIM); }
    }
    if (*scr + vis < n) gf_sp_r("...", rx, list_bot - 4.0f, 16.0f, 1.0f, GF_DIM);
    /* tip strip: like the pause menu's hint line -- dark band, red arrows, the selection's description */
    { const float sy0 = by1 - strip_h - 4.0f, sy1 = by1 - 6.0f;
      rgba(c0, 0.13f, 0.13f, 0.13f, 1); rect(bx0 + 8.0f, sy0, bx1 - 8.0f, sy1, c0, c0, 0);
      const float am = (sy0 + sy1) * 0.5f - 9.0f;
      text_draw("<", 1, bx0 + 20.0f, am, 18.0f, GF_RED); text_draw(">", 1, bx1 - 32.0f, am, 18.0f, GF_RED);
      const char* d = have_sel ? selrow.desc : "";
      if (s_sp_toast[0] && GetTickCount64() - s_sp_toast_t < 2600) d = s_sp_toast;
      char u[200]; gf_upper(u, sizeof u, d);
      const float dsz = 15.0f, maxw = (bx1 - bx0) - 110.0f;
      /* up to two lines, split at a space */
      int L = (int)strlen(u), cut = L;
      if (gf_sp_w(u, dsz, 1.0f) > maxw) { for (int k = L - 1; k > 0; k--) { if (u[k] != ' ') continue; char tmp[200]; memcpy(tmp, u, (size_t)k); tmp[k] = 0;
          if (gf_sp_w(tmp, dsz, 1.0f) <= maxw) { cut = k; break; } } }
      if (cut < L) { char a[200]; memcpy(a, u, (size_t)cut); a[cut] = 0;
          gf_sp_c(a, cx, sy0 + 12.0f, dsz, 1.0f, GF_PROMPT); gf_sp_c(u + cut + 1, cx, sy0 + 33.0f, dsz, 1.0f, GF_PROMPT); }
      else gf_sp_c(u, cx, (sy0 + sy1) * 0.5f - dsz * 0.62f, dsz, 1.0f, GF_PROMPT); }
    /* prompts under the box */
    { const int store_here = t->prompt && s_sp_prompt[0] && pg == 1 ? 1 : (pg == 2);
      const char* pr[4]; int np = 0; extern volatile int g_input_kbm; const int pad = g_input_kbm == 0;   /* v1093M */
      pr[np++] = pad ? "D-Pad  Select" : "Up/Down  Select"; pr[np++] = pg == 2 ? (pad ? "Cross  Buy" : "Enter  Buy") : (pad ? "Cross  Use" : "Enter  Use");
      if (store_here) pr[np++] = pg == 2 ? (pad ? "R1  Inventory" : "Tab  Inventory") : (pad ? "R1  Store" : "Tab  Store");
      pr[np++] = pad ? "Circle  Back" : "Esc  Back";
      for (int i = 0; i < np; i++) gf_sp_c(pr[i], cx + 190.0f * ((float)i - (np - 1) * 0.5f), y1 + 22.0f, 19.0f, 0.5f, GF_PROMPT); }
}
/* v1103A free-aim target HUD: name + health bar of the NPC under the crosshair (gf_ragdoll.c aimhud_tick publishes it), top right
 * like the game's own lock-on panel: letter-spaced caps, a green bar in a dark frame with the HUD's bronze edge. */
extern volatile int g_aimhud_on; extern char g_aimhud_name[64]; extern volatile float g_aimhud_hp, g_aimhud_hpmax; extern volatile unsigned long long g_aimhud_t;
static int aimhud_want(void) { extern volatile unsigned long long g_vt_t; return (g_aimhud_on && GetTickCount64() - g_aimhud_t < 180) || GetTickCount64() - g_vt_t < 4000; }
static void aimhud_build(void)
{
    { extern char g_vt_text[96]; extern volatile unsigned long long g_vt_t;   /* v1105T voice tester readout, top centre */
      if (GetTickCount64() - g_vt_t < 4000 && g_vt_text[0]) { float cb[4]; const float tw = gf_sp_w(g_vt_text, 16.0f, 1.5f), x = (s_vw - tw) * 0.5f;
          rgba(cb, 0, 0, 0, 0.75f); rect(x - 14.0f, 128.0f, x + tw + 14.0f, 156.0f, cb, cb, 0); gf_sp(g_vt_text, x, 134.0f, 16.0f, 1.5f, GF_GOLD); } }
    if (!(g_aimhud_on && GetTickCount64() - g_aimhud_t < 180)) return;
    float c0[4], c1[4]; char u[72];
    const float rx = s_vw - 56.0f, y = 30.0f, bw = 150.0f, bh = 9.0f;
    gf_upper(u, sizeof u, g_aimhud_name[0] ? g_aimhud_name : "");
    if (u[0]) { rgba(c0, 0, 0, 0, 0.7f); gf_sp_r(u, rx + 1.5f, y + 1.5f, 16.0f, 1.6f, c0); gf_sp_r(u, rx, y, 16.0f, 1.6f, GF_PROMPT); }
    float f = g_aimhud_hpmax > 0.0f ? g_aimhud_hp / g_aimhud_hpmax : 0.0f; f = f < 0 ? 0 : f > 1 ? 1 : f;
    const float x0 = rx - bw, by = y + 22.0f;
    rgba(c0, 0.55f, 0.36f, 0.22f, 0.95f); rect(x0 - 2.0f, by - 2.0f, rx + 2.0f, by + bh + 2.0f, c0, c0, 0);   /* bronze edge */
    rgba(c0, 0.06f, 0.06f, 0.06f, 0.95f); rect(x0, by, rx, by + bh, c0, c0, 0);
    if (f > 0.0f) { rgba(c0, 0.36f, 0.62f, 0.24f, 1.0f); rgba(c1, 0.24f, 0.45f, 0.15f, 1.0f); rect(x0, by, x0 + bw * f, by + bh, c0, c1, 0); }
}
/* v1106D street dialogue (Godfather gf_dialogue.c publishes it): the choice menu (left, the pause-menu look), the "T  TALK" prompt
 * when someone is close, and the subtitle of the line being spoken (bottom centre, speaker in gold). */
extern volatile int g_gfd_menu, g_gfd_sel, g_gfd_nopt, g_gfd_key; extern volatile unsigned long long g_gfd_sub_t1, g_gfd_prompt_t;
extern char g_gfd_opt[5][40], g_gfd_who[48], g_gfd_sub_who[48], g_gfd_sub[200];
static int gfd_want(void) { const ULONGLONG n = GetTickCount64(); return g_gfd_menu || n < g_gfd_sub_t1 || n - g_gfd_prompt_t < 400; }
static void gfd_build(void)
{
    float c0[4], c1[4]; char u[220]; const ULONGLONG now = GetTickCount64();
    extern volatile int g_input_kbm; const int pad = g_input_kbm == 0;
    if (g_gfd_menu) {
        const int n = g_gfd_nopt < 1 ? 1 : g_gfd_nopt > 5 ? 5 : g_gfd_nopt;   /* v1107L: up to 5 rows (Flirt) */
        const float x0 = 64.0f, x1 = 444.0f, y0 = 268.0f, rh = 40.0f, y1 = y0 + 58.0f + rh * (float)n + 14.0f;
        rgba(c0, 0.55f, 0.36f, 0.22f, 0.95f); rect(x0 - 2.0f, y0 - 2.0f, x1 + 2.0f, y1 + 2.0f, c0, c0, 0);   /* bronze edge */
        rgba(c0, 0.03f, 0.03f, 0.03f, 0.90f); rgba(c1, 0.08f, 0.07f, 0.06f, 0.90f); rect(x0, y0, x1, y1, c0, c1, 0);
        gf_upper(u, sizeof u, g_gfd_who[0] ? g_gfd_who : "Talk");
        gf_sp(u, x0 + 22.0f, y0 + 16.0f, 19.0f, 2.0f, GF_GOLD);
        rgba(c0, 0.55f, 0.36f, 0.22f, 0.8f); rect(x0 + 18.0f, y0 + 46.0f, x1 - 18.0f, y0 + 47.5f, c0, c0, 0);
        for (int i = 0; i < n; i++) {
            const float y = y0 + 58.0f + rh * (float)i; const int on = i == g_gfd_sel;
            if (on) { rgba(c0, 0.70f, 0.09f, 0.08f, 0.85f); rgba(c1, 0.45f, 0.05f, 0.05f, 0.85f); rect(x0 + 10.0f, y, x1 - 10.0f, y + rh - 6.0f, c0, c1, 1); }
            char k[4] = { (char)('1' + i), 0 };
            gf_sp(k, x0 + 24.0f, y + 8.0f, 18.0f, 1.0f, on ? GF_PROMPT : GF_GOLD);
            gf_upper(u, sizeof u, g_gfd_opt[i]); gf_sp(u, x0 + 56.0f, y + 8.0f, 18.0f, 1.6f, on ? GF_PROMPT : GF_ITEM);
        }
        char kk[2] = { (char)g_gfd_key, 0 }; char pr[64];
        (void)pad; snprintf(pr, sizeof pr, "1-%d / W S  SELECT    ENTER  CHOOSE    %s  LEAVE", n, kk);
        gf_sp(pr, x0 + 4.0f, y1 + 12.0f, 13.0f, 1.2f, GF_PROMPT);
    } else if (now - g_gfd_prompt_t < 400 && now >= g_gfd_sub_t1) {
        char key[2] = { (char)g_gfd_key, 0 }; const char* t = "TALK";
        const float tsz = 17.0f, tw = gf_sp_w(t, tsz, 2.0f), kw = 26.0f, gap = 10.0f, w = kw + gap + tw, x = (s_vw - w) * 0.5f, y = 600.0f;
        rgba(c0, 0.92f, 0.92f, 0.92f, 0.95f); rgba(c1, 0.72f, 0.72f, 0.72f, 0.95f); rect(x, y, x + kw, y + kw, c0, c1, 0);
        rgba(c0, 0.08f, 0.08f, 0.08f, 1); text_draw(key, 1, x + (kw - text_width(key, 1, 18.0f)) * 0.5f, y + 3.0f, 18.0f, c0);
        rgba(c0, 0, 0, 0, 0.6f); gf_sp(t, x + kw + gap + 1.5f, y + 5.5f, tsz, 2.0f, c0);
        gf_sp(t, x + kw + gap, y + 4.0f, tsz, 2.0f, GF_PROMPT);
    }
    if (now < g_gfd_sub_t1 && g_gfd_sub[0]) {   /* subtitle: "NAME  text", wrapped to two lines */
        const float sz = 21.0f, maxw = 900.0f, cx = s_vw * 0.5f, yb = 655.0f;
        char who[60]; gf_upper(who, sizeof who, g_gfd_sub_who); const float ww = who[0] ? gf_sp_w(who, 15.0f, 1.5f) + 14.0f : 0.0f;
        const char* s = g_gfd_sub; const int L = (int)strlen(s); int cut = L;
        if (text_width(s, L, sz) + ww > maxw) { for (int k = L - 1; k > 0; k--) if (s[k] == ' ' && text_width(s, k, sz) + ww <= maxw) { cut = k; break; } }
        const int two = cut < L; const float w1 = text_width(s, cut, sz) + ww, w2 = two ? text_width(s + cut + 1, L - cut - 1, sz) : 0.0f;
        const float bw = (w1 > w2 ? w1 : w2) + 36.0f, y0 = yb - (two ? 62.0f : 36.0f);
        rgba(c0, 0, 0, 0, 0.55f); rect(cx - bw * 0.5f, y0, cx + bw * 0.5f, yb + 6.0f, c0, c0, 0);
        float x = cx - w1 * 0.5f; rgba(c1, 0, 0, 0, 0.8f);
        if (who[0]) { gf_sp(who, x, y0 + 9.0f, 15.0f, 1.5f, GF_GOLD); x += ww; }
        text_draw(s, cut, x + 1.5f, y0 + 7.5f, sz, c1); text_draw(s, cut, x, y0 + 6.0f, sz, GF_PROMPT);
        if (two) { const float x2 = cx - w2 * 0.5f; text_draw(s + cut + 1, L - cut - 1, x2 + 1.5f, y0 + 33.5f, sz, c1); text_draw(s + cut + 1, L - cut - 1, x2, y0 + 32.0f, sz, GF_PROMPT); }
    }
}
/* GPS route line (Godfather gf_gps.c publishes it): "<key> Change Destination" centred above the radar (clear of the street
 * name below it), in the street name's grey with a soft shadow, while a car has more than one target to choose from. The key
 * is a prompt icon from ui_font.bin (gf_uifont_build.py): the D-pad on a controller, else the key cap of GF_GPS_KEY. */
extern volatile unsigned long long g_gps_hint_t; extern volatile int g_gps_key;
extern int (*volatile g_pad_menu_fn)(void);   /* cellPad.c: the game's pause menu / map is open */
static int gps_want(void) { return GetTickCount64() - g_gps_hint_t < 400 && !(g_pad_menu_fn && g_pad_menu_fn() > 0); }
static void gps_build(void)
{
    if (!gps_want()) return;
    extern volatile int g_input_kbm; const int pad = g_input_kbm == 0;
    const char* t = "Change Destination"; const int L = (int)strlen(t);
    const float sz = 17.0f, isz = 21.0f, gap = 6.0f, cx = s_vw - 172.0f, y = 433.0f;
    const unsigned ico = pad ? 0xF084u : 0xF300u + (unsigned)toupper(g_gps_key);
    const UGlyph* g = glyph(ico, 1);
    const float iw = g ? g->w * (isz / s_iline) : 0.0f, tw = text_width(t, L, sz), w = iw + (g ? gap : 0.0f) + tw;
    float x = cx - w * 0.5f, sh[4], fg[4];
    rgba(sh, 0, 0, 0, 0.75f); rgba(fg, 0.80f, 0.80f, 0.80f, 1.0f);
    if (g) { icon_draw(ico, x, y, isz, 1.0f); x += iw + gap; }
    text_draw(t, L, x + 1.5f, y + 2.0f + 1.5f, sz, sh); text_draw(t, L, x, y + 2.0f, sz, fg);
}
/* crew driver (Godfather gf_crewdrive.c publishes it): "<key> Step on it" / "<key> Take it easy" while a crew member drives,
 * above the radar like the GPS hint (and above it when both show). Cross on a controller, else the key cap of g_cdr_key. */
extern volatile unsigned long long g_cdr_hint_t; extern volatile int g_cdr_key; extern char g_cdr_hint[32];
static int cdr_want(void) { return GetTickCount64() - g_cdr_hint_t < 400 && g_cdr_hint[0] && !(g_pad_menu_fn && g_pad_menu_fn() > 0); }
static void cdr_build(void)
{
    if (!cdr_want()) return;
    extern volatile int g_input_kbm; const int pad = g_input_kbm == 0;
    char t[32]; snprintf(t, sizeof t, "%s", g_cdr_hint); const int L = (int)strlen(t);
    const float sz = 17.0f, isz = 21.0f, gap = 6.0f, cx = s_vw - 172.0f, y = gps_want() ? 404.0f : 433.0f;
    const unsigned ico = pad ? 0xF083u : 0xF300u + (unsigned)toupper(g_cdr_key);
    const UGlyph* g = glyph(ico, 1);
    const float iw = g ? g->w * (isz / s_iline) : 0.0f, tw = text_width(t, L, sz), w = iw + (g ? gap : 0.0f) + tw;
    float x = cx - w * 0.5f, sh[4], fg[4];
    rgba(sh, 0, 0, 0, 0.75f); rgba(fg, 0.80f, 0.80f, 0.80f, 1.0f);
    if (g) { icon_draw(ico, x, y, isz, 1.0f); x += iw + gap; }
    text_draw(t, L, x + 1.5f, y + 2.0f + 1.5f, sz, sh); text_draw(t, L, x, y + 2.0f, sz, fg);
}
/* closed: the store prompt (top centre, like the game's "[E] OPEN DOOR") and toasts */
static void sp_hud_build(void)
{
    float c0[4], c1[4];
    const ULONGLONG now = GetTickCount64();
    float y = 96.0f;
    if (s_sp_prompt_on && s_sp_prompt[0]) {
        extern volatile int g_input_kbm; const int pad = g_input_kbm == 0;   /* v1093M: "HOLD >" on a controller */
        char key[8] = { (char)sp_key('B', "GF_SHOP_KEY"), 0 }, u[112]; if (pad) { char t2[96]; snprintf(t2, sizeof t2, "%s", s_sp_prompt); snprintf(s_sp_prompt, sizeof s_sp_prompt, "Hold Right  %s", t2); strcpy(key, ">"); }
        gf_upper(u, sizeof u, s_sp_prompt);
        const float tsz = 17.0f, tw = gf_sp_w(u, tsz, 2.0f), kw = 26.0f, gap = 10.0f, w = kw + gap + tw, x = (s_vw - w) * 0.5f;
        rgba(c0, 0.92f, 0.92f, 0.92f, 0.95f); rgba(c1, 0.72f, 0.72f, 0.72f, 0.95f); rect(x, y, x + kw, y + kw, c0, c1, 0);   /* key cap */
        rgba(c0, 0.08f, 0.08f, 0.08f, 1); text_draw(key, 1, x + (kw - text_width(key, 1, 18.0f)) * 0.5f, y + 3.0f, 18.0f, c0);
        rgba(c0, 0, 0, 0, 0.6f); gf_sp(u, x + kw + gap + 1.5f, y + 5.5f, tsz, 2.0f, c0);   /* shadow */
        gf_sp(u, x + kw + gap, y + 4.0f, tsz, 2.0f, GF_PROMPT);
        y += 36.0f;
    }
    if (s_sp_toast[0] && now - s_sp_toast_t < 2600) {
        char u[128]; gf_upper(u, sizeof u, s_sp_toast);
        const float a = now - s_sp_toast_t > 2100 ? (2600.0f - (float)(now - s_sp_toast_t)) / 500.0f : 1.0f;
        const float tsz = 16.0f, w = gf_sp_w(u, tsz, 1.5f), x = (s_vw - w) * 0.5f;
        rgba(c0, 0, 0, 0, 0.55f * a); rect(x - 14.0f, y - 2.0f, x + w + 14.0f, y + 26.0f, c0, c0, 0);
        float g[4] = { GF_GOLD[0], GF_GOLD[1], GF_GOLD[2], a }; gf_sp(u, x, y + 4.0f, tsz, 1.5f, g);
    }
}

static void uiov_draw_list(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv, unsigned bbw, unsigned bbh);
/* v1112I curtain: the title raises a full-screen fade with a centred title (gf_shop.c: hides the game's own apparel screen while
 * the in-world tailor takes over). Fades in fast (~0.15 s), out slower (~0.4 s). */
volatile int g_uiov_curtain_on = 0; char g_uiov_curtain_text[64] = "";
static float s_cur_a; static ULONGLONG s_cur_t;
static int curtain_want(void)
{
    const ULONGLONG n = GetTickCount64(); float dt = s_cur_t ? (float)(n - s_cur_t) / 1000.0f : 0.0f; s_cur_t = n; if (dt > 0.1f) dt = 0.1f;
    s_cur_a += g_uiov_curtain_on ? dt * 7.0f : -dt * 2.5f; if (s_cur_a < 0.0f) s_cur_a = 0.0f; if (s_cur_a > 1.0f) s_cur_a = 1.0f;
    return s_cur_a > 0.001f;
}
static int sf_ok(void); static float sf_w(const char* s, int style, float size); static void sf_draw(const char* s, int style, float x, float y, float size, const float c[4]);
static void curtain_build(void)
{
    if (s_cur_a <= 0.001f) return;
    float c[4]; rgba(c, 0.03f, 0.022f, 0.016f, s_cur_a); rect(0, 0, s_vw, UIOV_VW_H, c, c, 0);
    if (g_uiov_curtain_text[0]) {
        const float sz = 40.0f, w = sf_w(g_uiov_curtain_text, 1, sz);
        float t[4] = { 0.937f, 0.902f, 0.824f, s_cur_a }; sf_draw(g_uiov_curtain_text, 1, (s_vw - w) * 0.5f, UIOV_VW_H * 0.5f - sz * 0.6f, sz, t);
        float g[4] = { 0.79f, 0.64f, 0.36f, s_cur_a * 0.8f }; rect((s_vw - 120.0f) * 0.5f, UIOV_VW_H * 0.5f + sz * 0.75f, (s_vw + 120.0f) * 0.5f, UIOV_VW_H * 0.5f + sz * 0.75f + 1.0f, g, g, 0);
    }
}
/* v212 F1 FPS counter: frames this overlay was drawn for (= presented guest frames), averaged over 0.5 s, top-left. */
volatile int g_uiov_show_fps = 0;
static double s_fps_val = 0.0;
static void fps_tick(void)
{
    static LARGE_INTEGER f, t0; static unsigned n = 0;
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); t0 = t; }
    n++;
    const double dt = (double)(t.QuadPart - t0.QuadPart) / (double)f.QuadPart;
    if (dt >= 0.5) { s_fps_val = n / dt; n = 0; t0 = t; }
}
static void fps_build(void)
{
    if (!g_uiov_show_fps) return;
    char t[32]; snprintf(t, sizeof t, "%.0f FPS", s_fps_val);
    const float sz = 26.0f, w = text_width(t, (int)strlen(t), sz);
    float bg[4], fg[4];
    rgba(bg, 0, 0, 0, 0.55f); rect(16.0f, 16.0f, 16.0f + w + 24.0f, 16.0f + 40.0f, bg, bg, 0);
    if (s_fps_val >= 58.0) rgba(fg, 0.55f, 1.0f, 0.55f, 1); else if (s_fps_val >= 45.0) rgba(fg, 1.0f, 0.9f, 0.4f, 1); else rgba(fg, 1.0f, 0.5f, 0.45f, 1);
    text_draw(t, (int)strlen(t), 28.0f, 16.0f + 20.0f - sz * 0.62f, sz, fg);
}
/* v1042S hold-to-skip prompt (state from cellPad.c): bottom-right, "Hold [SPACE | cross] to skip" with a fill bar,
 * then "Skipping..." while the cutscene drains off-screen. Fades in/out over ~150 ms. */
extern volatile float g_skip_progress;
extern volatile int   g_skip_visible;
extern volatile int   g_input_kbm;
static float s_skip_a = 0.0f;
static ULONGLONG s_skip_t = 0;
static int skip_wanted(void) { return g_skip_visible || s_skip_a > 0.01f; }
static void skip_build(void)
{
    const ULONGLONG now = GetTickCount64();
    const float dt = s_skip_t ? (float)(now - s_skip_t) / 150.0f : 0.0f; s_skip_t = now;
    const int vis = g_skip_visible;
    if (vis) { s_skip_a += dt; if (s_skip_a > 1.0f) s_skip_a = 1.0f; }
    else     { s_skip_a -= dt; if (s_skip_a < 0.0f) s_skip_a = 0.0f; }
    if (s_skip_a <= 0.0f) return;
    const float a = s_skip_a, k = vis == 2 ? 1.0f : g_skip_progress;
    const int kbm = g_input_kbm > 0;
    const float sz = 24.0f, isz = 30.0f, pad = 16.0f;
    const char* t1 = vis == 2 ? "Skipping..." : "Hold";
    const char* t2 = vis == 2 ? "" : "to skip";
    const unsigned ico = kbm ? 0xF100u : 0xF083u;
    const UGlyph* g = vis == 2 ? NULL : glyph(ico, 1);
    const float iw = g ? g->w * (isz / s_iline) : 0.0f;
    const float w1 = text_width(t1, (int)strlen(t1), sz), w2 = text_width(t2, (int)strlen(t2), sz);
    const float wtot = w1 + (g ? 10.0f + iw + 10.0f + w2 : 0.0f);
    const float x1 = s_vw - 48.0f, x0 = x1 - wtot - pad * 2.0f, y1 = UIOV_VW_H - 54.0f, y0 = y1 - 54.0f;
    float bg[4], ln[4], fg[4], bar[4];
    rgba(bg, 0.0f, 0.0f, 0.0f, 0.62f * a); rgba(ln, 0.75f, 0.62f, 0.28f, 0.55f * a);
    rgba(fg, 0.93f, 0.91f, 0.86f, a);      rgba(bar, 0.83f, 0.67f, 0.22f, 0.95f * a);
    rect(x0, y0, x1, y1, bg, bg, 0);
    rect(x0, y0, x1, y0 + 1.5f, ln, ln, 0);                         /* thin gold rule, like the game's panels */
    rect(x0, y1 - 4.0f, x0 + (x1 - x0) * k, y1, bar, bar, 0);         /* hold progress */
    const float ty = y0 + 25.0f - sz * 0.62f;
    float x = x0 + pad;
    text_draw(t1, (int)strlen(t1), x, ty, sz, fg); x += w1;
    if (g) {
        x += 10.0f; icon_draw(ico, x, y0 + 25.0f - isz * 0.5f, isz, a); x += iw + 10.0f;
        text_draw(t2, (int)strlen(t2), x, ty, sz, fg);
    }
}
/* The game executable supplies this title-specific status line.  It is empty
 * in ordinary single-player runs because the transport is opt-in. */
#ifdef PS3RECOMP_COOP_MOD
extern int infamous_coop_overlay_text(char* text, size_t capacity);
#else
static int infamous_coop_overlay_text(char* text, size_t capacity)
{ (void)text; (void)capacity; return 0; }
#endif
static void coop_build(void)
{
    char t[192];
    if (!infamous_coop_overlay_text(t, sizeof t)) return;
    const float sz = 20.0f, w = text_width(t, (int)strlen(t), sz);
    float bg[4], fg[4];
    rgba(bg, 0, 0, 0, 0.62f); rgba(fg, 0.40f, 0.85f, 1.0f, 1.0f);
    rect(16.0f, 62.0f, 16.0f + w + 24.0f, 96.0f, bg, bg, 0);
    text_draw(t, (int)strlen(t), 28.0f, 79.0f - sz * 0.62f, sz, fg);
}
/* ---- v1057X weapon wheel: layout from the game's draws + drawing ----
 * The game's wheel = 8 positions around the R marker (45 deg apart, 0 = up) + 4 around the d-pad marker. Each weapon
 * the player HAS is one icon draw at its position; the selected disc (106) sits on the selected position. Positions
 * with no icon are empty. (v1057W used the 103/106 discs as the slots -- those are only markers -> wrong slots.) */
static struct { int ring[8]; int dp[4]; int selr, seld, sel_icon, valid; } s_wl;   /* last layout (kept for the fade-out) */
static float s_gfw_a; static ULONGLONG s_gfw_t;
/* v1057Y: game-time scale while the wheel is up (ppu_recomp_002.cpp func_004E60F8 clock hook multiplies the game
 * timestep by it). GF_WHEEL_SLOWMO = scale (default 0.25; 1 = off). Eases with the wheel's fade. */
volatile double g_gfw_timescale = 1.0;
/* v1109A ammo on the wheel: the title (godfather gf_combat.c) answers per position 0..7 ring / 8..11 d-pad U/R/D/L, occ = positions
 * with an icon: 0 nothing, 1 gun (clip, reserve), 2 counted item (clip = count), | 4 infinite. closed = the wheel closed on pos. */
int (*volatile g_gfw_ammo_fn)(int pos, unsigned occ, int* clip, int* res) = 0;
void (*volatile g_gfw_closed_fn)(int pos) = 0;
static int gfw_ammo_q(int pos, unsigned occ, int* c, int* r) { *c = *r = 0; return g_gfw_ammo_fn ? g_gfw_ammo_fn(pos, occ, c, r) : 0; }
static int gfw_ammo_empty(int f, int c, int r) { return (f & 3) && !(f & 4) && c + r <= 0; }
static int gfw_wanted(void) { return s_wn_n > 0 || s_gfw_a > 0.0f; }
static void gfw_layout(const GfwNote* n, int nn)
{
    int rmk = -1, dmk = -1, sel = -1;
    float cx[GFW_MAXN], cy[GFW_MAXN];
    for (int i = 0; i < nn; i++) {
        cx[i] = (n[i].x0 + n[i].x1) * 0.5f; cy[i] = (n[i].y0 + n[i].y1) * 0.5f;
        const int k = s_gfw[n[i].ent].kind;
        if (k == 3) rmk = i; else if (k == 4) dmk = i; else if (k == 2) sel = i;
    }
    for (int k = 0; k < 8; k++) s_wl.ring[k] = -1;
    for (int k = 0; k < 4; k++) s_wl.dp[k] = -1;
    s_wl.selr = s_wl.seld = -1; s_wl.sel_icon = -1;
    if (rmk < 0 || !n[rmk].pos) { s_wl.valid = 0; return; }
    s_wl.valid = 1;
    const int ydown = dmk >= 0 ? (cy[dmk] > cy[rmk]) : 1;
    /* where does item i sit: ring position 0..7 (return 0..7) or d-pad direction (return 8..11) */
    #define GFW_WHERE(i, out) do { \
        float dxr = cx[i] - cx[rmk], dyr = ydown ? (cy[i] - cy[rmk]) : (cy[rmk] - cy[i]); \
        float dd = 1e30f, dxd = 0, dyd = 0; \
        if (dmk >= 0) { dxd = cx[i] - cx[dmk]; dyd = ydown ? (cy[i] - cy[dmk]) : (cy[dmk] - cy[i]); dd = dxd * dxd + dyd * dyd; } \
        if (dmk >= 0 && dd < dxr * dxr + dyr * dyr) \
            (out) = 8 + (fabsf(dxd) > fabsf(dyd) ? (dxd > 0 ? 1 : 3) : (dyd > 0 ? 2 : 0)); \
        else { float a = atan2f(dxr, -dyr); if (a < 0) a += 6.2831853f; (out) = ((int)floorf(a / 0.78539816f + 0.5f)) & 7; } \
    } while (0)
    for (int i = 0; i < nn; i++) {
        if (s_gfw[n[i].ent].kind != 0) continue;
        int w; GFW_WHERE(i, w);
        if (w < 8) s_wl.ring[w] = n[i].ent; else s_wl.dp[w - 8] = n[i].ent;
    }
    if (sel >= 0) {
        int w; GFW_WHERE(sel, w);
        if (w < 8) { s_wl.selr = w; s_wl.sel_icon = s_wl.ring[w]; } else { s_wl.seld = w - 8; s_wl.sel_icon = s_wl.dp[w - 8]; }
    }
    #undef GFW_WHERE
    static int s_logn = 0; static unsigned s_sig = 0;
    unsigned sig = 2166136261u;
    for (int k = 0; k < 8; k++) sig = (sig ^ (unsigned)(s_wl.ring[k] + 3)) * 16777619u;
    for (int k = 0; k < 4; k++) sig = (sig ^ (unsigned)(s_wl.dp[k] + 3)) * 16777619u;
    sig ^= (unsigned)(s_wl.selr * 31 + s_wl.seld + 77) * 131u;
    if (s_logn < 40 && sig != s_sig) {
        s_logn++; s_sig = sig;
        fprintf(stderr, "[gfw] v1057X ring (0=up, clockwise):");
        for (int k = 0; k < 8; k++) fprintf(stderr, " %d=%s%s", k, s_wl.ring[k] >= 0 ? s_gfw[s_wl.ring[k]].name : "-", s_wl.selr == k ? "*" : "");
        fprintf(stderr, " | dpad U/R/D/L:");
        for (int k = 0; k < 4; k++) fprintf(stderr, " %s%s", s_wl.dp[k] >= 0 ? s_gfw[s_wl.dp[k]].name : "-", s_wl.seld == k ? "*" : "");
        fprintf(stderr, "\n");
    }
}
static void tri(float ax, float ay, float bx, float by, float cx, float cy, const float c[4])
{
    if (s_nv + 3 > UIOV_MAX_VERTS) return;
    const float u = (s_wx + 0.5f) / s_aw, v = (s_wy + 0.5f) / s_ah;
    UVert* o = s_v + s_nv;
    UVert a = { ax, ay, u, v, c[0], c[1], c[2], c[3] }, b = { bx, by, u, v, c[0], c[1], c[2], c[3] }, d = { cx, cy, u, v, c[0], c[1], c[2], c[3] };
    o[0] = a; o[1] = b; o[2] = d; s_nv += 3;
}
/* annulus sector a0..a1 (radians, 0 = up, clockwise), radii ri..ro */
static void arc(float cx, float cy, float ri, float ro, float a0, float a1, const float c[4])
{
    int steps = (int)((a1 - a0) / 0.06f) + 1; if (steps > 120) steps = 120;
    for (int i = 0; i < steps; i++) {
        float t0 = a0 + (a1 - a0) * i / steps, t1 = a0 + (a1 - a0) * (i + 1) / steps;
        float s0 = sinf(t0), c0 = cosf(t0), s1 = sinf(t1), c1 = cosf(t1);
        float ox0 = cx + ro * s0, oy0 = cy - ro * c0, ox1 = cx + ro * s1, oy1 = cy - ro * c1;
        float ix0 = cx + ri * s0, iy0 = cy - ri * c0, ix1 = cx + ri * s1, iy1 = cy - ri * c1;
        tri(ox0, oy0, ox1, oy1, ix1, iy1, c);
        if (ri > 0.0f) tri(ox0, oy0, ix1, iy1, ix0, iy0, c);
    }
}
static void gfw_icon(int e, float cx, float cy, float maxw, float maxh, const float c[4])
{
    if (e < 0 || e >= s_gfw_n || !s_gfw[e].aw) return;
    const GfwEnt* g = &s_gfw[e];
    float k = maxw / g->aw; if (g->ah * k > maxh) k = maxh / g->ah;
    const float w = g->aw * k, h = g->ah * k;
    quad_uv(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f,
            (float)g->ax / s_aw, (1024.0f + g->ay) / s_ah, (float)(g->ax + g->aw) / s_aw, (1024.0f + g->ay + g->ah) / s_ah, c, c, 0);
}
static void gfw_build(void)
{
    const ULONGLONG now = GetTickCount64();
    const float dt = s_gfw_t ? (float)(now - s_gfw_t) / 120.0f : 0.0f; s_gfw_t = now;
    { static int was_open = 0;   /* v1109A: tell the title which position the wheel closed on */
      if (s_wn_n) was_open = 1;
      else if (was_open) { was_open = 0; const int sp = s_wl.valid ? (s_wl.selr >= 0 ? s_wl.selr : (s_wl.seld >= 0 ? 8 + s_wl.seld : -1)) : -1;
                           if (sp >= 0 && g_gfw_closed_fn) g_gfw_closed_fn(sp); } }
    if (s_wn_n) { gfw_layout(s_wn, s_wn_n); s_gfw_a += dt > 0.0f ? dt : 0.34f; if (s_gfw_a > 1.0f) s_gfw_a = 1.0f; }
    else { s_gfw_a -= dt; if (s_gfw_a < 0.0f) s_gfw_a = 0.0f; }
    s_wn_n = 0;
    { static double slow = -1.0;
      if (slow < 0.0) { const char* ev = getenv("GF_WHEEL_SLOWMO"); slow = ev && *ev ? atof(ev) : 0.25;
                        if (slow < 0.02) slow = 0.02; if (slow > 1.0) slow = 1.0;
                        fprintf(stderr, "[gfw] v1057Y slow motion while the wheel is open: x%.2f (GF_WHEEL_SLOWMO)\n", slow); }
      g_gfw_timescale = 1.0 - (1.0 - slow) * (double)s_gfw_a; }
    if (s_gfw_a <= 0.0f || !s_wl.valid) return;
    const float a = s_gfw_a, e = 1.0f - (1.0f - a) * (1.0f - a);
    float dim[4], glass[4], glass2[4], line[4], br[4], brf[4], iv[4], ivd[4], mut[4], faint[4];
    rgba(dim, 0.02f, 0.018f, 0.015f, (s_gpso && g_uiov_bb ? 0.22f : 0.58f) * e);   /* v1057Y: graded frame needs less */
    rgba(glass, 0.047f, 0.043f, 0.039f, 0.72f * e); rgba(glass2, 0.047f, 0.043f, 0.039f, 0.45f * e);
    rgba(line, 0.93f, 0.90f, 0.85f, 0.12f * e);
    rgba(br, 0.82f, 0.60f, 0.37f, e); rgba(brf, 0.82f, 0.60f, 0.37f, 0.24f * e);
    rgba(iv, 0.96f, 0.90f, 0.82f, e); rgba(ivd, 0.93f, 0.90f, 0.85f, 0.82f * e);
    rgba(mut, 0.93f, 0.90f, 0.85f, 0.62f * e); rgba(faint, 0.93f, 0.90f, 0.85f, 0.34f * e);
    float red[4], icx[4]; rgba(red, 0.86f, 0.36f, 0.30f, 0.95f * e); rgba(icx, 0.93f, 0.90f, 0.85f, 0.30f * e);   /* v1109A */
    unsigned occ = 0; for (int k = 0; k < 8; k++) if (s_wl.ring[k] >= 0) occ |= 1u << k; for (int k = 0; k < 4; k++) if (s_wl.dp[k] >= 0) occ |= 1u << (8 + k);
    char at[40];
    rect(0, 0, s_vw, UIOV_VW_H, dim, dim, 0);
    const float cx = s_vw * 0.5f, cy = 344.0f, sc = 0.94f + 0.06f * e;
    const float ro = 206.0f * sc, ri = 120.0f * sc, rm = (ro + ri) * 0.5f, rc = 106.0f * sc;
    arc(cx, cy, ro + 7.0f, ro + 8.0f, 0.0f, 6.2831853f, line);
    for (int k = 0; k < 8; k++) {
        const float ang = 0.78539816f * k, half = 0.78539816f * 0.5f - 0.035f, a0 = ang - half, a1 = ang + half;
        const int has = s_wl.ring[k] >= 0, selk = s_wl.selr == k;
        if (selk) {
            arc(cx, cy, ri, ro, a0, a1, brf); arc(cx, cy, ro - 1.0f, ro, a0, a1, br); arc(cx, cy, ri, ri + 1.0f, a0, a1, br);
            arc(cx, cy, ro + 5.0f, ro + 8.0f, a0 + 0.02f, a1 - 0.02f, br);
        } else {
            arc(cx, cy, ri, ro, a0, a1, has ? glass : glass2);
            arc(cx, cy, ro - 1.0f, ro, a0, a1, line);
        }
        const float ix = cx + rm * sinf(ang), iy = cy - rm * cosf(ang);
        if (has) { int ac, ar; const int af = gfw_ammo_q(k, occ, &ac, &ar), emp = gfw_ammo_empty(af, ac, ar);   /* v1109A */
            gfw_icon(s_wl.ring[k], ix, iy - (af ? 5.0f * sc : 0.0f), 62.0f * sc, 31.0f * sc, emp ? icx : (selk ? iv : ivd));
            if (af) { const float ty = iy + 17.0f * sc;
                if (emp) gf_sp_c("NO AMMO", ix, ty, 11.0f, 1.5f, red);
                else if (af & 4) gf_sp_c("INF", ix, ty, 12.0f, 2.0f, selk ? br : mut);
                else { if ((af & 3) == 1) snprintf(at, sizeof at, "%d / %d", ac, ar); else snprintf(at, sizeof at, "x%d", ac);
                       gf_sp_c(at, ix, ty, 12.0f, 1.5f, selk ? iv : mut); } } }
        else gf_sp_c("EMPTY", ix, iy - 11.0f * 0.62f, 11.0f, 2.0f, selk ? mut : faint);
    }
    /* centre: what is selected */
    arc(cx, cy, 0.0f, rc, 0.0f, 6.2831853f, glass);
    arc(cx, cy, rc - 1.0f, rc, 0.0f, 6.2831853f, line);
    arc(cx, cy, rc - 8.0f, rc - 7.0f, 0.0f, 6.2831853f, brf);
    if (s_wl.sel_icon >= 0) {
        char nm[32]; gf_upper(nm, sizeof nm, s_gfw[s_wl.sel_icon].name[0] ? s_gfw[s_wl.sel_icon].name : "Weapon");
        gfw_icon(s_wl.sel_icon, cx, cy - 30.0f * sc, 92.0f * sc, 40.0f * sc, iv);
        gf_sp_c(nm, cx, cy + 14.0f * sc - 26.0f * 0.62f, 26.0f, 3.0f, iv);
        rect(cx - 36.0f, cy + 40.0f * sc, cx + 36.0f, cy + 41.0f * sc, br, br, 0);
    } else gf_sp_c("EMPTY", cx, cy - 22.0f * 0.62f, 22.0f, 3.0f, mut);
    /* quick slots (d-pad) on the left */
    {
        const float gx = cx - 412.0f, gy = cy, cs = 50.0f, gp = 4.0f;
        gf_sp_c("QUICK SLOTS", gx, gy - cs * 1.5f - gp - 34.0f, 13.0f, 3.5f, mut);
        static const float off[4][2] = { { 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 } };
        for (int k = 0; k < 4; k++) {
            const float x = gx + off[k][0] * (cs + gp), y = gy + off[k][1] * (cs + gp);
            const int selk = s_wl.seld == k;
            rect(x - cs * 0.5f, y - cs * 0.5f, x + cs * 0.5f, y + cs * 0.5f, selk ? brf : glass, selk ? brf : glass, 0);
            const float* bc = selk ? br : line;
            rect(x - cs * 0.5f, y - cs * 0.5f, x + cs * 0.5f, y - cs * 0.5f + 1.0f, bc, bc, 0);
            rect(x - cs * 0.5f, y + cs * 0.5f - 1.0f, x + cs * 0.5f, y + cs * 0.5f, bc, bc, 0);
            rect(x - cs * 0.5f, y - cs * 0.5f, x - cs * 0.5f + 1.0f, y + cs * 0.5f, bc, bc, 0);
            rect(x + cs * 0.5f - 1.0f, y - cs * 0.5f, x + cs * 0.5f, y + cs * 0.5f, bc, bc, 0);
            if (s_wl.dp[k] >= 0) { int ac, ar; const int af = gfw_ammo_q(8 + k, occ, &ac, &ar), emp = gfw_ammo_empty(af, ac, ar);   /* v1109A */
                gfw_icon(s_wl.dp[k], x, y - (af ? 5.0f : 0.0f), 40.0f, 22.0f, emp ? icx : (selk ? iv : ivd));
                if (af) { if (emp) gf_sp_c("EMPTY", x, y + 12.0f, 8.0f, 1.0f, red);
                          else if (af & 4) gf_sp_c("INF", x, y + 12.0f, 9.0f, 1.0f, mut);
                          else { if ((af & 3) == 1) snprintf(at, sizeof at, "%d/%d", ac, ar); else snprintf(at, sizeof at, "x%d", ac);
                                 gf_sp_c(at, x, y + 12.0f, 9.0f, 1.0f, selk ? iv : mut); } } }
            else gf_sp_c("EMPTY", x, y - 8.0f * 0.62f, 8.0f, 1.0f, faint);
        }
        rect(gx - 8.0f, gy - 2.0f, gx + 8.0f, gy + 2.0f, mut, mut, 0); rect(gx - 2.0f, gy - 8.0f, gx + 2.0f, gy + 8.0f, mut, mut, 0);
    }
    const int kbm = g_input_kbm > 0;
    const char* h1 = kbm ? "MOUSE  SELECT" : "R  SELECT";
    const char* h2 = kbm ? "RELEASE Q  EQUIP" : "RELEASE L2  EQUIP";
    const float w1 = gf_sp_w(h1, 14.0f, 2.5f), w2 = gf_sp_w(h2, 14.0f, 2.5f), hy = UIOV_VW_H - 52.0f;
    gf_sp(h1, cx - (w1 + 40.0f + w2) * 0.5f, hy, 14.0f, 2.5f, mut);
    gf_sp(h2, cx - (w1 + 40.0f + w2) * 0.5f + w1 + 40.0f, hy, 14.0f, 2.5f, mut);
}
/* v1057Y wheel "noir" grade: while the overlay wheel is up the finished frame is copied (g_uiov_bb, set by the backend
 * right before uiov_render) and drawn back desaturated, slightly warm and darker (s_gpso). GF_WHEEL_GREY=0 = off. */
static ID3D12Resource* s_gtex; static unsigned s_gtw, s_gth;
static unsigned s_gvbi;
static void gfw_barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br; memset(&br, 0, sizeof br);
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; br.Transition.StateBefore = a; br.Transition.StateAfter = b;
    cl->lpVtbl->ResourceBarrier(cl, 1, &br);
}
static void gfw_grey(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv, unsigned bbw, unsigned bbh)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GF_WHEEL_GREY"); on = (e && *e == '0') ? 0 : 1; }
    ID3D12Resource* bb = g_uiov_bb;
    const float amt = s_gfw_a;
    if (!on || !bb || amt <= 0.0f || s_gfx_state != 1 || !s_gpso || !s_gheap || !s_gvbmap || !bbw || !bbh) return;
    if (!s_gtex || s_gtw != bbw || s_gth != bbh) {   /* (re)create the copy (an old one is leaked: resizes are rare) */
        D3D12_HEAP_PROPERTIES hp; memset(&hp, 0, sizeof hp); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td; memset(&td, 0, sizeof td);
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = bbw; td.Height = bbh; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        s_gtex = NULL;
        if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                        &IID_ID3D12Resource, (void**)&s_gtex)) || !s_gtex) { s_gtex = NULL; on = 0;
            fprintf(stderr, "[gfw] grey copy texture failed -> no grey-out\n"); return; }
        s_gtw = bbw; s_gth = bbh;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv; memset(&sv, 0, sizeof sv);
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE ch; RSX_D3D12_CPU_HEAP_START(s_gheap)(s_gheap, &ch);
        dev->lpVtbl->CreateShaderResourceView(dev, s_gtex, &sv, ch);
        fprintf(stderr, "[gfw] v1057Y wheel grey-out ready (%ux%u)\n", bbw, bbh);
    }
    gfw_barrier(cl, bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->lpVtbl->CopyResource(cl, s_gtex, bb);
    gfw_barrier(cl, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    gfw_barrier(cl, s_gtex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const float e = 1.0f - (1.0f - amt) * (1.0f - amt);
    const float vw = (float)bbw / ((float)bbh / UIOV_VW_H);
    s_gvbi = (s_gvbi + 1) % UIOV_FRAMES;
    UVert* o = s_gvbmap + s_gvbi * 6;
    const float c[4] = { 0.92f * e, 0.38f * e, 0.0f, 1.0f };   /* r = desaturation, g = darkening */
    UVert q[4] = { { 0, 0, 0, 0, c[0], c[1], c[2], c[3] }, { vw, 0, 1, 0, c[0], c[1], c[2], c[3] },
                   { 0, UIOV_VW_H, 0, 1, c[0], c[1], c[2], c[3] }, { vw, UIOV_VW_H, 1, 1, c[0], c[1], c[2], c[3] } };
    o[0] = q[0]; o[1] = q[1]; o[2] = q[2]; o[3] = q[2]; o[4] = q[1]; o[5] = q[3];
    D3D12_VIEWPORT vp = { 0, 0, (float)bbw, (float)bbh, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)bbw, (LONG)bbh };
    cl->lpVtbl->OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    cl->lpVtbl->RSSetViewports(cl, 1, &vp);
    cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_rs);
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, &s_gheap);
    D3D12_GPU_DESCRIPTOR_HANDLE gh; RSX_D3D12_GPU_HEAP_START(s_gheap)(s_gheap, &gh);
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 1, gh);
    float so[4] = { 2.0f / vw, 2.0f / UIOV_VW_H, 0, 0 };
    cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 0, 4, so, 0);
    cl->lpVtbl->SetPipelineState(cl, s_gpso);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv;
    vbv.BufferLocation = s_gvb->lpVtbl->GetGPUVirtualAddress(s_gvb) + (UINT64)s_gvbi * 6 * sizeof(UVert);
    vbv.SizeInBytes = 6 * sizeof(UVert); vbv.StrideInBytes = sizeof(UVert);
    cl->lpVtbl->IASetVertexBuffers(cl, 0, 1, &vbv);
    cl->lpVtbl->DrawInstanced(cl, 6, 1, 0, 0);
    gfw_barrier(cl, s_gtex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
}
static void uiov_draw_list(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv, unsigned bbw, unsigned bbh);
static void fps_only(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv, unsigned bbw, unsigned bbh, DXGI_FORMAT rtfmt)
{
    char coop_text[2];
    const int have_coop = infamous_coop_overlay_text(coop_text, sizeof coop_text);
    if ((!g_uiov_show_fps && !have_coop && !skip_wanted() && !gfw_wanted()) || !font_load() || !gfx_init(dev, rtfmt)) return;
    s_k = bbh / UIOV_VW_H; s_vw = bbw / s_k;
    s_vbi = (s_vbi + 1) % UIOV_FRAMES;
    s_v = s_vbmap[s_vbi]; s_nv = 0;
    gfw_build();    /* v1057W */
    fps_build();
    coop_build();
    skip_build();   /* v1042S */
    gfw_grey(dev, cl, rtv, bbw, bbh);   /* v1057Y */
    if (s_nv) uiov_draw_list(cl, rtv, bbw, bbh);
}
void uiov_render(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                 unsigned bbw, unsigned bbh, DXGI_FORMAT rtfmt, HWND hwnd)
{
    fps_tick();   /* v212 */
    /* v194: settings menu / pause hint (when no system dialog is up) */
    if (!D.active && font_load()) {
        s_k = bbh / UIOV_VW_H; s_vw = bbw / s_k;
        int want = s_menu_open ? 0 : tr_tick(hwnd, bbw, bbh);   /* F10 trainer page / active-cheat line */
        if (!s_tr_open) want |= menu_tick(hwnd, bbw, bbh);
        if (!s_tr_open && !s_menu_open) want |= sp_tick(hwnd, bbw, bbh);   /* v1092S */
        if (!s_tr_open && !s_menu_open && !s_sp_page) want |= aimhud_want();   /* v1103A */
        if (!s_tr_open && !s_menu_open && !s_sp_page) want |= gfd_want();   /* v1106D */
        if (!s_tr_open && !s_menu_open && !s_sp_page) want |= gps_want();   /* GPS route line hint */
        if (!s_tr_open && !s_menu_open && !s_sp_page) want |= cdr_want();   /* crew driver: Step on it / Take it easy */
        want |= curtain_want();   /* v1112I */
        if (want && gfx_init(dev, rtfmt)) {
            s_last_draw_ms = GetTickCount64();
            s_vbi = (s_vbi + 1) % UIOV_FRAMES;
            s_v = s_vbmap[s_vbi]; s_nv = 0;
            if (!s_tr_open && !s_menu_open && !s_sp_page) gfw_build();   /* v1057W */
            else s_wn_n = 0;
            if (s_tr_open) build_trainer();
            else if (s_menu_open) build_menu();
            else if (s_sp_page) build_shop();   /* v1092S */
            else { if (s_hint_on) build_hint(); if (s_wm_on) build_watermark(); else tr_hud_build(); sp_hud_build(); aimhud_build(); gfd_build(); gps_build(); cdr_build(); }   /* v238; v1103A aimhud; v1106D dialogue */
            curtain_build();   /* v1112I */
            fps_build();   /* v212 */
            coop_build();
            skip_build();   /* v1042S */
            gfw_grey(dev, cl, rtv, bbw, bbh);   /* v1057Y */
            if (s_nv) uiov_draw_list(cl, rtv, bbw, bbh);
            return;
        }
    }
    ULONGLONG now = GetTickCount64();
    s_last_draw_ms = now;
    if (s_cs_ready != 2) { fps_only(dev, cl, rtv, bbw, bbh, rtfmt); return; }   /* no dialog was ever opened */
    lock();
    int active = D.active;
    float a_close = 0.0f;
    if (!active && D.t_close && now - D.t_close < 140) a_close = D.fade_close * (1.0f - (now - D.t_close) / 140.0f);
    /* v168: nothing on screen and no answering key to wait for -> no input polling (XInputGetState on empty
     * slots is slow and this runs every frame). */
    if (!active && !D.held && a_close <= 0.0f) { unlock(); fps_only(dev, cl, rtv, bbw, bbh, rtfmt); return; }
    /* input */
    unsigned in = read_input(hwnd);
    /* v168 FIX: 'held' only covers the key that ANSWERED the dialog, until it is released (v164 re-armed it on
     * every later Space/C/X/Z/Enter/Esc press after any dialog -> the game saw a neutral pad: no jump, WASD dropped). */
    if (!active && D.held && (!(in & (IN_OK | IN_BACK)) || now - D.t_close > 3000)) D.held = 0;
    BtnRect br[2]; int nb = 0;
    if (active && now - D.t_open > 200) {
        unsigned prev = D.prev == 0xFFFFFFFFu ? in : D.prev;
        unsigned dn = in & ~prev;
        D.prev = in;
        if (D.kind == 2 && (dn & (IN_LEFT | IN_RIGHT | IN_TAB))) D.sel ^= 1;
        int answer = 0;
        if (dn & IN_OK)   answer = (D.kind == 2 && D.sel == 1) ? 2 : 1;
        if (dn & IN_BACK) answer = D.kind == 2 ? 2 : 1;
        /* mouse: hover selects, click answers (positions from the last frame's layout) */
        static int s_lbtn;
        int lb = (hwnd && GetForegroundWindow() == hwnd && (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) ? 1 : 0;
        POINT pt; RECT rc;
        if (hwnd && s_nb && GetCursorPos(&pt) && ScreenToClient(hwnd, &pt) && GetClientRect(hwnd, &rc) && rc.bottom > 0) {
            float vx = pt.x * (UIOV_VW_H / (float)rc.bottom), vy = pt.y * (UIOV_VW_H / (float)rc.bottom);
            vx *= ((float)rc.bottom / rc.right) * ((float)bbw / bbh);   /* client aspect may differ from backbuffer */
            for (int i = 0; i < s_nb; i++)
                if (vx >= s_br[i].x0 && vx <= s_br[i].x1 && vy >= s_br[i].y0 && vy <= s_br[i].y1) {
                    static POINT s_last; if (pt.x != s_last.x || pt.y != s_last.y) D.sel = i;
                    s_last = pt;
                    if (lb && !s_lbtn) answer = (D.kind == 2 && i == 1) ? 2 : 1;
                }
        }
        s_lbtn = lb;
        if (answer) {
            D.result = answer; D.active = 0; D.t_close = now; D.need_clear = 1; D.held = 1;
            D.fade_close = 1.0f; memcpy(D.last_msg, D.msg, sizeof D.msg); D.last_kind = D.kind; D.last_sel = answer == 2 && D.kind == 2 ? 1 : 0;
            fprintf(stderr, "[uiov] dialog answered: %s\n", answer == 1 ? (D.kind == 2 ? "YES" : "OK") : "NO");
            active = 0; a_close = 1.0f;
        }
        if (!active) s_nb = 0;
    } else if (active) {
        D.prev = D.prev == 0xFFFFFFFFu ? in : D.prev;
    }
    if (!active && a_close <= 0.0f) { unlock(); fps_only(dev, cl, rtv, bbw, bbh, rtfmt); return; }
    char msg[2048]; int kind, sel; float alpha, grow = 1.0f;
    if (active) {
        memcpy(msg, D.msg, sizeof msg); kind = D.kind; sel = D.sel;
        float t = (now - D.t_open) / 180.0f; if (t > 1) t = 1;
        alpha = 1.0f - (1.0f - t) * (1.0f - t);
        grow = 0.85f + 0.15f * alpha; if (t >= 1) grow = 1.0f;
    } else { memcpy(msg, D.last_msg, sizeof msg); kind = D.last_kind; sel = D.last_sel; alpha = a_close; }
    unlock();

    if (!font_load() || !gfx_init(dev, rtfmt)) return;
    s_vbi = (s_vbi + 1) % UIOV_FRAMES;
    s_v = s_vbmap[s_vbi]; s_nv = 0;
    s_k = bbh / UIOV_VW_H; s_vw = bbw / s_k;
    s_wn_n = 0;   /* v1057W: no wheel over a system dialog */
    nb = build_dialog(msg, kind, sel, alpha, grow, br);
    s_nb = active ? nb : 0;                      /* button rects for next frame's mouse test */
    if (s_nb) memcpy(s_br, br, sizeof(BtnRect) * s_nb);
    fps_build();   /* v212 */
    if (!s_nv) return;
    uiov_draw_list(cl, rtv, bbw, bbh);
}
static void uiov_draw_list(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv, unsigned bbw, unsigned bbh)
{
    if (!s_tex_uploaded) {
        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = s_tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = s_tex_up; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = s_aw; src.PlacedFootprint.Footprint.Height = s_ah;
        src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = s_aw * 4;
        cl->lpVtbl->CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = s_tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cl->lpVtbl->ResourceBarrier(cl, 1, &b);
        s_tex_uploaded = 1;
    }
    D3D12_VIEWPORT vp = { 0, 0, (float)bbw, (float)bbh, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)bbw, (LONG)bbh };
    cl->lpVtbl->OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    cl->lpVtbl->RSSetViewports(cl, 1, &vp);
    cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_rs);
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, &s_heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gh; RSX_D3D12_GPU_HEAP_START(s_heap)(s_heap, &gh);
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 1, gh);
    float so[4] = { 2.0f / s_vw, 2.0f / UIOV_VW_H, 0, 0 };
    cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 0, 4, so, 0);
    cl->lpVtbl->SetPipelineState(cl, s_pso);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv;
    vbv.BufferLocation = s_vb[s_vbi]->lpVtbl->GetGPUVirtualAddress(s_vb[s_vbi]);
    vbv.SizeInBytes = s_nv * sizeof(UVert); vbv.StrideInBytes = sizeof(UVert);
    cl->lpVtbl->IASetVertexBuffers(cl, 0, 1, &vbv);
    cl->lpVtbl->DrawInstanced(cl, s_nv, 1, 0, 0);
}

#include "rsx_post.h"   /* v1116A lighting pack (AO / bloom / grade) */
#else  /* !_WIN32: no overlay; cellMsgDialog keeps its non-interactive behaviour */
int  uiov_dialog_available(void) { return 0; }
unsigned long long uiov_last_draw_ms(void) { return 0; }
void uiov_dialog_open(const char* m, int k, int d) { (void)m; (void)k; (void)d; }
int  uiov_dialog_result(void) { return 0; }
void uiov_dialog_close(void) {}
int  uiov_pad_gate(void) { return 0; }
const uiov_shop_api* volatile g_uiov_shop = 0;   /* v1092S */
int gfd_pending(void) { return 0; }
void gpb_request(const float* cols, int n) { (void)cols; (void)n; }
int gpb_result(int i, float* y, unsigned* age) { (void)i; (void)y; (void)age; return 0; }
int gpb_wanted(void) { return 0; }
int gpost_wanted(void) { return 0; }
int (*volatile g_gf_rain_poll)(void) = 0;   /* v1118B */
int gpost_wants(int w) { (void)w; return 0; }
int gpost_run(void* a, void* b, void* r, void* st, void* z, const float* c, int ok, int w) { (void)a; (void)b; (void)r; (void)st; (void)z; (void)c; (void)ok; (void)w; return 0; }
int gpb_run(void* a, void* b, const float* c, void* z) { (void)a; (void)b; (void)c; (void)z; return 0; }
void gfs_add(const float* p, const float* v, float s0, float s1, float life, float alpha) { (void)p; (void)v; (void)s0; (void)s1; (void)life; (void)alpha; }
void gfm_set(int id, const float* m12, int on) { (void)id; (void)m12; (void)on; }
int gfm_on(void) { return 0; }
int gfd_render(void* a, void* b, const float* c, const float* e, int f, int z, void* zr) { (void)a; (void)b; (void)c; (void)e; (void)f; (void)z; (void)zr; return 0; }
void gfd_add(const float* p, const float* n, const float* t, float w, float h, int sprite, float alpha, float grow) { (void)p; (void)n; (void)t; (void)w; (void)h; (void)sprite; (void)alpha; (void)grow; }
void gfp_add(const float* p, const float* v, float size, float ground, float life) { (void)p; (void)v; (void)size; (void)ground; (void)life; }
#endif
