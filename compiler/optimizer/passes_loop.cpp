// Loop passes: loop-invariant code motion and strength reduction. Both find
// loops from back edges in the dominator tree and put new code in the loop's
// preheader, the block that runs once just before the loop.

#include <algorithm>
#include <set>

#include "common/util.hpp"
#include "ir/cfg.hpp"
#include "optimizer/analyses.hpp"
#include "optimizer/passes.hpp"

namespace parc::opt {

using ir::Function;
using ir::Instr;
using ir::Op;
using ir::Operand;
using ir::VT;

namespace {

int line_of(const Instr& instr) { return instr.pos.line > 0 ? instr.pos.line : instr.stmt.line; }

std::string code(const std::string& text) { return "'" + text + "'"; }

struct LoopView {
    const ir::Loop& loop;
    std::vector<bool> inside;         // per block
    std::vector<int> defs_in_loop;    // per variable: how many instructions in the loop write it
    std::vector<int> def_line;        // per variable: line of one such write

    LoopView(const Function& fn, const ir::Loop& l) : loop(l) {
        inside.assign(fn.blocks.size(), false);
        for (int b : l.blocks) inside[static_cast<size_t>(b)] = true;
        defs_in_loop.assign(fn.vars.size(), 0);
        def_line.assign(fn.vars.size(), 0);
        for (int b : l.blocks) {
            for (const auto& instr : fn.blocks[static_cast<size_t>(b)].instrs) {
                for (int v : ir::defs(instr)) {
                    ++defs_in_loop[static_cast<size_t>(v)];
                    if (line_of(instr) > 0) def_line[static_cast<size_t>(v)] = line_of(instr);
                }
            }
        }
    }
};

// Is v a counter of this loop: written exactly once in it, as v = v + c or v = v - c?
// On success `step` is the amount it changes by and (block, index) is where.
bool basic_induction(const Function& fn, const LoopView& view, int v, long long& step, int& block, int& index) {
    if (view.defs_in_loop[static_cast<size_t>(v)] != 1) return false;
    for (int b : view.loop.blocks) {
        const auto& instrs = fn.blocks[static_cast<size_t>(b)].instrs;
        for (int i = 0; i < isize(instrs); ++i) {
            const Instr& instr = instrs[static_cast<size_t>(i)];
            if (!instr.dst.is_var(v)) continue;
            if (instr.type != VT::Int) return false;
            block = b;
            index = i;
            if (instr.op == Op::Add && instr.a.is_var(v) && instr.b.is_int()) return step = instr.b.i, true;
            if (instr.op == Op::Add && instr.b.is_var(v) && instr.a.is_int()) return step = instr.a.i, true;
            if (instr.op == Op::Sub && instr.a.is_var(v) && instr.b.is_int()) return step = -instr.b.i, true;
            return false;
        }
    }
    return false;
}

// Where new code goes in a preheader: before the jump into the loop, and
// before the parfor of a parallel loop, so it runs once and every thread sees the result.
size_t insertion_point(const Function& fn, int preheader, int header) {
    const auto& instrs = fn.blocks[static_cast<size_t>(preheader)].instrs;
    for (size_t i = 0; i < instrs.size(); ++i)
        if (instrs[i].op == Op::ParFor && instrs[i].target == header) return i;
    return instrs.size() - 1;
}

// One round of loop-invariant code motion on one loop. With `notes_only` it
// moves nothing and only records why instructions have to stay.
bool hoist(const ir::Program& program, Function& fn, const ir::Loop& loop, const std::vector<int>& idom,
           PassRun& run, bool notes_only, std::set<std::pair<int, std::string>>& noted) {
    LoopView view(fn, loop);
    ReachingDefs reach = reaching_definitions(fn);
    Solution<BitSet> live = liveness(fn);

    std::vector<int> exits;  // blocks in the loop with a way out
    for (int b : loop.blocks)
        for (int s : fn.blocks[static_cast<size_t>(b)].succs)
            if (!view.inside[static_cast<size_t>(s)] && (exits.empty() || exits.back() != b)) exits.push_back(b);

    auto definition_number = [&reach](int block, int index, int var) {
        for (int d : reach.of_var[static_cast<size_t>(var)])
            if (reach.defs[static_cast<size_t>(d)].block == block && reach.defs[static_cast<size_t>(d)].index == index)
                return d;
        return -1;
    };
    auto note = [&](const Instr& instr, const std::string& why) {
        if (!notes_only || line_of(instr) == 0) return;
        std::string shown = instr.expr ? ast::to_source(*instr.expr) : ir::instr_text(program, fn, instr);
        std::string text = code(shown) + " stays inside the loop: " + why;
        if (noted.insert({line_of(instr), text}).second) run.missed.push_back({line_of(instr), text});
    };

    struct Candidate {
        int block;
        int index;
    };
    std::vector<Candidate> candidates;
    for (int b : loop.blocks) {
        const auto& instrs = fn.blocks[static_cast<size_t>(b)].instrs;
        for (int i = 0; i < isize(instrs); ++i) {
            const Instr& instr = instrs[static_cast<size_t>(i)];
            if (!ir::is_pure(instr) || !instr.dst.is_var()) continue;
            int dst = instr.dst.var;

            // Invariant: every operand is a constant, or was last written outside the loop.
            int changing = -1;
            for (int v : ir::uses(instr)) {
                for (int d : reach.reaching(fn, b, i, v)) {
                    int where = reach.defs[static_cast<size_t>(d)].block;
                    if (where >= 0 && view.inside[static_cast<size_t>(where)]) changing = v;
                }
            }
            if (changing >= 0) {
                // Worth a note only when it looks movable: an expression on plain variables
                // where one operand really is unchanged, and the other one existed before
                // the loop but is written in it (and is not simply the loop's counter).
                long long step = 0;
                int at_block = 0, at_index = 0;
                bool plain = ir::is_arithmetic(instr.op) && !instr.a.is_var(dst) && !instr.b.is_var(dst);
                bool one_unchanged = false;
                for (const Operand* o : {&instr.a, &instr.b}) {
                    if (!o->is_var()) continue;
                    if (fn.vars[static_cast<size_t>(o->var)].kind == ir::VarKind::Temp) plain = false;
                    if (view.defs_in_loop[static_cast<size_t>(o->var)] == 0) one_unchanged = true;
                }
                bool from_outside = false;
                for (int d : reach.reaching(fn, b, i, changing)) {
                    int where = reach.defs[static_cast<size_t>(d)].block;
                    if (where < 0 || !view.inside[static_cast<size_t>(where)]) from_outside = true;
                }
                if (plain && one_unchanged && from_outside &&
                    !basic_induction(fn, view, changing, step, at_block, at_index)) {
                    note(instr, fn.vars[static_cast<size_t>(changing)].name + " is written inside the loop (line " +
                                    std::to_string(view.def_line[static_cast<size_t>(changing)]) + ")");
                }
                continue;
            }
            if (ir::can_trap(instr)) {
                note(instr, "it could divide by zero, and the loop might not run at all");
                continue;
            }
            const std::string& name = fn.vars[static_cast<size_t>(dst)].name;
            if (view.defs_in_loop[static_cast<size_t>(dst)] != 1) {
                note(instr, name + " is written more than once in the loop");
                continue;
            }
            // Every use of dst in the loop must read this definition and no other.
            int self = definition_number(b, i, dst);
            bool only_this = true;
            for (int lb : loop.blocks) {
                const auto& body = fn.blocks[static_cast<size_t>(lb)].instrs;
                for (int j = 0; j < isize(body) && only_this; ++j) {
                    std::vector<int> used = ir::uses(body[static_cast<size_t>(j)]);
                    if (std::find(used.begin(), used.end(), dst) == used.end()) continue;
                    std::vector<int> seen = reach.reaching(fn, lb, j, dst);
                    only_this = seen.size() == 1 && seen[0] == self;
                }
            }
            if (!only_this) {
                note(instr, "the loop also reads the value " + name + " had before this line");
                continue;
            }
            // Moving it must not change what the code after the loop sees.
            bool on_every_pass = std::all_of(exits.begin(), exits.end(), [&](int e) { return ir::dominates(idom, b, e); });
            bool unused_after = true;
            for (int e : exits)
                for (int s : fn.blocks[static_cast<size_t>(e)].succs)
                    if (!view.inside[static_cast<size_t>(s)] && live.in[static_cast<size_t>(s)].test(dst)) unused_after = false;
            if (!on_every_pass && !unused_after) {
                note(instr, name + " is used after the loop, and this line does not run on every pass");
                continue;
            }
            candidates.push_back({b, i});
        }
    }
    if (notes_only || candidates.empty()) return false;

    // Take the instructions out (last first, so earlier positions stay valid)...
    std::vector<Instr> moved;
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
        auto& instrs = fn.blocks[static_cast<size_t>(it->block)].instrs;
        moved.push_back(instrs[static_cast<size_t>(it->index)]);
        instrs.erase(instrs.begin() + it->index);
    }
    std::reverse(moved.begin(), moved.end());
    // ...and put them in the preheader, in their original order.
    int header = loop.header;
    std::string loop_label = fn.blocks[static_cast<size_t>(header)].label;
    int loop_line = 0;
    for (const auto& instr : fn.blocks[static_cast<size_t>(header)].instrs)
        if (loop_line == 0) loop_line = line_of(instr);
    int pre = ensure_preheader(fn, header, loop.blocks);
    auto& target = fn.blocks[static_cast<size_t>(pre)].instrs;
    size_t at = insertion_point(fn, pre, header);
    target.insert(target.begin() + static_cast<std::ptrdiff_t>(at), moved.begin(), moved.end());
    for (const auto& instr : moved) {
        run.changes.push_back({line_of(instr), "moved " + code(ir::instr_text(program, fn, instr)) +
                                                   " out of the loop" +
                                                   (loop_line ? " at line " + std::to_string(loop_line) : " " + loop_label) +
                                                   ": its operands do not change inside the loop"});
    }
    ir::link(fn);
    return true;
}

}  // namespace

