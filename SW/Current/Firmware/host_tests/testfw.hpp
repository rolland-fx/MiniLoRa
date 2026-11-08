// Minimal zero-dependency test harness.
//
// Choice rationale (docs/PHASE2_DOMAIN.md): GoogleTest adds a fetch + build
// step and nothing here needs its fixtures/matchers; ~60 lines of assert-macro
// gives identical CI value with zero external deps. Swap to GTest later if
// the suite grows beyond simple value checks.

#pragma once

#include <cstdio>
#include <string_view>

namespace testfw {

inline int g_failures = 0;
inline int g_checks = 0;
inline std::string_view g_current;

inline void fail(std::string_view what, std::string_view file, int line)
{
    ++g_failures;
    std::printf("  FAIL [%s] %s:%d  %s\n",
                g_current.data(), file.data(), line, what.data());
}

inline void report()
{
    std::printf("%s: %d checks, %d failures\n",
                g_current.data(), g_checks, g_failures ? 1 : 0);
}

} // namespace testfw

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++testfw::g_checks;                                                \
        if (!(cond)) { testfw::fail(#cond, __FILE__, __LINE__); }          \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        ++testfw::g_checks;                                                \
        if (!((a) == (b))) { testfw::fail(#a " == " #b, __FILE__, __LINE__); } \
    } while (0)

#define RUN(fn)                                                            \
    do {                                                                   \
        testfw::g_current = #fn;                                           \
        fn();                                                              \
        testfw::report();                                                  \
    } while (0)
