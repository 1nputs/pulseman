/* How the game's picture looks past its pixels: dithering blended the way a television's signal
 * blended it, and a picture tube's scanlines and mask. (The colours' levels are mdrt_vdp_rgb's
 * curves.) No SDL: the SDL2 platform applies them to the game's frames (never the menu), and a
 * browser's platform can do the same with a canvas.
 *   blending:  dithering (columns of one colour between columns of another, which the console's
 *              composite signal blurred into a third colour on a television: its waterfalls and
 *              shadows) mixed where it is found, or every pixel softened with its neighbours across
 *   scanlines: the dark gaps between the tube's lines, at the picture's own lines whatever its size
 *   mask:      the tube's phosphors: an aperture grille's stripes, a slot mask's, or a shadow mask's
 *              dots, at the window's pixels
 * The scanlines and the mask are an overlay the size of the picture in the window: the platform
 * multiplies the picture by it (SDL_BLENDMODE_MOD), then adds the picture again at the lift
 * (SDL_BLENDMODE_ADD, the colour modulated by it), which makes up the light they take: dark and
 * middle colours keep their brightness on average, the brightest reach white, as a tube's did.
 * Pixels are 0xAARRGGBB. */
#ifndef MDRT_LOOK_H
#define MDRT_LOOK_H
#include <stdint.h>

enum { MDRT_BLEND_OFF, MDRT_BLEND_DITHER, MDRT_BLEND_SOFT, MDRT_BLEND_COUNT };
enum { MDRT_SCANLINES_OFF, MDRT_SCANLINES_LIGHT, MDRT_SCANLINES_MEDIUM, MDRT_SCANLINES_HEAVY, MDRT_SCANLINES_COUNT };
enum { MDRT_MASK_OFF, MDRT_MASK_APERTURE, MDRT_MASK_SLOT, MDRT_MASK_DOT, MDRT_MASK_COUNT };

/* in: w x h pixels, pitch pixels a row; out (not in): the same size, out_pitch pixels a row */
void mdrt_look_blend(int mode, const uint32_t *in, int w, int h, int pitch, uint32_t *out, int out_pitch);

/* whether the scanlines and mask make an overlay at all */
int mdrt_look_tube(int scanlines, int mask);
/* the overlay for a picture of cols x lines pixels shown w x h in the window, each pixel a factor
 * for its window pixel (0xFFRRGGBB; 255 is 1); and the lift, the picture added again (0-255) */
void mdrt_look_overlay(int scanlines, int mask, int w, int h, int cols, int lines, uint32_t *out, int pitch);
int mdrt_look_lift(int scanlines, int mask);

#endif
