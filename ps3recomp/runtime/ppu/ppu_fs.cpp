/*
 * ps3recomp - cellFs VFS (sys_fs HLE)
 *
 * Backs the game's file I/O with the real host filesystem so it can load its
 * data/config/assets. Guest PS3 paths (/dev_bdvd/..., /app_home/..., /dev_hdd0,
 * etc.) are translated to a host root (the game directory that contains
 * PS3_GAME), set by the boot harness from the EBOOT path (or $PS3_VFS_ROOT).
 *
 * Only the calls the boot actually imports are implemented:
 *   cellFsOpen/Close/Read/Write/Lseek/Stat/Fstat/Opendir/Readdir/Closedir/
 *   Mkdir/Rmdir/Unlink/Fsync.
 *
 * Context-aware HLE: guest pointers (path, buffers, out params) are read/written
 * through vm_base in big-endian.
 */
#include "ppu_recomp.h"      /* ppu_context */
#include "../../libs/filesystem/edat.h"
#include "ps3emu/nid.h"      /* ps3_compute_nid */
#include "ps3emu/guest_call.h" /* ps3_invoke_guest: AIO completion is a guest OPD */
#include "sdata_decrypt.h"   /* SDATA/EDAT (NPD) decryption for cellFsSdataOpen */
#include "ps3emu/hz_stats.h"   /* v225A hitch attribution: guest file-read time */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
extern "C" int pad_bench_world_mark(const char* gpath);   /* v1041R, libs/input/cellPad.c */
/* v1092S: every cellFsOpen path goes to the title too (Godfather gf_shop.c: which business interior streamed in). */
extern "C" { void (*volatile g_fs_open_note)(const char* gpath) = 0; }
#include <sys/stat.h>
#include <thread>   /* v114 */
#include <chrono>
#include "../platform/win32_dirent.h"   /* <dirent.h>, or a Win32 stand-in */

/* Guest-resolving host backtrace (ppu_loader.cpp). Must be declared at file scope:
 * `extern "C"` inside a function body is ill-formed in C++. */
extern "C" void ydkj_host_bt(const char* tag);
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>          /* open/close on MinGW */
#else
#include <unistd.h>
#endif
#ifndef O_BINARY
#define O_BINARY 0       /* POSIX has no text/binary distinction */
#endif

/* v1130P Windows long paths. open()/stat()/fopen() take an ANSI path, and the ANSI file APIs stop at MAX_PATH
 * (260) unless the user flipped LongPathsEnabled in the registry AND the exe is longPathAware. A disc dumped
 * into a deep folder (e.g. "...\Godfather, The - The Don's Edition (Europe)\Godfather, The - The Don's Edition
 * (Europe)") pushes the deepest Godfather files past it: smbiz_014_bakery_267/layout_014_design_267/
 * 267_venue_takeover_alt.str (Trapani's Bakery favour: the dealer in the alley is never spawned and the mission
 * can't go on) and construction_zone_content.str both came back ENOENT while RPCS3 reads them fine. So when the
 * plain call fails on a long path it is retried through the wide API with the "\\?\" prefix, which has no
 * MAX_PATH limit. CP_ACP is the code page the ANSI call itself used, so the same bytes name the same file. */
#ifdef _WIN32
static int fs_wlong(const char* p, wchar_t* out, size_t n)
{
    if (strlen(p) < 240) return 0;                       /* short paths: the plain call already told the truth */
    wchar_t w[2048], full[2048];
    if (!MultiByteToWideChar(CP_ACP, 0, p, -1, w, 2048)) return 0;
    DWORD fl = GetFullPathNameW(w, 2048, full, NULL);    /* absolute + '/' -> '\' (\\?\ disables both) */
    if (fl == 0 || fl >= 2048) return 0;
    if (full[0] == L'\\' && full[1] == L'\\')            /* \\server\share -> \\?\UNC\server\share */
        return _snwprintf(out, n, L"\\\\?\\UNC\\%ls", full + 2) > 0;
    return _snwprintf(out, n, L"\\\\?\\%ls", full) > 0;
}
static int fs_open(const char* p, int fl, int mode)
{
    int fd = open(p, fl, mode);
    if (fd >= 0) return fd;
    wchar_t w[2100];
    return fs_wlong(p, w, 2100) ? _wopen(w, fl, mode) : fd;
}
static int fs_stat(const char* p, struct stat* st)
{
    int r = stat(p, st);
    if (r == 0) return r;
    wchar_t w[2100];
    if (!fs_wlong(p, w, 2100)) return r;
#if defined(_MSC_VER) || defined(_UCRT)
    static_assert(sizeof(struct stat) == sizeof(struct _stat64i32), "stat layout");
    return _wstat64i32(w, (struct _stat64i32*)st);       /* what the CRT's own stat() forwards to */
#else
    return r;
#endif
}
static FILE* fs_fopen_rb(const char* p)
{
    int fd = fs_open(p, O_RDONLY | O_BINARY, 0666);
    if (fd < 0) return NULL;
    FILE* f = fdopen(fd, "rb");
    if (!f) close(fd);
    return f;
}
#else
#define fs_open open
#define fs_stat stat
static FILE* fs_fopen_rb(const char* p) { return fopen(p, "rb"); }
#endif

extern "C" uint8_t* vm_base;
extern "C" void ppu_guest_caller(char* out, size_t n);
extern "C" uint32_t ppu_vm_size;
extern "C" void     ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" void     vm_write32(uint64_t a, uint32_t v);
extern "C" void     vm_write64(uint64_t a, uint64_t v);
#include "../platform/win32_backtrace.h"   /* RtlCaptureStackBackTrace / GetModuleHandleA on POSIX */

/* Host root that the PS3 mount points map into (dir containing PS3_GAME). */
extern "C" const char* ppu_vfs_root = ".";

/* CELL_FS return / mode / flag constants. */
#define CELL_OK              0
#define CELL_FS_ENOENT       (-2147418106)   /* 0x80010006 -- was -2147418090, which is 0x80010016 = CELL_ENOTCONN, not ENOENT */
#define CELL_FS_EISDIR       (-2147418094)   /* 0x80010012 */
#define CELL_FS_EIO          (-2147418111)
#define CELL_FS_S_IFDIR      0x4000u
#define CELL_FS_S_IFREG      0x8000u
#define CELL_FS_O_RDONLY     0
#define CELL_FS_O_WRONLY     1
#define CELL_FS_O_RDWR       2
#define CELL_FS_O_CREAT      0x0200
#define CELL_FS_O_TRUNC      0x0400
#define CELL_FS_O_APPEND     0x0100
#define CELL_FS_SEEK_SET     0
#define CELL_FS_SEEK_CUR     1
#define CELL_FS_SEEK_END     2
#define CELL_FS_TYPE_DIR     1
#define CELL_FS_TYPE_REG     2

/* ---- guest memory string helpers ---- */
static void guest_strcpy(char* dst, uint32_t gaddr, size_t cap)
{
    size_t i = 0;
    for (; i < cap - 1; i++) {
        if (ppu_vm_size && gaddr + i >= ppu_vm_size) break;
        char c = (char)vm_base[gaddr + i];
        if (!c) break;
        dst[i] = c;
    }
    dst[i] = 0;
}

/* Defined in runtime/syscalls/sys_fs.c -- shared by all three path
 * translators. (extern "C" is ill-formed at block scope.) */
extern "C" void ps3_vfs_ps3game_fallback(char* path, size_t cap);

/* Translate a guest path to a host path under ppu_vfs_root. Known PS3 mount
 * prefixes are stripped; the rest is appended to the root. */
