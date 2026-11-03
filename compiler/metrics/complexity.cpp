#include "metrics/complexity.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>

namespace parc::metrics {

// ---------------------------------------------------------------- BigO

namespace {

constexpr double kEps = 1e-9;

void clean(Term& t) {
    std::erase_if(t.power, [](const auto& p) { return p.second < kEps; });
    std::erase_if(t.logs, [](const auto& p) { return p.second <= 0; });
}

bool term_constant(const Term& t) { return t.power.empty() && t.logs.empty() && t.exp.empty(); }

// Does a grow at least as fast as b?
bool term_ge(const Term& a, const Term& b) {
    if (!a.exp.empty() && b.exp.empty()) return true;
    if (a.exp != b.exp) return false;
    std::set<std::string> sizes;
    for (const auto& [s, p] : a.power) sizes.insert(s);
    for (const auto& [s, p] : b.power) sizes.insert(s);
    for (const auto& [s, l] : a.logs) sizes.insert(s);
    for (const auto& [s, l] : b.logs) sizes.insert(s);
    for (const auto& s : sizes) {
        double pa = a.power.count(s) ? a.power.at(s) : 0.0;
        double pb = b.power.count(s) ? b.power.at(s) : 0.0;
        int la = a.logs.count(s) ? a.logs.at(s) : 0;
        int lb = b.logs.count(s) ? b.logs.at(s) : 0;
        if (pa > pb + kEps) continue;
        if (std::abs(pa - pb) < kEps && la >= lb) continue;
        return false;
    }
    return true;
}

BigO simplify(std::vector<Term> terms) {
    for (auto& t : terms) clean(t);
    BigO out;
    for (size_t i = 0; i < terms.size(); ++i) {
        bool dropped = false;
        for (size_t j = 0; j < terms.size() && !dropped; ++j) {
            if (i == j || !term_ge(terms[j], terms[i])) continue;
            // Equal terms: keep the first one only.
            dropped = !term_ge(terms[i], terms[j]) || j < i;
        }
        if (!dropped) out.terms.push_back(terms[i]);
    }
    return out;
}

Term multiply(const Term& a, const Term& b) {
    Term t = a;
    for (const auto& [s, p] : b.power) t.power[s] += p;
    for (const auto& [s, l] : b.logs) t.logs[s] += l;
    if (t.exp.empty()) t.exp = b.exp;
    else if (!b.exp.empty() && b.exp != t.exp) t.exp = t.exp + " + " + b.exp;
    return t;
}

std::string power_text(const std::string& s, double p) {
    if (std::abs(p - 1.0) < kEps) return s;
    if (std::abs(p - 0.5) < kEps) return "sqrt(" + s + ")";
    if (std::abs(p - 1.5) < kEps) return s + " sqrt(" + s + ")";
    if (std::abs(p - std::round(p)) < kEps) return s + "^" + std::to_string(static_cast<int>(std::round(p)));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", p);
    return s + "^" + buf;
}

std::string term_text(const Term& t) {
    std::vector<std::string> parts;
    if (!t.exp.empty()) parts.push_back(t.exp.find(' ') == std::string::npos ? "2^" + t.exp : "2^(" + t.exp + ")");
    std::set<std::string> sizes;
    for (const auto& [s, p] : t.power) sizes.insert(s);
    for (const auto& [s, l] : t.logs) sizes.insert(s);
    for (const auto& s : sizes) {
        std::string part;
        if (t.power.count(s)) part = power_text(s, t.power.at(s));
        if (t.logs.count(s)) {
            int l = t.logs.at(s);
            part += std::string(part.empty() ? "" : " ") + (l == 1 ? "log " : "log^" + std::to_string(l) + " ") + s;
        }
        parts.push_back(part);
    }
    if (parts.empty()) return "1";
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : " * ") + p;
    return out;
}

}  // namespace

BigO BigO::one() {
    BigO b;
    b.terms.push_back({});
    return b;
}

BigO BigO::of(const std::string& size) {
    BigO b;
    Term t;
    t.power[size] = 1.0;
    b.terms.push_back(t);
    return b;
}

bool BigO::constant() const {
    return std::all_of(terms.begin(), terms.end(), term_constant);
}

BigO BigO::plus(const BigO& other) const {
    if (is_zero()) return other;
    if (other.is_zero()) return *this;
    std::vector<Term> all = terms;
    all.insert(all.end(), other.terms.begin(), other.terms.end());
    return simplify(std::move(all));
}

BigO BigO::times(const BigO& other) const {
    if (is_zero() || other.is_zero()) return zero();
    std::vector<Term> all;
    for (const auto& a : terms)
        for (const auto& b : other.terms) all.push_back(multiply(a, b));
    return simplify(std::move(all));
}

BigO BigO::log() const {
    if (constant()) return is_zero() ? zero() : one();
    std::vector<Term> all;
    for (const auto& t : terms) {
        if (!t.exp.empty()) {
            Term e;
            e.power[t.exp] = 1.0;
            all.push_back(e);
        }
        for (const auto& [s, p] : t.power) {
            Term l;
            l.logs[s] = 1;
            all.push_back(l);
        }
    }
    return all.empty() ? one() : simplify(std::move(all));
}

BigO BigO::sqrt() const { return pow(0.5); }

BigO BigO::pow(double p) const {
    if (constant()) return *this;
    std::vector<Term> all;
    for (Term t : terms) {
        for (auto& [s, q] : t.power) q *= p;
        for (auto& [s, l] : t.logs) l = std::max(1, static_cast<int>(std::lround(l * p)));
        all.push_back(t);
    }
    return simplify(std::move(all));
}

BigO BigO::over(const BigO& divisor) const {
    if (divisor.terms.empty()) return *this;
    const Term& d = divisor.terms.front();
    std::vector<Term> all;
    for (Term t : terms) {
        for (const auto& [s, p] : d.power)
            if (t.power.count(s)) t.power[s] = std::max(0.0, t.power[s] - p);
        for (const auto& [s, l] : d.logs)
            if (t.logs.count(s)) t.logs[s] = std::max(0, t.logs[s] - l);
        if (!d.exp.empty() && t.exp == d.exp) t.exp.clear();
        all.push_back(t);
    }
    return simplify(std::move(all));
}

