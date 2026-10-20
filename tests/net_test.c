/* Rollback netplay (mdrt/netplay.h) against itself: two peers in one process, each a console with a
 * test game (H-interrupts, HV counter reads as chance, the display off (drawing is quiet_test's), the clock moved, the pads read through the
 * I/O ports, sound), over a simulated network (latency with jitter, so packets come out of order;
 * some lost, some twice), each paced by its own clock (one a little slow, which the module must
 * keep in step). Their buttons come from scripts; a third console plays the same buttons alone.
 * The game has stretches without a resumable point, one longer than a rollback may reach. Every
 * frame either peer finally played must be the lone console's, bit for bit (the simulation hash),
 * and the session must have rolled back, checked for desyncs, and given frames back. Then: a game
 * made to differ on one peer ends the session as a desync; a guest of another game is refused,
 * two joiners refuse each other, two "either"s settle who hosts and the host's settings (15,000 or
 * 20,000 bytes, in chunks) reach the guest through loss, and so do 4 MB of them (rosters with their
 * art), each chunk sent about once (a lost one again at once, on three acknowledgements alike);
 * goodbye ends the other's session, and silence ends it as lost after 10 seconds (lost from 3).
 * Then two pads each (two players at each machine: the host pads 1 and 2, the guest 3 and 4):
 * every frame as a lone console's with all four pads' buttons. Then a game whose resumable points
 * depend on what it has played (as a game leaves its loop at a period's end): a guess shown wrong
 * can replay frames that have none where the guesses' had them, whose states must not be loaded.
 * Last, more machines (a host and up to three guests, the host passing each guest's buttons on to
 * the others): four with a player each, three with two and one and one, two with one and three
 * and with three and one, every frame of each as a lone console's; four, one leaving: every one's
 * session over, each told who left; a game for two that a third would join: refused. And the host
 * resetting the console mid-game (MDRT_NET_RESET_BIT in its buttons), a guest's reset bit too: every
 * frame of both as a lone console's that powers on again at the host's frame and not the guest's,
 * a rollback across the reset playing it again. */
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "mdrt/mdrt.h"
#include "mdrt/netplay.h"

#define FRAMES 900
#define MAXF (FRAMES + 200)
#define DELAY 2

static uint64_t clock_us;               /* the simulated time */
static const char *core;                /* the FM core (the fastest built in) */

/* ---- the network ----------------------------------------------------------------------------- */

typedef struct packet { uint64_t at; int from; size_t n; uint8_t b[MDRT_NET_MTU]; } packet;
typedef struct net_cond { uint32_t latency, jitter, loss, dup; } net_cond;   /* us, us, per mille, per mille */
static net_cond cond;
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

typedef struct peer peer;
struct peer {
    const char *name;
    int id;                             /* (the network's index for it: a host's link's for its guests) */
    peer *other;                        /* (a guest's host; with two, each the other) */
    packet *inbox;
    int ninbox, capinbox;
    int cut;                            /* its packets are lost (it has gone) */
    mdrt *m;
    mdrt_net *net;
    mdrt_net_config cfg;
    uint32_t seed;                      /* its players' scripts (seed, seed + 1 ... for its pads in turn) */
    int players;                        /* the pads it plays (0: 1) */
    int by_state;                       /* its resumable points depend on its game (state_resumable) */
    uint64_t period, next_at;
    int need_frame, state, ready;
    uint32_t perturb_at;                /* its game made to differ here (0: never) */
    int resets;                         /* the console powered on again (MDRT_NET_RESET) */
    uint64_t hash[MAXF];
    /* the game, a part of every state */
    struct game { uint32_t f, chance, hints, pads, n; uint64_t next, base; } g;
};

static peer *ids[8];                    /* the peers by id */
static int nids;

static void deliver(peer *to, int from, const void *b, size_t n)
{
    if (to->ninbox == to->capinbox) {
        to->capinbox = to->capinbox ? 2 * to->capinbox : 64;
        to->inbox = realloc(to->inbox, (size_t)to->capinbox * sizeof *to->inbox);
    }
    packet *p = &to->inbox[to->ninbox++];
    p->at = clock_us + cond.latency + (cond.jitter ? rnd() % cond.jitter : 0);
    p->from = from;
    p->n = n;
    memcpy(p->b, b, n);
}

static unsigned long chunks_sent;       /* (the settings' chunks put on the network) */
static int send_to_peer(peer *p, peer *to, const void *b, size_t n)
{
    if (n > 2 && ((const uint8_t *)b)[2] == 4) chunks_sent++;   /* (CHUNK) */
    if (p->cut || !to || n > MDRT_NET_MTU || rnd() % 1000 < cond.loss) return 0;
    deliver(to, p->id, b, n);
    if (rnd() % 1000 < cond.dup) deliver(to, p->id, b, n);
    return 0;
}
static int link_send(void *user, const void *b, size_t n) { peer *p = user; return send_to_peer(p, p->other, b, n); }
static int link_send_to(void *user, int to, const void *b, size_t n)
{
    peer *p = user;
    return to >= 0 && to < nids ? send_to_peer(p, ids[to], b, n) : -1;
}

