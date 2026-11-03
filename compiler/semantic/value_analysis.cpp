// The value analysis of flow_checks.hpp: abstract interpretation over
// intervals and nullness.
//
// Instead of running the program on real numbers, the analysis runs it on
// ranges: every int variable is "somewhere in [low, high]", and every array
// variable is "an array", "null" or "maybe null". Each instruction turns the
// ranges of its operands into the range of its result, and a branch narrows
// the ranges on each of its two edges (after "if (n != 0)", n is not 0 on the
// true edge). When a range allows a dangerous value, that is a finding.
//
// A loop would make the ranges grow forever (i is [0,0], then [0,1], ...), so
// after two visits a growing bound jumps straight to the int limit. This is
// called widening. A bound that came from widening says "the analysis gave
// up", not "the program can really reach this", so overflow is not reported
// from widened bounds. A few more passes without widening (narrowing) then
// win back what the loop condition guarantees.

#include <algorithm>
#include <cstdlib>
#include <set>

#include "common/util.hpp"
#include "ir/cfg.hpp"
#include "optimizer/dataflow.hpp"
#include "semantic/flow_checks.hpp"

namespace parc::sem {
namespace {

using ir::Function;
using ir::Instr;
using ir::Op;
using ir::Operand;
using ir::VT;

constexpr long long kMin = -2147483648LL;
constexpr long long kMax = 2147483647LL;

enum class Null : unsigned char { No, Yes, Maybe };

// What is known about one variable. An int uses the range fields, an array
// uses the null and length fields; other types are not tracked.
struct Val {
    long long lo = kMin;
    long long hi = kMax;
    bool lo_widened = false;  // the bound is at the limit only because widening put it there
    bool hi_widened = false;
    bool nonzero = false;     // known not to be 0 even though the range spans 0

    Null null = Null::No;
    int null_line = 0;            // where the null comes from
    bool null_from_caller = false;
    long long len_lo = 0;
    long long len_hi = kMax;

    bool operator==(const Val&) const = default;

    bool excludes_zero() const { return nonzero || lo > 0 || hi < 0; }
    bool is_exactly(long long v) const { return lo == v && hi == v; }
};

struct State {
    bool reachable = false;
    std::vector<Val> vars;
    bool operator==(const State&) const = default;
};

Val join(const Val& a, const Val& b) {
    Val r;
    r.lo = std::min(a.lo, b.lo);
    r.lo_widened = a.lo < b.lo ? a.lo_widened : b.lo < a.lo ? b.lo_widened : (a.lo_widened && b.lo_widened);
    r.hi = std::max(a.hi, b.hi);
    r.hi_widened = a.hi > b.hi ? a.hi_widened : b.hi > a.hi ? b.hi_widened : (a.hi_widened && b.hi_widened);
    r.nonzero = a.excludes_zero() && b.excludes_zero() && r.lo <= 0 && r.hi >= 0;
    if (a.null == b.null) {
        r.null = a.null;
        r.null_line = a.null_line ? a.null_line : b.null_line;
        r.null_from_caller = a.null_line ? a.null_from_caller : b.null_from_caller;
    } else {
        r.null = Null::Maybe;
        const Val& source = a.null != Null::No ? a : b;
        r.null_line = source.null_line;
        r.null_from_caller = source.null_from_caller;
    }
    r.len_lo = std::min(a.len_lo, b.len_lo);
    r.len_hi = std::max(a.len_hi, b.len_hi);
    return r;
}

State join(const State& a, const State& b) {
    if (!a.reachable) return b;
    if (!b.reachable) return a;
    State r = a;
    for (size_t i = 0; i < r.vars.size(); ++i) r.vars[i] = join(a.vars[i], b.vars[i]);
    return r;
}

// What the analysis knows about a function from the outside: which array
// parameters callers may pass as null, and whether it may return null.
struct NullFact {
    Null null = Null::No;
    int line = 0;
    bool operator==(const NullFact&) const = default;
};

NullFact join(const NullFact& a, const NullFact& b, bool first) {
    if (first) return b;
    if (a.null == b.null) return a;
    return {Null::Maybe, a.null != Null::No ? a.line : b.line};
}

struct Summaries {
    std::vector<std::vector<NullFact>> params;  // [function][parameter]
    std::vector<NullFact> returns;              // [function]
    bool operator==(const Summaries&) const = default;
};

std::string range_text(const Val& v) { return "[" + std::to_string(v.lo) + ", " + std::to_string(v.hi) + "]"; }

std::string quoted(const std::string& text) { return "'" + text + "'"; }

class Analysis {
public:
    explicit Analysis(const ir::Program& program) : program_(program) {
        summaries_.params.resize(program.functions.size());
        summaries_.returns.resize(program.functions.size());
        for (size_t f = 0; f < program.functions.size(); ++f)
            summaries_.params[f].resize(program.functions[f].params.size());
    }

