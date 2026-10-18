// Tiny assertion harness for the kit / pipe_base unit tests. Plain C++17,
// no dependencies. Tests register themselves with TEST(name) { ... } and the
// runner in main.cpp executes them all, reporting the first failing CHECK of
// each test with file:line.
#pragma once
#include <cmath>
#include <cstdio>

namespace testkit {

struct TestCase
{
    const char* name;
    void (*fn)();
};

struct Registry
{
    static constexpr int kMax = 256;
    TestCase cases[kMax];
    int      count = 0;

    static Registry& get()
    {
        static Registry r;
        return r;
    }
};

struct Registrar
{
    Registrar(const char* name, void (*fn)())
    {
        Registry& r = Registry::get();
        if (r.count < Registry::kMax) r.cases[r.count++] = TestCase{ name, fn };
    }
};

inline int  g_failures = 0;
inline bool g_currentFailed = false;

inline void fail(const char* file, int line, const char* expr)
{
    if (!g_currentFailed) std::printf("    FAIL %s:%d  %s\n", file, line, expr);
    g_currentFailed = true;
    ++g_failures;
}

inline bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

} // namespace testkit

#define TEST(name)                                                                   \
    static void test_##name();                                                       \
    static ::testkit::Registrar registrar_##name(#name, &test_##name);               \
    static void test_##name()

#define CHECK(expr)                                                                  \
    do { if (!(expr)) ::testkit::fail(__FILE__, __LINE__, #expr); } while (0)

#define CHECK_NEAR(a, b, tol)                                                        \
    do { if (!::testkit::near((a), (b), (tol))) ::testkit::fail(__FILE__, __LINE__, #a " ~= " #b); } while (0)
