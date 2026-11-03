// The passes that work on values: constant propagation and folding, copy
// propagation, local value numbering, common subexpressions, dead code.

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_map>

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

long long wrap32(long long value) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(value)));
}

namespace {

int line_of(const Instr& instr) { return instr.pos.line > 0 ? instr.pos.line : instr.stmt.line; }

std::string code(const std::string& text) { return "'" + text + "'"; }

bool is_unary(Op op) { return op == Op::Neg || op == Op::Not || op == Op::IntToFloat; }

// Computes "a op b" on constants exactly as the VM would, or returns nothing
// when that is not possible (division by zero must still happen at run time).
std::optional<Operand> fold(Op op, VT type, const Operand& a, const Operand& b) {
    using K = Operand::Kind;
    if (op == Op::IntToFloat) return a.kind == K::Int ? std::optional(Operand::of_float(static_cast<double>(a.i))) : std::nullopt;
    if (op == Op::Not) return a.kind == K::Bool ? std::optional(Operand::of_bool(!a.b)) : std::nullopt;
    if (op == Op::Neg) {
        if (a.kind == K::Int) return Operand::of_int(wrap32(-a.i));
        if (a.kind == K::Float) return Operand::of_float(-a.f);
        return std::nullopt;
    }
    if (ir::is_compare(op)) {
        auto compare = [op](auto x, auto y) {
            switch (op) {
                case Op::Lt: return x < y;
                case Op::Le: return x <= y;
                case Op::Gt: return x > y;
                case Op::Ge: return x >= y;
                case Op::Eq: return x == y;
                default: return x != y;
            }
        };
        if (a.kind == K::Int && b.kind == K::Int) return Operand::of_bool(compare(a.i, b.i));
        if (a.kind == K::Float && b.kind == K::Float) return Operand::of_bool(compare(a.f, b.f));
        if (a.kind == K::Bool && b.kind == K::Bool) return Operand::of_bool(compare(a.b, b.b));
        if (a.kind == K::Null && b.kind == K::Null) return Operand::of_bool(op == Op::Eq);
        return std::nullopt;
    }
    if (type == VT::Int && a.kind == K::Int && b.kind == K::Int) {
        switch (op) {
            case Op::Add: return Operand::of_int(wrap32(a.i + b.i));
            case Op::Sub: return Operand::of_int(wrap32(a.i - b.i));
            case Op::Mul: return Operand::of_int(wrap32(a.i * b.i));
            case Op::Div:
                if (b.i == 0) return std::nullopt;
                return Operand::of_int(b.i == -1 ? wrap32(-a.i) : a.i / b.i);
            case Op::Mod:
                if (b.i == 0) return std::nullopt;
                return Operand::of_int(b.i == -1 ? 0 : a.i % b.i);
            default: return std::nullopt;
        }
    }
    if (type == VT::Float && a.kind == K::Float && b.kind == K::Float) {
        switch (op) {
            case Op::Add: return Operand::of_float(a.f + b.f);
            case Op::Sub: return Operand::of_float(a.f - b.f);
            case Op::Mul: return Operand::of_float(a.f * b.f);
            case Op::Div: return Operand::of_float(a.f / b.f);
            default: return std::nullopt;
        }
    }
    return std::nullopt;
}

void make_copy(Instr& instr, const Operand& value) {
    instr.op = Op::Copy;
    instr.a = value;
    instr.b = Operand{};
}

// ---------------------------------------------------------------- constant propagation

// What is known about one variable: nothing yet, one constant, or "it varies".
struct Lat {
    enum class K : unsigned char { Undef, Const, Varying };
    K k = K::Undef;
    Operand value;
    bool operator==(const Lat&) const = default;