BigO BigO::substitute(const std::map<std::string, BigO>& sizes) const {
    BigO out = zero();
    for (const Term& t : terms) {
        Term rest;
        BigO factor = one();
        for (const auto& [s, p] : t.power) {
            auto found = sizes.find(s);
            if (found == sizes.end()) rest.power[s] = p;
            else factor = factor.times(found->second.pow(p));
        }
        for (const auto& [s, l] : t.logs) {
            auto found = sizes.find(s);
            if (found == sizes.end()) {
                rest.logs[s] = l;
            } else {
                for (int k = 0; k < l; ++k) factor = factor.times(found->second.log());
            }
        }
        if (!t.exp.empty()) {
            auto found = sizes.find(t.exp);
            if (found == sizes.end()) {
                rest.exp = t.exp;
            } else if (!found->second.constant()) {
                // 2^n with n replaced: 2^len(a), or 2^(...) for anything longer.
                rest.exp = found->second.inner();
            }
        }
        BigO term;
        term.terms.push_back(rest);
        out = out.plus(term.times(factor));
    }
    return out;
}

double BigO::degree(const std::string& size) const {
    double d = 0.0;
    for (const auto& t : terms)
        if (t.power.count(size)) d = std::max(d, t.power.at(size));
    return d;
}

std::string BigO::main_size() const {
    std::string best;
    double best_power = 0.0;
    for (const auto& t : terms)
        for (const auto& [s, p] : t.power)
            if (p > best_power + kEps) {
                best = s;
                best_power = p;
            }
    return best;
}

bool BigO::grows_faster_than(const BigO& other) const {
    auto covered = [](const BigO& small, const BigO& big) {
        return std::all_of(small.terms.begin(), small.terms.end(), [&big](const Term& s) {
            return std::any_of(big.terms.begin(), big.terms.end(), [&s](const Term& b) { return term_ge(b, s); });
        });
    };
    BigO a = is_zero() ? one() : *this;
    BigO b = other.is_zero() ? one() : other;
    return covered(b, a) && !covered(a, b);
}

std::string BigO::inner() const {
    if (constant()) return "1";
    std::vector<std::string> parts;
    for (const auto& t : terms)
        if (!term_constant(t)) parts.push_back(term_text(t));
    std::sort(parts.begin(), parts.end());  // the same text whatever order the terms were found in
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : " + ") + p;
    return out;
}

std::string BigO::str() const { return "O(" + inner() + ")"; }

// ---------------------------------------------------------------- the analysis

namespace {

bool is_comparison(const std::string& op) {
    return op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" || op == "!=";
}

bool is_alloc(const std::string& name) {
    return name == "new_int" || name == "new_float" || name == "new_bool" || name == "malloc" || name == "calloc" ||
           name == "realloc";
}

bool is_output(const std::string& name) {
    return name == "printf" || name == "puts" || name == "putchar" || name == "fprintf";
}

// Skips parentheses and casts, which do not change what an expression counts.
const CNode& strip(const CNode& n) {
    const CNode* p = &n;
    while ((p->kind == "ParenExpr" || p->kind == "CStyleCastExpr" || p->kind == "UnexposedExpr") && !p->children.empty())
        p = &p->children.back();
    return *p;
}

bool is_ref_to(const CNode& n, unsigned decl) {
    const CNode& s = strip(n);
    return s.kind == "DeclRefExpr" && s.decl == decl && decl != 0;
}

bool mentions(const CNode& n, unsigned decl) {
    if (n.kind == "DeclRefExpr" && n.decl == decl) return true;
    return std::any_of(n.children.begin(), n.children.end(), [decl](const CNode& c) { return mentions(c, decl); });
}

bool contains_op(const CNode& n, const std::string& op) {
    if ((n.kind == "BinaryOperator" || n.kind == "CompoundAssignOperator") && n.op == op) return true;
    return std::any_of(n.children.begin(), n.children.end(), [&op](const CNode& c) { return contains_op(c, op); });
}

void refs_in(const CNode& n, std::vector<unsigned>& out) {
    if (n.kind == "DeclRefExpr" && !n.function && n.decl && std::find(out.begin(), out.end(), n.decl) == out.end())
        out.push_back(n.decl);
    for (const auto& c : n.children) refs_in(c, out);
}

// The variable an assignment target or array access is about: v, a[i] -> a, a->d[i] -> a.
unsigned base_decl(const CNode& n) {
    const CNode& s = strip(n);
    if (s.kind == "DeclRefExpr") return s.decl;
    if ((s.kind == "ArraySubscriptExpr" || s.kind == "MemberRefExpr") && !s.children.empty()) return base_decl(s.children[0]);
    return 0;
}

bool is_assignment(const CNode& n) {
    return (n.kind == "BinaryOperator" && n.op == "=") || n.kind == "CompoundAssignOperator" ||
           (n.kind == "UnaryOperator" && (n.op == "++" || n.op == "--"));
}

// Every variable a piece of code assigns, and every array it writes into.
void assigned_in(const CNode& n, std::set<unsigned>& out) {
    if (is_assignment(n) && !n.children.empty()) {
        const CNode& target = strip(n.children[0]);
        if (unsigned d = base_decl(target)) out.insert(d);
    }
    for (const auto& c : n.children) assigned_in(c, out);
}

struct Summary {
    BigO time = BigO::one();
    BigO heap = BigO::zero();
    BigO stack = BigO::zero();
    bool pure = true;
    std::vector<std::string> param_sizes;  // the size each parameter stands for ("" for none)
    bool in_progress = false;
    bool done = false;
};

class Analyzer;

// Changes to the program a suggestion makes, for working out the better bound.
struct Rewrite {
    const CNode* loop = nullptr;
    bool loop_once = false;   // the loop's work is done once instead
    bool loop_sqrt = false;   // the loop runs sqrt as many times
    const CNode* call = nullptr;   // this call is made once, before the loops
    const CNode* alloc = nullptr;  // this array is allocated once, before the loops
    BigO extra_time = BigO::zero();
    BigO extra_heap = BigO::zero();
};

struct Work {
    BigO time = BigO::one();
    BigO heap = BigO::zero();
    void add(const Work& w) {
        time = time.plus(w.time);
        heap = heap.plus(w.heap);
    }
};

struct Step {
    enum class Kind { None, Add, Mul } kind = Kind::None;
    unsigned var = 0;
    bool constant = true;    // Add: by a constant
    unsigned by = 0;         // Add: by this variable
};

struct Trip {
    BigO trips;
    BigO range;              // how far the loop variable goes, for loops inside this one
    unsigned var = 0;
    std::string note;
    const CNode* start = nullptr;
    const CNode* bound = nullptr;
    bool sqrt = false;
    Step step;
};

struct ActiveLoop {
    const CNode* node;
    unsigned var;
    BigO trips;
    BigO range;
    std::set<unsigned> modified;
};

struct Candidate {
    Suggestion s;
    std::unique_ptr<Rewrite> rewrite;  // the change, analyzed again for the better bounds
    BigO time_better;                  // without a rewrite: the better bounds, already known
    BigO space_better;
};

BigO two_to(const std::string& n) {
    BigO b;
    Term t;
    t.exp = n;
    b.terms.push_back(t);
    return b;
}

class FunctionWalk {
public:
    FunctionWalk(Analyzer& an, const CUnit& unit, const CFunction& fn, std::string raw_name, const Rewrite* rewrite,
                 FunctionComplexity* report)
        : an_(an), unit_(unit), fn_(fn), raw_name_(std::move(raw_name)), rewrite_(rewrite), report_(report) {}

