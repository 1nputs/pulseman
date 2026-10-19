/* Rollback netplay (mdrt/netplay.h). The messages, all little-endian after "MN" and a type:
 *   HELLO      protocol u16, role u8, nonce u64, identity (length u8, bytes), name (length u8, bytes)
 *   ACCEPT     the guest's nonce u64, session u32, host's pads u8, guest's pads u8, delay u8,
 *              window u8, rollback u8, check_every u16, settings' size u32, the host's name
 *              (length u8, bytes)
 *   REFUSE     the guest's nonce u64, why (length u8, bytes)
 *   CHUNK      session u32, index u32, length u16, bytes (the settings, CHUNK bytes at a time)
 *   CHUNK_ACK  session u32, the chunks had in a row u32
 *   READY      session u32 (the guest has the settings)
 *   START      session u32 (both power on)
 *   INPUT      session u32, first frame u32, count u8, pads u8, the buttons (count x pads u16),
 *              the newest frame of the receiver's it has in a row u32, its own frame u32, how far
 *              ahead it thinks it is i16 (sixteenths of a frame), a desync check: frame u32 and
 *              hash u64 (frame 0: none)
 *   PING, PONG session u32, time u64 (PONG echoes PING's)
 *   BYE        session u32 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mdrt/netplay.h"

#define RING 256                        /* frames of buttons kept (more than the window, delay and replay) */
#define CHUNK 1024
#define CHUNKS_OUT 16                   /* settings chunks sent ahead of the guest's acknowledgement */
#define MAX_RESEND 100                  /* frames of buttons in one INPUT */
#define CHECKS 8
#define MS 1000u
#define LOST_US (3000u * MS)
#define GONE_US (10000u * MS)

enum { T_HELLO = 1, T_ACCEPT, T_REFUSE, T_CHUNK, T_CHUNK_ACK, T_READY, T_START, T_INPUT, T_PING, T_PONG, T_BYE };
enum { P_HELLO, P_HOSTING, P_GUEST, P_RUNNING, P_DONE };

typedef struct snap { uint32_t frame; int valid; size_t len, cap; uint8_t *buf; } snap;

