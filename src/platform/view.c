/* Where the picture goes in a window (mdrt/view.h). */
#include <math.h>
#include "mdrt/view.h"

mdrt_view mdrt_view_place(int win_w, int win_h, int pic_w, int pic_h, int scale, int aspect)
{
    mdrt_view v = { 0, 0, 0, 0, 1 };
    if (win_w <= 0 || win_h <= 0 || pic_w <= 0 || pic_h <= 0) return v;
    double w = aspect == MDRT_ASPECT_4_3 ? pic_h * 4.0 / 3.0 : pic_w;   /* the picture's width at 1x */
    double k = (double)win_w / w < (double)win_h / pic_h ? (double)win_w / w : (double)win_h / pic_h;
    if (scale == MDRT_SCALE_INTEGER && k >= 1) k = floor(k);
    if (scale == MDRT_SCALE_SHARP && k >= 2) v.prescale = (int)floor(k);
    v.w = (int)floor(w * k + 0.5);
    v.h = (int)floor(pic_h * k + 0.5);
    if (v.w > win_w) v.w = win_w;
    if (v.h > win_h) v.h = win_h;
    v.x = (win_w - v.w) / 2;
    v.y = (win_h - v.h) / 2;
    return v;
}
