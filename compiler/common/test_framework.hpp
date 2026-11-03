#pragma once

// A tiny test framework so the project has no downloads: TEST_CASE registers a
// function, CHECK/CHECK_EQ record failures, run_all() runs everything.

#include <iostream>
#include <vector>

namespace test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

template <typename A, typename B>
void check_eq(const A& a, const B& b, const char* expr_a, const char* expr_b, const char* file, int line) {
    if (a == b) return;
    ++failures();
    std::cerr << file << ":" << line << ": CHECK_EQ(" << expr_a << ", " << expr_b << ") failed\n";
    if constexpr (requires(std::ostream& os) { os << a; os << b; }) {
        std::cerr << "    left:  " << a << "\n    right: " << b << "\n";
    }
}

inline int run_all() {
    for (const auto& c : registry()) {
        int before = failures();
        c.fn();
        std::cout << (failures() == before ? "ok    " : "FAIL  ") << c.name << "\n";
    }
    std::cout << registry().size() << " tests, " << failures() << " failed checks\n";
    return failures() == 0 ? 0 : 1;
}

}  // namespace test

#define TEST_CASE(name)                                       \
    static void name();                                       \
    static test::Registrar name##_registrar(#name, name);     \
    static void name()

#define CHECK(...)                                                                                     \
    do {                                                                                               \
        if (!(__VA_ARGS__)) {                                                                          \
            ++test::failures();                                                                        \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK(" #__VA_ARGS__ ") failed\n";          \
        }                                                                                              \
    } while (0)

#define CHECK_EQ(a, b) test::check_eq((a), (b), #a, #b, __FILE__, __LINE__)
