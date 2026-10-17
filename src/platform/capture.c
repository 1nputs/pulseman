/* Captures (mdrt/capture.h). A frame's record: a head of 16 bytes (width, height, colours as
 * 16-bit numbers; the code size; a byte unused; the code stream's length and the sound's stereo
 * pairs as 32-bit numbers, all in the machine's byte order: a recording is read back where it was
 * made), the palette, the code stream (made even), the sound. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "mdrt/capture.h"
#include "mdrt/vdp.h"

enum { HEAD = 16, MAX_W = 320, MAX_H = 240 };

typedef struct head { uint16_t w, h, colours; uint8_t bits, unused; uint32_t code_len, sound_frames; } head;

static size_t body_size(const head *hd) { return hd->colours * 2u + ((hd->code_len + 1) & ~1u) + hd->sound_frames * 4u; }

struct mdrt_capture {
    uint8_t **ring;             /* the frames kept, the oldest at start */
    int keep, start, count;
    uint8_t *open;              /* the frame being made: its head, palette and code, the sound to come */
    size_t open_len;
    uint8_t idx[MAX_W * MAX_H];
    uint8_t *code;
    FILE *rec;
    long recorded;
};

mdrt_capture *mdrt_capture_new(int keep)
{
    mdrt_capture *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->code = malloc(mdrt_gif_lzw_bound(MAX_W * MAX_H));
    c->open = malloc(HEAD + 512 + mdrt_gif_lzw_bound(MAX_W * MAX_H) + 2);
    if (!c->code || !c->open) { mdrt_capture_free(c); return NULL; }
    mdrt_capture_keep(c, keep);
    return c;
}

static void drop_oldest(mdrt_capture *c)
{
    free(c->ring[c->start]);
    c->start = (c->start + 1) % c->keep;
    c->count--;
}

void mdrt_capture_keep(mdrt_capture *c, int keep)
{
    if (keep < 0) keep = 0;
    if (keep == c->keep) return;
    uint8_t **ring = keep ? calloc((size_t)keep, sizeof *ring) : NULL;
    if (keep && !ring) return;
    while (c->count > keep) drop_oldest(c);                 /* (the latest stay) */
    for (int i = 0; i < c->count; i++) ring[i] = c->ring[(c->start + i) % c->keep];
    free(c->ring);
    c->ring = ring;
    c->keep = keep;
    c->start = 0;
}

void mdrt_capture_free(mdrt_capture *c)
{
    if (!c) return;
    while (c->count) drop_oldest(c);
    free(c->ring);
    free(c->code);
    free(c->open);
    free(c);
}

void mdrt_capture_picture(mdrt_capture *c, const uint16_t *px, int w, int h, int pitch)
{
    c->open_len = 0;
    if (w <= 0 || h <= 0 || w > MAX_W || h > MAX_H) return;
    uint16_t pal[256];
    int colours = mdrt_gif_palette(px, w, h, pitch, c->idx, pal), bits = mdrt_gif_bits(colours);
    size_t len = mdrt_gif_lzw(c->idx, (size_t)w * h, bits, c->code, mdrt_gif_lzw_bound(MAX_W * MAX_H));
    if (!len) return;
    head hd = { (uint16_t)w, (uint16_t)h, (uint16_t)colours, (uint8_t)bits, 0, (uint32_t)len, 0 };
    uint8_t *p = c->open;
    memcpy(p, &hd, HEAD);
    memcpy(p + HEAD, pal, (size_t)colours * 2);
    memcpy(p + HEAD + colours * 2, c->code, len);
    if (len & 1) p[HEAD + colours * 2 + len++] = 0;
    c->open_len = HEAD + (size_t)colours * 2 + len;
}

void mdrt_capture_sound(mdrt_capture *c, const int16_t *samples, int frames)
{
    if (!c->open_len) return;
    if (frames < 0 || !samples) frames = 0;
    size_t len = c->open_len + (size_t)frames * 4;
    c->open_len = 0;
    if (!c->keep && !c->rec) return;
    uint8_t *r = malloc(len);
    if (!r) return;
    head hd;
    memcpy(&hd, c->open, HEAD);
    hd.sound_frames = (uint32_t)frames;
    memcpy(r, &hd, HEAD);
    memcpy(r + HEAD, c->open + HEAD, len - (size_t)frames * 4 - HEAD);
    memcpy(r + len - (size_t)frames * 4, samples, (size_t)frames * 4);
    if (c->rec) {
        if (fwrite(r, 1, len, c->rec) == len) c->recorded++;
        else { c->rec = NULL; c->recorded = -1; }
    }
    if (!c->keep) { free(r); return; }
    if (c->count == c->keep) drop_oldest(c);
    c->ring[(c->start + c->count) % c->keep] = r;
    c->count++;
}

