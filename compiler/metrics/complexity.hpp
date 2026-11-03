#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "metrics/clang_bridge.hpp"

// Time and space complexity of each function, read from Clang's syntax tree,
// with suggestions where a known rewrite gives a smaller bound.
//
// Sizes are named after what the program can see: a parameter n is "n", the
// length of an array a is "len(a)". A loop runs as many times as the distance
// between its start and its bound when its variable goes up or down by a
// constant, log of that when it is multiplied or divided, and the square root
// when the condition is i * i <= n. A call costs what the callee costs, with
// the callee's parameters replaced by the sizes of the arguments. Recursion is
// solved for the two common shapes: n - c (linear, or 2^n with two calls or
// more) and n / c (the master theorem). Space is what the function allocates
// (ParC never frees arrays, so an allocation in a loop adds up) plus the
// depth of its recursion.
//
// This is an estimate from the program text, an upper bound in big-O terms.
// What it cannot work out it shows as "?" with a note saying why.
namespace parc::metrics {

// One product of sizes: {n: 2}, {} logs, "" exp is n^2; exp "n" is 2^n.
struct Term {
    std::map<std::string, double> power;
    std::map<std::string, int> logs;
    std::string exp;
};

// A growth rate: the sum of its terms. No terms at all is zero (nothing allocated).
class BigO {
public:
    std::vector<Term> terms;

    static BigO zero() { return {}; }
    static BigO one();
    static BigO of(const std::string& size);

    bool is_zero() const { return terms.empty(); }
    bool constant() const;
    BigO plus(const BigO& other) const;
    BigO times(const BigO& other) const;
    BigO log() const;
    BigO sqrt() const;
    BigO pow(double p) const;
    BigO over(const BigO& divisor) const;  // drops the divisor's factors, for "this loop no longer runs"
    BigO substitute(const std::map<std::string, BigO>& sizes) const;
    double degree(const std::string& size) const;
    std::string main_size() const;          // the size with the largest power in the largest term
    bool grows_faster_than(const BigO& other) const;
    std::string str() const;                // "O(n^2)", "O(n log n)", "O(2^n)", "O(1)"
    std::string inner() const;              // the same without "O( )"
};

struct LoopInfo {
    int line = 0;
    std::string kind;   // "for", "while", "do"
    int depth = 1;      // 1 = not inside another loop
    std::string runs;   // "O(n)"
    std::string note;   // how the count was worked out, when it is not obvious
};

struct Suggestion {
    std::string kind;   // "exponential-recursion", "linear-recursion", "quadratic-sort", "prefix-sums",
                        // "trial-division", "invariant-call", "allocation-in-loop"
    int line = 0;
    std::string title;
    std::string why;
    std::string how;
    std::string time_now, time_better;
    std::string space_now, space_better;
};

struct FunctionComplexity {
    std::string name;
    int line = 0;
    BigO time;
    BigO space;
    std::string recursion;  // "", "linear", "exponential", "logarithmic", "divide and conquer", "mutual", "unknown"
    int nesting = 0;        // deepest loop nesting
    bool pure = true;       // no printing, no array writes, no allocation
    std::vector<LoopInfo> loops;
    std::vector<std::string> notes;
    std::vector<Suggestion> suggestions;
    std::vector<std::pair<std::string, std::string>> sizes;  // ("len(a)", "the length of array a")
};

struct ComplexityReport {
    bool ok = false;
    std::string error;
    std::vector<FunctionComplexity> functions;
};

ComplexityReport analyze_complexity(const CUnit& unit);

}  // namespace parc::metrics
