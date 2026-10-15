/* A minimal test harness: CHECK records a failure and goes on; check_done() prints the count. */
#ifndef MDRT_TESTS_CHECK_H
#define MDRT_TESTS_CHECK_H
#include <stdio.h>

extern int check_failures, check_count;
void check_fail(const char *file, int line, const char *what);
int check_done(const char *name);

#define CHECK(cond) do { check_count++; if (!(cond)) check_fail(__FILE__, __LINE__, #cond); } while (0)
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); check_count++; \
    if (a_ != b_) { char m_[160]; snprintf(m_, sizeof m_, "%s == %s (%lld, expected %lld)", #a, #b, a_, b_); check_fail(__FILE__, __LINE__, m_); } } while (0)

/* the directory of test vectors, or NULL (the vector tests are then skipped) */
const char *check_vectors(void);

#endif
