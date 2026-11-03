#include "semantic/flow_checks.hpp"

#include <algorithm>
#include <functional>

#include "common/util.hpp"
#include "ir/cfg.hpp"
#include "optimizer/analyses.hpp"

namespace parc::sem {
namespace {

using ir::Function;
using ir::Instr;
using ir::Op;
using ir::Pos;

Diagnostic finding(const char* severity, const char* kind, Pos pos, const Function& fn, std::string message,
                   std::string hint) {
    Diagnostic d;
    d.phase = "semantic";
    d.severity = severity;
    d.kind = kind;
    d.line = pos.line;
    d.col = pos.col;
    d.stmt_line = pos.line;
    d.function = fn.name;
    d.message = std::move(message);
    d.hint = std::move(hint);
    return d;
}

std::string quoted(const std::string& name) { return "'" + name + "'"; }

// Where a compiler-made instruction belongs in the source: its own position
// if it has one, else its statement's.
Pos place(const Instr& instr) { return instr.pos.line > 0 ? instr.pos : instr.stmt; }

}  // namespace

// ---------------------------------------------------------------- reachability

std::vector<Diagnostic> check_reachability(const ir::Program& program) {
    std::vector<Diagnostic> out;
    for (const auto& fn : program.functions) {
        std::vector<bool> reachable = ir::reachable_blocks(fn);

        // One report per stretch of unreachable blocks, at its first line of source code.
        bool in_dead_stretch = false;
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            if (reachable[b]) {
                in_dead_stretch = false;
                continue;
            }
            if (in_dead_stretch) continue;
            for (const auto& instr : fn.blocks[b].instrs) {
                if (instr.pos.line == 0) continue;  // added by the compiler
                Diagnostic d = finding("warning", "unreachable-code", instr.stmt.line > 0 ? instr.stmt : instr.pos, fn,
                                       "this code can never run",
                                       "every path to it ends earlier, with a return, break or continue");
                d.proof = "reachability: a depth-first search from the start of " + quoted(fn.name) +
                          " never arrives at this statement";
                d.cwe = "CWE-561";
                out.push_back(std::move(d));
                in_dead_stretch = true;
                break;
            }
        }

        // The builder ends every function with a return of its own. If that
        // one can be reached in a function that must return a value, some
        // path has no return statement.
        bool returns_value = fn.return_type.base != ast::BaseType::Void || fn.return_type.is_array;
        if (!returns_value) continue;
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            if (!reachable[b] || fn.blocks[b].instrs.empty()) continue;
            const Instr& last = fn.blocks[b].instrs.back();
            if (last.op != Op::Return || last.pos.line != 0) continue;
            Diagnostic d = finding("error", "missing-return", fn.pos, fn,
                                   quoted(fn.name) + " can reach its end without returning a value",
                                   "end the function with a return statement");
            d.proof = "reachability: the end of the function body can be reached from its start";
            d.subject = fn.name;
            out.push_back(std::move(d));
            break;
        }
    }
    return out;
}

// ---------------------------------------------------------------- definite assignment