    Summary run();

private:
    Analyzer& an_;
    const CUnit& unit_;
    const CFunction& fn_;
    std::string raw_name_;
    const Rewrite* rewrite_;
    FunctionComplexity* report_;  // null while working out a suggestion's bound

    struct DeclInfo {
        std::string name;
        bool param = false;
        std::string size;                     // parameters: the size they stand for
        const CNode* init = nullptr;
        std::vector<const CNode*> values;     // the init and every value assigned with =
    };
    std::map<unsigned, DeclInfo> decls_;
    std::vector<ActiveLoop> active_;
    struct SelfCall {
        const CNode* call;
        bool in_loop;
    };
    std::vector<SelfCall> self_calls_;
    BigO stack_ = BigO::zero();
    bool pure_ = true;
    int nesting_ = 0;
    const CNode* init_of_ = nullptr;  // the initializer of the variable being declared
    std::vector<Candidate> candidates_;

    std::string display(const std::string& name) const { return unit_.parc ? parc_names(name) : name; }

    void collect(const CNode& n) {
        if (n.kind == "VarDecl") {
            DeclInfo& d = decls_[n.decl];
            d.name = display(n.name);
            if (!n.children.empty()) {
                d.init = &n.children.back();
                d.values.push_back(d.init);
            }
        }
        if (n.kind == "BinaryOperator" && n.op == "=" && n.children.size() == 2) {
            const CNode& target = strip(n.children[0]);
            if (target.kind == "DeclRefExpr") decls_[target.decl].values.push_back(&n.children[1]);
        }
        for (const auto& c : n.children) collect(c);
    }

    std::string param_size(const CNode& p) const {
        const std::string& t = p.type;
        std::string name = display(p.name);
        if (t.find('*') != std::string::npos || t.find('[') != std::string::npos) return "len(" + name + ")";
        if (t == "double" || t == "float" || t == "bool" || t == "_Bool") return "";
        return name;
    }

    const ActiveLoop* loop_of(unsigned var) const {
        for (const auto& l : active_)
            if (l.var == var) return &l;
        return nullptr;
    }

    // ------------------------------------------------------------ sizes

    BigO size_of_decl(unsigned decl, int depth) {
        if (const ActiveLoop* l = loop_of(decl)) return l->range;
        auto found = decls_.find(decl);
        if (found == decls_.end()) return BigO::of("?");
        const DeclInfo& d = found->second;
        if (d.param) return d.size.empty() ? BigO::one() : BigO::of(d.size);
        if (d.init && depth < 8) return size_of(*d.init, depth + 1);
        return BigO::of(d.name);
    }

    BigO array_size(const CNode& base, int depth) {
        const CNode* p = &strip(base);
        while (p->kind == "MemberRefExpr" && !p->children.empty()) p = &strip(p->children[0]);  // a->d is a
        const CNode& b = *p;
        if (b.kind == "DeclRefExpr") {
            auto found = decls_.find(b.decl);
            if (found != decls_.end()) {
                const DeclInfo& d = found->second;
                if (d.param) return BigO::of(d.size.empty() ? "len(" + d.name + ")" : d.size);
                if (d.init && depth < 8) {
                    const CNode& init = strip(*d.init);
                    if (init.kind == "CallExpr" && is_alloc(init.name) && !init.children.empty())
                        return size_of(init.children[0], depth + 1);
                }
                return BigO::of("len(" + d.name + ")");
            }
        }
        return BigO::of("len(" + b.text + ")");
    }

    // How big the value of an expression can get, as a size.
    BigO size_of(const CNode& e, int depth = 0) {
        const CNode& n = strip(e);
        if (depth > 8) return BigO::of(n.text.empty() ? "?" : n.text);
        const std::string& k = n.kind;
        if (k == "IntegerLiteral" || k == "FloatingLiteral" || k == "CharacterLiteral" || k == "UnaryExpr")
            return BigO::one();
        if (k == "DeclRefExpr") return n.function ? BigO::of(n.text) : size_of_decl(n.decl, depth);
        if (k == "MemberRefExpr" && unit_.parc && n.name == "n" && !n.children.empty())
            return array_size(n.children[0], depth);
        if (k == "BinaryOperator" && n.children.size() == 2) {
            const CNode& l = n.children[0];
            const CNode& r = n.children[1];
            if (n.op == "+" || n.op == "-") return size_of(l, depth).plus(size_of(r, depth));
            if (n.op == "*") return size_of(l, depth).times(size_of(r, depth));
            if (n.op == "/" || n.op == ">>") return size_of(l, depth);
            if (n.op == "%") return size_of(r, depth);
            if (is_comparison(n.op) || n.op == "&&" || n.op == "||") return BigO::one();
        }
        if (k == "UnaryOperator" && !n.children.empty()) {
            if (n.op == "!") return BigO::one();
            if (n.op == "-" || n.op == "+") return size_of(n.children[0], depth);
        }
        if (k == "ConditionalOperator" && n.children.size() == 3)
            return size_of(n.children[1], depth).plus(size_of(n.children[2], depth));
        return BigO::of(n.text.empty() ? "?" : n.text);
    }

    // ------------------------------------------------------------ loops

    bool halving(unsigned decl) const {
        auto found = decls_.find(decl);
        if (found == decls_.end()) return false;
        return std::any_of(found->second.values.begin(), found->second.values.end(),
                           [](const CNode* v) { return contains_op(*v, "/") || contains_op(*v, ">>"); });
    }

    Step rhs_step(const CNode& value, unsigned var) const {
        Step s;
        s.var = var;
        const CNode& r = strip(value);
        if (r.kind == "BinaryOperator" && r.children.size() == 2) {
            const CNode& a = strip(r.children[0]);
            const CNode& b = strip(r.children[1]);
            if (r.op == "+" || r.op == "-") {
                const CNode* other = is_ref_to(a, var) ? &b : (r.op == "+" && is_ref_to(b, var)) ? &a : nullptr;
                if (other) {
                    s.kind = Step::Kind::Add;
                    s.constant = other->kind == "IntegerLiteral";
                    if (other->kind == "DeclRefExpr") s.by = other->decl;
                    return s;
                }
                for (const CNode* side : {&a, &b})
                    if (side->kind == "DeclRefExpr" && side->decl != var && halving(side->decl)) {
                        s.kind = Step::Kind::Mul;
                        return s;
                    }
            }
            if ((r.op == "*" || r.op == "/" || r.op == ">>" || r.op == "<<") && is_ref_to(a, var)) {
                s.kind = Step::Kind::Mul;
                return s;
            }
        }
        if (r.kind == "DeclRefExpr" && r.decl != var && halving(r.decl)) s.kind = Step::Kind::Mul;
        return s;
    }

