/* Pixel-art magnification of the picture: each source pixel becomes a block of 2 x 2 (3 x 3, 4 x 4)
 * pixels, by rules that round staircases and keep lines and outlines whole. Scale2x, Scale3x and
 * MMPX make each a copy of a pixel nearby (no new colours: the palette and the crispness kept);
 * xBR blends along the edges it finds (smooth, anti-aliased curves, new colours between).
 *   scale2x, scale3x: EPX (Eric Johnston, 1992) as Andrea Mazzoleni's Scale2x and Scale3x (2001),
 *            written here from the rules published at scale2x.it
 *   mmpx2x:  MMPX (Morgan McGuire and Mara Gagiu, "MMPX Style-Preserving Pixel Art
 *            Magnification", JCGT 10(2), 2021), following the authors' MIT-licensed C++
 *   mmpx4x:  MMPX twice
 *   xbr2x, xbr3x, xbr4x: Hyllian's xBR-lv2 (2011-2016), following his MIT-licensed shader: its
 *            defaults, the blend at edges a pixel of the output wide
 * Pixels are 0xAARRGGBB (MMPX takes dark, opaque pixels for the foreground where a pattern is
 * ambiguous); outside the picture, its edge pixels repeat. No SDL: the SDL2 platform scales the
 * game's frames with it (never the menu), and a browser's platform can do the same. */
#ifndef MDRT_UPSCALE_H
#define MDRT_UPSCALE_H
#include <stdint.h>

enum { MDRT_UPSCALE_OFF, MDRT_UPSCALE_SCALE2X, MDRT_UPSCALE_SCALE3X, MDRT_UPSCALE_MMPX2X, MDRT_UPSCALE_MMPX4X,
       MDRT_UPSCALE_XBR2X, MDRT_UPSCALE_XBR3X, MDRT_UPSCALE_XBR4X, MDRT_UPSCALE_COUNT };

/* how many times larger a method makes the picture (1 for off) */
int mdrt_upscale_factor(int method);
/* its name in the settings ("off", "scale2x", "scale3x", "mmpx2x", "mmpx4x", "xbr2x", "xbr3x",
 * "xbr4x") and for the player */
const char *mdrt_upscale_name(int method);
const char *mdrt_upscale_label(int method);
/* in: w x h pixels, pitch pixels a row; out: w x h times the factor, out_pitch pixels a row.
 * mmpx4x needs scratch, room for 4 w h pixels; xBR uses it when given (NULL: it takes its own);
 * NULL is fine for the others. */
void mdrt_upscale(int method, const uint32_t *in, int w, int h, int pitch, uint32_t *out, int out_pitch, uint32_t *scratch);

#endif
