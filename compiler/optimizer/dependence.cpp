#include "optimizer/dependence.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>

#include "common/util.hpp"

namespace parc::opt {
namespace {

using ast::Expr;
using ast::ExprKind;
using ast::Stmt;
using ast::StmtKind;

std::string quoted(const std::string& text) { return "'" + text + "'"; }

// ---------------------------------------------------------------- what functions do to their arguments

struct FunctionFacts {
    std::vector<bool> writes_param;  // does it store into the array passed as parameter k?
    bool prints = false;
};

// Calls f for every expression in a statement tree, outermost first.
template <typename F>
void each_expr(const Expr& e, F&& f) {
    f(e);
    switch (e.kind) {
        case ExprKind::Unary: each_expr(*ast::as<ast::Unary>(e).operand, f); break;
        case ExprKind::Binary:
            each_expr(*ast::as<ast::Binary>(e).lhs, f);
            each_expr(*ast::as<ast::Binary>(e).rhs, f);
            break;
        case ExprKind::Index:
            each_expr(*ast::as<ast::Index>(e).array, f);
            each_expr(*ast::as<ast::Index>(e).index, f);
            break;
        case ExprKind::Call:
            for (const auto& arg : ast::as<ast::Call>(e).args) each_expr(*arg, f);
            break;
        case ExprKind::NewArray: each_expr(*ast::as<ast::NewArray>(e).size, f); break;
        default: break;
    }
}

template <typename F>
void each_stmt(const Stmt& s, F&& f) {
    f(s);
    switch (s.kind) {
        case StmtKind::Block:
            for (const auto& child : ast::as<ast::Block>(s).stmts) each_stmt(*child, f);
            break;
        case StmtKind::If: {
            const auto& i = ast::as<ast::If>(s);
            each_stmt(*i.then_block, f);
            if (i.else_branch) each_stmt(*i.else_branch, f);
            break;
        }
        case StmtKind::While: each_stmt(*ast::as<ast::While>(s).body, f); break;
        case StmtKind::For: each_stmt(*ast::as<ast::For>(s).body, f); break;
        default: break;
    }
}

// The expressions a statement evaluates itself (not those of nested statements).
template <typename F>
void own_exprs(const Stmt& s, F&& f) {
    switch (s.kind) {
        case StmtKind::Let:
            if (ast::as<ast::Let>(s).init) f(*ast::as<ast::Let>(s).init);
            break;
        case StmtKind::Assign:
            f(*ast::as<ast::Assign>(s).target);
            f(*ast::as<ast::Assign>(s).value);
            break;
        case StmtKind::ExprStmt: f(*ast::as<ast::ExprStmt>(s).expr); break;
        case StmtKind::If: f(*ast::as<ast::If>(s).cond); break;
        case StmtKind::While: f(*ast::as<ast::While>(s).cond); break;
        case StmtKind::For:
            f(*ast::as<ast::For>(s).start);
            f(*ast::as<ast::For>(s).end);
            break;
        case StmtKind::Return:
            if (ast::as<ast::Return>(s).value) f(*ast::as<ast::Return>(s).value);
            break;
        default: break;
    }
}

// ---------------------------------------------------------------- subscripts of the form c*i + k

struct Affine {
    bool ok = true;
    long long coef = 0;                      // multiplies the loop variable
    long long konst = 0;
    std::map<std::string, long long> syms;   // loop-invariant variables and their multipliers

    bool constant() const { return coef == 0 && syms.empty(); }
};

class LoopAnalyzer {
public:
    LoopAnalyzer(const ast::Program& program, const sem::Result& types, bool force)
        : program_(program), types_(types), force_(force) {}

