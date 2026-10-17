/* Settings as text: one "key = value" a line, lines starting with # or ; are comments. A file is
 * read into the store, changed, and written back with its lines in their order, its comments and
 * the keys this build does not know kept as they were, so the player can edit it by hand. Pure
 * string work: reading and writing the file is the platform's (the SDL2 platform keeps its window,
 * scaling, sound and key bindings here). */
#ifndef MDRT_SETTINGS_H
#define MDRT_SETTINGS_H
#include <stddef.h>

typedef struct mdrt_settings mdrt_settings;

mdrt_settings *mdrt_settings_new(void);
void mdrt_settings_free(mdrt_settings *s);
/* read text into the store (keys already there take the text's values); the entries read */
int mdrt_settings_parse(mdrt_settings *s, const char *text, size_t len);
/* a key's value (NULL when absent), or as an integer (fallback when absent or not a number) */
const char *mdrt_settings_get(const mdrt_settings *s, const char *key);
int mdrt_settings_get_int(const mdrt_settings *s, const char *key, int fallback);
/* set a key (added at the end when new); the store is dirty when a value changed */
void mdrt_settings_set(mdrt_settings *s, const char *key, const char *value);
void mdrt_settings_set_int(mdrt_settings *s, const char *key, int value);
/* add a key with its default when it is absent (so a new file lists every setting) */
void mdrt_settings_default(mdrt_settings *s, const char *key, const char *value);
/* a key taken out (one a build no longer has); dirty when it was there */
void mdrt_settings_remove(mdrt_settings *s, const char *key);
/* the text: written into out when size is enough (with its terminating 0); returns the size it needs */
size_t mdrt_settings_format(const mdrt_settings *s, char *out, size_t size);
/* changed since it was last written (mdrt_settings_clean after writing it) */
int mdrt_settings_dirty(const mdrt_settings *s);
void mdrt_settings_clean(mdrt_settings *s);

#endif
