/* WP5: replay every .padtrace in $MDRT_VECTORS: the buttons each frame, the game's writes to the
 * I/O ports, and every read must return what the oracle returned. Format:
 * ../nhl95/tools/hybrid/oracle_shim.c ("MDPT"). Also the 6-button sequence and save RAM by hand. */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "mdrt/pad.h"
#include "mdrt/sram.h"

static void replay(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { check_failures++; return; }
    static uint8_t buf[1 << 24];
    size_t len = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (len < 24 || memcmp(buf, "MDPT", 4) != 0) { printf("  %s: not a padtrace\n", path); check_failures++; return; }
    mdrt_pads p;
    mdrt_pads_init(&p, 1, 0);
    memcpy(p.reg, buf + 8, 16);
    size_t o = 24;
    int frames = 0, reads = 0, bad = 0;
    char first[160] = "";
    while (o < len) {
        uint8_t t = buf[o++];
        if (t == 1) {
            mdrt_pads_frame(&p);
            mdrt_pad_set(&p, 0, (uint16_t)(buf[o] | buf[o + 1] << 8));
            mdrt_pad_set(&p, 1, (uint16_t)(buf[o + 2] | buf[o + 3] << 8));
            o += 4;
            frames++;
        } else if (t == 2 || t == 3) {
            uint32_t a = (uint32_t)(buf[o] | buf[o + 1] << 8 | buf[o + 2] << 16 | (uint32_t)buf[o + 3] << 24);
            int w = buf[o + 4];
            uint32_t v = (uint32_t)(buf[o + 5] | buf[o + 6] << 8 | buf[o + 7] << 16 | (uint32_t)buf[o + 8] << 24);
            o += 9;
            if (w != 1) continue;                         /* the game makes byte accesses only */
            if (t == 2) mdrt_pad_write(&p, a, (uint8_t)v);
            else {
                uint8_t got = mdrt_pad_read(&p, a);
                reads++;
                if (got != (uint8_t)v && !bad++) snprintf(first, sizeof first, "frame %d: read %06x %02x, expected %02x", frames, a, got, (uint8_t)v);
            }
        } else break;
    }
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    printf("  %-20s %4d frames: %d / %d reads%s%s\n", name, frames, reads - bad, reads, bad ? "; first: " : "", first);
    check_count++;
    if (bad) check_failures++;
}

static void six_button(void)
{
    mdrt_pads p;
    mdrt_pads_init(&p, 1, 0);
    mdrt_pad_set_type(&p, 0, MDRT_PAD_6BUTTON);
    mdrt_pad_write(&p, 0xA10003, 0x40);                   /* TH high, then an output */
    mdrt_pad_write(&p, 0xA10009, 0x40);
    mdrt_pads_frame(&p);
    mdrt_pad_set(&p, 0, MDRT_PAD_X | MDRT_PAD_START | MDRT_PAD_UP);
    uint8_t seq[8];
    for (int i = 0; i < 8; i++) {
        mdrt_pad_write(&p, 0xA10003, (i & 1) ? 0x00 : 0x40);
        seq[i] = mdrt_pad_read(&p, 0xA10003) & 0x7F;
    }
    CHECK_EQ(seq[0], 0x7E);                               /* TH 1: ?1CBRLDU, up held */
    CHECK_EQ(seq[1], 0x12);                               /* TH 0: ?0SA00DU, start and up held */
    CHECK_EQ(seq[5], 0x10);                               /* third TH 0: D3-D0 forced 0 */
    CHECK_EQ(seq[6], 0x7B);                               /* fourth TH 1: ?1CBMXYZ, X held */
    CHECK_EQ(seq[7], 0x1F);                               /* fourth TH 0: D3-D0 forced 1 */
    mdrt_pad_write(&p, 0xA10003, 0x40);
    CHECK_EQ(mdrt_pad_read(&p, 0xA10003) & 0x7F, 0x7E);   /* then 3-button values again */
    /* bit 7 reads back what was written; TH as an input reads high */
    mdrt_pad_write(&p, 0xA10009, 0x00);
    mdrt_pad_write(&p, 0xA10003, 0x80);
    CHECK_EQ(mdrt_pad_read(&p, 0xA10003) & 0xC0, 0xC0);
}

static void sram(void)
{
    static mdrt_sram s;
    mdrt_sram_init(&s, 0x2000);
    CHECK_EQ(mdrt_sram_read8(&s, 1), 0xFF);
    mdrt_sram_write8(&s, 3, 0x42);
    CHECK_EQ(s.dirty, 1);
    CHECK_EQ(mdrt_sram_read8(&s, 3), 0x42);
    CHECK_EQ(s.bytes[1], 0x42);
    mdrt_sram_write8(&s, 4, 0x11);                        /* even: not connected */
    CHECK_EQ(mdrt_sram_read8(&s, 4), 0xFF);
    mdrt_sram_write8(&s, 0x4001, 0x11);                   /* past the size */
    CHECK_EQ(mdrt_sram_read8(&s, 0x4001), 0xFF);
}

int main(void)
{
    six_button();
    sram();
    const char *dir = check_vectors();
    if (!dir) { printf("pad_trace: no vectors (MDRT_VECTORS), skipped the replays\n"); return check_done("pad_trace"); }
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[1024];
    while (d && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 9 || strcmp(e->d_name + n - 9, ".padtrace")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        replay(path);
    }
    if (d) closedir(d);
    return check_done("pad_trace");
}