struct mdrt_net {
    mdrt_net_config c;
    char identity[256];
    char name[MDRT_NET_NAME + 1], peer_name[MDRT_NET_NAME + 1];   /* this player's, the other's */
    int phase, result;
    char why[200];
    int host;
    uint64_t nonce, guest_nonce, rng;
    uint32_t session;
    uint8_t pads[2];                    /* the host's and the guest's */
    int delay, window, rollback, check_every;
    /* the settings */
    uint8_t *settings, *got;
    size_t settings_size;
    uint32_t chunks, chunks_have;       /* host: acknowledged in a row; guest: had in a row */
    uint64_t chunks_at, hello_at, ready_at;
    int ready_heard, started;
    /* time */
    uint64_t heard_at, ping_at, input_at;
    int rtt_us;
    uint32_t period_us;                 /* a frame */
    /* the console and the frames */
    mdrt *m;
    uint32_t frame;                     /* the frame last arrived at (its wait) */
    int arrived;
    uint32_t replay_to;                 /* a rollback plays again up to this frame (0: none) */
    uint16_t local[RING][MDRT_PADS];    /* buttons by frame: this peer's pads, */
    uint16_t remote[RING][MDRT_PADS];   /* the other's (known), */
    uint32_t remote_frame[RING];        /* (the frame each slot holds) */
    uint16_t used[RING][MDRT_PADS];     /* and what the console was given */
    uint32_t local_newest, remote_newest, latched, acked, wrong;
    snap *snaps;
    int nsnaps;
    /* keeping in step */
    uint32_t their_frame;
    uint64_t their_frame_at;
    int their_adv, my_adv, adv_known;   /* sixteenths of a frame */
    uint64_t hold_until;
    uint32_t sync_at;
    /* desync checks: this peer's hash of a frame (final once every button up to it is known and was
     * given), the other's */
    struct { uint32_t frame; uint64_t hash; int final, compared; } mine[CHECKS];
    struct { uint32_t frame; uint64_t hash; } theirs[CHECKS];
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
static void transmit(mdrt_net *n, const out *o) { if (o->n <= MDRT_NET_MTU) n->c.link.send(n->c.link.user, o->b, o->n); }

static uint64_t next_random(mdrt_net *n)          /* (splitmix64) */
{
    uint64_t z = (n->rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* a name as the other may show it: printable ASCII, cut short */
static void take_name(char *to, const uint8_t *from, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n && k < MDRT_NET_NAME; i++) to[k++] = from[i] >= 32 && from[i] < 127 ? (char)from[i] : '?';
    to[k] = 0;
}

static int count_bits(uint8_t v) { int k = 0; for (; v; v &= (uint8_t)(v - 1)) k++; return k; }
static uint8_t my_pads(const mdrt_net *n) { return n->pads[n->host ? 0 : 1]; }
static uint8_t their_pads(const mdrt_net *n) { return n->pads[n->host ? 1 : 0]; }

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

/* ---- the session's start ------------------------------------------------------------------------ */

mdrt_net *mdrt_net_new(const mdrt_net_config *c)
{
    if (!c->link.send || !c->link.recv || !c->link.ticks || c->role < MDRT_NET_HOST || c->role > MDRT_NET_EITHER ||
        c->delay < 0 || c->delay > MDRT_NET_MAX_DELAY || c->settings_size > MDRT_NET_MAX_SETTINGS ||
        (c->settings_size && !c->settings) || (c->identity && strlen(c->identity) > 255))
        return NULL;
    mdrt_net *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->c = *c;
    snprintf(n->identity, sizeof n->identity, "%s", c->identity ? c->identity : "");
    n->c.identity = n->identity;
    take_name(n->name, (const uint8_t *)(c->name ? c->name : ""), c->name ? strlen(c->name) : 0);
    n->c.name = n->name;
    n->pads[0] = c->host_pads || c->guest_pads ? c->host_pads : 1;
    n->pads[1] = c->host_pads || c->guest_pads ? c->guest_pads : 2;
    n->delay = c->delay;
    n->window = c->window > 0 ? c->window : 8;
    n->rollback = c->rollback > 0 ? c->rollback : 30;
    n->check_every = c->check_every > 0 ? c->check_every : 60;
    if ((n->pads[0] & n->pads[1]) || !n->pads[0] || !n->pads[1] || ((n->pads[0] | n->pads[1]) >> MDRT_PADS) ||
        n->window + n->delay + n->rollback + 8 > RING / 2) {
        free(n);
        return NULL;
    }
    n->rng = c->seed ? c->seed : now(n) ^ (uint64_t)(uintptr_t)n;
    n->nonce = next_random(n);
    n->rtt_us = -1;
    n->period_us = 16684;                                 /* (NTSC until the console says) */
    n->host = c->role == MDRT_NET_HOST;
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
    free(n);
}

int mdrt_net_is_host(const mdrt_net *n) { return n->host; }
int mdrt_net_state(const mdrt_net *n)
{
    return n->phase == P_DONE ? n->result : n->phase == P_RUNNING ? MDRT_NET_GO : MDRT_NET_CONNECTING;
}
uint8_t mdrt_net_pads(const mdrt_net *n) { return my_pads(n); }
int mdrt_net_delay(const mdrt_net *n) { return n->delay; }
const char *mdrt_net_peer_name(const mdrt_net *n) { return n->peer_name; }
const char *mdrt_net_why(const mdrt_net *n) { return n->why; }
const void *mdrt_net_settings(const mdrt_net *n, size_t *size)
{
    *size = n->settings_size;
    return n->settings_size ? n->settings : NULL;
}

static void send_hello(mdrt_net *n)
{
    out o;
    begin(&o, T_HELLO);
    p16(&o, MDRT_NET_PROTOCOL); p8(&o, (uint32_t)n->c.role); p64(&o, n->nonce);
    size_t k = strlen(n->identity);
    p8(&o, (uint32_t)k); pbytes(&o, n->identity, k);
    k = strlen(n->name);
    p8(&o, (uint32_t)k); pbytes(&o, n->name, k);
    transmit(n, &o);
}

static void send_accept(mdrt_net *n)
{
    out o;
    begin(&o, T_ACCEPT);
    p64(&o, n->guest_nonce); p32(&o, n->session); p8(&o, n->pads[0]); p8(&o, n->pads[1]);
    p8(&o, (uint32_t)n->delay); p8(&o, (uint32_t)n->window); p8(&o, (uint32_t)n->rollback); p16(&o, (uint32_t)n->check_every);
    p32(&o, (uint32_t)n->settings_size);
    size_t k = strlen(n->name);
    p8(&o, (uint32_t)k); pbytes(&o, n->name, k);
    transmit(n, &o);
}

static void send_refuse(mdrt_net *n, uint64_t nonce, const char *why)
{
    out o;
    begin(&o, T_REFUSE);
    p64(&o, nonce);
    size_t k = strlen(why);
    p8(&o, (uint32_t)k); pbytes(&o, why, k);
    transmit(n, &o);
}

static void send_session(mdrt_net *n, int type)
{
    out o;
    begin(&o, type);
    p32(&o, n->session);
    transmit(n, &o);
}

static void send_ping(mdrt_net *n, int type, uint64_t t)
{
    out o;
    begin(&o, type);
    p32(&o, n->session); p64(&o, t);
    transmit(n, &o);
}

/* the settings' chunks the guest has not acknowledged, a few ahead */
static void send_chunks(mdrt_net *n)
{
    for (uint32_t i = n->chunks_have; i < n->chunks && i < n->chunks_have + CHUNKS_OUT; i++) {
        out o;
        size_t at = (size_t)i * CHUNK, k = n->settings_size - at < CHUNK ? n->settings_size - at : CHUNK;
        begin(&o, T_CHUNK);
        p32(&o, n->session); p32(&o, i); p16(&o, (uint32_t)k); pbytes(&o, n->settings + at, k);
        transmit(n, &o);
    }
    n->chunks_at = now(n);
}

/* a guest's HELLO, taken by this peer as the host */
static void host_guest(mdrt_net *n, uint64_t nonce, uint32_t protocol, const uint8_t *id, size_t idn, const uint8_t *name, size_t namen)
{
    if (n->phase == P_HOSTING || n->phase == P_RUNNING) {
        if (nonce == n->guest_nonce) send_accept(n);      /* (it did not hear the first) */
        else send_refuse(n, nonce, "the host is playing with someone else");
        return;
    }
    char why[160];
    if (protocol != MDRT_NET_PROTOCOL)
        snprintf(why, sizeof why, "another version of netplay (%u here, %u there)", MDRT_NET_PROTOCOL, (unsigned)protocol);
    else if (idn != strlen(n->identity) || memcmp(id, n->identity, idn))
        snprintf(why, sizeof why, "another game, build or settings (here: %.50s; there: %.*s)", n->identity, idn > 50 ? 50 : (int)idn, (const char *)id);
    else why[0] = 0;
    if (why[0]) {
        send_refuse(n, nonce, why);
        snprintf(n->why, sizeof n->why, "refused a player: %.160s", why);
        return;
    }
    n->host = 1;
    n->phase = P_HOSTING;
    n->guest_nonce = nonce;
    take_name(n->peer_name, name, namen);
    n->session = (uint32_t)next_random(n) | 1;
    n->chunks = (uint32_t)((n->settings_size + CHUNK - 1) / CHUNK);
    n->chunks_have = 0;
    n->heard_at = now(n);
    send_accept(n);
    send_chunks(n);
}

static void handle(mdrt_net *n, const uint8_t *b, size_t size);

/* everything waiting on the link */
static void receive(mdrt_net *n)
{
    uint8_t b[MDRT_NET_MTU + 64];
    for (int k = 0; k < 1000; k++) {
        int got = n->c.link.recv(n->c.link.user, b, sizeof b);
        if (got <= 0) break;
        handle(n, b, (size_t)got);
    }
}

/* pings for the round trip, a few a second */
static void ping(mdrt_net *n)
{
    uint64_t t = now(n);
    if (n->session && t - n->ping_at >= 250 * MS) { send_ping(n, T_PING, t); n->ping_at = t; }
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
        if (n->chunks_have < n->chunks && t - n->chunks_at >= 100 * MS) send_chunks(n);
        if (n->chunks_have == n->chunks && n->ready_heard) {
            send_session(n, T_START);
            n->phase = P_RUNNING;
        }
    }
    if (n->phase == P_GUEST) {
        if (n->chunks_have == n->chunks && t - n->ready_at >= 100 * MS) { send_session(n, T_READY); n->ready_at = t; }
        if (n->started && n->chunks_have == n->chunks) n->phase = P_RUNNING;
    }
    ping(n);
    return n->phase == P_RUNNING ? MDRT_NET_READY : MDRT_NET_CONNECTING;
}

/* ---- the frames ---------------------------------------------------------------------------------- */

static uint32_t slot(uint32_t f) { return f % RING; }

/* the buttons a frame is given: this peer's pads its own, the other's as known or guessed (the last
 * known, held) */
static void pad_source(void *user, uint16_t b[MDRT_PADS])
{
    mdrt_net *n = user;
    uint32_t f = (uint32_t)mdrt_frame_count(n->m) + 1;
    uint8_t mine = my_pads(n), theirs = their_pads(n);
    uint32_t known = f <= n->remote_newest ? f : n->remote_newest;
    for (int p = 0; p < MDRT_PADS; p++) {
        if (mine >> p & 1) b[p] = f <= n->local_newest ? n->local[slot(f)][p] : 0;
        else if (theirs >> p & 1) b[p] = n->remote_frame[slot(known)] == known ? n->remote[slot(known)][p] : 0;
        else b[p] = 0;
    }
    memcpy(n->used[slot(f)], b, sizeof n->used[0]);
    n->latched = f;
}

void mdrt_net_start(mdrt_net *n, mdrt *m)
{
    n->m = m;
    n->period_us = (uint32_t)((uint64_t)mdrt_frame_mclks(m) * 1000000u / mdrt_master_clock(m));
    /* frames 1 to delay + 1: no one's buttons yet (all known, as nothing) */
    n->local_newest = n->remote_newest = n->acked = (uint32_t)n->delay + 1;
    for (uint32_t f = 0; f <= n->remote_newest; f++) n->remote_frame[slot(f)] = f;
    n->logged = 0;
    n->frame = 0;
    n->arrived = 0;
    n->latched = 0;
    n->heard_at = now(n);
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

static void send_input(mdrt_net *n)
{
    out o;
    uint8_t mine = my_pads(n);
    uint32_t first = n->acked + 1, last = n->local_newest;
    if (last >= MAX_RESEND && first < last - MAX_RESEND + 1) first = last - MAX_RESEND + 1;
    uint32_t count = first <= last ? last - first + 1 : 0;
    begin(&o, T_INPUT);
    p32(&o, n->session); p32(&o, count ? first : 0); p8(&o, count); p8(&o, (uint32_t)count_bits(mine));
    for (uint32_t f = first; count && f <= last; f++)
        for (int p = 0; p < MDRT_PADS; p++)
            if (mine >> p & 1) p16(&o, n->local[slot(f)][p]);
    int adv = n->my_adv < -30000 ? -30000 : n->my_adv > 30000 ? 30000 : n->my_adv;
    p32(&o, n->remote_newest); p32(&o, n->frame); p16(&o, (uint32_t)(uint16_t)(int16_t)adv);
    /* the newest final check */
    uint32_t cf = 0;
    uint64_t ch = 0;
    for (int i = 0; i < CHECKS; i++)
        if (n->mine[i].final && n->mine[i].frame > cf) { cf = n->mine[i].frame; ch = n->mine[i].hash; }
    p32(&o, cf); p64(&o, ch);
    transmit(n, &o);
    n->input_at = now(n);
}

/* the other's buttons for frames from first: the guesses they show wrong */
static void take_inputs(mdrt_net *n, in *i, uint32_t first, uint32_t count, uint32_t npads)
{
    uint8_t theirs = their_pads(n);
    for (uint32_t k = 0; k < count; k++) {
        uint32_t f = first + k;
        uint16_t b[MDRT_PADS] = { 0 };
        for (int p = 0, j = 0; p < MDRT_PADS && (uint32_t)j < npads; p++)
            if (theirs >> p & 1) { b[p] = (uint16_t)g16(i); j++; }
        if (i->bad) return;
        if (f <= n->remote_newest || f >= n->remote_newest + RING / 2) continue;
        memcpy(n->remote[slot(f)], b, sizeof b);
        n->remote_frame[slot(f)] = f;
    }
    while (n->remote_frame[slot(n->remote_newest + 1)] == n->remote_newest + 1) {
        uint32_t f = ++n->remote_newest;
        if (f > n->latched) continue;
        for (int p = 0; p < MDRT_PADS; p++)               /* (a frame the console was given a guess for) */
            if ((theirs >> p & 1) && n->used[slot(f)][p] != n->remote[slot(f)][p]) {
                if (!n->wrong || f < n->wrong) n->wrong = f;
                break;
            }
    }
}

static void handle(mdrt_net *n, const uint8_t *b, size_t size)
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
        uint32_t namen = protocol == MDRT_NET_PROTOCOL ? g8(&i) : 0;   /* (an older protocol's has none: refused below) */
        const uint8_t *name = namen ? gbytes(&i, namen) : NULL;
        if (i.bad || n->phase == P_DONE || nonce == n->nonce) return;
        if (n->phase == P_GUEST) return;
        int host_it = n->c.role == MDRT_NET_HOST || (n->c.role == MDRT_NET_EITHER && (role == MDRT_NET_JOIN ||
                      (role == MDRT_NET_EITHER && n->nonce > nonce)));
        if (n->c.role == MDRT_NET_JOIN && role == MDRT_NET_JOIN) {
            send_refuse(n, nonce, "both are joining: one must host");
            end(n, MDRT_NET_REFUSED, "both are joining: one must host");
            return;
        }
        if (n->c.role == MDRT_NET_HOST && role == MDRT_NET_HOST) return;   /* (a host never says hello) */
        if (host_it) host_guest(n, nonce, protocol, id, idn, name, namen);
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
        uint32_t session = g32(&i), hp = g8(&i), gp = g8(&i), delay = g8(&i), window = g8(&i), rollback = g8(&i);
        uint32_t every = g16(&i), ssize = g32(&i), namen = g8(&i);
        const uint8_t *name = namen ? gbytes(&i, namen) : NULL;
        if (i.bad || !hp || !gp || (hp & gp) || ((hp | gp) >> MDRT_PADS) || delay > MDRT_NET_MAX_DELAY || !window || !rollback ||
            !every || window + delay + rollback + 8 > RING / 2 || ssize > MDRT_NET_MAX_SETTINGS) return;
        n->host = 0;
        n->session = session;
        take_name(n->peer_name, name, namen);
        n->pads[0] = (uint8_t)hp; n->pads[1] = (uint8_t)gp;
        n->delay = (int)delay; n->window = (int)window; n->rollback = (int)rollback; n->check_every = (int)every;
        free(n->settings);
        n->settings = ssize ? malloc(ssize) : NULL;
        n->chunks = (ssize + CHUNK - 1) / CHUNK;
        n->got = n->chunks ? calloc(n->chunks, 1) : NULL;
        if ((ssize && !n->settings) || (n->chunks && !n->got)) { end(n, MDRT_NET_ENDED, "out of memory"); return; }
        n->settings_size = ssize;
        n->chunks_have = 0;
        n->phase = P_GUEST;
        n->heard_at = t;
        return;
    }
    uint32_t session = g32(&i);
    if (i.bad || !n->session || session != n->session) return;
    n->heard_at = t;
    n->st.lost = 0;
    switch (type) {
    case T_CHUNK: {
        uint32_t idx = g32(&i), k = g16(&i);
        const uint8_t *d = gbytes(&i, k);
        if (!d || n->host || idx >= n->chunks || (size_t)idx * CHUNK + k > n->settings_size) return;
        if (!n->got[idx]) { memcpy(n->settings + (size_t)idx * CHUNK, d, k); n->got[idx] = 1; }
        while (n->chunks_have < n->chunks && n->got[n->chunks_have]) n->chunks_have++;
        out o;
        begin(&o, T_CHUNK_ACK); p32(&o, n->session); p32(&o, n->chunks_have);
        transmit(n, &o);
        break;
    }
    case T_CHUNK_ACK: {
        uint32_t have = g32(&i);
        if (!i.bad && n->host && have <= n->chunks && have > n->chunks_have) { n->chunks_have = have; send_chunks(n); }
        break;
    }
    case T_READY:
        if (n->host) { n->ready_heard = 1; if (n->phase == P_RUNNING) send_session(n, T_START); }
        break;
    case T_START:
        if (!n->host) n->started = 1;
        break;
    case T_INPUT: {
        uint32_t first = g32(&i), count = g8(&i), npads = g8(&i);
        if (i.bad || npads != (uint32_t)count_bits(their_pads(n))) return;
        if (!n->host) n->started = 1;
        if (!n->m) return;                                /* (buttons come again once it starts) */
        take_inputs(n, &i, first, count, npads);
        uint32_t ack = g32(&i), frame = g32(&i);
        int adv = (int16_t)g16(&i);
        uint32_t cf = g32(&i);
        uint64_t ch = g64(&i);
        if (i.bad) return;
        if (ack > n->acked && ack <= n->local_newest) n->acked = ack;
        if (frame >= n->their_frame) { n->their_frame = frame; n->their_frame_at = t; }
        n->their_adv = adv;
        n->adv_known = 1;
        if (cf) { n->theirs[(cf / (uint32_t)n->check_every) % CHECKS].frame = cf; n->theirs[(cf / (uint32_t)n->check_every) % CHECKS].hash = ch; }
        break;
    }
    case T_PING:
        send_ping(n, T_PONG, g64(&i));
        break;
    case T_PONG: {
        uint64_t sent = g64(&i);
        if (i.bad || sent > t) return;
        int sample = (int)(t - sent);
        n->rtt_us = n->rtt_us < 0 ? sample : (n->rtt_us * 7 + sample) / 8;
        break;
    }
    case T_BYE: {
        char why[64];
        snprintf(why, sizeof why, "%s left", n->peer_name[0] ? n->peer_name : "the other player");
        end(n, MDRT_NET_ENDED, why);
        break;
    }
    default: break;
    }
}

