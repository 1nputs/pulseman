/* Replay every .padtrace in $MDRT_VECTORS: the buttons each frame, the game's writes to the
 * I/O ports, and every read must return what the oracle returned (version 1: two pads; 2: four,
 * and what is in the ports, EA's 4 Way Play among them). Format: docs/VECTORS.md ("MDPT"). Also
 * the 6-button sequence, the 4 Way Play's protocol, old states and save RAM by hand. */
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
    uint32_t version = (uint32_t)(buf[4] | buf[5] << 8 | buf[6] << 16 | (uint32_t)buf[7] << 24);
    if (version != 1 && version != 2) { printf("  %s: padtrace version %u (this reader knows 1 and 2)\n", path, version); check_failures++; return; }
    int pads = version == 1 ? 2 : MDRT_PADS;
    size_t o = version == 1 ? 8 : 12;
    mdrt_pads p;
    mdrt_pads_init(&p, 1, 0);
    if (version == 2) mdrt_pads_set_adaptor(&p, buf[8]);
    memcpy(p.reg, buf + o, 16);
    o += 16;
    int frames = 0, reads = 0, bad = 0;
    char first[160] = "";
    while (o < len) {
        uint8_t t = buf[o++];
        if (t == 1) {
            mdrt_pads_frame(&p);
            for (int i = 0; i < pads; i++) mdrt_pad_set(&p, i, (uint16_t)(buf[o + 2 * i] | buf[o + 2 * i + 1] << 8));
            o += 2u * (unsigned)pads;
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
    printf("  %-20s %4d frames: %d / %d reads%s%s%s\n", name, frames, reads - bad, reads, p.adaptor ? " (4 Way Play)" : "",
           bad ? "; first: " : "", first);
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

/* EA's 4 Way Play as a game drives it: found with $7C on port 2, then each pad chosen in turn */
static void four_way(void)
{
    mdrt_pads p;
    mdrt_pads_init(&p, 1, 0);
    mdrt_pad_write(&p, 0xA10009, 0x40);                   /* port 1: TH an output */
    mdrt_pad_write(&p, 0xA1000B, 0x7F);
    mdrt_pad_write(&p, 0xA10005, 0x7C);
    CHECK_EQ(mdrt_pad_read(&p, 0xA10003) & 3, 3);         /* no adaptor: a pad's up and down, not held */
    mdrt_pads_set_adaptor(&p, MDRT_ADAPTOR_EA4WAY);
    CHECK_EQ(mdrt_pad_read(&p, 0xA10003) & 3, 0);         /* the adaptor's mark */
    static const uint16_t held[4] = { MDRT_PAD_UP, MDRT_PAD_B | MDRT_PAD_DOWN, MDRT_PAD_START | MDRT_PAD_A, MDRT_PAD_C | MDRT_PAD_RIGHT };
    for (int i = 0; i < 4; i++) mdrt_pad_set(&p, i, held[i]);
    for (int i = 0; i < 4; i++) {
        mdrt_pad_write(&p, 0xA10005, (uint8_t)(0x0C | i << 4));
        mdrt_pad_write(&p, 0xA10003, 0x00);
        uint8_t lo = mdrt_pad_read(&p, 0xA10003) & 0x3F;
        mdrt_pad_write(&p, 0xA10003, 0x40);
        uint8_t hi = mdrt_pad_read(&p, 0xA10003) & 0x3F;
        CHECK_EQ(hi, 0x3F & ~(held[i] & 0x3F));           /* ?1CBRLDU */
        CHECK_EQ(lo, 0x33 & ~(((held[i] >> 2) & 0x30) | (held[i] & 3)));   /* ?0SA00DU */
    }
    CHECK_EQ(mdrt_pad_read(&p, 0xA10005) & 0x3F, 0x3C);   /* port 2: its own outputs only */
    /* a state from before the adaptor (two pads) loads, without it */
    uint8_t old[24] = { 0 }, now[64];
    old[16] = 0x41;                                       /* pad 1: up and A */
    CHECK_EQ(mdrt_pads_load(&p, old, sizeof old), 0);
    CHECK(p.buttons[0] == 0x41 && p.buttons[2] == 0 && p.adaptor == MDRT_ADAPTOR_NONE);
    mdrt_pads_set_adaptor(&p, MDRT_ADAPTOR_EA4WAY);
    mdrt_pad_set(&p, 3, MDRT_PAD_C);
    size_t n = mdrt_pads_save(&p, now);
    mdrt_pads q;
    mdrt_pads_init(&q, 1, 0);
    CHECK_EQ(mdrt_pads_load(&q, now, n), 0);
    CHECK(q.adaptor == MDRT_ADAPTOR_EA4WAY && q.buttons[3] == MDRT_PAD_C);
    CHECK_EQ(mdrt_pads_load(&q, now, n - 1), -1);
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
    four_way();
    sram();
    const char *dir = check_vectors();
    if (!dir) { check_skip("pad_trace", "no vectors (MDRT_VECTORS) for the replays"); return check_done("pad_trace"); }
    int traces = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[1024];
    while (d && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 9 || strcmp(e->d_name + n - 9, ".padtrace")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        replay(path);
        traces++;
    }
    if (d) closedir(d);
    if (!traces) check_skip("pad_trace", "no .padtrace in MDRT_VECTORS");
    return check_done("pad_trace");
}
