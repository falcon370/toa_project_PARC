// Tests that run programs on the VM. They also check the whole compiler end to
// end: every program must print the same thing with and without the optimizer,
// with many or few registers, and on one thread or several.

#include <source_location>
#include <string>
#include <vector>

#include "common/test_framework.hpp"
#include "common/test_support.hpp"

using namespace parc;
using test::contains;

namespace {

// Runs the body as the content of main and returns what it prints.
std::string main_prints(const std::string& body) { return test::output("fn main() { " + body + " }"); }

void expect_output(const std::string& source, const std::string& want,
                   std::source_location loc = std::source_location::current()) {
    // The same program in every configuration.
    for (bool optimize : {true, false}) {
        for (int registers : {12, 5, 4}) {
            for (int threads : {1, 4}) {
                CompileOptions compile_options;
                compile_options.optimize = optimize;
                compile_options.registers = registers;
                vm::Options run_options;
                run_options.threads = threads;
                std::string got = test::output(source, compile_options, run_options);
                if (got == want) continue;
                ++test::failures();
                std::cerr << loc.file_name() << ":" << loc.line() << ": " << (optimize ? "optimized" : "not optimized") << ", "
                          << registers << " registers, " << threads << " threads\n" << source << "\n    want: " << want
                          << "    got:  " << got << "\n";
                return;
            }
        }
    }
}

vm::Result run_with(const std::string& source, int threads, std::uint32_t seed, bool force_parallel = false) {
    CompileOptions compile_options;
    compile_options.force_parallel = force_parallel;
    vm::Options run_options;
    run_options.threads = threads;
    run_options.seed = seed;
    return test::run(source, compile_options, run_options);
}

}  // namespace

// ---------------------------------------------------------------- values and operators

TEST_CASE(int_arithmetic) {
    CHECK_EQ(main_prints("print(7 + 3, 7 - 10, 6 * 7, 7 / 2, 7 % 3);"), "10 -3 42 3 1\n");
    // Division rounds toward zero and the remainder takes the sign of the left side, as in C.
    CHECK_EQ(main_prints("let a: int = -7; print(a / 2, a % 3, 7 / -2, 7 % -3);"), "-3 -1 -3 1\n");
    CHECK_EQ(main_prints("let x: int = 5; print(-x, - -x, 2 + 3 * 4, (2 + 3) * 4);"), "-5 5 14 20\n");
}

TEST_CASE(float_arithmetic_and_promotion) {
    CHECK_EQ(main_prints("print(1.5 + 2, 7 / 2.0, 1.0 / 4, 10.0 / 4.0 * 2);"), "3.5 3.5 0.25 5\n");
    CHECK_EQ(main_prints("let x: float = 3; let n: int = 2; print(x * n, x / n, -x);"), "6 1.5 -3\n");
    CHECK_EQ(main_prints("print(1.0 / 0.0 > 1000000.0);"), "true\n");  // infinity, not an error
}

TEST_CASE(comparisons_and_logic) {
    CHECK_EQ(main_prints("print(1 < 2, 2 <= 2, 3 > 4, 4 >= 5, 5 == 5, 5 != 5);"), "true true false false true false\n");
    CHECK_EQ(main_prints("print(1.5 < 2, 2 == 2.0, true == false, !true);"), "true true false false\n");
    CHECK_EQ(main_prints("let t: bool = true; let f: bool = false; print(t && f, t || f, !(t && f), t && !f);"),
             "false true true true\n");
}

TEST_CASE(short_circuit_skips_the_right_side) {
    // 1 / z would stop the program if it were evaluated.
    expect_output("fn main() { let z: int = 0; print(z != 0 && 1 / z > 0, z == 0 || 1 / z > 0); }", "false true\n");
    expect_output("fn noisy(v: bool) -> bool { print(\"called\"); return v; } fn main() { print(false && noisy(true)); print(true || noisy(true)); print(true && noisy(false)); }",
                  "false\ntrue\ncalled\nfalse\n");
}

TEST_CASE(print_formats) {
    CHECK_EQ(main_prints("print(\"a\", 1, 2.5, true, \"b\"); print(); print(\"tab\\tquote\\\"\");"), "a 1 2.5 true b\n\ntab\tquote\"\n");
    CHECK_EQ(main_prints("print(100000.0 * 100000.0, 0.1 + 0.2, 1.0 / 3.0);"), "1e+10 0.3 0.333333\n");
}

// ---------------------------------------------------------------- control flow

