/* UDP for netplay (mdrt/net_udp.h). */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mdrt/net_udp.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define close_sock closesocket
typedef int socklen;
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define close_sock close
typedef socklen_t socklen;
#endif

typedef struct held { uint64_t at; size_t n; uint8_t b[MDRT_NET_MTU]; } held;

struct mdrt_udp {
    sock_t s;
    int family;
    struct sockaddr_storage peer;
    socklen peer_len;
    int have_peer, locked;
    uint16_t port;
    int sim[4];                         /* the simulated network */
    uint32_t rng;
    held *q;                            /* datagrams it holds back */
    int nq, capq;
};

static uint64_t ticks(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart * 1000000 + c.QuadPart % f.QuadPart * 1000000 / f.QuadPart);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
#endif
}

static int started(void)
{
#ifdef _WIN32
    static int done;
    if (!done) { WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w)) return 0; done = 1; }
#endif
    return 1;
}

/* "host", "host:port", "[v6]:port", "v6" (more than one colon, no brackets) into a name and port */
static int split(const char *addr, char *host, size_t hn, uint16_t *port)
{
    const char *colon = NULL;
    *port = MDRT_UDP_PORT;
    if (addr[0] == '[') {
        const char *close = strchr(addr, ']');
        if (!close || (size_t)(close - addr - 1) >= hn) return -1;
        memcpy(host, addr + 1, (size_t)(close - addr - 1));
        host[close - addr - 1] = 0;
        if (close[1] == ':') colon = close + 1;
        else if (close[1]) return -1;
    } else {
        const char *c = strchr(addr, ':');
        if (c && !strchr(c + 1, ':')) colon = c;          /* (one colon: a port; more: an IPv6 address) */
        size_t k = colon ? (size_t)(colon - addr) : strlen(addr);
        if (k >= hn || !k) return -1;
        memcpy(host, addr, k);
        host[k] = 0;
    }
    if (colon) {
        char *end;
        long p = strtol(colon + 1, &end, 10);
        if (*end || p < 1 || p > 65535) return -1;
        *port = (uint16_t)p;
    }
    return 0;
}

static int resolve(const char *addr, struct sockaddr_storage *out, socklen *len, char *err, size_t errsize)
{
    char host[256], service[8];
    uint16_t port;
    if (split(addr, host, sizeof host, &port)) { snprintf(err, errsize, "not an address: %s", addr); return -1; }
    snprintf(service, sizeof service, "%u", (unsigned)port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, service, &hints, &res) || !res) { snprintf(err, errsize, "cannot find %s", host); return -1; }
    memcpy(out, res->ai_addr, res->ai_addrlen);
    *len = (socklen)res->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

/* an IPv4 address as the IPv6 socket's mapped one */
static void to_family(int family, struct sockaddr_storage *a, socklen *len)
{
    if (family != AF_INET6 || a->ss_family != AF_INET) return;
    struct sockaddr_in v4;
    memcpy(&v4, a, sizeof v4);
    struct sockaddr_in6 v6;
    memset(&v6, 0, sizeof v6);
    v6.sin6_family = AF_INET6;
    v6.sin6_port = v4.sin_port;
    v6.sin6_addr.s6_addr[10] = v6.sin6_addr.s6_addr[11] = 0xFF;
    memcpy(&v6.sin6_addr.s6_addr[12], &v4.sin_addr, 4);
    memset(a, 0, sizeof *a);
    memcpy(a, &v6, sizeof v6);
    *len = (socklen)sizeof v6;
}