    LoopReport run() {
        summarize_functions();
        for (const auto& fn : program_.functions) {
            function_ = &fn;
            find_aliases(fn);
            if (fn.body) each_stmt(*fn.body, [this](const Stmt& s) {
                if (s.kind == StmtKind::For) analyze(ast::as<ast::For>(s));
            });
        }
        return std::move(report_);
    }

private:
    const ast::Program& program_;
    const sem::Result& types_;
    bool force_;
    LoopReport report_;
    std::unordered_map<std::string, FunctionFacts> facts_;
    const ast::Function* function_ = nullptr;
    std::unordered_map<std::string, std::string> alias_;  // array name -> its alias group

    bool is_array(const Expr& e) const {
        auto it = types_.types.find(&e);
        return it != types_.types.end() && it->second.is_array;
    }

    // ------------------------------------------------------------ function summaries

    void summarize_functions() {
        for (const auto& fn : program_.functions)
            facts_.try_emplace(fn.name).first->second.writes_param.assign(fn.params.size(), false);
        // A function writes a parameter if it stores into it or passes it to
        // a function that does; repeat until nothing more is found.
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& fn : program_.functions) {
                if (!fn.body) continue;
                FunctionFacts& mine = facts_[fn.name];
                auto param_index = [&fn](const Expr& e) {
                    if (e.kind != ExprKind::Name) return -1;
                    for (size_t k = 0; k < fn.params.size(); ++k)
                        if (fn.params[k].name == ast::as<ast::Name>(e).name) return static_cast<int>(k);
                    return -1;
                };
                auto mark = [&](int k) {
                    if (k < 0 || mine.writes_param[static_cast<size_t>(k)]) return;
                    mine.writes_param[static_cast<size_t>(k)] = true;
                    changed = true;
                };
                // Any array could be another name for a parameter (let b: int[] = a), so a
                // store through a local array counts as a store into every array parameter.
                auto mark_all = [&] {
                    for (size_t k = 0; k < fn.params.size(); ++k)
                        if (fn.params[k].type.is_array) mark(static_cast<int>(k));
                };
                each_stmt(*fn.body, [&](const Stmt& s) {
                    if (s.kind == StmtKind::Assign && ast::as<ast::Assign>(s).target->kind == ExprKind::Index) {
                        const Expr& array = *ast::as<ast::Index>(*ast::as<ast::Assign>(s).target).array;
                        if (param_index(array) >= 0) mark(param_index(array));
                        else mark_all();
                    }
                    own_exprs(s, [&](const Expr& top) {
                        each_expr(top, [&](const Expr& e) {
                            if (e.kind != ExprKind::Call) return;
                            const auto& call = ast::as<ast::Call>(e);
                            if (call.callee == "print" && !mine.prints) mine.prints = changed = true;
                            auto callee = facts_.find(call.callee);
                            if (callee == facts_.end()) return;
                            if (callee->second.prints && !mine.prints) mine.prints = changed = true;
                            for (size_t k = 0; k < call.args.size() && k < callee->second.writes_param.size(); ++k) {
                                if (!callee->second.writes_param[k]) continue;
                                if (param_index(*call.args[k]) >= 0) mark(param_index(*call.args[k]));
                                else mark_all();
                            }
                        });
                    });
                });
            }
        }
    }

    // ------------------------------------------------------------ which array names can be the same array

