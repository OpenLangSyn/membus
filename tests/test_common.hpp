/*
 * test_common.hpp — Shared test infrastructure for libmembus C++17 tests
 *
 * Provides: ASSERT_EQ, ASSERT_TRUE, ASSERT_MEM_EQ, TEST macro,
 *           global counters, and standalone main() injection.
 *
 * Standalone main() injection:
 *   Compile with -DTEST_MAIN_FN=<suite_fn> to generate main().
 *   The suite function must have signature:
 *     void fn(int& out_run, int& out_passed)
 */

#ifndef TEST_COMMON_HPP
#define TEST_COMMON_HPP

#include <cstdio>
#include <cstring>

/* ── Global test counters (file-scoped per standalone binary) ── */

static int tests_run    = 0;
static int tests_passed = 0;
static int _test_fail   = 0;

/* ── Assertion macros ── */

#define ASSERT_EQ(a, b) do {                                                \
    if (static_cast<long long>(a) != static_cast<long long>(b)) {           \
        std::printf("  FAIL %s:%d  expected %lld  got %lld\n",             \
               __FILE__, __LINE__,                                          \
               static_cast<long long>(b), static_cast<long long>(a));      \
        _test_fail = 1;                                                     \
    }                                                                       \
} while (0)

#define ASSERT_TRUE(x) do {                                                 \
    if (!(x)) {                                                             \
        std::printf("  FAIL %s:%d  expected true\n", __FILE__, __LINE__);  \
        _test_fail = 1;                                                     \
    }                                                                       \
} while (0)

#define ASSERT_MEM_EQ(a, b, n) do {                                         \
    if (std::memcmp((a), (b), static_cast<size_t>(n)) != 0) {              \
        std::printf("  FAIL %s:%d  memory mismatch (%zu bytes)\n",         \
               __FILE__, __LINE__, static_cast<size_t>(n));                 \
        _test_fail = 1;                                                     \
    }                                                                       \
} while (0)

/* ── Test runner macro ── */

#define TEST(fn) do {                                                       \
    _test_fail = 0;                                                         \
    tests_run++;                                                            \
    fn();                                                                   \
    if (!_test_fail) {                                                      \
        std::printf("  PASS: %s\n", #fn);                                  \
        tests_passed++;                                                     \
    }                                                                       \
} while (0)

/* ── Standalone main() injection ──
 *
 * Compile with -DTEST_MAIN_FN=<suite_fn> to generate main().
 * Example: -DTEST_MAIN_FN=test_membus_run
 */
#ifdef TEST_MAIN_FN
void TEST_MAIN_FN(int& out_run, int& out_passed);
int main() {
    int run = 0, passed = 0;
    TEST_MAIN_FN(run, passed);
    std::printf("\n%d/%d passed\n", passed, run);
    return (passed == run) ? 0 : 1;
}
#endif

#endif /* TEST_COMMON_HPP */
