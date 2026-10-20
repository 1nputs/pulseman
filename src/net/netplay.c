/* Rollback netplay (mdrt/netplay.h): up to four machines, a host and its guests, in a star: each
 * guest talks to the host alone, and the host passes every guest's buttons on to the others. The
 * messages, all little-endian after "MN" and a type:
 *   HELLO      protocol u16, role u8, nonce u64, identity (length u8, bytes), name (length u8,
 *              bytes), players u8 (the pads it plays)
 *   ACCEPT     the guest's nonce u64, session u32, delay u8, window u8, rollback u8, check_every u16,
 *              settings' size u32, the host's name (length u8, bytes)
 *   REFUSE     the guest's nonce u64, why (length u8, bytes)
 *   CHUNK      session u32, index u32, length u16, bytes (the settings, CHUNK bytes at a time)
 *   CHUNK_ACK  session u32, the chunks had in a row u32, which of the 128 after the first missing
 *              it has, two u64 (bit k of word w: chunk had in a row + 1 + 64 w + k)
 *   READY      session u32 (the guest has the settings)
 *   START      session u32, the receiver's place u8, peers u8, then each peer's pads u8 and name
 *              (length u8, bytes), the host first (every machine powers on)
 *   INPUT      session u32, whose u8 (the peer whose buttons these are: a guest sends its own, the
 *              host its own and the others'), first frame u32, count u8, pads u8, the buttons
 *              (count x pads u16); then the sender's: peers u8 and the newest frame of each
 *              peer's buttons it has in a row (u32 each), its own frame u32, how far ahead of the
 *              receiver it thinks it is i16 (sixteenths of a frame), a desync check: frame u32 and
 *              hash u64 (frame 0: none)
 *   PING, PONG session u32, time u64 (PONG echoes PING's)
 *   BYE        session u32, why (length u8, bytes: what ended the session, for every player) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mdrt/netplay.h"

#define RING 256                        /* frames of buttons kept (more than the window, delay and replay) */
#define CHUNK 1024
#define CHUNKS_OUT 128                  /* settings chunks in flight ahead of a guest's acknowledgement (128 KB:
                                           within a system's usual socket buffer) */
#define SACK (CHUNKS_OUT / 64)          /* the acknowledgement's words of which chunks the guest has */
#define MAX_RESEND 100                  /* frames of buttons in one INPUT */
#define CHECKS 8
#define PEERS MDRT_PADS                 /* machines at most: a pad each at least */
#define MS 1000u
#define LOST_US (3000u * MS)
#define GONE_US (10000u * MS)

enum { T_HELLO = 1, T_ACCEPT, T_REFUSE, T_CHUNK, T_CHUNK_ACK, T_READY, T_START, T_INPUT, T_PING, T_PONG, T_BYE };
enum { P_HELLO, P_HOSTING, P_GUEST, P_RUNNING, P_DONE };

typedef struct snap { uint32_t frame; int valid; size_t len, cap; uint8_t *buf; } snap;

/* a machine of the session: this one, the host, a guest */
typedef struct peer {
    int used;
    int link;                           /* (a host's guest) the link's index for it (-1: the link's one peer) */
    uint64_t nonce;
    char name[MDRT_NET_NAME + 1];
    int players;
    uint8_t pads;
    /* (a host's guest) the settings sent it: acknowledged in a row, sent up to, when each was last
     * sent, which of the window's it said it had, when they last went; it has them all and said so */
    uint32_t chunks_have, chunks_sent;
    uint64_t *chunk_at, chunk_mask[SACK], chunks_at;
    int ready;
    /* its buttons: known in a row up to newest, the frame each ring slot holds */
    uint32_t newest;
    uint32_t frame_of[RING];
    /* (a peer this one talks to) what it has of each peer's buttons in a row, as it last said */
    uint32_t has[PEERS];
    uint64_t heard_at, ping_at, input_at;
    int rtt_us;
    /* keeping in step: its frame as last heard, how far ahead each thinks it is (sixteenths) */
    uint32_t their_frame;
    uint64_t their_frame_at;
    int their_adv, my_adv, adv_known;
    struct { uint32_t frame; uint64_t hash; } theirs[CHECKS];
} peer;

struct mdrt_net {
    mdrt_net_config c;
    char identity[256];
    char name[MDRT_NET_NAME + 1];
    int phase, result;
    char why[200];
    int host, multi;                    /* this peer hosts; its link reaches more than one (send_to) */
    int me, npeers;                     /* this peer's place, the machines (the host 0) */
    peer p[PEERS];
    int8_t owner[MDRT_PADS];            /* the peer each pad is played at (-1: none) */
    int players, total;                 /* this peer's players; (a host's) the players in all */
    uint64_t nonce, rng;
    uint32_t session;
    int delay, window, rollback, check_every;
    /* the settings (a guest's: had in a row, which it has) */
    uint8_t *settings, *got;
    size_t settings_size;
    uint32_t chunks, have;
    uint64_t hello_at, ready_at;
    int started;
    uint32_t period_us;                 /* a frame */
    /* the console and the frames */
    mdrt *m;
    uint32_t frame;                     /* the frame last arrived at (its wait) */
    int arrived;
    uint32_t replay_to;                 /* a rollback plays again up to this frame (0: none) */
    uint16_t btn[RING][MDRT_PADS];      /* buttons by frame: this peer's own, the others' as known, */
    uint16_t used[RING][MDRT_PADS];     /* and what the console was given */
    uint32_t latched, wrong;
    uint32_t reset_done;                /* the frame the console last powered on again at (the host's reset) */
    snap *snaps;
    int nsnaps;
    uint64_t hold_until;
    uint32_t sync_at;
    /* desync checks: this peer's hash of a frame (final once every button up to it is known and was
     * given), and which of the peers it talks to have agreed */
    struct { uint32_t frame; uint64_t hash; int final; unsigned agreed; } mine[CHECKS];
    /* the log */
    mdrt_sink_fn log;
    void *log_user;
    uint32_t logged;
    mdrt_net_stats st;
};

/* ---- bytes ------------------------------------------------------------------------------------ */

typedef struct out { uint8_t b[MDRT_NET_MTU]; size_t n; } out;
static void p8(out *o, uint32_t v) { if (o->n < sizeof o->b) o->b[o->n] = (uint8_t)v; o->n++; }
static void p16(out *o, uint32_t v) { p8(o, v); p8(o, v >> 8); }
static void p32(out *o, uint32_t v) { p16(o, v); p16(o, v >> 16); }
static void p64(out *o, uint64_t v) { p32(o, (uint32_t)v); p32(o, (uint32_t)(v >> 32)); }
static void pbytes(out *o, const void *b, size_t n) { for (size_t i = 0; i < n; i++) p8(o, ((const uint8_t *)b)[i]); }
static void pstr(out *o, const char *s) { size_t k = strlen(s); if (k > 255) k = 255; p8(o, (uint32_t)k); pbytes(o, s, k); }
static void begin(out *o, int type) { o->n = 0; p8(o, 'M'); p8(o, 'N'); p8(o, (uint32_t)type); }