TEST_CASE(if_else_chains) {
    expect_output("fn sign(n: int) -> int { if (n > 0) { return 1; } else if (n < 0) { return -1; } else { return 0; } }"
                  " fn main() { print(sign(5), sign(-5), sign(0)); }",
                  "1 -1 0\n");
}

TEST_CASE(loops_with_break_and_continue) {
    expect_output("fn main() { let s: int = 0; for i in 0..10 { if (i % 2 == 0) { continue; } if (i > 7) { break; } s = s + i; } print(s); }", "16\n");
    expect_output("fn main() { let n: int = 27; let steps: int = 0; while (n != 1) { if (n % 2 == 0) { n = n / 2; } else { n = 3 * n + 1; } steps = steps + 1; } print(steps); }",
                  "111\n");
    expect_output("fn main() { for i in 5..5 { print(\"never\"); } for i in 3..1 { print(\"never\"); } print(\"done\"); }", "done\n");
    expect_output("fn main() { let c: int = 0; for i in 0..4 { for j in 0..4 { if (j > i) { break; } c = c + 1; } } print(c); }", "10\n");
}

TEST_CASE(a_loop_range_is_evaluated_once) {
    expect_output("fn main() { let n: int = 3; let c: int = 0; for i in 0..n { n = n + 1; c = c + 1; } print(c, n); }", "3 6\n");
}

TEST_CASE(functions_and_recursion) {
    expect_output("fn fact(n: int) -> int { if (n <= 1) { return 1; } return n * fact(n - 1); } fn main() { print(fact(1), fact(5), fact(10)); }",
                  "1 120 3628800\n");
    expect_output("fn fib(n: int) -> int { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); } fn main() { print(fib(15)); }", "610\n");
    expect_output("fn even(n: int) -> bool { if (n == 0) { return true; } return odd(n - 1); } fn odd(n: int) -> bool { if (n == 0) { return false; } return even(n - 1); }"
                  " fn main() { print(even(10), odd(7), even(3)); }",
                  "true true false\n");
    expect_output("fn add(a: int, b: int, c: int, d: int, e: int, f: int) -> int { return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6; } fn main() { print(add(1, 2, 3, 4, 5, 6)); }",
                  "91\n");
    expect_output("fn half(x: float) -> float { return x / 2; } fn main() { print(half(3), half(half(10))); }", "1.5 2.5\n");
}

TEST_CASE(scalars_are_passed_by_value_and_arrays_by_reference) {
    expect_output("fn change(n: int, a: int[]) { n = 99; a[0] = 99; } fn main() { let n: int = 1; let a: int[] = new int[2]; change(n, a); print(n, a[0]); }",
                  "1 99\n");
}

TEST_CASE(scopes_and_shadowing) {
    expect_output("fn main() { let x: int = 1; { let x: int = 2; x = x + 10; print(x); } print(x); }", "12\n1\n");
    expect_output("fn main() { let x: int = 5; let i: int = 0; while (i < 2) { let x: int = i * 10; print(x); i = i + 1; } print(x); }", "0\n10\n5\n");
}

TEST_CASE(arrays) {
    expect_output("fn main() { let a: int[] = new int[5]; for i in 0..len(a) { a[i] = i * i; } let s: int = 0; for i in 0..len(a) { s = s + a[i]; } print(s, len(a), a[4]); }",
                  "30 5 16\n");
    expect_output("fn main() { let f: float[] = new float[2]; let b: bool[] = new bool[2]; f[1] = 2.5; b[0] = true; print(f[0], f[1], b[0], b[1]); }",
                  "0 2.5 true false\n");
    expect_output("fn main() { let a: int[] = null; print(a == null); a = new int[0]; print(a != null, len(a)); let b: int[] = a; print(a == b); }",
                  "true\ntrue 0\ntrue\n");
    expect_output("fn make(n: int) -> int[] { let a: int[] = new int[n]; a[0] = n; return a; } fn main() { print(make(3)[0], len(make(7))); }", "3 7\n");
}

TEST_CASE(a_variable_without_a_value_starts_at_zero) {
    expect_output("fn main() { let x: int; let y: float; let ok: bool; let a: int[]; print(x, y, ok, a == null); }", "0 0 false true\n");
}

// ---------------------------------------------------------------- errors at run time

