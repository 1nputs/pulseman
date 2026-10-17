/* Settings as text (mdrt/settings.h): the lines in order, each a key and value or kept raw. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mdrt/settings.h"

typedef struct line { char *key, *value, *raw; } line;    /* key NULL: a raw line (comment, blank) */
struct mdrt_settings { line *l; size_t n, cap; int dirty; };

static int space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* a copy of [a, b) without its surrounding blanks */
static char *trimmed(const char *a, const char *b)
{
    while (a < b && space(*a)) a++;
    while (b > a && space(b[-1])) b--;
    char *t = malloc((size_t)(b - a) + 1);
    if (!t) return NULL;
    memcpy(t, a, (size_t)(b - a));
    t[b - a] = 0;
    return t;
}

static char *copy(const char *s) { return trimmed(s, s + strlen(s)); }

static line *add(mdrt_settings *s)
{
    if (s->n == s->cap) {
        size_t cap = s->cap ? 2 * s->cap : 32;
        line *l = realloc(s->l, cap * sizeof *l);
        if (!l) return NULL;
        s->l = l;
        s->cap = cap;
    }
    line *l = &s->l[s->n++];
    l->key = l->value = l->raw = NULL;
    return l;
}

static line *find(const mdrt_settings *s, const char *key)
{
    for (size_t i = 0; i < s->n; i++)
        if (s->l[i].key && !strcmp(s->l[i].key, key)) return &s->l[i];
    return NULL;
}

mdrt_settings *mdrt_settings_new(void) { return calloc(1, sizeof(mdrt_settings)); }

void mdrt_settings_free(mdrt_settings *s)
{
    if (!s) return;
    for (size_t i = 0; i < s->n; i++) { free(s->l[i].key); free(s->l[i].value); free(s->l[i].raw); }
    free(s->l);
    free(s);
}

int mdrt_settings_parse(mdrt_settings *s, const char *text, size_t len)
{
    int entries = 0;
    const char *p = text, *end = text + len;
    while (p < end) {
        const char *e = memchr(p, '\n', (size_t)(end - p));
        if (!e) e = end;
        const char *a = p;
        while (a < e && space(*a)) a++;
        const char *eq = memchr(a, '=', (size_t)(e - a));
        if (a == e || *a == '#' || *a == ';' || !eq || eq == a) {   /* a comment, a blank, or not a setting */
            line *l = add(s);
            if (l) l->raw = trimmed(p, e);
        } else {
            char *key = trimmed(a, eq), *value = trimmed(eq + 1, e);
            if (key && value) {
                line *l = find(s, key);
                if (l) { free(l->value); l->value = value; free(key); }
                else if ((l = add(s))) { l->key = key; l->value = value; }
                entries++;
            } else { free(key); free(value); }
        }
        p = e < end ? e + 1 : end;
    }
    return entries;
}

const char *mdrt_settings_get(const mdrt_settings *s, const char *key)
{
    line *l = find(s, key);
    return l ? l->value : NULL;
}

int mdrt_settings_get_int(const mdrt_settings *s, const char *key, int fallback)
{
    const char *v = mdrt_settings_get(s, key);
    if (!v || !*v) return fallback;
    char *e;
    long n = strtol(v, &e, 10);
    return *e ? fallback : (int)n;
}

void mdrt_settings_set(mdrt_settings *s, const char *key, const char *value)
{
    line *l = find(s, key);
    if (l && !strcmp(l->value, value)) return;
    char *v = copy(value);
    if (!v) return;
    if (!l) {
        char *k = copy(key);
        if (!k || !(l = add(s))) { free(k); free(v); return; }
        l->key = k;
    }
    free(l->value);
    l->value = v;
    s->dirty = 1;
}

void mdrt_settings_set_int(mdrt_settings *s, const char *key, int value)
{
    char v[24];
    snprintf(v, sizeof v, "%d", value);
    mdrt_settings_set(s, key, v);
}

void mdrt_settings_default(mdrt_settings *s, const char *key, const char *value)
{
    if (!find(s, key)) mdrt_settings_set(s, key, value);
}

void mdrt_settings_remove(mdrt_settings *s, const char *key)
{
    line *l = find(s, key);
    if (!l) return;
    free(l->key);
    free(l->value);
    memmove(l, l + 1, (size_t)(s->l + s->n - (l + 1)) * sizeof *l);
    s->n--;
    s->dirty = 1;
}

size_t mdrt_settings_format(const mdrt_settings *s, char *out, size_t size)
{
    size_t need = 1;
    for (size_t i = 0; i < s->n; i++)
        need += s->l[i].key ? strlen(s->l[i].key) + 2 + (*s->l[i].value ? 1 + strlen(s->l[i].value) : 0) + 1 : strlen(s->l[i].raw) + 1;
    if (!out || size < need) return need;
    char *p = out;
    for (size_t i = 0; i < s->n; i++)
        p += s->l[i].key ? sprintf(p, "%s =%s%s\n", s->l[i].key, *s->l[i].value ? " " : "", s->l[i].value)   /* ("key =" when empty) */
                         : sprintf(p, "%s\n", s->l[i].raw);
    *p = 0;
    return need;
}

int mdrt_settings_dirty(const mdrt_settings *s) { return s->dirty; }
void mdrt_settings_clean(mdrt_settings *s) { s->dirty = 0; }