/* the earliest packet come by now, and who sent it */
static int link_recv_from(void *user, void *buf, size_t size, int *from)
{
    peer *p = user;
    int best = -1;
    for (int i = 0; i < p->ninbox; i++)
        if (p->inbox[i].at <= clock_us && (best < 0 || p->inbox[i].at < p->inbox[best].at)) best = i;
    if (best < 0) return 0;
    packet k = p->inbox[best];
    p->inbox[best] = p->inbox[--p->ninbox];
    if (k.n > size) return -1;
    memcpy(buf, k.b, k.n);
    *from = k.from;
    return (int)k.n;
}
static int link_recv(void *user, void *buf, size_t size) { int from; return link_recv_from(user, buf, size, &from); }

static uint64_t link_ticks(void *user) { (void)user; return clock_us; }

/* ---- the game ---------------------------------------------------------------------------------- */

static size_t game_save(void *user, uint8_t *out) { peer *p = user; if (out) memcpy(out, &p->g, sizeof p->g); return sizeof p->g; }
static int game_load(void *user, const uint8_t *in, size_t size) { peer *p = user; if (size != sizeof p->g) return -1; memcpy(&p->g, in, size); return 0; }

static void tick(void *user, uint32_t until)
{
    peer *p = user;
    while (p->g.next < p->g.base + until) {
        uint32_t at = (uint32_t)(p->g.next - p->g.base);
        mdrt_ym_write(p->m, at, 0, 0x2A);
        mdrt_ym_write(p->m, at + 765, 1, (p->g.n++ / 8) & 1 ? 0xA0 : 0x60);
        p->g.next += 3600;
    }
}

static void hint(void *user)
{
    peer *p = user;
    mdrt_vdp *v = mdrt_get_vdp(p->m);
    p->g.hints++;
    mdrt_vdp_ctrl16(v, (uint16_t)(0x8700 | (p->g.hints & 15)));
    p->g.chance = p->g.chance * 31 + mdrt_vdp_hv16(v);
}

static uint16_t read_pad(mdrt *m, int port)
{
    mdrt_pads *p = mdrt_get_pads(m);
    uint32_t data = 0xA10003 + 2 * (uint32_t)port, ctrl = 0xA10009 + 2 * (uint32_t)port;
    mdrt_pad_write(p, ctrl, 0x40);
    mdrt_pad_write(p, data, 0x40);
    uint8_t hi = mdrt_pad_read(p, data);
    mdrt_pad_write(p, data, 0x00);
    uint8_t lo = mdrt_pad_read(p, data);
    return (uint16_t)(hi << 8 | lo);
}

/* a frame: the vertical blank's work, then the logic through the display, then the handshake */
static void game_frame(peer *p)
{
    mdrt *m = p->m;
    mdrt_vdp *v = mdrt_get_vdp(m);
    if (p->g.f == 0) {
        static const uint8_t regs[] = { 0x14, 0x04, 0x30, 0x3C, 0x07, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x1F, 0x00, 0x81, 0x37, 0x00, 0x02, 0x01 };
        for (int r = 0; r < (int)sizeof regs; r++) mdrt_vdp_ctrl16(v, (uint16_t)(0x8000 | r << 8 | regs[r]));
        mdrt_ym_write(m, 0, 0, 0x2B); mdrt_ym_write(m, 700, 1, 0x80);
    }
    p->g.pads = p->g.pads * 7 + read_pad(m, 0) + 3u * read_pad(m, 1);
    mdrt_vdp_ctrl32(v, 0x5C000003);
    mdrt_vdp_data16(v, (uint16_t)(p->g.f * 3 + (p->g.pads & 7)));
    if (p->g.pads & 0x10) mdrt_psg_port(m, 2000, (uint8_t)(0x80 | (p->g.pads & 15)));
    mdrt_vblank_end(m);
    for (int i = 0; i < 10 + (int)(p->g.pads % 13); i++) mdrt_charge(m, 1500);
    p->g.chance = p->g.chance * 31 + mdrt_vdp_hv16(v);
    mdrt_wait_line(m, 100 + (int)(p->g.chance % 80));
    p->g.chance = p->g.chance * 31 + mdrt_vdp_hv16(v);
    if (p->perturb_at && p->g.f == p->perturb_at) p->g.chance ^= 1;   /* (a game that differs) */
    mdrt_wait_vblank(m);
    p->g.base += mdrt_frame_mclks(m);
    p->g.f++;
}

/* a player's buttons, sampled at frame f: held for a few frames, then others */
static uint16_t script(uint32_t seed, uint32_t f)
{
    static const uint16_t bits[] = { MDRT_PAD_UP, MDRT_PAD_DOWN, MDRT_PAD_LEFT, MDRT_PAD_RIGHT, MDRT_PAD_A, MDRT_PAD_B, MDRT_PAD_C, MDRT_PAD_START };
    uint32_t h = (seed ^ (f / 5) * 0x9E3779B1u) * 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    uint16_t b = 0;
    for (int i = 0; i < 8; i++) if (h >> (3 * i) & 4) b |= bits[i];
    return b;
}