static void host_path(char* out, size_t cap, const char* guest)
{
    const char* rel = guest;
    /* /dev_hdd0 overlays the installed game-update dir (a disc title patched to
     * e.g. v1.30 runs the update's EBOOT and reads its patchN.farc from
     * /dev_hdd0/game/<title>/), mirroring the PS3/RPCS3 layout: base data on
     * /dev_bdvd (disc), update data on /dev_hdd0. Set PS3_HDD0_ROOT to the host
     * dir that /dev_hdd0 maps into (the one containing game/<title>/). */
    static const char* hdd0_root = nullptr; static int hdd0_init = 0;
    if (!hdd0_init) { hdd0_root = getenv("PS3_HDD0_ROOT"); hdd0_init = 1; }
    if (hdd0_root && strncmp(guest, "/dev_hdd0/", 10) == 0) {
        snprintf(out, cap, "%s/%s", hdd0_root, guest + 10);
        for (char* p = out; *p; p++) if (*p == '\\') *p = '/';
        return;
    }

    /* /dev_flash is FIRMWARE, not game data — mapping it into the game root (the
     * old behaviour) makes every firmware lookup miss. YDKJ's FMOD asks for
     * /dev_flash/sys/external/flashMP3.pic and, on a miss, prints "mp3 failed to
     * load MP3 codec (are you using the correct flash?)" and then crashes.
     * Serve it from a real dev_flash tree instead ($PS3_DEV_FLASH, else RPCS3's). */
    if (strncmp(guest, "/dev_flash/", 11) == 0) {
        const char* fw = getenv("PS3_DEV_FLASH");
        if (!fw || !*fw) fw = "D:/recomp/tools/rpcs3/dev_flash";
        snprintf(out, cap, "%s/%s", fw, guest + 11);
        for (char* p = out; *p; p++) if (*p == '\\') *p = '/';
        return;
    }

    static const char* mounts[] = {
        "/dev_bdvd/", "/app_home/", "/dev_hdd0/", "/dev_hdd1/",
        "/dev_flash/", "/host_root/", "/dev_usb000/", "/dev_usb/"
    };
    for (size_t i = 0; i < sizeof(mounts)/sizeof(mounts[0]); i++) {
        size_t n = strlen(mounts[i]);
        if (strncmp(guest, mounts[i], n) == 0) { rel = guest + n; break; }
        /* Every mount above ends in '/', so a BARE device root never matched
         * its own mount: "/dev_bdvd" is not a prefix-match for "/dev_bdvd/".
         * It fell through to the strip-leading-slash case and resolved to
         * <root>/dev_bdvd, which does not exist -- so a title that opendirs or
         * stats the device to check the medium is present reads it as "no
         * disc". YDKJ does exactly that, 27 times a boot, and consequently
         * loaded none of its Scaleform UI or FMOD banks: no logos, no legal
         * screen, just a clear colour.
         *
         * libs/filesystem/cellFs.c already carries this fix (Tokyo Jungle hit
         * the same wall and retried forever); ppu_fs is the other half of the
         * split filesystem and never got it. Map the bare root to the mount
         * directory itself. */
        if (n > 1 && strncmp(guest, mounts[i], n - 1) == 0 && guest[n - 1] == '\0') {
            rel = guest + n - 1;   /* points at the NUL -> <root>/ */
            break;
        }
    }
    if (rel == guest && guest[0] == '/') rel = guest + 1;   /* strip leading '/' */
    snprintf(out, cap, "%s/%s", ppu_vfs_root, rel);
    for (char* p = out; *p; p++) if (*p == '\\') *p = '/';
    ps3_vfs_ps3game_fallback(out, cap);
}

/* ---- fd / dir handle tables ---- */
#define FS_MAX 256
static FILE* g_files[FS_MAX];
static uint8_t g_fd_usm[FS_MAX];
/* Story-cutscene movies currently open (inFamous: movies/cutscene/*.bik other than the startup splash/logo
 * videos). The file stays open for exactly as long as the movie plays; rsx_ui_overlay.c hides the pause
 * watermark while this is non-zero, so it shows in menus and startup videos but not in cutscenes. */
extern "C" volatile int g_fs_cutscene_open = 0;
static uint8_t g_fd_cut[FS_MAX];
/* v1042S: last time each cutscene fd was read (ms) -- Godfather's in-engine cutscenes are detected by their dialogue
 * stream (audiostreams/<dir>/igc_<name>.exa.snu): open AND still being read means the scene is playing. */
static unsigned long long g_fd_cut_ms[FS_MAX];
static unsigned long long fs_now_ms(void)
{
    return (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
extern "C" volatile int g_gf_ffwd;   /* v1042S cellPad.c: cutscene skip in progress */
static volatile unsigned long long g_igc_shot_ms = 0;   /* v1042S: last IGC shot file opened */
extern "C" unsigned long long ps3_fs_igc_shot_age_ms(void)
{
    const unsigned long long t = g_igc_shot_ms;
    return t ? fs_now_ms() - t : ~0ull;
}
extern "C" int ps3_fs_igc_active(void)
{
    const unsigned long long now = fs_now_ms();
    for (int i = 3; i < FS_MAX; i++)
        if (g_fd_cut[i] == 2 && g_files[i] && now - g_fd_cut_ms[i] < 4000ull) return 1;
    return 0;
}
/* v1042V: a skipped Godfather cutscene leaves its dialogue file open and polled after the game has moved on.
 * gf_cutscene.c calls this once the skip has ended the sequence: the files open right now stop counting as a playing
 * cutscene (3 = "was a cutscene, skipped"; close still balances g_fs_cutscene_open). Files opened later count again. */
/* v1042Z GF_SKIP_AUDIO_END (default 1; 0 = old): a skipped cutscene's dialogue/music track (EA SNS blocks: u8 flags,
 * u24 size; flags 0x80 = last block) used to play on to its end. EOF is not an option (the title takes it as a disc
 * error and reopens the stream), so the stream is ENDED instead: when the skip jumps the sequence to its end, every
 * block header in the chunk last read for that fd gets the 0x80 "last block" flag in guest memory (whichever block the
 * decoder parses next ends the track), and so does every block of each later read of that fd. The game's own stream
 * player then sees a normal end of track (~35 ms per block). Log [skip-audio]. */
static char    g_fd_path[FS_MAX][192];   /* guest path per fd (diagnostics) */
#define SNU_RING 4   /* the player reads ahead: the chunk being decoded is usually one or two before the last read */
static uint32_t g_fd_lr_buf[FS_MAX][SNU_RING], g_fd_lr_n[FS_MAX][SNU_RING];   /* recent reads: guest buffer, bytes */
static long     g_fd_lr_pos[FS_MAX][SNU_RING];                               /* recent reads: file offset */
static uint8_t  g_fd_lr_head[FS_MAX];
static uint8_t  g_fd_snu_end[FS_MAX];                     /* 1 = end this stream at the next block header */
static int skip_audio_mode(void);
static int skip_audio_end_on(void) { return skip_audio_mode() != 0; }
/* v1042Z: own handle on the stream's host file -- the title's streaming thread reads through g_files[fd] at the
 * same time, so its file position must never be moved from here. */
static FILE* snu_open(int fd)
{
    char hp[1100];
    if (!g_fd_path[fd][0]) return NULL;
    host_path(hp, sizeof hp, g_fd_path[fd]);
    return fs_fopen_rb(hp);
}
/* v1053N: how a skipped cutscene's track is ended. GF_SKIP_AUDIO_END=1 (default) SILENCES it: the payload of every
 * SNS block (after the 8-byte header: flags/size + sample count) in the chunk is zeroed in guest memory, which EA-XAS
 * (codec 4, all PS3 IGC streams) decodes as silence. Block headers, sizes and sample counts are untouched, so the
 * game's stream player runs the track to its REAL end (silently) and every line cue in the file's 1BOT table starts
 * and finishes on time. =2 is the old v1042Z behaviour (flag blocks as last -> track ends mid-line). User 2026-11-03:
 * after skipping igc_act01_t03_03 with v1042Z the track was cut inside a spoken line and the game never opened another
 * dialogue stream ("all NPCs stopped talking"); same report on 2026-11-02, so the shot handling (v1049K) was not it. */
static int skip_audio_mode(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("GF_SKIP_AUDIO_END"); v = (e && *e) ? atoi(e) : 1; if (v < 0 || v > 2) v = 1;
        fprintf(stderr, "[skip-audio] v1053N skipped cutscene audio: %s (GF_SKIP_AUDIO_END=%d)\n",
                v == 1 ? "silenced, track runs to its real end" : v == 2 ? "ended early (old v1042Z)" : "left playing", v); }
    return v;
}
/* Block containing file offset `from` (walks the chain from the SNU data offset); -1 if none.
 * *sz_out = that block's size, *last_out = 1 if it is the track's last block. */
static long snu_block_containing(FILE* f, long from, uint32_t* sz_out, int* last_out)
{
    unsigned char h[16];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(h, 1, 16, f) != 16) return -1;
    long p = (long)(((uint32_t)h[8] << 24) | ((uint32_t)h[9] << 16) | ((uint32_t)h[10] << 8) | h[11]);
    for (int k = 0; k < 200000 && p > 0; k++) {
        unsigned char b[4];
        if (fseek(f, p, SEEK_SET) != 0 || fread(b, 1, 4, f) != 4) return -1;
        const uint32_t sz = ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
        if (sz < 8 || sz > 0x100000) return -1;   /* not SNS */
        if (from < p + (long)sz || (b[0] & 0x80)) { if (from < p) from = p; *sz_out = sz; *last_out = (b[0] & 0x80) != 0; return p; }
        p += (long)sz;
    }
    return -1;
}
/* The guest bytes [buf, buf+n) hold file [pos, pos+n). mode 1: zero the audio payload of every block (part) in it.
 * mode 2: set the last-block flag on every block header in it. Each block is checked against the file first, so a
 * buffer the player has since refilled with something else is left alone. */
