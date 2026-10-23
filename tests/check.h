#ifndef CHECK_H
#define CHECK_H

/* Minimal test checks. Each failing check prints file:line and is counted;
 * tests keep running so one run reports every failure. A test file lists
 * its cases in a test_case_t table and its main() is
 * `return RUN_TESTS(tests);`, which prints [ID] name PASS/FAIL per case
 * and gives make test / CI a nonzero exit if any case failed. */
#include <stddef.h>
#include <stdio.h>

static int check_failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "    %s:%d: CHECK failed: %s\n",            \
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
            fprintf(stderr, "    %s:%d: CHECK_EQ failed: %s == %s "     \
                    "(%lld vs %lld)\n", __FILE__, __LINE__, #a, #b,     \
                    check_a_, check_b_);                                \
            check_failures++;                                           \
        }                                                               \
    } while (0)

/* One case: an ID like "LSO-03", a short name, and a void test body. */
typedef struct {
    const char *id;
    const char *name;
    void (*run)(void);
} test_case_t;

/* Run every case in order. A case passes if it added no failures; the
 * count is snapshotted, never reset, so a later failure cannot flip an
 * earlier verdict. Flushing keeps a case's diagnostics above its line.
 * inline so files not yet using it don't trip -Wunused-function. */
static inline int run_tests(const test_case_t *t, size_t n)
{
    size_t passed = 0;
    for (size_t i = 0; i < n; i++) {
        int before = check_failures;
        t[i].run();
        int ok = (check_failures == before);
        fflush(stderr);
        printf("[%s] %-45s %s\n", t[i].id, t[i].name, ok ? "PASS" : "FAIL");
        fflush(stdout);
        if (ok) {
            passed++;
        }
    }
    printf("%zu/%zu tests passed\n", passed, n);
    return passed == n ? 0 : 1;
}

#define RUN_TESTS(tbl) run_tests((tbl), sizeof(tbl) / sizeof((tbl)[0]))

#endif
