#include <string>
#include <vector>

#include "common/test_framework.hpp"
#include "common/test_support.hpp"
#include "ir/cfg.hpp"

using namespace parc;
using test::contains;

namespace {

// The unoptimized IR of the first function in the source.
ir::Function lower(const std::string& source, std::string* text = nullptr) {
    Compilation c = compile(source);
    if (!c.lowered) {
        ++test::failures();
        std::cerr << "does not compile: " << source << "\n";
        for (const auto& d : c.findings) std::cerr << "    " << format(d) << "\n";
        return {};
    }
    if (text) *text = ir::to_text(c.ir, c.ir.functions.at(0));
    return c.ir.functions.at(0);
}

std::string lower_text(const std::string& source) {
    std::string text;
    lower(source, &text);
    return text;
}

int block_named(const ir::Function& fn, const std::string& prefix) {
    for (size_t b = 0; b < fn.blocks.size(); ++b)
        if (fn.blocks[b].label.starts_with(prefix)) return static_cast<int>(b);
    return -1;
}

}  // namespace

// ---------------------------------------------------------------- lowering

TEST_CASE(expressions_become_three_address_code) {
    CHECK_EQ(lower_text("fn f(a: int, b: int) -> int { let x: int = a + b * 2; return x - 1; }"),
             "fn f(a: int, b: int) -> int\n"
             "entry:\n"
             "    t1 = b * 2\n"
             "    x = a + t1\n"
             "    t2 = x - 1\n"
             "    return t2\n");
}

TEST_CASE(conditions_become_jumps_with_short_circuit) {
    // a / b must only run when the first test passed.
    CHECK_EQ(lower_text("fn f(a: int, b: int) -> int { if (b != 0 && a / b > 2) { return 1; } return 0; }"),
             "fn f(a: int, b: int) -> int\n"
             "entry:\n"
             "    t1 = b != 0\n"
             "    if t1 goto and.rhs3 else if.end2\n"
             "and.rhs3:\n"
             "    t2 = a / b\n"
             "    t3 = t2 > 2\n"
             "    if t3 goto if.then1 else if.end2\n"
             "if.then1:\n"
             "    return 1\n"
             "if.end2:\n"
             "    return 0\n");
    // "||" jumps to the body as soon as the left side is true, and "!" swaps the targets.
    std::string text = lower_text("fn f(a: bool, b: bool) -> int { if (a || !b) { return 1; } return 0; }");
    CHECK(contains(text, "if a goto if.then1 else or.rhs3"));
    CHECK(contains(text, "if b goto if.end2 else if.then1"));
}

TEST_CASE(a_logical_value_is_built_from_jumps) {
    std::string text = lower_text("fn f(a: bool, b: bool) -> bool { let r: bool = a && b; return r; }");
    CHECK(contains(text, "t1 = true"));
    CHECK(contains(text, "t1 = false"));
    CHECK(contains(text, "r = t1"));
}

TEST_CASE(while_loop_shape) {
    CHECK_EQ(lower_text("fn f(n: int) -> int { let i: int = 0; while (i < n) { i = i + 1; } return i; }"),
             "fn f(n: int) -> int\n"
             "entry:\n"
             "    i = 0\n"
             "    goto while.head1\n"
             "while.head1:\n"
             "    t1 = i < n\n"
             "    if t1 goto while.body2 else while.end3\n"
             "while.body2:\n"
             "    i = i + 1\n"
             "    goto while.head1\n"
             "while.end3:\n"
             "    return i\n");
}

TEST_CASE(for_loop_evaluates_its_range_once) {
    std::string text = lower_text("fn f(a: int[]) -> int { let s: int = 0; for i in 0..len(a) { s = s + a[i]; } return s; }");
    CHECK_EQ(test::count(text, "len(a)"), 1);
    CHECK(contains(text, "i.end = len(a)"));
    CHECK(contains(text, "t1 = i < i.end"));
    CHECK(contains(text, "for.step3:\n    i = i + 1\n    goto for.head1"));
}

TEST_CASE(break_and_continue_jump_to_the_right_blocks) {
    std::string text = lower_text(
        "fn f(n: int) { for i in 0..n { if (i == 2) { continue; } if (i == 5) { break; } print(i); } }");
    CHECK(contains(text, "if.then5:\n    goto for.step3"));  // continue goes to the step, not the head
    CHECK(contains(text, "if.then7:\n    goto for.end4"));
}

