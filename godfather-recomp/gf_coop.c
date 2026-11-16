/* gf_coop.c -- network co-op session + player-state sync.
 *
 * Protocol: every datagram starts with GFCO, a protocol version, type, player slot, packet sequence and sender clock.
 * Hardened v2 adds the latest received packet plus a 32-packet ack bitfield to every header. STATE is an unordered,
 * latest-wins stream. EVENT is a separate ordered channel (maximum 32 outstanding, retransmitted after about 1.5 RTT).
 * HELLO/WELCOME exchange a build id and player name; PING/PONG estimate RTT and clock offset. Multibyte fields are
 * native little-endian because both current ends are x64 Windows. GF_COOP_HARDEN=1 selects v2; its default is 0 so
 * the original v1 wire behaviour remains available while the new protocol is measured. Echo reserves event type
 * 0xFFFF with payload "TEST" for its one-shot reliable-channel self-test.
 *
 * Transport: plain UDP, one socket, so it works over a LAN, a VPN or a forwarded port. Everything network
 * specific sits behind net_open / net_send / net_recv so another transport can replace it later without touching the game side.
 *
 * Launch (env, set by coop-host.cmd / coop-join.cmd):
 *   GF_COOP=host           listen on GF_COOP_PORT (default 27960), accept the first player that says hello
 *   GF_COOP=join           connect to GF_COOP_PEER (IP or host name, optional :port)
 *   GF_COOP=echo           test mode: loop our own packets back through the session path and treat them as the partner,
 *                          GF_COOP_ECHO_OFS metres to the side (default 2) and GF_COOP_ECHO_DELAY ms late (default 100)
 *   GF_COOP_NAME           player name shown to the partner (default: Windows user name)
 *   GF_COOP_RATE           state packets per second (default 30)
 *   GF_COOP_HARDEN         0 = old protocol v1; default 1 = v2 reliability, timing, build handshake, statistics (v1058D)
 * v1058D: PK_BLOB unreliable latest-wins channel (gf_coop_send_blob / gf_coop_set_blob_handler, pose stream) and the
 * internal EV_IDENTITY event (skeleton + mesh GUID, player+0x2244/+0x2254) -> gf_coop_partner_identity().
 * Log [coop]. Nothing runs without GF_COOP.
 *
 * v1072S (agent S) N PEERS -- star topology, the host relays (design: NOTES_scaling.md). Every per-connection field of the
 * old file (address, ack window, reliable queues, snapshot ring, identity, camera, RTT, blob queue) now lives in a sess
 * struct. The joiner has exactly one session (to the host). The host has up to GF_COOP_MAXPEERS-1 (default 2 -> one
 * joiner = the old behaviour, a second HELLO still gets PK_FULL). Wire: unchanged packets; HELLO / WELCOME carry 8 more
 * bytes (caps, max peers, assigned peer id; older exes check n >= sizeof and ignore them); the header slot byte = own
 * peer id (was 0 / 1, never read). New PK_PSTATE (12) / PK_PBLOB (13) = another peer's state / player pose relayed by
 * the host, only sent to sessions that announced CAP_NPEER (older exes never see them).
 * Legacy API = the PRIMARY peer (host: first connected joiner; joiner: the host): gf_coop_partner_pose / _cam / _name /
 * _identity, and legacy event / blob handlers only get the primary's traffic. Legacy sends from the host go to EVERY
 * joiner (host-authoritative world). New API (gf_coop_peers.h): peer list, per-peer pose / cam / name / pose packets,
 * per-peer sends and all-peer handlers. GF_COOP_ECHO_PEERS=k (echo only): k fake relayed peers (test aid).
 *
 * Player state (read on the coop thread, read-only): player = gf_trainer.c chain; transform = 4x4 row-vector matrix at
 * *(player+0x68)+0x10 whose translation row (+0x40) is the feet position (HANDOFF v1044P); health player+0x14C8 /
 * max +0x14CC. The matrix layout is still an assumption -- the first state of a session logs the raw 16 floats.
 * Event handlers run on the co-op thread. Game-side handlers must queue all game work for the main PPU thread. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "gf_coop_peers.h"
#include "../ps3recomp/libs/video/rsx_ui_coop.h"   /* v1119M pause-menu CO-OP tab */
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")   /* GetUserNameA */

extern uint8_t* vm_base;

/* ------------------------------------------------------------------ guest reads (any thread, guarded) */
static int g_ok(uint32_t ea) { return ea >= 0x10000u && ea < 0xF0000000u; }
static uint32_t g_rd32(uint32_t ea) { const uint8_t* p = vm_base + ea; return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static float g_rdf(uint32_t ea) { uint32_t u = g_rd32(ea); float f; memcpy(&f, &u, 4); return f; }
static uint32_t g_player(void)
{
    __try {
        uint32_t a = g_rd32(0x10181290u); if (!g_ok(a)) return 0;
        a = g_rd32(a + 4); if (!g_ok(a)) return 0;
        a = g_rd32(a); if (!g_ok(a) || a < 0x17FCu) return 0;
        a -= 0x17FCu; return g_ok(a) ? a : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* ------------------------------------------------------------------ protocol (both current ends are x64 Windows: native LE) */
#define COOP_BUILD_ID UINT64_C(0x4746424C45533043) /* BLES00043 co-op contract C */
enum { COOP_MAGIC = 0x4F434647u /* "GFCO" */, COOP_V1 = 1, COOP_V2 = 2 };
enum { PK_HELLO = 1, PK_WELCOME, PK_STATE, PK_BYE, PK_FULL, PK_EVENT, PK_PING, PK_PONG, PK_REJECT, PK_ACK, PK_BLOB, PK_PSTATE, PK_PBLOB };
/* v1058D: event types < 0x100 are internal to gf_coop.c (never reach the game handler) */
enum { EV_IDENTITY = 1, EV_RELAY = 2, EV_RELAYED = 3 };   /* v1072S relay envelopes: joiner -> host {u16 type, data}; host -> joiners {u8 origin, u16 type, data} */
enum { REJECT_PROTOCOL = 1, REJECT_BUILD = 2 };
enum { CAP_NPEER = 1u };   /* v1072S: understands PK_PSTATE / PK_PBLOB and the assigned peer id */

typedef struct { uint32_t magic; uint16_t version; uint8_t type, slot; uint32_t seq, ms; } pk_hdr1;
typedef struct { pk_hdr1 v1; uint32_t ack, ack_bits; } pk_hdr2;
typedef struct { pk_hdr1 h; char name[24]; } pk_hello1;
typedef struct { pk_hdr2 h; uint64_t build; char name[24]; } pk_hello2;
typedef struct { pk_hello2 b; uint32_t caps; uint8_t maxpeers, your_id, pad[2]; } pk_hello3;   /* v1072S extension (+8 B) */
typedef struct { uint8_t valid, flags; uint16_t pad; float m[16]; float health, health_max; } state_body;
typedef struct { pk_hdr1 h; state_body s; } pk_state1;
typedef struct { pk_hdr2 h; state_body s; } pk_state2;
typedef struct { pk_hdr2 h; uint32_t event_seq; uint16_t event_type, len; uint8_t data[1000]; } pk_event;
typedef struct { pk_hdr2 h; uint8_t channel, pad; uint16_t len; uint8_t data[1150]; } pk_blob;   /* v1058D unreliable, latest wins */
typedef struct { pk_hdr2 h; uint32_t t0; } pk_ping;
typedef struct { pk_hdr2 h; uint32_t t0, t1; } pk_pong;
typedef struct { pk_hdr1 h; uint16_t reason, expected_version; uint64_t expected_build, got_build; char text[80]; } pk_reject1;
typedef struct { pk_hdr2 h; uint16_t reason, expected_version; uint64_t expected_build, got_build; char text[80]; } pk_reject2;
typedef struct { pk_hdr2 h; uint8_t origin, pad[3]; char name[24]; state_body s; } pk_pstate;                       /* v1072S relayed state */
typedef struct { pk_hdr2 h; uint8_t origin, channel; uint16_t len; uint8_t data[1150]; } pk_pblob;                   /* v1072S relayed pose */

/* ------------------------------------------------------------------ transport (the only socket-aware section) */
typedef struct { struct sockaddr_storage storage; int len; } net_addr;
static SOCKET s_sock = INVALID_SOCKET;
static int net_open(int port)
{
    WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w)) return 0;
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); if (s_sock == INVALID_SOCKET) return 0;
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons((u_short)port);
    if (bind(s_sock, (struct sockaddr*)&a, sizeof a)) { printf("[coop] bind to UDP port %d failed (%d) -- another copy running?\n", port, WSAGetLastError()); closesocket(s_sock); s_sock = INVALID_SOCKET; WSACleanup(); return 0; }
    u_long nb = 1; ioctlsocket(s_sock, FIONBIO, &nb); return 1;
}
static void net_close(void)   /* v1119M: leave co-op at runtime (co-op thread only) */
{
    if (s_sock != INVALID_SOCKET) { closesocket(s_sock); s_sock = INVALID_SOCKET; WSACleanup(); }
}
static int net_resolve(const char* spec, int def_port, net_addr* out)
{
    char host[128], ps[16]; int port = def_port; struct addrinfo hint, *res = NULL;
    snprintf(host, sizeof host, "%s", spec); char* c = strrchr(host, ':'); if (c) { *c = 0; port = atoi(c + 1); }
    memset(&hint, 0, sizeof hint); hint.ai_family = AF_INET; hint.ai_socktype = SOCK_DGRAM; snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hint, &res) || !res) return 0;
    memset(out, 0, sizeof *out); memcpy(&out->storage, res->ai_addr, res->ai_addrlen); out->len = (int)res->ai_addrlen; freeaddrinfo(res); return 1;
}
static int net_send_to(const net_addr* to, const void* p, int n)
{
    return to && to->len && sendto(s_sock, (const char*)p, n, 0, (const struct sockaddr*)&to->storage, to->len) == n;
}
static int net_recv(void* p, int cap, net_addr* from)
{
    from->len = sizeof from->storage; int n = recvfrom(s_sock, (char*)p, cap, 0, (struct sockaddr*)&from->storage, &from->len); return n > 0 ? n : 0;
}
static int net_same(const net_addr* a, const net_addr* b) { return a->len == b->len && a->len > 0 && !memcmp(&a->storage, &b->storage, (size_t)a->len); }
static void net_describe(const net_addr* a, char* out, size_t n)
{
    const struct sockaddr_in* i = (const struct sockaddr_in*)&a->storage; char ip[64] = "?";
    inet_ntop(AF_INET, &i->sin_addr, ip, sizeof ip); snprintf(out, n, "%s:%d", ip, ntohs(i->sin_port));
}

/* ------------------------------------------------------------------ session state */
enum { MODE_OFF, MODE_HOST, MODE_JOIN, MODE_ECHO };
enum { NSNAP = 32, MAXS = GF_COOP_MAX_PEERS - 1, RSTALE_MS = 3000 };
static int s_mode = MODE_OFF, s_port = 27960, s_rate = 30, s_harden = 0, s_cs_init = 0, s_refused = 0;
static int s_maxpeers = 2, s_self_id = -1, s_echo_peers = 0, s_net_maxpeers = 2;   /* v1072S; net = the host's setting (joiner: from WELCOME) */
static float s_echo_ofs = 2.0f; static int s_echo_delay = 100;
static char s_name[24] = "Player";
static uint32_t s_v1_tx = 0;
static CRITICAL_SECTION s_cs;
/* v1119M runtime host / join / leave (pause menu > Co-op): requests from the render thread, carried out on the co-op thread */
enum { CMD_NONE = 0, CMD_HOST = 1, CMD_JOIN = 2, CMD_LEAVE = 3 };
static volatile LONG s_cmd = CMD_NONE, s_busy = 0;
static char s_cmd_addr[128], s_join_addr[128], s_last_addr[128], s_msg[128];
static ULONGLONG s_mode_since;
static void coop_note(const char* fmt, ...);
static int coop_start(int mode, const char* peer);
static void coop_stop(void);
static int ui_status(char* l1, size_t n1, char* l2, size_t n2, char* l3, size_t n3); static int ui_host(void); static int ui_join(const char* a); static void ui_leave(void);   /* v1119M (self-test) */

typedef struct { ULONGLONG due; int n; uint8_t data[1200]; } echo_ent;
static echo_ent s_echo[128]; static int s_echo_n = 0;

typedef struct { uint64_t out_packets, in_packets, missing_packets, retransmits; float out_pps, in_pps; ULONGLONG window_ms; uint64_t window_out, window_in; } coop_stat;
static coop_stat s_stat;

typedef struct { ULONGLONG t; int valid; float m[16]; float hp, hpmax; } snap;
typedef struct { snap s[NSNAP]; int n, head; } snapring;

typedef void (*gf_coop_event_fn)(uint16_t type, const void* data, int len);
static gf_coop_event_fn s_event_handler = NULL;
static gf_coop_event_fn s_event_handlers[16];   /* v1063H 4 -> 8 slots; v1072S 8 -> 16 (M request); v1060W2: several modules (look, world) each get every event >= 0x100 */
typedef void (*gf_coop_blob_fn)(uint8_t channel, const void* data, int len);
static gf_coop_blob_fn s_blob_handler = NULL;
static gf_coop_peer_event_fn s_nev[8]; static gf_coop_peer_blob_fn s_nblob[8];   /* v1072S all-peer handlers */
static uint8_t s_my_ident[32];
typedef struct { int used; uint32_t event_seq, packet_seq; uint16_t type, len; uint8_t data[1000]; ULONGLONG sent; int sends; } rel_tx;
typedef struct { int used; uint32_t seq; uint16_t type, len; uint8_t data[1000]; } rel_rx;
typedef struct { int pending; uint8_t ch; int len; uint8_t data[1150]; } blobq;
enum { NBQ = 32 };   /* v1072S: slots 0..15 = channel & 15 (old), 16..31 = relay blobs (channel 0x80 | x) */

