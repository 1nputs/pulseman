/* Rollback netplay (the playbook's pulseman/NETPLAY.md): consoles on two to four machines (a peer
 * each: the host and its guests) play the same game. Each plays its own pads at once and sends
 * their buttons on; for the others' pads it guesses (the buttons last heard, held) and runs on.
 * When another's buttons for a frame come and differ from the guess, it loads its state from
 * before that frame and plays the frames since again, quietly (mdrt_set_quiet), with the right
 * buttons, all within one frame of the player's time. Input delay (frames between pressing and
 * the game seeing it) makes the guesses fewer; the window bounds how far a peer runs on guesses
 * before it waits. The machines are a star: each guest talks to the host alone, and the host
 * passes every guest's buttons on to the others as they come.
 *
 * The module knows no game and touches no sockets or clocks: the platform gives it a link (send
 * and receive datagrams, never blocking, and a clock) and the local players' buttons; the client
 * tells it, at every wait after its vertical blank code, whether it could go on from there after a
 * state is loaded (a resumable point: its game loop's wait), and restarts there when told. A
 * state is taken at every resumable point. Where a game has none for a while (a menu, a loading
 * screen), the module guesses only as far back as it can roll back, then waits for each frame's
 * buttons (input delay alone).
 *
 * The session: every peer starts its console at power on once all are in (frame 0); frames count
 * as mdrt_frame_count does, the first frame the console plays being 1. Each peer says how many
 * players it has (pads it plays); the host waits for the players in all it was told, then gives
 * the pads out in the order the guests came, its own first (two machines with one each: the host
 * pad 1, the guest pad 2; four with one each: the host 1, the guests 2, 3 and 4 as they joined;
 * two and one and one ...). The host's settings (a blob its client gives: game rules, save RAM)
 * go to every guest's client before its power on. The identity (a string the client makes from
 * everything the simulation depends on: its build, the ROM, settings that are compared rather
 * than sent) must be the same on all, or a guest is refused. Every check_every frames each sends
 * the simulation's hash (mdrt_sim_hash) of a frame all have played with every button known, and
 * the host compares each guest's with its own; a difference is a desync. A player who leaves (or
 * a desync, or a machine not heard for 10 seconds) ends the session for every machine, each told
 * why. */
#ifndef MDRT_NETPLAY_H
#define MDRT_NETPLAY_H
#include <stddef.h>
#include <stdint.h>
#include "mdrt/mdrt.h"

#define MDRT_NET_PROTOCOL 5             /* the messages' version: peers must agree (2: the players' names; 3: the
                                           settings' chunks acknowledged selectively; 4: up to four machines;
                                           5: the host's reset, MDRT_NET_RESET_BIT) */
#define MDRT_NET_NAME 24                /* a player's name, at most (printable ASCII) */

/* the other peers, as the platform reaches them (UDP natively: mdrt/net_udp.h). send: one datagram
 * (at most MDRT_NET_MTU bytes) to the peer, 0, or -1 when it could not go (it is lost, as any may
 * be); recv: the next datagram from the peer into buf, its length, 0 when none waits, -1 on an
 * error; ticks: microseconds since any start, never going back. A host's guests: send_to and
 * recv_from, the same for each of several peers by an index the link gives each address it hears
 * from (recv_from says which in *from); without them (NULL) a host takes one guest, by send and
 * recv. A guest (and both peers that connect at once, MDRT_NET_EITHER) use send and recv. None of
 * them may block. */
#define MDRT_NET_MTU 1200
typedef struct mdrt_net_link {
    void *user;
    int (*send)(void *user, const void *bytes, size_t n);
    int (*recv)(void *user, void *buf, size_t size);
    uint64_t (*ticks)(void *user);
    int (*send_to)(void *user, int to, const void *bytes, size_t n);
    int (*recv_from)(void *user, void *buf, size_t size, int *from);
} mdrt_net_link;

enum { MDRT_NET_HOST, MDRT_NET_JOIN, MDRT_NET_EITHER };    /* roles: EITHER settles it with the other */

typedef struct mdrt_net_config {
    int role;
    const char *identity;       /* what must match (compared whole; up to 255 bytes) */
    const char *name;           /* the player's name, for the other to show (NULL or "": none) */
    mdrt_net_link link;
    /* the local players' buttons now: buttons[0] the first pad this peer plays, [1] the second ...
     * A host's with MDRT_NET_RESET_BIT on any of its pads resets the console for everyone, at the
     * frame those buttons are for (a guest's is ignored) */
    void (*local_input)(void *user, uint16_t buttons[MDRT_PADS]);
    void *input_user;
    int players;                /* the pads this peer plays (0: 1), fewer than MDRT_PADS */
    /* the host's, sent to the guests (a guest's own are not used) */
    int players_total;          /* the players in all the host waits for before the game begins (0: 2) */
    int delay;                  /* input delay in frames, 0 to MDRT_NET_MAX_DELAY */
    int window;                 /* frames a peer may play past the other's last known buttons (0: 8) */
    int rollback;               /* frames a rollback may play again at most (0: 30) */
    int check_every;            /* frames between desync checks (0: 60) */
    const void *settings;       /* the host's session settings, for the guests' clients */
    size_t settings_size;       /* (at most MDRT_NET_MAX_SETTINGS bytes) */
    uint64_t seed;              /* for the session's numbers (0: from the clock) */
} mdrt_net_config;
#define MDRT_NET_MAX_DELAY 10
#define MDRT_NET_MAX_SETTINGS (16u << 20)    /* (a guest takes no more: 16 MB, a game's league with its art) */

