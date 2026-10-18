/* Captures: the game's frames kept as they are made, to be saved as an animated GIF. Each frame is
 * kept compressed, its exact colours and GIF's LZW (mdrt/gif.h: a game's play, about 8 KB a
 * frame). A capture keeps the last so many frames in memory (a replay: the last 20 seconds, taken
 * when the player asks), and can also write every frame to a file as it comes (a recording).
 * Either becomes a clip, read here into a GIF (lossless, at 30 frames a second: GIF's times are in
 * hundredths, and players show a hundredth as a tenth, so 60 cannot be had). No SDL; not
 * thread-safe (the platform locks around it). */
#ifndef MDRT_CAPTURE_H
#define MDRT_CAPTURE_H
#include <stdint.h>
#include <stdio.h>
#include "mdrt/gif.h"

typedef struct mdrt_capture mdrt_capture;
typedef struct mdrt_clip mdrt_clip;

/* a frame as kept */
typedef struct mdrt_capture_frame {
    int w, h, colours, bits;
    const uint16_t *palette;    /* VDP colours (mdrt_vdp_rgb) */
    const uint8_t *code;        /* GIF's code stream of its pixels */
    size_t code_len;
} mdrt_capture_frame;

/* keep: the frames kept in memory, the latest (0: none, only a recording) */
mdrt_capture *mdrt_capture_new(int keep);
void mdrt_capture_free(mdrt_capture *c);
void mdrt_capture_keep(mdrt_capture *c, int keep);       /* (fewer: the oldest go) */
/* a frame (VDP colours, pitch pixels a row): kept, and written to the recording */
void mdrt_capture_picture(mdrt_capture *c, const uint16_t *px, int w, int h, int pitch);
/* every frame from now also written to f (NULL: no longer); the frames written so far, or -1 when
 * a write failed (the recording stopped) */
void mdrt_capture_record(mdrt_capture *c, FILE *f);
long mdrt_capture_recorded(const mdrt_capture *c);
int mdrt_capture_kept(const mdrt_capture *c);
/* the frames kept, taken out as a clip (the capture keeps on from none); NULL when there are none */
mdrt_clip *mdrt_capture_take(mdrt_capture *c);

/* a recording's file as a clip (from its start; the clip reads it, the caller closes it after) */
mdrt_clip *mdrt_clip_file(FILE *f);
void mdrt_clip_free(mdrt_clip *clip);
/* the frames in turn (0 at the end, or a frame that cannot be read); from the start again */
int mdrt_clip_next(mdrt_clip *clip, mdrt_capture_frame *f);
void mdrt_clip_rewind(mdrt_clip *clip);
/* its frames' count, and the size most of them have (a game changes it on its menus) */
long mdrt_clip_frames(mdrt_clip *clip, int *w, int *h);

/* as a GIF through put: the colours by mdrt_vdp_rgb's curve, scale times the size, every frame
 * (fps 50 or fewer) or every other; done is set to the frames read as it goes (may be NULL);
 * 0 when a frame could not be read or made */
int mdrt_clip_gif(mdrt_clip *clip, int curve, int scale, double fps, mdrt_gif_put put, void *user, volatile long *done);

#endif