    std::vector<Diagnostic> run() {
        // Nullness crosses function boundaries: a parameter can be null only
        // if some caller passes a value that can be null. Repeat until the
        // facts about parameters and return values stop changing.
        for (int round = 0; round < 10; ++round) {
            Summaries before = summaries_;
            collected_ = Summaries{};
            collected_.params.resize(program_.functions.size());
            collected_.returns.resize(program_.functions.size());
            seen_call_.assign(program_.functions.size(), {});
            seen_return_.assign(program_.functions.size(), false);
            for (size_t f = 0; f < program_.functions.size(); ++f) {
                collected_.params[f].resize(program_.functions[f].params.size());
                seen_call_[f].assign(program_.functions[f].params.size(), false);
            }
            for (size_t f = 0; f < program_.functions.size(); ++f) analyze(static_cast<int>(f), false);
            summaries_ = collected_;
            if (summaries_ == before) break;
        }
        for (size_t f = 0; f < program_.functions.size(); ++f) analyze(static_cast<int>(f), true);
        return std::move(findings_);
    }

    // One entry per function: the operations the reporting walk looked at.
    std::vector<CheckedOps> take_checked() { return std::move(checked_); }

private:
    const ir::Program& program_;
    Summaries summaries_;   // facts used by the current round
    Summaries collected_;   // facts gathered for the next round
    std::vector<std::vector<bool>> seen_call_;
    std::vector<bool> seen_return_;
    std::vector<Diagnostic> findings_;
    const Function* fn_ = nullptr;
    int fn_index_ = 0;
    bool reporting_ = false;
    std::set<std::pair<int, int>> range_reported_;  // statements that already have an overflow report
    std::vector<CheckedOps> checked_;  // what the reporting walk counted, per function
    int worst_ = 0;                    // for the instruction being judged: 0 nothing, 1 a note, 2 a warning

    // ------------------------------------------------------------ one function

    void analyze(int index, bool report) {
        fn_ = &program_.functions[static_cast<size_t>(index)];
        fn_index_ = index;
        reporting_ = false;
        const Function& fn = *fn_;

        State entry;
        entry.reachable = true;
        entry.vars.assign(fn.vars.size(), Val{});
        for (size_t k = 0; k < fn.params.size(); ++k) {
            const NullFact& fact = summaries_.params[static_cast<size_t>(index)][k];
            Val& v = entry.vars[static_cast<size_t>(fn.params[k])];
            v.null = fact.null;
            v.null_line = fact.line;
            v.null_from_caller = true;
        }
        State unreached;
        unreached.vars.assign(fn.vars.size(), Val{});

        // A loop header is a block that a later block (in visiting order) jumps back to.
        std::vector<int> order = ir::reverse_postorder(fn);
        std::vector<int> position(fn.blocks.size(), -1);
        for (size_t i = 0; i < order.size(); ++i) position[static_cast<size_t>(order[i])] = static_cast<int>(i);
        std::set<int> headers;
        for (int b : order)
            for (int p : fn.blocks[static_cast<size_t>(b)].preds)
                if (position[static_cast<size_t>(p)] >= position[static_cast<size_t>(b)]) headers.insert(b);

        opt::Problem<State> p;
        p.forward = true;
        p.boundary = entry;
        p.initial = unreached;
        p.meet = [](const State& a, const State& b) { return join(a, b); };
        p.transfer = [this](int block, const State& in) { return run_block(block, in); };
        p.edge = [this](int from, int to, const State& out) { return refine(from, to, out); };
        p.widen = [&headers](int block, const State& before, const State& now, int visits) {
            if (!headers.count(block) || visits <= 2 || !before.reachable || !now.reachable) return now;
            State w = now;
            for (size_t i = 0; i < w.vars.size(); ++i) {
                Val& v = w.vars[i];
                const Val& old = before.vars[i];
                bool give_up = visits > 40;  // a safety net; two or three visits are normal
                if (v.lo < old.lo || give_up) {
                    v.lo = kMin;
                    v.lo_widened = true;
                }
                if (v.hi > old.hi || give_up) {
                    v.hi = kMax;
                    v.hi_widened = true;
                }
                if (v.len_lo < old.len_lo || give_up) v.len_lo = 0;
                if (v.len_hi > old.len_hi || give_up) v.len_hi = kMax;
                if (give_up) v.nonzero = false;
            }
            return w;
        };
        opt::Solution<State> solution = opt::solve(fn, p);

        // Narrowing: recompute every block from its predecessors without
        // widening. Bounds that the loop condition limits become exact again.
        for (int pass = 0; pass < 2; ++pass) {
            for (int b : order) {
                State in = b == 0 ? entry : unreached;
                for (int pred : fn.blocks[static_cast<size_t>(b)].preds) {
                    if (position[static_cast<size_t>(pred)] < 0) continue;
                    in = join(in, refine(pred, b, solution.out[static_cast<size_t>(pred)]));
                }
                solution.in[static_cast<size_t>(b)] = in;
                solution.out[static_cast<size_t>(b)] = run_block(b, in);
            }
        }

        if (!report) {
            // This walk only gathers what callers pass and what the function returns.
            for (int b : order) run_block(b, solution.in[static_cast<size_t>(b)], true);
            return;
        }
        reporting_ = true;
        range_reported_.clear();
        checked_.push_back(CheckedOps{});
        checked_.back().function = fn.name;
        for (int b : order) run_block(b, solution.in[static_cast<size_t>(b)]);
        reporting_ = false;
    }

