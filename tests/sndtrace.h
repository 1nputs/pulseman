/* Reading a sound trace (.sndtrace, version 3; written by ../nhl95/tools/hybrid/oracle_shim.c):
 * the chip writes in time order and the oracle's audio. */
#ifndef MDRT_TESTS_SNDTRACE_H
#define MDRT_TESTS_SNDTRACE_H
#include <stddef.h>
#include <stdint.h>

enum { ST_YM = 1, ST_PSG, ST_Z80RAM, ST_BUSREQ, ST_Z80RESET, ST_AUDIO, ST_END };

typedef struct st_event {
    uint8_t tag;
    uint64_t time;              /* master clocks since the trace started */
    uint8_t a, v;               /* ST_YM: port (0-3) and value; ST_PSG: v; ST_Z80RAM: v */
    uint16_t addr;              /* ST_Z80RAM: address; ST_BUSREQ, ST_Z80RESET: the value */
} st_event;

typedef struct sndtrace {
    uint8_t *data;
    size_t len, pos;
    uint32_t rate;              /* the oracle's audio rate (Hz) */
    unsigned fm_preamp, psg_preamp;   /* percent; 0: that chip is not in the audio */
    int power_on;               /* recorded from power on: every chip write is in the trace */
    uint64_t high;              /* the times' wraps (they are stored modulo 2^32) */
    uint32_t last;
    int16_t *audio;             /* all of its audio: stereo frames */
    size_t frames;              /* how many */
    uint64_t end;               /* the time of the last frame's end */
} sndtrace;

int sndtrace_load(sndtrace *t, const char *path);      /* 0: not a version 3 trace */
int sndtrace_next(sndtrace *t, st_event *e);           /* the next event; 0 at the end */
void sndtrace_free(sndtrace *t);

#endif