static uint32_t host_reset, guest_reset;  /* the frame whose buttons are sampled with the reset bit (0: none) */
static void local_input(void *user, uint16_t b[MDRT_PADS])
{
    peer *p = user;
    uint32_t f = (uint32_t)mdrt_frame_count(p->m);
    for (int k = 0; k < (p->players ? p->players : 1); k++) b[k] = script(p->seed + (uint32_t)k, f);
    if (f && f == (p->cfg.role == MDRT_NET_HOST ? host_reset : guest_reset)) b[0] |= MDRT_NET_RESET_BIT;
}

/* a resumable point at the top of most frames; none from 40 to 59 of each hundred, nor from 500 to 569 */
static int resumable(uint32_t f) { return !(f % 100 >= 40 && f % 100 < 60) && !(f >= 500 && f < 570); }
/* or as the game has played: a third of the frames without, by the buttons so far */
static int state_resumable(const peer *p) { return (p->g.pads >> 3) % 3 != 0; }

static mdrt *console(peer *p)
{
    mdrt_config c = { 0 };
    c.fm_core = core;
    mdrt *m = mdrt_new(&c);
    p->m = m;
    memset(&p->g, 0, sizeof p->g);
    mdrt_set_sound_tick(m, tick, p);
    mdrt_set_hint(m, hint, p);
    CHECK_EQ(mdrt_add_state_part(m, "GAME", game_save, game_load, p), 0);
    return m;
}

/* ---- the lone console: the same buttons ----------------------------------------------------------- */

static uint32_t ref_seed[MDRT_PADS];   /* each pad's script */
static uint8_t ref_used;                /* the pads played */
static void ref_pads(void *user, uint16_t b[MDRT_PADS])
{
    uint32_t f = (uint32_t)mdrt_frame_count(user) + 1;
    memset(b, 0, MDRT_PADS * sizeof *b);
    if (f < DELAY + 2) return;
    for (int k = 0; k < MDRT_PADS; k++) if (ref_used >> k & 1) b[k] = script(ref_seed[k], f - 1 - DELAY);
}

static uint64_t ref_hash[MAXF];
static uint32_t ref_reset;              /* the frame the lone console powers on again at (0: none) */
static void play_alone(void)
{
    static peer r;
    memset(&r, 0, sizeof r);
    console(&r);
    mdrt_set_pad_source(r.m, ref_pads, r.m);
    while (mdrt_frame_count(r.m) < MAXF - 1) {
        game_frame(&r);
        if (ref_reset && mdrt_frame_count(r.m) == ref_reset) { mdrt_power_on(r.m); memset(&r.g, 0, sizeof r.g); }   /* (the game from its power on) */
        ref_hash[mdrt_frame_count(r.m)] = mdrt_sim_hash(r.m);
    }
    mdrt_free(r.m);
}
/* the host pad 1, the guest pad 2 */
static void reference(uint32_t host_seed, uint32_t guest_seed)
{
    ref_seed[0] = host_seed; ref_seed[1] = guest_seed;
    ref_used = 0x3;
    play_alone();
}
/* the pads as a session gave them out: each peer's in turn its seed, seed + 1 ... */
static void reference_of(peer **ps, int n)
{
    ref_used = 0;
    for (int i = 0; i < n; i++) {
        uint8_t pads = mdrt_net_pads(ps[i]->net);
        uint32_t k = 0;
        for (int p = 0; p < MDRT_PADS; p++) if (pads >> p & 1) { ref_seed[p] = ps[i]->seed + k++; ref_used |= (uint8_t)(1u << p); }
    }
    play_alone();
}

/* ---- a session ---------------------------------------------------------------------------------- */

static void setup(peer *p, peer *other, const char *name, int role, uint32_t seed, uint64_t period)
{
    free(p->inbox);
    memset(p, 0, sizeof *p);
    p->id = -1;
    for (int i = 0; i < nids; i++) if (ids[i] == p) p->id = i;
    if (p->id < 0) { p->id = nids; ids[nids++] = p; }
    p->name = name;
    p->other = other;
    p->seed = seed;
    p->period = period;
    p->cfg = (mdrt_net_config){ .role = role, .identity = "net_test 1", .name = name, .delay = DELAY, .window = 8, .rollback = 30,
                                .check_every = 60, .seed = seed * 7919u + 1 };
    p->cfg.link = (mdrt_net_link){ .user = p, .send = link_send, .recv = link_recv, .ticks = link_ticks };
    if (role == MDRT_NET_HOST) { p->cfg.link.send_to = link_send_to; p->cfg.link.recv_from = link_recv_from; }   /* (a host hears several) */
    p->cfg.local_input = local_input;
    p->cfg.input_user = p;
}

