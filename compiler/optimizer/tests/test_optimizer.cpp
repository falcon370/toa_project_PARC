// Tests for the data-flow framework, the loop dependence analysis and the optimizer passes.

#include <source_location>
#include <string>
#include <vector>

#include "common/test_framework.hpp"
#include "common/test_support.hpp"
#include "optimizer/analyses.hpp"
#include "optimizer/passes.hpp"

using namespace parc;
using test::contains;

namespace {

using Names = std::vector<std::string>;

struct Lowered {
    Compilation c;
    const ir::Function& fn() const { return c.ir.functions.at(0); }
    int block(const std::string& prefix) const {
        for (size_t b = 0; b < fn().blocks.size(); ++b)
            if (fn().blocks[b].label.starts_with(prefix)) return static_cast<int>(b);
        ++test::failures();
        std::cerr << "no block named " << prefix << "\n";
        return 0;
    }
    Names vars(const BitSet& set) const {
        Names out;
        for (int v : set.members()) out.push_back(fn().vars[static_cast<size_t>(v)].name);
        std::sort(out.begin(), out.end());
        return out;
    }
};

Lowered lower(const std::string& source) {
    Lowered l{compile(source)};
    if (!l.c.lowered) {
        ++test::failures();
        std::cerr << "does not compile: " << source << "\n";
    }
    return l;
}

// The optimized IR of function f.
std::string optimized(const std::string& source) {
    Compilation c = compile(source);
    if (!c.runnable) {
        ++test::failures();
        std::cerr << "does not compile: " << source << "\n";
        return "";
    }
    std::string problem = opt::verify(c.optimized);
    if (!problem.empty()) {
        ++test::failures();
        std::cerr << "optimizer broke the IR: " << problem << "\n";
    }
    return test::ir_of(c.optimized, "f");
}

// Every change and missed note of the passes with this name.
std::string log_of(const Compilation& c, const std::string& pass) {
    std::string out;
    for (const auto& run : c.passes.runs) {
        if (run.pass != pass) continue;
        for (const auto& n : run.changes) out += n.text + "\n";
        for (const auto& n : run.missed) out += "MISSED " + n.text + "\n";
    }
    return out;
}

// "line:kind" of the loop findings.
std::vector<std::string> loops(const std::string& source) {
    return test::findings(compile(source), {"parallelized", "parallel-dependence", "parallelizable", "loop-dependence"});
}

std::string loop_message(const std::string& source) {
    Compilation c = compile(source);
    for (const auto& d : c.findings)
        if (d.phase == "optimizer" && d.kind != "missed-optimization") return d.message;
    return "";
}

}  // namespace

// ---------------------------------------------------------------- data-flow framework

TEST_CASE(live_variables) {
    //  entry: x = a + b; y = a * 2; if x > 0      uses a, b
    //  then:  return y                            uses y
    //  end:   return x                            uses x
    Lowered l = lower("fn f(a: int, b: int) -> int { let x: int = a + b; let y: int = a * 2; if (x > 0) { return y; } return x; }");
    opt::Solution<BitSet> live = opt::liveness(l.fn());
    CHECK(l.vars(live.in.at(0)) == (Names{"a", "b"}));
    CHECK(l.vars(live.out.at(0)) == (Names{"x", "y"}));
    CHECK(l.vars(live.in.at(static_cast<size_t>(l.block("if.then")))) == Names{"y"});
    CHECK(l.vars(live.in.at(static_cast<size_t>(l.block("if.end")))) == Names{"x"});
    CHECK(l.vars(live.out.at(static_cast<size_t>(l.block("if.end")))).empty());
}

TEST_CASE(liveness_goes_around_a_loop) {
    Lowered l = lower("fn f(n: int) -> int { let s: int = 0; let i: int = 0; while (i < n) { s = s + i; i = i + 1; } return s; }");
    opt::Solution<BitSet> live = opt::liveness(l.fn());
    size_t head = static_cast<size_t>(l.block("while.head"));
    // s, i and n are all needed at the top of every pass.
    CHECK(l.vars(live.in.at(head)) == (Names{"i", "n", "s"}));
    CHECK(l.vars(live.out.at(static_cast<size_t>(l.block("while.body")))) == (Names{"i", "n", "s"}));
    CHECK(l.vars(live.in.at(static_cast<size_t>(l.block("while.end")))) == Names{"s"});
}

