/* UDP for netplay (mdrt/net_udp.h), over the loopback: a host learns its peer from the first
 * datagram and, once locked, hears no one else; IPv6 too where there is one; the simulated network
 * loses and delays what it is told to; the command line's options. Then two netplay sessions in
 * one process over real sockets, a host and a joiner and two that both connect, each through a
 * simulated network with loss: both consoles reach the same simulation at the last frame. */
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "mdrt/mdrt.h"
#include "mdrt/net_udp.h"

static char err[200];

static int get(mdrt_udp *u, char *buf, size_t size, int ms)
{
    mdrt_net_link l = mdrt_udp_link(u);
    for (int i = 0; i < ms; i++) {
        int n = l.recv(l.user, buf, size);
        if (n) return n;
        mdrt_udp_wait(u, 1);
    }
    return 0;
}

static void sockets(void)
{
    mdrt_udp *host = mdrt_udp_open(0, NULL, err, sizeof err);
    CHECK(host != NULL);
    if (!host) { printf("  no socket: %s\n", err); return; }
    char peer[64], buf[1300], addr[64];
    snprintf(addr, sizeof addr, "127.0.0.1:%u", (unsigned)mdrt_udp_port(host));
    mdrt_udp *join = mdrt_udp_open(0, addr, err, sizeof err);
    CHECK(join != NULL);
    mdrt_net_link hl = mdrt_udp_link(host), jl = mdrt_udp_link(join);
    CHECK_EQ(hl.send(hl.user, "MN?", 3), -1);            /* (a host has no peer until it hears one) */
    CHECK_EQ(jl.send(jl.user, "MNhello", 7), 0);
    CHECK_EQ(get(host, buf, sizeof buf, 1000), 7);
    CHECK(!memcmp(buf, "MNhello", 7));
    char want[64];
    snprintf(want, sizeof want, "127.0.0.1:%u", (unsigned)mdrt_udp_port(join));
    printf("  the host heard %s (the joiner at %s)\n", mdrt_udp_peer(host, peer, sizeof peer), want);
    CHECK(!strcmp(peer, want));
    CHECK_EQ(hl.send(hl.user, "MNback", 6), 0);
    CHECK_EQ(get(join, buf, sizeof buf, 1000), 6);
    /* locked: another sender is not heard */
    mdrt_udp_lock(host);
    mdrt_udp *other = mdrt_udp_open(0, addr, err, sizeof err);
    mdrt_net_link ol = mdrt_udp_link(other);
    ol.send(ol.user, "MNother", 7);
    jl.send(jl.user, "MNmine", 6);
    int n = get(host, buf, sizeof buf, 500);
    CHECK_EQ(n, 6);
    CHECK_EQ(get(host, buf, sizeof buf, 100), 0);
    mdrt_udp_close(other);
    /* the simulated network: 30% lost, 20 ms late */
    mdrt_udp_simulate(join, 20, 5, 30, 0);
    uint64_t t0 = jl.ticks(jl.user);
    for (int i = 0; i < 400; i++) jl.send(jl.user, "MNsim", 5);
    int first = 0, got = 0;
    for (int i = 0; i < 400; i++) {
        mdrt_udp_wait(join, 1);                           /* (the joiner lets them go when their time comes) */
        while (hl.recv(hl.user, buf, sizeof buf) > 0) { if (!got) first = (int)((hl.ticks(hl.user) - t0) / 1000); got++; }
    }
    printf("  simulated: %d of 400 came, the first after %d ms\n", got, first);
    CHECK(got > 230 && got < 330);
    CHECK(first >= 19);
    mdrt_udp_close(join);
    mdrt_udp_close(host);
    /* IPv6, where there is one */
    mdrt_udp *h6 = mdrt_udp_open(0, NULL, err, sizeof err);
    snprintf(addr, sizeof addr, "[::1]:%u", (unsigned)mdrt_udp_port(h6));
    mdrt_udp *j6 = mdrt_udp_open(0, addr, err, sizeof err);
    if (j6) {
        mdrt_net_link l6 = mdrt_udp_link(j6);
        l6.send(l6.user, "MNv6", 4);
        n = get(h6, buf, sizeof buf, 1000);
        printf("  IPv6: the host heard %s\n", mdrt_udp_peer(h6, peer, sizeof peer));
        CHECK_EQ(n, 4);
        CHECK(peer[0] == '[');
        mdrt_udp_close(j6);
    } else printf("  IPv6: none here (%s)\n", err);
    mdrt_udp_close(h6);
    CHECK(mdrt_udp_open(0, "no.such.host.invalid:7000", err, sizeof err) == NULL);
    CHECK(mdrt_udp_open(0, "1.2.3.4:99999", err, sizeof err) == NULL);
}

static void options(void)
{
    mdrt_net_options o;
    mdrt_net_options_init(&o);
    char *argv[] = { "game", "--join", "10.0.0.5:7100", "--delay", "3", "--net-sim", "40,10,5,1", "--rom", "x", "--host", "--port", "7200" };
    int argc = (int)(sizeof argv / sizeof *argv), taken = 0, i;
    for (i = 1; i < argc; i++) {
        int r = mdrt_net_parse(&o, argc, argv, &i, err, sizeof err);
        CHECK(r >= 0);
        if (r > 0) taken++;
        else i++;                                         /* (--rom x is the client's) */
    }
    CHECK_EQ(taken, 5);
    CHECK(o.on && o.role == MDRT_NET_HOST && o.port == 7200 && o.delay == 3 && !strcmp(o.peer, "10.0.0.5:7100"));
    CHECK(o.sim[0] == 40 && o.sim[1] == 10 && o.sim[2] == 5 && o.sim[3] == 1);
    char *bad[] = { "game", "--delay", "99" };
    i = 1;
    CHECK_EQ(mdrt_net_parse(&o, 3, bad, &i, err, sizeof err), -1);
    char *bad2[] = { "game", "--join" };
    i = 1;
    CHECK_EQ(mdrt_net_parse(&o, 2, bad2, &i, err, sizeof err), -1);
}

