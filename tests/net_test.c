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
 * 20,000 bytes, in chunks) reach the guest through loss, and so do 4 MB of them (a league with its
 * art), each chunk sent about once (a lost one again at once, on three acknowledgements alike);
 * goodbye ends the other's session, and silence ends it as lost after 10 seconds (lost from 3). */
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

typedef struct packet { uint64_t at; size_t n; uint8_t b[MDRT_NET_MTU]; } packet;
typedef struct net_cond { uint32_t latency, jitter, loss, dup; } net_cond;   /* us, us, per mille, per mille */
static net_cond cond;
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

typedef struct peer peer;
struct peer {
    const char *name;
    peer *other;
    packet *inbox;
    int ninbox, capinbox;
    int cut;                            /* its packets are lost (it has gone) */
    mdrt *m;
    mdrt_net *net;
    mdrt_net_config cfg;
    uint32_t seed;                      /* its player's script */
    uint64_t period, next_at;
    int need_frame, state, ready;
    uint32_t perturb_at;                /* its game made to differ here (0: never) */
    uint64_t hash[MAXF];
    /* the game, a part of every state */
    struct game { uint32_t f, chance, hints, pads, n; uint64_t next, base; } g;
};

static void deliver(peer *to, const void *b, size_t n)
{
    if (to->ninbox == to->capinbox) {
        to->capinbox = to->capinbox ? 2 * to->capinbox : 64;
        to->inbox = realloc(to->inbox, (size_t)to->capinbox * sizeof *to->inbox);
    }
    packet *p = &to->inbox[to->ninbox++];
    p->at = clock_us + cond.latency + (cond.jitter ? rnd() % cond.jitter : 0);
    p->n = n;
    memcpy(p->b, b, n);
}

static unsigned long chunks_sent;       /* (the settings' chunks put on the network) */
static int link_send(void *user, const void *b, size_t n)
{
    peer *p = user;
    if (n > 2 && ((const uint8_t *)b)[2] == 4) chunks_sent++;   /* (CHUNK) */
    if (p->cut || n > MDRT_NET_MTU || rnd() % 1000 < cond.loss) return 0;
    deliver(p->other, b, n);
    if (rnd() % 1000 < cond.dup) deliver(p->other, b, n);
    return 0;
}

/* the earliest packet come by now */
static int link_recv(void *user, void *buf, size_t size)
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
    return (int)k.n;
}

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

static void local_input(void *user, uint16_t b[MDRT_PADS])
{
    peer *p = user;
    b[0] = script(p->seed, (uint32_t)mdrt_frame_count(p->m));
}

/* a resumable point at the top of most frames; none from 40 to 59 of each hundred, nor from 500 to 569 */
static int resumable(uint32_t f) { return !(f % 100 >= 40 && f % 100 < 60) && !(f >= 500 && f < 570); }

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

static uint32_t ref_seed[2];
static void ref_pads(void *user, uint16_t b[MDRT_PADS])
{
    uint32_t f = (uint32_t)mdrt_frame_count(user) + 1;
    memset(b, 0, MDRT_PADS * sizeof *b);
    if (f >= DELAY + 2) { b[0] = script(ref_seed[0], f - 1 - DELAY); b[1] = script(ref_seed[1], f - 1 - DELAY); }
}

static uint64_t ref_hash[MAXF];
static void reference(uint32_t host_seed, uint32_t guest_seed)
{
    static peer r;
    memset(&r, 0, sizeof r);
    ref_seed[0] = host_seed; ref_seed[1] = guest_seed;
    console(&r);
    mdrt_set_pad_source(r.m, ref_pads, r.m);
    while (mdrt_frame_count(r.m) < MAXF - 1) {
        game_frame(&r);
        ref_hash[mdrt_frame_count(r.m)] = mdrt_sim_hash(r.m);
    }
    mdrt_free(r.m);
}

/* ---- a session ---------------------------------------------------------------------------------- */

static void setup(peer *p, peer *other, const char *name, int role, uint32_t seed, uint64_t period)
{
    free(p->inbox);
    memset(p, 0, sizeof *p);
    p->name = name;
    p->other = other;
    p->seed = seed;
    p->period = period;
    p->cfg = (mdrt_net_config){ .role = role, .identity = "net_test 1", .name = name, .delay = DELAY, .window = 8, .rollback = 30,
                                .check_every = 60, .seed = seed * 7919u + 1 };
    p->cfg.link = (mdrt_net_link){ .user = p, .send = link_send, .recv = link_recv, .ticks = link_ticks };
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
        int r = mdrt_net_frame(p->net, resumable(f));
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

/* connect both; each powers on when ready (and, powered on, keeps hearing the other as a client
 * at its waits does, without playing on: a START lost on the way comes again) */
static int join_up(peer *a, peer *b, uint64_t limit_us)
{
    peer *ps[2] = { a, b };
    for (int i = 0; i < 2; i++) { ps[i]->net = mdrt_net_new(&ps[i]->cfg); ps[i]->state = MDRT_NET_CONNECTING; }
    for (uint64_t end = clock_us + limit_us; clock_us < end; clock_us += 1000) {
        for (int i = 0; i < 2; i++) {
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
        }
        if (a->state != MDRT_NET_CONNECTING && b->state != MDRT_NET_CONNECTING) break;
    }
    return a->state == MDRT_NET_GO && b->state == MDRT_NET_GO;
}

static void play(peer *a, peer *b, uint32_t until, uint64_t limit_us)
{
    for (uint64_t end = clock_us + limit_us; clock_us < end; clock_us += 250) {
        if (a->state == MDRT_NET_GO || a->state == MDRT_NET_WAIT) { if (clock_us >= a->next_at) turn(a, until); }
        if (b->state == MDRT_NET_GO || b->state == MDRT_NET_WAIT) { if (clock_us >= b->next_at) turn(b, until); }
        int done = 1;
        for (peer *p = a; p; p = p == a ? b : NULL) {
            if (p->state != MDRT_NET_GO && p->state != MDRT_NET_WAIT) continue;
            mdrt_net_stats s;
            mdrt_net_stats_get(p->net, &s);
            if (mdrt_frame_count(p->m) < until || !p->need_frame || mdrt_quiet(p->m) || s.confirmed < until) done = 0;
        }
        if (done || ((a->state == MDRT_NET_ENDED || a->state == MDRT_NET_REFUSED) && (b->state == MDRT_NET_ENDED || b->state == MDRT_NET_REFUSED))) break;
    }
}

static void finish(peer *a, peer *b)
{
    for (peer *p = a; p; p = p == a ? b : NULL) {
        mdrt_net_free(p->net); p->net = NULL;
        if (p->m) mdrt_free(p->m);
        p->m = NULL;
        free(p->inbox); p->inbox = NULL; p->ninbox = p->capinbox = 0;
    }
}

static void show(const peer *p)
{
    mdrt_net_stats s;
    mdrt_net_stats_get(p->net, &s);
    printf("    %s: frame %u, %ld rollbacks (%ld frames, at most %d), %ld waits, %ld frames given back, %ld checks, rtt %d ms\n",
           p->name, (unsigned)s.frame, s.rollbacks, s.replayed, s.most_replayed, s.waits, s.gave_back, s.checks, s.rtt_ms);
}

static peer A, B;

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
    /* a league's worth of settings, 4 MB, through the same loss */
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

int main(void)
{
    for (int i = 0; mdrt_fm_cores(i); i++)
        if (!strcmp(mdrt_fm_cores(i), "ymfm")) core = "ymfm";
    session();
    desync();
    protocol();
    return check_done("net_test");
}
