#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "check.h"

int check_failures, check_count;

void check_fail(const char *file, int line, const char *what)
{
    if (check_failures < 20) printf("  FAIL %s:%d: %s\n", file, line, what);
    check_failures++;
}

int check_done(const char *name)
{
    printf("%s: %d checks, %d failed\n", name, check_count, check_failures);
    return check_failures ? 1 : 0;
}

int check_skip(const char *name, const char *why)
{
    const char *r = getenv("MDRT_REQUIRE_VECTORS");
    int required = r && *r && strcmp(r, "0");
    printf("%s: %s, %s\n", name, why, required ? "FAILED (MDRT_REQUIRE_VECTORS is set)" : "skipped");
    if (required) check_failures++;
    return required;
}

const char *check_vectors(void)
{
    const char *d = getenv("MDRT_VECTORS");
    struct stat st;
    if (!d || stat(d, &st) != 0 || !S_ISDIR(st.st_mode)) return NULL;
    return d;
}
