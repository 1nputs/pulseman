/* Where the picture goes in a window: the scaling modes and the aspect, as arithmetic a platform
 * applies (the SDL2 platform's renderer; a browser canvas the same way).
 *   sharp:   as large as fits, enlarged first by the largest whole multiple with no smoothing, then
 *            smoothly to the size: no uneven pixel columns, no blur ("sharp bilinear")
 *   integer: the largest whole multiple that fits, centred: every pixel the same size
 *   fit:     as large as fits, without smoothing (pixel columns of uneven width at most sizes) */
#ifndef MDRT_VIEW_H
#define MDRT_VIEW_H

enum { MDRT_SCALE_SHARP, MDRT_SCALE_INTEGER, MDRT_SCALE_FIT };
enum { MDRT_ASPECT_SQUARE, MDRT_ASPECT_4_3 };      /* square pixels, or the 4:3 a television showed */

typedef struct mdrt_view {
    int x, y, w, h;             /* the picture's rectangle in the window */
    int prescale;               /* sharp: the whole multiple to enlarge by first (1 otherwise) */
} mdrt_view;

/* a picture of pic_w x pic_h in a window of win_w x win_h (in pixels of the drawing surface) */
mdrt_view mdrt_view_place(int win_w, int win_h, int pic_w, int pic_h, int scale, int aspect);

#endif