    State run_block(int block, const State& in, bool collect = false) {
        State s = in;
        for (const auto& instr : fn_->blocks[static_cast<size_t>(block)].instrs) {
            if (!s.reachable) break;
            step(instr, s, collect);
        }
        return s;
    }

    // ------------------------------------------------------------ findings

    std::string name(const Operand& o) const { return ir::operand_text(program_, *fn_, o); }

    // The source text an instruction is about: "sum / n".
    std::string subject(const Instr& instr) const {
        return instr.expr ? ast::to_source(*instr.expr) : ir::instr_text(program_, *fn_, instr);
    }

    void report(const Instr& instr, const char* kind, const char* cwe, std::string message, std::string hint,
                std::string proof, std::string guard, const char* severity = "warning") {
        if (!reporting_) return;
        // One overflow report per statement: in "total + x + y" the first sum says it all.
        bool is_range = std::string(kind) == "overflow" || std::string(kind) == "underflow";
        if (is_range && !range_reported_.insert({instr.stmt.line, instr.stmt.col}).second) return;
        Diagnostic d;
        d.phase = "semantic";
        d.severity = severity;
        d.kind = kind;
        d.cwe = cwe;
        ir::Pos pos = instr.pos.line > 0 ? instr.pos : instr.stmt;
        d.line = pos.line;
        d.col = pos.col;
        d.stmt_line = instr.stmt.line > 0 ? instr.stmt.line : pos.line;
        d.function = fn_->name;
        d.subject = subject(instr);
        d.message = std::move(message);
        d.hint = std::move(hint);
        d.proof = std::move(proof);
        d.guard = std::move(guard);
        worst_ = std::max(worst_, d.severity == "info" ? 1 : 2);
        findings_.push_back(std::move(d));
    }

    // How an operand reads in the source. A temporary such as t2 is shown as
    // the expression it holds: a[i].
    std::string label(const Instr& instr, const Operand& o) const {
        if (instr.expr && instr.expr->kind == ast::ExprKind::Binary) {
            const auto& b = ast::as<ast::Binary>(*instr.expr);
            return ast::to_source(&o == &instr.a ? *b.lhs : *b.rhs);
        }
        if (instr.expr && instr.expr->kind == ast::ExprKind::Unary)
            return ast::to_source(*ast::as<ast::Unary>(*instr.expr).operand);
        return name(o);
    }

    // "n is in [0, 10]" for a variable, nothing for a constant.
    std::string range_fact(const Operand& o, const Val& v, const std::string& text) const {
        if (!o.is_var()) return "";
        if (v.lo == kMin && v.hi == kMax) {
            const ir::Var& var = fn_->vars[static_cast<size_t>(o.var)];
            if (var.kind == ir::VarKind::Param) return text + " is a parameter, so it can be any int";
            if (var.kind == ir::VarKind::Temp && text.find('[') != std::string::npos)
                return text + " is an array element, so it can be any int";
            return text + " can be any int";
        }
        return text + " is in " + range_text(v);
    }

    std::string range_proof(const Instr& instr, const Val& a, const Val& b, const std::string& conclusion) const {
        std::string facts = range_fact(instr.a, a, label(instr, instr.a));
        std::string second = instr.b.is_none() ? "" : range_fact(instr.b, b, label(instr, instr.b));
        if (!second.empty() && !(instr.b == instr.a)) facts += (facts.empty() ? "" : "; ") + second;
        return "interval analysis: " + facts + (facts.empty() ? "" : ", so ") + conclusion;
    }

    // ------------------------------------------------------------ operands

    Val value(const State& s, const Operand& o, int line) const {
        Val v;
        switch (o.kind) {
            case Operand::Kind::Var: return s.vars[static_cast<size_t>(o.var)];
            case Operand::Kind::Int: v.lo = v.hi = o.i; break;
            case Operand::Kind::Null:
                v.null = Null::Yes;
                v.null_line = line;
                break;
            default: break;
        }
        return v;
    }

    static void set(State& s, const Operand& dst, const Val& v) {
        if (dst.is_var()) s.vars[static_cast<size_t>(dst.var)] = v;
    }

    bool is_int(const Operand& o) const {
        if (o.is_int()) return true;
        if (!o.is_var()) return false;
        const ast::Type& t = fn_->vars[static_cast<size_t>(o.var)].type;
        return t.base == ast::BaseType::Int && !t.is_array;
    }

