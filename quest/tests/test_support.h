#pragma once

// Minimal test framework shared by the host test files: each TEST registers a function; CHECK
// records failures; host_tests.cpp's main() runs them all.

#include <cmath>
#include <cstdio>
#include <functional>
#include <utility>
#include <vector>

namespace testing {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}
inline int& failures() {
    static int count = 0;
    return count;
}
struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

}  // namespace testing

#define TEST(name)                                                \
    void name();                                                  \
    const testing::Register register_##name(#name, name);         \
    void name()
#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            ++testing::failures();                                              \
        }                                                                       \
    } while (0)
#define CHECK_NEAR(a, b, tol)                                                                          \
    do {                                                                                               \
        const double va = (a), vb = (b);                                                               \
        if (!(std::fabs(va - vb) <= (tol))) {                                                          \
            std::printf("  FAILED %s:%d: %s = %.5f, expected %.5f (+-%g)\n", __FILE__, __LINE__, #a, va, vb, \
                        static_cast<double>(tol));                                                     \
            ++testing::failures();                                                                     \
        }                                                                                              \
    } while (0)