    // Two array parameters can be the same array (the caller decides), and
    // "let b: int[] = a;" makes b another name for a. An array made by "new"
    // and never copied from another name is on its own.
    void find_aliases(const ast::Function& fn) {
        alias_.clear();
        for (const auto& p : fn.params)
            if (p.type.is_array) alias_[p.name] = "a parameter";
        if (!fn.body) return;
        auto source_group = [this](const Expr& value) -> std::string {
            if (value.kind == ExprKind::NewArray || value.kind == ExprKind::NullLit) return "";
            if (value.kind == ExprKind::Name) {
                auto it = alias_.find(ast::as<ast::Name>(value).name);
                return it == alias_.end() ? "" : it->second;
            }
            return "a parameter";  // a call result: assume it may be any array the function was given
        };
        bool changed = true;
        while (changed) {
            changed = false;
            each_stmt(*fn.body, [&](const Stmt& s) {
                const std::string* name = nullptr;
                const Expr* value = nullptr;
                if (s.kind == StmtKind::Let && ast::as<ast::Let>(s).type.is_array && ast::as<ast::Let>(s).init) {
                    name = &ast::as<ast::Let>(s).name;
                    value = ast::as<ast::Let>(s).init.get();
                } else if (s.kind == StmtKind::Assign && ast::as<ast::Assign>(s).target->kind == ExprKind::Name &&
                           is_array(*ast::as<ast::Assign>(s).target)) {
                    name = &ast::as<ast::Name>(*ast::as<ast::Assign>(s).target).name;
                    value = ast::as<ast::Assign>(s).value.get();
                }
                if (!name) return;
                std::string group = source_group(*value);
                if (group.empty()) {
                    // A fresh array: its own group, unless it is already in a shared one.
                    // (If it is already in a shared group it stays there.)
                    if (!alias_.count(*name)) {
                        alias_[*name] = "fresh:" + *name;
                        changed = true;
                    }
                } else if (alias_[*name] != group) {
                    // Joining a shared group moves everything in the old group along.
                    std::string old = alias_[*name];
                    for (auto& [other, g] : alias_)
                        if (!old.empty() && g == old) g = group;
                    alias_[*name] = group;
                    changed = true;
                }
            });
        }
    }

    bool may_alias(const std::string& a, const std::string& b) const {
        if (a == b) return true;
        auto ga = alias_.find(a);
        auto gb = alias_.find(b);
        if (ga == alias_.end() || gb == alias_.end()) return true;  // unknown: be careful
        return ga->second == gb->second;
    }

    // ------------------------------------------------------------ one loop

    struct Access {
        std::string array;
        const Expr* subscript = nullptr;  // null: a call that may touch any element
        bool write = false;
        std::string text;  // "a[i - 1]"
        int line = 0;
    };

    struct ScalarWrite {
        int updates = 0;         // statements of the form x = x op e
        int other_writes = 0;
        char op = 0;             // '+' or '*', or '?' if the updates disagree
        int line = 0;
    };

    struct LoopFacts {
        std::set<std::string> inner;    // names declared inside the body
        std::set<std::string> written;  // outer scalars assigned inside the body
        std::map<std::string, ScalarWrite> scalars;
        std::map<std::string, int> reads;  // how often each name is read in the body
        std::vector<Access> accesses;
        std::string blocker;  // why the loop can't be suggested as parallel
    };

    // Is `value` of the form x + e, e + x, x * e or e * x, with e not using x?
    static char reduction_op(const std::string& x, const Expr& value) {
        if (value.kind != ExprKind::Binary) return 0;
        const auto& b = ast::as<ast::Binary>(value);
        if (b.op != Kind::Plus && b.op != Kind::Star) return 0;
        auto is_x = [&x](const Expr& e) { return e.kind == ExprKind::Name && ast::as<ast::Name>(e).name == x; };
        auto uses_x = [&](const Expr& e) {
            bool found = false;
            each_expr(e, [&](const Expr& part) { found = found || is_x(part); });
            return found;
        };
        bool left = is_x(*b.lhs) && !uses_x(*b.rhs);
        bool right = is_x(*b.rhs) && !uses_x(*b.lhs);
        return left || right ? (b.op == Kind::Plus ? '+' : '*') : 0;
    }

