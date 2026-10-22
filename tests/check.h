#ifndef CHECK_H
#define CHECK_H

/* Minimal test checks. Each failing check prints file:line and is counted;
 * tests keep running so one run reports every failure. A test's main()
 * ends with `return CHECK_RESULT();` so make test / CI see a nonzero exit. */
#include <stdio.h>

static int check_failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",                \
                    __FILE__, __LINE__, #cond);                         \
            check_failures++;                                           \
        }                                                               \
    } while (0)

/* Integer equality; prints both values on failure. */
#define CHECK_EQ(a, b)                                                  \
    do {                                                                \
        long long check_a_ = (long long)(a);                            \
        long long check_b_ = (long long)(b);                            \
        if (check_a_ != check_b_) {                                     \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s "         \
                    "(%lld vs %lld)\n", __FILE__, __LINE__, #a, #b,     \
                    check_a_, check_b_);                                \
            check_failures++;                                           \
        }                                                               \
    } while (0)

/* Print a summary and give main() its exit status. */
#define CHECK_RESULT()                                                  \
    (check_failures == 0                                                \
         ? (printf("all checks passed\n"), 0)                           \
         : (fprintf(stderr, "%d check(s) failed\n", check_failures), 1))

#endif