typedef struct in { const uint8_t *b; size_t n, size; int bad; } in;
static uint32_t g8(in *i) { if (i->n >= i->size) { i->bad = 1; return 0; } return i->b[i->n++]; }
static uint32_t g16(in *i) { uint32_t v = g8(i); return v | g8(i) << 8; }
static uint32_t g32(in *i) { uint32_t v = g16(i); return v | g16(i) << 16; }
static uint64_t g64(in *i) { uint64_t v = g32(i); return v | (uint64_t)g32(i) << 32; }
static const uint8_t *gbytes(in *i, size_t n)
{
    if (i->size - i->n < n || i->n > i->size) { i->bad = 1; return NULL; }
    const uint8_t *p = i->b + i->n;
    i->n += n;
    return p;
}

static uint64_t now(mdrt_net *n) { return n->c.link.ticks(n->c.link.user); }

/* a datagram to peer k: a host's guest by its link index, a guest's host (the link's one peer) */
static void to_peer(mdrt_net *n, int k, const out *o)
{
    if (o->n > MDRT_NET_MTU) return;
    if (n->multi && n->p[k].link >= 0) n->c.link.send_to(n->c.link.user, n->p[k].link, o->b, o->n);
    else n->c.link.send(n->c.link.user, o->b, o->n);
}

/* the peers this one talks to: a guest its host, a host its guests */
static int talks(const mdrt_net *n, int k) { return n->host ? k > 0 && n->p[k].used : k == 0 && n->p[0].used; }
static unsigned talk_mask(const mdrt_net *n)
{
    unsigned m = 0;
    for (int k = 0; k < PEERS; k++) if (talks(n, k)) m |= 1u << k;
    return m;
}

