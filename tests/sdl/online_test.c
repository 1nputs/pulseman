/* The SDL2 platform's Online page played as a player would (make sdl-check): a thread presses keys
 * while a small game runs under SDL's dummy video and real-time disk sound, and another thread is
 * the other player, a host on the loopback (a console of its own, no window, over UDP, what it sends
 * 60-80 ms late, longer than the input delay, so that the window guesses its buttons). The keys
 * open the menu's Online play, type a name, pick "join a host", type the host's address (a first
 * try put back by Esc) and port into their fields, and connect: the game stops and starts again in
 * the session, the host hearing the name and the window's game finding the host's settings at its
 * power on (the window's own asked for once, when its Start made the session). In the session the
 * window's page of the game's options shows the session's choice (the host's, which the game gives
 * it), greyed, its own kept in the settings file.
 * Then pad 1's keys are pressed and let go while both play (so the one ahead has guesses shown
 * wrong to play again), the menu is opened during play (the game does not stop), and the session is
 * left from the menu. It checks that the session played (the window's game and the host in step:
 * rollbacks, desync checks agreeing), that the host heard the goodbye, that the window's
 * game plays on alone, and that the settings file kept the page's choices. The window's save file
 * keeps what its game wrote alone before the session: the session's console, which plays on alone
 * once it is over, writes its save RAM (the session's) to no file.
 * Last the window hosts, on the port the other player hosted on: a player joins (a thread again),
 * the window leaves, and hosts on that port once more, a player joining again (the last session's
 * socket closed before the next is opened on its port). In that session the window saves a state
 * (one both played: of a frame before the window's), then loads it for everyone (the game's check
 * asked, both games going back to it, the session going on in step), and a load the game's check
 * refuses is not sent; the host's Reset, chosen in the menu, powers both consoles on again, the
 * session going on in step; and the window closing in the session saves the Autosave so too, the
 * joiner told the host left. */
#include <SDL2/SDL.h>
#ifdef _WIN32
#include <direct.h>
#define chdir _chdir
#else
#include <unistd.h>
#endif
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "mdrt/net_udp.h"
#include "mdrt/platform_sdl2.h"

#define PORT 7491
#define IDENTITY "online test 1"
static const char host_settings[] = "the host's rules: 7 minute periods";

static SDL_atomic_t settings_asked, settings_found, session_asked, text_value, used_value;

/* the game's option, on its page: the window's own is "a"; the session's (the host's) "c" */
static const char *const rule_names[] = { "a", "b", "c" };
static const mdrt_sdl_option rule = { "test.rule", "Test rule", 3, rule_names, NULL, 0, 0, 0, 0, NULL, NULL, NULL };
static void options_use(const int *v, void *user) { (void)user; SDL_AtomicSet(&used_value, v[0]); }
static void options_text(int page, const int *v, char *text, size_t size, void *user)
{
    (void)page; (void)user;
    SDL_AtomicSet(&text_value, v[0]);
    snprintf(text, size, "Played\t%s\n", rule_names[v[0] % 3]);
}
static int options_session(int *v, void *user)
{
    (void)user;
    SDL_AtomicAdd(&session_asked, 1);
    if (!SDL_AtomicGet(&settings_found)) return 0;        /* (none until the game has the host's) */
    v[0] = 2;
    return 1;
}
static const void *window_settings(size_t *size, void *user)
{
    (void)user;
    static const char mine[] = "the window's rules (not sent: it joins)";
    SDL_AtomicAdd(&settings_asked, 1);
    *size = sizeof mine;
    return mine;
}
/* the game's word on a state to load in a session: refused while refuse_states is set */
static SDL_atomic_t states_checked, refuse_states;
static const char *state_check(const void *state, size_t size, void *user)
{
    (void)state; (void)size; (void)user;
    SDL_AtomicAdd(&states_checked, 1);
    return SDL_AtomicGet(&refuse_states) ? "a state of other rules" : NULL;
}

static SDL_atomic_t frames, online, starts, window_checks, window_rollbacks, host_done, host_checks, host_rollbacks;
static SDL_atomic_t joiner_done, joiner_played, joiner_resets, window_resets;
static SDL_atomic_t window_f, joiner_loads;             /* the window's game's frames (its own count, in its state); the joiner's loads */
static SDL_atomic_t window_loaded_f, joiner_loaded_f;   /* each game's frame as a state was loaded (-1: none yet) */
static char host_why[200], host_heard[64], joiner_why[200];