/* a peer's turn, when its time has come: frames while it may play them (a replay all at once) */
static void turn(peer *p, uint32_t until)
{
    if (p->state != MDRT_NET_GO && p->state != MDRT_NET_WAIT) return;
    for (int guard = 0; guard < 1000; guard++) {
        int idle = p->need_frame && mdrt_frame_count(p->m) >= until && !mdrt_quiet(p->m);
        if (p->need_frame && !idle) {                     /* (at the end it waits there, still heard and still rolling back) */
            game_frame(p);
            p->need_frame = 0;
        }
        uint32_t f = (uint32_t)mdrt_frame_count(p->m);
        int r = mdrt_net_frame(p->net, p->by_state ? state_resumable(p) : resumable(f));
        if (r == MDRT_NET_RESET) { memset(&p->g, 0, sizeof p->g); r = MDRT_NET_GO; p->resets++; }   /* (powered on again: the game from its power on) */
        if (f < MAXF) p->hash[f] = mdrt_sim_hash(p->m);
        p->state = r;
        if (r == MDRT_NET_WAIT || (idle && r == MDRT_NET_GO)) { p->next_at = clock_us + 1000; return; }
        if (r != MDRT_NET_GO && r != MDRT_NET_RESTART) return;
        p->need_frame = 1;
        if (r == MDRT_NET_GO && !mdrt_quiet(p->m)) {      /* (paced; a replay goes at once) */
            p->next_at = (p->next_at + p->period > clock_us ? p->next_at : clock_us) + p->period;
            return;
        }
    }
}

/* connect them all; each powers on when ready (and, powered on, keeps hearing the others as a
 * client at its waits does, without playing on: a START lost on the way comes again) */
static int join_n(peer **ps, int n, uint64_t limit_us)
{
    for (int i = 0; i < n; i++) { ps[i]->net = mdrt_net_new(&ps[i]->cfg); ps[i]->state = MDRT_NET_CONNECTING; }
    for (uint64_t end = clock_us + limit_us; clock_us < end; clock_us += 1000) {
        int connecting = 0;
        for (int i = 0; i < n; i++) {
            peer *p = ps[i];
            if (p->state == MDRT_NET_GO || p->state == MDRT_NET_WAIT) { if (clock_us >= p->next_at) turn(p, 0); continue; }
            if (p->state != MDRT_NET_CONNECTING) continue;
            p->state = mdrt_net_connect(p->net);
            if (p->state == MDRT_NET_READY) {
                console(p);
                mdrt_net_start(p->net, p->m);
                p->state = MDRT_NET_GO;
                p->need_frame = 1;
                p->next_at = clock_us;
            }
            connecting |= p->state == MDRT_NET_CONNECTING;
        }
        if (!connecting) break;
    }
    int all = 1;
    for (int i = 0; i < n; i++) all &= ps[i]->state == MDRT_NET_GO;
    return all;
}
static int join_up(peer *a, peer *b, uint64_t limit_us) { peer *ps[2] = { a, b }; return join_n(ps, 2, limit_us); }

static void play_n(peer **ps, int n, uint32_t until, uint64_t limit_us)
{
    for (uint64_t end = clock_us + limit_us; clock_us < end; clock_us += 250) {
        for (int i = 0; i < n; i++)
            if ((ps[i]->state == MDRT_NET_GO || ps[i]->state == MDRT_NET_WAIT) && clock_us >= ps[i]->next_at) turn(ps[i], until);
        int done = 1, over = 1;
        for (int i = 0; i < n; i++) {
            peer *p = ps[i];
            if (p->state != MDRT_NET_ENDED && p->state != MDRT_NET_REFUSED) over = 0;
            if (p->state != MDRT_NET_GO && p->state != MDRT_NET_WAIT) continue;
            mdrt_net_stats st;
            mdrt_net_stats_get(p->net, &st);
            if (mdrt_frame_count(p->m) < until || !p->need_frame || mdrt_quiet(p->m) || st.confirmed < until) done = 0;
        }
        if (done || over) break;
    }
}
static void play(peer *a, peer *b, uint32_t until, uint64_t limit_us) { peer *ps[2] = { a, b }; play_n(ps, 2, until, limit_us); }

static void finish_n(peer **ps, int n)
{
    for (int i = 0; i < n; i++) {
        peer *p = ps[i];
        mdrt_net_free(p->net); p->net = NULL;
        if (p->m) mdrt_free(p->m);
        p->m = NULL;
        free(p->inbox); p->inbox = NULL; p->ninbox = p->capinbox = 0;
    }
}
static void finish(peer *a, peer *b) { peer *ps[2] = { a, b }; finish_n(ps, 2); }

static void show(const peer *p)
{
    mdrt_net_stats s;
    mdrt_net_stats_get(p->net, &s);
    printf("    %s: frame %u, %ld rollbacks (%ld frames, at most %d), %ld waits, %ld frames given back, %ld checks, rtt %d ms\n",
           p->name, (unsigned)s.frame, s.rollbacks, s.replayed, s.most_replayed, s.waits, s.gave_back, s.checks, s.rtt_ms);
}

static peer A, B, C, D;

