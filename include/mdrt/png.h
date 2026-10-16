/* A PNG of a picture, made in memory (RGB, 8 bits a channel; the deflate stream stored, so no zlib
 * is needed): screenshots and test output. Writing it to a file is the caller's. */
#ifndef MDRT_PNG_H
#define MDRT_PNG_H
#include <stddef.h>
#include <stdint.h>

/* width x height pixels 0x??RRGGBB, pitch pixels a row; the PNG goes into out when size is enough.
 * Returns the PNG's size either way (0 for a bad size). */
size_t mdrt_png_encode(const uint32_t *rgb, int width, int height, int pitch, uint8_t *out, size_t size);
/* a PNG mdrt_png_encode made (8-bit RGB, stored deflate, no filters) back into pixels 0xFFRRGGBB,
 * out of max_w x max_h at most (pitch max_w); the picture's size in *w, *h; 0 on success, -1 for
 * anything else */
int mdrt_png_decode(const uint8_t *png, size_t size, uint32_t *out, int max_w, int max_h, int *w, int *h);

#endif