/* ---- the game, the same on both: the pads read through the I/O chip, into its sum ------------- */

typedef struct game { uint32_t sum, f; } game;
static size_t part_save(void *user, uint8_t *out) { if (out) memcpy(out, user, sizeof(game)); return sizeof(game); }
static int part_load(void *user, const uint8_t *in, size_t size) { if (size != sizeof(game)) return -1; memcpy(user, in, size); return 0; }

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

static void play_frame(mdrt *m, game *g)
{
    if (!g->f) {                                          /* (the VDP set up, on both) */
        static const uint8_t regs[] = { 0x04, 0x44, 0x30, 0x3C, 0x07, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x81, 0x37, 0x00, 0x02, 0x01 };
        for (int r = 0; r < (int)sizeof regs; r++) mdrt_vdp_ctrl16(mdrt_get_vdp(m), (uint16_t)(0x8000 | r << 8 | regs[r]));
    }
    g->sum = g->sum * 31 + read_pad(m, 0) + 7u * read_pad(m, 1);
    g->f++;
    mdrt_vdp_ctrl32(mdrt_get_vdp(m), 0xC0000000);
    mdrt_vdp_data16(mdrt_get_vdp(m), (uint16_t)(g->sum & 0x0EEE));
    mdrt_vblank_end(m);
}

/* the window's: alone, or in the session its console has */
static void window_game(mdrt *m, void *user)
{
    (void)user;
    static game g;
    memset(&g, 0, sizeof g);
    long loads = 0;                                       /* (the session's, from this power on) */
    CHECK_EQ(mdrt_add_state_part(m, "GAME", part_save, part_load, &g), 0);
    SDL_AtomicAdd(&starts, 1);
    if (mdrt_get_net(m)) {                                /* (the session's settings, from power on: the host's) */
        size_t n;
        const void *set = mdrt_net_settings(mdrt_get_net(m), &n);
        SDL_AtomicSet(&settings_found, set && n == sizeof host_settings && !memcmp(set, host_settings, n));
    }
    for (;;) {
        play_frame(m, &g);
        if (mdrt_wait_vblank(m)) return;
        SDL_AtomicAdd(&frames, 1);
        SDL_AtomicSet(&window_f, (int)g.f);
        mdrt_net *n = mdrt_get_net(m);
        if (!n || mdrt_net_state(n) != MDRT_NET_GO) {     /* (alone: its save RAM written, its own or, after a session, the session's) */
            SDL_AtomicSet(&online, 0);
            mdrt_sram_write8(mdrt_get_sram(m), 1, n ? 0x22 : 0x11);
            continue;
        }
        SDL_AtomicSet(&online, 1);
        int r;
        while ((r = mdrt_net_frame(n, 1)) == MDRT_NET_WAIT)
            if (mdrt_idle(m)) { mdrt_net_quit(n); return; }
        if (r == MDRT_NET_RESET) { memset(&g, 0, sizeof g); SDL_AtomicAdd(&window_resets, 1); }   /* (the host's reset: from power on) */
        mdrt_net_stats st;                                /* (a RESTART: the state is loaded, and the loop plays on from it) */
        mdrt_net_stats_get(n, &st);
        if (st.loads != loads) { loads = st.loads; SDL_AtomicSet(&window_loaded_f, (int)g.f); }   /* (the host's state for everyone) */
        SDL_AtomicSet(&window_checks, (int)st.checks);
        SDL_AtomicSet(&window_rollbacks, (int)st.rollbacks);
    }
}

/* the other player: a host on the loopback, its buttons from a pattern, paced by the clock */
static void host_input(void *user, uint16_t b[MDRT_PADS])
{
    uint32_t f = (uint32_t)mdrt_frame_count(user);
    b[0] = (uint16_t)((f / 9) % 3 == 0 ? MDRT_PAD_A : (f / 5) % 2 ? MDRT_PAD_LEFT : 0);
}