/* the frames whose buttons are all known, to the log */
static void write_log(mdrt_net *n)
{
    uint32_t upto = n->remote_newest < n->local_newest ? n->remote_newest : n->local_newest;
    if (!n->log) { n->logged = upto; return; }
    uint8_t mine = my_pads(n), theirs = their_pads(n);
    while (n->logged < upto) {
        uint32_t f = ++n->logged;
        char line[64];
        uint16_t b[MDRT_PADS];
        for (int p = 0; p < MDRT_PADS; p++) b[p] = (mine >> p & 1) ? n->local[slot(f)][p] : (theirs >> p & 1) ? n->remote[slot(f)][p] : 0;
        int k = snprintf(line, sizeof line, "%u %04x %04x %04x %04x\n", (unsigned)f, b[0], b[1], b[2], b[3]);
        n->log(n->log_user, line, (size_t)k);
    }
}

void mdrt_net_set_log(mdrt_net *n, mdrt_sink_fn sink, void *user) { n->log = sink; n->log_user = user; }

/* this peer's checks that are final, against the other's */
static void check_desync(mdrt_net *n)
{
    for (int k = 0; k < CHECKS; k++) {
        if (!n->mine[k].frame || n->mine[k].compared) continue;
        if (!n->mine[k].final && n->remote_newest >= n->mine[k].frame && (!n->wrong || n->wrong > n->mine[k].frame) &&
            (!n->replay_to || n->mine[k].frame <= n->frame))
            n->mine[k].final = 1;
        if (!n->mine[k].final || n->theirs[k].frame != n->mine[k].frame) continue;
        n->mine[k].compared = 1;
        if (n->theirs[k].hash == n->mine[k].hash) { n->st.checks++; continue; }
        char why[96];
        snprintf(why, sizeof why, "desync at frame %u: the two games differ", (unsigned)n->mine[k].frame);
        end(n, MDRT_NET_ENDED, why);
        return;
    }
}

