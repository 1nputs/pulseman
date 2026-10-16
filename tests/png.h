/* PNG files for test output (the library's encoder, mdrt/png.h). */
#ifndef MDRT_TESTS_PNG_H
#define MDRT_TESTS_PNG_H
#include <stdint.h>
/* rgb: width x height pixels 0x00RRGGBB; returns 0 on success */
int png_write(const char *path, const uint32_t *rgb, int width, int height);
#endif