TEST_CASE(reaching_definitions) {
    //  x = 1; if c { x = 2 }; return x     both definitions of x reach the return
    Lowered l = lower("fn f(c: bool) -> int { let x: int = 1; if (c) { x = 2; } return x; }");
    opt::ReachingDefs reach = opt::reaching_definitions(l.fn());
    int end = l.block("if.end");
    int x = -1;
    for (size_t v = 0; v < l.fn().vars.size(); ++v)
        if (l.fn().vars[v].name == "x") x = static_cast<int>(v);
    CHECK_EQ(reach.reaching(l.fn(), end, 0, x).size(), size_t{2});
    // Inside the "then" block, after x = 2, only that definition reaches.
    int then_block = l.block("if.then");
    CHECK_EQ(reach.reaching(l.fn(), then_block, 1, x).size(), size_t{1});
    // The parameter's value on entry is a definition too.
    CHECK_EQ(reach.defs.at(0).block, -1);
    CHECK(reach.solution.in.at(0).test(0));
}

TEST_CASE(a_later_definition_kills_an_earlier_one) {
    Lowered l = lower("fn f() -> int { let x: int = 1; x = 2; x = 3; return x; }");
    opt::ReachingDefs reach = opt::reaching_definitions(l.fn());
    CHECK_EQ(reach.solution.out.at(0).members().size(), size_t{1});  // only x = 3 leaves the block
}

TEST_CASE(available_expressions) {
    // a * b is computed on both branches, so it is available at the join; a + b only on one.
    Lowered l = lower("fn f(a: int, b: int, c: bool) -> int { let x: int = 0; if (c) { x = a * b; x = a + b; } else { x = a * b; } return x; }");
    opt::AvailableExprs avail = opt::available_expressions(l.fn());
    const BitSet& at_join = avail.solution.in.at(static_cast<size_t>(l.block("if.end")));
    Names names;
    for (int e : at_join.members()) names.push_back(opt::expression_text(l.c.ir, l.fn(), avail.exprs[static_cast<size_t>(e)]));
    CHECK(names == Names{"a * b"});
}

TEST_CASE(writing_an_operand_ends_availability) {
    Lowered l = lower("fn f(a: int, b: int) -> int { let x: int = a * b; a = 5; return x; }");
    opt::AvailableExprs avail = opt::available_expressions(l.fn());
    CHECK(avail.solution.out.at(0).members().empty());
    // a + b and b + a are one expression.
    Lowered m = lower("fn f(a: int, b: int) -> int { let x: int = a + b; let y: int = b + a; return x + y; }");
    opt::AvailableExprs both = opt::available_expressions(m.fn());
    int sums = 0;
    for (const auto& e : both.exprs)
        if (opt::expression_text(m.c.ir, m.fn(), e) == "a + b") ++sums;
    CHECK_EQ(sums, 1);
}

TEST_CASE(the_solver_needs_few_visits) {
    Lowered l = lower("fn f(n: int) -> int { let s: int = 0; for i in 0..n { for j in 0..n { s = s + i * j; } } return s; }");
    opt::Solution<BitSet> live = opt::liveness(l.fn());
    // Visiting in the right order, a nested loop settles in a handful of visits per block.
    CHECK(live.visits <= 4 * static_cast<int>(l.fn().blocks.size()));
}

TEST_CASE(dataflow_json_lists_in_and_out_sets) {
    Compilation c = compile("fn f(a: int, b: int) -> int { let x: int = a + b; if (x > 0) { return a; } return x; }");
    std::string json = opt::dataflow_json(c.ir, "t.parc");
    CHECK(contains(json, "\"live_in\": [\n            \"a\",\n            \"b\"\n          ]"));
    CHECK(contains(json, "\"d1: a (parameter)\""));
    CHECK(contains(json, "\"a + b\""));
}

// ---------------------------------------------------------------- loop dependence

