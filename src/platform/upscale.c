/* Pixel-art magnification (mdrt/upscale.h).
 *
 * MMPX follows the authors' reference C++ (the code supplement of the paper), which carries this
 * notice:
 *
 *   Copyright 2020 Morgan McGuire & Mara Gagiu
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 *   and associated documentation files (the "Software"), to deal in the Software without
 *   restriction, including without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
 *   Software is furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all copies or
 *   substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 *   BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 *   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * xBR follows Hyllian's xBR-lv2 shader (libretro's glsl-shaders, xbr/shaders/xbr-lv2.glsl), which
 * carries this notice:
 *
 *   Copyright (C) 2011-2016 Hyllian - sergiogdb@gmail.com
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 *   and associated documentation files (the "Software"), to deal in the Software without
 *   restriction, including without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
 *   Software is furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all copies or
 *   substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 *   BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 *   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 *   Incorporates some of the ideas from SABR shader. Thanks to Joshua Street. */
#include <math.h>
#include <stdlib.h>
#include "mdrt/upscale.h"

static const struct { int factor; const char *name, *label; } methods[MDRT_UPSCALE_COUNT] = {
    { 1, "off", "off" }, { 2, "scale2x", "Scale2x" }, { 3, "scale3x", "Scale3x" },
    { 2, "mmpx2x", "MMPX 2x" }, { 4, "mmpx4x", "MMPX 4x" },
    { 2, "xbr2x", "xBR 2x" }, { 3, "xbr3x", "xBR 3x" }, { 4, "xbr4x", "xBR 4x" },
};

int mdrt_upscale_factor(int method) { return method > 0 && method < MDRT_UPSCALE_COUNT ? methods[method].factor : 1; }
const char *mdrt_upscale_name(int method) { return methods[method > 0 && method < MDRT_UPSCALE_COUNT ? method : 0].name; }
const char *mdrt_upscale_label(int method) { return methods[method > 0 && method < MDRT_UPSCALE_COUNT ? method : 0].label; }

typedef struct image { const uint32_t *px; int w, h, pitch; } image;

/* a source pixel; outside the picture, the nearest edge pixel */
static uint32_t src(const image *im, int x, int y)
{
    x = x < 0 ? 0 : x >= im->w ? im->w - 1 : x;
    y = y < 0 ? 0 : y >= im->h ? im->h - 1 : y;
    return im->px[y * im->pitch + x];
}

/* ---- Scale2x and Scale3x: B above E, D left, F right, H below; A C G I the corners ------------ */

static void scale2x(const image *im, uint32_t *out, int op)
{
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            uint32_t B = src(im, x, y - 1), D = src(im, x - 1, y), E = src(im, x, y), F = src(im, x + 1, y), H = src(im, x, y + 1);
            uint32_t *o = out + 2 * y * op + 2 * x;
            o[0] = D == B && B != F && D != H ? D : E;
            o[1] = B == F && B != D && F != H ? F : E;
            o[op] = D == H && D != B && H != F ? D : E;
            o[op + 1] = H == F && D != H && B != F ? F : E;
        }
}

static void scale3x(const image *im, uint32_t *out, int op)
{
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            uint32_t A = src(im, x - 1, y - 1), B = src(im, x, y - 1), C = src(im, x + 1, y - 1);
            uint32_t D = src(im, x - 1, y), E = src(im, x, y), F = src(im, x + 1, y);
            uint32_t G = src(im, x - 1, y + 1), H = src(im, x, y + 1), I = src(im, x + 1, y + 1);
            int db = D == B && B != F && D != H, bf = B == F && B != D && F != H;   /* the four corners' rules */
            int dh = D == H && D != B && H != F, hf = H == F && D != H && B != F;
            uint32_t *o = out + 3 * y * op + 3 * x;
            o[0] = db ? D : E;
            o[1] = (db && E != C) || (bf && E != A) ? B : E;
            o[2] = bf ? F : E;
            o[op] = (db && E != G) || (dh && E != A) ? D : E;
            o[op + 1] = E;
            o[op + 2] = (bf && E != I) || (hf && E != C) ? F : E;
            o[2 * op] = dh ? D : E;
            o[2 * op + 1] = (dh && E != I) || (hf && E != G) ? H : E;
            o[2 * op + 2] = hf ? F : E;
        }
}

