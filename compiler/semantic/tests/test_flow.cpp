// Tests for the semantic checks that run on the control-flow graph.

#include <source_location>
#include <string>
#include <vector>

#include "common/test_framework.hpp"
#include "common/test_support.hpp"

using namespace parc;
using test::contains;

namespace {

using Lines = std::vector<std::string>;

const std::vector<std::string> kHazards = {"overflow",      "underflow",           "divide-by-zero",
                                           "null-access",   "index-out-of-bounds", "negative-size"};

// Checks the "line:kind" list of the findings of the given kinds.
void expect(const std::string& source, const std::vector<std::string>& kinds, const Lines& want,
            std::source_location loc = std::source_location::current()) {
    Compilation c = compile(source);
    Lines got = test::findings(c, kinds);
    if (c.lowered && got == want) return;
    ++test::failures();
    std::cerr << loc.file_name() << ":" << loc.line() << ": findings for:\n" << source << "\n    want:";
    for (const auto& w : want) std::cerr << " " << w;
    std::cerr << "\n    got: ";
    for (const auto& g : got) std::cerr << " " << g;
    std::cerr << "\n";
    for (const auto& d : c.syntax_errors) std::cerr << "    " << format(d) << "\n";
    for (const auto& d : c.findings) std::cerr << "    " << format(d) << "\n";
}

void hazards(const std::string& source, const Lines& want, std::source_location loc = std::source_location::current()) {
    expect(source, kHazards, want, loc);
}

std::string message_of(const std::string& source, const std::string& kind) {
    Compilation c = compile(source);
    const Diagnostic* d = test::find_kind(c, kind);
    return d ? d->message : "";
}

}  // namespace

// ---------------------------------------------------------------- reachability

TEST_CASE(code_after_return_break_or_continue_is_unreachable) {
    expect("fn f() -> int {\n return 1;\n print(2);\n print(3);\n}", {"unreachable-code"}, {"3:unreachable-code"});
    expect("fn f(n: int) {\n while (n > 0) {\n  break;\n  n = n - 1;\n }\n}", {"unreachable-code"}, {"4:unreachable-code"});
    expect("fn f(n: int) {\n for i in 0..n {\n  continue;\n  print(i);\n }\n}", {"unreachable-code"}, {"4:unreachable-code"});
    expect("fn f(c: bool) -> int {\n if (c) { return 1; } else { return 2; }\n print(0);\n}", {"unreachable-code"},
           {"3:unreachable-code"});
    expect("fn f(c: bool) -> int {\n if (c) { return 1; }\n return 2;\n}", {"unreachable-code"}, {});
}

TEST_CASE(a_function_with_a_return_type_must_return_on_every_path) {
    expect("fn f(c: bool) -> int {\n if (c) { return 1; }\n}", {"missing-return"}, {"1:missing-return"});
    expect("fn f(n: int) -> int {\n while (n > 0) { return n; }\n}", {"missing-return"}, {"1:missing-return"});
    expect("fn f(c: bool) -> int { if (c) { return 1; } else { return 2; } }", {"missing-return"}, {});
    expect("fn f() -> int { while (true) { return 1; } }", {"missing-return"}, {});
    expect("fn f() { print(1); }", {"missing-return"}, {});
    Compilation c = compile("fn f(c: bool) -> int { if (c) { return 1; } }");
    CHECK(!c.runnable);  // it is an error, so there is no code
    CHECK_EQ(test::find_kind(c, "missing-return")->severity, "error");
}

// ---------------------------------------------------------------- definite assignment

TEST_CASE(a_variable_must_be_assigned_on_every_path_before_it_is_read) {
    expect("fn f(c: bool) {\n let x: int;\n if (c) { x = 1; }\n print(x);\n}", {"uninitialized"}, {"4:uninitialized"});
    expect("fn f() {\n let x: int;\n print(x);\n print(x);\n}", {"uninitialized"}, {"3:uninitialized"});  // once per variable
    expect("fn f(c: bool) { let x: int; if (c) { x = 1; } else { x = 2; } print(x); }", {"uninitialized"}, {});
    expect("fn f() { let x: int; x = 5; print(x); }", {"uninitialized"}, {});
    expect("fn f(n: int) {\n let x: int;\n while (n > 0) { x = n; n = n - 1; }\n print(x);\n}", {"uninitialized"},
           {"4:uninitialized"});  // the loop may run zero times
    CHECK(contains(message_of("fn f() { let total: int; print(total); }", "uninitialized"), "'total' is used here"));
}

