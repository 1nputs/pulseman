/* ZIP archives in memory (mdrt/zip.h), from the format's application note (APPNOTE.TXT): the
 * end of central directory record found from the end, each central directory entry checked
 * against its local header and the archive's bounds; entries stored or deflated, no ZIP64, no
 * encryption. Writing: local headers, the central directory and its end, in name order, every
 * time and date the format's first (1980-01-01). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mdrt/png.h"
#include "mdrt/zip.h"

static int no(char *why, size_t size, const char *say)
{
    if (why && size) snprintf(why, size, "%s", say);
    return -1;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* a name an entry may have: a relative path of plain characters, no part of it empty, "." or ".." */
static int plain_name(const char *s)
{
    if (!*s || *s == '/') return 0;
    for (const char *p = s; *p; p++)
        if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E || *p == '\\' || *p == ':') return 0;
    for (const char *p = s; *p;) {
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if ((n == 0 && e) || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return 0;
        if (!e) break;
        p = e + 1;
    }
    return 1;
}

void mdrt_zip_close(mdrt_zip *z)
{
    free(z->entry);
    free(z->names);
    memset(z, 0, sizeof *z);
}

static int by_name(const void *a, const void *b) { return strcmp(((const mdrt_zip_entry *)a)->name, ((const mdrt_zip_entry *)b)->name); }

int mdrt_zip_open(mdrt_zip *z, const uint8_t *data, size_t size, char *why, size_t why_size)
{
    memset(z, 0, sizeof *z);
    if (size < 22) return no(why, why_size, "not a zip archive");
    size_t end = size - 22, stop = size > 22 + 0xFFFF ? size - 22 - 0xFFFF : 0;
    while (le32(data + end) != 0x06054B50) {              /* (its end record, before a comment) */
        if (end == stop) return no(why, why_size, "not a zip archive");
        end--;
    }
    const uint8_t *e = data + end;
    uint32_t count = le16(e + 10), cd_size = le32(e + 12), cd = le32(e + 16);
    if (le16(e + 4) || le16(e + 6) || le16(e + 8) != count) return no(why, why_size, "a zip archive in parts (only whole ones)");
    if (count == 0xFFFF || cd == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu) return no(why, why_size, "a ZIP64 archive (only ordinary ones)");
    if (count > MDRT_ZIP_MAX_ENTRIES) return no(why, why_size, "a zip archive of too many files");
    if (cd > end || cd_size > end - cd) return no(why, why_size, "a zip archive damaged (its directory)");
    z->entry = calloc(count ? count : 1, sizeof *z->entry);
    z->names = malloc(cd_size + count + 1);
    if (!z->entry || !z->names) { mdrt_zip_close(z); return no(why, why_size, "out of memory"); }
    z->data = data;
    z->size = size;
    size_t at = cd, names = 0;
    uint64_t total = 0;
    const char *say = "a zip archive damaged (an entry)";
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *c = data + at;
        if (at + 46 > cd + cd_size || le32(c) != 0x02014B50) goto bad;
        uint16_t flags = le16(c + 8), method = le16(c + 10), nlen = le16(c + 28), xlen = le16(c + 30), clen = le16(c + 32);
        uint32_t crc = le32(c + 16), packed = le32(c + 20), unpacked = le32(c + 24), local = le32(c + 42);
        if (at + 46 + (size_t)nlen + xlen + clen > cd + cd_size) goto bad;
        if (flags & 1) { say = "a zip archive with an encrypted file"; goto bad; }
        if (method != 0 && method != 8) { say = "a zip archive packed some other way than stored or deflated"; goto bad; }
        if (unpacked > MDRT_ZIP_MAX_FILE || (total += unpacked) > MDRT_ZIP_MAX_TOTAL) { say = "a zip archive too big unpacked"; goto bad; }
        if (method == 0 && packed != unpacked) goto bad;
        char *name = z->names + names;
        memcpy(name, c + 46, nlen);
        name[nlen] = 0;
        names += (size_t)nlen + 1;
        if (strlen(name) != nlen || !plain_name(name)) { say = "a zip archive with a file named as no file of a folder may be"; goto bad; }
        if (local > cd || 30 > cd - local || le32(data + local) != 0x04034B50) goto bad;
        size_t from = (size_t)local + 30 + le16(data + local + 26) + le16(data + local + 28);
        if (from > cd || packed > cd - from) goto bad;
        mdrt_zip_entry *E = &z->entry[i];
        E->name = name;
        E->size = unpacked;
        E->packed = packed;
        E->crc = crc;
        E->at = (uint32_t)from;
        E->method = method;
        at += 46 + (size_t)nlen + xlen + clen;
    }
    z->count = (int)count;
    qsort(z->entry, count, sizeof *z->entry, by_name);
    for (uint32_t i = 1; i < count; i++)
        if (!strcmp(z->entry[i - 1].name, z->entry[i].name)) { say = "a zip archive with a file twice"; goto bad; }
    return 0;
bad:
    mdrt_zip_close(z);
    return no(why, why_size, say);
}