    LoopFacts collect(const ast::For& loop) {
        LoopFacts facts;
        facts.inner.insert(loop.var);
        each_stmt(*loop.body, [&](const Stmt& s) {
            if (s.kind == StmtKind::Let) facts.inner.insert(ast::as<ast::Let>(s).name);
            if (s.kind == StmtKind::For) facts.inner.insert(ast::as<ast::For>(s).var);
        });

        auto read = [&](const Expr& top) {
            each_expr(top, [&](const Expr& e) {
                if (e.kind == ExprKind::Name) ++facts.reads[ast::as<ast::Name>(e).name];
                if (e.kind == ExprKind::Index) {
                    const auto& ix = ast::as<ast::Index>(e);
                    record_access(facts, *ix.array, ix.index.get(), false, e);
                }
                if (e.kind == ExprKind::Call) record_call(facts, ast::as<ast::Call>(e));
            });
        };

        int loop_depth = 0;
        std::function<void(const Stmt&)> walk = [&](const Stmt& s) {
            switch (s.kind) {
                case StmtKind::Assign: {
                    const auto& a = ast::as<ast::Assign>(s);
                    read(*a.value);
                    if (a.target->kind == ExprKind::Index) {
                        const auto& ix = ast::as<ast::Index>(*a.target);
                        read(*ix.index);
                        record_access(facts, *ix.array, ix.index.get(), true, *a.target);
                    } else {
                        const std::string& name = ast::as<ast::Name>(*a.target).name;
                        if (facts.inner.count(name)) break;  // private to one iteration
                        facts.written.insert(name);
                        ScalarWrite& w = facts.scalars[name];
                        if (!w.line) w.line = s.pos.line;
                        char op = is_array(*a.target) ? 0 : reduction_op(name, *a.value);
                        if (!op) {
                            ++w.other_writes;
                        } else {
                            ++w.updates;
                            w.op = w.op == 0 || w.op == op ? op : '?';
                        }
                    }
                    break;
                }
                case StmtKind::Break:
                    if (loop_depth == 0) facts.blocker = "it can stop early with 'break'";
                    break;
                case StmtKind::Return: facts.blocker = "it can stop early with 'return'"; break;
                case StmtKind::Block:
                    for (const auto& child : ast::as<ast::Block>(s).stmts) walk(*child);
                    break;
                case StmtKind::If: {
                    const auto& i = ast::as<ast::If>(s);
                    read(*i.cond);
                    walk(*i.then_block);
                    if (i.else_branch) walk(*i.else_branch);
                    break;
                }
                case StmtKind::While:
                    read(*ast::as<ast::While>(s).cond);
                    ++loop_depth;
                    walk(*ast::as<ast::While>(s).body);
                    --loop_depth;
                    break;
                case StmtKind::For:
                    read(*ast::as<ast::For>(s).start);
                    read(*ast::as<ast::For>(s).end);
                    ++loop_depth;
                    walk(*ast::as<ast::For>(s).body);
                    --loop_depth;
                    break;
                default: own_exprs(s, read); break;
            }
        };
        walk(*loop.body);
        return facts;
    }

    void record_access(LoopFacts& facts, const Expr& array, const Expr* subscript, bool write, const Expr& whole) {
        Access a;
        a.array = array.kind == ExprKind::Name ? ast::as<ast::Name>(array).name : "?";
        if (facts.inner.count(a.array)) return;  // an array made inside the iteration is private to it
        a.subscript = subscript;
        a.write = write;
        a.text = ast::to_source(whole);
        a.line = whole.pos.line;
        facts.accesses.push_back(std::move(a));
    }

    void record_call(LoopFacts& facts, const ast::Call& call) {
        if (call.callee == "print") {
            if (facts.blocker.empty()) facts.blocker = "it prints, and the lines would come out in a different order";
            return;
        }
        auto callee = facts_.find(call.callee);
        if (callee == facts_.end()) return;  // len
        if (callee->second.prints && facts.blocker.empty())
            facts.blocker = quoted(call.callee) + " prints, and the lines would come out in a different order";
        for (size_t k = 0; k < call.args.size(); ++k) {
            if (!is_array(*call.args[k])) continue;
            Access a;
            a.array = call.args[k]->kind == ExprKind::Name ? ast::as<ast::Name>(*call.args[k]).name : "?";
            if (facts.inner.count(a.array)) continue;
            a.write = k < callee->second.writes_param.size() && callee->second.writes_param[k];
            a.text = ast::to_source(call);
            a.line = call.pos.line;
            facts.accesses.push_back(std::move(a));
        }
    }