    static Lat constant(const Operand& v) { return {K::Const, v}; }
    static Lat varying() { return {K::Varying, {}}; }
};

using Consts = std::vector<Lat>;

Lat lat_of(const Consts& s, const Operand& o) {
    if (o.is_var()) return s[static_cast<size_t>(o.var)];
    if (o.is_const()) return Lat::constant(o);
    return Lat::varying();
}

Lat evaluate(const Instr& instr, const Consts& s) {
    if (instr.op == Op::Copy) return lat_of(s, instr.a);
    if (!ir::is_pure(instr)) return Lat::varying();
    Lat a = lat_of(s, instr.a);
    Lat b = is_unary(instr.op) ? Lat::constant(Operand::of_int(0)) : lat_of(s, instr.b);
    auto is_zero = [](const Lat& l) { return l.k == Lat::K::Const && l.value.is_int() && l.value.i == 0; };
    if (instr.op == Op::Mul && instr.type == VT::Int && (is_zero(a) || is_zero(b)))
        return Lat::constant(Operand::of_int(0));  // 0 times anything
    if (a.k == Lat::K::Varying || b.k == Lat::K::Varying) return Lat::varying();
    if (a.k == Lat::K::Undef || b.k == Lat::K::Undef) return Lat{};
    std::optional<Operand> folded = fold(instr.op, instr.type, a.value, b.value);
    return folded ? Lat::constant(*folded) : Lat::varying();
}

void apply_constants(const Instr& instr, Consts& s) {
    Lat result = instr.dst.is_var() ? evaluate(instr, s) : Lat::varying();
    for (int v : ir::defs(instr)) s[static_cast<size_t>(v)] = instr.dst.is_var(v) ? result : Lat::varying();
}

// x + 0, x * 1, x - x and the like. Returns why it could simplify, or "".
std::string simplify(Instr& instr) {
    auto is_int = [](const Operand& o, long long v) { return o.is_int() && o.i == v; };
    auto is_float = [](const Operand& o, double v) { return o.kind == Operand::Kind::Float && o.f == v; };
    bool same = instr.a.is_var() && instr.a == instr.b;
    if (instr.type == VT::Int) {
        switch (instr.op) {
            case Op::Add:
                if (is_int(instr.b, 0)) return make_copy(instr, instr.a), "adding 0 changes nothing";
                if (is_int(instr.a, 0)) return make_copy(instr, instr.b), "adding 0 changes nothing";
                break;
            case Op::Sub:
                if (is_int(instr.b, 0)) return make_copy(instr, instr.a), "subtracting 0 changes nothing";
                if (same) return make_copy(instr, Operand::of_int(0)), "a value minus itself is 0";
                break;
            case Op::Mul:
                if (is_int(instr.b, 1)) return make_copy(instr, instr.a), "multiplying by 1 changes nothing";
                if (is_int(instr.a, 1)) return make_copy(instr, instr.b), "multiplying by 1 changes nothing";
                if (is_int(instr.a, 0) || is_int(instr.b, 0))
                    return make_copy(instr, Operand::of_int(0)), "anything times 0 is 0";
                break;
            case Op::Div:
                if (is_int(instr.b, 1)) return make_copy(instr, instr.a), "dividing by 1 changes nothing";
                break;
            case Op::Mod:
                if (is_int(instr.b, 1)) return make_copy(instr, Operand::of_int(0)), "the remainder of dividing by 1 is 0";
                break;
            default: break;
        }
    }
    if (instr.type == VT::Float && (instr.op == Op::Mul || instr.op == Op::Div) && is_float(instr.b, 1.0))
        return make_copy(instr, instr.a), "multiplying or dividing by 1.0 changes nothing";
    if (same && ir::is_compare(instr.op) && instr.type != VT::Float) {  // a float NaN is not equal to itself
        bool result = instr.op == Op::Eq || instr.op == Op::Le || instr.op == Op::Ge;
        return make_copy(instr, Operand::of_bool(result)), "a value compared with itself";
    }
    return "";
}

}  // namespace