static int host(void *user)
{
    (void)user;
    char err[200];
    mdrt_udp *u = mdrt_udp_open(PORT, NULL, err, sizeof err);
    CHECK(u != NULL);
    if (!u) { SDL_AtomicSet(&host_done, 1); return 0; }
    mdrt_udp_simulate(u, 60, 20, 0, 0);                   /* (its buttons late: the window has to guess them) */
    mdrt_config cfg = { 0 };
    cfg.sram_size = 0x2000;                               /* (as the window's: save RAM is in the simulation) */
    mdrt *m = mdrt_new(&cfg);
    static game g;
    CHECK_EQ(mdrt_add_state_part(m, "GAME", part_save, part_load, &g), 0);
    mdrt_net_config c = { .role = MDRT_NET_HOST, .identity = IDENTITY, .name = "Hostess", .delay = 2, .local_input = host_input, .input_user = m,
                          .settings = host_settings, .settings_size = sizeof host_settings };
    c.link = mdrt_udp_link(u);
    mdrt_net *n = mdrt_net_new(&c);
    int r = MDRT_NET_CONNECTING;
    for (Uint32 t0 = SDL_GetTicks(); r == MDRT_NET_CONNECTING && SDL_GetTicks() - t0 < 30000;) { r = mdrt_net_connect(n); mdrt_udp_wait(u, 10); }
    CHECK_EQ(r, MDRT_NET_READY);
    if (r == MDRT_NET_READY) {
        snprintf(host_heard, sizeof host_heard, "%s", mdrt_net_peer_name(n));
        mdrt_udp_lock(u);
        mdrt_net_start(n, m);
        Uint64 freq = SDL_GetPerformanceFrequency(), next = SDL_GetPerformanceCounter();
        for (Uint32 t0 = SDL_GetTicks(); SDL_GetTicks() - t0 < 40000;) {
            play_frame(m, &g);
            mdrt_wait_vblank(m);
            while ((r = mdrt_net_frame(n, 1)) == MDRT_NET_WAIT && SDL_GetTicks() - t0 < 40000) mdrt_udp_wait(u, 1);
            if (r == MDRT_NET_ENDED || r == MDRT_NET_REFUSED) break;
            if (r == MDRT_NET_RESTART || mdrt_quiet(m)) continue;   /* (played again at once) */
            next += freq / 60;
            Uint64 now = SDL_GetPerformanceCounter();
            if (next > now) SDL_Delay((Uint32)((next - now) * 1000 / freq));
            else next = now;
        }
        mdrt_net_stats st;
        mdrt_net_stats_get(n, &st);
        SDL_AtomicSet(&host_checks, (int)st.checks);
        SDL_AtomicSet(&host_rollbacks, (int)st.rollbacks);
        snprintf(host_why, sizeof host_why, "%s", mdrt_net_why(n));
    }
    mdrt_net_free(n);
    mdrt_free(m);
    mdrt_udp_close(u);
    SDL_AtomicSet(&host_done, 1);
    return 0;
}

/* a player joining the window's session on the loopback, until it ends (the window leaving) */
static int joiner(void *user)
{
    (void)user;
    char err[200];
    joiner_why[0] = 0;
    SDL_AtomicSet(&joiner_played, 0);
    char addr[32];
    snprintf(addr, sizeof addr, "127.0.0.1:%d", PORT);
    mdrt_udp *u = mdrt_udp_open(0, addr, err, sizeof err);
    CHECK(u != NULL);
    if (!u) { SDL_AtomicSet(&joiner_done, 1); return 0; }
    mdrt_config cfg = { 0 };
    cfg.sram_size = 0x2000;
    mdrt *m = mdrt_new(&cfg);
    static game g;
    memset(&g, 0, sizeof g);
    SDL_AtomicSet(&joiner_loads, 0);
    SDL_AtomicSet(&joiner_loaded_f, -1);
    CHECK_EQ(mdrt_add_state_part(m, "GAME", part_save, part_load, &g), 0);
    mdrt_net_config c = { .role = MDRT_NET_JOIN, .identity = IDENTITY, .name = "Joiner", .delay = 2, .local_input = host_input, .input_user = m };
    c.link = mdrt_udp_link(u);
    mdrt_net *n = mdrt_net_new(&c);
    int r = MDRT_NET_CONNECTING;
    for (Uint32 t0 = SDL_GetTicks(); r == MDRT_NET_CONNECTING && SDL_GetTicks() - t0 < 15000;) { r = mdrt_net_connect(n); mdrt_udp_wait(u, 10); }
    if (r == MDRT_NET_READY) {
        mdrt_udp_lock(u);
        mdrt_net_start(n, m);
        Uint64 freq = SDL_GetPerformanceFrequency(), next = SDL_GetPerformanceCounter();
        for (Uint32 t0 = SDL_GetTicks(); SDL_GetTicks() - t0 < 40000;) {
            play_frame(m, &g);
            mdrt_wait_vblank(m);
            while ((r = mdrt_net_frame(n, 1)) == MDRT_NET_WAIT && SDL_GetTicks() - t0 < 40000) mdrt_udp_wait(u, 1);
            if (r == MDRT_NET_ENDED || r == MDRT_NET_REFUSED) break;
            if (r == MDRT_NET_RESET) { memset(&g, 0, sizeof g); SDL_AtomicAdd(&joiner_resets, 1); }   /* (the host's reset: from power on) */
            mdrt_net_stats st;
            mdrt_net_stats_get(n, &st);
            if (st.loads != SDL_AtomicGet(&joiner_loads)) SDL_AtomicSet(&joiner_loaded_f, (int)g.f);   /* (the host's state for everyone) */
            SDL_AtomicSet(&joiner_loads, (int)st.loads);
            if (r == MDRT_NET_RESTART || mdrt_quiet(m)) continue;
            SDL_AtomicAdd(&joiner_played, 1);
            next += freq / 60;
            Uint64 now = SDL_GetPerformanceCounter();
            if (next > now) SDL_Delay((Uint32)((next - now) * 1000 / freq));
            else next = now;
        }
    }
    snprintf(joiner_why, sizeof joiner_why, "%s", r == MDRT_NET_CONNECTING ? "never connected" : mdrt_net_why(n));
    mdrt_net_free(n);
    mdrt_free(m);
    mdrt_udp_close(u);
    SDL_AtomicSet(&joiner_done, 1);
    return 0;
}

