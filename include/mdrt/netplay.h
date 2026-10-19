/* Rollback netplay (the playbook's pulseman/NETPLAY.md): two consoles, one on each peer's machine,
 * play the same game. Each plays its own pads at once and sends their buttons to the other; for
 * the other's pads it guesses (the buttons last heard, held) and runs on. When the other's buttons
 * for a frame come and differ from the guess, it loads its state from before that frame and plays
 * the frames since again, quietly (mdrt_set_quiet), with the right buttons, all within one frame
 * of the player's time. Input delay (frames between pressing and the game seeing it) makes the
 * guesses fewer; the window bounds how far a peer runs on guesses before it waits.
 *
 * The module knows no game and touches no sockets or clocks: the platform gives it a link (send
 * and receive datagrams, never blocking, and a clock) and the local players' buttons; the client
 * tells it, at every wait after its vertical blank code, whether it could go on from there after a
 * state is loaded (a resumable point: its game loop's wait), and restarts there when told. A
 * state is taken at every resumable point. Where a game has none for a while (a menu, a loading
 * screen), the module guesses only as far back as it can roll back, then waits for each frame's
 * buttons (input delay alone).
 *
 * The session: both peers start their consoles at power on once connected (frame 0); frames count
 * as mdrt_frame_count does, the first frame the console plays being 1. The pads each peer plays
 * are the host's choice (by default the host pad 1, the guest pad 2); the host's settings (a
 * blob its client gives: game rules, save RAM) go to the guest's client before its power on. The
 * identity (a string the client makes from everything the simulation depends on: its build, the
 * ROM, settings that are compared rather than sent) must be the same on both, or the guest is
 * refused. Every check_every frames each sends the simulation's hash (mdrt_sim_hash) of a frame
 * both have played with every button known; a difference is a desync, and the session ends. */
#ifndef MDRT_NETPLAY_H
#define MDRT_NETPLAY_H
#include <stddef.h>
#include <stdint.h>
#include "mdrt/mdrt.h"

#define MDRT_NET_PROTOCOL 1             /* the messages' version: peers must agree */

/* the other peer, as the platform reaches it (UDP natively: mdrt/net_udp.h). send: one datagram
 * (at most MDRT_NET_MTU bytes) to the peer, 0, or -1 when it could not go (it is lost, as any may
 * be); recv: the next datagram from the peer into buf, its length, 0 when none waits, -1 on an
 * error; ticks: microseconds since any start, never going back. None of them may block. */
#define MDRT_NET_MTU 1200
typedef struct mdrt_net_link {
    void *user;
    int (*send)(void *user, const void *bytes, size_t n);
    int (*recv)(void *user, void *buf, size_t size);
    uint64_t (*ticks)(void *user);
} mdrt_net_link;

enum { MDRT_NET_HOST, MDRT_NET_JOIN, MDRT_NET_EITHER };    /* roles: EITHER settles it with the other */

typedef struct mdrt_net_config {
    int role;
    const char *identity;       /* what must match (compared whole; up to 255 bytes) */
    mdrt_net_link link;
    /* the local players' buttons now: buttons[0] the first pad this peer plays, [1] the second ... */
    void (*local_input)(void *user, uint16_t buttons[MDRT_PADS]);
    void *input_user;
    /* the host's, sent to the guest (the guest's own are not used) */
    uint8_t host_pads, guest_pads;  /* the pads each plays, bit p for pad p + 1 (0 and 0: 1 and 2) */
    int delay;                  /* input delay in frames, 0 to MDRT_NET_MAX_DELAY */
    int window;                 /* frames a peer may play past the other's last known buttons (0: 8) */
    int rollback;               /* frames a rollback may play again at most (0: 30) */
    int check_every;            /* frames between desync checks (0: 60) */
    const void *settings;       /* the host's session settings, for the guest's client */
    size_t settings_size;       /* (at most MDRT_NET_MAX_SETTINGS bytes) */
    uint64_t seed;              /* for the session's numbers (0: from the clock) */
} mdrt_net_config;
#define MDRT_NET_MAX_DELAY 10
#define MDRT_NET_MAX_SETTINGS (1u << 20)

