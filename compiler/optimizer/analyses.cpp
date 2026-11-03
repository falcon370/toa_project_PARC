#include "optimizer/analyses.hpp"

#include <algorithm>

#include "common/json_writer.hpp"

namespace parc::opt {

using ir::Function;
using ir::Instr;
using ir::Op;

// ---------------------------------------------------------------- live variables

Solution<BitSet> liveness(const Function& fn) {
    int n = isize(fn.vars);
    Problem<BitSet> p;
    p.forward = false;
    p.boundary = BitSet(n);  // nothing is live after a return
    p.initial = BitSet(n);
    p.meet = [](const BitSet& a, const BitSet& b) {
        BitSet out = a;
        out.unite(b);
        return out;
    };
    p.transfer = [&fn](int block, const BitSet& live_out) {
        BitSet live = live_out;
        const auto& instrs = fn.blocks[static_cast<size_t>(block)].instrs;
        for (auto it = instrs.rbegin(); it != instrs.rend(); ++it) {
            for (int v : ir::defs(*it)) live.reset(v);
            for (int v : ir::uses(*it)) live.set(v);
        }
        return live;
    };
    return solve(fn, p);
}

// ---------------------------------------------------------------- reaching definitions

ReachingDefs reaching_definitions(const Function& fn) {
    ReachingDefs r;
    r.of_var.assign(fn.vars.size(), {});
    auto add = [&r](int block, int index, int var) {
        r.of_var[static_cast<size_t>(var)].push_back(isize(r.defs));
        r.defs.push_back({block, index, var});
    };
    for (int param : fn.params) add(-1, -1, param);
    for (int b = 0; b < isize(fn.blocks); ++b) {
        const auto& instrs = fn.blocks[static_cast<size_t>(b)].instrs;
        for (int i = 0; i < isize(instrs); ++i)
            for (int v : ir::defs(instrs[static_cast<size_t>(i)])) add(b, i, v);
    }

    int n = isize(r.defs);
    // GEN and KILL of each block, the classic formulation: OUT = GEN | (IN - KILL).
    // A definition kills every other definition of its variable, and only the
    // last definition of a variable in a block is still in GEN at its end.
    std::vector<BitSet> gen(fn.blocks.size(), BitSet(n));
    std::vector<BitSet> kill(fn.blocks.size(), BitSet(n));
    int d = isize(fn.params);  // definitions are numbered in the order they were added above
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        for (const auto& instr : fn.blocks[b].instrs) {
            for (int v : ir::defs(instr)) {
                for (int other : r.of_var[static_cast<size_t>(v)]) {
                    kill[b].set(other);
                    gen[b].reset(other);
                }
                gen[b].set(d++);
            }
        }
    }

    Problem<BitSet> p;
    p.forward = true;
    p.boundary = BitSet(n);
    for (int k = 0; k < isize(fn.params); ++k) p.boundary.set(k);  // parameters arrive defined
    p.initial = BitSet(n);
    p.meet = [](const BitSet& a, const BitSet& b) {
        BitSet out = a;
        out.unite(b);
        return out;
    };
    p.transfer = [&gen, &kill](int block, const BitSet& in) {
        BitSet out = in;
        out.subtract(kill[static_cast<size_t>(block)]);
        out.unite(gen[static_cast<size_t>(block)]);
        return out;
    };
    r.solution = solve(fn, p);
    return r;
}

std::vector<int> ReachingDefs::reaching(const Function& fn, int block, int index, int var) const {
    // Inside the block, the nearest earlier definition of the variable wins.
    const auto& instrs = fn.blocks[static_cast<size_t>(block)].instrs;
    for (int i = index - 1; i >= 0; --i) {
        for (int v : ir::defs(instrs[static_cast<size_t>(i)])) {
            if (v != var) continue;
            for (int d : of_var[static_cast<size_t>(var)]) {
                const Definition& def = defs[static_cast<size_t>(d)];
                if (def.block == block && def.index == i) return {d};
            }
        }
    }
    std::vector<int> out;
    const BitSet& in = solution.in[static_cast<size_t>(block)];
    for (int d : of_var[static_cast<size_t>(var)])
        if (in.test(d)) out.push_back(d);
    return out;
}

// ---------------------------------------------------------------- available expressions

namespace {

// A fixed order for the operands of a commutative operator.
bool operand_less(const ir::Operand& a, const ir::Operand& b) {
    if (a.kind != b.kind) return static_cast<int>(a.kind) < static_cast<int>(b.kind);
    if (a.kind == ir::Operand::Kind::Var) return a.var < b.var;
    if (a.kind == ir::Operand::Kind::Float) return a.f < b.f;
    if (a.kind == ir::Operand::Kind::Bool) return a.b < b.b;
    return a.i < b.i;
}

bool mentions(const Expression& e, int var) { return e.a.is_var(var) || e.b.is_var(var); }

}  // namespace

bool expression_of(const Instr& instr, Expression& out) {
    if (!ir::is_pure(instr)) return false;
    out.op = instr.op;
    out.type = instr.type;
    out.a = instr.a;
    out.b = instr.b;
    bool commutative = instr.op == Op::Add || instr.op == Op::Mul || instr.op == Op::Eq || instr.op == Op::Ne;
    if (commutative && operand_less(out.b, out.a)) std::swap(out.a, out.b);
    return true;
}