/* ---- the player at the window --------------------------------------------------------------------- */

static void key(SDL_Scancode k, int down)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = k;
    e.key.keysym.sym = SDL_GetKeyFromScancode(k);
    SDL_PushEvent(&e);
}
static void press(SDL_Scancode k) { key(k, 1); key(k, 0); SDL_Delay(40); }
static void keys(SDL_Scancode k, int n) { for (int i = 0; i < n; i++) press(k); }
static void type(const char *t)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_TEXTINPUT;
    snprintf(e.text.text, sizeof e.text.text, "%s", t);
    SDL_PushEvent(&e);
    SDL_Delay(40);
}
static int until(SDL_atomic_t *a, int v, int ms) { for (int t = 0; t < ms && SDL_AtomicGet(a) != v; t += 10) SDL_Delay(10); return SDL_AtomicGet(a) == v; }

/* a state file's game frame (its last part, the game's: sum, f), or -1 */
static int state_game_f(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    game g;
    int ok = !fseek(f, -(long)sizeof g, SEEK_END) && fread(&g, sizeof g, 1, f) == 1;
    fclose(f);
    return ok ? (int)g.f : -1;
}
static int exists(const char *path) { FILE *f = fopen(path, "rb"); if (f) fclose(f); return f != NULL; }

/* the main menu's Online play (the eighth below Resume: Reset, the two states, Video, Capture,
 * Sound, Players) */
static void online_page(void)
{
    press(SDL_SCANCODE_ESCAPE);
    keys(SDL_SCANCODE_DOWN, 8);
    press(SDL_SCANCODE_RETURN);
}

