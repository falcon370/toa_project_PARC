#include <string>
#include <vector>

#include "codegen/codegen.hpp"
#include "common/test_framework.hpp"
#include "common/test_support.hpp"

using namespace parc;
using test::contains;

namespace {

// The VM listing of the whole program.
std::string listing(const std::string& source, int registers = 12, bool optimize = true) {
    CompileOptions options;
    options.registers = registers;
    options.optimize = optimize;
    Compilation c = compile(source, options);
    if (!c.runnable) {
        ++test::failures();
        std::cerr << "does not compile: " << source << "\n";
        return "";
    }
    return vm::to_text(c.code);
}

codegen::Interval interval(int var, int start, int end) { return {var, start, end, {}}; }

}  // namespace

// ---------------------------------------------------------------- register allocation

TEST_CASE(intervals_that_do_not_overlap_share_a_register) {
    std::vector<codegen::Interval> intervals = {interval(0, 0, 2), interval(1, 3, 5), interval(2, 6, 8)};
    int slots = codegen::linear_scan(intervals, 1);
    CHECK_EQ(slots, 0);
    for (const auto& i : intervals) CHECK(!i.loc.slot && i.loc.index == 0);
}

TEST_CASE(overlapping_intervals_get_different_registers) {
    std::vector<codegen::Interval> intervals = {interval(0, 0, 9), interval(1, 1, 4), interval(2, 5, 8)};
    CHECK_EQ(codegen::linear_scan(intervals, 2), 0);
    CHECK_EQ(intervals[0].loc.index, 0);
    CHECK_EQ(intervals[1].loc.index, 1);
    CHECK_EQ(intervals[2].loc.index, 1);  // reuses the register that interval 1 gave back
}

TEST_CASE(when_registers_run_out_the_interval_that_ends_last_is_spilled) {
    //  v0 |---------------------|
    //  v1    |-----|
    //  v2      |-----|               three alive at once, two registers
    std::vector<codegen::Interval> intervals = {interval(0, 0, 20), interval(1, 2, 6), interval(2, 4, 8)};
    CHECK_EQ(codegen::linear_scan(intervals, 2), 1);
    CHECK(intervals[0].loc.slot);  // the longest one goes to the frame
    CHECK(!intervals[1].loc.slot && !intervals[2].loc.slot);
    CHECK(intervals[1].loc.index != intervals[2].loc.index);
    // If the new interval is itself the longest, it is the one spilled.
    std::vector<codegen::Interval> other = {interval(0, 0, 5), interval(1, 1, 6), interval(2, 2, 30)};
    CHECK_EQ(codegen::linear_scan(other, 2), 1);
    CHECK(other[2].loc.slot);
}

TEST_CASE(live_intervals_cover_loops) {
    Compilation c = compile("fn f(n: int) -> int { let s: int = 0; let i: int = 0; while (i < n) { s = s + i; i = i + 1; } return s; }");
    const ir::Function& fn = c.ir.functions.at(0);
    std::vector<codegen::Interval> intervals = codegen::live_intervals(fn);
    int last = fn.instruction_count() - 1;
    for (const auto& i : intervals) {
        const std::string& name = fn.vars[static_cast<size_t>(i.var)].name;
        CHECK(i.start <= i.end);
        // n is needed on every pass, so it lives from the start to the end of the loop.
        if (name == "n") CHECK(i.start == 0 && i.end >= last - 2);
        if (name == "s") CHECK_EQ(i.end, last);  // until the return
    }
    for (size_t k = 1; k < intervals.size(); ++k) CHECK(intervals[k - 1].start <= intervals[k].start);
}

// ---------------------------------------------------------------- instruction selection

TEST_CASE(one_ir_instruction_becomes_one_vm_instruction) {
    std::string text = listing("fn f(a: int, b: int) -> int { return a + b * 2; } fn main() { print(f(1, 2)); }");
    CHECK(contains(text, "GETARG r0, 0"));
    CHECK(contains(text, "GETARG r1, 1"));
    CHECK(contains(text, "MULI r2, r1, 2"));  // multiplication by a constant
    CHECK(contains(text, "ADD r0, r0, r2") || contains(text, "ADD r2, r0, r2") || contains(text, "ADD r1, r0, r2"));
    CHECK(contains(text, "RET r"));
}

TEST_CASE(constants_use_the_immediate_forms) {
    CHECK(contains(listing("fn f(x: int) -> bool { return 5 < x; } fn main() { print(f(1)); }"), "GTI "));  // 5 < x is x > 5
    CHECK(contains(listing("fn f(x: int) -> int { return x - 3; } fn main() { print(f(1)); }"), "SUBI "));
    CHECK(contains(listing("fn f(x: int) -> bool { return x == 0; } fn main() { print(f(1)); }"), "EQI "));
    // There is no immediate division: the constant is loaded into a spare register first.
    std::string div = listing("fn f(x: int) -> int { return x / 3; } fn main() { print(f(1)); }");
    CHECK(contains(div, "LI r") && contains(div, "DIV r"));
}

TEST_CASE(float_and_int_operations_use_different_instructions) {
    std::string text = listing("fn f(x: float, n: int) -> float { if (x < 1.5) { return x * n; } return -x; } fn main() { print(f(1.0, 2)); }");
    CHECK(contains(text, "FLT "));
    CHECK(contains(text, "I2F "));
    CHECK(contains(text, "FMUL "));
    CHECK(contains(text, "FNEG "));
    CHECK(contains(text, "LF r"));
}