int mdrt_zip_find(const mdrt_zip *z, const char *name)
{
    int lo = 0, hi = z->count;
    while (lo < hi) {
        int mid = (lo + hi) / 2, c = strcmp(z->entry[mid].name, name);
        if (!c) return mid;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

long mdrt_zip_read(const mdrt_zip *z, int i, uint8_t **out)
{
    *out = NULL;
    if (i < 0 || i >= z->count) return -1;
    const mdrt_zip_entry *E = &z->entry[i];
    uint8_t *b = malloc((size_t)E->size + 1);
    if (!b) return -1;
    size_t used;
    if (E->method == 0) memcpy(b, z->data + E->at, E->size);
    else if (mdrt_inflate(z->data + E->at, E->packed, b, E->size, &used) != (long)E->size) { free(b); return -1; }
    if (mdrt_crc32(0, b, E->size) != E->crc) { free(b); return -1; }
    b[E->size] = 0;
    *out = b;
    return (long)E->size;
}

/* ---- writing ------------------------------------------------------------------------------------ */

int mdrt_zip_add(mdrt_zip_writer *w, const char *name, const void *data, size_t size)
{
    if (!plain_name(name) || strlen(name) > 0xFFFF || size > MDRT_ZIP_MAX_FILE || w->count >= MDRT_ZIP_MAX_ENTRIES) return -1;
    if (w->count == w->cap) {
        int cap = w->cap ? w->cap * 2 : 64;
        void *p = realloc(w->file, (size_t)cap * sizeof *w->file);
        if (!p) return -1;
        w->file = p;
        w->cap = cap;
    }
    struct mdrt_zip_file *f = &w->file[w->count];
    f->name = malloc(strlen(name) + 1);
    f->data = malloc(size ? size : 1);
    if (!f->name || !f->data) { free(f->name); free(f->data); return -1; }
    strcpy(f->name, name);
    memcpy(f->data, data, size);
    f->size = size;
    w->count++;
    return 0;
}

void mdrt_zip_writer_free(mdrt_zip_writer *w)
{
    for (int i = 0; i < w->count; i++) { free(w->file[i].name); free(w->file[i].data); }
    free(w->file);
    memset(w, 0, sizeof *w);
}

static int file_order(const void *a, const void *b) { return strcmp(((const struct mdrt_zip_file *)a)->name, ((const struct mdrt_zip_file *)b)->name); }

static void put16le(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32le(uint8_t *p, uint32_t v) { put16le(p, v & 0xFFFF); put16le(p + 2, v >> 16); }

size_t mdrt_zip_finish(mdrt_zip_writer *w, uint8_t **out)
{
    *out = NULL;
    qsort(w->file, (size_t)w->count, sizeof *w->file, file_order);
    for (int i = 1; i < w->count; i++)
        if (!strcmp(w->file[i - 1].name, w->file[i].name)) { mdrt_zip_writer_free(w); return 0; }
    struct packed { uint8_t *z; size_t n; int method; uint32_t crc, at; } *pk = calloc((size_t)w->count + 1, sizeof *pk);
    size_t total = 22;
    for (int i = 0; pk && i < w->count; i++) {
        struct mdrt_zip_file *f = &w->file[i];
        pk[i].crc = mdrt_crc32(0, f->data, f->size);
        pk[i].n = f->size ? mdrt_deflate(f->data, f->size, &pk[i].z) : 0;
        pk[i].method = 8;
        if (!pk[i].z || pk[i].n >= f->size) { free(pk[i].z); pk[i].z = NULL; pk[i].n = f->size; pk[i].method = 0; }   /* (stored: not smaller deflated) */
        total += 30 + 46 + 2 * strlen(f->name) + pk[i].n;
    }
    uint8_t *b = pk && total < 0xFFFFFFFFu ? malloc(total) : NULL;
    if (!b) {
        for (int i = 0; pk && i < w->count; i++) free(pk[i].z);
        free(pk);
        mdrt_zip_writer_free(w);
        return 0;
    }
    uint8_t *p = b;
    for (int i = 0; i < w->count; i++) {                 /* the files, each after its local header */
        struct mdrt_zip_file *f = &w->file[i];
        size_t nlen = strlen(f->name);
        pk[i].at = (uint32_t)(p - b);
        put32le(p, 0x04034B50); put16le(p + 4, 20); put16le(p + 6, 0); put16le(p + 8, (unsigned)pk[i].method);
        put16le(p + 10, 0); put16le(p + 12, 0x21);            /* (00:00, 1980-01-01) */
        put32le(p + 14, pk[i].crc); put32le(p + 18, (uint32_t)pk[i].n); put32le(p + 22, (uint32_t)f->size);
        put16le(p + 26, (unsigned)nlen); put16le(p + 28, 0);
        memcpy(p + 30, f->name, nlen);
        p += 30 + nlen;
        memcpy(p, pk[i].z ? pk[i].z : f->data, pk[i].n);
        p += pk[i].n;
    }
    uint8_t *cd = p;
    for (int i = 0; i < w->count; i++) {                 /* the central directory */
        struct mdrt_zip_file *f = &w->file[i];
        size_t nlen = strlen(f->name);
        put32le(p, 0x02014B50); put16le(p + 4, 20); put16le(p + 6, 20); put16le(p + 8, 0); put16le(p + 10, (unsigned)pk[i].method);
        put16le(p + 12, 0); put16le(p + 14, 0x21);
        put32le(p + 16, pk[i].crc); put32le(p + 20, (uint32_t)pk[i].n); put32le(p + 24, (uint32_t)f->size);
        put16le(p + 28, (unsigned)nlen); put16le(p + 30, 0); put16le(p + 32, 0); put16le(p + 34, 0); put16le(p + 36, 0);
        put32le(p + 38, 0); put32le(p + 42, pk[i].at);
        memcpy(p + 46, f->name, nlen);
        p += 46 + nlen;
    }
    put32le(p, 0x06054B50); put16le(p + 4, 0); put16le(p + 6, 0); put16le(p + 8, (unsigned)w->count); put16le(p + 10, (unsigned)w->count);
    put32le(p + 12, (uint32_t)(p - cd)); put32le(p + 16, (uint32_t)(cd - b)); put16le(p + 20, 0);
    p += 22;
    for (int i = 0; i < w->count; i++) free(pk[i].z);
    free(pk);
    mdrt_zip_writer_free(w);
    *out = b;
    return (size_t)(p - b);
}