static int player(void *user)
{
    (void)user;
    while (SDL_AtomicGet(&frames) < 60) SDL_Delay(10);
    CHECK_EQ(SDL_AtomicGet(&online), 0);
    online_page();                                        /* (on Host game, below Name, Role, Port and Input delay) */
    keys(SDL_SCANCODE_UP, 4);
    press(SDL_SCANCODE_RETURN);                           /* the name */
    type("Tester");
    press(SDL_SCANCODE_RETURN);
    press(SDL_SCANCODE_DOWN);
    press(SDL_SCANCODE_RIGHT);                            /* join: Name, Role, Host address, Host port, ... */
    press(SDL_SCANCODE_DOWN);
    press(SDL_SCANCODE_RETURN);
    type("10.9.8.7");
    press(SDL_SCANCODE_ESCAPE);                           /* (put back as it was: empty) */
    press(SDL_SCANCODE_RETURN);
    type("127.0.0.1");
    press(SDL_SCANCODE_RETURN);
    press(SDL_SCANCODE_DOWN);
    press(SDL_SCANCODE_RETURN);
    keys(SDL_SCANCODE_BACKSPACE, 4);
    type("7491");
    press(SDL_SCANCODE_RETURN);
    keys(SDL_SCANCODE_DOWN, 2);                           /* (past Input delay) to Connect */
    press(SDL_SCANCODE_RETURN);                           /* ("Choose it again": a game is running) */
    press(SDL_SCANCODE_RETURN);
    int on = until(&online, 1, 15000);
    CHECK(on);
    int starts_then = SDL_AtomicGet(&starts);
    printf("  the Online page: %s (the game started %d times); the host heard the name \"%s\"\n",
           on ? "connected, playing in the session" : "NOT connected", starts_then, host_heard);
    CHECK_EQ(starts_then, 2);
    CHECK(!strcmp(host_heard, "Tester"));
    printf("  the settings: the window's asked for %d time(s); its game found the host's at power on: %s\n", SDL_AtomicGet(&settings_asked),
           SDL_AtomicGet(&settings_found) ? "yes" : "NO");
    CHECK_EQ(SDL_AtomicGet(&settings_asked), 1);
    CHECK_EQ(SDL_AtomicGet(&settings_found), 1);
    press(SDL_SCANCODE_ESCAPE);                           /* the game's page (below Online play): the session's choice, greyed */
    keys(SDL_SCANCODE_DOWN, 9);
    press(SDL_SCANCODE_RETURN);
    keys(SDL_SCANCODE_LEFT, 2);                           /* (on Back: the option is not to be changed) */
    press(SDL_SCANCODE_ESCAPE);
    press(SDL_SCANCODE_ESCAPE);
    printf("  the game's page in the session: asked %d time(s) for the session's choices, its lines showing %s\n", SDL_AtomicGet(&session_asked),
           rule_names[SDL_AtomicGet(&text_value) % 3]);
    CHECK(SDL_AtomicGet(&session_asked) >= 1);
    CHECK_EQ(SDL_AtomicGet(&text_value), 2);
    CHECK_EQ(SDL_AtomicGet(&used_value), 0);              /* (the game was powered on with the window's own: its settings say which it plays) */
    /* both play: pad 1's keys pressed and let go, so the host's guesses of this player go wrong */
    static const SDL_Scancode pads[] = { SDL_SCANCODE_RIGHT, SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_LEFT };
    for (int i = 0; i < 24; i++) { key(pads[i % 4], 1); SDL_Delay(90 + 37 * (i % 3)); key(pads[i % 4], 0); SDL_Delay(60); }
    int f0 = SDL_AtomicGet(&frames);                      /* the menu open while playing: the game goes on */
    press(SDL_SCANCODE_ESCAPE);
    SDL_Delay(500);
    int f1 = SDL_AtomicGet(&frames);
    press(SDL_SCANCODE_ESCAPE);
    printf("  the menu open in the session: %d frames went by in half a second\n", f1 - f0);
    CHECK(f1 - f0 > 15);
    SDL_Delay(1500);
    online_page();                                        /* (on Leave session) */
    press(SDL_SCANCODE_RETURN);
    CHECK(until(&host_done, 1, 5000));
    CHECK(until(&online, 0, 5000));
    f0 = SDL_AtomicGet(&frames);
    SDL_Delay(500);
    f1 = SDL_AtomicGet(&frames);
    printf("  left from the menu: the host heard \"%s\"; the game plays on alone (%d frames in half a second)\n", host_why, f1 - f0);
    CHECK(!strcmp(host_why, "Tester left"));
    CHECK(f1 - f0 > 15);
    printf("  in step: the window rolled back %d times and the host %d; desync checks agreeing %d and %d\n",
           SDL_AtomicGet(&window_rollbacks), SDL_AtomicGet(&host_rollbacks), SDL_AtomicGet(&window_checks), SDL_AtomicGet(&host_checks));
    CHECK(SDL_AtomicGet(&window_rollbacks) > 0);          /* (the host's buttons come late: guessed) */
    CHECK(SDL_AtomicGet(&window_checks) >= 2 && SDL_AtomicGet(&host_checks) >= 2);
    /* the window hosts on the port the other hosted on, is joined, leaves, and hosts there again */
    online_page();                                        /* (on Connect: below Name, Role, Host address, Host port, Input delay) */
    keys(SDL_SCANCODE_UP, 4);
    press(SDL_SCANCODE_LEFT);                             /* host: Name, Role, Port (7491), Input delay, Host game */
    keys(SDL_SCANCODE_DOWN, 3);
    for (int round = 1; round <= 2; round++) {
        press(SDL_SCANCODE_RETURN);                       /* (Host game, and "Choose it again": a game is running) */
        press(SDL_SCANCODE_RETURN);
        SDL_AtomicSet(&joiner_done, 0);
        SDL_Thread *j = SDL_CreateThread(joiner, "joiner", NULL);
        int hosting = until(&online, 1, 15000);
        for (int t = 0; t < 5000 && SDL_AtomicGet(&joiner_played) < 60; t += 10) SDL_Delay(10);
        printf("  hosting from the page, time %d: %s (the joiner played %d frames)\n", round,
               hosting ? "joined, playing" : "NOT joined", SDL_AtomicGet(&joiner_played));
        CHECK(hosting);
        CHECK(SDL_AtomicGet(&joiner_played) >= 60);
        if (round == 2) {                                 /* a state saved in the session (the menu's Save state, slot 1), loaded for everyone */
            press(SDL_SCANCODE_ESCAPE);
            keys(SDL_SCANCODE_DOWN, 2);
            keys(SDL_SCANCODE_RETURN, 2);
            for (int t = 0; t < 3000 && !exists("states/slot1.state"); t += 10) SDL_Delay(10);
            SDL_Delay(100);
            int saved = state_game_f("states/slot1.state"), at = SDL_AtomicGet(&window_f);
            printf("  saved in the session: the game's frame %d (the window's at %d)%s\n", saved, at, exists("states/slot1.png") ? ", its picture beside it" : "");
            CHECK(saved > 0 && saved <= at);
            CHECK(exists("states/slot1.png"));
            SDL_Delay(1500);
            int checks0 = SDL_AtomicGet(&window_checks);
            SDL_AtomicSet(&window_loaded_f, -1);
            press(SDL_SCANCODE_ESCAPE);
            keys(SDL_SCANCODE_DOWN, 3);
            keys(SDL_SCANCODE_RETURN, 2);                 /* (Load state: slot 1, the one chosen last) */
            int loaded = until(&joiner_loads, 1, 5000);
            SDL_Delay(200);
            for (int t = 0; t < 8000 && SDL_AtomicGet(&window_checks) < checks0 + 2; t += 10) SDL_Delay(10);
            printf("  loaded for everyone: %s (asked the game %d time(s)); both games back at the state's frame %d: the window's %d, the joiner's %d;\n"
                   "  then %d desync checks more agreeing\n", loaded ? "yes" : "NO", SDL_AtomicGet(&states_checked), saved,
                   SDL_AtomicGet(&window_loaded_f), SDL_AtomicGet(&joiner_loaded_f), SDL_AtomicGet(&window_checks) - checks0);
            CHECK(loaded);
            CHECK_EQ(SDL_AtomicGet(&states_checked), 1);
            CHECK_EQ(SDL_AtomicGet(&window_loaded_f), saved);
            CHECK_EQ(SDL_AtomicGet(&joiner_loaded_f), saved);
            CHECK(SDL_AtomicGet(&window_checks) >= checks0 + 2);
            SDL_AtomicSet(&refuse_states, 1);             /* the game's check refusing: nothing sent */
            press(SDL_SCANCODE_ESCAPE);
            keys(SDL_SCANCODE_DOWN, 3);
            keys(SDL_SCANCODE_RETURN, 2);
            SDL_Delay(1500);
            printf("  a load the game's check refuses: asked %d time(s), the joiner's loads %d\n", SDL_AtomicGet(&states_checked), SDL_AtomicGet(&joiner_loads));
            CHECK_EQ(SDL_AtomicGet(&states_checked), 2);
            CHECK_EQ(SDL_AtomicGet(&joiner_loads), 1);
            SDL_AtomicSet(&refuse_states, 0);
        }
        if (round == 2) {                                 /* the host's Reset (below Resume: no Game ROM here), chosen again: the console reset for all */
            int checks0 = SDL_AtomicGet(&window_checks), played0 = SDL_AtomicGet(&joiner_played);
            press(SDL_SCANCODE_ESCAPE);
            press(SDL_SCANCODE_DOWN);
            press(SDL_SCANCODE_RETURN);
            press(SDL_SCANCODE_RETURN);
            int reset = until(&joiner_resets, 1, 5000) && until(&window_resets, 1, 5000);
            for (int t = 0; t < 8000 && (SDL_AtomicGet(&window_checks) < checks0 + 2 || SDL_AtomicGet(&joiner_played) < played0 + 120); t += 10)
                SDL_Delay(10);
            printf("  the host's Reset in the session: the window's console powered on again %d time(s), the joiner's %d; then %d desync checks more\n"
                   "  agreeing, the joiner %d frames on\n", SDL_AtomicGet(&window_resets), SDL_AtomicGet(&joiner_resets),
                   SDL_AtomicGet(&window_checks) - checks0, SDL_AtomicGet(&joiner_played) - played0);
            CHECK(reset);
            CHECK(SDL_AtomicGet(&window_checks) >= checks0 + 2);
            CHECK(SDL_AtomicGet(&online) == 1);
        }
        if (round == 2) {                                 /* the window closed in the session: the Autosave first (both played it) */
            SDL_Event e;
            SDL_zero(e);
            e.type = SDL_QUIT;
            SDL_PushEvent(&e);
            CHECK(until(&joiner_done, 1, 5000));
            SDL_WaitThread(j, NULL);
            printf("  the window closed in the session: the joiner heard \"%s\"\n", joiner_why);
            CHECK(!strcmp(joiner_why, "Tester left"));
            return 0;
        }
        online_page();                                    /* (on Leave session) */
        press(SDL_SCANCODE_RETURN);
        CHECK(until(&joiner_done, 1, 5000));
        CHECK(until(&online, 0, 5000));
        SDL_WaitThread(j, NULL);
        printf("  the window left: the joiner heard \"%s\"\n", joiner_why);
        CHECK(!strcmp(joiner_why, "Tester left"));
        online_page();                                    /* (on Host game, for the next round) */
    }
    return 0;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    static char b[65536];
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    return b;
}

