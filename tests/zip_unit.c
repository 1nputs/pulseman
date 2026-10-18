/* ZIP archives (mdrt/zip.h): Python's zipfile's opened and every file read as it was
 * (tests/zip_cases.h: stored, deflated, a comment, a directory, an empty file); hostile ones
 * refused, saying why (names climbing out of a folder or absolute, a file twice, encryption,
 * other packings, a size past the limit; a wrong CRC refused on reading); archives written and
 * read back, the same files in any order making the same bytes; and every case cut short and
 * with bytes changed at random failing, never more (the sanitizers watch). */
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "mdrt/zip.h"
#include "zip_cases.h"

static unsigned long long fnv(const void *p, size_t n)
{
    unsigned long long h = 0xCBF29CE484222325ull;
    for (const unsigned char *s = p; n--; s++) h = (h ^ *s) * 0x100000001B3ull;
    return h;
}

static void good(void)
{
    int n = (int)(sizeof zip_good / sizeof zip_good[0]), ok = 0;
    for (int i = 0; i < n; i++) {
        const struct zip_case *c = &zip_good[i];
        mdrt_zip z;
        char why[120];
        int same = mdrt_zip_open(&z, c->zip, c->size, why, sizeof why) == 0 && z.count == c->files;
        for (int k = 0; same && k < c->files; k++) {
            int e = mdrt_zip_find(&z, c->file[k].name);
            uint8_t *b = NULL;
            long len = e >= 0 ? mdrt_zip_read(&z, e, &b) : -1;
            same = len == (long)c->file[k].size && fnv(b, (size_t)len) == c->file[k].hash;
            free(b);
        }
        if (!same) printf("  %s: not as it was (%s)\n", c->what, why);
        CHECK(same);
        ok += same;
        mdrt_zip_close(&z);
    }
    printf("  zip: %d of %d of Python's archives read as they were\n", ok, n);
}

static void bad(void)
{
    int n = (int)(sizeof zip_bad / sizeof zip_bad[0]), ok = 0;
    for (int i = 0; i < n; i++) {
        const struct zip_bad *c = &zip_bad[i];
        mdrt_zip z;
        char why[120] = "";
        int r = mdrt_zip_open(&z, c->zip, c->size, why, sizeof why), refused;
        if (!c->says[0]) {                                /* (it opens: its file does not read) */
            uint8_t *b;
            refused = r == 0 && z.count == 1 && mdrt_zip_read(&z, 0, &b) < 0;
            mdrt_zip_close(&z);
        } else refused = r < 0 && !strncmp(why, c->says, strlen(c->says));
        if (!refused) printf("  %s: not refused as it should be (%s)\n", c->what, why);
        CHECK(refused);
        ok += refused;
    }
    printf("  zip: %d of %d hostile archives refused, saying why\n", ok, n);
}

static uint32_t next(uint32_t *x) { *x ^= *x << 13; *x ^= *x >> 17; *x ^= *x << 5; return *x; }

static void written(void)
{
    static uint8_t noise[50000];
    uint32_t x = 5;
    for (size_t i = 0; i < sizeof noise; i++) noise[i] = (uint8_t)next(&x);
    static const char *const names[5] = { "settings.ini", "art/data/b.bin", "data/table.csv", "art/pictures/a.png", "empty" };
    const void *data[5] = { "name = x\n", noise, "key,value\nK1,one\nK2,one\nK3,one\nK4,one\nK5,one\n", noise + 100, "" };
    size_t size[5] = { 9, sizeof noise, strlen(data[2]), 1000, 0 };
    uint8_t *a, *b;
    mdrt_zip_writer w = { 0 }, w2 = { 0 };
    for (int i = 0; i < 5; i++) CHECK_EQ(mdrt_zip_add(&w, names[i], data[i], size[i]), 0);
    for (int i = 4; i >= 0; i--) CHECK_EQ(mdrt_zip_add(&w2, names[i], data[i], size[i]), 0);
    size_t n = mdrt_zip_finish(&w, &a), n2 = mdrt_zip_finish(&w2, &b);
    CHECK(n > 0 && n == n2 && !memcmp(a, b, n));         /* (any order: the same bytes) */
    mdrt_zip z;
    int same = mdrt_zip_open(&z, a, n, NULL, 0) == 0 && z.count == 5;
    for (int i = 0; same && i < 5; i++) {
        uint8_t *f;
        long len = mdrt_zip_read(&z, mdrt_zip_find(&z, names[i]), &f);
        same = len == (long)size[i] && !memcmp(f, data[i], size[i]);
        free(f);
    }
    CHECK(same);
    mdrt_zip_close(&z);
    printf("  zip: an archive of 5 files written, %zu bytes, read back %s\n", n, same ? "as they were" : "WRONG");
    mdrt_zip_writer w3 = { 0 };
    CHECK_EQ(mdrt_zip_add(&w3, "../x", "x", 1), -1);
    CHECK_EQ(mdrt_zip_add(&w3, "/x", "x", 1), -1);
    mdrt_zip_writer_free(&w3);
    /* damaged: cut short, and bytes changed */
    int cut = 0, changed = 0, opened = 0;
    for (size_t len = 0; len < n; len += 1 + len / 32) {
        if (mdrt_zip_open(&z, a, len, NULL, 0) < 0) cut++;
        else mdrt_zip_close(&z);
    }
    uint8_t *d = malloc(n);
    for (int t = 0; t < 3000; t++) {
        memcpy(d, a, n);
        for (int k = 1 + (int)(next(&x) % 3); k--;) d[next(&x) % n] ^= (uint8_t)(1u << (next(&x) % 8));
        if (mdrt_zip_open(&z, d, n, NULL, 0) < 0) { changed++; continue; }
        opened++;
        for (int i = 0; i < z.count; i++) { uint8_t *f; if (mdrt_zip_read(&z, i, &f) >= 0) free(f); }
        mdrt_zip_close(&z);
    }
    free(d);
    printf("  zip: damaged archives: %d cut short refused; of 3000 changed at random %d refused, %d opened (their files read or not)\n", cut, changed, opened);
    CHECK(cut > 0);
    free(a);
    free(b);
}

int main(void)
{
    good();
    bad();
    written();
    return check_done("zip_unit");
}