void mdrt_capture_record(mdrt_capture *c, FILE *f)
{
    c->rec = f;
    if (f) c->recorded = 0;
}

long mdrt_capture_recorded(const mdrt_capture *c) { return c->recorded; }
int mdrt_capture_kept(const mdrt_capture *c) { return c->count; }

/* ---- clips ------------------------------------------------------------------------------------ */

struct mdrt_clip {
    uint8_t **recs;             /* in memory: the records, owned */
    long n, pos;
    FILE *f;                    /* or a recording's file, from its start */
    uint8_t *buf;
    size_t cap;
};

mdrt_clip *mdrt_capture_take(mdrt_capture *c)
{
    if (!c->count) return NULL;
    mdrt_clip *clip = calloc(1, sizeof *clip);
    uint8_t **recs = malloc((size_t)c->count * sizeof *recs);
    if (!clip || !recs) { free(clip); free(recs); return NULL; }
    for (int i = 0; i < c->count; i++) recs[i] = c->ring[(c->start + i) % c->keep];
    clip->recs = recs;
    clip->n = c->count;
    c->count = c->start = 0;
    return clip;
}

mdrt_clip *mdrt_clip_file(FILE *f)
{
    mdrt_clip *clip = calloc(1, sizeof *clip);
    if (!clip) return NULL;
    clip->f = f;
    mdrt_clip_rewind(clip);
    return clip;
}

void mdrt_clip_free(mdrt_clip *clip)
{
    if (!clip) return;
    for (long i = 0; i < clip->n; i++) free(clip->recs[i]);
    free(clip->recs);
    free(clip->buf);
    free(clip);
}

void mdrt_clip_rewind(mdrt_clip *clip)
{
    clip->pos = 0;
    if (clip->f) fseek(clip->f, 0, SEEK_SET);
}

static int sane(const head *hd)
{
    return hd->w && hd->h && hd->w <= MAX_W && hd->h <= MAX_H && hd->colours && hd->colours <= 256
        && hd->bits >= 2 && hd->bits <= 8 && hd->code_len && hd->code_len <= mdrt_gif_lzw_bound(MAX_W * MAX_H)
        && hd->sound_frames < 48000;
}

/* the next record's head, and with body its palette, code and sound */
static int next_rec(mdrt_clip *clip, head *hd, const uint8_t **body, int want_body)
{
    if (!clip->f) {
        if (clip->pos >= clip->n) return 0;
        const uint8_t *r = clip->recs[clip->pos++];
        memcpy(hd, r, HEAD);
        *body = r + HEAD;
        return sane(hd);
    }
    if (fread(hd, 1, HEAD, clip->f) != HEAD || !sane(hd)) return 0;
    size_t n = body_size(hd);
    if (!want_body) return fseek(clip->f, (long)n, SEEK_CUR) == 0;
    if (n > clip->cap) {
        uint8_t *b = realloc(clip->buf, n);
        if (!b) return 0;
        clip->buf = b;
        clip->cap = n;
    }
    *body = clip->buf;
    return fread(clip->buf, 1, n, clip->f) == n;
}

int mdrt_clip_next(mdrt_clip *clip, mdrt_capture_frame *f)
{
    head hd;
    const uint8_t *b;
    if (!next_rec(clip, &hd, &b, 1)) return 0;
    f->w = hd.w;
    f->h = hd.h;
    f->colours = hd.colours;
    f->bits = hd.bits;
    f->palette = (const uint16_t *)(const void *)b;
    f->code = b + hd.colours * 2;
    f->code_len = hd.code_len;
    f->sound = (const int16_t *)(const void *)(f->code + ((hd.code_len + 1) & ~1u));
    f->sound_frames = (int)hd.sound_frames;
    return 1;
}

long mdrt_clip_frames(mdrt_clip *clip, int *w, int *h)
{
    struct { int w, h; long n; } sizes[8] = { { 0, 0, 0 } };
    head hd;
    const uint8_t *b;
    long n = 0;
    mdrt_clip_rewind(clip);
    while (next_rec(clip, &hd, &b, 0)) {
        n++;
        for (int i = 0; i < 8; i++)
            if (!sizes[i].n || (sizes[i].w == hd.w && sizes[i].h == hd.h)) { sizes[i].w = hd.w; sizes[i].h = hd.h; sizes[i].n++; break; }
    }
    mdrt_clip_rewind(clip);
    int best = 0;
    for (int i = 1; i < 8; i++) if (sizes[i].n > sizes[best].n) best = i;
    if (w) *w = sizes[best].w;
    if (h) *h = sizes[best].h;
    return n;
}

/* ---- made into files ------------------------------------------------------------------------- */