/* ---- MMPX: the 3 x 3 grid A-I around E, and P, Q, R, S two away above, left, right, below -------- */

/* coarse brightness with transparency: dark or opaque pixels are the foreground */
static uint32_t luma(uint32_t c) { return ((c >> 16 & 0xFF) + (c >> 8 & 0xFF) + (c & 0xFF) + 1) * (256 - (c >> 24)); }
static int all_eq2(uint32_t b, uint32_t a0, uint32_t a1) { return ((b ^ a0) | (b ^ a1)) == 0; }
static int all_eq3(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2) { return ((b ^ a0) | (b ^ a1) | (b ^ a2)) == 0; }
static int all_eq4(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) { return ((b ^ a0) | (b ^ a1) | (b ^ a2) | (b ^ a3)) == 0; }
static int any_eq3(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2) { return b == a0 || b == a1 || b == a2; }
static int none_eq2(uint32_t b, uint32_t a0, uint32_t a1) { return b != a0 && b != a1; }
static int none_eq4(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) { return b != a0 && b != a1 && b != a2 && b != a3; }

static void mmpx(const image *im, uint32_t *out, int op)
{
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            uint32_t A = src(im, x - 1, y - 1), B = src(im, x, y - 1), C = src(im, x + 1, y - 1);
            uint32_t D = src(im, x - 1, y), E = src(im, x, y), F = src(im, x + 1, y);
            uint32_t G = src(im, x - 1, y + 1), H = src(im, x, y + 1), I = src(im, x + 1, y + 1);
            uint32_t J = E, K = E, L = E, M = E;          /* the outputs, top left to bottom right */
            if (((A ^ E) | (B ^ E) | (C ^ E) | (D ^ E) | (F ^ E) | (G ^ E) | (H ^ E) | (I ^ E)) != 0) {
                uint32_t P = src(im, x, y - 2), Q = src(im, x - 2, y), R = src(im, x + 2, y), S = src(im, x, y + 2);
                uint32_t Bl = luma(B), Dl = luma(D), El = luma(E), Fl = luma(F), Hl = luma(H);

                /* 1:1 slopes, EPX's corners with more conditions */
                if ((D == B && D != H && D != F) && (El >= Dl || E == A) && any_eq3(E, A, C, G) && (El < Dl || A != D || E != P || E != Q)) J = D;
                if ((B == F && B != D && B != H) && (El >= Bl || E == C) && any_eq3(E, A, C, I) && (El < Bl || C != B || E != P || E != R)) K = B;
                if ((H == D && H != F && H != B) && (El >= Hl || E == G) && any_eq3(E, A, G, I) && (El < Hl || G != H || E != S || E != Q)) L = H;
                if ((F == H && F != B && F != D) && (El >= Fl || E == I) && any_eq3(E, C, G, I) && (El < Fl || I != H || E != R || E != S)) M = F;

                /* intersections, then triangle tips */
                if ((E != F && all_eq4(E, C, I, D, Q) && all_eq2(F, B, H)) && F != src(im, x + 3, y)) K = M = F;
                if ((E != D && all_eq4(E, A, G, F, R) && all_eq2(D, B, H)) && D != src(im, x - 3, y)) J = L = D;
                if ((E != H && all_eq4(E, G, I, B, P) && all_eq2(H, D, F)) && H != src(im, x, y + 3)) L = M = H;
                if ((E != B && all_eq4(E, A, C, H, S) && all_eq2(B, D, F)) && B != src(im, x, y - 3)) J = K = B;
                if (Bl < El && all_eq4(E, G, H, I, S) && none_eq4(E, A, D, C, F)) J = K = B;
                if (Hl < El && all_eq4(E, A, B, C, P) && none_eq4(E, D, G, I, F)) L = M = H;
                if (Fl < El && all_eq4(E, A, D, G, Q) && none_eq4(E, B, C, I, H)) K = M = F;
                if (Dl < El && all_eq4(E, C, F, I, R) && none_eq4(E, B, A, G, H)) J = L = D;

                /* 2:1 slopes */
                if (H != B) {
                    if (H != A && H != E && H != C) {
                        if (all_eq3(H, G, F, R) && none_eq2(H, D, src(im, x + 2, y - 1))) L = M;
                        if (all_eq3(H, I, D, Q) && none_eq2(H, F, src(im, x - 2, y - 1))) M = L;
                    }
                    if (B != I && B != G && B != E) {
                        if (all_eq3(B, A, F, R) && none_eq2(B, D, src(im, x + 2, y + 1))) J = K;
                        if (all_eq3(B, C, D, Q) && none_eq2(B, F, src(im, x - 2, y + 1))) K = J;
                    }
                }
                if (F != D) {
                    if (D != I && D != E && D != C) {
                        if (all_eq3(D, A, H, S) && none_eq2(D, B, src(im, x + 1, y + 2))) J = L;
                        if (all_eq3(D, G, B, P) && none_eq2(D, H, src(im, x + 1, y - 2))) L = J;
                    }
                    if (F != E && F != A && F != G) {
                        if (all_eq3(F, C, H, S) && none_eq2(F, B, src(im, x - 1, y + 2))) K = M;
                        if (all_eq3(F, I, B, P) && none_eq2(F, H, src(im, x - 1, y - 2))) M = K;
                    }
                }
            }
            uint32_t *o = out + 2 * y * op + 2 * x;
            o[0] = J; o[1] = K; o[op] = L; o[op + 1] = M;
        }
}