TEST_CASE(runtime_errors_stop_the_program_with_the_line) {
    vm::Result r = test::run("fn main() {\n print(\"before\");\n let z: int = 0;\n print(10 / z);\n print(\"after\");\n}");
    CHECK(!r.ok);
    CHECK_EQ(r.error, "division by zero");
    CHECK_EQ(r.error_line, 4);
    CHECK_EQ(r.error_function, "main");
    CHECK_EQ(r.output, "before\n");  // what ran before the error is kept

    CHECK_EQ(test::run("fn main() { let z: int = 0; print(10 % z); }").error, "remainder of a division by zero");
    CHECK_EQ(test::run("fn f(a: int[]) -> int { return a[0]; } fn main() { print(f(null)); }").error, "the array is null");
    CHECK_EQ(test::run("fn f(a: int[]) -> int { return len(a); } fn main() { print(f(null)); }").error, "the array is null");
    CHECK_EQ(test::run("fn main() { let a: int[] = new int[3]; let i: int = 3; a[i] = 1; }").error,
             "index 3 is outside the array (it has 3 elements)");
    CHECK_EQ(test::run("fn main() { let a: int[] = new int[3]; let i: int = -1; print(a[i]); }").error,
             "index -1 is outside the array (it has 3 elements)");
    CHECK_EQ(test::run("fn main() { let n: int = -2; let a: int[] = new int[n]; print(len(a)); }").error,
             "the array size is negative (-2)");
}

TEST_CASE(endless_loops_and_endless_recursion_are_stopped) {
    vm::Options quick;
    quick.max_steps = 10000;
    vm::Result loop = test::run("fn main() { let i: int = 0; while (true) { i = i + 1; } }", {}, quick);
    CHECK(!loop.ok && contains(loop.error, "ran for more than 10000 steps"));
    vm::Result deep = test::run("fn down(n: int) -> int { return down(n + 1); } fn main() { print(down(0)); }");
    CHECK(!deep.ok && contains(deep.error, "endless recursion"));
    vm::Result big = test::run("fn main() { let n: int = 2000000000; let a: int[] = new int[n]; print(len(a)); }");
    CHECK(!big.ok && contains(big.error, "more memory"));
}

TEST_CASE(overflow_wraps_and_is_noted) {
    // Without the optimizer, which would do this addition at compile time.
    CompileOptions plain;
    plain.optimize = false;
    vm::Result r = test::run("fn main() {\n let x: int = 2147483647;\n let y: int = x + 1;\n print(y);\n}", plain);
    CHECK(r.ok);
    CHECK_EQ(r.output, "-2147483648\n");
    CHECK_EQ(r.overflows, 1LL);
    CHECK_EQ(r.events.size(), size_t{1});
    if (!r.events.empty()) {
        CHECK_EQ(r.events[0].kind, "overflow");
        CHECK_EQ(r.events[0].line, 3);
        CHECK(contains(r.events[0].text, "wrapped around to -2147483648"));
    }
    CHECK_EQ(test::run("fn main() { print(1 + 2); }").overflows, 0LL);
    // The classic one: -2147483648 / -1.
    vm::Result div = test::run("fn main() { let m: int = -2147483647 - 1; let d: int = -1; print(m / d, m % d); }", plain);
    CHECK_EQ(div.output, "-2147483648 0\n");
    CHECK_EQ(div.overflows, 1LL);
}

TEST_CASE(a_program_needs_main) {
    vm::Result r = test::run("fn helper() { }");
    CHECK(!r.ok && contains(r.error, "has no 'fn main()'"));
}

// ---------------------------------------------------------------- parallel loops

TEST_CASE(a_reduction_gives_the_serial_result_on_any_number_of_threads) {
    std::string source = "fn main() { let a: int[] = new int[100]; for i in 0..100 { a[i] = i * 3 - 7; }"
                         " let s: int = 1000; parallel for i in 0..100 reduce(+: s) { s = s + a[i]; } print(s); }";
    for (int threads : {1, 2, 3, 4, 8, 64}) {
        for (std::uint32_t seed : {1u, 2u, 99u}) {
            vm::Result r = run_with(source, threads, seed);
            CHECK_EQ(r.output, "15150\n");
            CHECK_EQ(r.races, 0LL);
        }
    }
}

TEST_CASE(product_and_float_reductions) {
    expect_output("fn main() { let p: int = 2; parallel for i in 1..8 reduce(*: p) { p = p * i; } print(p); }", "10080\n");
    // Powers of two add exactly, so even the float result is the same on every thread count.
    expect_output("fn main() { let s: float = 0.5; parallel for i in 0..16 reduce(+: s) { s = s + 0.25; } print(s); }", "4.5\n");
    expect_output("fn main() { let a: int = 0; let b: int = 0; parallel for i in 0..10 reduce(+: a, b) { a = a + i; b = b + 1; } print(a, b); }", "45 10\n");
}