    Step step_of(const CNode& node) const {
        const CNode& n = strip(node);
        Step s;
        if (n.kind == "UnaryOperator" && (n.op == "++" || n.op == "--") && !n.children.empty()) {
            const CNode& v = strip(n.children[0]);
            if (v.kind == "DeclRefExpr") {
                s.kind = Step::Kind::Add;
                s.var = v.decl;
            }
        } else if (n.kind == "CompoundAssignOperator" && n.children.size() == 2) {
            const CNode& v = strip(n.children[0]);
            if (v.kind != "DeclRefExpr") return s;
            s.var = v.decl;
            const CNode& by = strip(n.children[1]);
            if (n.op == "+=" || n.op == "-=") {
                s.kind = Step::Kind::Add;
                s.constant = by.kind == "IntegerLiteral";
                if (by.kind == "DeclRefExpr") s.by = by.decl;
            } else if (n.op == "*=" || n.op == "/=" || n.op == ">>=" || n.op == "<<=") {
                s.kind = Step::Kind::Mul;
            }
        } else if (n.kind == "BinaryOperator" && n.op == "=" && n.children.size() == 2) {
            const CNode& v = strip(n.children[0]);
            if (v.kind == "DeclRefExpr") return rhs_step(n.children[1], v.decl);
        } else if (n.kind == "BinaryOperator" && n.op == ",") {
            for (const auto& c : n.children) {
                Step inner = step_of(c);
                if (inner.kind != Step::Kind::None) return inner;
            }
        }
        return s;
    }

    // The first change of `var` inside a piece of code that has a known step.
    Step find_step(const CNode& n, unsigned var) const {
        if (is_assignment(n) && !n.children.empty() && is_ref_to(n.children[0], var)) {
            Step s = step_of(n);
            if (s.kind != Step::Kind::None) return s;
        }
        for (const auto& c : n.children) {
            Step s = find_step(c, var);
            if (s.kind != Step::Kind::None) return s;
        }
        return {};
    }

    // Finds "var < bound" (or bound > var, var * var <= bound, var + 1 < bound) in a condition.
    const CNode* compared_with(const CNode& cond, unsigned var, bool& sqrt) const {
        const CNode& c = strip(cond);
        if (c.kind != "BinaryOperator" || c.children.size() != 2) return nullptr;
        if (c.op == "&&" || c.op == "||") {
            if (const CNode* found = compared_with(c.children[0], var, sqrt)) return found;
            return compared_with(c.children[1], var, sqrt);
        }
        if (!is_comparison(c.op)) return nullptr;
        auto side = [var, &sqrt](const CNode& x) {
            const CNode& s = strip(x);
            if (is_ref_to(s, var)) return true;
            if (s.kind == "BinaryOperator" && s.children.size() == 2) {
                if (s.op == "*" && is_ref_to(s.children[0], var) && is_ref_to(s.children[1], var)) {
                    sqrt = true;
                    return true;
                }
                if ((s.op == "+" || s.op == "-") && is_ref_to(s.children[0], var) &&
                    strip(s.children[1]).kind == "IntegerLiteral")
                    return true;
            }
            return false;
        };
        if (side(c.children[0])) return &c.children[1];
        if (side(c.children[1])) return &c.children[0];
        return nullptr;
    }

    Trip unknown(const std::string& why) {
        Trip t;
        t.trips = BigO::of("?");
        t.range = BigO::of("?");
        t.note = why;
        return t;
    }

    Trip trip_from(const CNode* cond, const Step& step, const CNode* start) {
        bool sqrt = false;
        const CNode* bound = cond ? compared_with(*cond, step.var, sqrt) : nullptr;
        if (!bound) return unknown("the condition does not compare the loop variable with a bound");
        Trip t;
        t.var = step.var;
        t.start = start;
        t.bound = bound;
        t.sqrt = sqrt;
        t.step = step;
        BigO base = (start ? size_of(*start) : size_of_decl(step.var, 0)).plus(size_of(*bound));
        t.range = base;
        if (step.kind == Step::Kind::Mul) {
            t.trips = base.log();
            t.note = "the variable is multiplied or divided each pass, so the loop runs a logarithmic number of times";
        } else if (!step.constant && step.by && loop_of(step.by)) {
            t.trips = base.log();
            t.note = "the step is the variable of an enclosing loop: n/1 + n/2 + n/3 + ... is a harmonic sum, n log n in total";
        } else if (!step.constant) {
            t.trips = base;
            t.note = "the step is not a constant; counted as if it were 1 (an upper bound)";
        } else {
            t.trips = sqrt ? base.sqrt() : base;
            if (sqrt) t.note = "the condition compares i * i with the bound, so the loop runs a square root of it";
        }
        if (base.constant()) {
            t.trips = BigO::one();  // a fixed number of passes
            t.note.clear();
        }
        return t;
    }

    static bool is_condition(const CNode& n) {
        const CNode& s = strip(n);
        return s.kind == "BinaryOperator" && (is_comparison(s.op) || s.op == "&&" || s.op == "||");
    }

    Trip for_trip(const CNode& loop) {
        const auto& c = loop.children;
        if (c.size() < 2) return unknown("the loop has no condition");
        const CNode* init = nullptr;
        const CNode* cond = nullptr;
        const CNode* inc = nullptr;
        for (size_t i = 0; i + 1 < c.size(); ++i) {
            if (c[i].kind == "DeclStmt") init = &c[i];
            else if (is_condition(c[i])) cond = &c[i];
            else if (!cond && !init) init = &c[i];
            else inc = &c[i];
        }
        Step step = inc ? step_of(*inc) : Step{};
        if (step.kind == Step::Kind::None) return unknown("the loop variable does not change by a simple step");
        const CNode* start = nullptr;
        if (init && init->kind == "DeclStmt") {
            for (const auto& v : init->children)
                if (v.kind == "VarDecl" && v.decl == step.var && !v.children.empty()) start = &v.children.back();
        } else if (init) {
            const CNode& a = strip(*init);
            if (a.kind == "BinaryOperator" && a.op == "=" && a.children.size() == 2 && is_ref_to(a.children[0], step.var))
                start = &a.children[1];
        }
        return trip_from(cond, step, start);
    }

