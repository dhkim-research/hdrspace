#pragma once

// A very small test runner for hdrspace's own calculations (no external framework).

#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace hdrspace::test {

struct TestCase {
    const char* name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

inline int& failureCount() {
    static int count = 0;
    return count;
}

struct Register {
    Register(const char* name, std::function<void()> body) {
        registry().push_back({name, std::move(body)});
    }
};

inline void fail(const char* file, int line, const std::string& what) {
    ++failureCount();
    std::cerr << file << ":" << line << ": " << what << "\n";
}

} // namespace hdrspace::test

#define HS_TEST_CAT2(a, b) a##b
#define HS_TEST_CAT(a, b) HS_TEST_CAT2(a, b)
#define TEST_CASE(name)                                                                              \
    static void HS_TEST_CAT(hsTest, __LINE__)();                                                     \
    static const hdrspace::test::Register HS_TEST_CAT(hsRegister, __LINE__)(name, HS_TEST_CAT(hsTest, __LINE__)); \
    static void HS_TEST_CAT(hsTest, __LINE__)()

#define CHECK(condition)                                                                             \
    do {                                                                                             \
        if (!(condition)) {                                                                          \
            hdrspace::test::fail(__FILE__, __LINE__, "CHECK(" #condition ")");                        \
        }                                                                                            \
    } while (0)

#define CHECK_NEAR(actual, expected, tolerance)                                                      \
    do {                                                                                             \
        const double hsActual = (actual);                                                            \
        const double hsExpected = (expected);                                                        \
        if (!(std::fabs(hsActual - hsExpected) <= (tolerance))) {                                    \
            hdrspace::test::fail(__FILE__, __LINE__, std::string("CHECK_NEAR(" #actual ", " #expected "): ") + \
                std::to_string(hsActual) + " vs " + std::to_string(hsExpected));                     \
        }                                                                                            \
    } while (0)
