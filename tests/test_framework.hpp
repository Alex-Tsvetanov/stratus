// A test runner in one header.
//
// It exists because the build must work on a machine with nothing but a compiler and
// CMake. A test framework fetched at configure time would be one more thing between a
// reader and a green run, and what is needed here is a registry, an assertion that prints
// the expression and the file, and a way to run one case by name so CTest can report the
// case rather than the binary.
#pragma once

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct Failure {
    std::string message;
};

using TestFn = void (*)();

inline std::map<std::string, TestFn>& registry() {
    static std::map<std::string, TestFn> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, TestFn fn) { registry()[name] = fn; }
};

[[noreturn]] inline void fail(const std::string& where, const std::string& what) {
    throw Failure{where + ": " + what};
}

inline bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

inline int run(int argc, char** argv) {
    if (argc < 2) {
        int failed = 0;
        for (const auto& [name, fn] : registry()) {
            try {
                fn();
                std::cout << "ok    " << name << "\n";
            } catch (const Failure& f) {
                std::cout << "FAIL  " << name << "\n      " << f.message << "\n";
                ++failed;
            }
        }
        std::cout << registry().size() - static_cast<std::size_t>(failed) << "/"
                  << registry().size() << " passed\n";
        return failed == 0 ? 0 : 1;
    }

    const std::string name = argv[1];
    const auto it = registry().find(name);
    if (it == registry().end()) {
        std::cerr << "unknown test case: " << name << "\n";
        return 2;
    }
    try {
        it->second();
    } catch (const Failure& f) {
        std::cerr << "FAIL " << name << "\n  " << f.message << "\n";
        return 1;
    }
    std::cout << "ok " << name << "\n";
    return 0;
}

}  // namespace testing

#define STRATUS_STR2(x) #x
#define STRATUS_STR(x) STRATUS_STR2(x)
#define STRATUS_WHERE (std::string(__FILE__ ":" STRATUS_STR(__LINE__)))

#define TEST(name)                                                     \
    static void name();                                                \
    static ::testing::Registrar reg_##name(#name, &name);              \
    static void name()

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) ::testing::fail(STRATUS_WHERE, "CHECK(" #cond ") failed"); \
    } while (0)

#define CHECK_EQ(a, b)                                                 \
    do {                                                               \
        const auto lhs_ = (a);                                         \
        const auto rhs_ = (b);                                         \
        if (!(lhs_ == rhs_)) {                                         \
            std::ostringstream os_;                                    \
            os_ << "CHECK_EQ(" #a ", " #b ") failed: " << lhs_ << " != " << rhs_; \
            ::testing::fail(STRATUS_WHERE, os_.str());                 \
        }                                                              \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                          \
    do {                                                               \
        const double lhs_ = static_cast<double>(a);                    \
        const double rhs_ = static_cast<double>(b);                    \
        if (!::testing::near(lhs_, rhs_, (eps))) {                     \
            std::ostringstream os_;                                    \
            os_ << "CHECK_NEAR(" #a ", " #b ") failed: " << lhs_ << " vs " << rhs_ \
                << " tolerance " << (eps);                             \
            ::testing::fail(STRATUS_WHERE, os_.str());                 \
        }                                                              \
    } while (0)