TEST_CASE(arrays_calls_and_print) {
    std::string text = listing("fn g(a: int[]) -> int { return len(a); } fn main() { let a: int[] = new int[4]; a[1] = 7; print(\"n\", g(a), a[1]); }", 12, false);
    CHECK(contains(text, "NEWARR r"));
    CHECK(contains(text, "ASTORE r"));
    CHECK(contains(text, "ARG r"));
    CHECK(contains(text, "CALL r"));
    CHECK(contains(text, ", g\n") || contains(text, ", g "));
    CHECK(contains(text, "ALOAD r"));
    CHECK(contains(text, "LEN r"));
    CHECK(contains(text, "PRINTS \"n\""));
}

// ---------------------------------------------------------------- jumps

TEST_CASE(jump_addresses_are_filled_in) {
    Compilation c = compile("fn f(n: int) -> int { let s: int = 0; for i in 0..n { if (i % 2 == 0) { continue; } s = s + i; } return s; } fn main() { print(f(9)); }");
    for (const auto& fn : c.code.functions) {
        for (const auto& instr : fn.code) {
            bool jump = instr.op == vm::VOp::JMP || instr.op == vm::VOp::BRT || instr.op == vm::VOp::BRF;
            if (jump) CHECK(instr.imm >= 0 && instr.imm < static_cast<long long>(fn.code.size()));
        }
    }
}

TEST_CASE(no_jump_to_the_next_instruction) {
    for (bool optimize : {false, true}) {
        CompileOptions options;
        options.optimize = optimize;
        Compilation c = compile("fn f(n: int) -> int { if (n > 0) { n = n + 1; } else { n = n - 1; } while (n > 5) { n = n - 2; } return n; } fn main() { print(f(9)); }", options);
        for (const auto& fn : c.code.functions)
            for (size_t pc = 0; pc < fn.code.size(); ++pc)
                if (fn.code[pc].op == vm::VOp::JMP) CHECK(fn.code[pc].imm != static_cast<long long>(pc) + 1);
    }
}

TEST_CASE(the_peephole_step_removes_instructions) {
    CompileOptions options;
    options.optimize = false;  // unoptimized IR has many blocks that fall into each other
    Compilation c = compile("fn main() { let x: int = 3; if (x > 1) { print(1); } else { print(2); } print(3); }", options);
    CHECK(c.code.peephole_removed > 0);
}

// ---------------------------------------------------------------- spilling and parallel loops

TEST_CASE(with_few_registers_values_are_spilled_and_the_result_is_the_same) {
    std::string source = "fn f(a: int, b: int, c: int, d: int) -> int { let e: int = a + b; let g: int = c + d; let h: int = e * g; "
                         "let k: int = a * d; return e + g + h + k + a + b + c + d; } fn main() { print(f(1, 2, 3, 4)); }";
    CompileOptions few;
    few.registers = 4;  // one register for variables, three spare
    Compilation c = compile(source, few);
    int slots = 0;
    for (const auto& fn : c.code.functions) slots += fn.slots;
    CHECK(slots > 0);
    CHECK(contains(vm::to_text(c.code), "LOADS r"));
    CHECK(contains(vm::to_text(c.code), "STORES r"));
    CHECK_EQ(test::output(source, few), "45\n");
    CHECK_EQ(test::output(source), "45\n");
}

TEST_CASE(a_parallel_loop_has_a_table_entry) {
    Compilation c = compile("fn f(a: int[], n: int) -> int { let s: int = 0; parallel for i in 0..n reduce(+: s) { s = s + a[i]; } return s; }");
    const vm::Function& fn = c.code.functions.at(0);
    CHECK_EQ(fn.loops.size(), size_t{1});
    const vm::ParLoop& loop = fn.loops.at(0);
    CHECK(loop.var.valid() && loop.end.valid());
    CHECK_EQ(loop.reductions.size(), size_t{1});
    CHECK_EQ(loop.reductions.at(0).op, '+');
    CHECK(fn.code.at(static_cast<size_t>(loop.end_pc)).op == vm::VOp::PAREND);
    CHECK(loop.head_pc > 0 && loop.head_pc < loop.end_pc);
    // The threads start right after the PARFOR.
    bool found = false;
    for (size_t pc = 0; pc < fn.code.size(); ++pc)
        if (fn.code[pc].op == vm::VOp::PARFOR) found = static_cast<int>(pc) < loop.head_pc;
    CHECK(found);
}

TEST_CASE(code_json_lists_code_and_variable_locations) {
    Compilation c = compile("fn f(a: int) -> int { let b: int = a * 2; return b + 1; } fn main() { print(f(3)); }");
    std::string json = vm::to_json(c.code, "t.parc");
    CHECK(contains(json, "\"registers\": 12"));
    CHECK(contains(json, "\"text\": \"GETARG r0, 0\""));
    CHECK(contains(json, "\"name\": \"a\""));
    CHECK(contains(json, "\"where\": \"r0\""));
}

TEST_CASE(every_example_generates_code) {
    for (const auto& path : test::example_files()) {
        Compilation c = compile(test::read_file(path));
        CHECK(c.runnable);
        for (const auto& fn : c.code.functions) CHECK(!fn.code.empty());
    }
}

int main() { return test::run_all(); }
