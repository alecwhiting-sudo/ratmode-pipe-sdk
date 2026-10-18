#include "harness.hpp"

int main()
{
    testkit::Registry& r = testkit::Registry::get();
    int failedTests = 0;
    for (int i = 0; i < r.count; ++i)
    {
        testkit::g_currentFailed = false;
        r.cases[i].fn();
        std::printf("%s %s\n", testkit::g_currentFailed ? "FAIL" : "ok  ", r.cases[i].name);
        if (testkit::g_currentFailed) ++failedTests;
    }
    std::printf("%d tests, %d failed (%d failing checks)\n", r.count, failedTests, testkit::g_failures);
    return failedTests == 0 ? 0 : 1;
}