    Trip while_trip(const CNode& cond, const CNode& body) {
        const CNode& c = strip(cond);
        if (c.kind == "IntegerLiteral" || (c.kind == "DeclRefExpr" && c.function))
            return unknown("the condition is always true; the loop ends at a break or return");
        std::vector<unsigned> vars;
        refs_in(cond, vars);
        for (unsigned v : vars) {
            Step s = find_step(body, v);
            if (s.kind == Step::Kind::None) continue;
            Trip t = trip_from(&cond, s, nullptr);
            if (!t.trips.is_zero() && t.var) return t;
        }
        // A loop on a flag, like bubble sort's "while (swapped)": if each pass
        // takes one off a size, it cannot run more often than that size.
        const CNode& b = strip(body);
        for (const auto& stmt : b.children) {
            if (!is_assignment(stmt) || stmt.children.empty()) continue;
            Step s = step_of(stmt);
            if (s.kind != Step::Kind::Add || !s.constant) continue;
            BigO size = size_of_decl(s.var, 0);
            if (size.constant()) continue;
            Trip t;
            t.trips = size;
            t.range = size;
            t.note = "assumed: each pass changes " + decls_[s.var].name + " by one, so the loop runs at most " +
                     size.inner() + " times";
            return t;
        }
        return unknown("the analysis could not tell how many times this loop runs");
    }

    // ------------------------------------------------------------ the walk

    Work walk(const CNode& n) {
        const std::string& k = n.kind;
        if (k == "ForStmt") return walk_loop(n, for_trip(n), "for");
        if (k == "WhileStmt" && n.children.size() == 2) return walk_loop(n, while_trip(n.children[0], n.children[1]), "while");
        if (k == "DoStmt" && n.children.size() == 2) return walk_loop(n, while_trip(n.children[1], n.children[0]), "do");
        if (k == "CallExpr") return walk_call(n);
        if (k == "IfStmt") check_sort(n);
        if (is_assignment(n) && !n.children.empty() && strip(n.children[0]).kind == "ArraySubscriptExpr") pure_ = false;
        Work w;
        if (k == "VarDecl" && !n.children.empty()) {
            init_of_ = &n.children.back();
            w.add(walk(n.children.back()));
            init_of_ = nullptr;
            return w;
        }
        for (const auto& c : n.children) w.add(walk(c));
        return w;
    }

    Work walk_loop(const CNode& n, Trip t, const char* kind) {
        if (rewrite_ && rewrite_->loop == &n) {
            if (rewrite_->loop_once) t.trips = BigO::one();
            if (rewrite_->loop_sqrt) t.trips = t.trips.sqrt();
        }
        if (report_) {
            report_->loops.push_back({n.line, kind, static_cast<int>(active_.size()) + 1, t.trips.str(), t.note});
            check_prefix_sums(n, t);
            check_trial_division(n, t);
        }
        ActiveLoop loop{&n, t.var, t.trips, t.range, {}};
        assigned_in(n, loop.modified);
        if (t.var) loop.modified.insert(t.var);
        active_.push_back(std::move(loop));
        nesting_ = std::max(nesting_, static_cast<int>(active_.size()));
        Work inner;
        for (const auto& c : n.children) inner.add(walk(c));
        active_.pop_back();
        Work out;
        out.time = t.trips.times(inner.time);
        out.heap = t.trips.times(inner.heap);
        return out;
    }

    // The product of the counts of every active loop except the innermost.
    BigO outer_trips() const {
        BigO p = BigO::one();
        for (size_t i = 0; i + 1 < active_.size(); ++i) p = p.times(active_[i].trips);
        return p;
    }

    Work walk_call(const CNode& n) {
        Work w;
        const CNode* init_of = init_of_;
        init_of_ = nullptr;
        for (const auto& a : n.children) w.add(walk(a));
        const std::string& name = n.name;
        if (is_alloc(name)) {
            pure_ = false;
            BigO size = n.children.empty() ? BigO::one() : size_of(n.children[0]);
            if (name == "calloc" && n.children.size() == 2) size = size.times(size_of(n.children[1]));
            if (name == "realloc" && n.children.size() == 2) size = size_of(n.children[1]);
            if (name != "malloc") w.time = w.time.plus(size);  // the elements are set to zero
            // Moved before the loop: still cleared on every pass, but allocated once.
            if (rewrite_ && rewrite_->alloc == &n) return w;
            w.heap = w.heap.plus(size);
            if (report_ && !active_.empty() && init_of == &n) check_alloc_in_loop(n, size);
            return w;
        }
        if (is_output(name)) {
            pure_ = false;
            return w;
        }
        if (name == raw_name_) {
            self_calls_.push_back({&n, !active_.empty()});
            return w;
        }
        Summary* callee = callee_summary(name);
        if (!callee) return w;  // a library function: counted as one step
        if (callee->in_progress) {
            if (report_) report_->notes.push_back("calls " + display(name) + ", which calls back here (mutual recursion): its cost is shown as ?");
            if (report_) report_->recursion = "mutual";
            w.time = w.time.plus(BigO::of("?"));
            pure_ = false;
            return w;
        }
        std::map<std::string, BigO> sizes;
        for (size_t i = 0; i < callee->param_sizes.size() && i < n.children.size(); ++i) {
            const std::string& s = callee->param_sizes[i];
            if (s.empty()) continue;
            sizes["#" + std::to_string(i)] = s.rfind("len(", 0) == 0 ? array_size(n.children[i], 0) : size_of(n.children[i]);
        }
        // Rename the callee's sizes first, so a caller variable with the same name is not replaced twice.
        std::map<std::string, BigO> rename;
        for (size_t i = 0; i < callee->param_sizes.size(); ++i)
            if (!callee->param_sizes[i].empty()) rename[callee->param_sizes[i]] = BigO::of("#" + std::to_string(i));
        BigO time = callee->time.substitute(rename).substitute(sizes);
        BigO heap = callee->heap.substitute(rename).substitute(sizes);
        BigO stack = callee->stack.substitute(rename).substitute(sizes);
        if (!callee->pure) pure_ = false;
        if (!(rewrite_ && rewrite_->call == &n)) w.time = w.time.plus(time);
        w.heap = w.heap.plus(heap);
        stack_ = stack_.plus(stack);
        if (report_ && !active_.empty() && callee->pure && !time.constant()) check_invariant_call(n, time);
        return w;
    }

    Summary* callee_summary(const std::string& raw_name);

    // ------------------------------------------------------------ recursion

    enum class Shape { Same, Minus, Divide, Other };