TEST_CASE(parallel_writes_to_separate_elements) {
    expect_output("fn main() { let a: int[] = new int[20]; let k: int = 3; parallel for i in 0..20 { let t: int = i * k; a[i] = t + 1; }"
                  " let s: int = 0; for i in 0..20 { s = s + a[i]; } print(s, a[0], a[19]); }",
                  "590 1 58\n");
}

TEST_CASE(parallel_loop_edge_cases) {
    expect_output("fn main() { let s: int = 7; parallel for i in 5..5 reduce(+: s) { s = s + 100; } print(s); }", "7\n");  // no iterations
    expect_output("fn main() { let s: int = 0; parallel for i in 0..1 reduce(+: s) { s = s + 5; } print(s); }", "5\n");
    expect_output("fn main() { let s: int = 0; parallel for i in 0..3 reduce(+: s) { if (i == 1) { continue; } s = s + i; } print(s); }", "2\n");
    // A parallel loop inside a serial loop, run many times.
    expect_output("fn main() { let total: int = 0; for r in 0..50 { let s: int = 0; parallel for i in 0..10 reduce(+: s) { s = s + i; } total = total + s; } print(total); }",
                  "2250\n");
}

TEST_CASE(calls_and_nested_loops_inside_a_parallel_loop) {
    expect_output("fn square(x: int) -> int { return x * x; } fn main() { let s: int = 0; parallel for i in 0..10 reduce(+: s) { s = s + square(i); } print(s); }", "285\n");
    expect_output("fn main() { let a: int[] = new int[6]; parallel for i in 0..6 { let s: int = 0; for j in 0..i { s = s + j; } a[i] = s; } print(a[0], a[3], a[5]); }",
                  "0 3 10\n");
    // A parallel loop reached from inside another parallel loop runs on the outer loop's thread.
    expect_output("fn row(n: int) -> int { let s: int = 0; parallel for j in 0..n reduce(+: s) { s = s + j; } return s; }"
                  " fn main() { let t: int = 0; parallel for i in 0..5 reduce(+: t) { t = t + row(i); } print(t); }",
                  "10\n");
}

TEST_CASE(the_scheduler_is_deterministic) {
    std::string source = "fn main() { let a: int[] = new int[12]; parallel for i in 0..12 { a[i] = i; print(i); } }";
    vm::Result first = run_with(source, 4, 7);
    vm::Result again = run_with(source, 4, 7);
    CHECK(first.ok);
    CHECK_EQ(first.output, again.output);  // same seed, same interleaving
    CHECK(first.schedule == again.schedule);
    CHECK(!first.schedule.empty());
    CHECK_EQ(first.parallel_loops, 1);
    // Every iteration ran exactly once, whatever the order.
    for (int i = 0; i < 12; ++i) CHECK_EQ(test::count("\n" + first.output, "\n" + std::to_string(i) + "\n"), 1);
    // Some other seed gives another order.
    bool different = false;
    for (std::uint32_t seed = 8; seed < 20 && !different; ++seed) different = run_with(source, 4, seed).output != first.output;
    CHECK(different);
    // On one thread the order is the serial order.
    CHECK_EQ(run_with(source, 1, 7).output, "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n");
}

TEST_CASE(an_unsafe_parallel_loop_runs_serially_and_gives_the_right_answer) {
    std::string source = "fn main() { let a: int[] = new int[16]; a[0] = 1; parallel for i in 1..16 { a[i] = a[i - 1] + 1; } print(a[15]); }";
    for (int threads : {1, 4, 8}) {
        vm::Result r = run_with(source, threads, 1);
        CHECK_EQ(r.output, "16\n");
        CHECK_EQ(r.parallel_loops, 0);  // it was turned into a normal loop
        CHECK_EQ(r.races, 0LL);
    }
}

TEST_CASE(forcing_it_parallel_shows_the_data_race) {
    std::string source = "fn main() { let a: int[] = new int[16]; a[0] = 1; parallel for i in 1..16 { a[i] = a[i - 1] + 1; } print(a[15]); }";
    vm::Result r = run_with(source, 4, 1, true);
    CHECK(r.ok);
    CHECK(r.races > 0);
    CHECK_EQ(r.parallel_loops, 1);
    bool has_race_event = false;
    for (const auto& e : r.events) has_race_event = has_race_event || (e.kind == "race" && contains(e.text, "data race on element"));
    CHECK(has_race_event);
    // On one thread there is nobody to race with.
    CHECK_EQ(run_with(source, 1, 1, true).races, 0LL);
}

// ---------------------------------------------------------------- calling one function, and the debugger trace