// ---------------------------------------------------------------- dead stores

TEST_CASE(a_value_that_is_never_read_is_a_dead_store) {
    expect("fn f() {\n let x: int = 0;\n x = 5;\n x = 6;\n print(x);\n}", {"dead-store"}, {"3:dead-store"});
    expect("fn f(n: int) -> int {\n let x: int = n * 2;\n x = 3;\n return x;\n}", {"dead-store"}, {"2:dead-store"});
    // "let x: int = 0;" before the real first value is a harmless habit.
    expect("fn f(n: int) -> int { let x: int = 0; if (n > 0) { x = 1; } else { x = 2; } return x; }", {"dead-store"}, {});
    expect("fn f(n: int) -> int { let s: int = 0; for i in 0..n { s = s + i; } return s; }", {"dead-store"}, {});
    // A variable that is never read at all has the unused-variable warning, not this one.
    expect("fn f() { let x: int = 1; x = 2; }", {"dead-store", "unused-variable"}, {"1:unused-variable"});
}

// ---------------------------------------------------------------- call graph

TEST_CASE(recursion_is_found_as_a_cycle_in_the_call_graph) {
    expect("fn fact(n: int) -> int {\n if (n <= 1) { return 1; }\n return n * fact(n - 1);\n}", {"recursion"}, {"1:recursion"});
    expect("fn even(n: int) -> bool { if (n == 0) { return true; } return odd(n - 1); }\n"
           "fn odd(n: int) -> bool { if (n == 0) { return false; } return even(n - 1); }",
           {"recursion"}, {"1:recursion"});
    CHECK_EQ(message_of("fn a(n: int) { b(n); } fn b(n: int) { a(n); }", "recursion"), "'a' and 'b' call each other (recursion)");
    expect("fn g() { } fn f() { g(); g(); }", {"recursion"}, {});
}

TEST_CASE(functions_main_never_reaches_are_reported) {
    expect("fn used() { }\nfn spare() { }\nfn main() { used(); }", {"unused-function"}, {"2:unused-function"});
    expect("fn helper() { }\nfn other() { helper(); }", {"unused-function"}, {});  // no main: nothing to judge by
}

// ---------------------------------------------------------------- divide by zero (CWE-369)

TEST_CASE(division_by_a_value_that_may_be_zero) {
    hazards("fn f(a: int, b: int) -> int {\n return a / b;\n}", {"2:divide-by-zero"});
    hazards("fn f(a: int, b: int) -> int {\n return a % b;\n}", {"2:divide-by-zero"});
    hazards("fn f(a: float, b: float) -> float { return a / b; }", {});  // float division gives infinity, it does not stop
    hazards("fn f(a: int) -> int { return a / 4; }", {});
    CHECK_EQ(message_of("fn f(a: int, b: int) -> int { return a / b; }", "divide-by-zero"),
             "'a / b' divides by zero when b is 0");
    CHECK_EQ(message_of("fn f(a: int) -> int { let z: int = 0; return a / z; }", "divide-by-zero"),
             "'a / z' always divides by zero: z is 0 here");
}

TEST_CASE(a_check_before_the_division_removes_the_warning) {
    hazards("fn f(a: int, b: int) -> int { if (b != 0) { return a / b; } return 0; }", {});
    hazards("fn f(a: int, b: int) -> int { if (b == 0) { return 0; } return a / b; }", {});
    hazards("fn f(a: int, b: int) -> int { if (b > 0) { return a / b; } return 0; }", {});
    hazards("fn f(a: int, b: int) -> int { if (b < 1) { return 0; } return a / b; }", {});
    hazards("fn f(a: int, b: int) -> bool { return b != 0 && a / b > 1; }", {});  // short circuit
    hazards("fn f(a: int, b: int) -> bool { return b == 0 || a / b > 1; }", {});
    // A check of something else does not help.
    hazards("fn f(a: int, b: int) -> int {\n if (a != 0) { return a / b; }\n return 0;\n}", {"2:divide-by-zero"});
    // Nor does a check that is undone before the division.
    hazards("fn f(a: int, b: int) -> int {\n if (b != 0) {\n  b = b - 1;\n  return a / b;\n }\n return 0;\n}",
            {"3:underflow", "4:divide-by-zero"});
}

TEST_CASE(one_division_is_reported_once) {
    // After "a / b" ran, b was not zero, so the second division is fine.
    hazards("fn f(a: int, b: int) -> int {\n let x: int = a / b;\n return x + a / b;\n}", {"2:divide-by-zero", "3:overflow"});
}