static sock_t make_socket(int family, uint16_t port, uint16_t *bound)
{
    sock_t s = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (s == BAD_SOCK) return BAD_SOCK;
    if (family == AF_INET6) {
        int off = 0;                                      /* (IPv4 too, as mapped addresses) */
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&off, sizeof off);
    }
    struct sockaddr_storage a;
    memset(&a, 0, sizeof a);
    socklen len;
    if (family == AF_INET6) {
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&a;
        v6->sin6_family = AF_INET6; v6->sin6_port = htons(port); v6->sin6_addr = in6addr_any;
        len = (socklen)sizeof *v6;
    } else {
        struct sockaddr_in *v4 = (struct sockaddr_in *)&a;
        v4->sin_family = AF_INET; v4->sin_port = htons(port); v4->sin_addr.s_addr = htonl(INADDR_ANY);
        len = (socklen)sizeof *v4;
    }
    if (bind(s, (struct sockaddr *)&a, len)) { close_sock(s); return BAD_SOCK; }
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    len = (socklen)sizeof a;
    getsockname(s, (struct sockaddr *)&a, &len);
    *bound = ntohs(a.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&a)->sin6_port : ((struct sockaddr_in *)&a)->sin_port);
    return s;
}

mdrt_udp *mdrt_udp_open(uint16_t port, const char *peer, char *err, size_t errsize)
{
    if (!started()) { snprintf(err, errsize, "no sockets (Winsock would not start)"); return NULL; }
    mdrt_udp *u = calloc(1, sizeof *u);
    if (!u) { snprintf(err, errsize, "out of memory"); return NULL; }
    u->s = BAD_SOCK;
    struct sockaddr_storage pa;
    socklen pl = 0;
    if (peer && resolve(peer, &pa, &pl, err, errsize)) { free(u); return NULL; }
    /* an IPv6 socket that takes IPv4 too, unless the peer is IPv4 or there is no IPv6 */
    int families[2] = { AF_INET6, AF_INET };
    for (int k = peer && pa.ss_family == AF_INET ? 1 : 0; k < 2 && u->s == BAD_SOCK; k++) {
        u->s = make_socket(families[k], port, &u->port);
        u->family = families[k];
    }
    if (u->s == BAD_SOCK) { snprintf(err, errsize, "cannot open UDP port %u (in use?)", (unsigned)port); free(u); return NULL; }
    if (peer) {
        to_family(u->family, &pa, &pl);
        u->peer = pa;
        u->peer_len = pl;
        u->have_peer = 1;
    }
    u->rng = (uint32_t)ticks() | 1;
    return u;
}

void mdrt_udp_close(mdrt_udp *u)
{
    if (!u) return;
    if (u->s != BAD_SOCK) close_sock(u->s);
    free(u->q);
    free(u);
}

int mdrt_udp_set_peer(mdrt_udp *u, const char *peer, char *err, size_t errsize)
{
    struct sockaddr_storage pa;
    socklen pl;
    if (resolve(peer, &pa, &pl, err, errsize)) return -1;
    to_family(u->family, &pa, &pl);
    if (pa.ss_family != u->family) { snprintf(err, errsize, "%s is not reachable from this socket", peer); return -1; }
    u->peer = pa;
    u->peer_len = pl;
    u->have_peer = 1;
    return 0;
}

void mdrt_udp_lock(mdrt_udp *u) { u->locked = 1; }
uint16_t mdrt_udp_port(const mdrt_udp *u) { return u->port; }

const char *mdrt_udp_peer(const mdrt_udp *u, char *buf, size_t size)
{
    if (!size) return buf;
    buf[0] = 0;
    if (!u->have_peer) return buf;
    char host[128] = "", service[16] = "";
    getnameinfo((const struct sockaddr *)&u->peer, u->peer_len, host, sizeof host, service, sizeof service, NI_NUMERICHOST | NI_NUMERICSERV);
    const char *h = !strncmp(host, "::ffff:", 7) ? host + 7 : host;   /* (an IPv4 address mapped) */
    snprintf(buf, size, strchr(h, ':') ? "[%s]:%s" : "%s:%s", h, service);
    return buf;
}

static uint32_t rnd(mdrt_udp *u) { u->rng ^= u->rng << 13; u->rng ^= u->rng >> 17; u->rng ^= u->rng << 5; return u->rng; }

