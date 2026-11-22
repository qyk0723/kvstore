#pragma once

// A tiny dependency-free test framework: registering a test is one macro, and
// `ctest` just runs the resulting binary.  No GoogleTest download, which means
// the suite builds offline and in CI without a package manager.

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace kvtest {

struct TestCase {
    const char* name;
    void (*fn)();
};

std::vector<TestCase>& registry();
int run_all();

extern int g_failures;
extern const char* g_current;
extern int g_checks;

void report_failure(const char* file, int line, const std::string& message);

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back(TestCase{name, fn}); }
};

inline void check(bool condition, const char* expression, const char* file, int line) {
    ++g_checks;
    if (!condition) {
        report_failure(file, line, std::string("expected true: ") + expression);
    }
}

inline void check_eq(long long actual, long long expected, const char* expression,
                     const char* file, int line) {
    ++g_checks;
    if (actual != expected) {
        report_failure(file, line, std::string(expression) + "\n    actual:   " +
                                       std::to_string(actual) + "\n    expected: " +
                                       std::to_string(expected));
    }
}

inline void check_str(std::string_view actual, std::string_view expected, const char* expression,
                      const char* file, int line) {
    ++g_checks;
    if (actual != expected) {
        std::string message = std::string(expression);
        message += "\n    actual:   [";
        message.append(actual.data(), actual.size());
        message += "]\n    expected: [";
        message.append(expected.data(), expected.size());
        message += "]";
        report_failure(file, line, message);
    }
}

}  // namespace kvtest

#define KV_TEST(name)                                    \
    static void name();                                  \
    static ::kvtest::Registrar kv_registrar_##name(#name, name); \
    static void name()

#define KV_CHECK(condition) ::kvtest::check((condition), #condition, __FILE__, __LINE__)

#define KV_CHECK_EQ(actual, expected)                                        \
    ::kvtest::check_eq(static_cast<long long>(actual), static_cast<long long>(expected), \
                       #actual " == " #expected, __FILE__, __LINE__)

#define KV_CHECK_STR(actual, expected) \
    ::kvtest::check_str((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)