    Shape arg_shape(const CNode& arg, unsigned param, double& divisor) const {
        const CNode& a = strip(arg);
        if (is_ref_to(a, param)) return Shape::Same;
        if (a.kind == "BinaryOperator" && a.children.size() == 2) {
            const CNode& l = strip(a.children[0]);
            const CNode& r = strip(a.children[1]);
            if ((a.op == "-" || a.op == "+") && is_ref_to(l, param) && r.kind == "IntegerLiteral") return Shape::Minus;
            if (a.op == "/" && is_ref_to(l, param) && r.kind == "IntegerLiteral" && r.value > 1) {
                divisor = static_cast<double>(r.value);
                return Shape::Divide;
            }
            if (a.op == ">>" && is_ref_to(l, param) && r.kind == "IntegerLiteral" && r.value > 0) {
                divisor = std::pow(2.0, static_cast<double>(r.value));
                return Shape::Divide;
            }
            if ((a.op == "+" || a.op == "-") && l.kind == "DeclRefExpr" && halving(l.decl)) return Shape::Divide;
        }
        if (a.kind == "DeclRefExpr" && halving(a.decl)) return Shape::Divide;
        return Shape::Other;
    }

    // Works out the cost of a recursive function from the cost of one call (`rest`).
    void solve_recursion(BigO& time, FunctionComplexity* report) {
        if (self_calls_.empty()) return;
        int calls = static_cast<int>(self_calls_.size());
        bool in_loop = std::any_of(self_calls_.begin(), self_calls_.end(), [](const SelfCall& c) { return c.in_loop; });
        Shape shape = Shape::Same;
        size_t varying = 0;
        double divisor = 2.0;
        bool mixed = false;
        for (const auto& call : self_calls_) {
            Shape call_shape = Shape::Same;
            for (size_t i = 0; i < fn_.params.size() && i < call.call->children.size(); ++i) {
                Shape s = arg_shape(call.call->children[i], fn_.params[i].decl, divisor);
                if (s == Shape::Same) continue;
                if (call_shape == Shape::Same) {
                    call_shape = s;
                    if (shape == Shape::Same) varying = i;
                }
            }
            if (shape == Shape::Same) shape = call_shape;
            else if (call_shape != shape && call_shape != Shape::Same) mixed = true;
        }
        // The size the recursion works down: the parameter that changes, or an array's length.
        std::string n;
        if (varying < fn_.params.size()) n = param_size(fn_.params[varying]);
        if (shape == Shape::Divide)
            for (const auto& p : fn_.params)
                if (param_size(p).rfind("len(", 0) == 0) n = param_size(p);
        if (n.empty()) n = "n";
        BigO size = BigO::of(n);
        std::string kind;
        if (mixed || shape == Shape::Other || shape == Shape::Same) {
            time = time.times(size);
            stack_ = stack_.plus(size);
            kind = "unknown";
            if (report) report->notes.push_back("the recursive calls do not follow n - c or n / c; counted as linear");
        } else if (shape == Shape::Minus) {
            stack_ = stack_.plus(size);
            if (calls == 1 && !in_loop) {
                time = time.times(size);
                kind = "linear";
            } else {
                time = time.times(two_to(n));
                kind = "exponential";
                if (report)
                    report->notes.push_back(in_loop ? "a recursive call inside a loop: at least exponential"
                                                    : std::to_string(calls) + " recursive calls on n - c each time: the calls form a tree of about 2^n nodes");
            }
        } else {
            // The master theorem for T(n) = a T(n / b) + f(n).
            if (n.rfind("len(", 0) == 0) {
                // sort(a, lo, hi): the index parameters span a stretch of the array, so they count as its length.
                std::map<std::string, BigO> spans;
                for (const auto& p : fn_.params) {
                    std::string s = param_size(p);
                    if (!s.empty() && s.rfind("len(", 0) != 0) spans[s] = BigO::of(n);
                }
                time = time.substitute(spans);
            }
            double a = calls;
            double critical = std::log(a) / std::log(divisor);
            double d = time.degree(n);
            stack_ = stack_.plus(size.log());
            if (critical < d - 1e-6) {
                kind = "divide and conquer";
            } else if (std::abs(critical - d) < 1e-6) {
                time = time.times(size.log());
                kind = calls == 1 ? "logarithmic" : "divide and conquer";
            } else {
                time = time.over(BigO::of(n).pow(d)).times(size.pow(critical));
                kind = "divide and conquer";
            }
            if (report) {
                char buf[160];
                std::snprintf(buf, sizeof buf, "T(n) = %d T(n/%g) + O(n^%g): the master theorem gives the bound", calls,
                              divisor, d);
                report->notes.push_back(buf);
            }
        }
        if (report) report->recursion = kind;
        if (report && shape == Shape::Minus && !mixed) suggest_recursion(calls, in_loop, n, time);
    }

    // ------------------------------------------------------------ suggestions

    void add(Suggestion s, std::unique_ptr<Rewrite> r, BigO time_better = {}, BigO space_better = {}) {
        for (const auto& c : candidates_)
            if (c.s.kind == s.kind && c.s.line == s.line) return;
        candidates_.push_back({std::move(s), std::move(r), std::move(time_better), std::move(space_better)});
    }

    // The heap part of the space stays; only the recursion's stack goes away.
    void suggest_recursion(int calls, bool in_loop, const std::string& n, const BigO& time) {
        if (in_loop) return;
        Suggestion s;
        s.line = fn_.line;
        BigO size = BigO::of(n);
        if (calls >= 2) {
            s.kind = "exponential-recursion";
            s.title = "compute the values bottom-up instead of recursing";
            s.why = display(fn_.name) + " calls itself " + std::to_string(calls) +
                    " times with smaller arguments, so the same values are computed again and again";
            s.how = "Use a loop from the smallest case up to " + n +
                    ", keeping only the last few results in variables (as fib_loop does in examples/09_fibonacci.parc), "
                    "or remember each result in an array (memoization).";
            add(std::move(s), nullptr, time.over(two_to(n)).times(size), BigO::one());
        } else {
            s.kind = "linear-recursion";
            s.title = "turn the recursion into a loop";
            s.why = "each call waits for the next one, so " + n + " calls are on the stack at the deepest point";
            s.how = "Carry the partial result in a variable and loop down from " + n + " (for example result = result * i).";
            add(std::move(s), nullptr, time, BigO::one());
        }
    }