    // ------------------------------------------------------------ arithmetic

    // The condition under which "x + c" and friends overflow, as ParC code.
    static std::string overflow_guard(const Instr& instr, bool upper) {
        if (!instr.expr || instr.expr->kind != ast::ExprKind::Binary) return "";
        const auto& b = ast::as<ast::Binary>(*instr.expr);
        auto literal = [](const ast::Expr& e, long long& out) {
            if (e.kind != ast::ExprKind::IntLit) return false;
            out = ast::as<ast::IntLit>(e).value;
            return out > 0;
        };
        // Parentheses keep the guard right when the operand is itself an expression like a + b.
        auto text = [](const ast::Expr& e) {
            std::string t = ast::to_source(e);
            return e.kind == ast::ExprKind::Binary ? "(" + t + ")" : t;
        };
        long long c = 0;
        if (instr.op == Op::Add && upper) {
            if (literal(*b.rhs, c)) return text(*b.lhs) + " > 2147483647 - " + std::to_string(c);
            if (literal(*b.lhs, c)) return text(*b.rhs) + " > 2147483647 - " + std::to_string(c);
        }
        if (instr.op == Op::Sub && !upper && literal(*b.rhs, c))
            return text(*b.lhs) + " < -2147483647 - 1 + " + std::to_string(c);
        if (instr.op == Op::Mul) {
            const ast::Expr* other = literal(*b.rhs, c) ? b.lhs.get() : literal(*b.lhs, c) ? b.rhs.get() : nullptr;
            if (other && c > 1) {
                std::string limit = "2147483647 / " + std::to_string(c);
                return text(*other) + " > " + limit + " || " + text(*other) + " < -" + limit;
            }
        }
        return "";
    }

    // Fits a computed range into int. If it does not fit, that is an overflow
    // finding, unless the excess comes only from widening.
    Val fit(const Instr& instr, long long lo, long long hi, bool lo_widened, bool hi_widened, const Val& a,
            const Val& b) {
        Val r;
        if (lo >= kMin && hi <= kMax) {
            r.lo = lo;
            r.hi = hi;
            r.lo_widened = lo_widened;
            r.hi_widened = hi_widened;
            return r;
        }
        bool reported = false;
        // A warning needs something definite: a constant, or a value known to lie in a
        // narrow range. When both operands could be almost anything, overflow is possible
        // in principle but nothing in the code points at it, and that is only a note.
        auto definite = [](const Val& v) { return v.hi - v.lo <= (1LL << 30); };
        const char* severity = definite(a) || (!instr.b.is_none() && definite(b)) ? "warning" : "info";
        std::string what = quoted(subject(instr));
        std::string result_range = subject(instr) + " is in [" + std::to_string(lo) + ", " + std::to_string(hi) + "]";
        if (hi > kMax && !hi_widened) {
            bool always = lo > kMax;
            std::string guard = overflow_guard(instr, true);
            report(instr, "overflow", "CWE-190",
                   what + (always ? " always overflows int" : " can overflow int") + ": the result can be as large as " +
                       std::to_string(hi) + ", and the largest int is 2147483647",
                   guard.empty() ? "keep the operands small enough, or check them before this line"
                                 : "check the value first: if (" + guard + ") { ... }",
                   range_proof(instr, a, b, result_range), guard, severity);
            reported = true;
        } else if (lo < kMin && !lo_widened) {
            bool always = hi < kMin;
            std::string guard = overflow_guard(instr, false);
            report(instr, "underflow", "CWE-191",
                   what + (always ? " always underflows int" : " can underflow int") +
                       ": the result can be as small as " + std::to_string(lo) + ", and the smallest int is -2147483648",
                   guard.empty() ? "keep the operands large enough, or check them before this line"
                                 : "check the value first: if (" + guard + ") { ... }",
                   range_proof(instr, a, b, result_range), guard, severity);
            reported = true;
        }
        // The VM wraps around on overflow, so afterwards any int is possible. A bound
        // that is only reachable by wrapping around is marked as not really known, so
        // that one overflow does not set off a chain of follow-on reports.
        r.lo_widened = !reported || lo > kMin || lo_widened;
        r.hi_widened = !reported || hi < kMax || hi_widened;
        return r;
    }

    Val arithmetic(const Instr& instr, const Val& a, const Val& b) {
        switch (instr.op) {
            case Op::Add:
                return fit(instr, a.lo + b.lo, a.hi + b.hi, a.lo_widened || b.lo_widened, a.hi_widened || b.hi_widened,
                           a, b);
            case Op::Sub:
                return fit(instr, a.lo - b.hi, a.hi - b.lo, a.lo_widened || b.hi_widened, a.hi_widened || b.lo_widened,
                           a, b);
            case Op::Mul: {
                long long corners[] = {a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi};
                bool widened = a.lo_widened || a.hi_widened || b.lo_widened || b.hi_widened;
                return fit(instr, *std::min_element(corners, corners + 4), *std::max_element(corners, corners + 4),
                           widened, widened, a, b);
            }
            default: return Val{};
        }
    }

