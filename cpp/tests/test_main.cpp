#include "test_framework.hpp"

#include <cstdio>
#include <exception>

#include "kv/net/socket.hpp"

namespace kvtest {

int g_failures = 0;
int g_checks = 0;
const char* g_current = "";

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

void report_failure(const char* file, int line, const std::string& message) {
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d  [%s]\n  %s\n", file, line, g_current, message.c_str());
}

int run_all() {
    int failed_tests = 0;
    int ran = 0;
    for (const auto& test : registry()) {
        g_current = test.name;
        const int before = g_failures;
        ++ran;
        try {
            test.fn();
        } catch (const std::exception& ex) {
            report_failure(__FILE__, __LINE__,
                           std::string("uncaught exception: ") + ex.what());
        } catch (...) {
            report_failure(__FILE__, __LINE__, "uncaught non-standard exception");
        }
        if (g_failures != before) {
            ++failed_tests;
            std::fprintf(stderr, "  -> %s FAILED\n", test.name);
        }
    }
    std::printf("\n%d test(s), %d check(s), %d failure(s)\n", ran, g_checks, g_failures);
    if (failed_tests > 0) {
        std::printf("%d test case(s) had failures\n", failed_tests);
    }
    return g_failures == 0 ? 0 : 1;
}

}  // namespace kvtest

int main() {
    std::string error;
    if (!kv::net::net_init(error)) {
        std::fprintf(stderr, "net_init failed: %s\n", error.c_str());
        return 2;
    }
    const int status = kvtest::run_all();
    kv::net::net_cleanup();
    return status;
}