    // Bubble sort and its relatives: two loops, comparing two elements of one array and swapping them.
    void check_sort(const CNode& n) {
        if (!report_ || active_.size() < 2 || n.children.size() < 2) return;
        const CNode& cond = strip(n.children[0]);
        if (cond.kind != "BinaryOperator" || cond.children.size() != 2) return;
        if (cond.op != "<" && cond.op != ">" && cond.op != "<=" && cond.op != ">=") return;
        const CNode& l = strip(cond.children[0]);
        const CNode& r = strip(cond.children[1]);
        if (l.kind != "ArraySubscriptExpr" || r.kind != "ArraySubscriptExpr") return;
        unsigned array = base_decl(l);
        if (!array || base_decl(r) != array) return;
        int writes = 0;
        std::vector<const CNode*> todo = {&n.children[1]};
        while (!todo.empty()) {
            const CNode* c = todo.back();
            todo.pop_back();
            if (is_assignment(*c) && !c->children.empty() && strip(c->children[0]).kind == "ArraySubscriptExpr" &&
                base_decl(c->children[0]) == array)
                ++writes;
            for (const auto& k : c->children) todo.push_back(&k);
        }
        if (writes < 2) return;
        Suggestion s;
        s.kind = "quadratic-sort";
        s.line = n.line;
        std::string name = decls_.count(array) ? decls_[array].name : "the array";
        s.title = "sort with merge sort or heap sort";
        s.why = "neighbouring elements of " + name + " are compared and swapped inside two loops: each pass moves one element into place";
        s.how = "Merge sort (split in halves, sort each, merge) or heap sort sorts in n log n steps; heap sort needs no extra array.";
        add(std::move(s), nullptr);
    }

    // An inner loop that adds up a range of an array that depends on the outer loop.
    void check_prefix_sums(const CNode& n, const Trip& t) {
        if (active_.empty() || !t.var || t.trips.constant()) return;
        const ActiveLoop& outer = active_.back();
        if (!outer.var) return;
        // ParC's "for j in 0..i" compares with a copy of i, so look through one variable.
        auto depends = [this, &outer](const CNode* node) {
            if (!node) return false;
            const CNode& s = strip(*node);
            return mentions(s, outer.var) || (s.kind == "DeclRefExpr" && decl_mentions(s.decl, outer.var));
        };
        if (!depends(t.start) && !depends(t.bound)) return;
        const CNode& body = strip(n.children.back());
        const std::vector<CNode>& stmts = body.kind == "CompoundStmt" ? body.children : n.children;
        const CNode* sum = nullptr;
        int other = 0;
        for (const auto& st : stmts) {
            if (!is_assignment(st)) continue;
            unsigned target = base_decl(st.children[0]);
            if (target == t.var) continue;
            const CNode* value = st.kind == "CompoundAssignOperator" ? &st.children[1] : st.children.size() == 2 ? &st.children[1] : nullptr;
            bool accumulates = value && (st.kind == "CompoundAssignOperator" ? st.op == "+=" : mentions(*value, target));
            if (accumulates && value && find_subscript(*value, t.var) && strip(st.children[0]).kind == "DeclRefExpr") sum = &st;
            else ++other;
        }
        if (!sum || other) return;
        const CNode* sub = find_subscript(*sum, t.var);
        unsigned array = base_decl(*sub);
        if (!array || outer_writes(array)) return;
        auto r = std::make_unique<Rewrite>();
        r->loop = &n;
        r->loop_once = true;
        r->extra_time = array_size(sub->children[0], 0);
        r->extra_heap = r->extra_time;
        Suggestion s;
        s.kind = "prefix-sums";
        s.line = n.line;
        s.title = "use prefix sums instead of adding the range again";
        s.why = "the loop at line " + std::to_string(n.line) + " adds up a stretch of " + decls_[array].name +
                " for every pass of the loop around it";
        s.how = "Before the loops, fill prefix[k + 1] = prefix[k] + " + decls_[array].name +
                "[k]; then the sum from i to j is prefix[j] - prefix[i], one step instead of a loop.";
        add(std::move(s), std::move(r));
    }

    bool decl_mentions(unsigned decl, unsigned other) const {
        auto found = decls_.find(decl);
        return found != decls_.end() && found->second.init && mentions(*found->second.init, other);
    }

    static const CNode* find_subscript(const CNode& n, unsigned index_var) {
        const CNode& s = strip(n);
        if (s.kind == "ArraySubscriptExpr" && s.children.size() == 2 && mentions(s.children[1], index_var)) return &s;
        for (const auto& c : s.children)
            if (const CNode* found = find_subscript(c, index_var)) return found;
        return nullptr;
    }

    bool outer_writes(unsigned array) const {
        for (const auto& l : active_)
            if (l.modified.count(array)) return true;
        return false;
    }

    // for d in 2..x { if (x % d == 0) ... }: a divisor search that can stop at sqrt(x).
    void check_trial_division(const CNode& n, const Trip& t) {
        if (!t.var || !t.bound || t.sqrt || t.step.kind != Step::Kind::Add || t.trips.constant()) return;
        std::vector<unsigned> bound_vars;
        refs_in(*t.bound, bound_vars);
        if (t.bound && strip(*t.bound).kind == "DeclRefExpr") {
            // ParC's "for d in 2..x" compares with a copy of x made before the loop.
            auto found = decls_.find(strip(*t.bound).decl);
            if (found != decls_.end() && found->second.init) refs_in(*found->second.init, bound_vars);
        }
        std::vector<const CNode*> todo = {&n.children.back()};
        while (!todo.empty()) {
            const CNode* c = todo.back();
            todo.pop_back();
            if (c->kind == "BinaryOperator" && c->op == "%" && c->children.size() == 2 && is_ref_to(c->children[1], t.var)) {
                unsigned x = base_decl(c->children[0]);
                if (x && std::find(bound_vars.begin(), bound_vars.end(), x) != bound_vars.end()) {
                    auto r = std::make_unique<Rewrite>();
                    r->loop = &n;
                    r->loop_sqrt = true;
                    Suggestion s;
                    s.kind = "trial-division";
                    s.line = n.line;
                    std::string var = decls_[t.var].name;
                    std::string xn = decls_[x].name;
                    s.title = "stop the divisor search at the square root";
                    s.why = "the loop tries every " + var + " below " + xn + ", but a divisor larger than sqrt(" + xn +
                            ") always comes with one smaller than it";
                    s.how = "Loop while " + var + " * " + var + " <= " + xn + " instead of " + var + " < " + xn + ".";
                    add(std::move(s), std::move(r));
                    return;
                }
            }
            for (const auto& k : c->children) todo.push_back(&k);
        }
    }

    // A call whose arguments do not change inside the loop: its result is the same every pass.
    void check_invariant_call(const CNode& call, const BigO& cost) {
        const ActiveLoop& loop = active_.back();
        for (const auto& a : call.children) {
            std::vector<unsigned> used;
            refs_in(a, used);
            for (unsigned d : used)
                if (loop.modified.count(d)) return;
        }
        auto r = std::make_unique<Rewrite>();
        r->call = &call;
        r->extra_time = cost.times(outer_trips());
        Suggestion s;
        s.kind = "invariant-call";
        s.line = call.line;
        s.title = "call " + display(call.name) + " once, before the loop";
        s.why = display(call.name) + " costs " + cost.str() + " and gets the same arguments on every pass of the loop at line " +
                std::to_string(loop.node->line);
        s.how = "Store the result in a variable before the loop and use the variable inside it.";
        add(std::move(s), std::move(r));
    }