    // Division and remainder: the divisor must not be 0.
    Val divide(const Instr& instr, State& s, const Val& a, Val b) {
        std::string divisor = instr.expr && instr.expr->kind == ast::ExprKind::Binary
                                  ? ast::to_source(*ast::as<ast::Binary>(*instr.expr).rhs)
                                  : name(instr.b);
        if (b.lo <= 0 && b.hi >= 0 && !b.nonzero) {
            bool always = b.is_exactly(0);
            std::string guard = divisor + " == 0";
            report(instr, "divide-by-zero", "CWE-369",
                   always ? quoted(subject(instr)) + " always divides by zero: " + divisor + " is 0 here"
                          : quoted(subject(instr)) + " divides by zero when " + divisor + " is 0",
                   "check it first: if (" + guard + ") { ... }",
                   "interval analysis: " +
                       (instr.b.is_var() ? range_fact(instr.b, b, divisor) : divisor + " is " + std::to_string(b.lo)) +
                       ", which includes 0, and no check on the way here rules 0 out",
                   guard);
            if (always) {
                s.reachable = false;  // the program stops here
                return Val{};
            }
        }
        // If execution continues, the divisor was not 0.
        b.nonzero = true;
        if (b.lo == 0) b.lo = 1;
        if (b.hi == 0) b.hi = -1;
        if (instr.b.is_var()) s.vars[static_cast<size_t>(instr.b.var)] = b;

        Val r;
        if (instr.op == Op::Mod) {
            long long m = std::max(std::llabs(b.lo), std::llabs(b.hi)) - 1;
            r.lo = a.lo >= 0 ? 0 : std::max(-m, a.lo);
            r.hi = a.hi <= 0 ? 0 : std::min(m, a.hi);
            return r;
        }
        // The extremes of a / b are at the ends of a's range and at the
        // divisors closest to zero and furthest from it.
        long long lo = kMax, hi = kMin;
        for (long long d : {b.lo, b.hi, -1LL, 1LL}) {
            if (d == 0 || d < b.lo || d > b.hi) continue;
            for (long long n : {a.lo, a.hi}) {
                lo = std::min(lo, n / d);
                hi = std::max(hi, n / d);
            }
        }
        if (lo > hi || lo < kMin || hi > kMax) return r;  // includes -2147483648 / -1, which wraps
        r.lo = lo;
        r.hi = hi;
        r.lo_widened = r.hi_widened = a.lo_widened || a.hi_widened;
        return r;
    }

    // ------------------------------------------------------------ arrays

    // Reports an access through an array that is or may be null.
    // Returns false when the program certainly stops here.
    bool check_null(const Instr& instr, State& s, const Operand& array) {
        Val v = value(s, array, instr.stmt.line);
        if (v.null == Null::No) return true;
        std::string a = name(array);
        std::string origin = v.null_from_caller
                                 ? "a caller passes null for " + quoted(a) + " (line " + std::to_string(v.null_line) + ")"
                                 : quoted(a) + " holds null from line " + std::to_string(v.null_line);
        std::string guard = a + " == null";
        bool always = v.null == Null::Yes;
        report(instr, "null-access", "CWE-476",
               always ? quoted(a) + " is null here, so " + quoted(subject(instr)) + " stops the program"
                      : quoted(a) + " can be null here, so " + quoted(subject(instr)) + " may stop the program",
               "check it first: if (" + guard + ") { ... }",
               "nullness analysis: " + origin + (always ? "" : ", and no check on the way here rules null out"), guard);
        if (always) {
            s.reachable = false;
            return false;
        }
        if (array.is_var()) s.vars[static_cast<size_t>(array.var)].null = Null::No;  // it was not null if we continue
        return true;
    }