std::vector<Diagnostic> check_definite_assignment(const ir::Program& program) {
    std::vector<Diagnostic> out;
    for (const auto& fn : program.functions) {
        int n = isize(fn.vars);
        bool any = false;
        BitSet at_entry(n, true);  // only variables declared without a value start out unassigned
        for (int v = 0; v < n; ++v) {
            if (!fn.vars[static_cast<size_t>(v)].declared_without_value) continue;
            at_entry.reset(v);
            any = true;
        }
        if (!any) continue;

        // The "let x: int;" itself stores a default value; that does not count.
        auto assigns = [&fn](const Instr& instr, BitSet& assigned) {
            for (int v : ir::defs(instr)) {
                bool is_default = instr.pos.line == 0 && instr.op == Op::Copy &&
                                  fn.vars[static_cast<size_t>(v)].declared_without_value;
                if (is_default) assigned.reset(v);
                else assigned.set(v);
            }
        };

        // Forward, meet = intersection: assigned on every path.
        opt::Problem<BitSet> p;
        p.forward = true;
        p.boundary = at_entry;
        p.initial = BitSet(n, true);
        p.meet = [](const BitSet& a, const BitSet& b) {
            BitSet r = a;
            r.intersect(b);
            return r;
        };
        p.transfer = [&](int block, const BitSet& in) {
            BitSet assigned = in;
            for (const auto& instr : fn.blocks[static_cast<size_t>(block)].instrs) assigns(instr, assigned);
            return assigned;
        };
        opt::Solution<BitSet> solution = opt::solve(fn, p);

        std::vector<bool> reachable = ir::reachable_blocks(fn);
        std::vector<bool> reported(fn.vars.size(), false);
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            if (!reachable[b]) continue;
            BitSet assigned = solution.in[b];
            for (const auto& instr : fn.blocks[b].instrs) {
                for (int v : ir::uses(instr)) {
                    if (assigned.test(v) || reported[static_cast<size_t>(v)]) continue;
                    reported[static_cast<size_t>(v)] = true;
                    const std::string& name = fn.vars[static_cast<size_t>(v)].name;
                    Diagnostic d = finding("warning", "uninitialized", place(instr), fn,
                                           quoted(name) + " is used here, but on some path it has no value yet",
                                           "give it a value where it is declared: let " + name + ": " +
                                               ast::to_string(fn.vars[static_cast<size_t>(v)].type) + " = ...;");
                    d.subject = name;
                    d.cwe = "CWE-457";
                    d.stmt_line = instr.stmt.line;
                    d.proof = "definite assignment: at least one path from the declaration of " + quoted(name) +
                              " at line " + std::to_string(fn.vars[static_cast<size_t>(v)].pos.line) +
                              " to this use never assigns it";
                    out.push_back(std::move(d));
                }
                assigns(instr, assigned);
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------- dead stores

std::vector<Diagnostic> check_dead_stores(const ir::Program& program) {
    std::vector<Diagnostic> out;
    for (const auto& fn : program.functions) {
        std::vector<int> reads(fn.vars.size(), 0);
        for (const auto& block : fn.blocks)
            for (const auto& instr : block.instrs)
                for (int v : ir::uses(instr)) ++reads[static_cast<size_t>(v)];

        opt::Solution<BitSet> live = opt::liveness(fn);
        std::vector<bool> reachable = ir::reachable_blocks(fn);
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            if (!reachable[b]) continue;
            // Walk the block backward, keeping the set of live variables up to date.
            BitSet alive = live.out[b];
            const auto& instrs = fn.blocks[b].instrs;
            for (auto it = instrs.rbegin(); it != instrs.rend(); ++it) {
                const Instr& instr = *it;
                if (instr.dst.is_var() && instr.pos.line > 0 && !alive.test(instr.dst.var)) {
                    const ir::Var& var = fn.vars[static_cast<size_t>(instr.dst.var)];
                    // A variable that is never read at all already has its own warning, and
                    // "let x: int = 0;" before the real first value is a common, harmless habit.
                    bool defensive_init = instr.decl_init && instr.op == Op::Copy && instr.a.is_const();
                    if (var.kind == ir::VarKind::Local && reads[static_cast<size_t>(instr.dst.var)] > 0 &&
                        !defensive_init) {
                        Diagnostic d = finding("warning", "dead-store", instr.stmt.line > 0 ? instr.stmt : instr.pos, fn,
                                               "the value assigned to " + quoted(var.name) + " here is never used",
                                               "it is overwritten, or the function ends, before anything reads it");
                        d.subject = var.name;
                        d.cwe = "CWE-563";
                        d.proof = "liveness: " + quoted(var.name) + " is not live after this assignment";
                        out.push_back(std::move(d));
                    }
                }
                for (int v : ir::defs(instr)) alive.reset(v);
                for (int v : ir::uses(instr)) alive.set(v);
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------- call graph

std::vector<Diagnostic> check_call_graph(const ir::Program& program) {
    std::vector<Diagnostic> out;
    int n = isize(program.functions);
    auto index_of = [&program, n](const std::string& name) {
        for (int i = 0; i < n; ++i)
            if (program.functions[static_cast<size_t>(i)].name == name) return i;
        return -1;
    };

    std::vector<std::vector<int>> calls(static_cast<size_t>(n));
    for (int f = 0; f < n; ++f) {
        for (const auto& block : program.functions[static_cast<size_t>(f)].blocks) {
            for (const auto& instr : block.instrs) {
                if (instr.op != Op::Call) continue;
                int callee = index_of(instr.callee);
                auto& list = calls[static_cast<size_t>(f)];
                if (callee >= 0 && std::find(list.begin(), list.end(), callee) == list.end()) list.push_back(callee);
            }
        }
    }

    // Tarjan's algorithm: a strongly connected component is a group of
    // functions that can all reach each other, which means recursion.
    std::vector<int> number(static_cast<size_t>(n), -1), low(static_cast<size_t>(n), 0);
    std::vector<bool> on_stack(static_cast<size_t>(n), false);
    std::vector<int> stack;
    int counter = 0;
    std::function<void(int)> visit = [&](int f) {
        size_t i = static_cast<size_t>(f);
        number[i] = low[i] = counter++;
        stack.push_back(f);
        on_stack[i] = true;
        for (int callee : calls[i]) {
            size_t c = static_cast<size_t>(callee);
            if (number[c] == -1) {
                visit(callee);
                low[i] = std::min(low[i], low[c]);
            } else if (on_stack[c]) {
                low[i] = std::min(low[i], number[c]);
            }
        }
        if (low[i] != number[i]) return;
        std::vector<int> component;
        while (true) {
            int top = stack.back();
            stack.pop_back();
            on_stack[static_cast<size_t>(top)] = false;
            component.push_back(top);
            if (top == f) break;
        }
        bool self_call = std::find(calls[i].begin(), calls[i].end(), f) != calls[i].end();
        if (component.size() == 1 && !self_call) return;
        std::sort(component.begin(), component.end());
        const Function& first = program.functions[static_cast<size_t>(component[0])];
        std::string names;
        for (size_t k = 0; k < component.size(); ++k) {
            names += (k == 0 ? "" : k + 1 == component.size() ? " and " : ", ") +
                     quoted(program.functions[static_cast<size_t>(component[k])].name);
        }
        Diagnostic d = finding("info", "recursion", first.pos, first,
                               component.size() == 1 ? names + " calls itself (recursion)"
                                                     : names + " call each other (recursion)",
                               "every call needs its own stack frame, so make sure the recursion stops");
        d.subject = first.name;
        d.proof = "call graph: these functions form a strongly connected component (Tarjan's algorithm)";
        out.push_back(std::move(d));
    };
    for (int f = 0; f < n; ++f)
        if (number[static_cast<size_t>(f)] == -1) visit(f);

    // Functions that main can never reach.
    int main_fn = index_of("main");
    if (main_fn >= 0) {
        std::vector<bool> used(static_cast<size_t>(n), false);
        std::vector<int> work{main_fn};
        used[static_cast<size_t>(main_fn)] = true;
        while (!work.empty()) {
            int f = work.back();
            work.pop_back();
            for (int callee : calls[static_cast<size_t>(f)]) {
                if (used[static_cast<size_t>(callee)]) continue;
                used[static_cast<size_t>(callee)] = true;
                work.push_back(callee);
            }
        }
        for (int f = 0; f < n; ++f) {
            if (used[static_cast<size_t>(f)]) continue;
            const Function& fn = program.functions[static_cast<size_t>(f)];
            Diagnostic d = finding("warning", "unused-function", fn.pos, fn,
                                   "function " + quoted(fn.name) + " is never called",
                                   "call it from main, or remove it");
            d.subject = fn.name;
            d.proof = "call graph: no chain of calls from 'main' leads to " + quoted(fn.name);
            out.push_back(std::move(d));
        }
    }
    return out;
}

std::vector<Diagnostic> check_flow(const ir::Program& program, std::vector<CheckedOps>* checked) {
    std::vector<Diagnostic> all;
    auto add = [&all](std::vector<Diagnostic> found) {
        all.insert(all.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
    };
    for (auto check : {check_reachability, check_definite_assignment, check_dead_stores, check_call_graph})
        add(check(program));
    add(check_values(program, checked));
    return all;
}

}  // namespace parc::sem