TEST_CASE(independent_iterations_run_in_parallel) {
    CHECK(loops("fn f(a: int[], n: int) {\n parallel for i in 0..n { a[i] = a[i] + 1; }\n}") == Names{"2:parallelized"});
    CHECK(loops("fn f(a: int[], n: int) {\n parallel for i in 0..n { a[2 * i] = a[2 * i + 1]; }\n}") == Names{"2:parallelized"});
    // GCD test: 2*i and 4*j + 1 are never equal (an even and an odd number).
    CHECK(loops("fn f(a: int[], n: int) {\n parallel for i in 0..n { a[2 * i] = a[4 * i + 1]; }\n}") == Names{"2:parallelized"});
    // ZIV test: a[0] and a[1] are different elements, but every iteration writes a[0].
    CHECK(loops("fn f(a: int[], n: int) {\n parallel for i in 0..n { a[0] = i; }\n}") == Names{"2:parallel-dependence"});
}

TEST_CASE(a_loop_carried_dependence_is_named) {
    std::string shifted = "fn f(a: int[], n: int) {\n parallel for i in 1..n { a[i] = a[i - 1] + 1; }\n}";
    CHECK(loops(shifted) == Names{"2:parallel-dependence"});
    CHECK(contains(loop_message(shifted), "'a[i]' is written, and 'a[i - 1]' is the element used 1 iteration away (distance 1)"));
    CHECK(contains(loop_message("fn f(a: int[], n: int) { parallel for i in 0..n { a[i] = a[i + 3]; } }"), "(distance 3)"));
    CHECK(contains(loop_message("fn f(a: int[], n: int) { parallel for i in 0..n { a[i] = a[2 * i]; } }"),
                   "the GCD test can't rule it out"));
    CHECK(contains(loop_message("fn f(a: int[], b: int[], n: int) { parallel for i in 0..n { a[b[i]] = i; } }"),
                   "is not of the form c*i + k"));
    Compilation c = compile(shifted);
    CHECK_EQ(test::find_kind(c, "parallel-dependence")->cwe, "CWE-362");
    CHECK(c.loops.parallel.empty());  // so it runs as a normal loop
}

TEST_CASE(a_distance_longer_than_the_loop_is_no_dependence) {
    CHECK(loops("fn f(a: int[]) {\n parallel for i in 0..4 { a[i] = a[i + 10]; }\n}") == Names{"2:parallelized"});
}

TEST_CASE(arrays_that_may_be_the_same_array) {
    // Two parameters can be one array if the caller passes it twice.
    std::string two = "fn f(a: int[], b: int[], n: int) {\n parallel for i in 1..n { a[i] = b[i - 1]; }\n}";
    CHECK(loops(two) == Names{"2:parallel-dependence"});
    CHECK(contains(loop_message(two), "a and b may be the same array"));
    // With the same subscript it does not matter whether they are the same.
    CHECK(loops("fn f(a: int[], b: int[], n: int) {\n parallel for i in 0..n { a[i] = b[i] * 2; }\n}") == Names{"2:parallelized"});
    // An array made with new inside the function is nobody else's.
    CHECK(loops("fn f(a: int[], n: int) {\n let c: int[] = new int[n];\n parallel for i in 1..n { c[i] = a[i - 1]; }\n}") ==
          Names{"3:parallelized"});
    // ...unless it is given another name.
    CHECK(loops("fn f(n: int) {\n let c: int[] = new int[n];\n let d: int[] = c;\n parallel for i in 1..n { c[i] = d[i - 1]; }\n}") ==
          Names{"4:parallel-dependence"});
}

