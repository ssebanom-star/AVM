// 초경량 단위 테스트 프레임워크 (외부 의존성 없음).
// 사용법:
//   TEST(SuiteName_TestName) { CHECK_EQ(a, b); }
#pragma once

#include <cinttypes>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace avm::test {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        Registry().push_back({name, std::move(fn)});
    }
};

inline int g_failures = 0;
inline const char* g_current_test = "";

inline void ReportFailure(const char* file, int line, const std::string& msg) {
    ++g_failures;
    std::fprintf(stderr, "FAIL %s (%s:%d): %s\n", g_current_test, file, line,
                 msg.c_str());
}

inline int RunAllTests() {
    int failed_tests = 0;
    for (const auto& tc : Registry()) {
        g_current_test = tc.name;
        const int before = g_failures;
        tc.fn();
        const bool ok = g_failures == before;
        if (!ok) ++failed_tests;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", tc.name);
    }
    std::printf("----\n%zu tests, %d failed\n", Registry().size(), failed_tests);
    return failed_tests == 0 ? 0 : 1;
}

} // namespace avm::test

#define TEST(name)                                                        \
    static void avm_test_##name();                                        \
    static ::avm::test::Registrar avm_registrar_##name(#name,             \
                                                       &avm_test_##name); \
    static void avm_test_##name()

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            ::avm::test::ReportFailure(__FILE__, __LINE__, "CHECK " #cond); \
        }                                                                 \
    } while (0)

#define CHECK_EQ(actual, expected)                                            \
    do {                                                                      \
        const auto a_ = (actual);                                             \
        const auto e_ = (expected);                                           \
        if (!(a_ == e_)) {                                                    \
            char buf_[256];                                                   \
            snprintf(buf_, sizeof(buf_),                                      \
                     "CHECK_EQ %s == %s (actual=0x%" PRIx64                   \
                     " expected=0x%" PRIx64 ")",                              \
                     #actual, #expected, (uint64_t)a_, (uint64_t)e_);         \
            ::avm::test::ReportFailure(__FILE__, __LINE__, buf_);             \
        }                                                                     \
    } while (0)