typedef struct {   /* v1072S: one connection (joiner: s_S[0] = the host; host: one per joiner) */
    int used; volatile int connected; net_addr addr; int id; uint32_t caps; ULONGLONG last_rx, since;
    uint32_t v1_rx, pkt_tx, pkt_rx, pkt_rx_bits;
    rel_tx tx[32]; rel_rx rx[32]; uint32_t ev_tx_seq, ev_rx_next;
    snapring sr; float rtt, clock_off, jitter; int32_t last_transit; int have_transit;
    char name[24]; uint8_t ident[32]; volatile int ident_ok; int ident_sent; uint8_t ident_sent_val[32];
    volatile LONG cam_h, cam_ok; volatile ULONGLONG cam_ms;
    blobq bq[NBQ]; ULONGLONG last_ping, last_ack, last_hello; int first_state_logged;
    volatile long drops;   /* broadcast events that found no free reliable slot */
    uint16_t epoch;   /* v1126C joiner connect epoch from its HELLO (host side) */
} sess;
static sess s_S[MAXS];
/* v1126C re-join: a joiner whose own session timed out (e.g. the host stalled while loading) says HELLO again with a NEW epoch
   (pk_hello3 pad[0..1]). Before, the host kept its still-connected session (event seqs continued) while the joiner reset its
   ev_rx_next to 1 -> every reliable event (spawns!) was outside the joiner's window forever ("spawns recv 0"). Now the host
   frees that session and ignores the address for 1.5 s (every module sees the disconnect edge), then accepts it fresh. */
static uint16_t s_join_epoch; static net_addr s_rj_addr; static ULONGLONG s_rj_until; static volatile LONG s_sess_gen, st_rejoins;
int gf_coop_session_gen(void) { return (int)s_sess_gen; }   /* v1126C: +1 on every new connection (either side) */
static ULONGLONG coop_timeout_ms(const sess* p, ULONGLONG now)   /* v1126C: 20 s during the first 60 s of a connection (loading), else 6 s */
{ static int e = -1; if (e < 0) { const char* v = getenv("GF_COOP_TIMEOUT_EARLY_S"); e = v && *v ? atoi(v) : 20; if (e < 6) e = 6; }
  return (p->since && now - p->since < 60000) ? (ULONGLONG)e * 1000u : 6000u; }
typedef struct {   /* v1072S joiner: another joiner, relayed by the host (PK_PSTATE / PK_PBLOB) */
    int used, id; char name[24]; snapring sr; ULONGLONG last_rx; float jitter; int32_t last_transit; int have_transit;
    volatile LONG cam_h, cam_ok; volatile ULONGLONG cam_ms;
} rpeer;
static rpeer s_R[GF_COOP_MAX_PEERS];
typedef struct { uint8_t d[2][1100]; int l[2]; uint32_t ms[2]; int n; ULONGLONG rx; } ppose;   /* latest two player-pose packets per peer id */
static ppose s_PP[GF_COOP_MAX_PEERS];
static volatile long st_relay_state, st_relay_pose, st_rx_pstate, st_rx_pblob, st_full, st_bcast_drop, st_rev_tx, st_rev_fwd, st_rev_rx, st_rblob_fwd, st_rblob_rx;
static volatile LONG64 s_bytes_out = 0; static LONG64 s_bytes_win = 0; static float s_out_kbps = 0.0f;   /* v1063H bandwidth (payload + 28 B UDP/IP) */
static __declspec(thread) int t_cur_peer = -1;   /* v1072S: origin peer id while a handler runs (co-op thread) */

static void stat_out(void) { EnterCriticalSection(&s_cs); s_stat.out_packets++; LeaveCriticalSection(&s_cs); }
static void stat_in(void) { EnterCriticalSection(&s_cs); s_stat.in_packets++; LeaveCriticalSection(&s_cs); }
static int self_slot(void) { return s_mode == MODE_HOST ? 0 : (s_self_id > 0 ? s_self_id : 1); }
static void update_rtt(sess* p, float sample)
{
    if (sample < 0.0f || sample > 5000.0f) return;
    EnterCriticalSection(&s_cs); p->rtt = p->rtt > 0.0f ? p->rtt * 0.875f + sample * 0.125f : sample; LeaveCriticalSection(&s_cs);
}
static void fill_hdr1(pk_hdr1* h, int type, uint16_t version)
{
    h->magic = COOP_MAGIC; h->version = version; h->type = (uint8_t)type; h->slot = (uint8_t)self_slot(); h->seq = ++s_v1_tx; h->ms = (uint32_t)GetTickCount64();
}
static uint32_t next_packet_seq(sess* p) { return (uint32_t)InterlockedIncrement((volatile LONG*)&p->pkt_tx); }
static void fill_hdr2(sess* p, pk_hdr2* h, int type, uint32_t packet_seq)
{
    h->v1.magic = COOP_MAGIC; h->v1.version = COOP_V2; h->v1.type = (uint8_t)type; h->v1.slot = (uint8_t)self_slot();
    h->v1.seq = packet_seq ? packet_seq : next_packet_seq(p); h->v1.ms = (uint32_t)GetTickCount64(); h->ack = p->pkt_rx; h->ack_bits = p->pkt_rx_bits;
}
static int wire_send(sess* p, const void* d, int n)
{
    if (s_mode == MODE_ECHO) {
        if (n > (int)sizeof s_echo[0].data || s_echo_n >= (int)(sizeof s_echo / sizeof s_echo[0])) return 0;
        echo_ent* e = &s_echo[s_echo_n++]; e->due = GetTickCount64() + (ULONGLONG)(s_echo_delay > 0 ? s_echo_delay : 0); e->n = n; memcpy(e->data, d, (size_t)n); stat_out(); InterlockedAdd64(&s_bytes_out, n + 28); return 1;
    }
    if (net_send_to(&p->addr, d, n)) { stat_out(); InterlockedAdd64(&s_bytes_out, n + 28); return 1; } return 0;
}
static int wire_send_to(const net_addr* to, const void* p, int n) { if (net_send_to(to, p, n)) { stat_out(); return 1; } return 0; }

static int packet_acked(uint32_t seq, uint32_t ack, uint32_t bits)
{
    if (seq == ack) return 1; uint32_t d = ack - seq; return d >= 1 && d <= 32 && (bits & (1u << (d - 1)));
}
static int note_packet(sess* p, uint32_t seq)
{
    if (!p->pkt_rx) { p->pkt_rx = seq; p->pkt_rx_bits = 0; return 1; }
    if ((int32_t)(seq - p->pkt_rx) > 0) {
        uint32_t d = seq - p->pkt_rx;
        EnterCriticalSection(&s_cs); if (d > 1) s_stat.missing_packets += d - 1; LeaveCriticalSection(&s_cs);
        p->pkt_rx_bits = d > 32 ? 0 : (p->pkt_rx_bits << d) | (1u << (d - 1)); p->pkt_rx = seq; return 1;
    }
    uint32_t d = p->pkt_rx - seq; if (!d || d > 32) return 0;
    uint32_t bit = 1u << (d - 1); if (p->pkt_rx_bits & bit) return 0; p->pkt_rx_bits |= bit;
    EnterCriticalSection(&s_cs); if (s_stat.missing_packets) s_stat.missing_packets--; LeaveCriticalSection(&s_cs); return 1;
}
static void apply_acks(sess* p, uint32_t ack, uint32_t bits, ULONGLONG now)
{
    EnterCriticalSection(&s_cs);
    for (int i = 0; i < 32; i++) if (p->tx[i].used && packet_acked(p->tx[i].packet_seq, ack, bits)) {
        if (p->tx[i].sends == 1) { float sample = (float)(now - p->tx[i].sent); p->rtt = p->rtt > 0.0f ? p->rtt * 0.875f + sample * 0.125f : sample; }
        p->tx[i].used = 0;
    }
    LeaveCriticalSection(&s_cs);
}
static void reset_session(sess* p)   /* new connection on this session: queues, rings, sequences, identity */
{
    EnterCriticalSection(&s_cs); p->sr.n = p->sr.head = 0; memset(p->tx, 0, sizeof p->tx); memset(p->rx, 0, sizeof p->rx); p->ev_tx_seq = 0; p->ev_rx_next = 1;
    memset(p->bq, 0, sizeof p->bq); LeaveCriticalSection(&s_cs);
    p->pkt_rx = p->pkt_rx_bits = p->v1_rx = 0; p->have_transit = 0; p->ident_ok = 0; p->ident_sent = 0; p->cam_ok = 0; p->drops = 0; p->first_state_logged = 0;
}
static void sess_free(sess* p, const char* why)
{
    if (p->connected) { printf("[coop] %s %s (peer %d)\n", p->name[0] ? p->name : "partner", why, p->id); fflush(stdout); }
    p->connected = 0; reset_session(p);
    if (s_mode == MODE_JOIN) { if (!++s_join_epoch) s_join_epoch = 1; }   /* v1126C next HELLO = new epoch */
    if (s_mode == MODE_HOST) { memset(&p->addr, 0, sizeof p->addr); p->used = 0; if (p->id >= 0 && p->id < GF_COOP_MAX_PEERS) { EnterCriticalSection(&s_cs); s_PP[p->id].n = 0; LeaveCriticalSection(&s_cs); } }
}
static sess* sess_by_addr(const net_addr* a) { for (int i = 0; i < MAXS; i++) if (s_S[i].used && net_same(&s_S[i].addr, a)) return &s_S[i]; return NULL; }
static sess* sess_by_id(int id) { for (int i = 0; i < MAXS; i++) if (s_S[i].used && s_S[i].connected && s_S[i].id == id) return &s_S[i]; return NULL; }
static int sess_limit(void) { return s_mode == MODE_HOST ? (s_maxpeers - 1 < MAXS ? s_maxpeers - 1 : MAXS) : 1; }
static sess* sess_primary(void)   /* host: first connected joiner (lowest slot); joiner / echo: the one session */
{
    for (int i = 0; i < MAXS; i++) if (s_S[i].used && s_S[i].connected) return &s_S[i];
    return NULL;
}