static void session(void)
{
    reference(101, 202);
    cond = (net_cond){ .latency = 35000, .jitter = 12000, .loss = 50, .dup = 10 };
    setup(&A, &B, "host", MDRT_NET_HOST, 101, 16684);
    setup(&B, &A, "guest", MDRT_NET_JOIN, 202, 16850);  /* (its clock 1% slow) */
    CHECK(join_up(&A, &B, 5000000));
    play(&A, &B, FRAMES, 120000000);
    int same[2] = { 0, 0 };
    peer *ps[2] = { &A, &B };
    for (int i = 0; i < 2; i++)
        for (uint32_t f = 1; f <= FRAMES; f++) same[i] += ps[i]->hash[f] == ref_hash[f];
    mdrt_net_stats sa, sb;
    mdrt_net_stats_get(A.net, &sa);
    mdrt_net_stats_get(B.net, &sb);
    printf("  a session of %d frames, 35-47 ms each way, 5%% lost, 1%% twice, the guest's clock 1%% slow:\n", FRAMES);
    show(&A); show(&B);
    printf("    every frame as the lone console's: host %d, guest %d of %d\n", same[0], same[1], FRAMES);
    CHECK_EQ(same[0], FRAMES);
    CHECK_EQ(same[1], FRAMES);
    CHECK(sa.rollbacks > 10 && sb.rollbacks > 10);
    CHECK(sa.checks >= FRAMES / 60 - 3 && sb.checks >= FRAMES / 60 - 3);
    CHECK(sa.gave_back > 0);
    CHECK(sa.rtt_ms >= 70 && sa.rtt_ms <= 120);
    CHECK_EQ(mdrt_net_pads(A.net), 1); CHECK_EQ(mdrt_net_pads(B.net), 2);
    printf("    names: the host sees \"%s\", the guest \"%s\"\n", mdrt_net_peer_name(A.net), mdrt_net_peer_name(B.net));
    CHECK(!strcmp(mdrt_net_peer_name(A.net), "guest") && !strcmp(mdrt_net_peer_name(B.net), "host"));
    finish(&A, &B);
}

/* the host resets the console at its buttons sampled at frame 400 (so frame 400 + 1 + DELAY), a
 * guest's reset bit at 250 is not one; with the other's buttons 35-47 ms late a peer rolls back
 * across the reset frame, the console powered on again as it plays that frame again */
static void resetting(void)
{
    ref_reset = 400 + 1 + DELAY;
    reference(101, 202);
    ref_reset = 0;
    host_reset = 400;
    guest_reset = 250;
    cond = (net_cond){ .latency = 35000, .jitter = 12000, .loss = 50, .dup = 10 };
    setup(&A, &B, "host", MDRT_NET_HOST, 101, 16684);
    setup(&B, &A, "guest", MDRT_NET_JOIN, 202, 16850);
    CHECK(join_up(&A, &B, 5000000));
    play(&A, &B, FRAMES, 120000000);
    int same[2] = { 0, 0 };
    peer *ps[2] = { &A, &B };
    for (int i = 0; i < 2; i++)
        for (uint32_t f = 1; f <= FRAMES; f++) same[i] += ps[i]->hash[f] == ref_hash[f];
    mdrt_net_stats sa, sb;
    mdrt_net_stats_get(A.net, &sa);
    mdrt_net_stats_get(B.net, &sb);
    printf("  the host resetting the console at frame %d (a guest's reset bit ignored):\n", 400 + 1 + DELAY);
    show(&A); show(&B);
    printf("    powered on again: host %d time(s), guest %d (the session's count %ld and %ld); every frame as the lone console's that\n"
           "    powered on there: host %d, guest %d of %d\n", A.resets, B.resets, sa.resets, sb.resets, same[0], same[1], FRAMES);
    CHECK_EQ(same[0], FRAMES);
    CHECK_EQ(same[1], FRAMES);
    CHECK(A.resets >= 1 && B.resets >= 1);
    CHECK_EQ(sa.resets, A.resets);
    CHECK(A.resets + B.resets >= 3);                      /* (one powered on again as it played the reset frame again) */
    host_reset = guest_reset = 0;
    finish(&A, &B);
}

/* one peer's game differs from frame 300: the session ends as a desync on both */
static void desync(void)
{
    cond = (net_cond){ .latency = 20000, .jitter = 5000 };
    setup(&A, &B, "host", MDRT_NET_HOST, 101, 16684);
    setup(&B, &A, "guest", MDRT_NET_JOIN, 202, 16684);
    B.perturb_at = 300;
    CHECK(join_up(&A, &B, 5000000));
    play(&A, &B, FRAMES, 60000000);
    printf("  a game made to differ at frame 300: host \"%s\", guest \"%s\"\n", mdrt_net_why(A.net), mdrt_net_why(B.net));
    CHECK_EQ(A.state, MDRT_NET_ENDED); CHECK_EQ(B.state, MDRT_NET_ENDED);
    CHECK(strstr(mdrt_net_why(A.net), "desync at frame 360") != NULL);
    finish(&A, &B);
}