    bool check_index(const Instr& instr, State& s, const Operand& array, const Operand& index) {
        Val idx = value(s, index, instr.stmt.line);
        Val arr = value(s, array, instr.stmt.line);
        const ast::Index* source = instr.expr && instr.expr->kind == ast::ExprKind::Index
                                       ? &ast::as<ast::Index>(*instr.expr)
                                       : nullptr;
        std::string i = source ? ast::to_source(*source->index) : name(index);
        std::string a = source ? ast::to_source(*source->array) : name(array);
        std::string guard = i + " < 0 || " + i + " >= len(" + a + ")";
        std::string hint = "check the index first: if (" + guard + ") { ... }";
        const char* cwe = instr.op == Op::Store ? "CWE-787" : "CWE-125";
        // The end of the array is only judged when its length is one known number;
        // "between 1 and 100 elements" would give false alarms for a loop up to that length.
        bool length_known = arr.len_lo == arr.len_hi;
        std::string index_fact = index.is_var() ? range_fact(index, idx, i) : i + " is " + std::to_string(idx.lo);
        std::string length_fact = arr.len_lo == arr.len_hi
                                      ? a + " has " + std::to_string(arr.len_lo) + " elements"
                                      : a + " has between " + std::to_string(arr.len_lo) + " and " +
                                            std::to_string(arr.len_hi) + " elements";

        if (idx.hi < 0) {
            report(instr, "index-out-of-bounds", cwe, "the index in " + quoted(subject(instr)) + " is always negative",
                   hint, "interval analysis: " + index_fact, guard);
            s.reachable = false;
            return false;
        }
        if (length_known && idx.lo >= arr.len_hi) {
            report(instr, "index-out-of-bounds", cwe,
                   "the index in " + quoted(subject(instr)) + " is always past the end of the array", hint,
                   "interval analysis: " + index_fact + ", and " + length_fact, guard);
            s.reachable = false;
            return false;
        }
        if (idx.lo < 0 && !idx.lo_widened) {
            report(instr, "index-out-of-bounds", cwe,
                   "the index in " + quoted(subject(instr)) + " can be negative (as low as " + std::to_string(idx.lo) + ")",
                   hint, "interval analysis: " + index_fact, guard);
        } else if (length_known && idx.hi >= arr.len_lo && !idx.hi_widened) {
            report(instr, "index-out-of-bounds", cwe,
                   "the index in " + quoted(subject(instr)) + " can reach " + std::to_string(idx.hi) +
                       ", past the end of the array",
                   hint, "interval analysis: " + index_fact + ", and " + length_fact, guard);
        }
        // If execution continues, the index was inside the array.
        if (index.is_var()) {
            Val& v = s.vars[static_cast<size_t>(index.var)];
            v.lo = std::max(v.lo, 0LL);
            if (length_known) v.hi = std::min(v.hi, arr.len_hi - 1);
        }
        return true;
    }

    // ------------------------------------------------------------ one instruction

    // Which count an instruction belongs to, if it is an operation that can fail at run time.
    OpCount* counted(const Instr& instr) {
        CheckedOps& c = checked_.back();
        switch (instr.op) {
            case Op::Add:
            case Op::Sub:
            case Op::Mul:
            case Op::Neg:
                // Only arithmetic the programmer wrote. The "i = i + 1" the compiler
                // adds to a for loop has no expression in the tree and is not counted.
                return instr.type == VT::Int && instr.expr ? &c.arithmetic : nullptr;
            case Op::Div:
            case Op::Mod: return instr.type == VT::Int ? &c.division : nullptr;
            case Op::Load:
            case Op::Store:
            case Op::Len: return &c.access;
            case Op::NewArray: return &c.allocation;
            default: return nullptr;
        }
    }

    // Judges one instruction. During the reporting walk it also counts it:
    // checked, and at risk or possibly at risk if a finding was reported for it.
    void step(const Instr& instr, State& s, bool collect) {
        OpCount* count = reporting_ ? counted(instr) : nullptr;
        worst_ = 0;
        apply(instr, s, collect);
        if (!count) return;
        ++count->checked;
        if (worst_ == 2) ++count->at_risk;
        if (worst_ == 1) ++count->possible;
    }