bool constant_propagation(const ir::Program& program, Function& fn, PassRun& run) {
    Problem<Consts> p;
    p.forward = true;
    p.initial = Consts(fn.vars.size());
    p.boundary = Consts(fn.vars.size());
    for (int param : fn.params) p.boundary[static_cast<size_t>(param)] = Lat::varying();  // any value may be passed
    p.meet = [](const Consts& a, const Consts& b) {
        Consts r = a;
        for (size_t i = 0; i < r.size(); ++i) {
            if (a[i].k == Lat::K::Undef) r[i] = b[i];
            else if (b[i].k != Lat::K::Undef && !(a[i] == b[i])) r[i] = Lat::varying();
        }
        return r;
    };
    p.transfer = [&fn](int block, const Consts& in) {
        Consts s = in;
        for (const auto& instr : fn.blocks[static_cast<size_t>(block)].instrs) apply_constants(instr, s);
        return s;
    };
    Solution<Consts> solution = solve(fn, p);

    bool changed = false;
    std::vector<bool> reachable = ir::reachable_blocks(fn);
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        if (!reachable[b]) continue;
        Consts s = solution.in[b];
        for (auto& instr : fn.blocks[b].instrs) {
            Instr original = instr;
            std::string before = ir::instr_text(program, fn, instr);
            std::string why;

            // 1. A variable that is always one constant is replaced by it.
            for (Operand* o : ir::use_operands(instr)) {
                if (!o->is_var()) continue;
                const Lat& known = s[static_cast<size_t>(o->var)];
                if (known.k != Lat::K::Const) continue;
                std::string fact = ir::operand_text(program, fn, *o) + " is always " +
                                   ir::operand_text(program, fn, known.value) + " here";
                if (why.find(fact) == std::string::npos) why += (why.empty() ? "" : ", ") + fact;
                *o = known.value;
            }
            // 2. An operation on constants is done now; x + 0 and friends are simplified.
            if (ir::is_pure(instr)) {
                std::optional<Operand> folded =
                    fold(instr.op, instr.type, instr.a, is_unary(instr.op) ? Operand::of_int(0) : instr.b);
                if (folded && instr.a.is_const() && (is_unary(instr.op) || instr.b.is_const())) {
                    make_copy(instr, *folded);
                    if (why.empty()) why = "both operands are constants";
                } else {
                    std::string rule = simplify(instr);
                    if (!rule.empty()) why += (why.empty() ? "" : ", and ") + rule;
                }
            }
            // 3. A branch whose condition is known always goes one way.
            if (instr.op == Op::Branch && instr.a.kind == Operand::Kind::Bool) {
                instr.target = instr.a.b ? instr.target : instr.target2;
                instr.target2 = -1;
                instr.op = Op::Jump;
                instr.a = Operand{};
                why += (why.empty() ? "" : ", so ") + std::string("the branch always goes the same way");
            }

            std::string after = ir::instr_text(program, fn, instr);
            if (after != before) {
                run.changes.push_back({line_of(instr), code(before) + " became " + code(after) + ": " + why});
                changed = true;
            }
            apply_constants(original, s);
        }
    }
    if (changed) ir::link(fn);
    return changed;
}

// ---------------------------------------------------------------- copy propagation

namespace {

// For each variable, the variable it is currently a copy of, or -1.
struct Copies {
    bool everything = false;  // the neutral value of the meet: "all copies hold"
    std::vector<int> of;
    bool operator==(const Copies&) const = default;
};

void apply_copy(const Instr& instr, Copies& s) {
    for (int v : ir::defs(instr)) {
        s.of[static_cast<size_t>(v)] = -1;
        for (int& source : s.of)
            if (source == v) source = -1;  // the original changed, so the copy is no longer one
    }
    if (instr.op == Op::Copy && instr.dst.is_var() && instr.a.is_var() && !instr.a.is_var(instr.dst.var)) {
        int source = s.of[static_cast<size_t>(instr.a.var)];
        s.of[static_cast<size_t>(instr.dst.var)] = source >= 0 ? source : instr.a.var;
    }
}

}  // namespace

bool copy_propagation(const ir::Program& program, Function& fn, PassRun& run) {
    Problem<Copies> p;
    p.forward = true;
    p.boundary.of.assign(fn.vars.size(), -1);
    p.initial.everything = true;
    p.initial.of.assign(fn.vars.size(), -1);
    // A copy holds at a join only if it holds on every path into it.
    p.meet = [](const Copies& a, const Copies& b) {
        if (a.everything) return b;
        if (b.everything) return a;
        Copies r = a;
        for (size_t i = 0; i < r.of.size(); ++i)
            if (a.of[i] != b.of[i]) r.of[i] = -1;
        return r;
    };
    p.transfer = [&fn](int block, const Copies& in) {
        Copies s = in;
        if (s.everything) return s;
        for (const auto& instr : fn.blocks[static_cast<size_t>(block)].instrs) apply_copy(instr, s);
        return s;
    };
    Solution<Copies> solution = solve(fn, p);

    bool changed = false;
    std::vector<bool> reachable = ir::reachable_blocks(fn);
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        if (!reachable[b] || solution.in[b].everything) continue;
        Copies s = solution.in[b];
        std::vector<Instr> kept;
        for (auto& instr : fn.blocks[b].instrs) {
            Instr original = instr;
            std::string before = ir::instr_text(program, fn, instr);
            std::string why;
            for (Operand* o : ir::use_operands(instr)) {
                if (!o->is_var()) continue;
                int source = s.of[static_cast<size_t>(o->var)];
                if (source < 0) continue;
                std::string fact = fn.vars[static_cast<size_t>(o->var)].name + " is a copy of " +
                                   fn.vars[static_cast<size_t>(source)].name;
                if (why.find(fact) == std::string::npos) why += (why.empty() ? "" : ", ") + fact;
                o->var = source;
            }
            std::string after = ir::instr_text(program, fn, instr);
            bool self_copy = instr.op == Op::Copy && instr.dst.is_var() && instr.a.is_var(instr.dst.var);
            if (self_copy) {
                run.changes.push_back({line_of(instr), "removed " + code(before) + ": it would copy a value onto itself"});
                changed = true;
            } else {
                if (after != before) {
                    run.changes.push_back({line_of(instr), code(before) + " became " + code(after) + ": " + why});
                    changed = true;
                }
                kept.push_back(instr);
            }
            apply_copy(original, s);
        }
        fn.blocks[b].instrs = std::move(kept);
    }
    return changed;
}