// ---------------------------------------------------------------- overflow and underflow (CWE-190, CWE-191)

TEST_CASE(arithmetic_that_can_leave_the_int_range) {
    hazards("fn f(input: int) -> int {\n return input + 1000;\n}", {"2:overflow"});
    hazards("fn f(input: int) -> int {\n return input - 1000;\n}", {"2:underflow"});
    hazards("fn f(input: int) -> int {\n return input * 2;\n}", {"2:overflow"});
    hazards("fn f(input: int) -> int {\n return -input;\n}", {"2:overflow"});
    hazards("fn f() -> int { let a: int = 2147483647; return a + 1; }", {"1:overflow"});
    hazards("fn f() -> int { let a: int = 10; let b: int = 20; return a + b * 3; }", {});
    Compilation c = compile("fn f(input: int) -> int { return input + 1000; }");
    const Diagnostic* d = test::find_kind(c, "overflow");
    CHECK(d && d->severity == "warning" && d->cwe == "CWE-190");
    CHECK(d && d->guard == "input > 2147483647 - 1000");
    CHECK(d && contains(d->proof, "input is a parameter, so it can be any int"));
    CHECK(contains(message_of("fn f() -> int { let a: int = 2147483647; return a + 1; }", "overflow"), "always overflows"));
}

TEST_CASE(a_range_check_removes_the_overflow_warning) {
    hazards("fn f(input: int) -> int { if (input > 2147483647 - 1000) { return 0; } return input + 1000; }", {});
    hazards("fn f(input: int) -> int { if (input < 1000000 && input > -1000000) { return input * 100; } return 0; }", {});
    hazards("fn f(input: int) -> int { if (input < -2147483647 - 1 + 1000) { return 0; } return input - 1000; }", {});
}

TEST_CASE(two_unknown_values_give_a_note_not_a_warning) {
    Compilation c = compile("fn f(a: int, b: int) -> int { return a + b; }");
    const Diagnostic* d = test::find_kind(c, "overflow");
    CHECK(d && d->severity == "info");
    CHECK_EQ(c.count("warning"), 0);
}

TEST_CASE(loop_counters_do_not_raise_overflow_warnings) {
    hazards("fn f(n: int) -> int { let count: int = 0; for i in 0..n { count = count + 1; } return count; }", {});
    hazards("fn f(n: int) -> int { let i: int = 0; while (i < n) { i = i + 1; } return i; }", {});
    hazards("fn f(a: int[], n: int) -> int { let s: int = 0; for i in 0..n { s = s + i; } return s; }", {});
    hazards("fn f() -> int { let k: int = 10; while (k > 0) { k = k - 1; } return 100 / (k + 1); }", {});
}

TEST_CASE(a_statement_gets_one_overflow_report) {
    hazards("fn f(a: int, b: int, c: int) -> int {\n return a + b + c + 1;\n}", {"2:overflow"});
}

// ---------------------------------------------------------------- null arrays (CWE-476)

TEST_CASE(null_reaches_a_parameter_only_if_a_caller_passes_it) {
    hazards("fn g(a: int[]) -> int {\n return a[0];\n}\nfn main() { let x: int[] = new int[2]; print(g(x)); }", {});
    hazards("fn g(a: int[]) -> int {\n return a[0];\n}\nfn main() { print(g(null)); }", {"2:null-access"});
    // One caller passes an array, another may pass null: "can be null".
    std::string two_callers = "fn g(a: int[]) -> int {\n return a[0];\n}\n"
                              "fn main() { let x: int[] = new int[2]; print(g(x)); let y: int[] = null; print(g(y)); }";
    hazards(two_callers, {"2:null-access"});
    CHECK(contains(message_of(two_callers, "null-access"), "'a' can be null here"));
    CHECK(contains(message_of("fn g(a: int[]) -> int { return a[0]; } fn main() { print(g(null)); }", "null-access"),
                   "'a' is null here"));
}

TEST_CASE(null_checks_and_local_nulls) {
    hazards("fn g(a: int[]) -> int { if (a == null) { return 0; } return a[0]; }\nfn main() { print(g(null)); }", {});
    hazards("fn g(a: int[]) -> int { if (a != null) { return len(a); } return 0; }\nfn main() { print(g(null)); }", {});
    hazards("fn f() -> int {\n let a: int[] = null;\n return a[0];\n}", {"3:null-access"});
    hazards("fn f(c: bool) -> int {\n let a: int[] = null;\n if (c) { a = new int[1]; }\n return len(a);\n}", {"4:null-access"});
    hazards("fn f() -> int { let a: int[] = null; a = new int[3]; return a[0]; }", {});
    // After the first access went through, the array was not null.
    hazards("fn g(a: int[]) -> int {\n a[0] = 1;\n return a[0];\n}\nfn main() { let y: int[] = null; print(g(y)); print(g(new int[1])); }",
            {"2:null-access"});
}

