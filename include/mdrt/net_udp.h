/* UDP for netplay's link (mdrt/netplay.h), natively: POSIX sockets, Winsock 2 on Windows (link
 * ws2_32), IPv4 and IPv6, names resolved. A host opens a port and takes the first peer it hears
 * from (it follows whoever sends until mdrt_udp_lock, which the platform calls once the session
 * starts); a joiner, and each of two peers that both connect (a simultaneous open: each sends to
 * the other's public address from its own port, which most home routers let through without a
 * forwarded port), names the other's address. Nothing here blocks except mdrt_udp_wait.
 *
 * Also the command line's netplay options, the same for every client: mdrt_net_parse. */
#ifndef MDRT_NET_UDP_H
#define MDRT_NET_UDP_H
#include <stddef.h>
#include <stdint.h>
#include "mdrt/netplay.h"

#define MDRT_UDP_PORT 7000              /* the default port */

typedef struct mdrt_udp mdrt_udp;
/* a socket on port (0: any free one), and the peer ("host", "host:port", "1.2.3.4:port",
 * "[v6]:port"; the port MDRT_UDP_PORT when none is given) or NULL (a host: the first to send); NULL
 * on failure, with why in err */
mdrt_udp *mdrt_udp_open(uint16_t port, const char *peer, char *err, size_t errsize);
void mdrt_udp_close(mdrt_udp *u);
int mdrt_udp_set_peer(mdrt_udp *u, const char *peer, char *err, size_t errsize);   /* 0, or -1 */
void mdrt_udp_lock(mdrt_udp *u);        /* from now only the peer it has is heard */
uint16_t mdrt_udp_port(const mdrt_udp *u);
const char *mdrt_udp_peer(const mdrt_udp *u, char *buf, size_t size);   /* the peer's address ("" none yet) */
/* the link for mdrt_net_config (its clock: a monotonic one, in microseconds) */
mdrt_net_link mdrt_udp_link(mdrt_udp *u);
/* this machine's address on its own network (a LAN's, as 192.168.1.20: the one its traffic to the
 * internet leaves by; no datagram is sent), into buf; "" when it has none */
const char *mdrt_udp_lan_address(char *buf, size_t size);
/* wait until a datagram comes or ms milliseconds pass (for a caller that has nothing to do) */
void mdrt_udp_wait(mdrt_udp *u, int ms);
/* a simulated network, for tests: every datagram sent delayed by delay_ms plus up to jitter_ms
 * (so some overtake others), loss_pct per cent of them lost and dup_pct sent twice */
void mdrt_udp_simulate(mdrt_udp *u, int delay_ms, int jitter_ms, int loss_pct, int dup_pct);

/* the command line's netplay options:
 *   --host [PORT]          host, on PORT (MDRT_UDP_PORT)
 *   --join ADDR[:PORT]     join the host at ADDR
 *   --peer ADDR[:PORT]     both connect: each gives the other's address (and --port its own)
 *   --port PORT            this peer's port (with --peer; with --join, to fix it)
 *   --delay N              input delay in frames (the host's choice; 2 by default)
 *   --name NAME            the player's name, for the other to see
 *   --pads N               the pads each plays: 1 (the host pad 1, the guest pad 2), or 2 (EA's 4
 *                          Way Play: the host pads 1 and 2, the guest 3 and 4); the host's choice
 *   --net-sim MS,MS,%,%    a simulated network: delay, jitter, loss, duplicates */
typedef struct mdrt_net_options {
    int on;                     /* a session was asked for */
    int role;                   /* MDRT_NET_HOST, _JOIN or _EITHER */
    char peer[256];
    uint16_t port;              /* 0: MDRT_UDP_PORT for a host or --peer, any for a joiner */
    int delay;
    char name[MDRT_NET_NAME + 1];
    int sim[4];                 /* delay, jitter, loss, duplicates (all 0: none) */
    int pads;                   /* the pads each plays: 1 or 2 (mdrt_net_options_pads) */
} mdrt_net_options;
#define MDRT_NET_USAGE "[--host [PORT] | --join ADDR[:PORT] | --peer ADDR[:PORT]] [--port PORT] [--delay N] [--name NAME] [--pads 1|2] [--net-sim MS,MS,%,%]"
void mdrt_net_options_init(mdrt_net_options *o);
/* argv[*i] (and what follows it) if it is one of them: 1, with *i on the last word it took; 0
 * when it is not one; -1 when it is malformed (err says why) */
int mdrt_net_parse(mdrt_net_options *o, int argc, char **argv, int *i, char *err, size_t errsize);
/* the pads each peer plays as the options say, for mdrt_net_config's host_pads and guest_pads (0 and
 * 0 for one each: the host pad 1, the guest pad 2) */
void mdrt_net_options_pads(const mdrt_net_options *o, uint8_t *host_pads, uint8_t *guest_pads);
/* the socket the options ask for (NULL on failure, err says why) */
mdrt_udp *mdrt_net_open_udp(const mdrt_net_options *o, char *err, size_t errsize);

#endif