// ---------------------------------------------------------------- local value numbering

bool local_value_numbering(const ir::Program& program, Function& fn, PassRun& run) {
    bool changed = false;
    for (auto& block : fn.blocks) {
        int next = 0;
        std::unordered_map<int, int> number_of_var;
        std::map<std::string, int> number_of_const;
        // (operator, type, number of a, number of b) -> (number of the result, variable holding it)
        std::map<std::tuple<int, int, int, int>, std::pair<int, int>> table;

        auto number = [&](const Operand& o) {
            if (o.is_var()) {
                auto [it, fresh] = number_of_var.try_emplace(o.var, next);
                if (fresh) ++next;
                return it->second;
            }
            std::string key = std::to_string(static_cast<int>(o.kind)) + ":" + ir::operand_text(program, fn, o);
            auto [it, fresh] = number_of_const.try_emplace(key, next);
            if (fresh) ++next;
            return it->second;
        };

        for (auto& instr : block.instrs) {
            if (ir::is_pure(instr) && instr.dst.is_var()) {
                int a = number(instr.a);
                int b = is_unary(instr.op) ? -1 : number(instr.b);
                bool commutative = instr.op == Op::Add || instr.op == Op::Mul || instr.op == Op::Eq || instr.op == Op::Ne;
                if (commutative && b < a) std::swap(a, b);
                auto key = std::make_tuple(static_cast<int>(instr.op), static_cast<int>(instr.type), a, b);
                auto found = table.find(key);
                if (found != table.end()) {
                    auto [value, holder] = found->second;
                    auto current = number_of_var.find(holder);
                    if (holder != instr.dst.var && current != number_of_var.end() && current->second == value) {
                        std::string before = ir::instr_text(program, fn, instr);
                        make_copy(instr, Operand::of_var(holder));
                        run.changes.push_back({line_of(instr), code(before) + " became " +
                                                                   code(ir::instr_text(program, fn, instr)) + ": " +
                                                                   fn.vars[static_cast<size_t>(holder)].name +
                                                                   " already holds this value in the same block"});
                        number_of_var[instr.dst.var] = value;
                        changed = true;
                        continue;
                    }
                }
                number_of_var[instr.dst.var] = next;
                table[key] = {next, instr.dst.var};
                ++next;
                continue;
            }
            if (instr.op == Op::Copy && instr.dst.is_var()) {
                int value = number(instr.a);  // a copy has the same value number as its source
                number_of_var[instr.dst.var] = value;
                continue;
            }
            for (int v : ir::defs(instr)) number_of_var[v] = next++;
        }
    }
    return changed;
}

// ---------------------------------------------------------------- global common subexpressions

