/* GIF (GIF89a, as CompuServe's specification of 1990 describes it): the game's frames as an
 * animated picture, losslessly. A console's frame has at most 64 colours at once (more only when
 * its colours change while it is drawn), so each frame keeps its exact colours in a table of its
 * own; its pixels, indices into that table, are compressed with GIF's variable-length LZW. No SDL:
 * the SDL2 platform's captures (mdrt/capture.h) are made with it, and a browser's can be. */
#ifndef MDRT_GIF_H
#define MDRT_GIF_H
#include <stddef.h>
#include <stdint.h>

/* a frame of VDP colours (w x h, pitch pixels a row) as indices into its palette, the colours in
 * the order first seen; the palette's size (at most 256: beyond, a colour is the nearest of them) */
int mdrt_gif_palette(const uint16_t *px, int w, int h, int pitch, uint8_t *idx, uint16_t pal[256]);
/* the bits an index needs for so many colours (GIF's minimum code size: 2 to 8) */
int mdrt_gif_bits(int colours);

/* n indices of that many bits LZW-compressed as GIF's code stream (without its sub-blocks); the
 * bytes written, 0 when they would not fit in cap (mdrt_gif_lzw_bound(n) is room for any) */
size_t mdrt_gif_lzw(const uint8_t *idx, size_t n, int bits, uint8_t *out, size_t cap);
size_t mdrt_gif_lzw_bound(size_t n);
/* back: n indices from a code stream; 0 when it is not one or ends before n (a reader's check) */
int mdrt_gif_unlzw(const uint8_t *in, size_t len, int bits, uint8_t *idx, size_t n);

/* a file through put: its start (the screen w x h, looping for ever), each frame (at x, y, w x h,
 * shown for delay hundredths of a second, its colours as 0xRRGGBB, its code stream), its end */
typedef void (*mdrt_gif_put)(void *user, const void *bytes, size_t n);
void mdrt_gif_begin(mdrt_gif_put put, void *user, int w, int h);
void mdrt_gif_frame(mdrt_gif_put put, void *user, int x, int y, int w, int h, int delay,
                    const uint32_t *rgb, int colours, int bits, const uint8_t *code, size_t len);
void mdrt_gif_end(mdrt_gif_put put, void *user);

#endif