static uint64_t next_random(mdrt_net *n)          /* (splitmix64) */
{
    uint64_t z = (n->rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* a name as the others may show it: printable ASCII, cut short */
static void take_name(char *to, const uint8_t *from, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n && k < MDRT_NET_NAME; i++) to[k++] = from[i] >= 32 && from[i] < 127 ? (char)from[i] : '?';
    to[k] = 0;
}

static int count_bits(uint8_t v) { int k = 0; for (; v; v &= (uint8_t)(v - 1)) k++; return k; }
static uint8_t my_pads(const mdrt_net *n) { return n->p[n->me].pads; }

/* a player as the others say it: their name, else Player and their first pad's number (the host's
 * before the pads are given: Player 1) */
static void who(const mdrt_net *n, int k, char *buf, size_t size)
{
    const peer *q = &n->p[k];
    int pad = 0;
    while (pad < MDRT_PADS && !(q->pads >> pad & 1)) pad++;
    if (q->name[0]) snprintf(buf, size, "%s", q->name);
    else snprintf(buf, size, "Player %d", pad < MDRT_PADS ? pad + 1 : k + 1);
}

/* the session over; from inside mdrt_net_frame (detach: the console plays on alone) or not */
static void end_with(mdrt_net *n, int result, const char *why, int detach)
{
    if (n->phase == P_DONE) return;
    n->phase = P_DONE;
    n->result = result;
    snprintf(n->why, sizeof n->why, "%s", why);
    if (n->m && detach) {
        mdrt_set_pad_source(n->m, NULL, NULL);
        mdrt_set_quiet(n->m, 0);
    }
}
static void end(mdrt_net *n, int result, const char *why) { end_with(n, result, why, 1); }

static void send_bye(mdrt_net *n, int k, const char *why)
{
    out o;
    begin(&o, T_BYE);
    p32(&o, n->session);
    pstr(&o, why);
    for (int i = 0; i < 3; i++) to_peer(n, k, &o);        /* (three: one is enough) */
}

/* the session over for every machine: each peer this one talks to told why (a host's guests pass
 * nothing on: the host tells them all) */
static void end_all(mdrt_net *n, int result, const char *why, int detach)
{
    if (n->phase == P_DONE) return;
    if (n->session)
        for (int k = 0; k < PEERS; k++) if (talks(n, k)) send_bye(n, k, why);
    end_with(n, result, why, detach);
}

/* ---- the session's start ------------------------------------------------------------------------ */

mdrt_net *mdrt_net_new(const mdrt_net_config *c)
{
    int players = c->players ? c->players : 1, total = c->players_total ? c->players_total : 2;
    if (!c->link.send || !c->link.recv || !c->link.ticks || c->role < MDRT_NET_HOST || c->role > MDRT_NET_EITHER ||
        c->delay < 0 || c->delay > MDRT_NET_MAX_DELAY || c->settings_size > MDRT_NET_MAX_SETTINGS ||
        (c->settings_size && !c->settings) || (c->identity && strlen(c->identity) > 255) ||
        players < 1 || players >= MDRT_PADS || total > MDRT_PADS || (c->role != MDRT_NET_JOIN && total <= players))
        return NULL;
    mdrt_net *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->c = *c;
    snprintf(n->identity, sizeof n->identity, "%s", c->identity ? c->identity : "");
    n->c.identity = n->identity;
    take_name(n->name, (const uint8_t *)(c->name ? c->name : ""), c->name ? strlen(c->name) : 0);
    n->c.name = n->name;
    n->players = players;
    n->total = total;
    n->delay = c->delay;
    n->window = c->window > 0 ? c->window : 8;
    n->rollback = c->rollback > 0 ? c->rollback : 30;
    n->check_every = c->check_every > 0 ? c->check_every : 60;
    if (n->window + n->delay + n->rollback + 8 > RING / 2) { free(n); return NULL; }
    n->rng = c->seed ? c->seed : now(n) ^ (uint64_t)(uintptr_t)n;
    n->nonce = next_random(n);
    for (int k = 0; k < PEERS; k++) { n->p[k].link = -1; n->p[k].rtt_us = -1; }
    n->host = c->role == MDRT_NET_HOST;
    n->multi = n->host && c->link.send_to && c->link.recv_from;
    if (n->host) {                                        /* (a waiting room: its own players first) */
        n->phase = P_HOSTING;
        n->p[0].used = 1;
        n->p[0].players = players;
        n->p[0].pads = (uint8_t)((1u << players) - 1);
        snprintf(n->p[0].name, sizeof n->p[0].name, "%s", n->name);
    }
    if (c->settings_size) {                               /* (a guest's are the host's, once heard) */
        n->settings = malloc(c->settings_size);
        if (!n->settings) { free(n); return NULL; }
        memcpy(n->settings, c->settings, c->settings_size);
        n->settings_size = c->settings_size;
    }
    n->st.rtt_ms = -1;
    return n;
}

void mdrt_net_free(mdrt_net *n)
{
    if (!n) return;
    for (int i = 0; i < n->nsnaps; i++) free(n->snaps[i].buf);
    free(n->snaps);
    free(n->settings);
    free(n->got);
    for (int k = 0; k < PEERS; k++) free(n->p[k].chunk_at);
    free(n);
}

int mdrt_net_is_host(const mdrt_net *n) { return n->host; }
int mdrt_net_state(const mdrt_net *n)
{
    return n->phase == P_DONE ? n->result : n->phase == P_RUNNING ? MDRT_NET_GO : MDRT_NET_CONNECTING;
}
uint8_t mdrt_net_pads(const mdrt_net *n) { return my_pads(n); }
int mdrt_net_delay(const mdrt_net *n) { return n->delay; }
const char *mdrt_net_why(const mdrt_net *n) { return n->why; }
const void *mdrt_net_settings(const mdrt_net *n, size_t *size)
{
    *size = n->settings_size;
    return n->settings_size ? n->settings : NULL;
}
int mdrt_net_peers(const mdrt_net *n)
{
    int k = 0;
    for (int i = 0; i < PEERS; i++) k += n->p[i].used;
    return k;
}
int mdrt_net_me(const mdrt_net *n) { return n->me; }
const char *mdrt_net_name_of(const mdrt_net *n, int k) { return k >= 0 && k < PEERS && n->p[k].used ? n->p[k].name : ""; }
uint8_t mdrt_net_pads_of(const mdrt_net *n, int k) { return k >= 0 && k < PEERS && n->p[k].used ? n->p[k].pads : 0; }
int mdrt_net_players_of(const mdrt_net *n, int k) { return k >= 0 && k < PEERS && n->p[k].used ? n->p[k].players : 0; }
uint8_t mdrt_net_all_pads(const mdrt_net *n)
{
    uint8_t all = 0;
    for (int k = 0; k < PEERS; k++) if (n->p[k].used) all |= n->p[k].pads;
    return all;
}
/* the other player's name: a guest's host, a host's latest guest */
const char *mdrt_net_peer_name(const mdrt_net *n)
{
    if (!n->host) return n->p[0].name;
    for (int k = PEERS - 1; k > 0; k--) if (n->p[k].used) return n->p[k].name;
    return "";
}

static void send_hello(mdrt_net *n)
{
    out o;
    begin(&o, T_HELLO);
    p16(&o, MDRT_NET_PROTOCOL); p8(&o, (uint32_t)n->c.role); p64(&o, n->nonce);
    pstr(&o, n->identity);
    pstr(&o, n->name);
    p8(&o, (uint32_t)n->players);
    to_peer(n, 0, &o);
}

static void send_accept(mdrt_net *n, int k)
{
    out o;
    begin(&o, T_ACCEPT);
    p64(&o, n->p[k].nonce); p32(&o, n->session);
    p8(&o, (uint32_t)n->delay); p8(&o, (uint32_t)n->window); p8(&o, (uint32_t)n->rollback); p16(&o, (uint32_t)n->check_every);
    p32(&o, (uint32_t)n->settings_size);
    pstr(&o, n->name);
    to_peer(n, k, &o);
}

/* a refusal to whoever sent a HELLO (from: the link's index for it) */
static void send_refuse(mdrt_net *n, int from, uint64_t nonce, const char *why)
{
    out o;
    begin(&o, T_REFUSE);
    p64(&o, nonce);
    pstr(&o, why);
    if (n->multi && from >= 0) n->c.link.send_to(n->c.link.user, from, o.b, o.n);
    else n->c.link.send(n->c.link.user, o.b, o.n);
}

static void send_session(mdrt_net *n, int k, int type)
{
    out o;
    begin(&o, type);
    p32(&o, n->session);
    to_peer(n, k, &o);
}

static void send_ping(mdrt_net *n, int k, int type, uint64_t t)
{
    out o;
    begin(&o, type);
    p32(&o, n->session); p64(&o, t);
    to_peer(n, k, &o);
}

static void send_start(mdrt_net *n, int k)
{
    out o;
    begin(&o, T_START);
    p32(&o, n->session); p8(&o, (uint32_t)k); p8(&o, (uint32_t)n->npeers);
    for (int i = 0; i < n->npeers; i++) { p8(&o, n->p[i].pads); pstr(&o, n->p[i].name); }
    to_peer(n, k, &o);
}

static void send_chunk(mdrt_net *n, int k, uint32_t i)
{
    peer *g = &n->p[k];
    if (g->chunk_at) g->chunk_at[i] = now(n);
    out o;
    size_t at = (size_t)i * CHUNK, len = n->settings_size - at < CHUNK ? n->settings_size - at : CHUNK;
    begin(&o, T_CHUNK);
    p32(&o, n->session); p32(&o, i); p16(&o, (uint32_t)len); pbytes(&o, n->settings + at, len);
    to_peer(n, k, &o);
}

/* the settings' chunks up to CHUNKS_OUT past guest k's acknowledgement: those not sent yet, or
 * (again: nothing acknowledged for a while) every one not acknowledged. The guest acknowledges
 * every chunk it gets with the ones it has in a row and which of the window's others it has, so
 * the host sends again just those it lacks, a round trip after their last sending (sack) */
static int guest_has(const peer *g, uint32_t i)
{
    uint32_t k = i - g->chunks_have - 1;                  /* (chunks_have itself: lacking) */
    return i > g->chunks_have && k < CHUNKS_OUT && (g->chunk_mask[k / 64] >> (k % 64) & 1);
}
static void send_chunks(mdrt_net *n, int k, int again)
{
    peer *g = &n->p[k];
    uint32_t end = n->chunks - g->chunks_have > CHUNKS_OUT ? g->chunks_have + CHUNKS_OUT : n->chunks;
    if (again)                                            /* (those the guest said it lacks) */
        for (uint32_t i = g->chunks_have; i < end && i < g->chunks_sent; i++) if (!guest_has(g, i)) send_chunk(n, k, i);
    for (uint32_t i = g->chunks_sent > g->chunks_have ? g->chunks_sent : g->chunks_have; i < end; i++) send_chunk(n, k, i);
    if (end > g->chunks_sent) g->chunks_sent = end;
    g->chunks_at = now(n);
}

/* a guest's place in the waiting room given up (it left, or went quiet) */
static void drop_guest(mdrt_net *n, int k)
{
    free(n->p[k].chunk_at);
    memset(&n->p[k], 0, sizeof n->p[k]);
    n->p[k].link = -1;
    n->p[k].rtt_us = -1;
}

/* a guest's HELLO (from: the link's index for its address), taken by this peer as the host */
static void host_guest(mdrt_net *n, int from, uint64_t nonce, uint32_t protocol, const uint8_t *id, size_t idn,
                       const uint8_t *name, size_t namen, int players)
{
    for (int k = 1; k < PEERS; k++)
        if (n->p[k].used && n->p[k].nonce == nonce) { n->p[k].link = from; send_accept(n, k); return; }   /* (it did not hear the first) */
    char why[160];
    int seats = n->total;
    for (int k = 0; k < PEERS; k++) if (n->p[k].used) seats -= n->p[k].players;
    int free_k = 0;
    for (int k = PEERS - 1; k > 0; k--) if (!n->p[k].used) free_k = k;
    if (n->phase == P_RUNNING) snprintf(why, sizeof why, "the host's game has begun");
    else if (protocol != MDRT_NET_PROTOCOL)
        snprintf(why, sizeof why, "another version of netplay (%u here, %u there)", MDRT_NET_PROTOCOL, (unsigned)protocol);
    else if (idn != strlen(n->identity) || memcmp(id, n->identity, idn))
        snprintf(why, sizeof why, "another game, build or settings (here: %.50s; there: %.*s)", n->identity, idn > 50 ? 50 : (int)idn, (const char *)id);
    else if (!n->multi && mdrt_net_peers(n) > 1) snprintf(why, sizeof why, "the host is playing with someone else");
    else if (players < 1 || players > seats || !free_k)
        snprintf(why, sizeof why, seats > 0 ? "the game has room for %d more player%s" : "the game is full", seats, seats == 1 ? "" : "s");
    else why[0] = 0;
    if (why[0]) {
        send_refuse(n, from, nonce, why);
        if (n->phase != P_RUNNING) snprintf(n->why, sizeof n->why, "refused a player: %.160s", why);
        return;
    }
    n->host = 1;
    n->phase = P_HOSTING;
    if (!n->p[0].used) {                                  /* (an "either" that hosts) */
        n->p[0].used = 1;
        n->p[0].players = n->players;
        n->p[0].pads = (uint8_t)((1u << n->players) - 1);
        snprintf(n->p[0].name, sizeof n->p[0].name, "%s", n->name);
    }
    if (!n->session) n->session = (uint32_t)next_random(n) | 1;
    peer *g = &n->p[free_k];
    drop_guest(n, free_k);
    g->used = 1;
    g->link = from;
    g->nonce = nonce;
    g->players = players;
    take_name(g->name, name, namen);
    n->chunks = (uint32_t)((n->settings_size + CHUNK - 1) / CHUNK);
    g->chunk_at = n->chunks ? calloc(n->chunks, sizeof *g->chunk_at) : NULL;
    g->heard_at = now(n);
    send_accept(n, free_k);
    send_chunks(n, free_k, 0);
}

/* every place taken and every guest with the settings: the guests in the order they came (their
 * places closed up), the pads given in that order (the host's first), START to each */
static void begin_session(mdrt_net *n)
{
    int k = 1;
    for (int i = 1; i < PEERS; i++) {
        if (!n->p[i].used) continue;
        if (i != k) { n->p[k] = n->p[i]; memset(&n->p[i], 0, sizeof n->p[i]); n->p[i].link = -1; n->p[i].rtt_us = -1; }
        k++;
    }
    n->npeers = k;
    int pad = 0;
    for (int i = 0; i < MDRT_PADS; i++) n->owner[i] = -1;
    for (int i = 0; i < n->npeers; i++) {
        n->p[i].pads = 0;
        for (int j = 0; j < n->p[i].players; j++, pad++) { n->p[i].pads |= (uint8_t)(1u << pad); n->owner[pad] = (int8_t)i; }
    }
    for (int i = 1; i < n->npeers; i++) send_start(n, i);
    n->phase = P_RUNNING;
}

static void handle(mdrt_net *n, const uint8_t *b, size_t size, int from);

/* everything waiting on the link */
static void receive(mdrt_net *n)
{
    uint8_t b[MDRT_NET_MTU + 64];
    for (int k = 0; k < 1000; k++) {
        int from = -1;
        int got = n->multi ? n->c.link.recv_from(n->c.link.user, b, sizeof b, &from) : n->c.link.recv(n->c.link.user, b, sizeof b);
        if (got <= 0) break;
        handle(n, b, (size_t)got, from);
    }
}

/* pings for the round trip, a few a second, to each peer this one talks to */
static void ping(mdrt_net *n)
{
    uint64_t t = now(n);
    if (!n->session) return;
    for (int k = 0; k < PEERS; k++)
        if (talks(n, k) && t - n->p[k].ping_at >= 250 * MS) { send_ping(n, k, T_PING, t); n->p[k].ping_at = t; }
}

int mdrt_net_connect(mdrt_net *n)
{
    if (n->phase == P_DONE) return n->result;
    if (n->phase == P_RUNNING) return MDRT_NET_READY;
    receive(n);
    if (n->phase == P_DONE) return n->result;
    uint64_t t = now(n);
    if (n->phase == P_HELLO && n->c.role != MDRT_NET_HOST && t - n->hello_at >= 100 * MS) { send_hello(n); n->hello_at = t; }
    if (n->phase == P_HOSTING) {
        int all_ready = 1, seats = n->total, guests = 0;
        for (int k = 0; k < PEERS; k++) {
            peer *g = &n->p[k];
            if (!g->used) continue;
            seats -= g->players;
            if (k == 0) continue;
            guests++;
            if (t - g->heard_at > GONE_US) {              /* (gone before the game: its place given up) */
                char name[40];
                who(n, k, name, sizeof name);
                snprintf(n->why, sizeof n->why, "%s went quiet", name);
                drop_guest(n, k);
                all_ready = 0;
                continue;
            }
            uint64_t wait = g->rtt_us > 0 ? 2 * (uint64_t)g->rtt_us : 100 * MS;   /* (nothing acknowledged in twice the round trip) */
            if (g->chunks_have < n->chunks && t - g->chunks_at >= (wait > 100 * MS ? wait : 100 * MS)) send_chunks(n, k, 1);
            if (!g->ready) all_ready = 0;
        }
        if (guests && all_ready && seats == 0) begin_session(n);
    }
    if (n->phase == P_GUEST) {
        if (n->have == n->chunks && t - n->ready_at >= 100 * MS) { send_session(n, 0, T_READY); n->ready_at = t; }
        if (n->started && n->have == n->chunks) n->phase = P_RUNNING;
    }
    ping(n);
    return n->phase == P_RUNNING ? MDRT_NET_READY : MDRT_NET_CONNECTING;
}

/* ---- the frames ---------------------------------------------------------------------------------- */

static uint32_t slot(uint32_t f) { return f % RING; }

/* the frames whose buttons every peer's are known to (the lowest newest), and the others' (this
 * peer's own aside) */
static uint32_t known_all(const mdrt_net *n)
{
    uint32_t m = UINT32_MAX;
    for (int k = 0; k < n->npeers; k++) if (n->p[k].newest < m) m = n->p[k].newest;
    return m == UINT32_MAX ? 0 : m;
}
static uint32_t known_others(const mdrt_net *n)
{
    uint32_t m = UINT32_MAX;
    for (int k = 0; k < n->npeers; k++) if (k != n->me && n->p[k].newest < m) m = n->p[k].newest;
    return m == UINT32_MAX ? n->p[n->me].newest : m;
}

/* the buttons a frame is given: this peer's pads its own, each other's as known or guessed (the
 * last known, held) */
static void pad_source(void *user, uint16_t b[MDRT_PADS])
{
    mdrt_net *n = user;
    uint32_t f = (uint32_t)mdrt_frame_count(n->m) + 1;
    for (int p = 0; p < MDRT_PADS; p++) {
        int q = n->owner[p];
        b[p] = 0;
        if (q < 0) continue;
        if (q == n->me) { if (f <= n->p[q].newest) b[p] = n->btn[slot(f)][p]; continue; }
        uint32_t known = f <= n->p[q].newest ? f : n->p[q].newest;
        if (n->p[q].frame_of[slot(known)] == known) b[p] = n->btn[slot(known)][p];
        if (known < f) b[p] &= (uint16_t)~MDRT_NET_RESET_BIT;   /* (a guess holds the buttons, never a reset) */
    }
    memcpy(n->used[slot(f)], b, sizeof n->used[0]);
    for (int p = 0; p < MDRT_PADS; p++) b[p] &= (uint16_t)~MDRT_NET_RESET_BIT;   /* (no pad's button) */
    n->latched = f;
}

/* frame f's buttons, as the console was given them, hold the host's reset */
static int reset_at(const mdrt_net *n, uint32_t f)
{
    if (n->latched != f) return 0;
    for (int p = 0; p < MDRT_PADS; p++)
        if (n->owner[p] == 0 && (n->used[slot(f)][p] & MDRT_NET_RESET_BIT)) return 1;
    return 0;
}

/* frame f goes on: powered on again first if the host reset the console there (once a pass) */
static int go(mdrt_net *n, uint32_t f)
{
    if (n->reset_done == f || !reset_at(n, f)) return MDRT_NET_GO;
    n->reset_done = f;
    mdrt_power_on(n->m);
    n->st.resets++;
    return MDRT_NET_RESET;
}

void mdrt_net_start(mdrt_net *n, mdrt *m)
{
    n->m = m;
    n->period_us = (uint32_t)((uint64_t)mdrt_frame_mclks(m) * 1000000u / mdrt_master_clock(m));
    /* frames 1 to delay + 1: no one's buttons yet (all known, as nothing) */
    uint32_t first = (uint32_t)n->delay + 1;
    for (int k = 0; k < n->npeers; k++) {
        n->p[k].newest = first;
        for (uint32_t f = 0; f <= first; f++) n->p[k].frame_of[slot(f)] = f;
        for (int j = 0; j < PEERS; j++) n->p[k].has[j] = first;
        n->p[k].heard_at = now(n);
    }
    n->logged = 0;
    n->frame = 0;
    n->arrived = 0;
    n->latched = 0;
    int k = n->rollback + n->window + 4;
    n->snaps = calloc((size_t)k, sizeof *n->snaps);
    n->nsnaps = n->snaps ? k : 0;
    mdrt_set_pad_source(m, pad_source, n);
}

/* the state at frame f (a resumable point): in place of the same frame's, or the oldest */
static void take_snapshot(mdrt_net *n, uint32_t f)
{
    snap *s = NULL;
    for (int i = 0; i < n->nsnaps; i++)
        if (n->snaps[i].valid && n->snaps[i].frame == f) s = &n->snaps[i];
    for (int i = 0; i < n->nsnaps && !s; i++)
        if (!n->snaps[i].valid) s = &n->snaps[i];
    for (int i = 0; i < n->nsnaps && !s; i++)               /* (none free: the oldest) */
        for (int j = 0; j < n->nsnaps; j++)
            if (!s || n->snaps[j].frame < s->frame) s = &n->snaps[j];
    if (!s) return;
    size_t need = mdrt_get_state(n->m, NULL, 0);
    if (need > s->cap) {
        uint8_t *b = realloc(s->buf, need);
        if (!b) { s->valid = 0; return; }
        s->buf = b;
        s->cap = need;
    }
    s->len = mdrt_get_state(n->m, s->buf, s->cap);
    s->frame = f;
    s->valid = 1;
}

/* the newest state at or before frame f, or NULL */
static snap *snapshot_before(mdrt_net *n, uint32_t f)
{
    snap *best = NULL;
    for (int i = 0; i < n->nsnaps; i++)
        if (n->snaps[i].valid && n->snaps[i].frame <= f && (!best || n->snaps[i].frame > best->frame)) best = &n->snaps[i];
    return best;
}

/* peer whose's buttons to peer `to`, from the first it lacks; then what this peer has of each,
 * its frame, its advantage over `to` and its newest final check */
static void send_buttons(mdrt_net *n, int to, int whose)
{
    peer *t = &n->p[to], *w = &n->p[whose];
    uint32_t first = t->has[whose] + 1, last = w->newest;
    if (last >= MAX_RESEND && first < last - MAX_RESEND + 1) first = last - MAX_RESEND + 1;
    uint32_t count = first <= last ? last - first + 1 : 0;
    out o;
    begin(&o, T_INPUT);
    p32(&o, n->session); p8(&o, (uint32_t)whose); p32(&o, count ? first : 0); p8(&o, count); p8(&o, (uint32_t)count_bits(w->pads));
    for (uint32_t f = first; count && f <= last; f++)
        for (int p = 0; p < MDRT_PADS; p++)
            if (w->pads >> p & 1) p16(&o, n->btn[slot(f)][p]);
    p8(&o, (uint32_t)n->npeers);
    for (int k = 0; k < n->npeers; k++) p32(&o, n->p[k].newest);
    int adv = t->my_adv < -30000 ? -30000 : t->my_adv > 30000 ? 30000 : t->my_adv;
    p32(&o, n->frame); p16(&o, (uint32_t)(uint16_t)(int16_t)adv);
    uint32_t cf = 0;                                      /* (the newest final check) */
    uint64_t ch = 0;
    for (int i = 0; i < CHECKS; i++)
        if (n->mine[i].final && n->mine[i].frame > cf) { cf = n->mine[i].frame; ch = n->mine[i].hash; }
    p32(&o, cf); p64(&o, ch);
    to_peer(n, to, &o);
    if (whose == n->me) t->input_at = now(n);
}

/* this peer's buttons (and, the host, the others') to each peer it talks to */
static void send_inputs(mdrt_net *n, int relays)
{
    for (int k = 0; k < n->npeers; k++) {
        if (!talks(n, k)) continue;
        send_buttons(n, k, n->me);
        if (relays) for (int w = 1; w < n->npeers; w++) if (w != k) send_buttons(n, k, w);
    }
}

/* peer whose's buttons for frames from first: the guesses they show wrong */
static void take_inputs(mdrt_net *n, in *i, int whose, uint32_t first, uint32_t count, uint32_t npads)
{
    peer *w = &n->p[whose];
    for (uint32_t k = 0; k < count; k++) {
        uint32_t f = first + k;
        uint16_t b[MDRT_PADS] = { 0 };
        for (int p = 0, j = 0; p < MDRT_PADS && (uint32_t)j < npads; p++)
            if (w->pads >> p & 1) { b[p] = (uint16_t)g16(i); j++; }
        if (i->bad) return;
        if (f <= w->newest || f >= w->newest + RING / 2) continue;
        for (int p = 0; p < MDRT_PADS; p++) if (w->pads >> p & 1) n->btn[slot(f)][p] = b[p];
        w->frame_of[slot(f)] = f;
    }
    while (w->frame_of[slot(w->newest + 1)] == w->newest + 1) {
        uint32_t f = ++w->newest;
        if (f > n->latched) continue;
        for (int p = 0; p < MDRT_PADS; p++)               /* (a frame the console was given a guess for) */
            if ((w->pads >> p & 1) && n->used[slot(f)][p] != n->btn[slot(f)][p]) {
                if (!n->wrong || f < n->wrong) n->wrong = f;
                break;
            }
    }
}

/* the peer a datagram came from (from: the link's index; -1, the link's one peer), or -1 */
static int sender(const mdrt_net *n, int from)
{
    if (!n->host) return n->p[0].used ? 0 : -1;
    for (int k = 1; k < PEERS; k++)
        if (n->p[k].used && (n->p[k].link == from || !n->multi)) return k;
    return -1;
}

static void handle(mdrt_net *n, const uint8_t *b, size_t size, int from)
{
    in i = { b, 0, size, 0 };
    if (g8(&i) != 'M' || g8(&i) != 'N') return;
    int type = (int)g8(&i);
    uint64_t t = now(n);
    if (type == T_HELLO) {
        uint32_t protocol = g16(&i), role = g8(&i);
        uint64_t nonce = g64(&i);
        uint32_t idn = g8(&i);
        const uint8_t *id = gbytes(&i, idn);
        uint32_t namen = protocol == MDRT_NET_PROTOCOL ? g8(&i) : 0;   /* (an older protocol's: refused below) */
        const uint8_t *name = namen ? gbytes(&i, namen) : NULL;
        int players = protocol == MDRT_NET_PROTOCOL ? (int)g8(&i) : 1;
        if (i.bad || n->phase == P_DONE || nonce == n->nonce) return;
        if (n->phase == P_GUEST || (n->phase == P_RUNNING && !n->host)) return;
        int host_it = n->c.role == MDRT_NET_HOST || (n->c.role == MDRT_NET_EITHER && (role == MDRT_NET_JOIN ||
                      (role == MDRT_NET_EITHER && n->nonce > nonce)));
        if (n->c.role == MDRT_NET_JOIN && role == MDRT_NET_JOIN) {
            send_refuse(n, from, nonce, "both are joining: one must host");
            end(n, MDRT_NET_REFUSED, "both are joining: one must host");
            return;
        }
        if (n->c.role == MDRT_NET_HOST && role == MDRT_NET_HOST) return;   /* (a host never says hello) */
        if (host_it) host_guest(n, from, nonce, protocol, id, idn, name, namen, players);
        return;
    }
    if (type == T_ACCEPT || type == T_REFUSE) {
        uint64_t nonce = g64(&i);
        if (i.bad || nonce != n->nonce || n->phase != P_HELLO || n->c.role == MDRT_NET_HOST) return;
        if (type == T_REFUSE) {
            uint32_t k = g8(&i);
            const uint8_t *why = gbytes(&i, k);
            char text[160];
            snprintf(text, sizeof text, "refused by the host: %.*s", why ? (int)k : 0, why ? (const char *)why : "");
            end(n, MDRT_NET_REFUSED, text);
            return;
        }
        uint32_t session = g32(&i), delay = g8(&i), window = g8(&i), rollback = g8(&i);
        uint32_t every = g16(&i), ssize = g32(&i), namen = g8(&i);
        const uint8_t *name = namen ? gbytes(&i, namen) : NULL;
        if (i.bad || delay > MDRT_NET_MAX_DELAY || !window || !rollback || !every || window + delay + rollback + 8 > RING / 2 ||
            ssize > MDRT_NET_MAX_SETTINGS) return;
        n->host = 0;
        n->session = session;
        n->p[0].used = 1;
        n->p[0].link = -1;
        n->p[0].heard_at = t;
        take_name(n->p[0].name, name, namen);
        n->delay = (int)delay; n->window = (int)window; n->rollback = (int)rollback; n->check_every = (int)every;
        free(n->settings);
        n->settings = ssize ? malloc(ssize) : NULL;
        n->chunks = (ssize + CHUNK - 1) / CHUNK;
        n->got = n->chunks ? calloc(n->chunks, 1) : NULL;
        if ((ssize && !n->settings) || (n->chunks && !n->got)) { end(n, MDRT_NET_ENDED, "out of memory"); return; }
        n->settings_size = ssize;
        n->have = 0;
        n->phase = P_GUEST;
        return;
    }
    uint32_t session = g32(&i);
    if (i.bad || !n->session || session != n->session) return;
    int s = sender(n, from);
    if (s < 0) return;
    peer *sp = &n->p[s];
    sp->heard_at = t;
    n->st.lost = 0;
    switch (type) {
    case T_CHUNK: {
        uint32_t idx = g32(&i), k = g16(&i);
        const uint8_t *d = gbytes(&i, k);
        if (!d || n->host || idx >= n->chunks || (size_t)idx * CHUNK + k > n->settings_size) return;
        if (!n->got[idx]) { memcpy(n->settings + (size_t)idx * CHUNK, d, k); n->got[idx] = 1; }
        while (n->have < n->chunks && n->got[n->have]) n->have++;
        uint64_t mask[SACK] = { 0 };
        for (uint32_t j = 0; j < CHUNKS_OUT && n->have + 1 + j < n->chunks; j++)
            if (n->got[n->have + 1 + j]) mask[j / 64] |= 1ull << (j % 64);
        out o;
        begin(&o, T_CHUNK_ACK); p32(&o, n->session); p32(&o, n->have);
        for (int w = 0; w < SACK; w++) p64(&o, mask[w]);
        to_peer(n, 0, &o);
        break;
    }
    case T_CHUNK_ACK: {
        uint32_t have = g32(&i);
        uint64_t mask[SACK];
        for (int w = 0; w < SACK; w++) mask[w] = g64(&i);
        if (i.bad || !n->host || have > n->chunks || have < sp->chunks_have) break;
        int moved = have > sp->chunks_have;
        sp->chunks_have = have;
        memcpy(sp->chunk_mask, mask, sizeof mask);
        uint64_t at = now(n), rtt = sp->rtt_us > 0 ? (uint64_t)sp->rtt_us : 100 * MS;
        for (uint32_t c = have; c <= have + CHUNKS_OUT && c < sp->chunks_sent; c++)   /* (the guest's lacking ones, a round trip on) */
            if (!guest_has(sp, c) && sp->chunk_at && at - sp->chunk_at[c] > rtt + rtt / 4) send_chunk(n, s, c);
        if (moved) send_chunks(n, s, 0);
        break;
    }
    case T_READY:
        if (n->host && sp->chunks_have == n->chunks) { sp->ready = 1; if (n->phase == P_RUNNING) send_start(n, s); }
        break;
    case T_START: {
        uint32_t me = g8(&i), np = g8(&i);
        if (n->host || i.bad || np < 2 || np > PEERS || me < 1 || me >= np) return;
        uint8_t pads[PEERS], all = 0;
        char names[PEERS][MDRT_NET_NAME + 1];
        for (uint32_t k = 0; k < np; k++) {
            pads[k] = (uint8_t)g8(&i);
            uint32_t len = g8(&i);
            const uint8_t *nm = gbytes(&i, len);
            if (i.bad || !pads[k] || (pads[k] & all) || (pads[k] >> MDRT_PADS)) return;
            all |= pads[k];
            take_name(names[k], nm, len);
        }
        if (n->started) return;
        n->me = (int)me;
        n->npeers = (int)np;
        for (int k = 0; k < MDRT_PADS; k++) n->owner[k] = -1;
        for (uint32_t k = 0; k < np; k++) {
            n->p[k].used = 1;
            n->p[k].pads = pads[k];
            n->p[k].players = count_bits(pads[k]);
            memcpy(n->p[k].name, names[k], sizeof n->p[k].name);   /* (the same size, ended) */
            for (int p = 0; p < MDRT_PADS; p++) if (pads[k] >> p & 1) n->owner[p] = (int8_t)k;
        }
        n->started = 1;
        break;
    }
    case T_INPUT: {
        uint32_t whose = g8(&i), first = g32(&i), count = g8(&i), npads = g8(&i);
        if (i.bad || !n->m || n->phase != P_RUNNING) return;   /* (buttons come again once it plays) */
        if (whose >= (uint32_t)n->npeers || (int)whose == n->me || npads != (uint32_t)count_bits(n->p[whose].pads)) return;
        if (n->host && (int)whose != s) return;           /* (a guest sends its own) */
        take_inputs(n, &i, (int)whose, first, count, npads);
        uint32_t np = g8(&i), has[PEERS] = { 0 };
        for (uint32_t k = 0; k < np; k++) { uint32_t v = g32(&i); if (k < PEERS) has[k] = v; }
        uint32_t frame = g32(&i);
        int adv = (int16_t)g16(&i);
        uint32_t cf = g32(&i);
        uint64_t ch = g64(&i);
        if (i.bad) return;
        for (int k = 0; k < n->npeers && k < (int)np; k++)  /* (what it has: at most what is here, where that is known) */
            if (has[k] > sp->has[k] && ((k != n->me && !n->host) || has[k] <= n->p[k].newest)) sp->has[k] = has[k];
        if (frame >= sp->their_frame) { sp->their_frame = frame; sp->their_frame_at = t; }
        sp->their_adv = adv;
        sp->adv_known = 1;
        if (cf) { sp->theirs[(cf / (uint32_t)n->check_every) % CHECKS].frame = cf; sp->theirs[(cf / (uint32_t)n->check_every) % CHECKS].hash = ch; }
        if (n->host && count)                             /* (passed on to the others at once) */
            for (int k = 1; k < n->npeers; k++) if (k != s) send_buttons(n, k, s);
        break;
    }
    case T_PING:
        send_ping(n, s, T_PONG, g64(&i));
        break;
    case T_PONG: {
        uint64_t sent = g64(&i);
        if (i.bad || sent > t) return;
        int sample = (int)(t - sent);
        sp->rtt_us = sp->rtt_us < 0 ? sample : (sp->rtt_us * 7 + sample) / 8;
        break;
    }
    case T_BYE: {
        uint32_t k = g8(&i);
        const uint8_t *w = gbytes(&i, k);
        char why[200], name[40];
        who(n, s, name, sizeof name);
        if (w && k) snprintf(why, sizeof why, "%.*s", (int)k, (const char *)w);
        else snprintf(why, sizeof why, "%s left", name);
        if (n->host && n->phase == P_HOSTING) {           /* (left the waiting room: its place given up) */
            snprintf(n->why, sizeof n->why, "%.190s", why);
            drop_guest(n, s);
            break;
        }
        end_all(n, MDRT_NET_ENDED, why, 1);               /* (a host tells the others) */
        break;
    }
    default: break;
    }
}

/* the frames whose buttons are all known, to the log */
static void write_log(mdrt_net *n)
{
    uint32_t upto = known_all(n);
    if (!n->log) { n->logged = upto; return; }
    while (n->logged < upto) {
        uint32_t f = ++n->logged;
        char line[64];
        uint16_t b[MDRT_PADS];
        for (int p = 0; p < MDRT_PADS; p++) b[p] = n->owner[p] >= 0 ? n->btn[slot(f)][p] : 0;
        int k = snprintf(line, sizeof line, "%u %04x %04x %04x %04x\n", (unsigned)f, b[0], b[1], b[2], b[3]);
        n->log(n->log_user, line, (size_t)k);
    }
}

void mdrt_net_set_log(mdrt_net *n, mdrt_sink_fn sink, void *user) { n->log = sink; n->log_user = user; }

/* this peer's checks that are final, against each peer it talks to */
static void check_desync(mdrt_net *n)
{
    unsigned all = talk_mask(n);
    for (int c = 0; c < CHECKS; c++) {
        if (!n->mine[c].frame || n->mine[c].agreed == all) continue;
        if (!n->mine[c].final && known_all(n) >= n->mine[c].frame && (!n->wrong || n->wrong > n->mine[c].frame) &&
            (!n->replay_to || n->mine[c].frame <= n->frame))
            n->mine[c].final = 1;
        if (!n->mine[c].final) continue;
        for (int k = 0; k < PEERS; k++) {
            if (!(all >> k & 1) || (n->mine[c].agreed >> k & 1) || n->p[k].theirs[c].frame != n->mine[c].frame) continue;
            if (n->p[k].theirs[c].hash == n->mine[c].hash) { n->mine[c].agreed |= 1u << k; n->st.checks++; continue; }
            char why[96];
            snprintf(why, sizeof why, "desync at frame %u: the games differ", (unsigned)n->mine[c].frame);
            end_all(n, MDRT_NET_ENDED, why, 1);
            return;
        }
    }
}

/* how far ahead of peer k this peer is, in sixteenths of a frame, averaged */
static void step_advantage(mdrt_net *n, int k)
{
    peer *q = &n->p[k];
    if (!q->their_frame_at || q->rtt_us < 0) return;
    uint64_t since = now(n) - q->their_frame_at + (uint64_t)q->rtt_us / 2;
    int64_t est = (int64_t)q->their_frame * 16 + (int64_t)(since * 16 / n->period_us);
    int sample = (int)((int64_t)n->frame * 16 - est);
    q->my_adv = (q->my_adv * 15 + sample) / 16;
}

int mdrt_net_frame(mdrt_net *n, int resumable)
{
    if (n->phase == P_DONE) return n->result;
    if (!n->m || n->phase != P_RUNNING) return MDRT_NET_WAIT;
    uint32_t f = (uint32_t)mdrt_frame_count(n->m);
    peer *me = &n->p[n->me];
    if (!n->arrived || f != n->frame) {                   /* arriving at frame f's wait */
        n->arrived = 1;
        n->frame = f;
        if (n->replay_to && f >= n->replay_to) n->replay_to = 0;
        if (resumable && !reset_at(n, f)) take_snapshot(n, f);   /* (a state about to be powered off: none) */
        while (me->newest < f + 1 + (uint32_t)n->delay) { /* this peer's buttons, for frame f + 1 + delay */
            uint32_t g = ++me->newest;
            uint16_t held[MDRT_PADS] = { 0 };
            if (n->c.local_input && g == f + 1 + (uint32_t)n->delay) n->c.local_input(n->c.input_user, held);
            for (int p = 0, k = 0; p < MDRT_PADS; p++)
                if (me->pads >> p & 1) n->btn[slot(g)][p] = held[k++];
            me->frame_of[slot(g)] = g;
        }
        int c = (int)((f / (uint32_t)n->check_every) % CHECKS);
        if (f % (uint32_t)n->check_every == 0 && f && !(n->mine[c].frame == f && n->mine[c].agreed == talk_mask(n))) {
            n->mine[c].frame = f;
            n->mine[c].hash = mdrt_sim_hash(n->m);
            n->mine[c].final = 0;
            n->mine[c].agreed = 0;
        }
        if (!n->replay_to) {
            for (int k = 0; k < n->npeers; k++) if (talks(n, k)) step_advantage(n, k);
            send_inputs(n, 0);
        }
    }
    receive(n);
    if (n->phase == P_DONE) return n->result;
    uint64_t t = now(n);
    uint64_t oldest = t;
    for (int k = 0; k < n->npeers; k++) if (talks(n, k) && n->p[k].input_at < oldest) oldest = n->p[k].input_at;
    if (t - oldest >= 16 * MS) send_inputs(n, n->host);   /* (again, in case it was lost; a host the others' too) */
    ping(n);
    write_log(n);
    int rtt = -1;
    for (int k = 0; k < n->npeers; k++) if (talks(n, k) && n->p[k].rtt_us > rtt) rtt = n->p[k].rtt_us;
    n->st.rtt_ms = rtt < 0 ? -1 : (rtt + 500) / 1000;

    /* a guess shown wrong: the state from before it, the frames since played again */
    if (n->wrong && n->wrong <= n->latched) {
        uint32_t back_to = n->replay_to > f ? n->replay_to : f;
        snap *s = snapshot_before(n, n->wrong - 1);
        if (!s || mdrt_set_state(n->m, s->buf, s->len)) {
            char why[96];
            snprintf(why, sizeof why, "could not roll back to frame %u", (unsigned)n->wrong);
            end_all(n, MDRT_NET_ENDED, why, 1);
            return n->result;
        }
        uint32_t replay = back_to - s->frame;
        mdrt_set_quiet(n->m, (int)replay);
        n->replay_to = back_to;
        n->latched = s->frame;
        n->frame = s->frame;
        n->wrong = 0;
        n->reset_done = 0;                                /* (a reset after it: played again too) */
        for (int i = 0; i < n->nsnaps; i++)               /* (the states after it were the guesses': gone, even where the */
            if (n->snaps[i].valid && n->snaps[i].frame > s->frame) n->snaps[i].valid = 0;   /* frames played again have no resumable point) */
        for (int c = 0; c < CHECKS; c++)
            if (n->mine[c].frame > s->frame && n->mine[c].agreed != talk_mask(n)) n->mine[c].frame = 0;
        n->st.rollbacks++;
        n->st.replayed += replay;
        n->st.last_replayed = (int)replay;
        if ((int)replay > n->st.most_replayed) n->st.most_replayed = (int)replay;
        return MDRT_NET_RESTART;
    }
    check_desync(n);
    if (n->phase == P_DONE) return n->result;
    if (n->replay_to) return go(n, f);                    /* (playing frames again: no waiting) */

    /* frame f + 1 may go: every other peer's buttons known, or guessed within the window and a state to roll back to */
    uint32_t next = f + 1, known = known_others(n);
    int wait = 0;
    if (next > known) {
        snap *s = snapshot_before(n, known);
        wait = next - known > (uint32_t)n->window || !s || next - s->frame > (uint32_t)n->rollback;
    }
    /* keeping in step: this peer gives a frame back when it runs ahead of one it talks to */
    if (!wait && f >= n->sync_at) {
        int ahead = 0, any = 0;
        for (int k = 0; k < n->npeers; k++) {
            if (!talks(n, k) || !n->p[k].adv_known) continue;
            any = 1;
            int a = (n->p[k].my_adv - n->p[k].their_adv) / 2;
            if (a > ahead) ahead = a;
        }
        if (any) {
            n->sync_at = f + 60;
            if (ahead >= 16) {
                n->hold_until = t + n->period_us;
                n->st.gave_back++;
            }
        }
    }
    if (!wait && n->hold_until > t) wait = 1;
    if (wait) {
        n->st.waits++;
        for (int k = 0; k < n->npeers; k++) {
            if (!talks(n, k)) continue;
            if (t - n->p[k].heard_at > LOST_US) n->st.lost = 1;
            if (t - n->p[k].heard_at > GONE_US) {
                char why[96], name[40];
                who(n, k, name, sizeof name);
                if (n->host) snprintf(why, sizeof why, "connection lost with %s", name);
                else snprintf(why, sizeof why, "connection lost");
                end_all(n, MDRT_NET_ENDED, why, 1);
                return n->result;
            }
        }
        return MDRT_NET_WAIT;
    }
    return go(n, f);
}

void mdrt_net_quit(mdrt_net *n)
{
    if (n->phase == P_DONE) return;
    char why[64], name[40];
    who(n, n->me, name, sizeof name);
    snprintf(why, sizeof why, "%s left", name);          /* (what the others are told) */
    end_all(n, MDRT_NET_ENDED, why, 0);
    snprintf(n->why, sizeof n->why, "this player left");
}

void mdrt_net_stats_get(const mdrt_net *n, mdrt_net_stats *s)
{
    *s = n->st;
    s->frame = n->frame;
    s->confirmed = n->npeers ? known_all(n) : 0;
    s->remote_frame = UINT32_MAX;
    s->remote_has = UINT32_MAX;
    for (int k = 0; k < n->npeers; k++) {
        if (!talks(n, k)) continue;
        if (n->p[k].their_frame < s->remote_frame) s->remote_frame = n->p[k].their_frame;
        for (int w = 0; w < n->npeers; w++) {             /* (every peer's buttons it is to have: a guest its own here, a host everyone's there) */
            if (w == k || (!n->host && w != n->me)) continue;
            if (n->p[k].has[w] < s->remote_has) s->remote_has = n->p[k].has[w];
        }
    }
    if (s->remote_frame == UINT32_MAX) s->remote_frame = 0;
    if (s->remote_has == UINT32_MAX) s->remote_has = 0;
    s->delay = n->delay;
}