static void put_out(mdrt_udp *u, const void *b, size_t n)
{
    sendto(u->s, (const char *)b, (int)n, 0, (const struct sockaddr *)&u->peer, u->peer_len);
}

/* the datagrams held back whose time has come */
static void release(mdrt_udp *u)
{
    uint64_t t = ticks();
    for (int i = 0; i < u->nq;) {
        if (u->q[i].at <= t) { put_out(u, u->q[i].b, u->q[i].n); u->q[i] = u->q[--u->nq]; }
        else i++;
    }
}

static void hold(mdrt_udp *u, const void *b, size_t n)
{
    if (u->nq == u->capq) {
        int cap = u->capq ? 2 * u->capq : 64;
        held *q = realloc(u->q, (size_t)cap * sizeof *q);
        if (!q) return;
        u->q = q;
        u->capq = cap;
    }
    held *h = &u->q[u->nq++];
    h->at = ticks() + (uint64_t)u->sim[0] * 1000u + (u->sim[1] ? rnd(u) % ((uint32_t)u->sim[1] * 1000u) : 0);
    h->n = n;
    memcpy(h->b, b, n);
}

static int link_send(void *user, const void *b, size_t n)
{
    mdrt_udp *u = user;
    if (!u->have_peer || n > MDRT_NET_MTU) return -1;
    release(u);
    if (!(u->sim[0] | u->sim[1] | u->sim[2] | u->sim[3])) { put_out(u, b, n); return 0; }
    if ((int)(rnd(u) % 100) < u->sim[2]) return 0;
    hold(u, b, n);
    if ((int)(rnd(u) % 100) < u->sim[3]) hold(u, b, n);
    return 0;
}

static int same_peer(const mdrt_udp *u, const struct sockaddr_storage *a, socklen len)
{
    if (len != u->peer_len) return 0;
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6 *x = (const struct sockaddr_in6 *)a, *y = (const struct sockaddr_in6 *)&u->peer;
        return x->sin6_port == y->sin6_port && !memcmp(&x->sin6_addr, &y->sin6_addr, sizeof x->sin6_addr);
    }
    const struct sockaddr_in *x = (const struct sockaddr_in *)a, *y = (const struct sockaddr_in *)&u->peer;
    return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
}

static int link_recv(void *user, void *buf, size_t size)
{
    mdrt_udp *u = user;
    release(u);
    for (;;) {
        struct sockaddr_storage from;
        socklen len = (socklen)sizeof from;
        int n = (int)recvfrom(u->s, (char *)buf, (int)size, 0, (struct sockaddr *)&from, &len);
        if (n < 0) {
#ifdef _WIN32
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) return 0;
            if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue;   /* (an ICMP reply to an earlier send; too long) */
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            if (errno == EINTR || errno == ECONNREFUSED) continue;
#endif
            return -1;
        }
        if (u->locked && !same_peer(u, &from, len)) continue;   /* (someone else) */
        if (!u->locked && (!u->have_peer || !same_peer(u, &from, len))) {   /* (a host follows whoever sends) */
            u->peer = from;
            u->peer_len = len;
            u->have_peer = 1;
        }
        return n;
    }
}

static uint64_t link_ticks(void *user) { (void)user; return ticks(); }

mdrt_net_link mdrt_udp_link(mdrt_udp *u) { return (mdrt_net_link){ .user = u, .send = link_send, .recv = link_recv, .ticks = link_ticks }; }

void mdrt_udp_wait(mdrt_udp *u, int ms)
{
    release(u);
    uint64_t until = ticks() + (uint64_t)ms * 1000u;
    if (u->nq) {                                          /* (wake for the next held datagram) */
        for (int i = 0; i < u->nq; i++) if (u->q[i].at < until) until = u->q[i].at;
    }
    uint64_t t = ticks();
    uint64_t us = until > t ? until - t : 0;
    fd_set r;
    FD_ZERO(&r);
    FD_SET(u->s, &r);
    struct timeval tv = { (long)(us / 1000000u), (long)(us % 1000000u) };
    select((int)u->s + 1, &r, NULL, NULL, &tv);
    release(u);
}