static void protocol(void)
{
    cond = (net_cond){ .latency = 10000, .jitter = 4000, .loss = 100 };
    /* another game */
    setup(&A, &B, "host", MDRT_NET_HOST, 1, 16684);
    setup(&B, &A, "guest", MDRT_NET_JOIN, 2, 16684);
    B.cfg.identity = "another game";
    CHECK(!join_up(&A, &B, 3000000));
    printf("  another game: the guest \"%s\"; the host \"%s\"\n", mdrt_net_why(B.net), mdrt_net_why(A.net));
    CHECK_EQ(B.state, MDRT_NET_REFUSED); CHECK_EQ(A.state, MDRT_NET_CONNECTING);
    CHECK(strstr(mdrt_net_why(B.net), "another game") != NULL && strstr(mdrt_net_why(A.net), "refused a player") != NULL);
    finish(&A, &B);
    /* two joiners */
    setup(&A, &B, "a", MDRT_NET_JOIN, 1, 16684);
    setup(&B, &A, "b", MDRT_NET_JOIN, 2, 16684);
    CHECK(!join_up(&A, &B, 3000000));
    CHECK(A.state == MDRT_NET_REFUSED || B.state == MDRT_NET_REFUSED);
    CHECK(A.state != MDRT_NET_READY && B.state != MDRT_NET_READY);
    finish(&A, &B);
    /* two eithers: one hosts, its settings reach the other */
    static uint8_t sa[20000], sb[15000];
    for (size_t i = 0; i < sizeof sa; i++) sa[i] = (uint8_t)(i * 7 + 3);
    for (size_t i = 0; i < sizeof sb; i++) sb[i] = (uint8_t)(i * 13 + 5);
    setup(&A, &B, "a", MDRT_NET_EITHER, 1, 16684);
    setup(&B, &A, "b", MDRT_NET_EITHER, 2, 16684);
    A.cfg.settings = sa; A.cfg.settings_size = sizeof sa;
    B.cfg.settings = sb; B.cfg.settings_size = sizeof sb;
    CHECK(join_up(&A, &B, 5000000));
    peer *host = mdrt_net_is_host(A.net) ? &A : &B, *guest = host == &A ? &B : &A;
    CHECK(mdrt_net_is_host(A.net) != mdrt_net_is_host(B.net));
    size_t n;
    const void *got = mdrt_net_settings(guest->net, &n);
    const uint8_t *want = host == &A ? sa : sb;
    size_t wn = host == &A ? sizeof sa : sizeof sb;
    printf("  two eithers: %s hosts; its %zu bytes of settings reach %s: %s\n", host->name, wn, guest->name,
           n == wn && got && !memcmp(got, want, n) ? "whole" : "NOT whole");
    CHECK(n == wn && got && !memcmp(got, want, n));
    finish(&A, &B);
    /* rosters' worth of settings, 4 MB, through the same loss */
    enum { BIG = 4u << 20 };
    uint8_t *big = malloc(BIG);
    for (size_t i = 0; big && i < BIG; i++) big[i] = (uint8_t)(i * 2654435761u >> 13);
    setup(&A, &B, "a", MDRT_NET_HOST, 1, 16684);
    setup(&B, &A, "b", MDRT_NET_JOIN, 2, 16684);
    A.cfg.settings = big; A.cfg.settings_size = BIG;
    net_cond keep = cond;
    cond = (net_cond){ .latency = 35000, .jitter = 12000, .loss = 50, .dup = 10 };
    chunks_sent = 0;
    uint64_t t0 = clock_us;
    CHECK(big && join_up(&A, &B, 120000000));
    got = mdrt_net_settings(B.net, &n);
    unsigned long chunks = (BIG + 1023) / 1024;
    printf("  4 MB of settings: whole %s, in %.1f s (35-47 ms each way, 5%% lost), %lu chunks sent for %lu\n",
           n == BIG && got && big && !memcmp(got, big, n) ? "yes" : "NO", (double)(clock_us - t0) / 1e6, chunks_sent, chunks);
    CHECK(n == BIG && got && big && !memcmp(got, big, n));
    CHECK(chunks_sent < chunks + chunks / 5);             /* (the lost ones again, and little more) */
    cond = keep;
    A.cfg.settings_size = MDRT_NET_MAX_SETTINGS + 1;      /* (more than a guest takes: refused at once) */
    CHECK(mdrt_net_new(&A.cfg) == NULL);
    A.cfg.settings = NULL; A.cfg.settings_size = 0;
    free(big);
    finish(&A, &B);
    /* goodbye: two eithers again */
    setup(&A, &B, "a", MDRT_NET_EITHER, 1, 16684);
    setup(&B, &A, "b", MDRT_NET_EITHER, 2, 16684);
    CHECK(join_up(&A, &B, 5000000));
    host = mdrt_net_is_host(A.net) ? &A : &B;
    guest = host == &A ? &B : &A;
    /* goodbye */
    play(&A, &B, 120, 10000000);
    mdrt_net_quit(guest->net);
    play(&A, &B, 1000, 2000000);
    printf("  goodbye: the host \"%s\"\n", mdrt_net_why(host->net));
    CHECK_EQ(host->state, MDRT_NET_ENDED);
    char left[64];
    snprintf(left, sizeof left, "%s left", guest->name);
    CHECK(!strcmp(mdrt_net_why(host->net), left));
    finish(&A, &B);
    /* silence */
    cond = (net_cond){ .latency = 10000, .jitter = 4000 };
    setup(&A, &B, "host", MDRT_NET_HOST, 1, 16684);
    setup(&B, &A, "guest", MDRT_NET_JOIN, 2, 16684);
    CHECK(join_up(&A, &B, 3000000));
    play(&A, &B, 120, 10000000);
    B.cut = 1;
    B.state = MDRT_NET_ENDED;                             /* (gone without a word) */
    play(&A, &B, 2000, 4000000);
    mdrt_net_stats s;
    mdrt_net_stats_get(A.net, &s);
    CHECK_EQ(s.lost, 1); CHECK_EQ(A.state, MDRT_NET_WAIT);
    play(&A, &B, 2000, 8000000);
    printf("  silence: lost after 3 s, then \"%s\"\n", mdrt_net_why(A.net));
    CHECK_EQ(A.state, MDRT_NET_ENDED);
    CHECK(strstr(mdrt_net_why(A.net), "connection lost") != NULL);
    finish(&A, &B);
}