int AvailableExprs::find(const Expression& e) const {
    auto it = std::find(exprs.begin(), exprs.end(), e);
    return it == exprs.end() ? -1 : static_cast<int>(it - exprs.begin());
}

void step_available(const AvailableExprs& info, const Instr& instr, BitSet& available) {
    Expression e;
    int computed = expression_of(instr, e) ? info.find(e) : -1;
    if (computed >= 0) available.set(computed);
    // Writing a variable ends the availability of every expression that reads
    // it, including the one just computed in "x = x + 1".
    for (int v : ir::defs(instr))
        for (int i = 0; i < isize(info.exprs); ++i)
            if (mentions(info.exprs[static_cast<size_t>(i)], v)) available.reset(i);
}

AvailableExprs available_expressions(const Function& fn) {
    AvailableExprs info;
    for (const auto& block : fn.blocks) {
        for (const auto& instr : block.instrs) {
            Expression e;
            if (expression_of(instr, e) && info.find(e) < 0) info.exprs.push_back(e);
        }
    }
    int n = isize(info.exprs);
    Problem<BitSet> p;
    p.forward = true;
    p.boundary = BitSet(n);       // nothing is computed yet at the entry
    p.initial = BitSet(n, true);  // "everything", the neutral value of intersection
    p.meet = [](const BitSet& a, const BitSet& b) {
        BitSet out = a;
        out.intersect(b);
        return out;
    };
    p.transfer = [&fn, &info](int block, const BitSet& in) {
        BitSet available = in;
        for (const auto& instr : fn.blocks[static_cast<size_t>(block)].instrs) step_available(info, instr, available);
        return available;
    };
    info.solution = solve(fn, p);
    return info;
}

std::string expression_text(const ir::Program& program, const Function& fn, const Expression& e) {
    std::string a = ir::operand_text(program, fn, e.a);
    if (e.op == Op::Neg || e.op == Op::Not) return std::string(ir::op_symbol(e.op)) + a;
    if (e.op == Op::IntToFloat) return "float(" + a + ")";
    return a + " " + std::string(ir::op_symbol(e.op)) + " " + ir::operand_text(program, fn, e.b);
}

// ---------------------------------------------------------------- JSON

std::string dataflow_json(const ir::Program& program, std::string_view filename) {
    json::Writer w;
    auto names = [&w](const std::vector<std::string>& items) {
        w.begin_array();
        for (const auto& item : items) w.string(item);
        w.end_array();
    };

    w.begin_object();
    w.key("file");
    w.string(filename);
    w.key("functions");
    w.begin_array();
    for (const auto& fn : program.functions) {
        Solution<BitSet> live = liveness(fn);
        ReachingDefs reach = reaching_definitions(fn);
        AvailableExprs avail = available_expressions(fn);
        std::vector<bool> reachable = ir::reachable_blocks(fn);

        auto var_names = [&fn](const BitSet& set) {
            std::vector<std::string> out;
            for (int v : set.members()) out.push_back(fn.vars[static_cast<size_t>(v)].name);
            return out;
        };
        // A definition is shown as "d3: x (line 5)".
        auto def_name = [&](int d) {
            const Definition& def = reach.defs[static_cast<size_t>(d)];
            std::string text = "d" + std::to_string(d + 1) + ": " + fn.vars[static_cast<size_t>(def.var)].name;
            if (def.block < 0) return text + " (parameter)";
            int line = fn.blocks[static_cast<size_t>(def.block)].instrs[static_cast<size_t>(def.index)].stmt.line;
            return line > 0 ? text + " (line " + std::to_string(line) + ")" : text;
        };
        auto def_names = [&](const BitSet& set) {
            std::vector<std::string> out;
            for (int d : set.members()) out.push_back(def_name(d));
            return out;
        };
        auto expr_names = [&](const BitSet& set) {
            std::vector<std::string> out;
            for (int e : set.members()) out.push_back(expression_text(program, fn, avail.exprs[static_cast<size_t>(e)]));
            return out;
        };

        w.begin_object();
        w.key("name");
        w.string(fn.name);
        w.key("definitions");
        w.begin_array();
        for (int d = 0; d < isize(reach.defs); ++d) w.string(def_name(d));
        w.end_array();
        w.key("expressions");
        w.begin_array();
        for (const auto& e : avail.exprs) w.string(expression_text(program, fn, e));
        w.end_array();
        w.key("solver_visits");
        w.begin_object();
        w.key("live");
        w.integer(live.visits);
        w.key("reaching");
        w.integer(reach.solution.visits);
        w.key("available");
        w.integer(avail.solution.visits);
        w.end_object();
        w.key("blocks");
        w.begin_array();
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            if (!reachable[b]) continue;
            w.begin_object();
            w.key("id");
            w.integer(static_cast<long long>(b));
            w.key("label");
            w.string(fn.blocks[b].label);
            w.key("live_in");
            names(var_names(live.in[b]));
            w.key("live_out");
            names(var_names(live.out[b]));
            w.key("reach_in");
            names(def_names(reach.solution.in[b]));
            w.key("reach_out");
            names(def_names(reach.solution.out[b]));
            w.key("avail_in");
            names(expr_names(avail.solution.in[b]));
            w.key("avail_out");
            names(expr_names(avail.solution.out[b]));
            w.end_object();
        }
        w.end_array();
        w.end_object();
    }
    w.end_array();
    w.end_object();
    return w.str() + "\n";
}

}  // namespace parc::opt
