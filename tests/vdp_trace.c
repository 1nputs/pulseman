/* Replay every .vdptrace in $MDRT_VECTORS. The port accesses are applied in order,
 * DMA reads come from the recorded source bytes, data port reads must return the oracle's value,
 * and at every FRAME the registers, VRAM, CRAM and VSRAM must match byte for byte and the
 * rendered picture pixel for pixel (9-bit colours). Version 2 traces carry each access's time in
 * the frame: the picture is drawn a line at a time as the replay passes each line's start
 * (mdrt_vdp_line_start), as the composition draws it, so writes during the display land on the
 * lines after them. Format: docs/VECTORS.md. */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "mdrt/vdp.h"
#include "png.h"

enum { T_CTRL16 = 1, T_CTRL32, T_DATA16, T_DATA32, T_READ16, T_STATUS, T_HV8, T_HV16, T_DMASRC, T_FRAME, T_END };

struct reader { const uint8_t *p, *end; };
static int more(const struct reader *r, size_t n) { return (size_t)(r->end - r->p) >= n; }
static uint8_t u8(struct reader *r) { return *r->p++; }
static uint16_t u16(struct reader *r) { uint16_t v = (uint16_t)(r->p[0] | r->p[1] << 8); r->p += 2; return v; }
static uint32_t u32(struct reader *r) { uint32_t v = (uint32_t)(r->p[0] | r->p[1] << 8 | r->p[2] << 16 | (uint32_t)r->p[3] << 24); r->p += 4; return v; }

static void read_state(struct reader *r, mdrt_vdp_state *s)
{
    memcpy(s->reg, r->p, 24); r->p += 24;
    memcpy(s->vram, r->p, 0x10000); r->p += 0x10000;
    for (int i = 0; i < 64; i++, r->p += 2) s->cram[i] = (uint16_t)(r->p[0] << 8 | r->p[1]);
    for (int i = 0; i < 40; i++, r->p += 2) s->vsram[i] = (uint16_t)(r->p[0] << 8 | r->p[1]);
}

/* the DMA source bytes the trace recorded, served to the VDP in order */
struct dmaq { const uint8_t *p; uint32_t left; int over; };
static uint8_t dma_read(void *user, uint32_t addr)
{
    (void)addr;
    struct dmaq *q = user;
    if (!q->left) { q->over = 1; return 0; }
    q->left--;
    return *q->p++;
}

static int state_diff(const mdrt_vdp_state *a, const mdrt_vdp_state *b, char *what, size_t n)
{
    for (int i = 0; i < 24; i++) if (a->reg[i] != b->reg[i]) { snprintf(what, n, "R%d %02x, expected %02x", i, a->reg[i], b->reg[i]); return 1; }
    for (int i = 0; i < 0x10000; i++) if (a->vram[i] != b->vram[i]) { snprintf(what, n, "VRAM %04x %02x, expected %02x", i, a->vram[i], b->vram[i]); return 1; }
    for (int i = 0; i < 64; i++) if (a->cram[i] != b->cram[i]) { snprintf(what, n, "CRAM %d %03x, expected %03x", i, a->cram[i], b->cram[i]); return 1; }
    for (int i = 0; i < 40; i++) if (a->vsram[i] != b->vsram[i]) { snprintf(what, n, "VSRAM %d %03x, expected %03x", i, a->vsram[i], b->vsram[i]); return 1; }
    return 0;
}

static mdrt_vdp vdp;
static mdrt_vdp_state want;
static uint16_t picture[320 * 240];
static mdrt_vdp_beam beam;              /* the frame drawn a line at a time, as the composition draws it */
static uint32_t panels[(320 * 3 + 8) * 240];

/* framediff: the runtime's picture, the oracle's, and the differing pixels in red, side by side,
 * to build/framediff/NAME-FRAME.png */
static void framediff(const char *name, int frame, const uint8_t *expect, int w, int h)
{
    int pw = w * 3 + 8;
    for (int i = 0; i < pw * h; i++) panels[i] = 0x404040;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint16_t g = picture[y * 320 + x], e = (uint16_t)(expect[(y * w + x) * 2] | expect[(y * w + x) * 2 + 1] << 8);
            panels[y * pw + x] = mdrt_vdp_rgb(g, 0);
            panels[y * pw + w + 4 + x] = mdrt_vdp_rgb(e, 0);
            panels[y * pw + 2 * w + 8 + x] = g == e ? (mdrt_vdp_rgb(e, 0) >> 2 & 0x3F3F3F) : 0xFF0000;
        }
    char path[512];
    (void)!system("mkdir -p build/framediff");
    snprintf(path, sizeof path, "build/framediff/%s-%03d.png", name, frame);
    if (png_write(path, panels, pw, h) == 0) printf("  wrote %s\n", path);
}