/* two pads each: the host 1 and 2, the guest 3 and 4 (the pads' buttons are in the simulation's hash) */
static void two_each(void)
{
    ref_seed[0] = 303; ref_seed[1] = 304; ref_seed[2] = 404; ref_seed[3] = 405;
    ref_used = 0xF;
    play_alone();
    cond = (net_cond){ .latency = 30000, .jitter = 10000, .loss = 50, .dup = 10 };
    setup(&A, &B, "host", MDRT_NET_HOST, 303, 16684);
    setup(&B, &A, "guest", MDRT_NET_JOIN, 404, 16700);
    A.players = B.players = A.cfg.players = B.cfg.players = 2;
    A.cfg.players_total = 4;
    CHECK(join_up(&A, &B, 5000000));
    play(&A, &B, FRAMES, 120000000);
    int same[2] = { 0, 0 };
    peer *ps[2] = { &A, &B };
    for (int i = 0; i < 2; i++)
        for (uint32_t f = 1; f <= FRAMES; f++) same[i] += ps[i]->hash[f] == ref_hash[f];
    printf("  two pads each (the host 1 and 2, the guest 3 and 4): pads %X and %X; every frame as the lone console's: host %d, guest %d of %d\n",
           mdrt_net_pads(A.net), mdrt_net_pads(B.net), same[0], same[1], FRAMES);
    CHECK_EQ(mdrt_net_pads(A.net), 0x3);
    CHECK_EQ(mdrt_net_pads(B.net), 0xC);
    CHECK_EQ(same[0], FRAMES);
    CHECK_EQ(same[1], FRAMES);
    finish(&A, &B);
}

/* resumable points by the game's state: a replay may have none where the guesses' frames had them */
static void by_state(void)
{
    reference(505, 606);
    cond = (net_cond){ .latency = 40000, .jitter = 15000, .loss = 50, .dup = 10 };
    setup(&A, &B, "host", MDRT_NET_HOST, 505, 16684);
    setup(&B, &A, "guest", MDRT_NET_JOIN, 606, 16700);
    A.by_state = B.by_state = 1;
    CHECK(join_up(&A, &B, 5000000));
    play(&A, &B, FRAMES, 120000000);
    int same[2] = { 0, 0 };
    peer *ps[2] = { &A, &B };
    for (int i = 0; i < 2; i++)
        for (uint32_t f = 1; f <= FRAMES; f++) same[i] += ps[i]->hash[f] == ref_hash[f];
    mdrt_net_stats sa, sb;
    mdrt_net_stats_get(A.net, &sa);
    mdrt_net_stats_get(B.net, &sb);
    printf("  resumable points by the game's state: %ld and %ld rollbacks; every frame as the lone console's: host %d, guest %d of %d\n",
           sa.rollbacks, sb.rollbacks, same[0], same[1], FRAMES);
    CHECK(sa.rollbacks > 10 && sb.rollbacks > 10);
    CHECK_EQ(same[0], FRAMES);
    CHECK_EQ(same[1], FRAMES);
    finish(&A, &B);
}

/* ---- more machines ---------------------------------------------------------------------------- */

static int bits(uint8_t v) { int k = 0; for (; v; v &= (uint8_t)(v - 1)) k++; return k; }

/* n machines, the host first, with players[i] pads each: the pads given out in turn (every one,
 * once, the host's first), and every frame each machine played the lone console's with all of
 * their buttons */