    void apply(const Instr& instr, State& s, bool collect) {
        int line = instr.stmt.line;
        switch (instr.op) {
            case Op::Copy: set(s, instr.dst, value(s, instr.a, line)); break;
            case Op::Add:
            case Op::Sub:
            case Op::Mul:
                set(s, instr.dst,
                    instr.type == VT::Int ? arithmetic(instr, value(s, instr.a, line), value(s, instr.b, line)) : Val{});
                break;
            case Op::Div:
            case Op::Mod: {
                if (instr.type != VT::Int) {
                    set(s, instr.dst, Val{});
                    break;
                }
                Val r = divide(instr, s, value(s, instr.a, line), value(s, instr.b, line));
                if (s.reachable) set(s, instr.dst, r);
                break;
            }
            case Op::Neg: {
                if (instr.type != VT::Int) {
                    set(s, instr.dst, Val{});
                    break;
                }
                Val a = value(s, instr.a, line);
                set(s, instr.dst, fit(instr, -a.hi, -a.lo, a.hi_widened, a.lo_widened, a, Val{}));
                break;
            }
            case Op::NewArray: {
                Val n = value(s, instr.a, line);
                std::string size = instr.expr && instr.expr->kind == ast::ExprKind::NewArray
                                       ? ast::to_source(*ast::as<ast::NewArray>(*instr.expr).size)
                                       : name(instr.a);
                if (n.hi < 0 || (n.lo < 0 && !n.lo_widened)) {
                    std::string guard = size + " < 0";
                    report(instr, "negative-size", "CWE-131",
                           "the size in " + quoted(subject(instr)) + (n.hi < 0 ? " is always negative" : " can be negative"),
                           "check it first: if (" + guard + ") { ... }",
                           "interval analysis: " + (instr.a.is_var() ? range_fact(instr.a, n, size)
                                                                     : size + " is " + std::to_string(n.lo)),
                           guard);
                    if (n.hi < 0) {
                        s.reachable = false;
                        break;
                    }
                }
                Val r;
                r.len_lo = std::max(0LL, n.lo);
                r.len_hi = std::max(0LL, n.hi);
                set(s, instr.dst, r);
                break;
            }
            case Op::Len: {
                if (!check_null(instr, s, instr.a)) break;
                Val a = value(s, instr.a, line);
                Val r;
                r.lo = a.len_lo;
                r.hi = a.len_hi;
                set(s, instr.dst, r);
                break;
            }
            case Op::Load:
            case Op::Store:
                if (!check_null(instr, s, instr.a)) break;
                if (!check_index(instr, s, instr.a, instr.b)) break;
                set(s, instr.dst, Val{});  // an element can hold any value
                break;
            case Op::Call: {
                int callee = -1;
                for (size_t f = 0; f < program_.functions.size(); ++f)
                    if (program_.functions[f].name == instr.callee) callee = static_cast<int>(f);
                Val result;
                if (callee >= 0) {
                    size_t c = static_cast<size_t>(callee);
                    if (collect) {
                        for (size_t k = 0; k < instr.args.size() && k < collected_.params[c].size(); ++k) {
                            Val arg = value(s, instr.args[k], line);
                            NullFact fact{arg.null, arg.null == Null::No ? 0 : line};
                            collected_.params[c][k] = join(collected_.params[c][k], fact, !seen_call_[c][k]);
                            seen_call_[c][k] = true;
                        }
                    }
                    result.null = summaries_.returns[c].null;
                    result.null_line = line;
                }
                set(s, instr.dst, result);
                break;
            }
            case Op::Return:
                if (collect && fn_->return_type.is_array) {
                    size_t f = static_cast<size_t>(fn_index_);
                    Val v = value(s, instr.a, line);
                    collected_.returns[f] = join(collected_.returns[f], NullFact{v.null, line}, !seen_return_[f]);
                    seen_return_[f] = true;
                }
                break;
            case Op::ParFor: {
                // Each thread runs a part of the range: its loop variable
                // starts somewhere in it, and its end is at most the full end.
                Val i = value(s, instr.a, line);
                Val end = value(s, instr.b, line);
                Val thread_i;
                thread_i.lo = i.lo;
                thread_i.hi = std::max(i.lo, end.hi);
                Val thread_end;
                thread_end.lo = std::min(i.lo, end.lo);
                thread_end.hi = end.hi;
                set(s, instr.a, thread_i);
                set(s, instr.b, thread_end);
                [[fallthrough]];
            }
            case Op::ParEnd:
                for (const auto& r : instr.reductions) {
                    Val unknown;  // an accumulated value: not bounded by the analysis
                    unknown.lo_widened = unknown.hi_widened = true;
                    s.vars[static_cast<size_t>(r.var)] = unknown;
                }
                break;
            default:
                set(s, instr.dst, Val{});  // comparisons, float conversions: not tracked
                break;
        }
    }

    // ------------------------------------------------------------ branches

    static Op negated(Op op) {
        switch (op) {
            case Op::Lt: return Op::Ge;
            case Op::Le: return Op::Gt;
            case Op::Gt: return Op::Le;
            case Op::Ge: return Op::Lt;
            case Op::Eq: return Op::Ne;
            default: return Op::Eq;
        }
    }

    // If `operand` is a temporary computed in this block as x + c or x - c, a
    // range for the temporary gives a range for x. (This assumes the addition
    // did not wrap around; if it can, that has its own overflow finding.)
    void through_offset(const std::vector<Instr>& instrs, int before, const Operand& operand, const Val& range,
                        State& s) const {
        if (!operand.is_var() || fn_->vars[static_cast<size_t>(operand.var)].kind != ir::VarKind::Temp) return;
        for (int i = before - 1; i >= 0; --i) {
            const Instr& def = instrs[static_cast<size_t>(i)];
            if (!def.dst.is_var(operand.var)) continue;
            if (def.type != VT::Int || (def.op != Op::Add && def.op != Op::Sub)) return;
            const Operand* x = def.a.is_var() && def.b.is_int() ? &def.a
                               : def.op == Op::Add && def.b.is_var() && def.a.is_int() ? &def.b
                                                                                       : nullptr;
            if (!x) return;
            long long c = x == &def.a ? def.b.i : def.a.i;
            if (def.op == Op::Sub) c = -c;  // temp = x + c
            // x must still hold the value it had when the temporary was computed.
            for (size_t later = static_cast<size_t>(i) + 1; later < instrs.size(); ++later)
                for (int v : ir::defs(instrs[later]))
                    if (v == x->var) return;
            Val& v = s.vars[static_cast<size_t>(x->var)];
            if (range.lo - c > v.lo && range.lo > kMin) {
                v.lo = range.lo - c;
                v.lo_widened = range.lo_widened;
            }
            if (range.hi - c < v.hi && range.hi < kMax) {
                v.hi = range.hi - c;
                v.hi_widened = range.hi_widened;
            }
            if (v.lo > v.hi) s.reachable = false;
            return;
        }
    }