static void replay(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  cannot open %s\n", path); check_failures++; return; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)len);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(buf); check_failures++; return; }
    fclose(f);
    struct reader r = { buf, buf + len };
    if (len < 12 || memcmp(buf, "MDVT", 4) != 0) { printf("  %s: not a vdptrace\n", path); check_failures++; free(buf); return; }
    r.p += 4;
    uint32_t version = u32(&r);
    if (version != 1 && version != 2) { printf("  %s: vdptrace version %u (this reads 1 and 2)\n", path, version); check_failures++; free(buf); return; }
    int pal = u8(&r);
    r.p += 3;
    mdrt_vdp_init(&vdp, pal);
    read_state(&r, &want);
    mdrt_vdp_set_state(&vdp, &want);
    struct dmaq q = { NULL, 0, 0 };
    mdrt_vdp_set_memory(&vdp, dma_read, &q);
    int frames = 0, state_bad = 0, pic_bad = 0, reads = 0, reads_bad = 0, ended = 0, mid = 0;
    char first[200] = "";
    mdrt_vdp_beam_start(&beam, &vdp, picture, 320);
    while (more(&r, 1) && !ended) {
        uint8_t t = u8(&r);
        if (version >= 2 && t >= T_CTRL16 && t <= T_HV16) {
            int32_t at = (int32_t)u32(&r);
            if (t <= T_DATA32) {
                if (at >= (int64_t)mdrt_vdp_line_start(&vdp, 0) && at < (int64_t)mdrt_vdp_line_start(&vdp, mdrt_vdp_height(&vdp)) && (vdp.s.reg[1] & 0x40)) mid++;
                mdrt_vdp_beam_to(&beam, at < 0 ? 0 : (uint32_t)at);
            }
        }
        switch (t) {
        case T_CTRL16: mdrt_vdp_ctrl16(&vdp, u16(&r)); break;
        case T_CTRL32: mdrt_vdp_ctrl32(&vdp, u32(&r)); break;
        case T_DATA16: mdrt_vdp_data16(&vdp, u16(&r)); break;
        case T_DATA32: mdrt_vdp_data32(&vdp, u32(&r)); break;
        case T_READ16: {
            uint16_t expect = u16(&r), got = mdrt_vdp_read16(&vdp);
            reads++;
            if (got != expect && reads_bad++ == 0 && !first[0]) snprintf(first, sizeof first, "read %d: %04x, expected %04x", reads, got, expect);
            break;
        }
        case T_STATUS: (void)u16(&r); (void)mdrt_vdp_status(&vdp); break;
        case T_HV8: r.p += 2; break;
        case T_HV16: (void)u16(&r); break;
        case T_DMASRC: {
            uint32_t n = u32(&r);
            if (q.left && !first[0]) snprintf(first, sizeof first, "frame %d: %u DMA bytes not read", frames, q.left);
            q.p = r.p; q.left = n;
            r.p += n;
            break;
        }
        case T_FRAME: {
            mdrt_vdp_beam_to(&beam, UINT32_MAX);              /* the rest of the frame, before any reset below */
            read_state(&r, &want);
            int w = u16(&r), h = u16(&r);
            mdrt_vdp_state got;
            mdrt_vdp_get_state(&vdp, &got);
            char what[160];
            if (state_diff(&got, &want, what, sizeof what)) {
                if (!state_bad++ && !first[0]) snprintf(first, sizeof first, "frame %d: %s", frames, what);
                mdrt_vdp_set_state(&vdp, &want);          /* go on from the oracle's state */
            }
            if (w != mdrt_vdp_width(&vdp) || h != mdrt_vdp_height(&vdp)) {
                if (!pic_bad++ && !first[0]) snprintf(first, sizeof first, "frame %d: %dx%d, expected %dx%d", frames, mdrt_vdp_width(&vdp), mdrt_vdp_height(&vdp), w, h);
            } else {
                mdrt_vdp_beam_to(&beam, UINT32_MAX);
                int bad = 0;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) {
                        uint16_t e = (uint16_t)(r.p[(y * w + x) * 2] | r.p[(y * w + x) * 2 + 1] << 8);
                        if (picture[y * 320 + x] != e) {
                            if (!bad++ && !pic_bad && !first[0]) snprintf(first, sizeof first, "frame %d: pixel %d,%d %03x, expected %03x", frames, x, y, picture[y * 320 + x], e);
                        }
                    }
                if (bad && !pic_bad) {
                    char base[64];
                    const char *slash = strrchr(path, '/');
                    snprintf(base, sizeof base, "%s", slash ? slash + 1 : path);
                    char *dot = strchr(base, '.');
                    if (dot) *dot = 0;
                    framediff(base, frames, r.p, w, h);
                }
                if (bad) pic_bad++;
            }
            r.p += (size_t)w * h * 2;
            frames++;
            mdrt_vdp_beam_start(&beam, &vdp, picture, 320);
            break;
        }
        case T_END: ended = 1; break;
        default:
            printf("  %s: bad tag %d\n", path, t);
            check_failures++;
            free(buf);
            return;
        }
    }
    if (q.over && !first[0]) snprintf(first, sizeof first, "a DMA read past its recorded bytes");
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    printf("  %-20s %3d frames: state %d bad, picture %d bad, %d / %d reads, %d writes during the display%s%s\n", name, frames,
           state_bad, pic_bad, reads - reads_bad, reads, mid, first[0] ? "; first: " : "", first);
    check_count++;
    if (state_bad || pic_bad || reads_bad || q.over || !ended) check_failures++;
    free(buf);
}

int main(void)
{
    int traces = 0;
    const char *dir = check_vectors();
    if (!dir) return check_skip("vdp_trace", "no vectors (MDRT_VECTORS)");
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[1024];
    while (d && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 9 || strcmp(e->d_name + n - 9, ".vdptrace")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        replay(path);
        traces++;
    }
    if (d) closedir(d);
    if (!traces) check_skip("vdp_trace", "no .vdptrace in MDRT_VECTORS");
    return check_done("vdp_trace");
}