TEST_CASE(shared_scalars_need_a_reduction) {
    std::string plain = "fn f(a: int[], n: int) -> int {\n let s: int = 0;\n parallel for i in 0..n { s = s + a[i]; }\n return s;\n}";
    CHECK(loops(plain) == Names{"3:parallel-dependence"});
    CHECK(contains(loop_message(plain), "it needs reduce(+: s)"));
    CHECK(loops("fn f(a: int[], n: int) -> int {\n let s: int = 0;\n parallel for i in 0..n reduce(+: s) { s = s + a[i]; }\n return s;\n}") ==
          Names{"3:parallelized"});
    CHECK(loops("fn f(a: int[], n: int) -> int {\n let s: int = 1;\n parallel for i in 0..n reduce(*: s) { s = a[i] * s; }\n return s;\n}") ==
          Names{"3:parallelized"});
    // The wrong operator, or another use of the variable, is not a reduction.
    CHECK(loops("fn f(a: int[], n: int) -> int {\n let s: int = 0;\n parallel for i in 0..n reduce(*: s) { s = s + a[i]; }\n return s;\n}") ==
          Names{"3:parallel-dependence"});
    CHECK(loops("fn f(a: int[], n: int) -> int {\n let s: int = 0;\n parallel for i in 0..n reduce(+: s) { s = s + a[i]; a[i] = s; }\n return s;\n}") ==
          Names{"3:parallel-dependence"});
    // A variable declared inside the loop belongs to one iteration.
    CHECK(loops("fn f(a: int[], n: int) {\n parallel for i in 0..n { let t: int = a[i] * 2; a[i] = t; }\n}") == Names{"2:parallelized"});
    CHECK(contains(loop_message("fn f(a: int[], n: int) -> int { let last: int = 0; parallel for i in 0..n { last = a[i]; } return last; }"),
                   "'last' is written inside the loop"));
}

TEST_CASE(calls_that_write_arrays_block_parallel_loops) {
    std::string source = "fn set(a: int[], i: int) { a[i] = 1; }\n"
                         "fn f(a: int[], n: int) {\n parallel for i in 0..n { set(a, i); }\n}";
    CHECK(loops(source) == Names{"3:parallel-dependence"});
    CHECK(contains(loop_message(source), "the call 'set(a, i)' may touch any element of 'a'"));
    // A call that only reads is fine when the loop does not write that array.
    CHECK(loops("fn get(a: int[], i: int) -> int { return a[i]; }\n"
                "fn f(a: int[], n: int) -> int {\n let s: int = 0;\n parallel for i in 0..n reduce(+: s) { s = s + get(a, i); }\n return s;\n}") ==
          Names{"4:parallelized"});
}

TEST_CASE(normal_loops_get_a_note_when_it_is_useful) {
    std::string sum = "fn f(a: int[], n: int) -> int {\n let s: int = 0;\n for i in 0..n { s = s + a[i]; }\n return s;\n}";
    CHECK(loops(sum) == Names{"3:parallelizable"});
    Compilation c = compile(sum);
    CHECK_EQ(test::find_kind(c, "parallelizable")->guard, "reduce(+: s)");
    CHECK_EQ(test::find_kind(c, "parallelizable")->hint, "write: parallel for i in 0..n reduce(+: s)");
    CHECK(loops("fn f(a: int[], n: int) {\n for i in 1..n { a[i] = a[i - 1] + 1; }\n}") == Names{"2:loop-dependence"});
    // Printing, or leaving early, means the order matters: no suggestion.
    CHECK(loops("fn f(a: int[], n: int) { for i in 0..n { a[i] = i; print(i); } }").empty());
    CHECK(loops("fn f(a: int[], n: int) { for i in 0..n { if (a[i] == 0) { break; } a[i] = 0; } }").empty());
    CHECK(loops("fn f(n: int) { for i in 0..n { print(i); } }").empty());
}

// ---------------------------------------------------------------- optimizer passes

TEST_CASE(constants_are_folded_and_propagated) {
    CHECK(contains(optimized("fn f() -> int { let x: int = 2 * 3 + 4; let y: int = x * x; return y - 1; }"), "return 99"));
    CHECK(contains(optimized("fn f() -> float { let x: float = 1.5; return x * 2 + 1; }"), "return 4.0"));
    CHECK(contains(optimized("fn f() -> bool { let x: int = 5; return x > 3 && x < 10; }"), "return true"));
    // A parameter is not a constant, and a variable with two different values is not either.
    CHECK(contains(optimized("fn f(n: int) -> int { return n + 1; }"), "n + 1"));
    CHECK(contains(optimized("fn f(c: bool) -> int { let x: int = 1; if (c) { x = 2; } return x * 10; }"), "x * 10"));
    // The same constant on both paths is still a constant.
    CHECK(contains(optimized("fn f(c: bool) -> int { let x: int = 1; if (c) { x = 1; } return x * 10; }"), "return 10"));
}