bool loop_invariant_code_motion(const ir::Program& program, Function& fn, PassRun& run) {
    bool changed = false;
    std::set<std::pair<int, std::string>> noted;
    // Each round moves at least one instruction out of some loop, then the
    // analyses are redone; an instruction that depended on a moved one can follow.
    for (int round = 0; round < 200; ++round) {
        ir::link(fn);
        std::vector<int> idom = ir::immediate_dominators(fn);
        std::vector<ir::Loop> loops = ir::natural_loops(fn, idom);
        bool moved = false;
        for (auto loop = loops.rbegin(); loop != loops.rend() && !moved; ++loop)  // inner loops first
            moved = hoist(program, fn, *loop, idom, run, false, noted);
        if (!moved) break;
        changed = true;
    }
    ir::link(fn);
    std::vector<int> idom = ir::immediate_dominators(fn);
    for (const auto& loop : ir::natural_loops(fn, idom)) hoist(program, fn, loop, idom, run, true, noted);
    return changed;
}

bool strength_reduction(const ir::Program& program, Function& fn, PassRun& run) {
    bool changed = false;
    std::set<std::pair<int, std::string>> noted;
    for (int round = 0; round < 100; ++round) {
        ir::link(fn);
        std::vector<int> idom = ir::immediate_dominators(fn);
        std::vector<ir::Loop> loops = ir::natural_loops(fn, idom);
        bool reduced = false;
        for (auto loop = loops.rbegin(); loop != loops.rend() && !reduced; ++loop) {
            LoopView view(fn, *loop);
            for (size_t k = 0; k < loop->blocks.size() && !reduced; ++k) {
                int b = loop->blocks[k];
                auto& instrs = fn.blocks[static_cast<size_t>(b)].instrs;
                for (int i = 0; i < isize(instrs) && !reduced; ++i) {
                    Instr& mul = instrs[static_cast<size_t>(i)];
                    // t = i * c or t = c * i, with i a counter of this loop.
                    if (mul.op != Op::Mul || mul.type != VT::Int || !mul.dst.is_var()) continue;
                    const Operand* counter = mul.a.is_var() && mul.b.is_int() ? &mul.a
                                             : mul.b.is_var() && mul.a.is_int() ? &mul.b
                                                                                : nullptr;
                    if (!counter || mul.dst.is_var(counter->var)) continue;
                    long long factor = counter == &mul.a ? mul.b.i : mul.a.i;
                    long long step = 0;
                    int step_block = 0, step_index = 0;
                    if (!basic_induction(fn, view, counter->var, step, step_block, step_index)) continue;
                    std::string before = ir::instr_text(program, fn, mul);
                    if (loop->parallel) {
                        std::string text = code(before) + " keeps its multiplication: in a parallel loop every thread "
                                           "starts at a different " + fn.vars[static_cast<size_t>(counter->var)].name +
                                           ", so a running total can't be carried from one iteration to the next";
                        if (noted.insert({line_of(mul), text}).second) run.missed.push_back({line_of(mul), text});
                        continue;
                    }

                    int iv = counter->var;
                    std::string iv_name = fn.vars[static_cast<size_t>(iv)].name;
                    // Named after what it holds: i.x4 is i times 4.
                    std::string running_base = iv_name + ".x" + std::to_string(factor);
                    std::string unique = running_base;
                    for (int n = 2; std::any_of(fn.vars.begin(), fn.vars.end(), [&](const ir::Var& v) { return v.name == unique; }); ++n)
                        unique = running_base + "." + std::to_string(n);
                    int running = fn.add_var(unique, fn.vars[static_cast<size_t>(mul.dst.var)].type, ir::VarKind::Temp);
                    // 1. The multiplication reads the running value.
                    Instr stmt_source = mul;
                    mul.op = Op::Copy;
                    mul.a = Operand::of_var(running);
                    mul.b = Operand{};
                    std::string after = ir::instr_text(program, fn, mul);
                    // 2. Right after the counter changes, the running value changes by factor * step.
                    Instr bump;
                    bump.op = Op::Add;
                    bump.type = VT::Int;
                    bump.dst = Operand::of_var(running);
                    bump.a = Operand::of_var(running);
                    bump.b = Operand::of_int(wrap32(factor * step));
                    auto& step_instrs = fn.blocks[static_cast<size_t>(step_block)].instrs;
                    bump.stmt = step_instrs[static_cast<size_t>(step_index)].stmt;
                    step_instrs.insert(step_instrs.begin() + step_index + 1, bump);
                    // 3. Before the loop it starts as counter * factor.
                    int header = loop->header;
                    int pre = ensure_preheader(fn, header, loop->blocks);
                    Instr init;
                    init.op = Op::Mul;
                    init.type = VT::Int;
                    init.dst = Operand::of_var(running);
                    init.a = Operand::of_var(iv);
                    init.b = Operand::of_int(factor);
                    init.stmt = stmt_source.stmt;
                    auto& pre_instrs = fn.blocks[static_cast<size_t>(pre)].instrs;
                    pre_instrs.insert(pre_instrs.end() - 1, init);

                    std::string running_name = fn.vars[static_cast<size_t>(running)].name;
                    run.changes.push_back(
                        {line_of(stmt_source), code(before) + " became " + code(after) + ": " + running_name +
                                                   " starts as " + iv_name + " * " + std::to_string(factor) +
                                                   " before the loop and grows by " + std::to_string(wrap32(factor * step)) +
                                                   " each time " + iv_name + " changes by " + std::to_string(step) +
                                                   ", so the loop adds where it used to multiply"});
                    reduced = true;
                }
            }
        }
        if (!reduced) break;
        changed = true;
    }
    ir::link(fn);
    return changed;
}

}  // namespace parc::opt