TEST_CASE(a_function_that_may_return_null) {
    hazards("fn make(n: int) -> int[] { if (n > 0) { return new int[n]; } return null; }\n"
            "fn main() {\n let a: int[] = make(3);\n print(a[0]);\n}",
            {"4:null-access"});
    hazards("fn make(n: int) -> int[] { return new int[3]; }\nfn main() { let a: int[] = make(3); print(a[0]); }", {});
}

// ---------------------------------------------------------------- array bounds (CWE-125, CWE-787)

TEST_CASE(index_checked_against_a_known_length) {
    hazards("fn f() {\n let a: int[] = new int[3];\n a[3] = 1;\n}", {"3:index-out-of-bounds"});
    hazards("fn f() -> int {\n let a: int[] = new int[3];\n return a[-1];\n}", {"3:index-out-of-bounds"});
    hazards("fn f() { let a: int[] = new int[3]; a[0] = 1; a[2] = 3; }", {});
    hazards("fn f() {\n let a: int[] = new int[3];\n for i in 0..5 {\n  a[i] = i;\n }\n}", {"4:index-out-of-bounds"});
    hazards("fn f() { let a: int[] = new int[3]; for i in 0..3 { a[i] = i; } }", {});
    hazards("fn f() { let a: int[] = new int[3]; for i in 0..len(a) { a[i] = i; } }", {});
    CHECK(contains(message_of("fn f() { let a: int[] = new int[3]; for i in 0..5 { a[i] = i; } }", "index-out-of-bounds"),
                   "can reach 3, past the end of the array"));
    CHECK_EQ(test::find_kind(compile("fn f() { let a: int[] = new int[3]; a[3] = 1; }"), "index-out-of-bounds")->cwe, "CWE-787");
}

TEST_CASE(an_index_that_can_be_negative) {
    hazards("fn f(a: int[], n: int) -> int {\n if (n > 100) { return 0; }\n return a[n];\n}", {"3:index-out-of-bounds"});
    hazards("fn f(a: int[], n: int) -> int { if (n < 0) { return 0; } return a[n]; }", {});
    // The length of a parameter is unknown, so only the negative side can be judged.
    hazards("fn f(a: int[], n: int) { for i in 0..n { print(a[i]); } }", {});
}

TEST_CASE(a_check_on_an_offset_says_something_about_the_variable) {
    // "n - 1 < 0" is a fact about n, not only about the temporary holding n - 1.
    hazards("fn f(a: int[], n: int) -> int { if (n - 1 < 0) { return 0; } return a[n - 1]; }", {"1:underflow"});
    hazards("fn f(a: int[], i: int) -> int { if (i + 1 > 0 && i < 100) { return a[i + 1]; } return 0; }", {"1:overflow"});
    hazards("fn f(n: int) -> int { if (n + 5 <= 10 && n + 5 >= 0) { return 100 / (n + 6); } return 0; }", {"1:overflow"});
}

TEST_CASE(array_sizes) {
    hazards("fn f() {\n let a: int[] = new int[0 - 1];\n print(len(a));\n}", {"2:negative-size"});
    hazards("fn f(n: int) {\n let a: int[] = new int[n];\n print(len(a));\n}", {"2:negative-size"});
    hazards("fn f(n: int) { if (n < 0) { return; } let a: int[] = new int[n]; print(len(a)); }", {});
}

// ---------------------------------------------------------------- findings as facts

TEST_CASE(findings_carry_what_the_agent_needs) {
    Compilation c = compile("fn mean(sum: int, n: int) -> int {\n    return sum / n;\n}");
    CHECK_EQ(c.findings.size(), size_t{1});
    const Diagnostic& d = c.findings.at(0);
    CHECK_EQ(d.id, "F1");
    CHECK_EQ(d.kind, "divide-by-zero");
    CHECK_EQ(d.function, "mean");
    CHECK_EQ(d.subject, "sum / n");
    CHECK_EQ(d.guard, "n == 0");
    CHECK_EQ(d.cwe, "CWE-369");
    CHECK_EQ(d.line, 2);
    CHECK_EQ(d.col, 16);  // the '/'
    CHECK_EQ(d.stmt_line, 2);
    CHECK(contains(d.proof, "n is a parameter, so it can be any int"));
    std::string json = findings_json(c, "t.parc");
    CHECK(contains(json, "\"id\": \"F1\""));
    CHECK(contains(json, "\"guard\": \"n == 0\""));
    CHECK(contains(json, "\"stage\": \"complete\""));
}