static int snu_patch_range(int fd, uint32_t buf, long pos, uint32_t n, const char* when)
{
    if (!n) return 0;
    const int mode = skip_audio_mode();
    if (!mode) return 0;
    FILE* f = snu_open(fd);
    if (!f) return 0;
    int hits = 0; long first = -1; uint32_t zeroed = 0;
    const long end = pos + (long)n;
    uint32_t sz = 0; int last = 0;
    long hb = snu_block_containing(f, pos, &sz, &last);
    for (int k = 0; hb >= 0 && hb < end && k < 4096; k++) {
        if (mode == 2) {
            if (hb >= pos && hb + 4 <= end) {
                unsigned char fb[4]; uint8_t* g = vm_base + buf + (uint32_t)(hb - pos);
                if (fseek(f, hb, SEEK_SET) != 0 || fread(fb, 1, 4, f) != 4 || memcmp(fb, g, 4)) break;
                g[0] |= 0x80; hits++; if (first < 0) first = hb;
            }
        } else {
            long a = hb + 8, b = hb + (long)sz;           /* this block's audio payload */
            if (a < pos) a = pos;
            if (b > end) b = end;
            if (a < b) {
                unsigned char fb[16]; uint8_t* g = vm_base + buf + (uint32_t)(a - pos);
                const size_t cmp = (size_t)((b - a) < 16 ? (b - a) : 16);
                if (fseek(f, a, SEEK_SET) != 0 || fread(fb, 1, cmp, f) != cmp) break;
                if (memcmp(fb, g, cmp)) {                  /* already silenced (all zero) is fine, anything else: stop */
                    size_t z = 0; while (z < cmp && g[z] == 0) z++;
                    if (z != cmp) break;
                }
                memset(g, 0, (size_t)(b - a)); zeroed += (uint32_t)(b - a); hits++; if (first < 0) first = hb;
            }
        }
        if (last) break;
        hb += (long)sz;
        if (hb >= end) break;
        unsigned char nb[4];
        if (fseek(f, hb, SEEK_SET) != 0 || fread(nb, 1, 4, f) != 4) break;
        sz = ((uint32_t)nb[1] << 16) | ((uint32_t)nb[2] << 8) | nb[3]; last = (nb[0] & 0x80) != 0;
        if (sz < 8 || sz > 0x100000) break;
    }
    fclose(f);
    if (hits) {
        static int logn = 0;
        if (logn < 400) { logn++;
            if (mode == 2) fprintf(stderr, "[skip-audio] v1042Z %s: '%s' ends at block 0x%lX (%d block(s) flagged as last)\n", when, g_fd_path[fd], first, hits);
            else fprintf(stderr, "[skip-audio] v1053N %s: '%s' silenced from block 0x%lX (%d block(s), %u bytes)\n", when, g_fd_path[fd], first, hits, zeroed); }
    }
    return hits;
}
/* v1042Z: called by gf_cutscene.c when the skip jumps the sequence past its last shot -- end the audio tracks now. */
extern "C" void ps3_fs_igc_end_audio(void)
{
    if (!skip_audio_end_on()) return;
    for (int i = 3; i < FS_MAX; i++) if (g_fd_cut[i] == 2 && g_files[i] && !g_fd_snu_end[i]) {
        g_fd_snu_end[i] = 1;
        for (int r = 0; r < SNU_RING; r++) if (g_fd_lr_n[i][r]) snu_patch_range(i, g_fd_lr_buf[i][r], g_fd_lr_pos[i][r], g_fd_lr_n[i][r], "skip jump (buffered chunk)");
    }
}
extern "C" void ps3_fs_igc_forget(void)
{
    for (int i = 3; i < FS_MAX; i++) if (g_fd_cut[i] == 2) {
        g_fd_cut[i] = 3;
        if (skip_audio_end_on() && g_files[i] && !g_fd_snu_end[i]) {
            g_fd_snu_end[i] = 1;
            /* the chunk the player is about to decode is usually the one read last: end it there if still in memory */
            for (int r = 0; r < SNU_RING; r++) if (g_fd_lr_n[i][r]) snu_patch_range(i, g_fd_lr_buf[i][r], g_fd_lr_pos[i][r], g_fd_lr_n[i][r], "skip (buffered chunk)");
        }
    }
}
static int fs_is_story_cutscene(const char* gpath)
{
    /* v1042S Godfather: an IGC's dialogue stream */
    { const char* a = strstr(gpath, "/audiostreams/");
      if (a && strstr(a, "/igc_") && strstr(a, ".snu")) return 2; }
    const char* d = strstr(gpath, "/movies/cutscene/");
    if (!d) return 0;
    const char* name = d + 17;
    if (!strstr(name, ".bik")) return 0;
    return strncmp(name, "splash_", 7) != 0 && strncmp(name, "infamous_logo", 13) != 0;
}
/* g_fd_path: declared above the v1042Z helpers */   /* 1 if this fd is an open .usm movie (read tracker) */
static DIR*  g_dirs[FS_MAX];
static char  g_dir_path[FS_MAX][1024];   /* host path per open dir (for readdir stat) */

static int fd_alloc_file(FILE* f)
{
    for (int i = 3; i < FS_MAX; i++) if (!g_files[i] && !g_dirs[i]) { g_files[i] = f; return i; }
    return -1;
}
static int fd_alloc_dir(DIR* d)
{
    for (int i = 3; i < FS_MAX; i++) if (!g_files[i] && !g_dirs[i]) { g_dirs[i] = d; return i; }
    return -1;
}