TEST_CASE(folding_wraps_like_the_vm_and_leaves_division_by_zero_alone) {
    CHECK(contains(optimized("fn f() -> int { let x: int = 2147483647; return x + 1; }"), "return -2147483648"));
    CHECK(contains(optimized("fn f() -> int { return -7 / 2 * 10 + -7 % 3; }"), "return -31"));
    CHECK(contains(optimized("fn f() -> int { let z: int = 0; return 5 / z; }"), "5 / 0"));  // must still fail at run time
}

TEST_CASE(algebraic_simplification) {
    std::string text = optimized("fn f(x: int) -> int { let a: int = x + 0; let b: int = a * 1; let c: int = b - b; let d: int = x * 0; return b + c + d; }");
    CHECK(contains(text, "return x"));
    CHECK(!contains(text, "*"));
}

TEST_CASE(a_branch_on_a_constant_disappears) {
    std::string text = optimized("fn f() -> int { let debug: bool = false; if (debug) { print(\"dbg\"); } return 1; }");
    CHECK(!contains(text, "print"));
    CHECK(!contains(text, "if "));
    CHECK_EQ(test::count(text, ":\n"), 1);  // one block left
}

TEST_CASE(copies_are_propagated) {
    Compilation c = compile("fn f(a: int) -> int { let b: int = a; let d: int = b; return d + b; }");
    CHECK(contains(test::ir_of(c.optimized, "f"), "= a + a"));
    CHECK(contains(log_of(c, "copy propagation"), "is a copy of a"));
}

TEST_CASE(a_copy_stops_being_one_when_either_side_changes) {
    // b must keep the old value of a.
    CHECK_EQ(test::output("fn f(a: int) -> int { let b: int = a; a = a + 1; return b * 10 + a; } fn main() { print(f(1)); }"), "12\n");
}

TEST_CASE(repeated_computations_in_a_block_are_reused) {
    Compilation c = compile("fn f(a: int, b: int) -> int { let x: int = a * b; let y: int = b * a; return x + y; }");
    CHECK_EQ(test::count(test::ir_of(c.optimized, "f"), " * "), 1);
    CHECK(contains(log_of(c, "local value numbering"), "already holds this value in the same block"));
    // Not when an operand changed in between.
    CHECK_EQ(test::count(optimized("fn f(a: int, b: int) -> int { let x: int = a * b; a = a + 1; let y: int = a * b; return x + y; }"), " * "), 2);
}

TEST_CASE(repeated_computations_across_blocks_are_reused) {
    Compilation c = compile("fn f(a: int, b: int, c: bool) -> int { let x: int = a * b; let r: int = 0; if (c) { r = 1; } let y: int = a * b; return x + y + r; }");
    CHECK_EQ(test::count(test::ir_of(c.optimized, "f"), "a * b"), 1);
    CHECK(contains(log_of(c, "common subexpression elimination"), "was already computed on every path"));
    // Only computed on one path: it has to be computed again.
    CHECK_EQ(test::count(optimized("fn f(a: int, b: int, c: bool) -> int { let x: int = 0; if (c) { x = a * b; } let y: int = a * b; return x + y; }"), "a * b"), 2);
}

TEST_CASE(dead_code_is_removed_but_not_what_can_fail) {
    std::string text = optimized("fn f(a: int, b: int) -> int { let t: int = a * b + 7; let u: int = t - a; return a; }");
    CHECK(!contains(text, "*"));
    CHECK(!contains(text, "t ="));
    // A division might stop the program, so it stays even if its result is unused.
    CHECK(contains(optimized("fn f(a: int, b: int) -> int { let t: int = a / b; return a; }"), "a / b"));
    CHECK(contains(optimized("fn f(a: int[]) -> int { let t: int = a[5]; return 1; }"), "a[5]"));
    // Calls are kept: they may print.
    CHECK(contains(optimized("fn g() -> int { print(1); return 2; } fn f() -> int { let t: int = g(); return 1; }"), "call g()"));
}

