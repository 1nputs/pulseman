/* ZIP archives in memory (stored or deflated entries, mdrt/png.h's deflate): read, checked so
 * that one from anywhere (another player over the network) is safe to open, and written, the
 * same files always making the same bytes (in name order, no times or extras). A game's bundles
 * of data, or a ROM in a zip. Files and directories on disk are the caller's. */
#ifndef MDRT_ZIP_H
#define MDRT_ZIP_H
#include <stddef.h>
#include <stdint.h>

#define MDRT_ZIP_MAX_ENTRIES 20000
#define MDRT_ZIP_MAX_FILE (32u << 20)       /* (an entry, unpacked) */
#define MDRT_ZIP_MAX_TOTAL (256u << 20)     /* (all of them) */

typedef struct mdrt_zip_entry {
    const char *name;           /* (in the archive's memory, NUL-terminated copy: see below) */
    uint32_t size, packed, crc, at;   /* unpacked and packed sizes, CRC-32, its data's offset */
    int method;                 /* 0 stored, 8 deflated */
} mdrt_zip_entry;

typedef struct mdrt_zip {
    const uint8_t *data;
    size_t size;
    int count;
    mdrt_zip_entry *entry;      /* (malloc'd) */
    char *names;                /* (malloc'd: the names, each NUL-terminated) */
} mdrt_zip;

/* the archive in data (size bytes, kept by the caller while z is open): its central directory
 * read and checked (every entry inside it, stored or deflated, sizes within the limits, names
 * plain relative paths: no absolute ones, no "..", no backslashes or control characters, none
 * twice). 0, or -1 with why */
int mdrt_zip_open(mdrt_zip *z, const uint8_t *data, size_t size, char *why, size_t why_size);
void mdrt_zip_close(mdrt_zip *z);
int mdrt_zip_find(const mdrt_zip *z, const char *name);   /* its entry, -1 when none */
/* entry i unpacked into *out (malloc'd, a NUL after it), its CRC checked: its size, or -1 */
long mdrt_zip_read(const mdrt_zip *z, int i, uint8_t **out);

/* writing: files added in any order, then the archive made (in name order: the same files make
 * the same bytes; each deflated when that is smaller, else stored) */
typedef struct mdrt_zip_writer { int count, cap; struct mdrt_zip_file { char *name; uint8_t *data; size_t size; } *file; } mdrt_zip_writer;
int mdrt_zip_add(mdrt_zip_writer *w, const char *name, const void *data, size_t size);   /* (copied) 0, -1 */
size_t mdrt_zip_finish(mdrt_zip_writer *w, uint8_t **out);   /* the archive (malloc'd): its size, 0; w freed */
void mdrt_zip_writer_free(mdrt_zip_writer *w);

#endif
