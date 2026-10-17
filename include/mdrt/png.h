/* PNG pictures in memory, with no zlib: a screenshot's (RGB, 8 bits a channel, the deflate stream
 * stored), an indexed picture's (8 bits a pixel, deflated), and any PNG read back (every colour
 * type and bit depth, interlaced or not), checked throughout so that a file from anywhere (an
 * image editor, another player over the network) is safe to read. Writing to files is the
 * caller's. */
#ifndef MDRT_PNG_H
#define MDRT_PNG_H
#include <stddef.h>
#include <stdint.h>

/* width x height pixels 0x??RRGGBB, pitch pixels a row; the PNG goes into out when size is enough.
 * Returns the PNG's size either way (0 for a bad size). */
size_t mdrt_png_encode(const uint32_t *rgb, int width, int height, int pitch, uint8_t *out, size_t size);
/* a PNG back into pixels 0xFFRRGGBB (alpha dropped), out of max_w x max_h at most (pitch max_w);
 * the picture's size in *w, *h; 0 on success, -1 for anything else */
int mdrt_png_decode(const uint8_t *png, size_t size, uint32_t *out, int max_w, int max_h, int *w, int *h);

/* a PNG read: its pixels as 0xAARRGGBB, and, when it is indexed (colour type 3), each pixel's
 * palette index and the palette (0xAARRGGBB, its tRNS alpha in) */
typedef struct mdrt_png {
    int width, height;
    uint32_t *rgba;             /* width x height (malloc'd) */
    uint8_t *index;             /* width x height, or NULL when it is not indexed (malloc'd) */
    uint32_t palette[256];
    int colours;                /* the palette's entries (0: not indexed) */
} mdrt_png;
#define MDRT_PNG_MAX_SIDE 8192
#define MDRT_PNG_MAX_PIXELS (16u << 20)
/* any PNG of at most MDRT_PNG_MAX_SIDE a side and MDRT_PNG_MAX_PIXELS pixels: 0, else -1 with
 * why (a sentence) */
int mdrt_png_read(const uint8_t *png, size_t size, mdrt_png *out, char *why, size_t why_size);
void mdrt_png_free(mdrt_png *p);
/* an indexed PNG of width x height palette indexes (pitch a row), colours entries of palette
 * (0x00RRGGBB; transparent0: entry 0 transparent), deflated: into *out (malloc'd, the caller's
 * to free); its size, 0 on failure. The same pixels make the same bytes. */
size_t mdrt_png_write_indexed(const uint8_t *index, int width, int height, int pitch, const uint32_t *palette, int colours,
                              int transparent0, uint8_t **out);

/* deflate (RFC 1951) both ways, for the PNGs and for archives: a raw stream of in (n bytes) into
 * out, at most max bytes; its size, or -1 when it is not a stream that fits (*used: the input
 * bytes it took, when not NULL) */
long mdrt_inflate(const uint8_t *in, size_t n, uint8_t *out, size_t max, size_t *used);
/* in, deflated (LZ77, fixed Huffman codes) into *out (malloc'd): its size, 0 on failure. The same
 * input makes the same bytes. */
size_t mdrt_deflate(const uint8_t *in, size_t n, uint8_t **out);
uint32_t mdrt_crc32(uint32_t crc, const void *p, size_t n);   /* (crc 0 to start) */

#endif
