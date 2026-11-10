#include "TestHarness.h"

int main() {
    int failedCases = 0;
    for (const auto& testCase : hdrspace::test::registry()) {
        const int before = hdrspace::test::failureCount();
        try {
            testCase.body();
        } catch (const std::exception& e) {
            hdrspace::test::fail(__FILE__, __LINE__, std::string(testCase.name) + " threw: " + e.what());
        }
        const bool ok = hdrspace::test::failureCount() == before;
        failedCases += ok ? 0 : 1;
        std::cout << (ok ? "ok    " : "FAIL  ") << testCase.name << "\n";
    }
    std::cout << hdrspace::test::registry().size() - static_cast<size_t>(failedCases) << " of "
              << hdrspace::test::registry().size() << " tests passed\n";
    return failedCases == 0 ? 0 : 1;
}