/* ---- handlers ---- */
static void cellFsOpen(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t flags  = (uint32_t)ctx->gpr[4];
    uint32_t fd_ptr = (uint32_t)ctx->gpr[5];
    host_path(hpath, sizeof hpath, gpath);

    /* Opt-in boot-media bypass.  Some titles gate their frontend behind an
     * unskippable loading movie; while bringing up a newly lifted movie SPU,
     * returning the normal "optional file absent" result is a much safer
     * diagnostic than patching guest control flow.  Match only the caller's
     * explicit guest-path substring and leave ordinary runs untouched. */
    /* v1041R BENCH_WORLD_MARK (default "world_terrain.str"): the first open of a path containing it marks
     * "gameplay world loaded" for the benchmark input logic in cellPad.c (PAD_AUTOPRESS stops, BENCH_MOVE). */
    { static int done = 0; if (!done) done = pad_bench_world_mark(gpath); }
    { void (*nf)(const char*) = g_fs_open_note; if (nf) nf(gpath); }   /* v1092S */
    /* SKIP_INTRO=1 (settings.ini): the boot logos / intro movies are timed by wall clock and cannot be skipped with
     * a button, so they are reported absent here (same "optional file missing" result as PS3_FS_SKIP_PATH) and the
     * game moves on to the next stage.  SKIP_INTRO_PATHS = comma list overriding the default substrings. */
    { static int si = -1; static const char* lst = "movies/logo.vp6,movies/paramount.vp6,movies/load_";
      if (si < 0) { const char* e = getenv("SKIP_INTRO"); si = (e && *e && *e != '0') ? 1 : 0;
                    const char* l = getenv("SKIP_INTRO_PATHS"); if (l && *l) lst = l; }
      if (si && strstr(gpath, "movies/")) {
          char buf[512]; snprintf(buf, sizeof buf, "%s", lst);
          for (char* t = strtok(buf, ","); t; t = strtok(NULL, ","))
              if (*t && strstr(gpath, t)) {
                  fprintf(stderr, "[fs-skip] SKIP_INTRO: '%s' reported absent\n", gpath);
                  ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT;
                  return;
              }
          fprintf(stderr, "[fs-skip] SKIP_INTRO: movie '%s' kept (not in list)\n", gpath);
      } }
    { const char* skip = getenv("PS3_FS_SKIP_PATH");
      if (skip && *skip && strstr(gpath, skip)) {
          fprintf(stderr, "[fs-skip] optional boot media '%s' (PS3_FS_SKIP_PATH='%s')\n",
                  gpath, skip);
          ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT;
          return;
      }
    }

    /* NPDRM: see the note in sys_fs.c -- an EDAT is decrypted once into a cache
     * file and that is opened in its place. */
    { char dec_path[1200];
      const char* use = edat_resolve(hpath, dec_path, sizeof dec_path);
      if (use != hpath) snprintf(hpath, sizeof hpath, "%s", use); }

    /* fopen() mode strings can't express the PS3/POSIX open semantics (e.g.
     * O_WRONLY without create+truncate, or O_CREAT without O_TRUNC), so build
     * real open() flags from the access mode (low 2 bits) + the modifiers, then
     * wrap the fd in a FILE* with fdopen() (which neither creates nor truncates
     * -- that was already decided by open()). */
    int acc = flags & 0x3;
    int oflags = (acc == CELL_FS_O_RDWR)   ? O_RDWR
               : (acc == CELL_FS_O_WRONLY) ? O_WRONLY : O_RDONLY;
    if (flags & CELL_FS_O_CREAT)  oflags |= O_CREAT;
    if (flags & CELL_FS_O_TRUNC)  oflags |= O_TRUNC;
    if (flags & CELL_FS_O_APPEND) oflags |= O_APPEND;

    /* A bare device root ("/dev_bdvd") resolves to a DIRECTORY, and open() on a
     * directory fails on Windows. Real cellFsOpen answers EISDIR there, and a
     * title reads that as "the medium is mounted" -- YDKJ probes the disc this
     * way before loading its Scaleform UI and FMOD banks, so the ENOENT it got
     * instead read as "no disc" and it drew nothing but a clear colour.
     * S_IFMT/S_IFDIR rather than S_ISDIR: MSVC's CRT does not define the macro. */
    {
        struct stat _dst;
        if (fs_stat(hpath, &_dst) == 0 && (_dst.st_mode & S_IFMT) == S_IFDIR) {
            ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EISDIR;
            return;
        }
    }
    int hfd = fs_open(hpath, oflags | O_BINARY, 0666);
    if (hfd < 0) {
        fprintf(stderr, "[fs] open FAIL '%s' -> '%s'\n", gpath, hpath);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return;
    }
    const char* fmode = (acc == CELL_FS_O_RDWR)
                        ? ((flags & CELL_FS_O_APPEND) ? "ab+" : "rb+")
                        : (acc == CELL_FS_O_WRONLY)
                          ? ((flags & CELL_FS_O_APPEND) ? "ab" : "wb")
                          : "rb";
    FILE* f = fdopen(hfd, fmode);
    if (!f) {
        close(hfd);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return;
    }
    int fd = fd_alloc_file(f);
    if (fd < 0) { fclose(f); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
    g_fd_usm[fd] = (strstr(gpath, ".usm") != nullptr) ? 1 : 0;
    g_fd_cut[fd] = (uint8_t)fs_is_story_cutscene(gpath);
    if (g_fd_cut[fd]) { g_fs_cutscene_open++; g_fd_cut_ms[fd] = fs_now_ms(); }
    if (strstr(gpath, "_igcs/") && strstr(gpath, "_shot")) g_igc_shot_ms = fs_now_ms();   /* v1042S */
    /* FS_CALLER=1: name the guest function that opened each file. A title whose
     * streaming layer opens a file and never reads it (You Don't Know Jack's
     * movies) is diagnosed from the opener: its sibling read path is the one
     * that is not running. */
    if (fd >= 0 && fd < FS_MAX) { strncpy(g_fd_path[fd], gpath, sizeof g_fd_path[fd]-1); g_fd_path[fd][sizeof g_fd_path[fd]-1]=0; }
    { char who[64] = "?";
      if (getenv("FS_CALLER")) { ppu_guest_caller(who, sizeof who);
          fprintf(stderr, "[fs] open '%s' -> fd %d  (opened by %s)\n", gpath, fd, who);
          /* FS_STACK=<substr>: every open funnels through one guest wrapper, so
           * one caller level says nothing about WHICH subsystem wanted the file.
           * Dump the guest call chain for the opens that matter. */
          { const char* want = getenv("FS_STACK");
            if (want && *want && strstr(gpath, want)) ydkj_host_bt("fs-open");
          } }
      else fprintf(stderr, "[fs] open '%s' -> fd %d\n", gpath, fd); }
    if (getenv("PS3_FSLOG_BT") && strstr(gpath, ".usm")) {
        /* Resolve to GUEST functions (raw host RVAs are useless here): this tells us
         * which criMv/criFs function opened the movie, so the reader-attach path
         * (stream+0x10, never wired => movie opened but never read) can be found. */
        fprintf(stderr,"[USMBT] open '%s' -> fd %d\n", gpath, fd); fflush(stderr);
        ydkj_host_bt("usm-open");
    }
    ctx->gpr[3] = CELL_OK;
}

/* cellFsSdataOpen(path, flags, fd*, arg*, size): opens a PS3 SDATA/EDAT
 * encrypted-data file and returns an fd whose reads yield the DECRYPTED
 * plaintext. LBP 1.30 stores its SPU job code modules inside patch.sdat (an
 * FSHb container, zlib-compressed C0DEC0DE modules) and opens it via this call;
 * leaving it faked (default CELL_OK, no handle) meant the container was never
 * read -> zero job modules loaded -> every SPU physics job dispatched into an
 * empty code buffer -> the loading freeze.
 *
 * Our run env's PS3_HDD0_ROOT (RPCS3 dev_hdd0) already holds patch.sdat in
 * DECRYPTED form (magic "FSHb", not "NPD"), so we just open it read-only --
 * no EDAT decryption needed. (A title supplying a still-encrypted NPD .sdat
 * would need the SDATA block cipher; not required here.) The license `arg`
 * (gpr[6]) and its size (gpr[7]) are ignored. */
static void cellFsSdataOpen(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t fd_ptr = (uint32_t)ctx->gpr[5];
    host_path(hpath, sizeof hpath, gpath);

    /* Same NPDRM step cellFsOpen does: an encrypted container is decrypted once
     * into a cache file and that is opened in its place. This used to be a
     * second, weaker decryptor local to this function (sdata_decrypt below),
     * which handled the self-keyed SDATA form only and refused every real EDAT
     * -- so a title whose data file was EDAT rather than SDATA got "success
     * without a handle" here while the very same file opened fine through
     * cellFsOpen. One decryptor, reached from both doors. */
    { char dec_path[1200];
      const char* use = edat_resolve(hpath, dec_path, sizeof dec_path);
      if (use != hpath) snprintf(hpath, sizeof hpath, "%s", use); }

    int hfd = fs_open(hpath, O_RDONLY | O_BINARY, 0666);
    if (hfd < 0) {
        fprintf(stderr, "[fs] SdataOpen FAIL '%s' -> '%s'\n", gpath, hpath);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return;
    }
    FILE* f = fdopen(hfd, "rb");
    if (!f) { close(hfd); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    /* If the file IS still NPD-encrypted, we can't serve it plaintext here --
     * warn loudly rather than hand back ciphertext the title will choke on.
     * Use portable fread/fseek (POSIX read()/ssize_t aren't available under the
     * exe's compiler). */
    unsigned char magic[4] = {0};
    size_t got = fread(magic, 1, 4, f);
    fseek(f, 0, SEEK_SET);
    if (got == 4 && magic[0]=='N' && magic[1]=='P' && magic[2]=='D') {
        /* NPD-encrypted SDATA: decrypt it in full and serve the plaintext from
         * an anonymous tmpfile so the title's reads see the real container. */
        fseek(f, 0, SEEK_END); long enc_sz = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t* enc = (enc_sz > 0) ? (uint8_t*)malloc((size_t)enc_sz) : nullptr;
        size_t rd = enc ? fread(enc, 1, (size_t)enc_sz, f) : 0;
        fclose(f);
        size_t dec_sz = 0;
        uint8_t* dec = (enc && rd == (size_t)enc_sz) ? sdata_decrypt(enc, rd, &dec_sz) : nullptr;
        free(enc);
        if (!dec) {
            fprintf(stderr, "[fs] SdataOpen '%s' NPD decrypt FAILED (unsupported "
                    "EDAT/needs license); returning success without a handle\n", gpath);
            ctx->gpr[3] = CELL_OK; return;   /* don't feed the title ciphertext */
        }
        char dmagic[4] = {0};
        if (dec_sz >= 4) memcpy(dmagic, dec, 4);
        FILE* tf = tmpfile();
        if (!tf || fwrite(dec, 1, dec_sz, tf) != dec_sz) {
            if (tf) fclose(tf); free(dec);
            ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return;
        }
        free(dec);
        rewind(tf);
        int fd = fd_alloc_file(tf);
        if (fd < 0) { fclose(tf); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
        if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
        fprintf(stderr, "[fs] SdataOpen '%s' -> fd %d (NPD decrypted, 0x%zX bytes, magic '%c%c%c%c')\n",
                gpath, fd, dec_sz,
                dmagic[0]?dmagic[0]:'?', dmagic[1]?dmagic[1]:'?',
                dmagic[2]?dmagic[2]:'?', dmagic[3]?dmagic[3]:'?');
        ctx->gpr[3] = CELL_OK;
        return;
    }
    int fd = fd_alloc_file(f);
    if (fd < 0) { fclose(f); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
    fprintf(stderr, "[fs] SdataOpen '%s' -> fd %d (magic %c%c%c%c)\n", gpath, fd,
            magic[0]?magic[0]:'?', magic[1]?magic[1]:'?', magic[2]?magic[2]:'?', magic[3]?magic[3]:'?');
    ctx->gpr[3] = CELL_OK;
}

static void cellFsClose(ppu_context* ctx)
{
    int fd = (int)(uint32_t)ctx->gpr[3];
    if (getenv("FS_CALLER")) { char w[64]="?"; ppu_guest_caller(w,sizeof w);
        fprintf(stderr, "[fs] close fd=%d  (by %s)\n", fd, w); }
    if (fd >= 0 && fd < FS_MAX && g_files[fd]) {
        if (g_fd_usm[fd] && getenv("PS3_FSLOG_READS")) fprintf(stderr, "[USMRD] CLOSE usm fd=%d\n", fd);
        fclose(g_files[fd]); g_files[fd] = nullptr; g_fd_usm[fd] = 0;
        if (g_fd_cut[fd]) { g_fd_cut[fd] = 0; if (g_fs_cutscene_open > 0) g_fs_cutscene_open--; }
        g_fd_snu_end[fd] = 0; memset(g_fd_lr_n[fd], 0, sizeof g_fd_lr_n[fd]);   /* v1042Z */
    }
    ctx->gpr[3] = CELL_OK;
}

/* Pre-fault a guest range so the kernel can write into it.
 *
 * The flat VM is MEM_RESERVEd and each page is committed on FIRST ACCESS by a
 * vectored exception handler. That covers CPU access from lifted code, but
 * fread/fwrite move data through the kernel, and a kernel write to a reserved
 * page does not raise a user-mode exception -- the I/O just fails, returning 0
 * with ferror set. Twisted Metal hit this on every read whose destination the
 * guest had not touched yet: read 1 of a file succeeded, read 2 into a fresh
 * page came back n=0 eof=0 err=1, and the title logged its own
 * "Short read ... Possible reasons include disc eject" and gave up.
 *
 * Touch each page read-then-write so the fault happens here, in user mode,
 * where the handler can commit it. Read-then-write rather than a plain store so
 * nothing already in the buffer is disturbed. */
static inline void fs_prefault(uint32_t buf, uint64_t len)
{
    if (!vm_base || !len) return;
    volatile uint8_t* p = (volatile uint8_t*)(vm_base + buf);
    for (uint64_t o = 0; o < len; o += 0x1000) p[o] = p[o];
    p[len - 1] = p[len - 1];
}

static void cellFsRead(ppu_context* ctx)
{
    int fd          = (int)(uint32_t)ctx->gpr[3];
    uint32_t buf    = (uint32_t)ctx->gpr[4];
    uint64_t nbytes = ctx->gpr[5];
    uint32_t nread_ptr = (uint32_t)ctx->gpr[6];
    if (fd < 0 || fd >= FS_MAX || !g_files[fd]) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    if (g_fd_cut[fd]) g_fd_cut_ms[fd] = fs_now_ms();   /* v1042S */
    /* A skip must preserve the stream's bytes, file position and read counts.
     * Returning premature EOF to mute compressed dialogue triggers the title's
     * disc-error/retry path, repeatedly reopening the same IGC stream. Keep
     * normal reads during fast-forward; muting belongs at audio output. */
    uint64_t raw_nbytes = ctx->gpr[5];
    long fpos_before = ftell(g_files[fd]);
    if (ppu_vm_size && (uint64_t)buf + nbytes > ppu_vm_size) nbytes = ppu_vm_size - buf;
    fs_prefault(buf, nbytes);
    const unsigned long long _hz0 = hz_tsc();   /* v225A */
    size_t n = fread(vm_base + buf, 1, (size_t)nbytes, g_files[fd]);   /* raw bytes, no swap */
    { const long long _hzd = (long long)(hz_tsc() - _hz0); hz_add(HZ_FS_TSC, _hzd); hz_add(HZ_FS_N, 1); hz_add(HZ_FS_BYTES, (long long)n);
      if (ps3_hz_is_main()) { hz_add(HZ_FS_MAIN_TSC, _hzd); hz_add(HZ_FS_MAIN_N, 1); } }
    /* v114 FS_READ_DELAY_MS=<ms> (default 0 = off): CAUSAL TEST, not a fix. Sleep after every exactly-64 KB
     * read (the FIOS world-streaming chunks of infamous1.psarc_s; loading reads are ~490 KB and unaffected).
     * If the world-movement hitches get longer by about the added burst time, streaming speed causes them. */
    { static long dms = -1;
      if (dms < 0) { const char* e = getenv("FS_READ_DELAY_MS"); dms = e ? atol(e) : 0; if (dms < 0) dms = 0;
                     if (dms) fprintf(stderr, "[fs] v114 CAUSAL TEST: +%ld ms after every 64 KB read (FS_READ_DELAY_MS)\n", dms); }
      if (dms && nbytes == 65536) std::this_thread::sleep_for(std::chrono::milliseconds(dms)); }
    /* PS3_FSLOG_EOF=<fd>,<bytes>: report end-of-file past <bytes> on one descriptor.
     * The intro cinematic is 28 seconds and this port renders it at a few frames
     * a second, so it cannot be watched to its end -- truncating the stream makes
     * the demuxer see EOF and the movie finish, which is what advances the title
     * to whatever follows the intro. Deliberately a testing knob, not a fix. */
    { static int efd = -2; static long elim = 0;
      if (efd == -2) { const char* e = getenv("PS3_FSLOG_EOF");
          if (e) { efd = atoi(e); const char* c = strchr(e, 44); elim = c ? atol(c + 1) : 0; }
          else efd = -1; }
      if (efd >= 0 && fd == efd && elim > 0 && fpos_before >= elim) {
          static int once = 0;
          if (!once++) fprintf(stderr, "[fs] PS3_FSLOG_EOF: fd=%d truncated at %ld bytes\n", fd, elim);
          n = 0;
      } }
    if (g_fd_usm[fd] && getenv("PS3_FSLOG_READS")) fprintf(stderr, "[USMRD] READ usm fd=%d nbytes=%llu -> %zu magic=%02X%02X%02X%02X pos=%ld lr=0x%08X\n", fd, (unsigned long long)nbytes, n, vm_base[buf], vm_base[buf+1], vm_base[buf+2], vm_base[buf+3], fpos_before, (uint32_t)ctx->lr);
    /* PS3_FSLOG_READS=<fd>: log every read on one descriptor. PS3_FSLOG caps at 20
     * lines and they are all spent before a movie ever opens, so it cannot
     * answer "is the streamer reading the .avi". */
    { static int wfd = -2;
      if (wfd == -2) { const char* e = getenv("PS3_FSLOG_READS"); wfd = e ? atoi(e) : -1; }
      if (wfd >= 0 && fd == wfd)
          fprintf(stderr, "[fsread] fd=%d want=%llu got=%zu pos=%ld\n",
                  fd, (unsigned long long)nbytes, n, fpos_before); }
    if (getenv("PS3_FSLOG")) { static int _fd=0; if((_fd < 20 && _fd++ < 20)) fprintf(stderr,"[FSDBG] fd=%d raw_nbytes=0x%llX clamped=0x%llX buf=0x%08X fpos_before=%ld n=%zu eof=%d err=%d\n", fd,(unsigned long long)raw_nbytes,(unsigned long long)nbytes,buf,fpos_before,n,feof(g_files[fd]),ferror(g_files[fd])); }
#ifdef _WIN32
    if (getenv("PS3_FSLOG") && buf==0 && raw_nbytes>0x10000) { static int _b=0; if((_b < 2 && _b++ < 2)){ void* fr[30]; unsigned short nn=RtlCaptureStackBackTrace(0,30,fr,0); uintptr_t mb=(uintptr_t)GetModuleHandleA(0); fprintf(stderr,"[FSBT] null-buf read caller rvas:"); for(unsigned short i=0;i<nn&&i<16;i++) fprintf(stderr," %llX",(unsigned long long)((uintptr_t)fr[i]-mb)); fprintf(stderr,"\n"); } }
#endif
    /* Per-fd totals, not just the first 50 lines. The flat cap made "this file is
     * opened and never read" unfalsifiable: reads on a later-opened fd fall off
     * the end of the log and look identical to reads that never happen.
     * FS_READ_ALL=1 logs every read; otherwise a per-fd first-read line plus a
     * periodic summary is enough to tell the two apart. */
    { static uint64_t tot=0; static int _n=0;
      static uint64_t per_fd[64]; static uint32_t cnt_fd[64];
      tot+=n;
      if (fd>=0 && fd<64) { per_fd[fd]+=n; cnt_fd[fd]++; }
      int first_for_fd = (fd>=0 && fd<64 && cnt_fd[fd]==1);
      if((_n < 50 && _n++ < 50) || first_for_fd || getenv("FS_READ_ALL"))
        fprintf(stderr,"[fs] read fd=%d nbytes=%llu -> %zu (magic=%02X%02X%02X%02X, total=%llu)%s\n",
                fd,(unsigned long long)nbytes,n,vm_base[buf],vm_base[buf+1],vm_base[buf+2],vm_base[buf+3],
                (unsigned long long)tot, first_for_fd?"  <= FIRST READ ON THIS FD":"");
      if (getenv("FS_READ_PATH") && fd>=0 && fd<64 && g_fd_path[fd][0])
          fprintf(stderr, "        from %s\n", g_fd_path[fd]);
      if ((_n % 2000)==0) { fprintf(stderr,"[fs] read summary after %d reads:",_n);
          for (int i=0;i<64;i++) if (cnt_fd[i]) fprintf(stderr," fd%d=%ux/%lluB",i,cnt_fd[i],(unsigned long long)per_fd[i]);
          fprintf(stderr,"\n"); } }
    if (getenv("PS3_FSLOG_TOC") && nbytes >= 50000) {  /* data.toc read -> who parses it? */
        fprintf(stderr, "[TOC] data.toc read into buf=0x%08X n=%zu; lr=0x%08llX; guest-stack RAs:\n", buf, n, (unsigned long long)ctx->lr);
        uint32_t sp = (uint32_t)ctx->gpr[1];
        for (uint32_t i = 0; i < 128 && sp + i*4 + 4 <= ppu_vm_size; i++) {
            uint32_t a = sp + i*4; uint32_t w = (vm_base[a]<<24)|(vm_base[a+1]<<16)|(vm_base[a+2]<<8)|vm_base[a+3];
            if (w >= 0x10000u && w < 0x600000u) fprintf(stderr, "[TOC]   ra 0x%08X (@sp+0x%X)\n", w, i*4);
        }
    }
    /* v1042Z GF_SKIP_AUDIO_END: remember the last chunk of a cutscene stream; after a skip, end the track inside it */
    if (g_fd_cut[fd] && n) {
        const int r = g_fd_lr_head[fd]++ % SNU_RING;
        g_fd_lr_buf[fd][r] = buf; g_fd_lr_pos[fd][r] = fpos_before; g_fd_lr_n[fd][r] = (uint32_t)n;
        if (g_fd_snu_end[fd]) snu_patch_range(fd, buf, fpos_before, (uint32_t)n, "read after skip");
    }
    if (nread_ptr) vm_write64(nread_ptr, n);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsWrite(ppu_context* ctx)
{
    int fd          = (int)(uint32_t)ctx->gpr[3];
    uint32_t buf    = (uint32_t)ctx->gpr[4];
    uint64_t nbytes = ctx->gpr[5];
    uint32_t nwr_ptr = (uint32_t)ctx->gpr[6];
    if (fd < 0 || fd >= FS_MAX || !g_files[fd]) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    fs_prefault(buf, nbytes);   /* kernel READS the buffer; same reserved-page trap */
    size_t n = fwrite(vm_base + buf, 1, (size_t)nbytes, g_files[fd]);
    /* DIAGNOSTIC (PS3_FSLOG_BT=1): dump the guest back-chain when the game logs the
     * render-config failure, to locate setScreenRenderTargetInternal & the config obj. */
    if (getenv("PS3_FSLOG_BT") && buf && nbytes > 0 && nbytes < 4096 && vm_base) {
        char tmp[256]; uint32_t nn = (uint32_t)(nbytes < 255 ? nbytes : 255);
        memcpy(tmp, vm_base + buf, nn); tmp[nn] = 0;
        if (strstr(tmp,"config") || strstr(tmp,"Config") || strstr(tmp,"Mystery") ||
            strstr(tmp,"downsample") || strstr(tmp,"RenderTarget")) {
            uint32_t sp = (uint32_t)ctx->gpr[1];
            fprintf(stderr, "[cfgbt] write \"%.60s\" lr=0x%08X sp=0x%08X\n", tmp, (uint32_t)ctx->lr, sp);
            for (int i = 0; i < 24 && sp && sp < 0x10000000u; i++) {
                uint32_t nsp; memcpy(&nsp, vm_base + sp, 4);
                nsp = ((nsp>>24)&0xFF)|((nsp>>8)&0xFF00)|((nsp<<8)&0xFF0000)|((nsp<<24)&0xFF000000);
                if (nsp <= sp || nsp >= 0x10000000u) break;
                uint32_t lr; memcpy(&lr, vm_base + nsp + 0x10, 4);
                lr = ((lr>>24)&0xFF)|((lr>>8)&0xFF00)|((lr<<8)&0xFF0000)|((lr<<24)&0xFF000000);
                fprintf(stderr, "[cfgbt]   #%d lr=0x%08X\n", i, lr);
                sp = nsp;
            }
            fflush(stderr);
        }
    }
    if (nwr_ptr) vm_write64(nwr_ptr, n);
    ctx->gpr[3] = CELL_OK;
}

/* cellFsFGetBlockSize(int fd, u64* sector_size, u64* block_size)
 * cellFsGetBlockSize(const char* path, u64* sector_size, u64* block_size)
 *
 * Titles ask the filesystem what granularity it wants and then size and ALIGN
 * their reads to the answer. Left unregistered, the call fell through to the
 * unresolved-NID path: the out-params were never written, so the title read
 * whatever was already in them.
 *
 * The Orange Box's chooser does exactly this. With no answer it re-seeks to
 * offset 0 and re-reads the same 64 KB of APP_CHOOSER.GRP forever -- 241,997
 * reads and 15.86 GB of I/O against a 4 MB file in one run -- because the
 * offset arithmetic it derives from the block size collapses to zero.
 *
 * Both out-params are u64 and big-endian, so they go through vm_write64.
 * PS3_FS_BLOCK_SIZE overrides the value for a title that wants a different
 * granularity. */
static void fs_report_block_size(ppu_context* ctx, uint32_t sec_ptr, uint32_t blk_ptr)
{
    static uint64_t bs = 0;
    if (!bs) {
        const char* e = getenv("PS3_FS_BLOCK_SIZE");
        bs = (e && *e) ? (uint64_t)strtoull(e, nullptr, 0) : 4096ull;
        if (!bs) bs = 4096ull;
    }
    if (sec_ptr) vm_write64(sec_ptr, bs);
    if (blk_ptr) vm_write64(blk_ptr, bs);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsFGetBlockSize(ppu_context* ctx)
{
    int fd = (int)(uint32_t)ctx->gpr[3];
    if (fd < 0 || fd >= FS_MAX || !g_files[fd]) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO;
        return;
    }
    fs_report_block_size(ctx, (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5]);
}

static void cellFsGetBlockSize(ppu_context* ctx)
{
    /* Path variant: the answer does not depend on the file, only the device,
     * and every mount we serve is backed by the same host filesystem. */
    fs_report_block_size(ctx, (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5]);
}

static void cellFsLseek(ppu_context* ctx)
{
    int fd        = (int)(uint32_t)ctx->gpr[3];
    int64_t off   = (int64_t)ctx->gpr[4];
    uint32_t wh   = (uint32_t)ctx->gpr[5];
    uint32_t pos_ptr = (uint32_t)ctx->gpr[6];
    if (fd < 0 || fd >= FS_MAX || !g_files[fd]) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    if (getenv("FS_CALLER")) { char w[64]="?"; ppu_guest_caller(w,sizeof w);
        fprintf(stderr, "[fs] lseek fd=%d off=%lld whence=%u  (by %s)\n", fd, (long long)off, wh, w); }
    int worigin = (wh == CELL_FS_SEEK_END) ? SEEK_END : (wh == CELL_FS_SEEK_CUR) ? SEEK_CUR : SEEK_SET;
    fseek(g_files[fd], (long)off, worigin);
    long p = ftell(g_files[fd]);
    if (pos_ptr) vm_write64(pos_ptr, (uint64_t)p);
    ctx->gpr[3] = CELL_OK;
}

/* CellFsStat is 0x34 (52) bytes, 4-byte aligned -- the s64/u64 members are
 * be_t<...,4> so there is NO 4-byte pad after gid (verified vs RPCS3:
 * CHECK_SIZE_ALIGN(CellFsStat, 52, 4)). Laying it out 8-byte-aligned (0x38,
 * pad@0x0C) shifts size/blksize +4 and overruns the struct by 4 bytes -- games
 * that embed a CellFsStat inside a larger object (e.g. Dantelion's
 * DLFileDeviceStream, stat@obj+0xD8) then have the trailing blksize clobber the
 * field right after the stat (the fd at obj+0x10c), which later fails lseek.
 * Layout: mode@0 uid@4 gid@8 atime@0x0C mtime@0x14 ctime@0x1C size@0x24 blksize@0x2C. */
static void write_stat(uint32_t sb, uint32_t mode, uint64_t size)
{
    vm_write32(sb + 0x00, mode);
    vm_write32(sb + 0x04, 0);            /* uid */
    vm_write32(sb + 0x08, 0);            /* gid */
    vm_write64(sb + 0x0C, 0);            /* atime */
    vm_write64(sb + 0x14, 0);            /* mtime */
    vm_write64(sb + 0x1C, 0);            /* ctime */
    vm_write64(sb + 0x24, size);         /* size */
    vm_write64(sb + 0x2C, 0x200);        /* blksize */
}

static void cellFsStat(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t sb = (uint32_t)ctx->gpr[4];
    host_path(hpath, sizeof hpath, gpath);
    struct stat st;
    if (fs_stat(hpath, &st) != 0) {
        if (getenv("PS3_FSLOG")) fprintf(stderr, "[fs] stat '%s' -> ENOENT\n", gpath);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return;
    }
    if (getenv("PS3_FSLOG")) fprintf(stderr, "[fs] stat '%s' -> OK (size=%lld)\n", gpath, (long long)st.st_size);
    uint32_t mode = (st.st_mode & S_IFDIR) ? (CELL_FS_S_IFDIR | 0x1FF)
                                           : (CELL_FS_S_IFREG | 0x1B6);
    if (sb) write_stat(sb, mode, (uint64_t)st.st_size);
    if (getenv("PS3_FSLOG") && strstr(gpath,".toc")) fprintf(stderr,"[FSDBG] cellFsStat('%s') -> size=0x%llX\n",gpath,(unsigned long long)st.st_size);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsFstat(ppu_context* ctx)
{
    int fd      = (int)(uint32_t)ctx->gpr[3];
    uint32_t sb = (uint32_t)ctx->gpr[4];
    if (fd < 0 || fd >= FS_MAX || !g_files[fd]) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    if (getenv("FS_CALLER")) { char w[64]="?"; ppu_guest_caller(w,sizeof w);
        fprintf(stderr, "[fs] fstat fd=%d  (by %s)\n", fd, w); }
    long cur = ftell(g_files[fd]);
    fseek(g_files[fd], 0, SEEK_END);
    long sz = ftell(g_files[fd]);
    fseek(g_files[fd], cur, SEEK_SET);
    /* FS_FSTAT_CAP=<bytes>: DIAGNOSTIC. Report a smaller size than the file
     * has. You Don't Know Jack reads a 100 KB archive whole but only probes
     * (open/fstat/close) its 500 KB and 2.4 MB ones; if that branch is driven
     * by the size it just asked for, capping it makes the title take the read
     * path. Answers whether the split is size-based -- nothing more. */
    { const char* cap = getenv("FS_FSTAT_CAP");
      const char* only = getenv("FS_FSTAT_CAP_PATH");
      if (cap && only && *only && !(g_fd_path[fd][0] && strstr(g_fd_path[fd], only))) cap = 0;
      if (cap) { long c = strtol(cap, 0, 0);
          if (c > 0 && sz > c) {
              fprintf(stderr, "[fs] fstat fd=%d size %ld -> capped %ld (FS_FSTAT_CAP)\n", fd, sz, c);
              sz = c; } } }
    if (sb) write_stat(sb, CELL_FS_S_IFREG | 0x1B6, (uint64_t)sz);
    if (getenv("PS3_FSLOG")) { static int _n=0; if((_n < 12 && _n++ < 12)) fprintf(stderr,"[FSDBG] cellFsFstat(fd=%d) -> size=0x%lX\n",fd,sz); }
    ctx->gpr[3] = CELL_OK;
}

static void cellFsOpendir(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t fd_ptr = (uint32_t)ctx->gpr[4];
    host_path(hpath, sizeof hpath, gpath);
    DIR* d = opendir(hpath);
    if (getenv("PS3_FSLOG")) fprintf(stderr, "[fs] opendir '%s' -> %s\n", gpath, d ? "OK" : "ENOENT");
    if (!d) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return; }
    int fd = fd_alloc_dir(d);
    if (fd < 0) { closedir(d); ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    strncpy(g_dir_path[fd], hpath, sizeof g_dir_path[fd] - 1);
    if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
    ctx->gpr[3] = CELL_OK;
}

/* CellFsDirent: d_type(1) d_namlen(1) d_name[256]; total 0x102. */
static void cellFsReaddir(ppu_context* ctx)
{
    int fd          = (int)(uint32_t)ctx->gpr[3];
    uint32_t dirent = (uint32_t)ctx->gpr[4];
    uint32_t nread_ptr = (uint32_t)ctx->gpr[5];
    if (fd < 0 || fd >= FS_MAX || !g_dirs[fd]) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    struct dirent* e = readdir(g_dirs[fd]);
    if (!e) { if (nread_ptr) vm_write64(nread_ptr, 0); ctx->gpr[3] = CELL_OK; return; }
    char full[1300]; struct stat st;
    snprintf(full, sizeof full, "%s/%s", g_dir_path[fd], e->d_name);
    uint8_t type = (fs_stat(full, &st) == 0 && (st.st_mode & S_IFDIR))
                   ? CELL_FS_TYPE_DIR : CELL_FS_TYPE_REG;
    size_t nl = strlen(e->d_name); if (nl > 255) nl = 255;
    vm_base[dirent + 0] = type;
    vm_base[dirent + 1] = (uint8_t)nl;
    for (size_t i = 0; i < nl; i++) vm_base[dirent + 2 + i] = (uint8_t)e->d_name[i];
    vm_base[dirent + 2 + nl] = 0;
    if (nread_ptr) vm_write64(nread_ptr, 0x102);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsClosedir(ppu_context* ctx)
{
    int fd = (int)(uint32_t)ctx->gpr[3];
    if (fd >= 0 && fd < FS_MAX && g_dirs[fd]) { closedir(g_dirs[fd]); g_dirs[fd] = nullptr; }
    ctx->gpr[3] = CELL_OK;
}

#ifdef _WIN32
#include <direct.h>
#define HOST_MKDIR(p) _mkdir(p)
#else
#define HOST_MKDIR(p) mkdir((p), 0777)
#endif

/* Create `path` and any missing parent dirs on the host. Returns 0 on
 * success or if the directory already exists. */
static int host_mkdir_p(const char* path)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t len = strlen(tmp);
    while (len > 1 && (tmp[len-1] == '/' || tmp[len-1] == '\\')) tmp[--len] = 0;
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') { char c = *p; *p = 0; HOST_MKDIR(tmp); *p = c; }
    }
    int r = HOST_MKDIR(tmp);
    if (r != 0) { struct stat st; if (stat(tmp, &st) == 0 && (st.st_mode & S_IFDIR)) r = 0; }
    return r;
}

/* cellFsMkdir(path, mode): create the guest directory on the host. A no-op stub
 * here silently breaks games that create then poll a dir (e.g. LBP's boot waits
 * for /dev_hdd0/game/<title>/USRDIR to appear). Create parents too. */
static void cellFsMkdir(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    host_path(hpath, sizeof hpath, gpath);
    int r = host_mkdir_p(hpath);
    if (getenv("PS3_FSLOG")) fprintf(stderr, "[fs] mkdir '%s' -> '%s' (%s)\n", gpath, hpath, r == 0 ? "OK" : "FAIL");
    ctx->gpr[3] = CELL_OK;   /* existing dir is benign for boot */
}
static void cellFsRmdir(ppu_context* ctx)  { ctx->gpr[3] = CELL_OK; }
static void cellFsUnlink(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }
static void cellFsFsync(ppu_context* ctx)
{
    int fd = (int)(uint32_t)ctx->gpr[3];
    if (fd >= 0 && fd < FS_MAX && g_files[fd]) fflush(g_files[fd]);
    ctx->gpr[3] = CELL_OK;
}
/* File-area preallocation (LBP cache warm-up spams this): host filesystems
 * grow files on write, so accepting is correct -- no preallocation needed. */
static void cellFsAllocateFileAreaWithoutZeroFill(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }

/* ---- cellFs AIO -----------------------------------------------------------
 *
 * Scott Pilgrim loads everything through cellFsAioRead: it opens gamedata.fat
 * with cellFsOpen and then never calls cellFsRead again. With the AIO NIDs
 * unresolved, its loader thread submitted requests that nothing ever completed
 * and the whole boot parked -- one open, no reads, and the main thread spinning.
 *
 * CellFsAio, big-endian, 0x28 bytes:
 *   0x00 u32 fd    0x08 u64 offset    0x10 u32 buf    0x18 u64 size    0x20 u64 user_data
 *
 * The completion callback is (CellFsAio* aio, s32 error, s32 id, u64 size).
 *
 * ponytail: the read runs synchronously and the callback fires before the
 * submit call returns. A title that submits, THEN arms the thing the callback
 * signals, would miss it -- move completion to a worker thread if one shows up.
 */
#ifdef _WIN32
#  define HOST_FSEEK64(f,off) _fseeki64((f), (__int64)(off), SEEK_SET)
#else
#  define HOST_FSEEK64(f,off) fseeko((f), (off_t)(off), SEEK_SET)
#endif

static uint32_t guest_be32(uint32_t a)
{
    if (ppu_vm_size && (uint64_t)a + 4 > ppu_vm_size) return 0;
    return ((uint32_t)vm_base[a] << 24) | ((uint32_t)vm_base[a+1] << 16) |
           ((uint32_t)vm_base[a+2] << 8) | (uint32_t)vm_base[a+3];
}
static uint64_t guest_be64(uint32_t a)
{
    return ((uint64_t)guest_be32(a) << 32) | guest_be32(a + 4);
}

static void cellFsAioInit(ppu_context* ctx)   { ctx->gpr[3] = CELL_OK; }
static void cellFsAioFinish(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }
/* Nothing is ever outstanding, so a cancel has nothing to find. Real cellFs
 * answers CELL_FS_ENOENT for an unknown id and callers treat that as "already
 * done", which is exactly true here. */
static void cellFsAioCancel(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }

static void cellFsAioRead(ppu_context* ctx)
{
    uint32_t aio    = (uint32_t)ctx->gpr[3];
    uint32_t id_ptr = (uint32_t)ctx->gpr[4];
    uint32_t cb_opd = (uint32_t)ctx->gpr[5];

    int      fd     = (int)guest_be32(aio + 0x00);
    if (fd >= 0 && fd < FS_MAX && g_fd_cut[fd]) g_fd_cut_ms[fd] = fs_now_ms();   /* v1042S */
    uint64_t offset = guest_be64(aio + 0x08);
    uint32_t buf    = guest_be32(aio + 0x10);
    uint64_t size   = guest_be64(aio + 0x18);

    static int32_t s_next_id = 1;
    int32_t id = s_next_id++;
    if (id_ptr) vm_write32(id_ptr, (uint32_t)id);

    size_t n = 0;
    int32_t err = CELL_FS_ENOENT;
    if (fd >= 0 && fd < FS_MAX && g_files[fd]) {
        if (ppu_vm_size && (uint64_t)buf + size > ppu_vm_size) size = ppu_vm_size - buf;
        fs_prefault(buf, size);
        /* AIO reads are absolute -- they do not disturb the fd's own file
         * position, and the guest interleaves them freely across threads. */
        const unsigned long long _hz0 = hz_tsc();   /* v225A */
        if (HOST_FSEEK64(g_files[fd], offset) == 0)
            n = fread(vm_base + buf, 1, (size_t)size, g_files[fd]);
        { const long long _hzd = (long long)(hz_tsc() - _hz0); hz_add(HZ_FS_TSC, _hzd); hz_add(HZ_FS_N, 1); hz_add(HZ_FS_BYTES, (long long)n);
          if (ps3_hz_is_main()) { hz_add(HZ_FS_MAIN_TSC, _hzd); hz_add(HZ_FS_MAIN_N, 1); } }
        err = CELL_OK;
    }
    if (getenv("PS3_FSLOG"))
        fprintf(stderr, "[fs] aio read id=%d fd=%d off=%llu size=%llu -> %zu\n",
                id, fd, (unsigned long long)offset, (unsigned long long)size, n);

    ctx->gpr[3] = CELL_OK;
    if (cb_opd) ps3_invoke_guest(cb_opd, aio, (uint64_t)(int64_t)err,
                                 (uint64_t)(int64_t)id, (uint64_t)n, 0, 0, 0, 0);
}

extern "C" void ppu_fs_register(void)
{
    ps3_hle_register_ctx(ps3_compute_nid("cellFsOpen"),     "cellFsOpen",     cellFsOpen);
    /* Register by the literal import NID: LBP imports 0xB1840B53 for
     * cellFsSdataOpen, which ps3_compute_nid("cellFsSdataOpen") does NOT match
     * (the real exported symbol name differs from the friendly name). */
    ps3_hle_register_ctx(0xB1840B53u, "cellFsSdataOpen", cellFsSdataOpen);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsClose"),    "cellFsClose",    cellFsClose);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsRead"),     "cellFsRead",     cellFsRead);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsWrite"),    "cellFsWrite",    cellFsWrite);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsLseek"),    "cellFsLseek",    cellFsLseek);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsFGetBlockSize"), "cellFsFGetBlockSize", cellFsFGetBlockSize);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsGetBlockSize"),  "cellFsGetBlockSize",  cellFsGetBlockSize);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsStat"),     "cellFsStat",     cellFsStat);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsFstat"),    "cellFsFstat",    cellFsFstat);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsOpendir"),  "cellFsOpendir",  cellFsOpendir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsReaddir"),  "cellFsReaddir",  cellFsReaddir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsClosedir"), "cellFsClosedir", cellFsClosedir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsMkdir"),    "cellFsMkdir",    cellFsMkdir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsRmdir"),    "cellFsRmdir",    cellFsRmdir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsUnlink"),   "cellFsUnlink",   cellFsUnlink);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsFsync"),    "cellFsFsync",    cellFsFsync);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsAllocateFileAreaWithoutZeroFill"),
                         "cellFsAllocateFileAreaWithoutZeroFill", cellFsAllocateFileAreaWithoutZeroFill);
    /* AIO by literal import NID -- ps3_compute_nid() of the friendly name does
     * not match the exported symbols. */
    ps3_hle_register_ctx(0xDB869F20u, "cellFsAioInit",   cellFsAioInit);
    ps3_hle_register_ctx(0x9F951810u, "cellFsAioFinish", cellFsAioFinish);
    ps3_hle_register_ctx(0xC1C507E7u, "cellFsAioRead",   cellFsAioRead);
    ps3_hle_register_ctx(0x7F13FC8Cu, "cellFsAioCancel", cellFsAioCancel);
}