TEST_CASE(int_is_converted_where_a_float_is_needed) {
    std::string text = lower_text("fn f(n: int, x: float) -> float { let s: float = 0; s = x * n; return n; }");
    CHECK(contains(text, "s = 0.0"));
    CHECK(contains(text, "t1 = float(n)\n    s = x * t1"));
    CHECK(contains(text, "t2 = float(n)\n    return t2"));
}

TEST_CASE(arrays_calls_and_print) {
    std::string text = lower_text(
        "fn f(n: int) -> int { let a: int[] = new int[n]; a[0] = len(a); print(\"v\", a[0], 1.5, true); return g(a[0]); }"
        " fn g(x: int) -> int { return x; }");
    CHECK(contains(text, "a = new int[n]"));
    CHECK(contains(text, "t1 = len(a)\n    a[0] = t1"));
    CHECK(contains(text, "print \"v\", t2, 1.5, true"));
    CHECK(contains(text, "= call g(t3)"));
}

TEST_CASE(variables_with_the_same_name_get_different_ir_names) {
    std::string text = lower_text("fn f() { let x: int = 1; { let x: int = 2; print(x); } print(x); }");
    CHECK(contains(text, "x = 1"));
    CHECK(contains(text, "x.2 = 2"));
    CHECK(contains(text, "print x.2\n    print x\n"));
}

TEST_CASE(code_after_a_return_goes_to_a_block_nothing_reaches) {
    ir::Function fn = lower("fn f() -> int { return 1; print(2); }");
    int dead = block_named(fn, "dead");
    CHECK(dead > 0);
    CHECK(!ir::reachable_blocks(fn).at(static_cast<size_t>(dead)));
}

TEST_CASE(a_safe_parallel_loop_gets_parfor_and_parend) {
    std::string text = lower_text(
        "fn f(a: int[], n: int) -> int { let s: int = 0; parallel for i in 0..n reduce(+: s) { s = s + a[i]; } return s; }");
    CHECK(contains(text, "parfor i .. i.end reduce(+: s)"));
    CHECK(contains(text, "par.end5:\n    parend reduce(+: s)"));
    // An unsafe one is lowered as a normal loop.
    text = lower_text("fn f(a: int[], n: int) { parallel for i in 1..n { a[i] = a[i - 1]; } }");
    CHECK(!contains(text, "parfor"));
}

// ---------------------------------------------------------------- control-flow graph

TEST_CASE(edges_follow_the_terminators) {
    ir::Function fn = lower("fn f(c: bool) -> int { if (c) { return 1; } else { return 2; } }");
    CHECK_EQ(fn.blocks.at(0).succs.size(), size_t{2});
    int then_block = block_named(fn, "if.then");
    CHECK_EQ(fn.blocks.at(static_cast<size_t>(then_block)).preds.size(), size_t{1});
    CHECK(fn.blocks.at(static_cast<size_t>(then_block)).succs.empty());
}

TEST_CASE(reverse_postorder_visits_a_block_before_what_it_jumps_forward_to) {
    ir::Function fn = lower("fn f(n: int) -> int { let i: int = 0; while (i < n) { if (i == 3) { i = i + 2; } i = i + 1; } return i; }");
    std::vector<int> order = ir::reverse_postorder(fn);
    CHECK_EQ(order.at(0), 0);
    std::vector<int> position(fn.blocks.size(), -1);
    for (size_t i = 0; i < order.size(); ++i) position[static_cast<size_t>(order[i])] = static_cast<int>(i);
    std::vector<int> idom = ir::immediate_dominators(fn);
    // A dominator always comes earlier in reverse postorder.
    for (int b : order)
        if (idom[static_cast<size_t>(b)] >= 0) CHECK(position[static_cast<size_t>(idom[static_cast<size_t>(b)])] < position[static_cast<size_t>(b)]);
}

TEST_CASE(dominators_of_a_diamond) {
    ir::Function fn = lower("fn f(c: bool) -> int { let x: int = 0; if (c) { x = 1; } else { x = 2; } return x; }");
    std::vector<int> idom = ir::immediate_dominators(fn);
    int then_block = block_named(fn, "if.then"), else_block = block_named(fn, "if.else"), end = block_named(fn, "if.end");
    CHECK_EQ(idom.at(0), -1);
    CHECK_EQ(idom.at(static_cast<size_t>(then_block)), 0);
    CHECK_EQ(idom.at(static_cast<size_t>(else_block)), 0);
    CHECK_EQ(idom.at(static_cast<size_t>(end)), 0);  // neither branch dominates the join
    CHECK(ir::dominates(idom, 0, end));
    CHECK(!ir::dominates(idom, then_block, end));
    CHECK(ir::dominates(idom, end, end));
}