static void machines(const char *what, const int *players, int n, uint32_t seed0)
{
    static const char *const names[4] = { "host", "b", "c", "d" };
    peer *ps[4] = { &A, &B, &C, &D };
    cond = (net_cond){ .latency = 30000, .jitter = 15000, .loss = 50, .dup = 10 };
    int total = 0;
    for (int i = 0; i < n; i++) total += players[i];
    for (int i = 0; i < n; i++) {
        setup(ps[i], i ? &A : &B, names[i], i ? MDRT_NET_JOIN : MDRT_NET_HOST, seed0 + 100u * (uint32_t)i, (uint64_t)(16684 + 20 * i));
        ps[i]->players = ps[i]->cfg.players = players[i];
    }
    A.cfg.players_total = total;
    CHECK(join_n(ps, n, 10000000));
    uint8_t all = 0;
    int given = 1;
    for (int i = 0; i < n; i++) {
        uint8_t pads = ps[i]->net ? mdrt_net_pads(ps[i]->net) : 0;
        given &= !(pads & all) && bits(pads) == players[i];
        all |= pads;
    }
    CHECK(given && all == (1u << total) - 1 && (mdrt_net_pads(A.net) & 1));
    CHECK_EQ(mdrt_net_peers(A.net), n);
    CHECK(n < 2 || !strcmp(mdrt_net_name_of(B.net, 0), "host"));
    reference_of(ps, n);
    play_n(ps, n, FRAMES, 200000000);
    int same[4] = { 0 }, ok = 1;
    long rollbacks = 0;
    for (int i = 0; i < n; i++) {
        for (uint32_t f = 1; f <= FRAMES; f++) same[i] += ps[i]->hash[f] == ref_hash[f];
        ok &= same[i] == FRAMES;
        mdrt_net_stats st;
        mdrt_net_stats_get(ps[i]->net, &st);
        rollbacks += st.rollbacks;
    }
    printf("  %s: pads %X %X %X %X, %ld rollbacks in all; every frame as the lone console's: %d %d %d %d of %d\n", what,
           mdrt_net_pads(A.net), n > 1 ? mdrt_net_pads(B.net) : 0, n > 2 ? mdrt_net_pads(C.net) : 0, n > 3 ? mdrt_net_pads(D.net) : 0,
           rollbacks, same[0], same[1], same[2], same[3], FRAMES);
    CHECK(ok);
    CHECK(rollbacks > 10);
    finish_n(ps, n);
}

/* four machines, one leaves: the session over on every one, each told who left */
static void leaving(void)
{
    static const int one[4] = { 1, 1, 1, 1 };
    peer *ps[4] = { &A, &B, &C, &D };
    static const char *const names[4] = { "host", "b", "c", "d" };
    cond = (net_cond){ .latency = 20000, .jitter = 5000 };
    for (int i = 0; i < 4; i++) {
        setup(ps[i], i ? &A : &B, names[i], i ? MDRT_NET_JOIN : MDRT_NET_HOST, 700u + (uint32_t)i, 16684);
        ps[i]->cfg.players = one[i];
    }
    A.cfg.players_total = 4;
    CHECK(join_n(ps, 4, 10000000));
    play_n(ps, 4, 200, 20000000);
    mdrt_net_quit(C.net);
    C.state = MDRT_NET_ENDED;
    play_n(ps, 4, 2000, 3000000);
    printf("  four machines, c leaves: the host \"%s\", b \"%s\", d \"%s\"\n", mdrt_net_why(A.net), mdrt_net_why(B.net), mdrt_net_why(D.net));
    CHECK(A.state == MDRT_NET_ENDED && B.state == MDRT_NET_ENDED && D.state == MDRT_NET_ENDED);
    CHECK(!strcmp(mdrt_net_why(A.net), "c left") && !strcmp(mdrt_net_why(B.net), "c left") && !strcmp(mdrt_net_why(D.net), "c left"));
    finish_n(ps, 4);
}

/* a host for two, two guests: one plays, the other is refused */
static void full(void)
{
    peer *ps[3] = { &A, &B, &C };
    cond = (net_cond){ .latency = 10000, .jitter = 4000 };
    setup(&A, &B, "host", MDRT_NET_HOST, 1, 16684);
    setup(&B, &A, "b", MDRT_NET_JOIN, 2, 16684);
    setup(&C, &A, "c", MDRT_NET_JOIN, 3, 16684);
    CHECK(!join_n(ps, 3, 5000000));
    peer *out = B.state == MDRT_NET_REFUSED ? &B : &C, *in = out == &B ? &C : &B;
    printf("  a game for two, three machines: %s plays, %s \"%s\"\n", in->name, out->name, mdrt_net_why(out->net));
    CHECK(A.state == MDRT_NET_GO && in->state == MDRT_NET_GO && out->state == MDRT_NET_REFUSED);
    CHECK(strstr(mdrt_net_why(out->net), "full") || strstr(mdrt_net_why(out->net), "begun"));
    finish_n(ps, 3);
}

int main(void)
{
    for (int i = 0; mdrt_fm_cores(i); i++)
        if (!strcmp(mdrt_fm_cores(i), "ymfm")) core = "ymfm";
    session();
    resetting();
    desync();
    protocol();
    two_each();
    by_state();
    static const int four[4] = { 1, 1, 1, 1 }, two_one_one[3] = { 2, 1, 1 }, one_three[2] = { 1, 3 }, three_one[2] = { 3, 1 };
    machines("four machines, one player each", four, 4, 800);
    machines("three machines, the host's two players and one each", two_one_one, 3, 900);
    machines("two machines, one player and three", one_three, 2, 1000);
    machines("two machines, three players and one", three_one, 2, 1100);
    leaving();
    full();
    return check_done("net_test");
}