bool common_subexpressions(const ir::Program& program, Function& fn, PassRun& run) {
    AvailableExprs info = available_expressions(fn);
    int n = isize(info.exprs);
    std::vector<bool> reachable = ir::reachable_blocks(fn);

    auto computes = [&info](const Instr& instr) {
        Expression e;
        return instr.dst.is_var() && expression_of(instr, e) ? info.find(e) : -1;
    };

    // Which expressions are computed somewhere although they are already available?
    std::vector<int> temp(static_cast<size_t>(n), -1);
    std::vector<int> first_line(static_cast<size_t>(n), 0);
    bool any = false;
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        if (!reachable[b]) continue;
        BitSet available = info.solution.in[b];
        for (const auto& instr : fn.blocks[b].instrs) {
            int k = computes(instr);
            if (k >= 0) {
                if (first_line[static_cast<size_t>(k)] == 0) first_line[static_cast<size_t>(k)] = line_of(instr);
                if (available.test(k) && temp[static_cast<size_t>(k)] < 0) {
                    temp[static_cast<size_t>(k)] = fn.add_var("cse" + std::to_string(fn.vars.size()),
                                                              fn.vars[static_cast<size_t>(instr.dst.var)].type,
                                                              ir::VarKind::Temp);
                    any = true;
                }
            }
            step_available(info, instr, available);
        }
    }
    if (!any) return false;

    // Every place that computes such an expression saves the result in the
    // expression's own temporary; every redundant place reads the temporary.
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        if (!reachable[b]) continue;
        BitSet available = info.solution.in[b];
        std::vector<Instr> out;
        for (auto& instr : fn.blocks[b].instrs) {
            Instr original = instr;
            int k = computes(instr);
            int holder = k >= 0 ? temp[static_cast<size_t>(k)] : -1;
            if (holder < 0) {
                out.push_back(instr);
            } else if (available.test(k)) {
                std::string before = ir::instr_text(program, fn, instr);
                make_copy(instr, Operand::of_var(holder));
                out.push_back(instr);
                run.changes.push_back(
                    {line_of(instr), code(before) + " became " + code(ir::instr_text(program, fn, instr)) + ": " +
                                         code(expression_text(program, fn, info.exprs[static_cast<size_t>(k)])) +
                                         " was already computed on every path to this point (first at line " +
                                         std::to_string(first_line[static_cast<size_t>(k)]) + ")"});
            } else {
                out.push_back(instr);
                Instr save;
                save.op = Op::Copy;
                save.dst = Operand::of_var(holder);
                save.a = instr.dst;
                save.stmt = instr.stmt;
                out.push_back(save);
            }
            step_available(info, original, available);
        }
        fn.blocks[b].instrs = std::move(out);
    }
    return true;
}

// ---------------------------------------------------------------- dead code elimination

bool dead_code_elimination(const ir::Program& program, Function& fn, PassRun& run) {
    ReachingDefs reach = reaching_definitions(fn);
    std::vector<bool> reachable = ir::reachable_blocks(fn);

    // Mark: instructions the outside world can notice are needed.
    std::vector<std::vector<bool>> needed(fn.blocks.size());
    std::vector<std::pair<int, int>> work;
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        needed[b].assign(fn.blocks[b].instrs.size(), !reachable[b]);  // unreachable blocks are left alone
        if (!reachable[b]) continue;
        for (size_t i = 0; i < fn.blocks[b].instrs.size(); ++i) {
            const Instr& instr = fn.blocks[b].instrs[i];
            if (ir::has_side_effect(instr) || ir::can_trap(instr)) {
                needed[b][i] = true;
                work.push_back({static_cast<int>(b), static_cast<int>(i)});
            }
        }
    }
    // ...and so is every instruction that computed one of their operands.
    while (!work.empty()) {
        auto [b, i] = work.back();
        work.pop_back();
        const Instr& instr = fn.blocks[static_cast<size_t>(b)].instrs[static_cast<size_t>(i)];
        for (int v : ir::uses(instr)) {
            for (int d : reach.reaching(fn, b, i, v)) {
                const Definition& def = reach.defs[static_cast<size_t>(d)];
                if (def.block < 0 || needed[static_cast<size_t>(def.block)][static_cast<size_t>(def.index)]) continue;
                needed[static_cast<size_t>(def.block)][static_cast<size_t>(def.index)] = true;
                work.push_back({def.block, def.index});
            }
        }
    }

    // Sweep: delete the rest.
    bool changed = false;
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        std::vector<Instr> kept;
        for (size_t i = 0; i < fn.blocks[b].instrs.size(); ++i) {
            const Instr& instr = fn.blocks[b].instrs[i];
            if (needed[b][i]) {
                kept.push_back(instr);
                continue;
            }
            run.changes.push_back({line_of(instr), "removed " + code(ir::instr_text(program, fn, instr)) +
                                                       ": nothing uses its result"});
            changed = true;
        }
        fn.blocks[b].instrs = std::move(kept);
    }
    return changed;
}

}  // namespace parc::opt
