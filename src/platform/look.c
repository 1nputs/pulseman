/* How the game's picture looks past its pixels (mdrt/look.h). */
#include <math.h>
#include "mdrt/look.h"

/* ---- blending ------------------------------------------------------------------------------- */

static uint32_t mix2(uint32_t a, uint32_t b)               /* half of each, channel by channel */
{
    return 0xFF000000u | ((((a >> 16 & 0xFF) + (b >> 16 & 0xFF) + 1) >> 1) << 16)
         | ((((a >> 8 & 0xFF) + (b >> 8 & 0xFF) + 1) >> 1) << 8) | (((a & 0xFF) + (b & 0xFF) + 1) >> 1);
}

static uint32_t mix121(uint32_t l, uint32_t c, uint32_t r)  /* a quarter, a half and a quarter */
{
    uint32_t out = 0xFF000000u;
    for (int sh = 0; sh <= 16; sh += 8)
        out |= (((l >> sh & 0xFF) + 2 * (c >> sh & 0xFF) + (r >> sh & 0xFF) + 2) >> 2) << sh;
    return out;
}

/* dithering: a pixel between two of another colour, with its own colour again two along (so a
 * lone column, a line one pixel wide, stays as it is), becomes the two colours' mix; soft: every
 * pixel a quarter of each neighbour across. Outside the picture its edge pixels repeat. */
void mdrt_look_blend(int mode, const uint32_t *in, int w, int h, int pitch, uint32_t *out, int out_pitch)
{
    for (int y = 0; y < h; y++) {
        const uint32_t *p = in + y * pitch;
        uint32_t *o = out + y * out_pitch;
        for (int x = 0; x < w; x++) {
            uint32_t c = p[x], l = p[x > 0 ? x - 1 : 0], r = p[x < w - 1 ? x + 1 : w - 1];
            if (mode == MDRT_BLEND_SOFT) o[x] = mix121(l, c, r);
            else if (mode == MDRT_BLEND_DITHER && l == r && c != l
                     && ((x >= 2 && p[x - 2] == c) || (x + 2 < w && p[x + 2] == c))) o[x] = mix2(c, l);
            else o[x] = c;
        }
    }
}

/* ---- the tube ---------------------------------------------------------------------------------- */

/* the scanlines: how dark the gap gets. The gap is the second half of each line, darkest three
 * quarters of the way down (a raised cosine): so at twice the size one row in two is darker, and
 * larger, the gap is a soft band. On average a line loses a quarter of its depth. */
static const double scan_depth[MDRT_SCANLINES_COUNT] = { 0, 0.30, 0.50, 0.70 };
/* the mask: how much of the other two channels a phosphor's column (or dot) lets through less; a
 * slot mask's breaks, between slots, darker by as much again */
static const double mask_depth[MDRT_MASK_COUNT] = { 0, 0.30, 0.30, 0.35 };

static double gap(double t)                                 /* t in [0, 1): down a line */
{
    return t < 0.5 ? 0 : (1 - cos((t - 0.5) * 4 * 3.14159265358979)) / 2;
}

/* the light left on average, of the scanlines and of the mask */
static double kept(int scanlines, int mask)
{
    double k = 1 - scan_depth[scanlines] / 4;
    if (mask) k *= 1 - mask_depth[mask] * 2 / 3;
    if (mask == MDRT_MASK_SLOT) k *= 1 - mask_depth[mask] / 4;
    return k;
}

int mdrt_look_tube(int scanlines, int mask)
{
    return (scanlines > 0 && scanlines < MDRT_SCANLINES_COUNT) || (mask > 0 && mask < MDRT_MASK_COUNT);
}

/* the mask's factor for a channel (0 red, 1 green, 2 blue) at window pixel x, y. With fewer than
 * three window pixels to a picture pixel the stripes are magenta and green, two pixels apart. */
static double mask_at(int mask, int x, int y, int ch, int narrow)
{
    double d = mask_depth[mask];
    int lit;
    if (narrow) lit = (x & 1) ? ch == 1 : ch != 1;
    else if (mask == MDRT_MASK_DOT) lit = (x + (y & 1) * 2) % 3 == ch;   /* (the rows' triads offset) */
    else lit = x % 3 == ch;
    double f = lit ? 1 : 1 - d;
    if (mask == MDRT_MASK_SLOT && (y + ((narrow ? x / 2 : x / 3) & 1) * 2) % 4 == 3) f *= 1 - d;   /* (staggered) */
    return f;
}

/* the picture is multiplied by f x b, b the light lost made up (so on average none is): the
 * overlay is f x b - lift, the lift as much of b - 1 as the darkest point allows */
static double gain(int scanlines, int mask) { double b = 1 / kept(scanlines, mask); return b > 1.8 ? 1.8 : b; }

static double lift(int scanlines, int mask)
{
    double darkest = (1 - scan_depth[scanlines]) * (mask ? 1 - mask_depth[mask] : 1) * (mask == MDRT_MASK_SLOT ? 1 - mask_depth[mask] : 1);
    double b = gain(scanlines, mask);
    return b - 1 < darkest * b ? b - 1 : darkest * b;
}

static int valid(int *scanlines, int *mask)
{
    if (*scanlines < 0 || *scanlines >= MDRT_SCANLINES_COUNT) *scanlines = 0;
    if (*mask < 0 || *mask >= MDRT_MASK_COUNT) *mask = 0;
    return *scanlines || *mask;
}

int mdrt_look_lift(int scanlines, int mask)
{
    return valid(&scanlines, &mask) ? (int)floor(lift(scanlines, mask) * 255 + 0.5) : 0;
}

void mdrt_look_overlay(int scanlines, int mask, int w, int h, int cols, int lines, uint32_t *out, int pitch)
{
    int on = valid(&scanlines, &mask), narrow = cols > 0 && w < 3 * cols;
    double b = on ? gain(scanlines, mask) : 1, up = on ? floor(lift(scanlines, mask) * 255 + 0.5) / 255 : 0;
    double m[4][12][3];                                     /* (the masks repeat every 12 columns and 4 rows) */
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 12; x++)
            for (int ch = 0; ch < 3; ch++) m[y][x][ch] = mask ? mask_at(mask, x, y, ch, narrow) : 1;
    for (int y = 0; y < h; y++) {
        double row = 1;
        if (scanlines && lines > 0) {                       /* the gap's share of the pixel's height */
            double a = (double)y * lines / h, span = (double)lines / h, dark = 0;
            for (int i = 0; i < 16; i++) {
                double t = a + (i + 0.5) / 16 * span;
                dark += gap(t - floor(t));
            }
            row = 1 - scan_depth[scanlines] * dark / 16;
        }
        uint32_t pattern[12], *o = out + y * pitch;
        for (int x = 0; x < 12; x++) {
            pattern[x] = 0xFF000000u;
            for (int ch = 0; ch < 3; ch++) {
                double f = row * m[y & 3][x][ch] * b - up;
                pattern[x] |= (uint32_t)floor((f < 0 ? 0 : f > 1 ? 1 : f) * 255 + 0.5) << (16 - 8 * ch);
            }
        }
        for (int x = 0; x < w; x++) o[x] = pattern[x % 12];
    }
}