TEST_CASE(natural_loops_and_their_nesting) {
    ir::Function fn = lower("fn f(n: int) { for i in 0..n { for j in 0..n { print(i, j); } } while (n > 100) { n = n - 1; } }");
    std::vector<int> idom = ir::immediate_dominators(fn);
    std::vector<ir::Loop> loops = ir::natural_loops(fn, idom);
    CHECK_EQ(loops.size(), size_t{3});
    int depth_two = 0;
    for (const auto& loop : loops) {
        CHECK(fn.blocks.at(static_cast<size_t>(loop.header)).label.find("head") != std::string::npos);
        CHECK(!loop.latches.empty());
        for (int latch : loop.latches) CHECK(ir::dominates(idom, loop.header, latch));
        if (loop.depth == 2) ++depth_two;
        CHECK(!loop.parallel);
    }
    CHECK_EQ(depth_two, 1);
    // The outer loop is listed before the loop inside it and contains its blocks.
    CHECK(loops[0].blocks.size() > loops[1].blocks.size());
}

TEST_CASE(parallel_loops_are_marked) {
    ir::Function fn = lower("fn f(a: int[], n: int) { parallel for i in 0..n { a[i] = i; } }");
    std::vector<ir::Loop> loops = ir::natural_loops(fn, ir::immediate_dominators(fn));
    CHECK_EQ(loops.size(), size_t{1});
    CHECK(loops.at(0).parallel);
}

TEST_CASE(cyclomatic_complexity_counts_decisions) {
    CHECK_EQ(ir::cyclomatic_complexity(lower("fn f() { print(1); }")), 1);
    CHECK_EQ(ir::cyclomatic_complexity(lower("fn f(c: bool) { if (c) { print(1); } }")), 2);
    CHECK_EQ(ir::cyclomatic_complexity(lower("fn f(c: bool) { if (c) { print(1); } else { print(2); } }")), 2);
    CHECK_EQ(ir::cyclomatic_complexity(lower("fn f(n: int) { while (n > 0) { if (n == 3) { print(n); } n = n - 1; } }")), 3);
    // Each && and || is one more decision.
    CHECK_EQ(ir::cyclomatic_complexity(lower("fn f(a: bool, b: bool) { if (a && b) { print(1); } }")), 3);
    CHECK_EQ(lower("fn f(n: int) { while (n > 0) { if (n == 3) { print(n); } n = n - 1; } }").max_nesting, 2);
}

// ---------------------------------------------------------------- files

TEST_CASE(ir_and_cfg_json) {
    Compilation c = compile("fn f(n: int) -> int { let s: int = 0; for i in 0..n { s = s + i; } return s; }");
    std::string ir_json = ir::to_json(c.ir, "t.parc");
    CHECK(contains(ir_json, "\"signature\": \"fn f(n: int) -> int\""));
    CHECK(contains(ir_json, "\"label\": \"for.head1\""));
    CHECK(contains(ir_json, "\"text\": \"s = s + i\""));
    CHECK(contains(ir_json, "\"kind\": \"temporary\""));
    std::string cfg = ir::cfg_json(c.ir, "t.parc");
    CHECK(contains(cfg, "\"kind\": \"true\""));
    CHECK(contains(cfg, "\"back\": true"));
    CHECK(contains(cfg, "\"cyclomatic_complexity\": 2"));
    CHECK(contains(cfg, "\"loops\": 1"));
}

TEST_CASE(every_example_lowers_to_ir) {
    for (const auto& path : test::example_files()) {
        Compilation c = compile(test::read_file(path));
        CHECK(c.lowered);
        if (!c.lowered) std::cerr << "    " << path.filename().string() << "\n";
        for (const auto& fn : c.ir.functions) {
            CHECK(!fn.blocks.empty());
            for (const auto& block : fn.blocks) CHECK(!block.instrs.empty() && ir::is_terminator(block.instrs.back().op));
        }
    }
}

int main() { return test::run_all(); }