static void ring_push(snapring* r, const state_body* st, ULONGLONG t)
{
    snap* s = &r->s[r->head]; r->head = (r->head + 1) % NSNAP; if (r->n < NSNAP) r->n++;
    s->t = t; s->valid = st->valid; memcpy(s->m, st->m, sizeof s->m); s->hp = st->health; s->hpmax = st->health_max;
}
static void transit_note(int32_t* last, int* have, float* jit, ULONGLONG t, uint32_t sender_ms)
{
    int32_t transit = (int32_t)((uint32_t)t - sender_ms); if (*have) { float d = (float)llabs((long long)transit - (long long)*last); *jit += (d - *jit) * 0.0625f; } *last = transit; *have = 1;
}
static void snap_push(sess* p, const state_body* st, ULONGLONG t, uint32_t sender_ms)
{
    EnterCriticalSection(&s_cs);
    ring_push(&p->sr, st, t);
    if (s_harden) transit_note(&p->last_transit, &p->have_transit, &p->jitter, t, sender_ms);
    LeaveCriticalSection(&s_cs);
}
static float vdot(const float* a, const float* b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
static int vnorm(float* v)
{
    float q = vdot(v, v); if (q < 1e-12f) return 0; float z = 1.0f / sqrtf(q); v[0] *= z; v[1] *= z; v[2] *= z; return 1;
}
static void matrix_blend(float out[16], const float a[16], const float b[16], float f)
{
    for (int i = 0; i < 16; i++) out[i] = a[i] + (b[i] - a[i]) * f;
    float x[3] = { out[0], out[1], out[2] }, y[3] = { out[4], out[5], out[6] }, oldz[3] = { out[8], out[9], out[10] };
    if (!vnorm(x)) { x[0] = 1; x[1] = x[2] = 0; }
    float d = vdot(x, y); y[0] -= d*x[0]; y[1] -= d*x[1]; y[2] -= d*x[2]; if (!vnorm(y)) { y[0] = -x[1]; y[1] = x[0]; y[2] = 0; if (!vnorm(y)) { y[0] = 0; y[1] = 1; y[2] = 0; } }
    float z[3] = { x[1]*y[2] - x[2]*y[1], x[2]*y[0] - x[0]*y[2], x[0]*y[1] - x[1]*y[0] };
    if (vdot(z, oldz) < 0) { z[0] = -z[0]; z[1] = -z[1]; z[2] = -z[2]; }
    for (int i = 0; i < 3; i++) { out[i] = x[i]; out[4+i] = y[i]; out[8+i] = z[i]; }
}

/* Interpolates at a jitter-aware delay. After loss, translation and rotation may extrapolate for at most 250 ms. Caller holds s_cs. */
static int ring_pose(const snapring* r, float rtt, float jitter, float out_m[16], float* hp)
{
    ULONGLONG now = GetTickCount64(), interval = (ULONGLONG)(1000 / (s_rate > 0 ? s_rate : 30)), delay = 2 * interval;
    if (s_harden) { ULONGLONG jd = (ULONGLONG)(0.5f*rtt + 2.0f*jitter); if (jd > delay) delay = jd; if (delay < 40) delay = 40; if (delay > 250) delay = 250; }
    ULONGLONG t = now > delay ? now - delay : 0; const snap *a = NULL, *b = NULL, *newest = NULL, *previous = NULL;
    for (int k = 0; k < r->n; k++) { const snap* q = &r->s[(r->head - 1 - k + NSNAP) % NSNAP]; if (!newest) newest = q; else if (!previous) previous = q; if (q->t > t) b = q; else { a = q; break; } }
    int ok = 0; float f = 0.0f;
    if (s_harden && a == newest && !b && newest && previous && newest->valid && previous->valid && newest->t > previous->t && t > newest->t && t - newest->t <= 250) {
        f = (float)(t - previous->t) / (float)(newest->t - previous->t); matrix_blend(out_m, previous->m, newest->m, f); if (hp) *hp = newest->hp; ok = 1;
    }
    else if (a && a->valid) { if (b && b->valid && b->t > a->t) { f = (float)(t - a->t) / (float)(b->t - a->t); if (s_harden) matrix_blend(out_m, a->m, b->m, f); else for (int i = 0; i < 16; i++) out_m[i] = a->m[i] + (b->m[i] - a->m[i])*f; } else memcpy(out_m, a->m, sizeof a->m); if (hp) *hp = a->hp; ok = 1; }
    return ok;
}
int gf_coop_partner_pose(float out_m[16], float* hp)
{
    int ok = 0; if (!s_cs_init) return 0;
    EnterCriticalSection(&s_cs); { const sess* p = sess_primary(); if (p) ok = ring_pose(&p->sr, p->rtt, p->jitter, out_m, hp); } LeaveCriticalSection(&s_cs);
    return ok;
}
int gf_coop_connected(void) { return s_cs_init && sess_primary() != NULL; }
const char* gf_coop_partner_name(void) { const sess* p = s_cs_init ? sess_primary() : NULL; return p ? p->name : ""; }
float gf_coop_rtt_ms(void) { float r = 0; if (s_cs_init) { EnterCriticalSection(&s_cs); { const sess* p = sess_primary(); if (p) r = p->rtt; } LeaveCriticalSection(&s_cs); } return r; }
static void add_event_handler(gf_coop_event_fn fn) { if (!fn) return; for (int i = 0; i < 16; i++) { if (s_event_handlers[i] == fn) return; if (!s_event_handlers[i]) { s_event_handlers[i] = fn; return; } }
    printf("[coop] v1072S event handler table FULL (16): handler %p dropped\n", (void*)fn); fflush(stdout); }
void gf_coop_set_event_handler(gf_coop_event_fn fn) { if (!s_cs_init) { s_event_handler = fn; add_event_handler(fn); return; } EnterCriticalSection(&s_cs); s_event_handler = fn; add_event_handler(fn); LeaveCriticalSection(&s_cs); }   /* v1060W2: adds (old name kept) */
int gf_coop_mode(void) { return s_mode; }   /* v1060W2: 0 off, 1 host, 2 join, 3 echo */
int gf_coop_local_pos(float out[3]);
static int sess_queue_event(sess* p, uint16_t type, const void* data, int len)   /* caller holds s_cs */
{
    int slot = -1; for (int i = 0; i < 32; i++) if (!p->tx[i].used) { slot = i; break; }
    if (slot < 0) return 0;
    { rel_tx* q = &p->tx[slot]; memset(q, 0, sizeof *q); q->used = 1; q->event_seq = ++p->ev_tx_seq; q->packet_seq = next_packet_seq(p); q->type = type; q->len = (uint16_t)len; if (len) memcpy(q->data, data, (size_t)len); }
    return 1;
}
/* legacy: host -> EVERY joiner (1 = the primary took it; a full queue on another joiner is counted in st_bcast_drop), joiner -> host */
int gf_coop_send_event(uint16_t type, const void* data, int len)
{
    int r = 0, any = 0; sess* prim;
    if (!s_harden || !s_cs_init || len < 0 || len > 1000 || (len && !data)) return 0;
    EnterCriticalSection(&s_cs); prim = sess_primary();
    for (int i = 0; i < MAXS; i++) { sess* p = &s_S[i]; if (!p->used || !p->connected) continue; any = 1;
        const int ok = sess_queue_event(p, type, data, len);
        if (p == prim) r = ok; else if (!ok) { p->drops++; st_bcast_drop++; } }
    LeaveCriticalSection(&s_cs); return any && r;
}
/* v1058D unreliable latest-wins blobs (pose stream). channel 0..255, len <= 1150. Handler runs on the co-op thread. */
void gf_coop_set_blob_handler(gf_coop_blob_fn fn) { if (!s_cs_init) { s_blob_handler = fn; return; } EnterCriticalSection(&s_cs); s_blob_handler = fn; LeaveCriticalSection(&s_cs); }
static int send_blob_impl(sess* p, uint8_t channel, const void* data, int len);
static void echo_fake_pose(uint8_t ch, const void* data, int len);
static void blob_put(sess* p, uint8_t channel, const void* data, int len) { blobq* q = &p->bq[(channel & 0x80) ? 16 + (channel & 15) : (channel & 15)]; q->ch = channel; q->len = len; memcpy(q->data, data, (size_t)len); q->pending = 1; }   /* caller holds s_cs */
int gf_coop_send_blob(uint8_t channel, const void* data, int len)
{
    int any = 0;
    if (!s_harden || len <= 0 || len > 1150 || !data || !s_cs_init) return 0;
    EnterCriticalSection(&s_cs);
    for (int i = 0; i < MAXS; i++) { sess* p = &s_S[i]; if (p->used && p->connected) { blob_put(p, channel, data, len); any = 1; } }
    LeaveCriticalSection(&s_cs);
    if (any && s_mode == MODE_ECHO && s_echo_peers > 0) echo_fake_pose(channel, data, len);
    return any;
}
static void blob_flush(sess* p)   /* co-op thread */
{
    for (int q = 0; q < NBQ; q++) { uint8_t buf[1150]; int len = 0; uint8_t ch = 0;
        EnterCriticalSection(&s_cs); if (p->bq[q].pending) { p->bq[q].pending = 0; len = p->bq[q].len; ch = p->bq[q].ch; memcpy(buf, p->bq[q].data, (size_t)len); } LeaveCriticalSection(&s_cs);
        if (len) send_blob_impl(p, ch, buf, len); }
}
/* v1058D partner identity: skeleton GUID (player+0x2244) then mesh GUID (+0x2254), 16 bytes each. 1 = known. */
int gf_coop_partner_identity(uint8_t out[32])
{
    int ok = 0; if (!s_cs_init) return 0;
    EnterCriticalSection(&s_cs); { const sess* p = sess_primary(); if (p && p->ident_ok) { memcpy(out, p->ident, 32); ok = 1; } } LeaveCriticalSection(&s_cs);
    return ok;
}
void gf_coop_stats(char* out, size_t n)
{
    if (!out || !n) return; if (!s_cs_init || s_mode == MODE_OFF) { snprintf(out, n, "CO-OP off"); return; }
    EnterCriticalSection(&s_cs); double denom = (double)(s_stat.in_packets + s_stat.missing_packets); double loss = denom > 0 ? 100.0 * (double)s_stat.missing_packets / denom : 0.0;
    { const sess* p = sess_primary(); const float rtt = p ? p->rtt : 0.0f, jit = p ? p->jitter : 0.0f;
      int np = 0; for (int i = 0; i < MAXS; i++) if (s_S[i].used && s_S[i].connected) np++;
      snprintf(out, n, "%.1f pkt/s out (%.1f KB/s), %.1f in | loss %.1f%% | RTT %.0f ms | jitter %.1f ms", s_stat.out_pps, s_out_kbps, s_stat.in_pps, loss, rtt, jit);
      if (s_maxpeers > 2) { const size_t l = strlen(out); snprintf(out + l, n - l, " | sessions %d/%d", np, sess_limit()); } }
    LeaveCriticalSection(&s_cs);
}

/* ------------------------------------------------------------------ packet/session layer (no socket calls below this point) */
/* v1071B shared view: the sender's CAMERA direction rides in state_body.pad (16-bit heading of the forward vector the game's own view
 * test func_00283DA4 caches at *(TOC+0x980) x / *(TOC+0x984) z), flags bit 1 = valid. Both fields were always 0, so an older peer
 * just ignores them. GF_COOP_SHARED_VIEW=0 = not sent. */
static int s_cam_tx = -1;
static void cam_stamp(state_body* st)
{
    if (s_cam_tx < 0) { const char* e = getenv("GF_COOP_SHARED_VIEW"); s_cam_tx = !(e && *e == '0'); }
    if (!s_cam_tx || !st->valid) return;
    __try {
        const uint32_t toc = 0x00B4BF60u, px = g_rd32(toc + 0x980u), pz = g_rd32(toc + 0x984u);
        float fx, fz, l;
        if (!g_ok(px) || !g_ok(pz)) return;
        fx = g_rdf(px); fz = g_rdf(pz); l = fx * fx + fz * fz;
        if (!(l > 0.5f && l < 1.5f)) return;
        { const int h = (int)lrintf(atan2f(fx, fz) * (32768.0f / 3.14159265f)); st->pad = (uint16_t)(h & 0xFFFF); st->flags |= 1; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static void cam_note(volatile LONG* h, volatile LONG* ok, volatile ULONGLONG* ms, const state_body* st, ULONGLONG now) { if (st->flags & 1) { *h = st->pad; *ms = now; *ok = 1; } }
static int cam_get(LONG h, LONG ok, ULONGLONG ms, float* fx, float* fz)
{
    const float a = (float)(int16_t)(uint16_t)h * (3.14159265f / 32768.0f);
    if (!ok || GetTickCount64() - ms > 1000) return 0;
    *fx = sinf(a); *fz = cosf(a); return 1;
}
int gf_coop_local_cam(float* fx, float* fz)   /* v1071D: OUR camera forward (x, z) from the game's view-test cache, 1 = valid */
{
    __try {
        const uint32_t toc = 0x00B4BF60u, px = g_rd32(toc + 0x980u), pz = g_rd32(toc + 0x984u);
        float x, z, l;
        if (!g_ok(px) || !g_ok(pz)) return 0;
        x = g_rdf(px); z = g_rdf(pz); l = x * x + z * z;
        if (!(l > 0.5f && l < 1.5f)) return 0;
        l = sqrtf(l); *fx = x / l; *fz = z / l; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
int gf_coop_partner_cam(float* fx, float* fz)   /* v1071B: partner camera forward (x, z), 1 = fresh (< 1 s) */
{
    const sess* p = s_cs_init ? sess_primary() : NULL;
    return p ? cam_get(p->cam_h, p->cam_ok, p->cam_ms, fx, fz) : 0;
}
static int read_state(state_body* st)
{
    memset(st, 0, sizeof *st); const uint32_t pl = g_player(); if (!pl) return 0;
    __try { const uint32_t xf = g_rd32(pl + 0x68); if (!g_ok(xf)) return 1; for (int i = 0; i < 16; i++) st->m[i] = g_rdf(xf + 0x10 + 4u*(uint32_t)i);
        st->health = g_rdf(pl + 0x14C8); st->health_max = g_rdf(pl + 0x14CC); for (int i = 12; i < 15; i++) if (!(st->m[i] == st->m[i]) || fabsf(st->m[i]) > 1e6f) return 1; st->valid = 1;
        if (fabsf(st->m[12]) + fabsf(st->m[13]) + fabsf(st->m[14]) < 0.01f) st->valid = 0;   /* v1059A: exact origin = player not placed yet (loading) */
    } __except (EXCEPTION_EXECUTE_HANDLER) { st->valid = 0; } return 1;
}
int gf_coop_local_pos(float out[3])   /* v1060W2: local player world position (main or co-op thread) */
{
    state_body st; if (!read_state(&st) || !st.valid) return 0; out[0] = st.m[12]; out[1] = st.m[13]; out[2] = st.m[14]; return 1;
}
/* v1059A motion in the RwMatrix padding words (never written to the game: puppet write_matrix keeps its own padding):
 * m[3], m[7], m[11] = world velocity m/s (send-side finite difference over >= 30 ms), m[15] = horizontal speed m/s. */
static void add_motion(state_body* st, ULONGLONG now)
{
    static float pp[3]; static ULONGLONG pt; static float v[3];
    if (!st->valid) { pt = 0; st->m[3] = st->m[7] = st->m[11] = st->m[15] = 0.0f; return; }
    if (pt && now > pt + 30 && now - pt < 500) { const float dt = (float)(now - pt) * 0.001f;
        for (int i = 0; i < 3; i++) { float nv = (st->m[12 + i] - pp[i]) / dt; if (!(nv == nv) || fabsf(nv) > 60.0f) nv = 0.0f; v[i] += (nv - v[i]) * 0.5f; } }
    if (!pt || now > pt + 30) { pp[0] = st->m[12]; pp[1] = st->m[13]; pp[2] = st->m[14]; pt = now; }
    st->m[3] = v[0]; st->m[7] = v[1]; st->m[11] = v[2]; st->m[15] = sqrtf(v[0]*v[0] + v[2]*v[2]);
}
static void echo_offset(state_body* st) { st->m[12] += st->m[0]*s_echo_ofs; st->m[13] += st->m[1]*s_echo_ofs; st->m[14] += st->m[2]*s_echo_ofs; }
static void send_hello(sess* p, int type)
{
    if (s_harden) { pk_hello3 q; memset(&q, 0, sizeof q); fill_hdr2(p, &q.b.h, type, 0); q.b.build = COOP_BUILD_ID; memcpy(q.b.name, s_name, sizeof q.b.name);
        q.caps = CAP_NPEER; q.maxpeers = (uint8_t)s_maxpeers; q.your_id = (uint8_t)(type == PK_WELCOME ? p->id : 0);
        if (s_mode == MODE_JOIN && type == PK_HELLO) { if (!s_join_epoch) s_join_epoch = (uint16_t)(((uint32_t)GetTickCount64() ^ (GetCurrentProcessId() * 40503u)) | 1u); memcpy(q.pad, &s_join_epoch, 2); }   /* v1126C */
        wire_send(p, &q, sizeof q); }   /* v1072S: +8 B, older exes read only sizeof(pk_hello2) */
    else { pk_hello1 q; memset(&q, 0, sizeof q); fill_hdr1(&q.h, type, COOP_V1); memcpy(q.name, s_name, sizeof q.name); wire_send(p, &q, sizeof q); }
}
static void send_reject(sess* p, const net_addr* to, uint16_t target_version, int reason, uint64_t got_build, const char* text)
{
    if (target_version == COOP_V2) { pk_reject2 q; memset(&q, 0, sizeof q); fill_hdr2(p, &q.h, PK_REJECT, 0); q.reason = (uint16_t)reason; q.expected_version = s_harden ? COOP_V2 : COOP_V1; q.expected_build = COOP_BUILD_ID; q.got_build = got_build; snprintf(q.text, sizeof q.text, "%s", text); wire_send_to(to, &q, sizeof q); }
    else { pk_reject1 q; memset(&q, 0, sizeof q); fill_hdr1(&q.h, PK_REJECT, target_version); q.reason = (uint16_t)reason; q.expected_version = s_harden ? COOP_V2 : COOP_V1; q.expected_build = COOP_BUILD_ID; q.got_build = got_build; snprintf(q.text, sizeof q.text, "%s", text); wire_send_to(to, &q, sizeof q); }
}
static void send_control(sess* p, int type)
{
    if (s_harden) { pk_hdr2 q; fill_hdr2(p, &q, type, 0); wire_send(p, &q, sizeof q); } else { pk_hdr1 q; fill_hdr1(&q, type, COOP_V1); wire_send(p, &q, sizeof q); }
}
static void send_state(sess* p, const state_body* st)
{
    if (s_harden) { pk_state2 q; fill_hdr2(p, &q.h, PK_STATE, 0); q.s = *st; wire_send(p, &q, sizeof q); }
    else { pk_state1 q; fill_hdr1(&q.h, PK_STATE, COOP_V1); q.s = *st; wire_send(p, &q, sizeof q); }
}
static void send_ping(sess* p) { pk_ping q; fill_hdr2(p, &q.h, PK_PING, 0); q.t0 = q.h.v1.ms; wire_send(p, &q, sizeof q); }
static void send_pong(sess* p, uint32_t t0, uint32_t t1) { pk_pong q; fill_hdr2(p, &q.h, PK_PONG, 0); q.t0 = t0; q.t1 = t1; wire_send(p, &q, sizeof q); }
static int send_blob_impl(sess* p, uint8_t channel, const void* data, int len)
{
    pk_blob q; fill_hdr2(p, &q.h, PK_BLOB, 0); q.channel = channel; q.pad = 0; q.len = (uint16_t)len; memcpy(q.data, data, (size_t)len);
    return wire_send(p, &q, (int)(sizeof q - sizeof q.data) + len);
}
/* v1058D identity: read on the co-op thread (guarded), sent reliably on connect and whenever it changes */
static int read_identity(uint8_t out[32])
{
    const uint32_t pl = g_player(); if (!pl) return 0;
    __try { memcpy(out, vm_base + pl + 0x2244, 16); memcpy(out + 16, vm_base + pl + 0x2254, 16); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    for (int i = 0; i < 32; i++) if (out[i]) return 1;
    return 0;   /* all zero = not set yet */
}
static void identity_tick(sess* p)
{
    uint8_t id[32]; int ok; if (!read_identity(id)) return;
    if (p->ident_sent && !memcmp(id, p->ident_sent_val, 32)) return;
    EnterCriticalSection(&s_cs); ok = sess_queue_event(p, EV_IDENTITY, id, 32); LeaveCriticalSection(&s_cs);
    if (ok) { memcpy(p->ident_sent_val, id, 32); memcpy(s_my_ident, id, 32); p->ident_sent = 1;
        printf("[coop] sent our character identity (skeleton %08X.., mesh %08X..)%s\n", *(const uint32_t*)id, *(const uint32_t*)(id + 16), s_maxpeers > 2 ? " [per session]" : ""); fflush(stdout); }
}

static void deliver_event(sess* p, uint16_t type, const uint8_t* data, int len)
{
    gf_coop_event_fn fns[16]; gf_coop_peer_event_fn nf[8]; const int prim = p == sess_primary();
    EnterCriticalSection(&s_cs); memcpy(fns, s_event_handlers, sizeof fns); memcpy(nf, s_nev, sizeof nf); LeaveCriticalSection(&s_cs);
    t_cur_peer = p->id;
    if (prim) for (int k = 0; k < 16; k++) if (fns[k]) fns[k](type, data, len);   /* v1060W2 all legacy handlers: primary peer only (v1072S) */
    for (int k = 0; k < 8; k++) if (nf[k]) nf[k](p->id, type, data, len);
    t_cur_peer = -1;
}
static void deliver_peer_event(int origin, uint16_t type, const uint8_t* data, int len)   /* all-peer handlers only */
{
    gf_coop_peer_event_fn nf[8]; EnterCriticalSection(&s_cs); memcpy(nf, s_nev, sizeof nf); LeaveCriticalSection(&s_cs);
    t_cur_peer = origin; for (int k = 0; k < 8; k++) if (nf[k]) nf[k](origin, type, data, len); t_cur_peer = -1;
}
static void deliver_peer_blob(int origin, uint8_t ch, const uint8_t* data, int len)
{
    gf_coop_peer_blob_fn nf[8]; EnterCriticalSection(&s_cs); memcpy(nf, s_nblob, sizeof nf); LeaveCriticalSection(&s_cs);
    t_cur_peer = origin; for (int k = 0; k < 8; k++) if (nf[k]) nf[k](origin, ch, data, len); t_cur_peer = -1;
}
static void relay_event_fwd(const sess* from, int origin, uint16_t type, const uint8_t* data, int len)   /* host: -> every other CAP joiner */
{
    uint8_t env[1000];
    if (len < 0 || len > 997) return;
    env[0] = (uint8_t)origin; memcpy(env + 1, &type, 2); if (len) memcpy(env + 3, data, (size_t)len);
    EnterCriticalSection(&s_cs);
    for (int i = 0; i < MAXS; i++) { sess* q = &s_S[i]; if (!q->used || !q->connected || q == from || !(q->caps & CAP_NPEER)) continue;
        if (sess_queue_event(q, EV_RELAYED, env, len + 3)) st_rev_fwd++; else { q->drops++; st_bcast_drop++; } }
    LeaveCriticalSection(&s_cs);
}
static void rel_deliver(sess* p)
{
    for (;;) { rel_rx* found = NULL; for (int i = 0; i < 32; i++) if (p->rx[i].used && p->rx[i].seq == p->ev_rx_next) { found = &p->rx[i]; break; } if (!found) break;
        uint16_t type = found->type, len = found->len; uint8_t data[1000]; if (len) memcpy(data, found->data, len); found->used = 0; p->ev_rx_next++;
        if (s_mode == MODE_ECHO && type == 0xFFFFu && len == 4 && !memcmp(data, "TEST", 4)) { printf("[coop] reliable event echo test OK\n"); fflush(stdout); }
        else if (type == EV_IDENTITY) { if (len == 32) { EnterCriticalSection(&s_cs); memcpy(p->ident, data, 32); LeaveCriticalSection(&s_cs); p->ident_ok = 1;
                printf("[coop] partner identity received (skeleton %08X.., mesh %08X..)%s\n", *(const uint32_t*)data, *(const uint32_t*)(data + 16), s_maxpeers > 2 ? " [per session]" : ""); fflush(stdout); } }
        else if (type == EV_RELAY && len >= 2) {   /* v1072S: a joiner asks the host to relay (host: deliver here + forward; echo: deliver) */
            uint16_t t2; memcpy(&t2, data, 2); st_rev_rx++;
            if (s_mode == MODE_ECHO && t2 == 0xFFFEu && len == 6 && !memcmp(data + 2, "RLAY", 4)) { printf("[coop] v1072S relay event echo test OK\n"); fflush(stdout); }
            else if (t2 >= 0x100) { deliver_peer_event(p->id, t2, data + 2, len - 2); if (s_mode == MODE_HOST) relay_event_fwd(p, p->id, t2, data + 2, len - 2); } }
        else if (type == EV_RELAYED && len >= 3) {   /* host -> us: another peer's relayed event */
            uint16_t t2; memcpy(&t2, data + 1, 2); st_rev_rx++;
            if (t2 >= 0x100 && data[0] != s_self_id) deliver_peer_event(data[0], t2, data + 3, len - 3); }
        else if (type >= 0x100) deliver_event(p, type, data, len);
    }
}
static void rel_receive(sess* p, const pk_event* q, int n)
{
    if (q->len > 1000 || n < (int)(sizeof(pk_event) - 1000 + q->len)) return;
    if ((int32_t)(q->event_seq - p->ev_rx_next) < 0 || q->event_seq - p->ev_rx_next >= 32) return;
    for (int i = 0; i < 32; i++) if (p->rx[i].used && p->rx[i].seq == q->event_seq) return;
    for (int i = 0; i < 32; i++) if (!p->rx[i].used) { p->rx[i].used = 1; p->rx[i].seq = q->event_seq; p->rx[i].type = q->event_type; p->rx[i].len = q->len; if (q->len) memcpy(p->rx[i].data, q->data, q->len); break; }
    rel_deliver(p);
}
static void rel_pump(sess* p, ULONGLONG now)
{
    float rtt; EnterCriticalSection(&s_cs); rtt = p->rtt; LeaveCriticalSection(&s_cs);
    ULONGLONG retry = (ULONGLONG)(1.5f * (rtt > 20.0f ? rtt : 100.0f)); if (retry < 75) retry = 75; if (retry > 1000) retry = 1000;
    EnterCriticalSection(&s_cs);
    for (int i = 0; i < 32; i++) { rel_tx* q = &p->tx[i]; if (!q->used || (q->sends && now - q->sent < retry)) continue;
        if (q->sends) q->packet_seq = next_packet_seq(p); /* keep a retry inside the peer's rolling 32-packet ack window */
        pk_event e; memset(&e, 0, sizeof e); fill_hdr2(p, &e.h, PK_EVENT, q->packet_seq); e.event_seq = q->event_seq; e.event_type = q->type; e.len = q->len; if (q->len) memcpy(e.data, q->data, q->len);
        if (wire_send(p, &e, (int)(sizeof e - sizeof e.data + q->len))) { if (q->sends) s_stat.retransmits++; q->sent = now; q->sends++; }
    }
    LeaveCriticalSection(&s_cs);
}

/* ------------------------------------------------------------------ v1072S relay (host) / relayed peers (joiner) */
static void ppose_store(int id, const uint8_t* d, int len, uint32_t ms)   /* latest two player-pose packets of peer id */
{
    if (id < 0 || id >= GF_COOP_MAX_PEERS || len <= 0 || len > 1100) return;
    EnterCriticalSection(&s_cs);
    { ppose* q = &s_PP[id]; memcpy(q->d[0], q->d[1], sizeof q->d[0]); q->l[0] = q->l[1]; q->ms[0] = q->ms[1];
      memcpy(q->d[1], d, (size_t)len); q->l[1] = len; q->ms[1] = ms; if (q->n < 2) q->n++; q->rx = GetTickCount64(); }
    LeaveCriticalSection(&s_cs);
}
static rpeer* rpeer_get(int id, int create)   /* caller holds s_cs */
{
    if (id < 0 || id >= GF_COOP_MAX_PEERS) return NULL;
    if (s_R[id].used) return &s_R[id];
    if (!create) return NULL;
    memset(&s_R[id], 0, sizeof s_R[id]); s_R[id].used = 1; s_R[id].id = id; return &s_R[id];
}
static void rpeer_state(int id, const char* name, const state_body* st, ULONGLONG now, uint32_t sender_ms)
{
    int fresh_peer = 0;
    EnterCriticalSection(&s_cs);
    { rpeer* r = rpeer_get(id, 1);
      if (r) { if (!r->last_rx || (LONGLONG)(now - r->last_rx) > (LONGLONG)RSTALE_MS) { fresh_peer = 1; r->sr.n = r->sr.head = 0; r->have_transit = 0; }
          memcpy(r->name, name, sizeof r->name); r->name[23] = 0; ring_push(&r->sr, st, now); transit_note(&r->last_transit, &r->have_transit, &r->jitter, now, sender_ms);
          cam_note(&r->cam_h, &r->cam_ok, &r->cam_ms, st, now); r->last_rx = now; } }
    LeaveCriticalSection(&s_cs);
    if (fresh_peer) { printf("[coop] v1072S peer %d (%s) is in the session (relayed by the host)\n", id, name); fflush(stdout); }
}
/* v1072S relay tiers (GF_COOP_RELAY_TIERS=0 off): per receiver, by distance between the two players' newest states:
 * < 60 m every state + every pose, 60-200 m every 3rd state + every 3rd pose, > 200 m every 15th state (2 Hz), no pose. */
static int s_tiers = -1; static uint8_t s_rctr[MAXS][MAXS][2]; static volatile long st_tier_skip[2];
static float sess_dist2(const sess* a, const sess* b)   /* caller: co-op thread; reads the newest snapshot of both */
{
    const snap *x, *y;
    if (!a->sr.n || !b->sr.n) return 0.0f;
    x = &a->sr.s[(a->sr.head - 1 + NSNAP) % NSNAP]; y = &b->sr.s[(b->sr.head - 1 + NSNAP) % NSNAP];
    if (!x->valid || !y->valid) return 0.0f;
    { const float dx = x->m[12] - y->m[12], dz = x->m[14] - y->m[14]; return dx * dx + dz * dz; }
}
static int relay_due(sess* from, sess* to, int kind)   /* kind 0 state, 1 pose */
{
    float d2; int every; uint8_t* c;
    if (s_tiers < 0) { const char* e = getenv("GF_COOP_RELAY_TIERS"); s_tiers = !(e && *e == '0'); printf("[coop] v1072S relay tiers by distance %s (GF_COOP_RELAY_TIERS)\n", s_tiers ? "ON" : "off"); fflush(stdout); }
    if (!s_tiers) return 1;
    d2 = sess_dist2(from, to);
    every = d2 < 22500.0f ? 1 : d2 < 90000.0f ? 3 : (kind ? 0 : 15);   /* v1072Z: full rate < 150 m (was 60), 1/3 to 300 m (was 200) */
    if (!every) { st_tier_skip[kind]++; return 0; }
    c = &s_rctr[from - s_S][to - s_S][kind];
    if (++*c >= every) { *c = 0; return 1; }
    st_tier_skip[kind]++; return 0;
}
static void relay_state(sess* from, const state_body* st)   /* host co-op thread: joiner state -> the other CAP_NPEER joiners */
{
    pk_pstate q; int any = 0;
    if (s_mode != MODE_HOST || s_maxpeers <= 2) return;
    for (int i = 0; i < MAXS; i++) { sess* p = &s_S[i]; if (!p->used || !p->connected || p == from || !(p->caps & CAP_NPEER)) continue;
        if (!relay_due(from, p, 0)) continue;
        memset(&q, 0, sizeof q); fill_hdr2(p, &q.h, PK_PSTATE, 0); q.origin = (uint8_t)from->id; memcpy(q.name, from->name, sizeof q.name); q.s = *st;
        if (wire_send(p, &q, sizeof q)) any = 1; }
    if (any) InterlockedIncrement(&st_relay_state);
}
static void relay_pose(sess* from, uint8_t ch, const uint8_t* d, int len)
{
    pk_pblob q; int any = 0;
    if (s_mode != MODE_HOST || s_maxpeers <= 2 || len <= 0 || len > 1150) return;
    for (int i = 0; i < MAXS; i++) { sess* p = &s_S[i]; if (!p->used || !p->connected || p == from || !(p->caps & CAP_NPEER)) continue;
        if (!relay_due(from, p, 1)) continue;
        fill_hdr2(p, &q.h, PK_PBLOB, 0); q.origin = (uint8_t)from->id; q.channel = ch; q.len = (uint16_t)len; memcpy(q.data, d, (size_t)len);
        if (wire_send(p, &q, (int)(sizeof q - sizeof q.data) + len)) any = 1; }
    if (any) InterlockedIncrement(&st_relay_pose);
}
static void relay_blob_fwd(sess* from, uint8_t ch, const uint8_t* d, int len)   /* host: relay blob (channel 0x80 | x) -> other CAP joiners, no tiers */
{
    pk_pblob q;
    if (len <= 0 || len > 1150) return;
    for (int i = 0; i < MAXS; i++) { sess* p = &s_S[i]; if (!p->used || !p->connected || p == from || !(p->caps & CAP_NPEER)) continue;
        fill_hdr2(p, &q.h, PK_PBLOB, 0); q.origin = (uint8_t)from->id; q.channel = ch; q.len = (uint16_t)len; memcpy(q.data, d, (size_t)len);
        if (wire_send(p, &q, (int)(sizeof q - sizeof q.data) + len)) st_rblob_fwd++; }
}
/* echo test aid GF_COOP_ECHO_PEERS=k: k fake relayed peers (ids 2..k+1) that copy our own state k*3 m to the side and our pose */
static void echo_fake_state(const state_body* st0, ULONGLONG now)
{
    for (int k = 0; k < s_echo_peers; k++) { state_body st = *st0; char nm[24]; const float o = s_echo_ofs + 3.0f * (float)(k + 1);
        st.m[12] += st.m[0]*o; st.m[13] += st.m[1]*o; st.m[14] += st.m[2]*o; snprintf(nm, sizeof nm, "echo%d", k + 2);
        rpeer_state(k + 2, nm, &st, now, (uint32_t)now); }
}
static void echo_fake_pose(uint8_t ch, const void* data, int len)
{
    if (ch != 1 /* GF_COOP_POSE_BLOB_CHANNEL */) return;
    for (int k = 0; k < s_echo_peers; k++) ppose_store(k + 2, (const uint8_t*)data, len, (uint32_t)GetTickCount64());
}

static void on_v1_packet(sess* p, const uint8_t* b, int n, const net_addr* from, const char* who)
{
    const pk_hdr1* h = (const pk_hdr1*)b; int known = s_mode == MODE_ECHO || (p && p->used && net_same(&p->addr, from));
    if (h->type == PK_REJECT && n >= (int)sizeof(pk_reject1)) { const pk_reject1* r = (const pk_reject1*)b; printf("[coop] connection refused by %s: %s (they require protocol v%u, build %016llX)\n", who, r->text, r->expected_version, (unsigned long long)r->expected_build); s_refused = 1; coop_note("Refused by the host: %s", r->text); return; }
    if (h->type == PK_HELLO && s_mode == MODE_HOST && n >= (int)sizeof(pk_hello1)) { p = &s_S[0];   /* v1 protocol: one joiner only */
        if (p->connected && !net_same(&p->addr, from)) { pk_hdr1 f; fill_hdr1(&f, PK_FULL, COOP_V1); wire_send_to(from, &f, sizeof f); return; }
        p->used = 1; p->addr = *from; p->id = 1; memcpy(p->name, ((const pk_hello1*)b)->name, sizeof p->name); p->name[23] = 0; if (!p->connected) { printf("[coop] %s joined from %s\n", p->name, who); reset_session(p); } p->connected = 1; p->last_rx = GetTickCount64(); send_hello(p, PK_WELCOME); return; }
    if (!known || !p) return; p->last_rx = GetTickCount64();
    switch (h->type) {
    case PK_WELCOME: if (n >= (int)sizeof(pk_hello1)) { memcpy(p->name, ((const pk_hello1*)b)->name, sizeof p->name); p->name[23] = 0; } if (!p->connected) { reset_session(p); printf("[coop] connected to %s (%s)\n", p->name, who); } p->connected = 1; p->id = 0; break;
    case PK_FULL: printf("[coop] %s: session is full\n", who); s_refused = 1; coop_note("Refused: the host's game is full"); break;
    case PK_BYE: sess_free(p, "left"); break;
    case PK_STATE: if (n >= (int)sizeof(pk_state1) && p->connected && (int32_t)(h->seq - p->v1_rx) > 0) { state_body st = ((const pk_state1*)b)->s; p->v1_rx = h->seq; if (s_mode == MODE_ECHO) echo_offset(&st); snap_push(p, &st, GetTickCount64(), h->ms); cam_note(&p->cam_h, &p->cam_ok, &p->cam_ms, &st, GetTickCount64()); } break;
    }
}
static void host_hello_v2(const uint8_t* b, int n, const net_addr* from, const char* who, ULONGLONG now)
{
    const pk_hello2* q = (const pk_hello2*)b; sess* p = sess_by_addr(from); int used = 0; uint16_t ep = 0;
    if (n >= (int)sizeof(pk_hello3)) memcpy(&ep, ((const pk_hello3*)b)->pad, 2);
    if (q->build != COOP_BUILD_ID) { printf("[coop] refusing %s: build mismatch, peer %016llX local %016llX\n", who, (unsigned long long)q->build, (unsigned long long)COOP_BUILD_ID); send_reject(&s_S[0], from, COOP_V2, REJECT_BUILD, q->build, "build id mismatch; update both games"); return; }
    if (s_rj_until > now && net_same(&s_rj_addr, from)) return;   /* v1126C re-join hold */
    if (p && p->connected && ep && p->epoch && ep != p->epoch) {   /* v1126C the joiner restarted its session */
        InterlockedIncrement(&st_rejoins); printf("[coop] v1126C %s says HELLO with a new epoch (%u -> %u): its session restarted -> dropping ours, accepting it fresh in 1.5 s\n", p->name, p->epoch, ep); fflush(stdout);
        sess_free(p, "re-joined (session restarted)"); s_rj_addr = *from; s_rj_until = now + 1500; return; }
    if (!p) {
        for (int i = 0; i < MAXS; i++) if (s_S[i].used) used++;
        if (used >= sess_limit()) { pk_hdr2 f; sess tmp; memset(&tmp, 0, sizeof tmp); fill_hdr2(&tmp, &f, PK_FULL, 0); wire_send_to(from, &f, sizeof f); InterlockedIncrement(&st_full);
            { static int nl; if (nl++ < 8) { printf("[coop] v1072S %s refused: session full (%d/%d joiners, GF_COOP_MAXPEERS=%d)\n", who, used, sess_limit(), s_maxpeers); fflush(stdout); } } return; }
        for (int i = 0; i < MAXS; i++) if (!s_S[i].used) { p = &s_S[i]; memset(p, 0, sizeof *p); p->used = 1; p->addr = *from; p->id = i + 1; break; }
        if (!p) return;
    }
    memcpy(p->name, q->name, sizeof p->name); p->name[23] = 0;
    p->caps = n >= (int)sizeof(pk_hello3) ? ((const pk_hello3*)b)->caps : 0;
    if (!p->connected) { reset_session(p); p->since = now; p->epoch = ep; InterlockedIncrement(&s_sess_gen);
        printf("[coop] %s joined from %s (build %016llX)", p->name, who, (unsigned long long)q->build);
        if (s_maxpeers > 2) printf(" -> peer %d, %s", p->id, (p->caps & CAP_NPEER) ? "N-peer capable" : "OLD exe: sees the host only");
        printf("\n"); fflush(stdout); }
    p->connected = 1; p->last_rx = now; send_hello(p, PK_WELCOME);
}
static void on_v2_packet(sess* p, const uint8_t* b, int n, const net_addr* from, const char* who)
{
    if (n < (int)sizeof(pk_hdr2)) return; const pk_hdr2* h = (const pk_hdr2*)b; ULONGLONG now = GetTickCount64(); int fresh;
    if (h->v1.type == PK_REJECT && n >= (int)sizeof(pk_reject2)) { const pk_reject2* r = (const pk_reject2*)b; printf("[coop] connection refused by %s: %s (they require protocol v%u, build %016llX)\n", who, r->text, r->expected_version, (unsigned long long)r->expected_build); s_refused = 1; coop_note("Refused by the host: %s", r->text); return; }
    if (h->v1.type == PK_HELLO && s_mode == MODE_HOST && n >= (int)sizeof(pk_hello2)) { host_hello_v2(b, n, from, who, now); return; }
    if (!p) return;
    apply_acks(p, h->ack, h->ack_bits, now); fresh = note_packet(p, h->v1.seq); p->last_rx = now;
    switch (h->v1.type) {
    case PK_WELCOME: if (n >= (int)sizeof(pk_hello2)) { const pk_hello2* q = (const pk_hello2*)b; if (q->build != COOP_BUILD_ID) { printf("[coop] refusing host %s: build mismatch, peer %016llX local %016llX\n", who, (unsigned long long)q->build, (unsigned long long)COOP_BUILD_ID); send_reject(p, from, COOP_V2, REJECT_BUILD, q->build, "build id mismatch; update both games"); s_refused = 1; break; }
            memcpy(p->name, q->name, sizeof p->name); p->name[23] = 0; p->id = 0;
            if (n >= (int)sizeof(pk_hello3)) { const pk_hello3* x = (const pk_hello3*)b; p->caps = x->caps; if (x->your_id > 0 && x->your_id < GF_COOP_MAX_PEERS) s_self_id = x->your_id; if (x->maxpeers >= 2) s_net_maxpeers = x->maxpeers; }
            if (!p->connected) { reset_session(p); p->since = now; InterlockedIncrement(&s_sess_gen); printf("[coop] connected to %s (%s, build %016llX)", p->name, who, (unsigned long long)q->build);
                if (p->caps & CAP_NPEER) printf(" -> we are peer %d of up to %d", s_self_id, n >= (int)sizeof(pk_hello3) ? ((const pk_hello3*)b)->maxpeers : 2);
                printf("\n"); fflush(stdout); }
            p->connected = 1; } break;
    case PK_FULL: printf("[coop] %s: session is full\n", who); s_refused = 1; coop_note("Refused: the host's game is full"); break;
    case PK_BYE: sess_free(p, "left"); break;
    case PK_STATE: if (fresh && n >= (int)sizeof(pk_state2) && p->connected) { state_body st = ((const pk_state2*)b)->s; if (s_mode == MODE_ECHO) echo_offset(&st); snap_push(p, &st, now, h->v1.ms); cam_note(&p->cam_h, &p->cam_ok, &p->cam_ms, &st, now);
            relay_state(p, &((const pk_state2*)b)->s); } break;
    case PK_EVENT: if (p->connected) rel_receive(p, (const pk_event*)b, n); break;
    case PK_BLOB: if (fresh && p->connected && n >= (int)(sizeof(pk_blob) - 1150)) { const pk_blob* q = (const pk_blob*)b; if (q->len <= 1150 && n >= (int)(sizeof(pk_blob) - 1150) + q->len) {
            gf_coop_blob_fn fn; gf_coop_peer_blob_fn nf[8]; const int prim = p == sess_primary();
            EnterCriticalSection(&s_cs); fn = s_blob_handler; memcpy(nf, s_nblob, sizeof nf); LeaveCriticalSection(&s_cs);
            if (q->channel == 1 /* GF_COOP_POSE_BLOB_CHANNEL */) { if (q->len <= 1100) ppose_store(p->id, q->data, q->len, h->v1.ms); relay_pose(p, q->channel, q->data, q->len); }
            if (q->channel & 0x80) {   /* v1072S relay blob: all-peer handlers only (+ host forwards to the other joiners) */
                st_rblob_rx++; deliver_peer_blob(p->id, q->channel, q->data, q->len);
                if (s_mode == MODE_HOST) relay_blob_fwd(p, q->channel, q->data, q->len);
                break; }
            t_cur_peer = p->id;
            if (fn && prim) fn(q->channel, q->data, q->len);   /* v1072S: legacy blob handler = primary peer only */
            for (int k = 0; k < 8; k++) if (nf[k]) nf[k](p->id, q->channel, q->data, q->len);
            t_cur_peer = -1; } } break;
    case PK_PSTATE: if (fresh && p->connected && s_mode != MODE_HOST && n >= (int)sizeof(pk_pstate)) { const pk_pstate* q = (const pk_pstate*)b;
            if (q->origin != s_self_id && q->origin > 0 && q->origin < GF_COOP_MAX_PEERS) { InterlockedIncrement(&st_rx_pstate); rpeer_state(q->origin, q->name, &q->s, now, h->v1.ms); } } break;
    case PK_PBLOB: if (fresh && p->connected && s_mode != MODE_HOST && n >= (int)(sizeof(pk_pblob) - 1150)) { const pk_pblob* q = (const pk_pblob*)b;
            if (q->len <= 1150 && n >= (int)(sizeof(pk_pblob) - 1150) + q->len && q->origin != s_self_id) {
                if (q->channel == 1 && q->len <= 1100) { InterlockedIncrement(&st_rx_pblob); ppose_store(q->origin, q->data, q->len, h->v1.ms); }
                else if (q->channel & 0x80) { st_rblob_rx++; deliver_peer_blob(q->origin, q->channel, q->data, q->len); } } } break;
    case PK_PING: if (n >= (int)sizeof(pk_ping)) send_pong(p, ((const pk_ping*)b)->t0, (uint32_t)now); break;
    case PK_PONG: if (n >= (int)sizeof(pk_pong)) { const pk_pong* q = (const pk_pong*)b; uint32_t t3 = (uint32_t)now, t2 = h->v1.ms; int32_t r = (int32_t)(t3 - q->t0) - (int32_t)(t2 - q->t1); if (r >= 0) update_rtt(p, (float)r); EnterCriticalSection(&s_cs); p->clock_off = 0.875f*p->clock_off + 0.125f*0.5f*((float)(int32_t)(q->t1-q->t0) + (float)(int32_t)(t2-t3)); LeaveCriticalSection(&s_cs); } break;
    }
}
static void on_packet(const uint8_t* b, int n, const net_addr* from)
{
    if (n < (int)sizeof(pk_hdr1)) return; const pk_hdr1* h = (const pk_hdr1*)b; if (h->magic != COOP_MAGIC) return; char who[64]; if (s_mode == MODE_ECHO) snprintf(who, sizeof who, "echo"); else net_describe(from, who, sizeof who);
    uint16_t expected = s_harden ? COOP_V2 : COOP_V1;
    sess* p = s_mode == MODE_ECHO ? &s_S[0] : s_mode == MODE_JOIN ? (net_same(&s_S[0].addr, from) ? &s_S[0] : NULL) : sess_by_addr(from);
    if (h->version != expected) { printf("[coop] refusing %s: protocol mismatch, peer v%u local v%u (set GF_COOP_HARDEN the same on both games)\n", who, h->version, expected); send_reject(&s_S[0], from, h->version, REJECT_PROTOCOL, 0, "protocol mismatch; GF_COOP_HARDEN must match"); return; }
    if (s_harden) on_v2_packet(p, b, n, from, who); else on_v1_packet(p, b, n, from, who);
}

static void stats_tick(ULONGLONG now)
{
    EnterCriticalSection(&s_cs); if (!s_stat.window_ms) s_stat.window_ms = now; ULONGLONG dt = now - s_stat.window_ms;
    if (dt >= 10000) { s_stat.out_pps = (float)(s_stat.out_packets - s_stat.window_out) * 1000.0f / (float)dt; s_stat.in_pps = (float)(s_stat.in_packets - s_stat.window_in) * 1000.0f / (float)dt; s_stat.window_out = s_stat.out_packets; s_stat.window_in = s_stat.in_packets; s_stat.window_ms = now; { const LONG64 b = s_bytes_out; s_out_kbps = (float)(b - s_bytes_win) / 1.024f / (float)dt; s_bytes_win = b; } }
    LeaveCriticalSection(&s_cs);
}
static void peers_log(void);
static DWORD WINAPI coop_thread(LPVOID arg)
{
    (void)arg; ULONGLONG next = GetTickCount64(), last_hello = 0, last_log = 0; int first_state = 1, echo_probe_sent = 0; uint8_t buf[1500];
    for (;;) { ULONGLONG now = GetTickCount64(); net_addr from; int n;
        {   /* v1119M: host / join / leave requested by the pause-menu CO-OP tab (render thread) -- carried out here, the only
             * thread that touches the socket */
            const LONG c = InterlockedExchange(&s_cmd, 0);
            if (c == CMD_LEAVE && s_mode != MODE_OFF) { coop_stop(); first_state = 1; echo_probe_sent = 0; last_hello = 0; }
            else if ((c == CMD_HOST || c == CMD_JOIN) && s_mode == MODE_OFF) {
                char a[128]; EnterCriticalSection(&s_cs); snprintf(a, sizeof a, "%s", s_cmd_addr); LeaveCriticalSection(&s_cs);
                if (coop_start(c == CMD_HOST ? MODE_HOST : MODE_JOIN, a)) { first_state = 1; echo_probe_sent = 0; last_hello = 0; next = now; } }
            if (c != CMD_NONE) InterlockedExchange(&s_busy, 0); }
        {   /* v1119M GF_COOP_SELFTEST="host@30,leave@60,join@75:1.2.3.4,..." -- drives the pause-menu calls (ui_host / ui_join /
             * ui_leave) at those seconds after start, for unattended runner tests of the runtime start / stop */
            static int st_init, st_n, st_i; static struct { int op, at; char a[64]; } st_ev[12]; static ULONGLONG st_t0;
            if (!st_init) { st_init = 1; st_t0 = now; const char* e = getenv("GF_COOP_SELFTEST");
                if (e && *e) { char b[512]; snprintf(b, sizeof b, "%s", e); char* tok = strtok(b, ",");
                    while (tok && st_n < 12) { char* at = strchr(tok, '@'); if (at) { *at = 0; st_ev[st_n].op = !strcmp(tok, "host") ? 1 : !strcmp(tok, "join") ? 2 : !strcmp(tok, "leave") ? 3 : 0;
                            char* c2 = strchr(at + 1, ':'); st_ev[st_n].at = atoi(at + 1); st_ev[st_n].a[0] = 0; if (c2) snprintf(st_ev[st_n].a, sizeof st_ev[st_n].a, "%s", c2 + 1);
                            if (st_ev[st_n].op) st_n++; } tok = strtok(NULL, ","); }
                    printf("[coop] v1119M self-test: %d steps (GF_COOP_SELFTEST)\n", st_n); fflush(stdout); } }
            if (st_i < st_n && now - st_t0 >= (ULONGLONG)st_ev[st_i].at * 1000u && !s_busy && !s_cmd) {
                const int op = st_ev[st_i].op; int ok = 0;
                if (op == 1) ok = ui_host(); else if (op == 2) ok = ui_join(st_ev[st_i].a); else { ui_leave(); ok = 1; }
                printf("[coop] v1119M self-test step %d at %d s: %s %s -> %s\n", st_i + 1, st_ev[st_i].at, op == 1 ? "host" : op == 2 ? "join" : "leave", st_ev[st_i].a, ok ? "queued" : "refused"); fflush(stdout);
                st_i++; }
            {   static int last = -1; char l1[160], l2[160], l3[160]; if (st_n) { static ULONGLONG lt; if (now - lt >= 5000) { lt = now; const int stt = ui_status(l1, sizeof l1, l2, sizeof l2, l3, sizeof l3);
                    if (stt != last || 1) { printf("[coop] v1119M menu status %d: %s | %s | %s\n", stt, l1, l2, l3); fflush(stdout); last = stt; } } } }
        }
        if (s_mode == MODE_OFF) { Sleep(20); continue; }
        while ((n = net_recv(buf, sizeof buf, &from)) > 0) { stat_in(); on_packet(buf, n, &from); }
        if (s_mode == MODE_ECHO) { int k = 0; net_addr dummy; memset(&dummy, 0, sizeof dummy); for (int i = 0; i < s_echo_n; i++) { if (s_echo[i].due <= now) { stat_in(); on_packet(s_echo[i].data, s_echo[i].n, &dummy); } else s_echo[k++] = s_echo[i]; } s_echo_n = k; }
        if (s_mode == MODE_JOIN && !s_S[0].connected && !s_refused && now - last_hello > 1000) { send_hello(&s_S[0], PK_HELLO); last_hello = now; static int tries; if (tries++ % 10 == 0) { char w[64]; net_describe(&s_S[0].addr, w, sizeof w); printf("[coop] calling host %s ...\n", w); } }
        if (s_mode == MODE_JOIN && s_S[0].connected && s_S[0].since) {   /* v1126C GF_COOP_TEST_REJOIN_S=N: joiner drops its session once, N s after
            connecting, without a BYE (= what a timeout looks like to the host) -> exercises the host re-join path */
            static int rj = -1; if (rj < 0) { const char* e = getenv("GF_COOP_TEST_REJOIN_S"); rj = e && *e ? atoi(e) : 0; }
            if (rj > 0 && now - s_S[0].since > (ULONGLONG)rj * 1000u) { rj = 0; printf("[coop] v1126C test: dropping our session without a BYE (GF_COOP_TEST_REJOIN_S)\n"); fflush(stdout);
                sess_free(&s_S[0], "dropped (re-join test)"); EnterCriticalSection(&s_cs); memset(s_R, 0, sizeof s_R); LeaveCriticalSection(&s_cs); } }
        if (s_mode != MODE_ECHO) for (int i = 0; i < MAXS; i++) { sess* p = &s_S[i];
            if (p->used && p->connected && (LONGLONG)(now - p->last_rx) > (LONGLONG)coop_timeout_ms(p, now)) {   /* v1126C signed: last_rx is stamped AFTER this loop's now (packet drain) -> unsigned now - last_rx wrapped to ~2^64 = a false "timed out" (the 22:35 + 23:0x joiner drops) */
                printf("[coop] %s timed out (no packets for %u s)\n", p->name, (unsigned)((now - p->last_rx) / 1000u)); fflush(stdout); coop_note("%s timed out (connection lost)", p->name[0] ? p->name : "Partner"); sess_free(p, "timed out");
                if (s_mode == MODE_JOIN) { EnterCriticalSection(&s_cs); memset(s_R, 0, sizeof s_R); LeaveCriticalSection(&s_cs); } }
            else if (s_mode == MODE_HOST && p->used && !p->connected) { p->used = 0; } }
        if (now >= next) { ULONGLONG step = (ULONGLONG)(1000 / s_rate); next += step; if (next < now) next = now + step; state_body st;
            if (read_state(&st)) { add_motion(&st, now); cam_stamp(&st);
                if (st.valid && first_state) { first_state = 0; printf("[coop] first player transform (row-vector 4x4 at *(player+0x68)+0x10):\n"); for (int r = 0; r < 4; r++) printf("[coop]   %9.3f %9.3f %9.3f %9.3f\n", st.m[r*4],st.m[r*4+1],st.m[r*4+2],st.m[r*4+3]); fflush(stdout); }
                for (int i = 0; i < MAXS; i++) if (s_S[i].used && s_S[i].connected) send_state(&s_S[i], &st);   /* host: every joiner */
                if (s_mode == MODE_ECHO && s_echo_peers > 0 && s_S[0].connected) echo_fake_state(&st, now); } }
        if (s_harden) for (int i = 0; i < MAXS; i++) { sess* p = &s_S[i]; if (!p->used || !p->connected) continue;
            if (s_mode == MODE_ECHO && !echo_probe_sent) { EnterCriticalSection(&s_cs); echo_probe_sent = sess_queue_event(p, 0xFFFFu, "TEST", 4);
                { static const uint8_t r[6] = { 0xFE, 0xFF, 'R', 'L', 'A', 'Y' }; sess_queue_event(p, EV_RELAY, r, 6); } LeaveCriticalSection(&s_cs); }   /* v1072S relay self test */
            rel_pump(p, now); blob_flush(p);
            if (now - p->last_ping >= 1000) { send_ping(p); p->last_ping = now; identity_tick(p); }
            if (now - p->last_ack >= 250) { send_control(p, PK_ACK); p->last_ack = now; } }
        stats_tick(now);
        { static int tr = -1; static ULONGLONG last_tr; if (tr < 0) { const char* te = getenv("GF_COOP_TRACE"); tr = te && atoi(te) == 1; }   /* v1059A 1 s movement trace */
          if (tr && now - last_tr >= 1000) { last_tr = now; state_body me; float m[16], hp = 0; read_state(&me);
              const int hv = gf_coop_connected() && gf_coop_partner_pose(m, &hp);
              printf("[coop-trace] me %s %.2f %.2f %.2f yaw %.0f | partner %s %.2f %.2f %.2f yaw %.0f speed %.2f\n", me.valid ? "ok" : "--",
                     (double)me.m[12], (double)me.m[13], (double)me.m[14], (double)(atan2f(me.m[8], me.m[10]) * 57.29578f), hv ? "ok" : "--",
                     hv ? (double)m[12] : 0.0, hv ? (double)m[13] : 0.0, hv ? (double)m[14] : 0.0, hv ? (double)(atan2f(m[8], m[10]) * 57.29578f) : 0.0, hv ? (double)m[15] : 0.0); fflush(stdout); } }
        if (now - last_log >= 10000) { last_log = now; if (s_harden) { char line[192]; gf_coop_stats(line, sizeof line); printf("[coop] stats: %s\n", line); }
            { float m[16], hp = 0; const char* pn = gf_coop_partner_name();
              if (gf_coop_connected() && gf_coop_partner_pose(m, &hp)) { state_body me; read_state(&me); float dx=m[12]-me.m[12],dy=m[13]-me.m[13],dz=m[14]-me.m[14]; printf("[coop] partner %s at %.1f %.1f %.1f (%.1f m from you), health %.0f\n", pn,m[12],m[13],m[14],me.valid?sqrtf(dx*dx+dy*dy+dz*dz):-1.0f,hp); }
              else if (gf_coop_connected()) printf("[coop] connected to %s, partner not in the world yet\n", pn); }
            if (s_maxpeers > 2 || s_echo_peers > 0 || s_self_id > 1) peers_log();
            fflush(stdout); }
        Sleep(2);
    }
}

void gf_coop_shutdown(void)
{
    if (s_mode == MODE_HOST || s_mode == MODE_JOIN) for (int i = 0; i < MAXS; i++) if (s_S[i].used && s_S[i].connected) for (int k = 0; k < 3; k++) send_control(&s_S[i], PK_BYE);
}
/* ------------------------------------------------------------------ v1119M runtime start / stop (user 20:37: "get the co-op mechanic
 * somewhere in the menu so players can join each other"). The co-op core (critical section, settings, thread) now starts with
 * every game; hosting / joining / leaving happens at runtime from the pause menu (rsx_ui_overlay.c CO-OP tab, rsx_ui_coop.h).
 * GF_COOP=host|join|echo still starts a mode at boot (launchers, runner tests); GF_COOP=off disables co-op completely. */
#include <stdarg.h>
static void coop_note(const char* fmt, ...)
{
    extern const char* ui_tr(const char* en);   /* ui_i18n.c: game language */
    char t[128]; va_list ap; va_start(ap, fmt); vsnprintf(t, sizeof t, ui_tr(fmt), ap); va_end(ap);
    if (s_cs_init) { EnterCriticalSection(&s_cs); snprintf(s_msg, sizeof s_msg, "%s", t); LeaveCriticalSection(&s_cs); }
    printf("[coop] v1119M %s\n", t); fflush(stdout);
}
static void coop_banner(void)
{
    printf("[coop] %s as \"%s\" (UDP %d, %d states/s, protocol v%d, build %016llX)\n", s_mode == MODE_HOST ? "hosting" : s_mode == MODE_JOIN ? "joining" : "echo test", s_name, s_port, s_rate, s_harden ? COOP_V2 : COOP_V1, (unsigned long long)COOP_BUILD_ID);
    if (s_harden) printf("[coop] GF_COOP_HARDEN is ON: reliable ordered events, build handshake, RTT/jitter timing and stats enabled\n");
    printf("[coop] v1072S N-peer session layer: max peers %d (GF_COOP_MAXPEERS, 2 = classic host + one joiner)%s; host relays player state + pose to N-peer joiners\n",
           s_maxpeers, s_echo_peers ? " + ECHO fake peers" : "");
    if (s_echo_peers) printf("[coop] v1072S GF_COOP_ECHO_PEERS=%d: fake relayed peers 2..%d copy our state %.0f+3k m to the side and our pose\n", s_echo_peers, s_echo_peers + 1, (double)s_echo_ofs);
    fflush(stdout);
}
static void coop_clear_sessions(void)
{
    EnterCriticalSection(&s_cs);
    memset(s_S, 0, sizeof s_S); memset(s_R, 0, sizeof s_R); memset(s_PP, 0, sizeof s_PP); s_echo_n = 0;
    LeaveCriticalSection(&s_cs);
    s_refused = 0; s_self_id = -1; s_net_maxpeers = 2;
}
static int coop_start(int mode, const char* peer)   /* co-op thread (or gf_coop_init before the thread exists) */
{
    if (s_mode != MODE_OFF) return 0;
    coop_clear_sessions();
    if (!net_open(mode == MODE_JOIN ? 0 : s_port)) { coop_note("Could not open UDP port %d (is another copy of the game hosting?)", mode == MODE_JOIN ? 0 : s_port); return 0; }
    if (mode == MODE_JOIN) {
        if (!peer || !*peer || !net_resolve(peer, s_port, &s_S[0].addr)) { net_close(); coop_note("\"%s\" is not a usable address", peer ? peer : ""); return 0; }
        s_S[0].used = 1; s_S[0].id = 0; s_S[0].ev_rx_next = 1;
        EnterCriticalSection(&s_cs); snprintf(s_join_addr, sizeof s_join_addr, "%s", peer); LeaveCriticalSection(&s_cs);
    }
    if (mode == MODE_HOST) s_self_id = 0;
    if (mode == MODE_ECHO) { s_S[0].used = 1; s_S[0].connected = 1; s_S[0].id = 1; s_S[0].ev_rx_next = 1; s_self_id = 0; snprintf(s_S[0].name, sizeof s_S[0].name, "echo"); }
    s_mode_since = GetTickCount64();
    s_mode = mode;   /* published last: the rest of the game sees a fully set-up session */
    coop_banner();
    if (mode == MODE_HOST) coop_note("Hosting on UDP port %d", s_port);
    else if (mode == MODE_JOIN) coop_note("Calling the host at %s", peer);
    else coop_note("Echo test session");
    return 1;
}
static void coop_stop(void)   /* co-op thread */
{
    if (s_mode == MODE_HOST || s_mode == MODE_JOIN) for (int i = 0; i < MAXS; i++) if (s_S[i].used && s_S[i].connected) for (int k = 0; k < 3; k++) send_control(&s_S[i], PK_BYE);
    for (int i = 0; i < MAXS; i++) if (s_S[i].used) { if (s_S[i].connected) { printf("[coop] v1119M leaving: %s disconnected\n", s_S[i].name[0] ? s_S[i].name : "partner"); } s_S[i].connected = 0; }
    s_mode = MODE_OFF;   /* the game stops seeing a partner from here on (gf_coop_connected / gf_coop_mode) */
    net_close();
    coop_clear_sessions();
    coop_note("Left co-op");
}
/* local IPv4 addresses for the host's status line (private LAN first) */
static void local_ips(char* out, size_t n)
{
    char h[256]; struct addrinfo hint, *res = NULL, *r; size_t len = 0; out[0] = 0;
    if (gethostname(h, sizeof h)) return;
    memset(&hint, 0, sizeof hint); hint.ai_family = AF_INET; hint.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(h, NULL, &hint, &res) || !res) return;
    for (int pass = 0; pass < 2; pass++) for (r = res; r; r = r->ai_next) {
        const struct sockaddr_in* a = (const struct sockaddr_in*)r->ai_addr; const uint32_t ip = ntohl(a->sin_addr.s_addr); char t[32];
        const int priv = (ip >> 24) == 10 || (ip >> 20) == (172u << 4 | 1) || (ip >> 16) == (192u << 8 | 168);
        if ((ip >> 24) == 127 || (ip >> 16) == (169u << 8 | 254) || (pass == 0) != priv) continue;
        inet_ntop(AF_INET, &a->sin_addr, t, sizeof t);
        if (len + strlen(t) + 4 < n) { len += (size_t)snprintf(out + len, n - len, "%s%s", len ? "  " : "", t); }
    }
    freeaddrinfo(res);
}
/* ---- rsx_ui_coop.h API (render thread) ---- */
static int ui_status(char* l1, size_t n1, char* l2, size_t n2, char* l3, size_t n3)
{
    char msg[128], ja[128]; const ULONGLONG now = GetTickCount64(); int st = UIOV_COOP_OFF;
    EnterCriticalSection(&s_cs); snprintf(msg, sizeof msg, "%s", s_msg); snprintf(ja, sizeof ja, "%s", s_join_addr); LeaveCriticalSection(&s_cs);
    l1[0] = l2[0] = l3[0] = 0;
    if (s_busy) { snprintf(l1, n1, "Please wait..."); return UIOV_COOP_BUSY; }
    switch (s_mode) {
    case MODE_HOST: {
        char names[96] = ""; size_t k = 0; int nc = 0;
        EnterCriticalSection(&s_cs);
        for (int i = 0; i < MAXS; i++) if (s_S[i].used && s_S[i].connected) { nc++; if (k + 26 < sizeof names) k += (size_t)snprintf(names + k, sizeof names - k, "%s%s", k ? ", " : "", s_S[i].name[0] ? s_S[i].name : "?"); }
        LeaveCriticalSection(&s_cs);
        if (nc) snprintf(l1, n1, "Hosting - %d player%s joined: %s", nc, nc == 1 ? "" : "s", names);
        else snprintf(l1, n1, "Hosting - waiting for players to join");
        { static char ips[96]; static ULONGLONG ips_t; if (!ips_t || now - ips_t > 10000) { ips_t = now; local_ips(ips, sizeof ips); }
          snprintf(l2, n2, "Your address: %s  (port %d)", ips[0] ? ips : "?", s_port); }
        snprintf(l3, n3, "Over the internet: forward UDP port %d on your router and give your public IP", s_port);
        return UIOV_COOP_HOSTING; }
    case MODE_JOIN:
        if (gf_coop_connected()) {
            const sess* p; float rtt = 0; char hn[24] = "";
            EnterCriticalSection(&s_cs); p = sess_primary(); if (p) { rtt = p->rtt; memcpy(hn, p->name, sizeof hn); hn[23] = 0; } LeaveCriticalSection(&s_cs);
            snprintf(l1, n1, "Connected to %s", hn[0] ? hn : ja); snprintf(l2, n2, "Ping %.0f ms", (double)rtt); return UIOV_COOP_CONNECTED; }
        if (s_refused) { snprintf(l1, n1, "Could not join %s", ja); snprintf(l2, n2, "%s", msg); snprintf(l3, n3, "Choose LEAVE CO-OP, then try again"); return UIOV_COOP_CONNECTING; }
        snprintf(l1, n1, "Calling %s ...  %llu s", ja, (unsigned long long)((now - s_mode_since) / 1000));
        if (now - s_mode_since > 15000) { snprintf(l2, n2, "No answer yet: check the address, that the host is HOSTING,"); snprintf(l3, n3, "and that the host forwarded UDP port %d (internet play)", s_port); }
        else if (msg[0] && strncmp(msg, "Calling", 7)) snprintf(l2, n2, "%s", msg);
        return UIOV_COOP_CONNECTING;
    case MODE_ECHO: snprintf(l1, n1, "Echo test session (GF_COOP=echo)"); return UIOV_COOP_TEST;
    default:
        snprintf(l1, n1, "Not in a co-op game"); if (msg[0]) snprintf(l2, n2, "%s", msg);
        snprintf(l3, n3, "Both players need the same game build.");
        return UIOV_COOP_OFF;
    }
}
static int ui_host(void) { if (s_mode != MODE_OFF || s_busy) return 0; InterlockedExchange(&s_busy, 1); InterlockedExchange(&s_cmd, CMD_HOST); return 1; }
static int ui_join(const char* a)
{
    if (s_mode != MODE_OFF || s_busy || !a || !*a) return 0;
    EnterCriticalSection(&s_cs); snprintf(s_cmd_addr, sizeof s_cmd_addr, "%s", a); snprintf(s_last_addr, sizeof s_last_addr, "%s", a); LeaveCriticalSection(&s_cs);
    InterlockedExchange(&s_busy, 1); InterlockedExchange(&s_cmd, CMD_JOIN); return 1;
}
static void ui_leave(void) { if (s_mode == MODE_OFF || s_mode == MODE_ECHO) return; InterlockedExchange(&s_busy, 1); InterlockedExchange(&s_cmd, CMD_LEAVE); }
static const char* ui_name(void) { return s_name; }
static void ui_set_name(const char* n)
{
    if (s_mode != MODE_OFF || !n) return;
    char t[24]; int k = 0; for (int i = 0; n[i] && k < 23; i++) { const unsigned char c = (unsigned char)n[i]; if (c >= 32 && c < 127) t[k++] = (char)c; } t[k] = 0;
    while (k > 0 && t[k - 1] == ' ') t[--k] = 0;
    if (!k) return;
    memcpy(s_name, t, sizeof t);   /* GF_COOP_NAME is what gf_trainer / gf_coop_look read for the local name */
    { char e[40]; snprintf(e, sizeof e, "%s", s_name); _putenv_s("GF_COOP_NAME", e); _putenv_s("COOP_NAME", e); }
}
static const char* ui_address(void) { return s_last_addr; }
static void ui_set_address(const char* a) { if (!a) return; EnterCriticalSection(&s_cs); snprintf(s_last_addr, sizeof s_last_addr, "%s", a); LeaveCriticalSection(&s_cs); _putenv_s("COOP_ADDR", s_last_addr); }
static const uiov_coop_api k_coop_api = { ui_status, ui_host, ui_join, ui_leave, ui_name, ui_set_name, ui_address, ui_set_address };
const char* gf_coop_local_name(void) { return s_name; }

void gf_coop_init(void)
{
    const char* e = getenv("GF_COOP");
    if (e && (!_stricmp(e, "off") || !strcmp(e, "0"))) { printf("[coop] GF_COOP=%s: co-op disabled (no pause-menu CO-OP tab)\n", e); fflush(stdout); return; }
    int mode = MODE_OFF;
    if (e && *e) { if (!_stricmp(e, "host")) mode = MODE_HOST; else if (!_stricmp(e, "join")) mode = MODE_JOIN; else if (!_stricmp(e, "echo")) mode = MODE_ECHO; else { printf("[coop] GF_COOP=%s: use host, join, echo or off\n", e); } }
    InitializeCriticalSection(&s_cs); s_cs_init = 1;
    if ((e = getenv("GF_COOP_PORT")) && atoi(e) > 0) s_port = atoi(e); if ((e = getenv("GF_COOP_RATE")) && atoi(e) > 0) s_rate = atoi(e) > 120 ? 120 : atoi(e);
    if ((e = getenv("GF_COOP_ECHO_OFS"))) s_echo_ofs = (float)atof(e); if ((e = getenv("GF_COOP_ECHO_DELAY"))) s_echo_delay = atoi(e);
    s_harden = 1; if ((e = getenv("GF_COOP_HARDEN")) && atoi(e) == 0) s_harden = 0;   /* v1058D: v2 is the default (tested q58i) */
    if ((e = getenv("GF_COOP_MAXPEERS")) && atoi(e) >= 2) s_maxpeers = atoi(e) > GF_COOP_MAX_PEERS ? GF_COOP_MAX_PEERS : atoi(e);   /* v1072S */
    if (!s_harden && s_maxpeers > 2) { printf("[coop] v1072S GF_COOP_MAXPEERS=%d needs GF_COOP_HARDEN=1 -> 2\n", s_maxpeers); s_maxpeers = 2; }
    if (mode == MODE_ECHO && (e = getenv("GF_COOP_ECHO_PEERS")) && atoi(e) > 0) s_echo_peers = atoi(e) > GF_COOP_MAX_PEERS - 2 ? GF_COOP_MAX_PEERS - 2 : atoi(e);
    /* name: GF_COOP_NAME (launchers), else COOP_NAME (settings.ini, set in the CO-OP tab), else the Windows user name */
    if ((e = getenv("GF_COOP_NAME")) && *e) snprintf(s_name, sizeof s_name, "%s", e);
    else if ((e = getenv("COOP_NAME")) && *e) snprintf(s_name, sizeof s_name, "%s", e);
    else { DWORD z = sizeof s_name; if (!GetUserNameA(s_name, &z)) snprintf(s_name, sizeof s_name, "Player"); }
    if ((e = getenv("COOP_ADDR")) && *e) snprintf(s_last_addr, sizeof s_last_addr, "%s", e);
    else if ((e = getenv("GF_COOP_PEER")) && *e) snprintf(s_last_addr, sizeof s_last_addr, "%s", e);
    /* v1119M: the co-op modules' switches the coop-local launchers used to set -- defaults now, so a co-op game started from the
     * menu has the puppet + pose stream (an explicit launcher / env value still wins) */
    { static const char* const def[][2] = { { "GF_COOP_PUPPET", "drive" }, { "GF_COOP_PUPPET_AUTO", "1" }, { "GF_COOP_POSE", "1" },
                                             { "GF_COOP_PROXY_PREGUID", "0" }, { "GF_COOP_PROXY_NOMODEL_FIX", "0" } };
      for (int i = 0; i < (int)(sizeof def / sizeof def[0]); i++) { const char* v = getenv(def[i][0]); if (!v || !*v) _putenv_s(def[i][0], def[i][1]); } }
    if (mode != MODE_OFF && !coop_start(mode, getenv("GF_COOP_PEER"))) mode = MODE_OFF;
    HANDLE h = CreateThread(NULL, 256u << 10, coop_thread, NULL, 0, NULL); if (h) CloseHandle(h);
    g_uiov_coop = &k_coop_api;
    if (mode == MODE_OFF) { printf("[coop] v1119M co-op ready as \"%s\": pause menu > Co-op (host / join), UDP port %d\n", s_name, s_port); fflush(stdout); }
}

/* ================================================================== v1072S N-peer API (gf_coop_peers.h) */
typedef struct { int id, kind, idx; } pref;   /* kind 1 = direct session, 2 = relayed peer */
static int peer_list(pref out[GF_COOP_MAX_PEERS])   /* caller holds s_cs; ascending peer id */
{
    int n = 0; const ULONGLONG now = GetTickCount64();
    for (int id = 0; id < GF_COOP_MAX_PEERS; id++) {
        int done = 0;
        for (int i = 0; i < MAXS && !done; i++) if (s_S[i].used && s_S[i].connected && s_S[i].id == id) { out[n].id = id; out[n].kind = 1; out[n].idx = i; n++; done = 1; }
        if (!done && s_mode != MODE_HOST && id != s_self_id && s_R[id].used && (LONGLONG)(now - s_R[id].last_rx) < (LONGLONG)RSTALE_MS) { out[n].id = id; out[n].kind = 2; out[n].idx = id; n++; }
    }
    return n;
}
int gf_coop_maxpeers(void) { return s_maxpeers; }
int gf_coop_self_id(void) { return s_self_id; }
int gf_coop_cur_peer(void) { return t_cur_peer; }
static int npeer_active(void) { return s_cs_init && (s_maxpeers > 2 || s_net_maxpeers > 2 || s_echo_peers > 0); }
int gf_coop_npeer_active(void) { return npeer_active(); }
int gf_coop_peer_count(void) { pref l[GF_COOP_MAX_PEERS]; int n; if (!npeer_active()) return -1; EnterCriticalSection(&s_cs); n = peer_list(l); LeaveCriticalSection(&s_cs); return n; }   /* -1 = classic 2-player (W contract) */
int gf_coop_peer_id(int i)
{
    pref l[GF_COOP_MAX_PEERS]; int n, r = -1; if (!s_cs_init || i < 0) return -1;
    EnterCriticalSection(&s_cs); n = peer_list(l); if (i < n) r = l[i].id; LeaveCriticalSection(&s_cs); return r;
}
static int peer_find(int id, pref* o) { pref l[GF_COOP_MAX_PEERS]; const int n = peer_list(l); for (int i = 0; i < n; i++) if (l[i].id == id) { *o = l[i]; return 1; } return 0; }
int gf_coop_peer_pose_id(int id, float out_m[16], float* hp)
{
    pref r; int ok = 0; if (!s_cs_init) return 0;
    EnterCriticalSection(&s_cs);
    if (peer_find(id, &r)) { if (r.kind == 1) ok = ring_pose(&s_S[r.idx].sr, s_S[r.idx].rtt, s_S[r.idx].jitter, out_m, hp);
        else { const float rtt = s_S[0].rtt + 20.0f; ok = ring_pose(&s_R[r.idx].sr, rtt, s_R[r.idx].jitter, out_m, hp); } }   /* relayed: one more hop */
    LeaveCriticalSection(&s_cs); return ok;
}
int gf_coop_peer_pose(int i, float out_m[16], uint16_t* peer_id) { const int id = gf_coop_peer_id(i); if (peer_id) *peer_id = (uint16_t)(id < 0 ? 0xFFFF : id); return id >= 0 ? gf_coop_peer_pose_id(id, out_m, NULL) : 0; }
int gf_coop_peer_cam_id(int id, float* fx, float* fz)
{
    pref r; LONG h = 0, ok = 0; ULONGLONG ms = 0; if (!s_cs_init) return 0;
    EnterCriticalSection(&s_cs);
    if (peer_find(id, &r)) { if (r.kind == 1) { h = s_S[r.idx].cam_h; ok = s_S[r.idx].cam_ok; ms = s_S[r.idx].cam_ms; } else { h = s_R[r.idx].cam_h; ok = s_R[r.idx].cam_ok; ms = s_R[r.idx].cam_ms; } }
    LeaveCriticalSection(&s_cs); return cam_get(h, ok, ms, fx, fz);
}
int gf_coop_peer_cam(int i, float* fx, float* fz) { const int id = gf_coop_peer_id(i); return id >= 0 ? gf_coop_peer_cam_id(id, fx, fz) : 0; }
int gf_coop_peer_name_id(int id, char out[24])
{
    pref r; int ok = 0; if (!s_cs_init || !out) return 0; out[0] = 0;
    EnterCriticalSection(&s_cs);
    if (peer_find(id, &r)) { memcpy(out, r.kind == 1 ? s_S[r.idx].name : s_R[r.idx].name, 24); out[23] = 0; ok = 1; }
    LeaveCriticalSection(&s_cs); return ok;
}
int gf_coop_peer_is_primary_id(int id) { const sess* p; int r; if (!s_cs_init) return 0; EnterCriticalSection(&s_cs); p = sess_primary(); r = p && p->id == id; LeaveCriticalSection(&s_cs); return r; }
int gf_coop_peer_direct_id(int id) { pref r; int d = 0; if (!s_cs_init) return 0; EnterCriticalSection(&s_cs); d = peer_find(id, &r) && r.kind == 1; LeaveCriticalSection(&s_cs); return d; }
int gf_coop_peer_pose_packets(int id, uint8_t* a, int* la, uint8_t* b, int* lb, uint32_t* ma, uint32_t* mb, unsigned long long* age_ms)
{
    int n = 0; if (!s_cs_init || id < 0 || id >= GF_COOP_MAX_PEERS) return 0;
    EnterCriticalSection(&s_cs);
    { const ppose* q = &s_PP[id]; n = q->n;
      if (n >= 1) { if (b) memcpy(b, q->d[1], (size_t)q->l[1]); if (lb) *lb = q->l[1]; if (mb) *mb = q->ms[1]; if (age_ms) *age_ms = GetTickCount64() - q->rx; }
      if (n >= 2) { if (a) memcpy(a, q->d[0], (size_t)q->l[0]); if (la) *la = q->l[0]; if (ma) *ma = q->ms[0]; } else if (la) *la = 0; }
    LeaveCriticalSection(&s_cs); return n;
}
int gf_coop_send_event_to(uint16_t id, uint16_t type, const void* data, int len)
{
    int ok = 0; if (!s_harden || !s_cs_init || len < 0 || len > 1000 || (len && !data) || type < 0x100) return 0;
    EnterCriticalSection(&s_cs); { sess* p = sess_by_id(id); if (p) ok = sess_queue_event(p, type, data, len); } LeaveCriticalSection(&s_cs); return ok;
}
int gf_coop_send_blob_to(uint16_t id, uint8_t channel, const void* data, int len)
{
    int ok = 0; if (!s_harden || !s_cs_init || len <= 0 || len > 1150 || !data) return 0;
    EnterCriticalSection(&s_cs); { sess* p = sess_by_id(id); if (p) { blob_put(p, channel, data, len); ok = 1; } } LeaveCriticalSection(&s_cs); return ok;
}
/* v1072S generic relay: every OTHER peer gets it with our id as origin (joiner -> host -> other joiners; host -> all joiners).
 * Delivered ONLY to all-peer handlers (gf_coop_add_peer_event_handler / _blob_handler), never to legacy handlers. Events: type >= 0x100,
 * len <= 997 (reliable on each hop). Blobs: channel 0x80..0xFF (latest-wins per (channel & 15) relay slot), len <= 1150. */
int gf_coop_send_event_relay(uint16_t type, const void* data, int len)
{
    int ok = 0; uint8_t env[1000];
    if (!s_harden || !s_cs_init || len < 0 || len > 997 || (len && !data) || type < 0x100) return 0;
    if (s_mode == MODE_HOST) { relay_event_fwd(NULL, 0, type, (const uint8_t*)data, len); st_rev_tx++; return 1; }
    memcpy(env, &type, 2); if (len) memcpy(env + 2, data, (size_t)len);
    EnterCriticalSection(&s_cs); { sess* p = &s_S[0]; if (p->used && p->connected) ok = sess_queue_event(p, EV_RELAY, env, len + 2); } LeaveCriticalSection(&s_cs);
    if (ok) st_rev_tx++;
    return ok;
}
int gf_coop_send_blob_relay(uint8_t channel, const void* data, int len)
{
    if (!(channel & 0x80)) return 0;
    if (s_mode == MODE_HOST) { int any = 0; if (!s_harden || !s_cs_init || len <= 0 || len > 1150 || !data) return 0;
        for (int i = 0; i < MAXS; i++) { sess* p = &s_S[i]; if (!p->used || !p->connected || !(p->caps & CAP_NPEER)) continue;
            { pk_pblob q; fill_hdr2(p, &q.h, PK_PBLOB, 0); q.origin = 0; q.channel = channel; q.len = (uint16_t)len; memcpy(q.data, data, (size_t)len);
              if (wire_send(p, &q, (int)(sizeof q - sizeof q.data) + len)) any = 1; } }
        return any; }
    return gf_coop_send_blob(channel, data, len);   /* joiner: to the host, which forwards */
}
void gf_coop_add_peer_event_handler(gf_coop_peer_event_fn fn) { if (!fn) return; if (s_cs_init) EnterCriticalSection(&s_cs); for (int i = 0; i < 8; i++) { if (s_nev[i] == fn) break; if (!s_nev[i]) { s_nev[i] = fn; break; } } if (s_cs_init) LeaveCriticalSection(&s_cs); }
void gf_coop_add_peer_blob_handler(gf_coop_peer_blob_fn fn) { if (!fn) return; if (s_cs_init) EnterCriticalSection(&s_cs); for (int i = 0; i < 8; i++) { if (s_nblob[i] == fn) break; if (!s_nblob[i]) { s_nblob[i] = fn; break; } } if (s_cs_init) LeaveCriticalSection(&s_cs); }
static void peers_log(void)
{
    pref l[GF_COOP_MAX_PEERS]; int n; char line[640]; size_t o = 0;
    EnterCriticalSection(&s_cs); n = peer_list(l);
    o += (size_t)snprintf(line + o, sizeof line - o, "[coop] v1072S peers: self %d, %d remote (max %d) |", s_self_id, n, s_maxpeers);
    for (int i = 0; i < n && o < sizeof line - 64; i++) {
        const char* nm = l[i].kind == 1 ? s_S[l[i].idx].name : s_R[l[i].idx].name; const int prim = l[i].kind == 1 && &s_S[l[i].idx] == sess_primary();
        o += (size_t)snprintf(line + o, sizeof line - o, " %d %s%s%s rtt %.0f, pose pkts %d;", l[i].id, nm, prim ? " (primary)" : "", l[i].kind == 2 ? " (relayed)" : "",
                              l[i].kind == 1 ? (double)s_S[l[i].idx].rtt : -1.0, s_PP[l[i].id].n); }
    LeaveCriticalSection(&s_cs);
    printf("%s relay: states %ld, poses %ld (tier-skipped %ld / %ld) | rx relayed states %ld, poses %ld | generic relay: events sent %ld, forwarded %ld, received %ld, blobs forwarded %ld, received %ld | full refusals %ld, broadcast event drops %ld\n", line, st_relay_state, st_relay_pose, st_tier_skip[0], st_tier_skip[1], st_rx_pstate, st_rx_pblob, st_rev_tx, st_rev_fwd, st_rev_rx, st_rblob_fwd, st_rblob_rx, st_full, st_bcast_drop);
}