typedef struct mdrt_net mdrt_net;
mdrt_net *mdrt_net_new(const mdrt_net_config *c);        /* NULL: out of memory, or a bad configuration */
void mdrt_net_free(mdrt_net *n);

/* Connecting, never blocking: call it until it returns other than MDRT_NET_CONNECTING (a few
 * times a second is enough; more is fine). MDRT_NET_READY: power on and call mdrt_net_start.
 * MDRT_NET_REFUSED (a guest refused, or a host or join meeting its own kind) and MDRT_NET_ENDED:
 * mdrt_net_why says why. A host that refuses a guest goes on waiting for another (mdrt_net_why
 * says whom it refused, for the player). */
enum { MDRT_NET_CONNECTING, MDRT_NET_READY, MDRT_NET_REFUSED, MDRT_NET_ENDED,
       MDRT_NET_GO, MDRT_NET_WAIT, MDRT_NET_RESTART };
int mdrt_net_connect(mdrt_net *n);
/* where the session is: MDRT_NET_CONNECTING, MDRT_NET_GO (connected: playing), MDRT_NET_REFUSED or
 * MDRT_NET_ENDED (mdrt_net_why) */
int mdrt_net_state(const mdrt_net *n);
int mdrt_net_is_host(const mdrt_net *n);
uint8_t mdrt_net_pads(const mdrt_net *n);                /* the pads this peer plays (bit p: pad p + 1) */
int mdrt_net_delay(const mdrt_net *n);
/* the host's settings, once ready (the host's own on the host): NULL and 0 when it gave none */
const void *mdrt_net_settings(const mdrt_net *n, size_t *size);

/* the session on a console just powered on (frame 0): the module becomes its pad source */
void mdrt_net_start(mdrt_net *n, mdrt *m);
/* At every wait of the client's, after its vertical blank code; resumable: whether the client can
 * go on from here after a state is loaded. MDRT_NET_GO: play the next frame. MDRT_NET_WAIT: not
 * yet (the other's buttons are too far behind, or this peer is ahead and gives a frame back):
 * call again soon (a millisecond or so later), without playing on. MDRT_NET_RESTART: a state was
 * loaded (a rollback): restart at the resumable point and play on. MDRT_NET_ENDED: the session is
 * over (mdrt_net_why); the console is as it was and can play on alone. */
int mdrt_net_frame(mdrt_net *n, int resumable);
/* say goodbye (the other's session ends at once rather than when it stops hearing from this one).
 * Neither this nor mdrt_net_free touches the console, which may be gone by then: a client that
 * plays on alone after it sets its own pad source. (A session that ends inside mdrt_net_frame
 * leaves the console as it was, its pad source the platform's.) */
void mdrt_net_quit(mdrt_net *n);
const char *mdrt_net_why(const mdrt_net *n);

typedef struct mdrt_net_stats {
    uint32_t frame, remote_frame;   /* the last frame played here; the newest the other is known to have played */
    uint32_t confirmed;             /* frames whose buttons are all known */
    uint32_t remote_has;            /* the newest frame of this peer's buttons the other is known to have */
    int rtt_ms;                     /* the round trip, smoothed (-1: not measured yet) */
    int delay;
    long rollbacks, replayed;       /* rollbacks, and the frames they played again */
    int last_replayed, most_replayed;
    long waits;                     /* calls that returned MDRT_NET_WAIT */
    long gave_back;                 /* frames given back to keep in step with the other */
    long checks;                    /* desync checks that agreed */
    int lost;                       /* nothing heard for 3 seconds (the session ends after 10) */
} mdrt_net_stats;
void mdrt_net_stats_get(const mdrt_net *n, mdrt_net_stats *s);

/* The session's buttons, frame by frame as they become known for every pad: a line each,
 * "FRAME B1 B2 B3 B4" (decimal, then each pad's MDRT_PAD_* bits in hex), to sink: a replay of the
 * session from power on, and what a desync report plays again. */
void mdrt_net_set_log(mdrt_net *n, mdrt_sink_fn sink, void *user);

#endif