    // Narrows the state along the edge from -> to, using the comparison the branch tests.
    State refine(int from, int to, const State& out) const {
        if (!out.reachable) return out;
        const auto& instrs = fn_->blocks[static_cast<size_t>(from)].instrs;
        const Instr& branch = instrs.back();
        if (branch.op != Op::Branch || !branch.a.is_var() || branch.target == branch.target2) return out;

        int def = -1;
        for (int i = isize(instrs) - 2; i >= 0 && def < 0; --i)
            if (instrs[static_cast<size_t>(i)].dst.is_var(branch.a.var)) def = i;
        if (def < 0 || !ir::is_compare(instrs[static_cast<size_t>(def)].op)) return out;
        const Instr& cmp = instrs[static_cast<size_t>(def)];
        // The compared values must still be the ones in the state at the block's end.
        for (int i = def + 1; i < isize(instrs) - 1; ++i)
            for (int v : ir::defs(instrs[static_cast<size_t>(i)]))
                if (cmp.a.is_var(v) || cmp.b.is_var(v)) return out;

        State s = out;
        Op op = to == branch.target ? cmp.op : negated(cmp.op);
        int line = cmp.stmt.line;
        if (cmp.type == VT::Int) {
            Val a = value(s, cmp.a, line);
            Val b = value(s, cmp.b, line);
            auto lower = [](Val& v, long long bound, bool widened) {  // v >= bound
                if (bound > v.lo) {
                    v.lo = bound;
                    v.lo_widened = widened;
                }
            };
            auto upper = [](Val& v, long long bound, bool widened) {  // v <= bound
                if (bound < v.hi) {
                    v.hi = bound;
                    v.hi_widened = widened;
                }
            };
            switch (op) {
                case Op::Lt:
                    upper(a, b.hi - 1, b.hi_widened);
                    lower(b, a.lo + 1, a.lo_widened);
                    break;
                case Op::Le:
                    upper(a, b.hi, b.hi_widened);
                    lower(b, a.lo, a.lo_widened);
                    break;
                case Op::Gt:
                    lower(a, b.lo + 1, b.lo_widened);
                    upper(b, a.hi - 1, a.hi_widened);
                    break;
                case Op::Ge:
                    lower(a, b.lo, b.lo_widened);
                    upper(b, a.hi, a.hi_widened);
                    break;
                case Op::Eq: {
                    Val both = a;
                    lower(both, b.lo, b.lo_widened);
                    upper(both, b.hi, b.hi_widened);
                    both.nonzero = a.nonzero || b.nonzero;
                    a = b = both;
                    break;
                }
                default:  // Ne: only a single excluded value can be used
                    for (auto [v, other] : {std::pair<Val*, Val*>{&a, &b}, std::pair<Val*, Val*>{&b, &a}}) {
                        if (other->lo != other->hi) continue;
                        if (v->lo == other->lo) ++v->lo;
                        if (v->hi == other->lo) --v->hi;
                        if (other->lo == 0) v->nonzero = true;
                    }
                    break;
            }
            for (Val* v : {&a, &b}) {
                if (v->nonzero && v->lo == 0) v->lo = 1;
                if (v->nonzero && v->hi == 0) v->hi = -1;
                if (v->lo > v->hi) s.reachable = false;  // this edge can't be taken
            }
            if (cmp.a.is_var()) s.vars[static_cast<size_t>(cmp.a.var)] = a;
            if (cmp.b.is_var()) s.vars[static_cast<size_t>(cmp.b.var)] = b;
            // "if (n - 1 < 0)" compares a temporary, but it says something about n.
            if (s.reachable) {
                through_offset(instrs, def, cmp.a, a, s);
                through_offset(instrs, def, cmp.b, b, s);
            }
        } else if (cmp.type == VT::Ref && (op == Op::Eq || op == Op::Ne)) {
            const Operand* var = cmp.a.is_var() && cmp.b.kind == Operand::Kind::Null   ? &cmp.a
                                 : cmp.b.is_var() && cmp.a.kind == Operand::Kind::Null ? &cmp.b
                                                                                       : nullptr;
            if (var) {
                Val& v = s.vars[static_cast<size_t>(var->var)];
                if (op == Op::Eq) {
                    if (v.null == Null::No) v.null_line = line;
                    v.null = Null::Yes;
                } else {
                    v.null = Null::No;
                }
            }
        }
        return s;
    }
};

}  // namespace

std::vector<Diagnostic> check_values(const ir::Program& program, std::vector<CheckedOps>* checked) {
    Analysis analysis(program);
    std::vector<Diagnostic> findings = analysis.run();
    if (checked) *checked = analysis.take_checked();
    return findings;
}

}  // namespace parc::sem