/* how far ahead of the other this peer is, in sixteenths of a frame, averaged */
static void step_advantage(mdrt_net *n)
{
    if (!n->their_frame_at || n->rtt_us < 0) return;
    uint64_t since = now(n) - n->their_frame_at + (uint64_t)n->rtt_us / 2;
    int64_t est = (int64_t)n->their_frame * 16 + (int64_t)(since * 16 / n->period_us);
    int sample = (int)((int64_t)n->frame * 16 - est);
    n->my_adv = (n->my_adv * 15 + sample) / 16;
}

int mdrt_net_frame(mdrt_net *n, int resumable)
{
    if (n->phase == P_DONE) return n->result;
    if (!n->m || n->phase != P_RUNNING) return MDRT_NET_WAIT;
    uint32_t f = (uint32_t)mdrt_frame_count(n->m);
    if (!n->arrived || f != n->frame) {                   /* arriving at frame f's wait */
        n->arrived = 1;
        n->frame = f;
        if (n->replay_to && f >= n->replay_to) n->replay_to = 0;
        if (resumable) take_snapshot(n, f);
        while (n->local_newest < f + 1 + (uint32_t)n->delay) {   /* this peer's buttons, for frame f + 1 + delay */
            uint32_t g = ++n->local_newest;
            uint16_t held[MDRT_PADS] = { 0 }, b[MDRT_PADS] = { 0 };
            if (n->c.local_input && g == f + 1 + (uint32_t)n->delay) n->c.local_input(n->c.input_user, held);
            for (int p = 0, k = 0; p < MDRT_PADS; p++)
                if (my_pads(n) >> p & 1) b[p] = held[k++];
            memcpy(n->local[slot(g)], b, sizeof b);
        }
        int k = (int)((f / (uint32_t)n->check_every) % CHECKS);
        if (f % (uint32_t)n->check_every == 0 && f && !(n->mine[k].frame == f && n->mine[k].compared)) {
            n->mine[k].frame = f;
            n->mine[k].hash = mdrt_sim_hash(n->m);
            n->mine[k].final = n->mine[k].compared = 0;
        }
        if (!n->replay_to) { step_advantage(n); send_input(n); }
    }
    receive(n);
    if (n->phase == P_DONE) return n->result;
    uint64_t t = now(n);
    if (t - n->input_at >= 16 * MS) send_input(n);        /* (again, in case it was lost) */
    ping(n);
    write_log(n);
    n->st.rtt_ms = n->rtt_us < 0 ? -1 : (n->rtt_us + 500) / 1000;

    /* a guess shown wrong: the state from before it, the frames since played again */
    if (n->wrong && n->wrong <= n->latched) {
        uint32_t back_to = n->replay_to > f ? n->replay_to : f;
        snap *s = snapshot_before(n, n->wrong - 1);
        if (!s || mdrt_set_state(n->m, s->buf, s->len)) {
            char why[96];
            snprintf(why, sizeof why, "could not roll back to frame %u", (unsigned)n->wrong);
            end(n, MDRT_NET_ENDED, why);
            return n->result;
        }
        uint32_t replay = back_to - s->frame;
        mdrt_set_quiet(n->m, (int)replay);
        n->replay_to = back_to;
        n->latched = s->frame;
        n->frame = s->frame;
        n->wrong = 0;
        for (int i = 0; i < n->nsnaps; i++)               /* (the states after it were the guesses': gone, even where the */
            if (n->snaps[i].valid && n->snaps[i].frame > s->frame) n->snaps[i].valid = 0;   /* frames played again have no resumable point) */
        for (int k = 0; k < CHECKS; k++)
            if (n->mine[k].frame > s->frame && !n->mine[k].compared) n->mine[k].frame = 0;
        n->st.rollbacks++;
        n->st.replayed += replay;
        n->st.last_replayed = (int)replay;
        if ((int)replay > n->st.most_replayed) n->st.most_replayed = (int)replay;
        return MDRT_NET_RESTART;
    }
    check_desync(n);
    if (n->phase == P_DONE) return n->result;
    if (n->replay_to) return MDRT_NET_GO;                 /* (playing frames again: no waiting) */

    /* frame f + 1 may go: its buttons known, or guessed within the window and a state to roll back to */
    uint32_t next = f + 1;
    int wait = 0;
    if (next > n->remote_newest) {
        snap *s = snapshot_before(n, n->remote_newest);
        wait = next - n->remote_newest > (uint32_t)n->window || !s || next - s->frame > (uint32_t)n->rollback;
    }
    /* keeping in step: this peer gives a frame back when it runs ahead of the other */
    if (!wait && n->adv_known && f >= n->sync_at) {
        n->sync_at = f + 60;
        if ((n->my_adv - n->their_adv) / 2 >= 16) {
            n->hold_until = t + n->period_us;
            n->st.gave_back++;
        }
    }
    if (!wait && n->hold_until > t) wait = 1;
    if (wait) {
        n->st.waits++;
        if (t - n->heard_at > LOST_US) n->st.lost = 1;
        if (t - n->heard_at > GONE_US) { end(n, MDRT_NET_ENDED, "connection lost"); return n->result; }
        return MDRT_NET_WAIT;
    }
    return MDRT_NET_GO;
}

void mdrt_net_quit(mdrt_net *n)
{
    if (n->session && n->phase != P_DONE) for (int i = 0; i < 3; i++) send_session(n, T_BYE);
    end_with(n, MDRT_NET_ENDED, "this player left", 0);
}

void mdrt_net_stats_get(const mdrt_net *n, mdrt_net_stats *s)
{
    *s = n->st;
    s->frame = n->frame;
    s->remote_frame = n->their_frame;
    s->confirmed = n->remote_newest < n->local_newest ? n->remote_newest : n->local_newest;
    s->remote_has = n->acked;
    s->delay = n->delay;
}