typedef struct mdrt_net mdrt_net;
mdrt_net *mdrt_net_new(const mdrt_net_config *c);        /* NULL: out of memory, or a bad configuration */
void mdrt_net_free(mdrt_net *n);

/* Connecting, never blocking: call it until it returns other than MDRT_NET_CONNECTING (a few
 * times a second is enough; more is fine). MDRT_NET_READY: every player is in: power on and call
 * mdrt_net_start. MDRT_NET_REFUSED (a guest refused: another game, a full one, one begun; or a
 * host or join meeting its own kind) and MDRT_NET_ENDED: mdrt_net_why says why. A host that
 * refuses a guest goes on waiting (mdrt_net_why says whom it refused, for the player); a guest
 * that leaves the waiting room, or is not heard from for 10 seconds, gives its place up. */
enum { MDRT_NET_CONNECTING, MDRT_NET_READY, MDRT_NET_REFUSED, MDRT_NET_ENDED,
       MDRT_NET_GO, MDRT_NET_WAIT, MDRT_NET_RESTART, MDRT_NET_RESET };
/* a bit of no pad's in a host's buttons (local_input): reset the console. It goes with the host's
 * buttons, so every machine resets at the same frame, and a guess never holds it */
#define MDRT_NET_RESET_BIT 0x8000
int mdrt_net_connect(mdrt_net *n);
/* where the session is: MDRT_NET_CONNECTING, MDRT_NET_GO (connected: playing), MDRT_NET_REFUSED or
 * MDRT_NET_ENDED (mdrt_net_why) */
int mdrt_net_state(const mdrt_net *n);
int mdrt_net_is_host(const mdrt_net *n);
uint8_t mdrt_net_pads(const mdrt_net *n);                /* the pads this peer plays (bit p: pad p + 1), once ready */
int mdrt_net_delay(const mdrt_net *n);
/* the other player's name ("" when they gave none): a guest's host, a host's latest guest */
const char *mdrt_net_peer_name(const mdrt_net *n);
/* the machines: how many (in the session; a host's waiting room: so far, itself included), this
 * peer's place among them (the host 0), and each one's name, players and pads (pads once ready;
 * a host's waiting room: by place, which may be left empty); every pad played, for the adaptor
 * the ports need (pads 3 and 4: EA's 4 Way Play) */
int mdrt_net_peers(const mdrt_net *n);
int mdrt_net_me(const mdrt_net *n);
const char *mdrt_net_name_of(const mdrt_net *n, int k);
int mdrt_net_players_of(const mdrt_net *n, int k);
uint8_t mdrt_net_pads_of(const mdrt_net *n, int k);
uint8_t mdrt_net_all_pads(const mdrt_net *n);
/* the host's settings, once ready (the host's own on the host): NULL and 0 when it gave none */
const void *mdrt_net_settings(const mdrt_net *n, size_t *size);

/* the session on a console just powered on (frame 0): the module becomes its pad source */
void mdrt_net_start(mdrt_net *n, mdrt *m);
/* At every wait of the client's, after its vertical blank code; resumable: whether the client can
 * go on from here after a state is loaded. MDRT_NET_GO: play the next frame. MDRT_NET_WAIT: not
 * yet (the other's buttons are too far behind, or this peer is ahead and gives a frame back):
 * call again soon (a millisecond or so later), without playing on. MDRT_NET_RESTART: a state was
 * loaded (a rollback): restart at the resumable point and play on. MDRT_NET_RESET: the host reset
 * the console at this frame: it has been powered on again (mdrt_power_on, its save RAM kept): start
 * the client's code again from its power on, and play on in the session (a rollback across it
 * powers it on again as it plays the frame again). MDRT_NET_ENDED: the session is over
 * (mdrt_net_why); the console is as it was and can play on alone. */
int mdrt_net_frame(mdrt_net *n, int resumable);
/* say goodbye (the others' sessions end at once rather than when they stop hearing from this one).
 * Neither this nor mdrt_net_free touches the console, which may be gone by then: a client that
 * plays on alone after it sets its own pad source. (A session that ends inside mdrt_net_frame
 * leaves the console as it was, its pad source the platform's.) */
void mdrt_net_quit(mdrt_net *n);
const char *mdrt_net_why(const mdrt_net *n);

typedef struct mdrt_net_stats {
    uint32_t frame, remote_frame;   /* the last frame played here; the newest every peer this one talks to is
                                       known to have played (a guest's: its host's) */
    uint32_t confirmed;             /* frames whose buttons are all known */
    uint32_t remote_has;            /* a guest: the newest frame of its buttons the host has; a host: of every
                                       player's buttons each guest has */
    int rtt_ms;                     /* the round trip, smoothed (-1: not measured yet; a host: the longest) */
    int delay;
    long rollbacks, replayed;       /* rollbacks, and the frames they played again */
    int last_replayed, most_replayed;
    long waits;                     /* calls that returned MDRT_NET_WAIT */
    long gave_back;                 /* frames given back to keep in step with the others */
    long checks;                    /* desync checks that agreed (a host: one for each guest) */
    long resets;                    /* the host's resets, as they were played (again, after a rollback across one) */
    int lost;                       /* nothing heard for 3 seconds (the session ends after 10) */
} mdrt_net_stats;
void mdrt_net_stats_get(const mdrt_net *n, mdrt_net_stats *s);

/* The session's buttons, frame by frame as they become known for every pad: a line each,
 * "FRAME B1 B2 B3 B4" (decimal, then each pad's MDRT_PAD_* bits in hex), to sink: a replay of the
 * session from power on, and what a desync report plays again. */
void mdrt_net_set_log(mdrt_net *n, mdrt_sink_fn sink, void *user);

#endif