TEST_CASE(the_examples_trigger_the_warnings_their_comments_promise) {
    auto of = [](const std::string& file) {
        return test::findings(compile(test::read_file(std::filesystem::path(PARC_EXAMPLES_DIR) / file)), kHazards);
    };
    CHECK(of("01_clean.parc").empty());
    CHECK(of("02_cwe190_overflow.parc") == Lines{"3:overflow"});
    CHECK(of("03_cwe191_underflow.parc") == Lines{"3:underflow"});
    CHECK(of("04_cwe369_divzero.parc") == Lines{"3:divide-by-zero"});
    CHECK(of("05_cwe476_null.parc") == Lines{"4:null-access"});
    CHECK(of("06_dot_parallel.parc") == (Lines{"5:overflow", "7:divide-by-zero"}));
}

// The vulnerability percentage is at_risk / checked, so these counts must be right.

TEST_CASE(one_unguarded_division_is_one_of_one_operations_at_risk) {
    Compilation c = compile("fn mean(sum: int, n: int) -> int {\n    return sum / n;\n}\nfn main() {\n    print(mean(10, 2));\n}\n");
    sem::OpCount all = c.checked_total();
    CHECK_EQ(all.checked, 1);
    CHECK_EQ(all.at_risk, 1);
    CHECK_EQ(all.possible, 0);
    CHECK_EQ(c.checked.size(), size_t{2});  // one entry per function, also for main with nothing to check
    CHECK_EQ(c.checked[0].function, std::string("mean"));
    CHECK_EQ(c.checked[0].division.at_risk, 1);
}

TEST_CASE(a_guarded_division_is_checked_and_safe) {
    Compilation c = compile("fn mean(sum: int, n: int) -> int {\n    if (n == 0) {\n        return 0;\n    }\n    return sum / n;\n}\n"
                            "fn main() {\n    print(mean(10, 2));\n}\n");
    sem::OpCount all = c.checked_total();
    CHECK_EQ(all.checked, 1);
    CHECK_EQ(all.at_risk, 0);
}

TEST_CASE(each_kind_of_operation_is_counted_once_and_the_loop_counter_is_not) {
    // new int[4]: 1 allocation. a[i] = i * 2: 1 access and 1 multiplication.
    // len(a): 1 access. The loop's own i = i + 1 is the compiler's, not the programmer's.
    Compilation c = compile("fn main() {\n    let a: int[] = new int[4];\n    for i in 0..len(a) {\n        a[i] = i * 2;\n    }\n"
                            "    print(a[3]);\n}\n");
    const sem::CheckedOps& fn = c.checked[0];
    CHECK_EQ(fn.allocation.checked, 1);
    CHECK_EQ(fn.arithmetic.checked, 1);
    CHECK_EQ(fn.access.checked, 3);
    CHECK_EQ(fn.division.checked, 0);
    CHECK_EQ(fn.total().at_risk, 0);
}

TEST_CASE(a_note_counts_as_possible_and_not_as_at_risk) {
    // Both operands can be any int, so the overflow is only a note.
    Compilation c = compile("fn mul(x: int, y: int) -> int {\n    return x * y;\n}\nfn main() {\n    print(mul(2, 3));\n}\n");
    sem::OpCount all = c.checked_total();
    CHECK_EQ(all.checked, 1);
    CHECK_EQ(all.at_risk, 0);
    CHECK_EQ(all.possible, 1);
}

TEST_CASE(findings_json_carries_the_counts) {
    Compilation c = compile("fn mean(sum: int, n: int) -> int {\n    return sum / n;\n}\nfn main() {\n    print(mean(10, 2));\n}\n");
    std::string json = findings_json(c, "mean.parc");
    CHECK(contains(json, "\"checked\": {\"operations\": 1, \"at_risk\": 1, \"possible\": 0"));
    CHECK(contains(json, "\"division\": {\"checked\": 1, \"at_risk\": 1, \"possible\": 0}"));
    // A program that stops at a type error never reaches the value analysis.
    CHECK(!contains(findings_json(compile("fn main() {\n    let x: int = true;\n}\n"), "bad.parc"), "\"checked\""));
}

int main() { return test::run_all(); }