/* a frame's pixels as indices, at w x h (stretched to it, nearest) */
static int indices(const mdrt_capture_frame *f, int w, int h, uint8_t *out)
{
    static _Thread_local uint8_t idx[MAX_W * MAX_H];
    if (!mdrt_gif_unlzw(f->code, f->code_len, f->bits, idx, (size_t)f->w * f->h)) return 0;
    if (w == f->w && h == f->h) { memcpy(out, idx, (size_t)w * h); return 1; }
    for (int y = 0; y < h; y++) {
        const uint8_t *row = idx + (size_t)(y * f->h / h) * f->w;
        for (int x = 0; x < w; x++) *out++ = row[x * f->w / w];
    }
    return 1;
}

int mdrt_clip_gif(mdrt_clip *clip, int curve, int scale, double fps, mdrt_gif_put put, void *user, volatile long *done)
{
    int dw, dh;
    if (mdrt_clip_frames(clip, &dw, &dh) <= 0 || fps <= 0) return 0;
    scale = scale < 1 ? 1 : scale > 4 ? 4 : scale;
    int W = dw * scale, H = dh * scale, step = fps > 50 ? 2 : 1, ok = 1;
    uint8_t *idx = malloc((size_t)W * H), *code = malloc(mdrt_gif_lzw_bound((size_t)W * H));
    if (!idx || !code) { free(idx); free(code); return 0; }
    mdrt_gif_begin(put, user, W, H);
    mdrt_capture_frame f;
    for (long i = 0; mdrt_clip_next(clip, &f); i++) {
        if (done) *done = i + 1;
        if (i % step) continue;
        int delay = (int)(lround((double)(i + step) * 100 / fps) - lround((double)i * 100 / fps));
        uint32_t rgb[256];
        for (int k = 0; k < f.colours; k++) rgb[k] = mdrt_vdp_rgb(f.palette[k], curve);
        if (f.w == W && f.h == H) { mdrt_gif_frame(put, user, 0, 0, W, H, delay, rgb, f.colours, f.bits, f.code, f.code_len); continue; }
        size_t len = indices(&f, W, H, idx) ? mdrt_gif_lzw(idx, (size_t)W * H, f.bits, code, mdrt_gif_lzw_bound((size_t)W * H)) : 0;
        if (!len) { ok = 0; break; }
        mdrt_gif_frame(put, user, 0, 0, W, H, delay, rgb, f.colours, f.bits, code, len);
    }
    mdrt_gif_end(put, user);
    free(idx);
    free(code);
    return ok;
}

int mdrt_clip_rgb(const mdrt_capture_frame *f, int curve, int w, int h, uint8_t *rgb)
{
    static _Thread_local uint8_t idx[MAX_W * MAX_H];
    if (w <= 0 || h <= 0 || (size_t)w * h > sizeof idx || !indices(f, w, h, idx)) return 0;
    uint32_t pal[256];
    for (int k = 0; k < 256; k++) pal[k] = k < f->colours ? mdrt_vdp_rgb(f->palette[k], curve) : 0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint32_t c = pal[idx[i]];
        *rgb++ = (uint8_t)(c >> 16);
        *rgb++ = (uint8_t)(c >> 8);
        *rgb++ = (uint8_t)c;
    }
    return 1;
}

static void le16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) { le16(p, v & 0xFFFF); le16(p + 2, v >> 16); }

void mdrt_clip_wav(mdrt_clip *clip, uint32_t rate, double fps, mdrt_gif_put put, void *user)
{
    long n = mdrt_clip_frames(clip, NULL, NULL);
    if (fps <= 0) fps = 60;
    uint32_t total = (uint32_t)llround((double)n * rate / fps), bytes = total * 4;
    uint8_t h[44] = "RIFF....WAVEfmt ";
    le32(h + 4, 36 + bytes);
    le32(h + 16, 16);
    le16(h + 20, 1);                                        /* PCM, two channels, 16 bits */
    le16(h + 22, 2);
    le32(h + 24, rate);
    le32(h + 28, rate * 4);
    le16(h + 32, 4);
    le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    le32(h + 40, bytes);
    put(user, h, 44);
    mdrt_capture_frame f;
    uint8_t buf[4096];
    for (long i = 0; mdrt_clip_next(clip, &f); i++) {
        long want = llround((double)(i + 1) * rate / fps) - llround((double)i * rate / fps);
        for (long k = 0; k < want;) {
            int m = 0;
            for (; m < (int)sizeof buf / 4 && k < want; m++, k++) {
                int16_t l = k < f.sound_frames ? f.sound[2 * k] : 0, r = k < f.sound_frames ? f.sound[2 * k + 1] : 0;
                le16(buf + 4 * m, (uint16_t)l);
                le16(buf + 4 * m + 2, (uint16_t)r);
            }
            put(user, buf, (size_t)m * 4);
        }
    }
}