int main(int argc, char **argv)
{
    if (chdir(argc > 1 ? argv[1] : "build/sdlnet")) { perror("chdir"); return 1; }
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("SDL_AUDIODRIVER", "disk", 1);
    SDL_setenv("SDL_DISKAUDIOFILE", "sound.raw", 1);
    remove("settings.ini");
    remove("sound.raw");
    remove("online-test.srm");
    static const char *const old[] = { "states/slot1.state", "states/slot1.png", "states/autosave.state", "states/autosave.png" };
    for (int i = 0; i < 4; i++) remove(old[i]);
    mdrt_config c = { 0 };
    c.audio_rate = 48000;
    c.sram_size = 0x2000;
    mdrt_sdl_options o = { .title = "online test", .app = "online-test", .net_identity = IDENTITY, .net_settings = window_settings, .net_states = 1, .state_check = state_check, .net_reset = 1, .sram_path = "online-test.srm",
                           .options_title = "Test rules", .options = &rule, .noptions = 1, .options_use = options_use,
                           .options_text = options_text, .options_session = options_session };
    SDL_Thread *h = SDL_CreateThread(host, "host", NULL), *p = SDL_CreateThread(player, "player", NULL);
    CHECK_EQ(mdrt_sdl_run(&c, &o, window_game, NULL), 0);
    SDL_WaitThread(p, NULL);
    SDL_WaitThread(h, NULL);
    const char *set = slurp("settings.ini");
    CHECK(set && strstr(set, "netplay.role = host") && strstr(set, "netplay.address = 127.0.0.1") && strstr(set, "netplay.port = 7491") &&
          strstr(set, "netplay.name = Tester") && strstr(set, "test.rule = a"));
    printf("  the settings kept: %s\n", set && strstr(set, "netplay.address = 127.0.0.1") ? "host, 127.0.0.1 kept, port 7491" : "NOT kept");
    FILE *f = fopen("online-test.srm", "rb");
    int first = f ? fgetc(f) : -1;
    if (f) fclose(f);
    printf("  the save file: %s\n", first == 0x11 ? "as the game alone left it (the session's console wrote none)" : first == 0x22 ? "OVERWRITTEN by the session's console" : "missing");
    CHECK_EQ(first, 0x11);
    int autosaved = state_game_f("states/autosave.state");
    printf("  the Autosave, from the session: the game's frame %d\n", autosaved);
    CHECK(autosaved > 0);
    return check_done("online_test");
}