    Affine affine(const Expr& e, const std::string& var, const LoopFacts& facts) const {
        Affine r;
        switch (e.kind) {
            case ExprKind::IntLit: r.konst = ast::as<ast::IntLit>(e).value; return r;
            case ExprKind::Name: {
                const std::string& name = ast::as<ast::Name>(e).name;
                if (name == var) r.coef = 1;
                else if (facts.inner.count(name) || facts.written.count(name) || is_array(e)) r.ok = false;
                else r.syms[name] = 1;  // the same value in every iteration
                return r;
            }
            case ExprKind::Unary: {
                if (ast::as<ast::Unary>(e).op != Kind::Minus) break;
                r = affine(*ast::as<ast::Unary>(e).operand, var, facts);
                r.coef = -r.coef;
                r.konst = -r.konst;
                for (auto& [name, c] : r.syms) c = -c;
                return r;
            }
            case ExprKind::Binary: {
                const auto& b = ast::as<ast::Binary>(e);
                Affine l = affine(*b.lhs, var, facts);
                Affine rr = affine(*b.rhs, var, facts);
                if (!l.ok || !rr.ok) break;
                if (b.op == Kind::Plus || b.op == Kind::Minus) {
                    long long sign = b.op == Kind::Plus ? 1 : -1;
                    l.coef += sign * rr.coef;
                    l.konst += sign * rr.konst;
                    for (const auto& [name, c] : rr.syms) l.syms[name] += sign * c;
                    std::erase_if(l.syms, [](const auto& entry) { return entry.second == 0; });
                    return l;
                }
                if (b.op == Kind::Star && (l.constant() || rr.constant())) {
                    const Affine& scale = l.constant() ? l : rr;
                    Affine other = l.constant() ? rr : l;
                    other.coef *= scale.konst;
                    other.konst *= scale.konst;
                    for (auto& [name, c] : other.syms) c *= scale.konst;
                    std::erase_if(other.syms, [](const auto& entry) { return entry.second == 0; });
                    return other;
                }
                break;
            }
            default: break;
        }
        r.ok = false;
        return r;
    }

    // Is there a dependence between different iterations? Returns the reason, or "" if not.
    // `note` gets a line for the proof either way.
    std::string depends(const Access& w, const Access& x, const ast::For& loop, const LoopFacts& facts,
                        std::string& note) const {
        if (!may_alias(w.array, x.array)) return "";
        std::string pair = w.text + (w.text == x.text ? "" : " and " + x.text);
        std::string same = w.array == x.array ? "" : " (" + w.array + " and " + x.array + " may be the same array)";
        if (!w.subscript || !x.subscript) {
            const Access& call = w.subscript ? x : w;
            return "the call " + quoted(call.text) + " may touch any element of " + quoted(call.array) + same;
        }
        Affine a1 = affine(*w.subscript, loop.var, facts);
        Affine a2 = affine(*x.subscript, loop.var, facts);
        if (!a1.ok || !a2.ok) {
            const Access& odd = a1.ok ? x : w;
            return "the subscript of " + quoted(odd.text) + " is not of the form c*" + loop.var +
                   " + k, so the compiler can't tell which element each iteration uses" + same;
        }
        if (a1.syms != a2.syms)
            return "the distance between " + quoted(w.text) + " and " + quoted(x.text) + " is not a known constant" + same;

        long long c = a2.konst - a1.konst;  // a1.coef*i1 - a2.coef*i2 = c has a solution?
        if (a1.coef == 0 && a2.coef == 0) {  // ZIV test
            if (c != 0) {
                note = pair + ": different fixed elements (ZIV test)";
                return "";
            }
            return "every iteration " + std::string(x.write ? "writes " : "uses ") + quoted(w.text) +
                   ", the same element" + same;
        }
        if (a1.coef == a2.coef) {  // strong SIV test
            if (c % a1.coef != 0) {
                note = pair + ": never the same element (" + std::to_string(c) + " is not a multiple of " +
                       std::to_string(a1.coef) + ", strong SIV test)";
                return "";
            }
            long long distance = std::llabs(c / a1.coef);
            if (distance == 0) {
                note = pair + ": the same element only within one iteration (distance 0)";
                return "";
            }
            if (loop.start->kind == ExprKind::IntLit && loop.end->kind == ExprKind::IntLit) {
                long long trips = ast::as<ast::IntLit>(*loop.end).value - ast::as<ast::IntLit>(*loop.start).value;
                if (distance >= trips) {
                    note = pair + ": distance " + std::to_string(distance) + " is longer than the loop";
                    return "";
                }
            }
            return quoted(w.text) + " is written, and " + quoted(x.text) + " is the element " +
                   (x.write ? "written " : "used ") + std::to_string(distance) +
                   (distance == 1 ? " iteration" : " iterations") + " away (distance " + std::to_string(distance) +
                   ")" + same;
        }
        long long g = std::gcd(std::llabs(a1.coef), std::llabs(a2.coef));  // GCD test
        if (c % g != 0) {
            note = pair + ": never the same element (gcd " + std::to_string(g) + " does not divide " + std::to_string(c) +
                   ", GCD test)";
            return "";
        }
        return quoted(w.text) + " and " + quoted(x.text) + " can be the same element in different iterations (gcd " +
               std::to_string(g) + " divides " + std::to_string(c) + ", so the GCD test can't rule it out)" + same;
    }

