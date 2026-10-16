/* PNG files for test output (tests/png.h), made by the library's encoder (mdrt/png.h). */
#include <stdio.h>
#include <stdlib.h>
#include "mdrt/png.h"
#include "png.h"

int png_write(const char *path, const uint32_t *rgb, int width, int height)
{
    size_t n = mdrt_png_encode(rgb, width, height, width, NULL, 0);
    uint8_t *b = n ? malloc(n) : NULL;
    FILE *f = b ? fopen(path, "wb") : NULL;
    int ok = f && mdrt_png_encode(rgb, width, height, width, b, n) == n && fwrite(b, 1, n, f) == n;
    if (f && fclose(f)) ok = 0;
    free(b);
    return ok ? 0 : -1;
}