TEST_CASE(run_function_calls_one_function_with_arguments) {
    Compilation c = compile("fn scale(a: int[], k: int) -> int { let s: int = 0; for i in 0..len(a) { a[i] = a[i] * k; s = s + a[i]; } return s; }"
                            " fn ratio(x: float, ok: bool) -> float { if (ok) { return x / 4; } return x; }"
                            " fn pick(a: int[]) -> int[] { return a; }");
    vm::Arg array;
    array.is_array = true;
    array.items = {vm::Value::of_int(1), vm::Value::of_int(2), vm::Value::of_int(3)};
    vm::Arg k;
    k.value = vm::Value::of_int(10);
    vm::Result r = vm::run_function(c.code, c.code.find("scale"), {array, k});
    CHECK(r.ok);
    CHECK_EQ(r.returned, "60");
    CHECK_EQ(r.arrays.at(0), "[10, 20, 30]");  // the caller sees the changed array

    vm::Arg x, ok;
    x.value = vm::Value::of_float(1.0);
    ok.value = vm::Value::of_bool(true);
    CHECK_EQ(vm::run_function(c.code, c.code.find("ratio"), {x, ok}).returned, "0.25");

    vm::Arg null_array;
    null_array.is_array = true;
    null_array.is_null = true;
    CHECK_EQ(vm::run_function(c.code, c.code.find("pick"), {null_array}).returned, "null");
    vm::Result bad = vm::run_function(c.code, c.code.find("scale"), {null_array, k});
    CHECK(!bad.ok && bad.error == "the array is null");
}

TEST_CASE(the_trace_records_each_executed_instruction) {
    vm::Options options;
    options.trace_steps = 50;
    vm::Result r = test::run("fn main() {\n let a: int[] = new int[2];\n a[1] = 7;\n print(a[1] + 1);\n}", {}, options);
    CHECK(r.ok);
    CHECK(!r.trace.empty() && r.trace.size() <= 50);
    CHECK_EQ(static_cast<long long>(r.trace.size()), r.steps);
    bool made = false, stored = false, printed = false;
    for (const auto& step : r.trace) {
        CHECK_EQ(step.thread, 0);
        CHECK_EQ(step.depth >= 0, true);
        made = made || step.heap == "new #1 [2]";
        stored = stored || step.heap == "#1[1] = 7";
        printed = printed || step.output == "8\n";
    }
    CHECK(made && stored && printed);
    CHECK_EQ(r.trace.at(0).line, 2);
    // A limit shorter than the run keeps only the first steps.
    options.trace_steps = 3;
    CHECK_EQ(test::run("fn main() { let s: int = 0; for i in 0..100 { s = s + i; } print(s); }", {}, options).trace.size(), size_t{3});
    std::string json = vm::to_json(r, compile("fn main() { }").code);
    CHECK(contains(json, "\"heap\": \"#1[1] = 7\""));
    CHECK(contains(json, "\"output\": \"8\\n\""));
}

// ---------------------------------------------------------------- the examples

TEST_CASE(examples_print_the_same_in_every_configuration) {
    for (const auto& path : test::example_files()) {
        std::string source = test::read_file(path);
        vm::Result reference = test::run(source);
        std::string want = reference.output + (reference.ok ? "" : "error: " + reference.error);
        for (bool optimize : {true, false}) {
            for (int registers : {12, 4}) {
                for (int threads : {1, 4}) {
                    CompileOptions compile_options;
                    compile_options.optimize = optimize;
                    compile_options.registers = registers;
                    vm::Options run_options;
                    run_options.threads = threads;
                    std::string got = test::output(source, compile_options, run_options);
                    if (got == want) continue;
                    ++test::failures();
                    std::cerr << "    " << path.filename().string() << " differs when " << (optimize ? "optimized" : "not optimized")
                              << " with " << registers << " registers and " << threads << " threads\n      want: " << want
                              << "      got:  " << got << "\n";
                }
            }
        }
    }
}

TEST_CASE(examples_match_their_expected_output_files) {
    // examples/expected/NAME.out holds what "parc run" prints, with a last
    // line "error: ..." if the program stops with a runtime error.
    int checked = 0;
    for (const auto& path : test::example_files()) {
        std::filesystem::path expected = path.parent_path() / "expected" / (path.stem().string() + ".out");
        if (!std::filesystem::exists(expected)) continue;
        ++checked;
        std::string want = test::read_file(expected);
        std::erase(want, '\r');
        std::string got = test::output(test::read_file(path));
        if (!got.empty() && got.back() != '\n') got += "\n";
        CHECK_EQ(got, want);
        if (got != want) std::cerr << "    " << path.filename().string() << "\n";
    }
    CHECK(checked > 0);
}

int main() { return test::run_all(); }