    void analyze(const ast::For& loop) {
        LoopFacts facts = collect(loop);
        std::vector<std::string> problems;  // each one makes the loop unsafe to run in parallel
        std::vector<std::string> subjects;
        std::vector<std::string> notes;     // tests that passed, for the proof
        std::string reduce_op(1, loop.reduce_op == Kind::Star ? '*' : '+');

        // Scalars shared by all iterations.
        std::vector<std::string> candidates;  // reductions the loop could declare
        char candidate_op = 0;
        bool mixed_ops = false;
        for (const auto& [name, w] : facts.scalars) {
            bool declared = std::find(loop.reduce_vars.begin(), loop.reduce_vars.end(), name) != loop.reduce_vars.end();
            bool pure = w.other_writes == 0 && w.op != '?' && facts.reads[name] == w.updates;
            if (declared) {
                if (pure && std::string(1, w.op) == reduce_op) {
                    notes.push_back(name + ": every use is the reduction update " + name + " = " + name + " " + reduce_op +
                                    " ...");
                } else {
                    problems.push_back(quoted(name) + " is listed in reduce(" + reduce_op + ": " + name +
                                       "), but the loop does more with it than " + name + " = " + name + " " +
                                       reduce_op + " ...");
                    subjects.push_back(name);
                }
            } else if (pure) {
                candidates.push_back(name);
                mixed_ops = mixed_ops || (candidate_op && candidate_op != w.op);
                candidate_op = w.op;
                if (loop.parallel) {
                    problems.push_back(quoted(name) + " is updated by every iteration (line " + std::to_string(w.line) +
                                       "); it needs reduce(" + std::string(1, w.op) + ": " + name + ")");
                    subjects.push_back(name);
                }
            } else {
                problems.push_back(quoted(name) + " is written inside the loop (line " + std::to_string(w.line) +
                                   ") and shared by all iterations");
                subjects.push_back(name);
            }
        }
        // A reduction variable that is only read would see each thread's partial value.
        for (const auto& name : loop.reduce_vars) {
            if (!facts.scalars.count(name) && facts.reads[name] > 0) {
                problems.push_back(quoted(name) + " is listed in reduce(...), but the loop only reads it");
                subjects.push_back(name);
            }
        }

        // Arrays: every write against every access.
        bool array_write = false;
        std::set<std::string> seen;
        for (size_t i = 0; i < facts.accesses.size(); ++i) {
            const Access& w = facts.accesses[i];
            if (!w.write) continue;
            array_write = true;
            for (size_t j = 0; j < facts.accesses.size(); ++j) {
                const Access& x = facts.accesses[j];
                if (x.write && j < i) continue;  // that pair was tested the other way round
                std::string note;
                std::string reason = depends(w, x, loop, facts, note);
                if (!note.empty() && seen.insert(note).second) notes.push_back(note);
                if (!reason.empty() && seen.insert(reason).second) {
                    problems.push_back(reason);
                    subjects.push_back(x.text);
                }
            }
        }

        auto emit = [&](const char* severity, const char* kind, std::string message, std::string hint) -> Diagnostic& {
            Diagnostic d;
            d.phase = "optimizer";
            d.severity = severity;
            d.kind = kind;
            d.line = loop.pos.line;
            d.col = loop.pos.col;
            d.stmt_line = loop.pos.line;
            d.function = function_->name;
            d.message = std::move(message);
            d.hint = std::move(hint);
            std::string proof;
            for (size_t i = 0; i < notes.size() && i < 4; ++i) proof += (i ? "; " : "") + notes[i];
            d.proof = proof.empty() ? "" : "dependence tests: " + proof;
            report_.findings.push_back(std::move(d));
            return report_.findings.back();
        };

        if (loop.parallel) {
            if (problems.empty()) {
                report_.parallel.insert(&loop);
                std::string message = "this loop runs in parallel: no iteration depends on another";
                if (!loop.reduce_vars.empty()) {
                    message += ", and ";
                    for (size_t i = 0; i < loop.reduce_vars.size(); ++i) message += (i ? ", " : "") + loop.reduce_vars[i];
                    message += loop.reduce_vars.size() == 1 ? " is combined with reduce(" : " are combined with reduce(";
                    message += reduce_op + ")";
                }
                Diagnostic& d = emit("info", "parallelized", message, "");
                d.subject = "parallel for " + loop.var;
                if (d.proof.empty()) d.proof = "dependence tests: the loop body writes nothing that another iteration uses";
            } else {
                if (force_) report_.parallel.insert(&loop);
                Diagnostic& d = emit("warning", "parallel-dependence",
                                     "this parallel loop is not safe, so it runs as a normal loop: " + problems[0],
                                     "make each iteration use only its own data, or write 'for' instead of 'parallel for'");
                d.subject = subjects[0];
                d.cwe = "CWE-362";
                d.proof = "dependence analysis: " + problems[0];
                for (size_t i = 1; i < problems.size() && i < 3; ++i) d.proof += "; " + problems[i];
            }
            return;
        }

        // A normal loop: say whether it could be parallel, when that is worth saying.
        if (problems.empty() && facts.blocker.empty() && !mixed_ops && (array_write || !candidates.empty())) {
            std::string clause;
            if (!candidates.empty()) {
                clause = "reduce(" + std::string(1, candidate_op) + ": ";
                for (size_t i = 0; i < candidates.size(); ++i) clause += (i ? ", " : "") + candidates[i];
                clause += ")";
            }
            Diagnostic& d = emit("info", "parallelizable",
                                 "this loop could be a 'parallel for': no iteration depends on another" +
                                     (clause.empty() ? "" : ", and " + clause + " combines the results"),
                                 "write: parallel for " + loop.var + " in " + ast::to_source(*loop.start) + ".." +
                                     ast::to_source(*loop.end) + (clause.empty() ? "" : " " + clause));
            d.subject = "for " + loop.var;
            d.guard = clause;
            if (d.proof.empty()) d.proof = "dependence tests: the loop body writes nothing that another iteration uses";
        } else if (!problems.empty() && array_write) {
            Diagnostic& d = emit("info", "loop-dependence", "this loop has to stay serial: " + problems[0], "");
            d.subject = subjects[0];
            d.proof = "dependence analysis: " + problems[0];
        }
    }
};

}  // namespace

LoopReport analyze_loops(const ast::Program& program, const sem::Result& types, bool force_parallel) {
    return LoopAnalyzer(program, types, force_parallel).run();
}

}  // namespace parc::opt