TEST_CASE(invariant_code_moves_out_of_the_loop) {
    Compilation c = compile("fn f(a: int[], n: int, k: int) { let i: int = 0; while (i < n) { a[i] = k * 7 + i; i = i + 1; } }");
    std::string text = test::ir_of(c.optimized, "f");
    CHECK(contains(log_of(c, "loop-invariant code motion"), "out of the loop"));
    // k * 7 is now before the loop's first block.
    CHECK(text.find("k * 7") < text.find("while.head"));
    CHECK_EQ(test::output("fn f(a: int[], n: int, k: int) { let i: int = 0; while (i < n) { a[i] = k * 7 + i; i = i + 1; } }"
                          " fn main() { let a: int[] = new int[3]; f(a, 3, 2); print(a[0], a[1], a[2]); }"),
             "14 15 16\n");
}

TEST_CASE(code_that_cannot_move_gets_a_reason) {
    // A division may fail, and the loop may not run at all: moving it could add a failure.
    Compilation c = compile("fn f(a: int[], n: int, k: int, d: int) { let i: int = 0; while (i < n) { a[i] = k / d; i = i + 1; } }");
    CHECK(contains(log_of(c, "loop-invariant code motion"), "MISSED 'k / d' stays inside the loop: it could divide by zero"));
    CHECK(test::ir_of(c.optimized, "f").find("k / d") > test::ir_of(c.optimized, "f").find("while.head"));
    CHECK(test::find_kind(c, "missed-optimization") != nullptr);
    // An operand that is written in the loop.
    Compilation d = compile("fn f(a: int[], n: int, k: int, m: int) { let i: int = 0; while (i < n) { a[i] = k * m; m = m + a[i]; i = i + 1; } }");
    CHECK(contains(log_of(d, "loop-invariant code motion"), "MISSED 'k * m' stays inside the loop: m is written inside the loop"));
}

TEST_CASE(moving_code_keeps_a_loop_that_runs_zero_times_correct) {
    std::string source = "fn f(n: int, k: int) -> int { let r: int = 5; let i: int = 0; while (i < n) { r = k * 3; i = i + 1; } return r; }"
                         " fn main() { print(f(0, 2), f(1, 2)); }";
    CHECK_EQ(test::output(source), "5 6\n");
}

TEST_CASE(multiplication_by_the_counter_becomes_addition) {
    Compilation c = compile("fn f(a: int[], n: int) { let i: int = 0; while (i < n) { a[i] = i * 12; i = i + 1; } }");
    std::string text = test::ir_of(c.optimized, "f");
    CHECK(contains(log_of(c, "strength reduction"), "so the loop adds where it used to multiply"));
    CHECK(!contains(text, "* 12"));
    CHECK(contains(text, "i.x12 = i.x12 + 12"));
    CHECK_EQ(test::output("fn main() { let a: int[] = new int[4]; let i: int = 1; while (i < 4) { a[i] = i * 12; i = i + 1; } print(a[0], a[1], a[2], a[3]); }"),
             "0 12 24 36\n");
    // In a parallel loop each thread starts at its own i, so the multiplication stays.
    Compilation p = compile("fn f(a: int[], n: int) { parallel for i in 0..n { a[i] = i * 12; } }");
    CHECK(contains(test::ir_of(p.optimized, "f"), "i * 12"));
    CHECK(contains(log_of(p, "strength reduction"), "MISSED"));
}

TEST_CASE(the_pass_log_explains_every_change) {
    Compilation c = compile("fn f() -> int { let x: int = 2 * 3; return x + 0; }");
    CHECK(!c.passes.runs.empty());
    CHECK(c.passes.instructions_after < c.passes.instructions_before);
    for (const auto& run : c.passes.runs) {
        CHECK(!run.what.empty());
        CHECK(run.before != run.after || !run.missed.empty());
    }
    std::string json = opt::passes_json(c.passes, "t.parc");
    CHECK(contains(json, "\"pass\": \"constant propagation\""));
    CHECK(contains(json, "both operands are constants"));
    CHECK(contains(json, "\"before\": \"fn f() -> int\\nentry:\\n"));
}

TEST_CASE(optimized_ir_of_every_example_is_well_formed) {
    for (const auto& path : test::example_files()) {
        Compilation c = compile(test::read_file(path));
        if (!c.runnable) continue;
        std::string problem = opt::verify(c.optimized);
        CHECK(problem.empty());
        if (!problem.empty()) std::cerr << "    " << path.filename().string() << ": " << problem << "\n";
        CHECK(c.passes.instructions_after <= c.passes.instructions_before);
    }
}

int main() { return test::run_all(); }