    // let tmp = new int[n] inside a loop: a new array every pass, none of them freed.
    void check_alloc_in_loop(const CNode& call, const BigO& size) {
        const ActiveLoop& loop = active_.back();
        // The array may not leave the loop: it must not be returned or assigned to another variable.
        std::vector<const CNode*> todo = {loop.node};
        unsigned var = 0;
        for (const auto& [id, d] : decls_)
            if (d.init == &call || (d.init && &strip(*d.init) == &call)) var = id;
        if (!var) return;
        while (!todo.empty()) {
            const CNode* c = todo.back();
            todo.pop_back();
            if (c->kind == "ReturnStmt" && mentions(*c, var)) return;
            if (c->kind == "BinaryOperator" && c->op == "=" && c->children.size() == 2 && is_ref_to(c->children[1], var)) return;
            for (const auto& k : c->children) todo.push_back(&k);
        }
        auto r = std::make_unique<Rewrite>();
        r->alloc = &call;
        r->extra_heap = size.times(outer_trips());
        Suggestion s;
        s.kind = "allocation-in-loop";
        s.line = call.line;
        s.title = "allocate " + decls_[var].name + " once, before the loop";
        s.why = "a new array is made on every pass of the loop at line " + std::to_string(loop.node->line) +
                ", and ParC never frees the old ones";
        s.how = "Create the array before the loop and reuse it, clearing it at the top of each pass if needed.";
        add(std::move(s), std::move(r));
    }

    friend class Analyzer;
    void finish(Summary& out);
};

class Analyzer {
public:
    explicit Analyzer(const CUnit& unit) : unit_(unit) {}

    // A callee is analyzed (without a report) when its first caller needs its
    // summary; each function is analyzed once more here, with its report.
    ComplexityReport run() {
        ComplexityReport report;
        report.ok = true;
        for (size_t i = 0; i < unit_.functions.size(); ++i) {
            FunctionComplexity fc;
            summary_of(i, &fc);
            report.functions.push_back(std::move(fc));
        }
        return report;
    }

    Summary* find(const std::string& raw_name) {
        for (size_t i = 0; i < unit_.functions.size(); ++i)
            if (raw(unit_.functions[i]) == raw_name) return &summary_of(i, nullptr);
        return nullptr;
    }

    std::string raw(const CFunction& fn) const { return unit_.parc ? "f_" + fn.name : fn.name; }

    Summary& summary_of(size_t i, FunctionComplexity* report) {
        Summary& s = summaries_[i];
        if (s.in_progress || (s.done && !report)) return s;
        s.in_progress = true;
        const CFunction& fn = unit_.functions[i];
        FunctionWalk walk(*this, unit_, fn, raw(fn), nullptr, report);
        Summary result = walk.run();
        result.done = true;
        s = result;
        return s;
    }

    const CUnit& unit_;
    std::map<size_t, Summary> summaries_;
};

Summary* FunctionWalk::callee_summary(const std::string& raw_name) { return an_.find(raw_name); }

Summary FunctionWalk::run() {
    for (const auto& p : fn_.params) {
        DeclInfo& d = decls_[p.decl];
        d.name = display(p.name);
        d.param = true;
        d.size = param_size(p);
    }
    collect(fn_.body);
    Work w = walk(fn_.body);
    BigO time = w.time;
    solve_recursion(time, report_);
    Summary out;
    out.time = time;
    out.heap = w.heap;
    out.stack = stack_;
    if (rewrite_) {
        out.time = out.time.plus(rewrite_->extra_time);
        out.heap = out.heap.plus(rewrite_->extra_heap);
    }
    out.pure = pure_ && self_calls_.empty();
    for (const auto& p : fn_.params) out.param_sizes.push_back(param_size(p));
    if (report_) finish(out);
    return out;
}

// Fills the function's report, including the better bounds of each suggestion.
void FunctionWalk::finish(Summary& out) {
    FunctionComplexity& r = *report_;
    r.name = fn_.name;
    r.line = fn_.line;
    r.time = out.time;
    r.space = out.heap.plus(out.stack);
    r.nesting = nesting_;
    r.pure = out.pure;
    std::set<std::string> sizes;
    for (const BigO* b : {&r.time, &r.space})
        for (const auto& t : b->terms) {
            for (const auto& [s, p] : t.power) sizes.insert(s);
            for (const auto& [s, l] : t.logs) sizes.insert(s);
            if (!t.exp.empty()) sizes.insert(t.exp);
        }
    for (const auto& s : sizes) {
        std::string what = s == "?" ? "a loop count the analysis could not work out (see the notes)"
                           : s.rfind("len(", 0) == 0 ? "the length of array " + s.substr(4, s.size() - 5)
                                                       : "the value of " + s;
        r.sizes.push_back({s, what});
    }
    // The current bounds; then each suggestion's bound, by analyzing the function again with the change made.
    for (auto& c : candidates_) {
        BigO time = c.time_better;
        BigO space = c.space_better;
        if (c.rewrite) {
            FunctionWalk probe(an_, unit_, fn_, raw_name_, c.rewrite.get(), nullptr);
            Summary better = probe.run();
            time = better.time;
            space = better.heap.plus(better.stack);
        } else if (c.s.kind == "quadratic-sort") {
            std::string n = r.time.main_size();
            BigO size = BigO::of(n.empty() ? "n" : n);
            time = size.times(size.log());
            space = r.space;
        } else if (c.s.kind == "linear-recursion" || c.s.kind == "exponential-recursion") {
            space = out.heap.plus(space);
        }
        // Only a change that makes a bound smaller is worth suggesting.
        if (!r.time.grows_faster_than(time) && !r.space.grows_faster_than(space)) continue;
        Suggestion s = c.s;
        s.time_now = r.time.str();
        s.space_now = r.space.str();
        // Shown as they are, also when one bound grows (prefix sums buy time with an array).
        s.time_better = time.str();
        s.space_better = space.str();
        r.suggestions.push_back(std::move(s));
    }
}

}  // namespace

ComplexityReport analyze_complexity(const CUnit& unit) {
    if (!unit.ok) {
        ComplexityReport r;
        r.error = unit.error.empty() ? "Clang's syntax tree is not available" : unit.error;
        return r;
    }
    return Analyzer(unit).run();
}

}  // namespace parc::metrics