/* ---- a session over the sockets ------------------------------------------------------------------- */

#define FRAMES 240
typedef struct side { mdrt_udp *u; mdrt_net *n; mdrt *m; int state, ready; uint32_t seed; } side;

static void buttons(void *user, uint16_t b[MDRT_PADS])
{
    side *s = user;
    uint32_t f = (uint32_t)mdrt_frame_count(s->m);
    b[0] = (uint16_t)(((s->seed ^ (f / 4)) * 2654435761u) >> 20 & 0xFF);
}

static void sides(side *a, side *b)
{
    side *ss[2] = { a, b };
    for (int i = 0; i < 2; i++) {
        mdrt_net_config c = { .role = 0, .identity = "udp_test", .delay = 1, .window = 8, .rollback = 30 };
        c.role = ss[i]->state;
        c.link = mdrt_udp_link(ss[i]->u);
        c.local_input = buttons;
        c.input_user = ss[i];
        c.seed = ss[i]->seed;
        ss[i]->n = mdrt_net_new(&c);
        ss[i]->state = MDRT_NET_CONNECTING;
        ss[i]->m = NULL;
        ss[i]->ready = 0;
        mdrt_udp_simulate(ss[i]->u, 15, 10, 10, 2);
    }
    for (int k = 0; k < 20000; k++) {
        int going = 0;
        for (int i = 0; i < 2; i++) {
            side *s = ss[i];
            if (s->state == MDRT_NET_CONNECTING) {
                s->state = mdrt_net_connect(s->n);
                if (s->state == MDRT_NET_READY) {
                    mdrt_config cfg = { 0 };
                    for (int c = 0; mdrt_fm_cores(c); c++)
                        if (!strcmp(mdrt_fm_cores(c), "ymfm")) cfg.fm_core = "ymfm";   /* (the faster) */
                    s->m = mdrt_new(&cfg);
                    mdrt_net_start(s->n, s->m);
                    mdrt_udp_lock(s->u);
                    s->state = MDRT_NET_GO;
                }
                going = 1;
                continue;
            }
            if (s->state != MDRT_NET_GO && s->state != MDRT_NET_WAIT && s->state != MDRT_NET_RESTART) continue;
            mdrt_net_stats st;
            mdrt_net_stats_get(s->n, &st);
            if (s->state != MDRT_NET_WAIT && (mdrt_frame_count(s->m) < FRAMES || mdrt_quiet(s->m))) {
                mdrt_vblank_end(s->m);
                mdrt_wait_vblank(s->m);
            }
            s->state = mdrt_net_frame(s->n, 1);
            if (mdrt_frame_count(s->m) < FRAMES || st.confirmed < FRAMES || mdrt_quiet(s->m)) going = 1;
            if (s->state == MDRT_NET_GO && mdrt_frame_count(s->m) >= FRAMES && !mdrt_quiet(s->m)) s->state = MDRT_NET_WAIT;
        }
        if (!going) break;
        mdrt_udp_wait(a->u, 1);
    }
}

static void session(int either)
{
    side a = { .seed = 11 }, b = { .seed = 22 };
    char addr[64];
    if (either) {
        a.u = mdrt_udp_open(0, NULL, err, sizeof err);
        snprintf(addr, sizeof addr, "127.0.0.1:%u", (unsigned)mdrt_udp_port(a.u));
        b.u = mdrt_udp_open(0, addr, err, sizeof err);
        snprintf(addr, sizeof addr, "127.0.0.1:%u", (unsigned)mdrt_udp_port(b.u));
        CHECK_EQ(mdrt_udp_set_peer(a.u, addr, err, sizeof err), 0);
        a.state = b.state = MDRT_NET_EITHER;
    } else {
        a.u = mdrt_udp_open(0, NULL, err, sizeof err);
        snprintf(addr, sizeof addr, "localhost:%u", (unsigned)mdrt_udp_port(a.u));
        b.u = mdrt_udp_open(0, addr, err, sizeof err);
        a.state = MDRT_NET_HOST;
        b.state = MDRT_NET_JOIN;
    }
    CHECK(a.u && b.u);
    if (!a.u || !b.u) return;
    sides(&a, &b);
    CHECK(a.m && b.m);
    if (a.m && b.m) {
        mdrt_net_stats sa, sb;
        mdrt_net_stats_get(a.n, &sa);
        mdrt_net_stats_get(b.n, &sb);
        uint64_t ha = mdrt_sim_hash(a.m), hb = mdrt_sim_hash(b.m);
        printf("  %s over UDP: frames %u and %u, %ld and %ld rollbacks, round trip %d ms; the same at the last frame: %s\n",
               either ? "two that both connect" : "a host and a joiner", (unsigned)mdrt_frame_count(a.m), (unsigned)mdrt_frame_count(b.m),
               sa.rollbacks, sb.rollbacks, sa.rtt_ms, ha == hb ? "yes" : "NO");
        CHECK_EQ(mdrt_frame_count(a.m), FRAMES);
        CHECK_EQ(mdrt_frame_count(b.m), FRAMES);
        CHECK(ha == hb);
        CHECK(mdrt_net_is_host(a.n) != mdrt_net_is_host(b.n));
    }
    for (side *s = &a; s; s = s == &a ? &b : NULL) {
        mdrt_net_free(s->n);
        mdrt_free(s->m);
        mdrt_udp_close(s->u);
    }
}

int main(void)
{
    sockets();
    options();
    session(0);
    session(1);
    return check_done("udp_test");
}