void mdrt_udp_simulate(mdrt_udp *u, int delay_ms, int jitter_ms, int loss_pct, int dup_pct)
{
    u->sim[0] = delay_ms > 0 ? delay_ms : 0;
    u->sim[1] = jitter_ms > 0 ? jitter_ms : 0;
    u->sim[2] = loss_pct > 0 ? loss_pct : 0;
    u->sim[3] = dup_pct > 0 ? dup_pct : 0;
}

/* ---- the command line's options ------------------------------------------------------------- */

void mdrt_net_options_init(mdrt_net_options *o)
{
    memset(o, 0, sizeof *o);
    o->delay = 2;
}

static int number(const char *s, long lo, long hi, long *v)
{
    char *end;
    *v = strtol(s, &end, 10);
    return *s && !*end && *v >= lo && *v <= hi;
}

int mdrt_net_parse(mdrt_net_options *o, int argc, char **argv, int *i, char *err, size_t errsize)
{
    const char *a = argv[*i], *next = *i + 1 < argc ? argv[*i + 1] : NULL;
    long v;
    if (!strcmp(a, "--host")) {
        o->on = 1;
        o->role = MDRT_NET_HOST;
        if (next && next[0] != '-') {
            if (!number(next, 1, 65535, &v)) { snprintf(err, errsize, "--host: not a port: %s", next); return -1; }
            o->port = (uint16_t)v;
            ++*i;
        }
        return 1;
    }
    if (!strcmp(a, "--join") || !strcmp(a, "--peer")) {
        if (!next) { snprintf(err, errsize, "%s needs an address", a); return -1; }
        o->on = 1;
        o->role = a[2] == 'j' ? MDRT_NET_JOIN : MDRT_NET_EITHER;
        snprintf(o->peer, sizeof o->peer, "%s", next);
        ++*i;
        return 1;
    }
    if (!strcmp(a, "--port")) {
        if (!next || !number(next, 1, 65535, &v)) { snprintf(err, errsize, "--port needs a port (1-65535)"); return -1; }
        o->port = (uint16_t)v;
        ++*i;
        return 1;
    }
    if (!strcmp(a, "--delay")) {
        if (!next || !number(next, 0, MDRT_NET_MAX_DELAY, &v)) { snprintf(err, errsize, "--delay needs frames (0-%d)", MDRT_NET_MAX_DELAY); return -1; }
        o->delay = (int)v;
        ++*i;
        return 1;
    }
    if (!strcmp(a, "--net-sim")) {
        int k = next ? sscanf(next, "%d,%d,%d,%d", &o->sim[0], &o->sim[1], &o->sim[2], &o->sim[3]) : 0;
        if (k != 4 || o->sim[0] < 0 || o->sim[1] < 0 || o->sim[2] < 0 || o->sim[2] > 100 || o->sim[3] < 0 || o->sim[3] > 100) {
            snprintf(err, errsize, "--net-sim needs DELAY_MS,JITTER_MS,LOSS%%,DUPLICATES%%");
            return -1;
        }
        ++*i;
        return 1;
    }
    return 0;
}

mdrt_udp *mdrt_net_open_udp(const mdrt_net_options *o, char *err, size_t errsize)
{
    uint16_t port = o->port ? o->port : o->role == MDRT_NET_JOIN ? 0 : MDRT_UDP_PORT;
    mdrt_udp *u = mdrt_udp_open(port, o->role == MDRT_NET_HOST ? NULL : o->peer, err, errsize);
    if (u && (o->sim[0] | o->sim[1] | o->sim[2] | o->sim[3])) mdrt_udp_simulate(u, o->sim[0], o->sim[1], o->sim[2], o->sim[3]);
    return u;
}
