#pragma once

// Tiny zero-dependency test assertion harness for AetherFlow's first-party
// unit tests. No gtest / catch2 / external dependency: a single global failure
// counter, a CHECK(cond) macro that prints file:line on failure, and a
// TEST_MAIN_RETURN() helper that turns the counter into a process exit code so
// CTest sees pass (0) / fail (nonzero).
//
// Usage:
//   #include "test_assert.h"
//   int main() {
//       CHECK(1 + 1 == 2);
//       CHECK_MSG(foo() == 3, "foo should be 3");
//       return aetherflow_test::Summary();
//   }

#include <cstdio>

namespace aetherflow_test {

inline int& FailureCount() {
    static int failures = 0;
    return failures;
}

inline int& CheckCount() {
    static int checks = 0;
    return checks;
}

inline void RecordCheck(bool condition,
                        const char* expr,
                        const char* file,
                        int line,
                        const char* msg) {
    ++CheckCount();
    if (!condition) {
        ++FailureCount();
        std::fprintf(stderr,
                     "[FAIL] %s:%d: CHECK(%s)%s%s\n",
                     file,
                     line,
                     expr,
                     msg ? " -- " : "",
                     msg ? msg : "");
    }
}

// Prints a one-line summary and returns the process exit code: 0 when every
// CHECK passed, 1 when any CHECK failed.
inline int Summary() {
    const int failures = FailureCount();
    const int checks = CheckCount();
    if (failures == 0) {
        std::fprintf(stdout, "[PASS] all %d checks passed\n", checks);
        return 0;
    }
    std::fprintf(stderr, "[FAIL] %d of %d checks failed\n", failures, checks);
    return 1;
}

} // namespace aetherflow_test

#define CHECK(cond) \
    ::aetherflow_test::RecordCheck((cond), #cond, __FILE__, __LINE__, nullptr)

#define CHECK_MSG(cond, msg) \
    ::aetherflow_test::RecordCheck((cond), #cond, __FILE__, __LINE__, (msg))