/* ---- xBR (lv2): the shader's sums of four corners at once, a corner a lane: lane 0 the lower
 * right (H below E, F right), 1 the upper right, 2 the upper left, 3 the lower left. Its defaults:
 * corner rule C, smooth tips, the luma weights (14.352, 28.176, 5.472), a threshold of 15, the
 * level 2 coefficient 2; its blend a pixel of the output wide (XBR_SCALE = the factor). Its
 * decisions are made here in whole numbers (the lumas times 255000), so a tie is a tie: a
 * console's colour levels are evenly spaced, so its comparisons often come out exactly equal,
 * where a GPU's last bit decides one way or the other. The blends are in double precision. */

static int32_t yw(uint32_t c) { return (int32_t)(c >> 16 & 0xFF) * 14352 + (int32_t)(c >> 8 & 0xFF) * 28176 + (int32_t)(c & 0xFF) * 5472; }
static int32_t df(int32_t a, int32_t b) { return a > b ? a - b : b - a; }
static int eqt(int32_t a, int32_t b) { return df(a, b) <= 15 * 255000; }   /* eq(): within the threshold */
static int neqt(int32_t a, int32_t b) { return df(a, b) > 15 * 255000; }   /* neq() */
static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* lumas: the picture's, each pixel's once, with two pixels of border repeated around it (in the
 * caller's scratch, else its own) */
static void xbr(const image *im, uint32_t *out, int op, int n, uint32_t *scratch)
{
    static const double Ao[4] = { 1, -1, -1, 1 }, Bo[4] = { 1, 1, -1, -1 }, Co[4] = { 1.5, 0.5, -0.5, 0.5 };
    static const double Bx[4] = { 0.5, 2, -0.5, -2 }, Cx[4] = { 1, 1, -0.5, 0 };
    static const double By[4] = { 2, 0.5, -2, -0.5 }, Cy[4] = { 2, 0, -1, 0.5 };
    double dl = 1.0 / n, dh = 0.5 / n;
    double delta[4] = { dl, dl, dl, dl }, delta_l[4] = { dh, dl, dh, dl }, delta_u[4] = { dl, dh, dl, dh };
    /* each point's blend for each corner, by the rules that hold there (a mask: 1 45 inner, 2 45,
     * 4 30, 8 60), the furthest of theirs */
    double ramp[4][4][4][16];                               /* [sy][sx][corner][rules] */
    for (int sy = 0; sy < n; sy++)
        for (int sx = 0; sx < n; sx++)
            for (int k = 0; k < 4; k++) {
                double fpx = (sx + 0.5) / n, fpy = (sy + 0.5) / n, v[4];
                double fx = Ao[k] * fpy + Bo[k] * fpx, fx_l = Ao[k] * fpy + Bx[k] * fpx, fx_u = Ao[k] * fpy + By[k] * fpx;
                v[0] = clamp01((fx + delta[k] - Co[k] - 0.25) / (2 * delta[k]));
                v[1] = clamp01((fx + delta[k] - Co[k]) / (2 * delta[k]));
                v[2] = clamp01((fx_l + delta_l[k] - Cx[k]) / (2 * delta_l[k]));
                v[3] = clamp01((fx_u + delta_u[k] - Cy[k]) / (2 * delta_u[k]));
                for (int mask = 0; mask < 16; mask++) {
                    double best = 0;
                    for (int q = 0; q < 4; q++) if ((mask >> q & 1) && v[q] > best) best = v[q];
                    ramp[sy][sx][k][mask] = best;
                }
            }
    int lw = im->w + 4, lh = im->h + 4;
    int32_t *xbr_luma = scratch && lw * lh <= 4 * im->w * im->h ? (int32_t *)(void *)scratch : malloc((size_t)lw * lh * sizeof *xbr_luma);
    if (!xbr_luma) return;
    for (int y = -2; y < im->h + 2; y++)
        for (int x = -2; x < im->w + 2; x++) xbr_luma[(y + 2) * lw + x + 2] = yw(src(im, x, y));
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) {
            const int32_t *L = xbr_luma + (y + 2) * lw + x + 2;
            #define Y(dx, dy) L[(dy) * lw + (dx)]
            int32_t e = Y(0, 0);
            int32_t b[4] = { Y(0, -1), Y(-1, 0), Y(0, 1), Y(1, 0) };
            if (e == b[0] && e == b[1] && e == b[2] && e == b[3]) {   /* (flat here: no edge, the pixel as it is) */
                uint32_t E = src(im, x, y) | 0xFF000000u;
                for (int sy = 0; sy < n; sy++)
                    for (int sx = 0; sx < n; sx++) out[(y * n + sy) * op + x * n + sx] = E;
                continue;
            }
            int32_t c[4] = { Y(1, -1), Y(-1, -1), Y(-1, 1), Y(1, 1) };
            int32_t i4[4] = { Y(2, 1), Y(1, -2), Y(-2, -1), Y(-1, 2) };
            int32_t i5[4] = { Y(1, 2), Y(2, -1), Y(-1, -2), Y(-2, 1) };
            int32_t h5[4] = { Y(0, 2), Y(2, 0), Y(0, -2), Y(-2, 0) };
            #undef Y
            uint32_t E = src(im, x, y), B = src(im, x, y - 1), D = src(im, x - 1, y), F = src(im, x + 1, y), H = src(im, x, y + 1);
            uint32_t col_h[4] = { H, F, B, D }, col_f[4] = { F, B, D, H };
            int rules[4], any = 0;                          /* each corner's rules that hold (ramp's mask) */
            uint32_t to[4];                                 /* each corner's colour to blend towards */
            for (int k = 0; k < 4; k++) {                   /* (& and |, not && and ||: no branches to guess) */
                int32_t d = b[(k + 1) & 3], f = b[(k + 3) & 3], g = c[(k + 2) & 3], h = b[(k + 2) & 3], i = c[(k + 3) & 3], f4 = h5[(k + 1) & 3];
                int irlv0 = (e != f) & (e != h);
                int irlv1 = irlv0 & (((neqt(f, b[k]) & neqt(f, c[k])) | (neqt(h, d) & neqt(h, g))
                                      | (eqt(e, i) & ((neqt(f, f4) & neqt(f, i4[k])) | (neqt(h, h5[k]) & neqt(h, i5[k]))))
                                      | eqt(e, g) | eqt(e, c[k])));
                int irlv2l = (e != g) & (d != g), irlv2u = (e != c[k]) & (b[k] != c[k]);
                int32_t wd1 = df(e, c[k]) + df(e, g) + df(i, h5[k]) + df(i, f4) + 4 * df(h, f);
                int32_t wd2 = df(h, d) + df(h, i5[k]) + df(f, i4[k]) + df(f, b[k]) + 4 * df(e, i);
                int r45i = (wd2 >= wd1) & irlv0, r45 = (wd2 >= wd1 + 25500) & irlv1;   /* (0.1) */
                int r30 = (df(h, c[k]) >= 2 * df(f, g)) & irlv2l & r45, r60 = (df(f, g) >= 2 * df(h, c[k])) & irlv2u & r45;
                rules[k] = r45i | r45 << 1 | r30 << 2 | r60 << 3;
                to[k] = df(e, h) >= df(e, f) ? col_f[k] : col_h[k];   /* (px) */
                any |= rules[k];
            }
            if (!any) {
                for (int sy = 0; sy < n; sy++)
                    for (int sx = 0; sx < n; sx++) out[(y * n + sy) * op + x * n + sx] = E | 0xFF000000u;
                continue;
            }
            for (int sy = 0; sy < n; sy++)
                for (int sx = 0; sx < n; sx++) {
                    double m[4] = { ramp[sy][sx][0][rules[0]], ramp[sy][sx][1][rules[1]], ramp[sy][sx][2][rules[2]], ramp[sy][sx][3][rules[3]] };
                    if (m[0] == 0 && m[1] == 0 && m[2] == 0 && m[3] == 0) { out[(y * n + sy) * op + x * n + sx] = E | 0xFF000000u; continue; }
                    double r1[3], r2[3], d1 = 0, d2 = 0;
                    for (int ch = 0; ch < 3; ch++) {        /* the lower right then the upper left; the other two */
                        int sh = 16 - 8 * ch;
                        double ec = (double)(E >> sh & 0xFF);
                        r1[ch] = ec * (1 - m[0]) + (double)(to[0] >> sh & 0xFF) * m[0];
                        r1[ch] = r1[ch] * (1 - m[2]) + (double)(to[2] >> sh & 0xFF) * m[2];
                        r2[ch] = ec * (1 - m[1]) + (double)(to[1] >> sh & 0xFF) * m[1];
                        r2[ch] = r2[ch] * (1 - m[3]) + (double)(to[3] >> sh & 0xFF) * m[3];
                        d1 += fabs(ec - r1[ch]);
                        d2 += fabs(ec - r2[ch]);
                    }
                    const double *r = d2 >= d1 - 1e-9 ? r2 : r1;   /* (the one further from E; a tie, the second) */
                    uint32_t o = 0xFF000000u;
                    for (int ch = 0; ch < 3; ch++) o |= (uint32_t)(r[ch] + 0.5 + 1e-7) << (16 - 8 * ch);   /* (a half up, as exact sums round) */
                    out[(y * n + sy) * op + x * n + sx] = o;
                }
        }
    if ((void *)xbr_luma != (void *)scratch) free(xbr_luma);
}

void mdrt_upscale(int method, const uint32_t *in, int w, int h, int pitch, uint32_t *out, int out_pitch, uint32_t *scratch)
{
    if (w <= 0 || h <= 0) return;
    image im = { in, w, h, pitch };
    switch (method) {
    case MDRT_UPSCALE_SCALE2X: scale2x(&im, out, out_pitch); break;
    case MDRT_UPSCALE_SCALE3X: scale3x(&im, out, out_pitch); break;
    case MDRT_UPSCALE_MMPX2X: mmpx(&im, out, out_pitch); break;
    case MDRT_UPSCALE_MMPX4X: {
        if (!scratch) return;
        mmpx(&im, scratch, 2 * w);
        image twice = { scratch, 2 * w, 2 * h, 2 * w };
        mmpx(&twice, out, out_pitch);
        break;
    }
    case MDRT_UPSCALE_XBR2X: case MDRT_UPSCALE_XBR3X: case MDRT_UPSCALE_XBR4X:
        xbr(&im, out, out_pitch, methods[method].factor, scratch);
        break;
    default:                                              /* off: each pixel as it is */
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) out[y * out_pitch + x] = in[y * pitch + x];
        break;
    }
}
