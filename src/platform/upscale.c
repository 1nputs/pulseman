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
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. */
#include "mdrt/upscale.h"

static const struct { int factor; const char *name, *label; } methods[MDRT_UPSCALE_COUNT] = {
    { 1, "off", "off" }, { 2, "scale2x", "Scale2x" }, { 3, "scale3x", "Scale3x" },
    { 2, "mmpx2x", "MMPX 2x" }, { 4, "mmpx4x", "MMPX 4x" },
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
    default:                                              /* off: each pixel as it is */
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) out[y * out_pitch + x] = in[y * pitch + x];
        break;
    }
}
