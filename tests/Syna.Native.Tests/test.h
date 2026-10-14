#pragma once

// Minimal test harness: TEST registers a case, CHECK* throw on failure. No external framework.

#include <cmath>
#include <cstdio>
#include <format>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace syna_test {

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

// Path of a generated fixture; tests skip (by throwing Skip) when it is missing.
struct Skip : std::runtime_error {
    using std::runtime_error::runtime_error;
};
std::wstring fixture(const char* name);
std::wstring temp_path(const char* name);
// Opt-in real recording from SYNA_REAL_CLIP; skips when unset. Only ever read.
std::wstring real_clip();

}  // namespace syna_test

#define TEST(name)                                                         \
    static void name();                                                    \
    static ::syna_test::Registrar name##_registrar(#name, &name);           \
    static void name()

#define CHECK(cond)                                                                                       \
    do {                                                                                                  \
        if (!(cond)) throw ::syna_test::Failure(std::format("{}:{}: CHECK({})", __FILE__, __LINE__, #cond)); \
    } while (0)

#define CHECK_MSG(cond, ...)                                                                                            \
    do {                                                                                                                \
        if (!(cond))                                                                                                    \
            throw ::syna_test::Failure(std::format("{}:{}: CHECK({}) {}", __FILE__, __LINE__, #cond, std::format(__VA_ARGS__))); \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                                       \
    do {                                                                                                            \
        const double va_ = static_cast<double>(a), vb_ = static_cast<double>(b);                                    \
        if (std::fabs(va_ - vb_) > (tol))                                                                           \
            throw ::syna_test::Failure(std::format("{}:{}: CHECK_NEAR({}, {}) {} vs {} (tol {})", __FILE__, __LINE__, #a, #b, va_, vb_, \
                                                  static_cast<double>(tol)));                                       \
    } while (0)
