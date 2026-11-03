#include "semantic/semantic.hpp"

#include <algorithm>
#include <optional>
#include <unordered_set>

#include "common/json.hpp"
#include "common/util.hpp"

namespace parc::sem {
namespace {

using ast::Expr;
using ast::ExprKind;
using ast::Pos;
using ast::Stmt;
using ast::StmtKind;

std::string quoted(const std::string& name) { return "'" + name + "'"; }

std::string at(Pos p) { return std::to_string(p.line) + ":" + std::to_string(p.col); }

// How many single-character edits turn a into b. Used for "did you mean" hints.
int edit_distance(const std::string& a, const std::string& b) {
    std::vector<int> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); ++i) {
        int diagonal = row[0];
        row[0] = static_cast<int>(i);
        for (size_t j = 1; j <= b.size(); ++j) {
            int above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

std::optional<std::string> closest(const std::string& name, const std::vector<std::string>& candidates) {
    std::optional<std::string> best;
    int best_distance = 3;  // accept at most two edits
    for (const auto& candidate : candidates) {
        int d = edit_distance(name, candidate);
        if (d < best_distance && d < isize(name)) {
            best_distance = d;
            best = candidate;
        }
    }
    return best;
}

// Can a value of type `value` be stored where `target` is expected?
bool assignable(Type target, Type value) {
    if (is_error(target) || is_error(value)) return true;  // already reported
    if (target == value) return true;
    if (target == kFloat && value == kInt) return true;  // int is promoted to float
    if (target.is_array && value == kNull) return true;
    return false;
}

std::string mismatch_hint(Type target, Type value) {
    if (value == kNull) return "only an array can be null";
    if (target == kInt && value == kFloat)
        return "a float can't become an int automatically, because the fraction would be lost";
    if ((target == kBool && value == kInt) || (target == kInt && value == kBool))
        return "there is no automatic conversion between int and bool";
    if (!target.is_array && value.is_array) return "use one element, for example a[0]";
    if (target.is_array && !value.is_array)
        return "create an array with new " + to_string(Type{target.kind, false}) + "[n]";
    return "";
}

bool comparable(Type l, Type r) {
    if (is_numeric(l) && is_numeric(r)) return true;
    if (l == kBool && r == kBool) return true;
    bool l_ref = l.is_array || l == kNull;
    bool r_ref = r.is_array || r == kNull;
    if (l_ref && r_ref) return l == kNull || r == kNull || l == r;
    return false;
}

class Analyzer {
public:
    Result run(const ast::Program& program) {
        declare_builtins();
        for (const auto& fn : program.functions) declare_function(fn);
        for (const auto& fn : program.functions) check_function(fn);
        report_unused();
        std::stable_sort(result_.diagnostics.begin(), result_.diagnostics.end(),
                         [](const Diagnostic& a, const Diagnostic& b) {
                             return a.line != b.line ? a.line < b.line : a.col < b.col;
                         });
        return std::move(result_);
    }

private:
    Result result_;
    SymbolTable symbols_;
    std::string function_;                      // the function being checked
    Type return_type_ = kVoid;
    std::vector<bool> loops_;                   // one entry per enclosing loop: is it parallel?
    std::unordered_set<std::string> reported_;  // undeclared names already reported in this function
    std::vector<bool> has_initializer_;         // per symbol row, for the unused-variable message
    std::unordered_set<std::string> functions_with_errors_;

    // ------------------------------------------------------------ reporting

    void report(const char* severity, const char* kind, Pos pos, std::string message, std::string hint = "",
                std::string subject = "") {
        Diagnostic d;
        d.phase = "semantic";
        d.severity = severity;
        d.line = pos.line;
        d.col = pos.col;
        d.message = std::move(message);
        d.hint = std::move(hint);
        d.kind = kind;
        d.function = function_;
        d.subject = std::move(subject);
        d.stmt_line = pos.line;
        if (d.severity == "error") functions_with_errors_.insert(function_);
        result_.diagnostics.push_back(std::move(d));
    }

    void error(const char* kind, Pos pos, std::string message, std::string hint = "", std::string subject = "") {
        report("error", kind, pos, std::move(message), std::move(hint), std::move(subject));
    }

    void warning(const char* kind, Pos pos, std::string message, std::string hint = "", std::string subject = "") {
        report("warning", kind, pos, std::move(message), std::move(hint), std::move(subject));
    }

    // ------------------------------------------------------------ declarations

    void declare_builtins() {
        Symbol print{"print", SymbolKind::Builtin, kVoid, {}, {}, {}, -1};
        Symbol len{"len", SymbolKind::Builtin, kInt, {}, {}, {}, -1};
        symbols_.declare(print);
        symbols_.declare(len);
    }

    static std::string signature(const Symbol& fn) {
        std::string out = "fn " + fn.name + "(";
        for (size_t i = 0; i < fn.params.size(); ++i)
            out += (i ? ", " : "") + fn.param_names[i] + ": " + to_string(fn.params[i]);
        out += ")";
        return fn.type == kVoid ? out : out + " -> " + to_string(fn.type);
    }

    static std::string where(const Symbol& s) { return s.pos.line > 0 ? " at " + at(s.pos) : ""; }

    // Functions are declared before any body is checked, so a function can
    // call one that is written further down the file.
    void declare_function(const ast::Function& fn) {
        function_ = fn.name;
        Symbol s;
        s.name = fn.name;
        s.kind = SymbolKind::Function;
        s.type = from_ast(fn.return_type);
        for (const auto& p : fn.params) {
            s.params.push_back(from_ast(p.type));
            s.param_names.push_back(p.name);
        }
        s.pos = fn.name_pos;
        s.index = isize(result_.symbols);

        if (const Symbol* clash = symbols_.declare(s)) {
            if (clash->kind == SymbolKind::Builtin) {
                error("redeclared", fn.name_pos, quoted(fn.name) + " is a built-in function and can't be defined again",
                      "pick a different name");
            } else {
                error("redeclared", fn.name_pos, "function " + quoted(fn.name) + " is already defined" + where(*clash),
                      "pick a different name for one of them");
            }
            return;
        }
        add_row(s, signature(s), "");

        if (fn.name == "main") {
            if (!fn.params.empty())
                error("bad-main", fn.params[0].pos, "'main' takes no parameters", "the program starts at fn main()");
            if (fn.return_type.is_array || (s.type != kVoid && s.type != kInt))
                error("bad-main", fn.name_pos, "'main' can only return int or nothing");
        }
    }

    void add_row(const Symbol& s, std::string type_text, const std::string& owner) {
        SymbolInfo info;
        info.name = s.name;
        info.kind = std::string(describe(s.kind));
        info.type = std::move(type_text);
        info.function = owner;
        info.line = s.pos.line;
        info.col = s.pos.col;
        info.depth = symbols_.depth();
        result_.symbols.push_back(std::move(info));
        has_initializer_.push_back(false);
    }

    // Declares a variable, parameter or loop variable in the innermost scope.
    // Returns its row in the symbol list, or -1 when the name was already taken.
    int declare(const std::string& name, SymbolKind kind, Type type, Pos pos) {
        const Symbol* outer = symbols_.lookup(name);
        std::optional<Symbol> hidden = outer ? std::optional<Symbol>(*outer) : std::nullopt;

        Symbol s;
        s.name = name;
        s.kind = kind;
        s.type = type;
        s.pos = pos;
        s.index = isize(result_.symbols);
        if (const Symbol* clash = symbols_.declare(s)) {
            std::string hint = "use a different name";
            if (clash->kind == SymbolKind::Variable && kind == SymbolKind::Variable)
                hint += ", or assign with " + name + " = ... instead of declaring it again";
            error("redeclared", pos,
                  quoted(name) + " is already declared in this scope as a " + std::string(describe(clash->kind)) +
                      where(*clash),
                  hint, name);
            return -1;
        }
        add_row(s, to_string(type), function_);

        if (hidden) {
            warning("shadowing", pos,
                    "this " + quoted(name) + " hides the " + std::string(describe(hidden->kind)) + " " + quoted(name) +
                        where(*hidden),
                    "inside this block " + quoted(name) + " now means the new one; a different name avoids mix-ups",
                    name);
        }
        return s.index;
    }

    void count_read(const Symbol& s) {
        if (s.index >= 0) ++result_.symbols[s.index].reads;
    }

    void count_write(const Symbol& s) {
        if (s.index >= 0) ++result_.symbols[s.index].writes;
    }

    void undeclared(const std::string& name, Pos pos, bool function) {
        // One report per name per function is enough.
        if (!reported_.insert((function ? "fn " : "var ") + name).second) return;
        std::string hint;
        if (auto near = closest(name, symbols_.visible_names(function))) {
            hint = "did you mean " + quoted(*near) + "?";
        } else {
            hint = function ? "define it with: fn " + name + "() { ... }"
                            : "declare it first, for example: let " + name + ": int = 0;";
        }
        error("undeclared", pos, (function ? "function " : "") + quoted(name) + " is not declared", hint, name);
    }

    void report_unused() {
        for (size_t i = 0; i < result_.symbols.size(); ++i) {
            const SymbolInfo& s = result_.symbols[i];
            if (s.kind != "variable" || s.reads > 0 || s.name.starts_with("_")) continue;
            // A function with an error often has a misspelled use of the variable, so stay quiet there.
            if (functions_with_errors_.count(s.function)) continue;
            function_ = s.function;
            bool assigned_later = s.writes > (has_initializer_[i] ? 1 : 0);
            warning("unused-variable", Pos{s.line, s.col},
                    assigned_later ? "variable " + quoted(s.name) + " is assigned, but its value is never used"
                                   : "variable " + quoted(s.name) + " is never used",
                    "remove it, or use its value", s.name);
        }
    }

    // ------------------------------------------------------------ functions and statements

    void check_function(const ast::Function& fn) {
        function_ = fn.name;
        return_type_ = from_ast(fn.return_type);
        reported_.clear();
        loops_.clear();

        // Parameters and the body's own declarations share one scope, so a
        // 'let' can't reuse a parameter's name.
        symbols_.push_scope();
        for (const auto& p : fn.params) declare(p.name, SymbolKind::Parameter, from_ast(p.type), p.pos);
        if (fn.body) check_statements(*fn.body);
        symbols_.pop_scope();
    }

    void check_statements(const ast::Block& block) {
        for (const auto& stmt : block.stmts) check_stmt(*stmt);
    }

    void check_block(const ast::Block& block) {
        symbols_.push_scope();
        check_statements(block);
        symbols_.pop_scope();
    }

    void check_condition(const Expr& cond) {
        Type t = value_of(cond);
        if (is_error(t) || t == kBool) return;
        error("type-mismatch", ast::start_of(cond), "a condition must be bool, but this is " + to_string(t),
              t == kInt ? "ParC does not treat numbers as true or false; compare instead, for example x != 0" : "",
              ast::to_source(cond));
    }

    void check_stmt(const Stmt& stmt) {
        switch (stmt.kind) {
            case StmtKind::Block: check_block(ast::as<ast::Block>(stmt)); break;
            case StmtKind::Let: check_let(ast::as<ast::Let>(stmt)); break;
            case StmtKind::Assign: check_assign(ast::as<ast::Assign>(stmt)); break;
            case StmtKind::ExprStmt: {
                const Expr& e = *ast::as<ast::ExprStmt>(stmt).expr;
                if (e.kind == ExprKind::Call) {
                    check_expr(e);  // a call may return nothing
                } else if (!is_error(value_of(e))) {
                    warning("unused-value", ast::start_of(e), "this expression's value is not used",
                            "assign it to a variable, or remove the statement", ast::to_source(e));
                }
                break;
            }
            case StmtKind::If: {
                const auto& s = ast::as<ast::If>(stmt);
                check_condition(*s.cond);
                check_block(*s.then_block);
                if (s.else_branch) check_stmt(*s.else_branch);
                break;
            }
            case StmtKind::While: {
                const auto& s = ast::as<ast::While>(stmt);
                check_condition(*s.cond);
                loops_.push_back(false);
                check_block(*s.body);
                loops_.pop_back();
                break;
            }
            case StmtKind::For: check_for(ast::as<ast::For>(stmt)); break;
            case StmtKind::Return: check_return(ast::as<ast::Return>(stmt)); break;
            case StmtKind::Break:
            case StmtKind::Continue: {
                bool is_break = stmt.kind == StmtKind::Break;
                std::string word = is_break ? "'break'" : "'continue'";
                if (loops_.empty()) {
                    error("bad-jump", stmt.pos, word + " can only be used inside a loop");
                } else if (is_break && loops_.back()) {
                    error("bad-jump", stmt.pos, "'break' can't leave a 'parallel for' loop",
                          "its iterations run at the same time, so there is no \"rest of the loop\" to skip; "
                          "use a normal 'for'");
                }
                break;
            }
        }
    }

    void check_let(const ast::Let& let) {
        Type declared = from_ast(let.type);
        if (let.init) {
            // The initial value is checked before the name exists, so
            // "let x: int = x;" refers to an outer x, or is an error.
            Type t = value_of(*let.init);
            if (!assignable(declared, t)) {
                error("type-mismatch", ast::start_of(*let.init),
                      quoted(let.name) + " is declared as " + to_string(declared) + ", but its initial value is " +
                          to_string(t),
                      mismatch_hint(declared, t), ast::to_source(*let.init));
            }
        }
        int row = declare(let.name, SymbolKind::Variable, declared, let.name_pos);
        if (row >= 0 && let.init) {
            ++result_.symbols[row].writes;
            has_initializer_[row] = true;
        }
    }

    void check_assign(const ast::Assign& assign) {
        Type target = kError;
        if (assign.target->kind == ExprKind::Name) {
            const auto& name = ast::as<ast::Name>(*assign.target);
            const Symbol* s = symbols_.lookup(name.name);
            if (!s) {
                undeclared(name.name, name.pos, false);
            } else if (s->kind == SymbolKind::Function || s->kind == SymbolKind::Builtin) {
                error("bad-assign", name.pos, quoted(name.name) + " is a function, so it can't be assigned");
            } else if (s->kind == SymbolKind::LoopVariable) {
                error("bad-assign", name.pos, "can't assign to the loop variable " + quoted(name.name),
                      "the loop sets it for each iteration; use a separate variable, or a while loop", name.name);
            } else {
                target = s->type;
                count_write(*s);
            }
            result_.types[assign.target.get()] = target;
        } else {
            target = check_expr(*assign.target);  // a[i]
        }

        Type value = value_of(*assign.value);
        if (!assignable(target, value)) {
            error("type-mismatch", ast::start_of(*assign.value),
                  "can't assign " + to_string(value) + " here: the left side is " + to_string(target),
                  mismatch_hint(target, value), ast::to_source(*assign.value));
        }
    }

    void check_for(const ast::For& loop) {
        for (const Expr* bound : {loop.start.get(), loop.end.get()}) {
            Type t = value_of(*bound);
            if (!is_error(t) && t != kInt) {
                error("type-mismatch", ast::start_of(*bound), "a loop range must be int, but this is " + to_string(t),
                      "ranges look like 0..n", ast::to_source(*bound));
            }
        }

        // Reduction variables are looked up before the loop variable exists:
        // they must live outside the loop.
        for (size_t i = 0; i < loop.reduce_vars.size(); ++i) {
            const std::string& name = loop.reduce_vars[i];
            Pos pos = loop.reduce_pos[i];
            std::string clause = "reduce(" + std::string(kind_symbol(loop.reduce_op)) + ": " + name + ")";
            auto earlier_end = loop.reduce_vars.begin() + static_cast<std::ptrdiff_t>(i);
            if (std::find(loop.reduce_vars.begin(), earlier_end, name) != earlier_end) {
                error("bad-reduce", pos, quoted(name) + " is listed twice in reduce(...)", "", name);
                continue;
            }
            const Symbol* s = symbols_.lookup(name);
            if (!s || name == loop.var) {
                error("bad-reduce", pos, quoted(name) + " in reduce(...) is not declared before the loop",
                      "declare it first, for example: let " + name + ": int = 0;", name);
            } else if (s->kind != SymbolKind::Variable && s->kind != SymbolKind::Parameter) {
                error("bad-reduce", pos,
                      clause + " needs a variable, but " + quoted(name) + " is a " + std::string(describe(s->kind)), "",
                      name);
            } else if (!is_numeric(s->type)) {
                error("bad-reduce", pos,
                      clause + " needs an int or float variable, but " + quoted(name) + " is " + to_string(s->type), "",
                      name);
            } else if (s->type == kFloat) {
                warning("float-reduction", pos,
                        "the parallel result in float " + quoted(name) + " can differ slightly from a normal loop's",
                        "float arithmetic is not associative: (a + b) + c can differ from a + (b + c) in the "
                        "last digits",
                        name);
                result_.diagnostics.back().proof =
                    quoted(name) + " is float, and " + clause +
                    " combines each thread's partial result in a different order than a serial loop";
            }
        }

        // The loop variable and the body's own declarations share one scope.
        symbols_.push_scope();
        declare(loop.var, SymbolKind::LoopVariable, kInt, loop.var_pos);
        loops_.push_back(loop.parallel);
        check_statements(*loop.body);
        loops_.pop_back();
        symbols_.pop_scope();
    }

    void check_return(const ast::Return& ret) {
        if (std::find(loops_.begin(), loops_.end(), true) != loops_.end()) {
            error("bad-jump", ret.pos, "'return' can't leave a 'parallel for' loop",
                  "its iterations run at the same time; use a normal 'for' if the loop must stop early");
        }
        if (!ret.value) {
            if (return_type_ != kVoid) {
                error("bad-return", ret.pos,
                      quoted(function_) + " returns " + to_string(return_type_) + ", so 'return' needs a value",
                      "for example: return 0;");
            }
            return;
        }
        Type t = value_of(*ret.value);
        if (return_type_ == kVoid) {
            if (!is_error(t)) {
                error("bad-return", ast::start_of(*ret.value),
                      quoted(function_) + " has no return type, so it can't return a value",
                      "add a return type to the function: fn " + function_ + "(...) -> " + to_string(t),
                      ast::to_source(*ret.value));
            }
        } else if (!assignable(return_type_, t)) {
            error("bad-return", ast::start_of(*ret.value),
                  quoted(function_) + " returns " + to_string(return_type_) + ", but this value is " + to_string(t),
                  mismatch_hint(return_type_, t), ast::to_source(*ret.value));
        }
    }

    // ------------------------------------------------------------ expressions

    // Gives the expression a type and remembers it.
    Type check_expr(const Expr& e) {
        Type t = compute(e);
        result_.types[&e] = t;
        return t;
    }

    // The same, for a place that needs a value: not a call that returns
    // nothing, and not a string.
    Type value_of(const Expr& e) {
        Type t = check_expr(e);
        if (t == kVoid) {
            std::string name = e.kind == ExprKind::Call ? ast::as<ast::Call>(e).callee : "this";
            error("type-mismatch", e.pos, quoted(name) + " returns no value, so it can't be used here",
                  name == "print" ? "print only shows its arguments"
                                  : "give the function a return type and a return statement",
                  ast::to_source(e));
            return result_.types[&e] = kError;
        }
        if (t == kString) {
            error("type-mismatch", e.pos, "a string can only be used as an argument to print", "", ast::to_source(e));
            return result_.types[&e] = kError;
        }
        return t;
    }

    Type compute(const Expr& e) {
        switch (e.kind) {
            case ExprKind::IntLit: return kInt;
            case ExprKind::FloatLit: return kFloat;
            case ExprKind::BoolLit: return kBool;
            case ExprKind::StringLit: return kString;
            case ExprKind::NullLit: return kNull;
            case ExprKind::Name: return check_name(ast::as<ast::Name>(e));
            case ExprKind::Unary: return check_unary(ast::as<ast::Unary>(e));
            case ExprKind::Binary: return check_binary(ast::as<ast::Binary>(e));
            case ExprKind::Index: return check_index(ast::as<ast::Index>(e));
            case ExprKind::Call: return check_call(ast::as<ast::Call>(e));
            case ExprKind::NewArray: {
                const auto& n = ast::as<ast::NewArray>(e);
                Type size = value_of(*n.size);
                if (!is_error(size) && size != kInt) {
                    error("type-mismatch", ast::start_of(*n.size),
                          "an array size must be int, but this is " + to_string(size), "", ast::to_source(*n.size));
                }
                return Type{from_ast(ast::Type{n.elem, false}).kind, true};
            }
        }
        return kError;
    }

    Type check_name(const ast::Name& name) {
        const Symbol* s = symbols_.lookup(name.name);
        if (!s) {
            undeclared(name.name, name.pos, false);
            return kError;
        }
        if (s->kind == SymbolKind::Function || s->kind == SymbolKind::Builtin) {
            error("type-mismatch", name.pos, quoted(name.name) + " is a function, not a variable",
                  "call it with parentheses: " + name.name + "(...)", name.name);
            return kError;
        }
        count_read(*s);
        return s->type;
    }

    // Each helper reports at the operand that is wrong and returns false.
    // An operand that already had an error passes silently.
    bool numeric_operand(Type t, const Expr& operand, const std::string& op) {
        if (is_error(t) || is_numeric(t)) return true;
        std::string hint;
        if (t == kBool) hint = "bool is not a number in ParC";
        if (t.is_array) hint = "use one element, for example a[i]";
        error("type-mismatch", ast::start_of(operand),
              "'" + op + "' needs a number (int or float), but this is " + to_string(t), hint,
              ast::to_source(operand));
        return false;
    }

    bool int_operand(Type t, const Expr& operand, const std::string& op) {
        if (is_error(t) || t == kInt) return true;
        error("type-mismatch", ast::start_of(operand), "'" + op + "' needs int on both sides, but this is " + to_string(t),
              t == kFloat ? "the remainder is only defined for whole numbers" : "", ast::to_source(operand));
        return false;
    }

    bool bool_operand(Type t, const Expr& operand, const std::string& op) {
        if (is_error(t) || t == kBool) return true;
        error("type-mismatch", ast::start_of(operand), "'" + op + "' needs bool, but this is " + to_string(t),
              t == kInt ? "ParC does not treat numbers as true or false; compare instead, for example x != 0" : "",
              ast::to_source(operand));
        return false;
    }

    Type check_unary(const ast::Unary& u) {
        Type t = value_of(*u.operand);
        if (u.op == Kind::Not) {
            bool_operand(t, *u.operand, "!");
            return kBool;
        }
        if (!numeric_operand(t, *u.operand, "-") || is_error(t)) return kError;
        return t;
    }

    Type check_binary(const ast::Binary& b) {
        Type l = value_of(*b.lhs);
        Type r = value_of(*b.rhs);
        std::string op(kind_symbol(b.op));
        switch (b.op) {
            case Kind::Plus:
            case Kind::Minus:
            case Kind::Star:
            case Kind::Slash:
                // One wrong operand is one error, so the right side is only checked when the left is fine.
                if (!numeric_operand(l, *b.lhs, op) || !numeric_operand(r, *b.rhs, op)) return kError;
                if (is_error(l) || is_error(r)) return kError;
                return (l == kFloat || r == kFloat) ? kFloat : kInt;  // int is promoted to float
            case Kind::Percent:
                if (!int_operand(l, *b.lhs, op) || !int_operand(r, *b.rhs, op)) return kError;
                return (is_error(l) || is_error(r)) ? kError : kInt;
            case Kind::Lt:
            case Kind::Le:
            case Kind::Gt:
            case Kind::Ge:
                if (numeric_operand(l, *b.lhs, op)) numeric_operand(r, *b.rhs, op);
                return kBool;
            case Kind::Eq:
            case Kind::Ne:
                if (!is_error(l) && !is_error(r) && !comparable(l, r)) {
                    std::string hint;
                    if (l == kNull || r == kNull) hint = "only an array can be null";
                    else if ((l == kInt && r == kBool) || (l == kBool && r == kInt))
                        hint = "there is no automatic conversion between int and bool";
                    error("type-mismatch", ast::start_of(*b.rhs),
                          "can't compare " + to_string(l) + " with " + to_string(r), hint, ast::to_source(*b.rhs));
                }
                return kBool;
            case Kind::And:
            case Kind::Or:
                if (bool_operand(l, *b.lhs, op)) bool_operand(r, *b.rhs, op);
                return kBool;
            default: return kError;
        }
    }

    Type check_index(const ast::Index& ix) {
        Type array = value_of(*ix.array);
        Type index = value_of(*ix.index);
        Type element = kError;
        if (!is_error(array)) {
            if (array.is_array) {
                element = Type{array.kind, false};
            } else {
                error("type-mismatch", ast::start_of(*ix.array),
                      "only an array can be indexed, but this is " + to_string(array), "", ast::to_source(*ix.array));
            }
        }
        if (!is_error(index) && index != kInt) {
            error("type-mismatch", ast::start_of(*ix.index), "an array index must be int, but this is " + to_string(index),
                  "", ast::to_source(*ix.index));
        }
        return element;
    }

    Type check_call(const ast::Call& call) {
        const Symbol* found = symbols_.lookup(call.callee);
        if (!found) {
            undeclared(call.callee, call.pos, true);
            for (const auto& arg : call.args) check_argument(*arg);
            return kError;
        }
        Symbol fn = *found;  // copied: checking the arguments can add scopes
        if (fn.kind == SymbolKind::Builtin) return fn.name == "print" ? check_print(call) : check_len(call);
        if (fn.kind != SymbolKind::Function) {
            error("bad-call", call.pos,
                  quoted(call.callee) + " is a " + std::string(describe(fn.kind)) + ", not a function", "", call.callee);
            for (const auto& arg : call.args) check_argument(*arg);
            return kError;
        }
        count_read(fn);

        if (call.args.size() != fn.params.size()) {
            auto count = [](size_t n) { return std::to_string(n) + (n == 1 ? " argument" : " arguments"); };
            error("bad-call", call.pos,
                  quoted(fn.name) + " takes " + count(fn.params.size()) + ", but " +
                      std::to_string(call.args.size()) + (call.args.size() == 1 ? " was given" : " were given"),
                  "its signature is " + signature(fn), call.callee);
            for (const auto& arg : call.args) check_argument(*arg);
            return fn.type;
        }
        for (size_t i = 0; i < call.args.size(); ++i) {
            Type t = value_of(*call.args[i]);
            if (!assignable(fn.params[i], t)) {
                error("bad-call", ast::start_of(*call.args[i]),
                      "argument " + std::to_string(i + 1) + " of " + quoted(fn.name) + " must be " +
                          to_string(fn.params[i]) + ", but this is " + to_string(t),
                      mismatch_hint(fn.params[i], t), ast::to_source(*call.args[i]));
            }
        }
        return fn.type;
    }

    // Arguments of a call that is itself wrong are still checked for their own mistakes.
    void check_argument(const Expr& arg) {
        if (arg.kind == ExprKind::StringLit) check_expr(arg);
        else value_of(arg);
    }

    Type check_print(const ast::Call& call) {
        for (const auto& arg : call.args) {
            if (arg->kind == ExprKind::StringLit) {
                check_expr(*arg);
                continue;
            }
            Type t = value_of(*arg);
            if (t.is_array) {
                error("bad-call", ast::start_of(*arg), "print can't show a whole array",
                      "print one element, for example a[0], or its length with len(a)", ast::to_source(*arg));
            } else if (t == kNull) {
                error("bad-call", arg->pos, "print can't show null");
            }
        }
        return kVoid;
    }

    Type check_len(const ast::Call& call) {
        if (call.args.size() != 1) {
            error("bad-call", call.pos,
                  "'len' takes 1 argument (an array), but " + std::to_string(call.args.size()) +
                      (call.args.size() == 1 ? " was given" : " were given"),
                  "for example: len(a)");
            for (const auto& arg : call.args) check_argument(*arg);
            return kInt;
        }
        Type t = value_of(*call.args[0]);
        if (!is_error(t) && !t.is_array) {
            error("bad-call", ast::start_of(*call.args[0]), "len needs an array, but this is " + to_string(t), "",
                  ast::to_source(*call.args[0]));
        }
        return kInt;
    }
};

}  // namespace

Result analyze(const ast::Program& program) { return Analyzer().run(program); }

std::string symbols_json(const Result& result, std::string_view filename) {
    std::string out = "{\n  \"file\": " + json::quote(filename) + ",\n  \"symbols\": [";
    for (size_t i = 0; i < result.symbols.size(); ++i) {
        const SymbolInfo& s = result.symbols[i];
        out += i ? ",\n    " : "\n    ";
        out += "{\"name\": " + json::quote(s.name) + ", \"kind\": " + json::quote(s.kind) + ", \"type\": " +
               json::quote(s.type) + ", \"function\": " + json::quote(s.function) + ", \"line\": " +
               std::to_string(s.line) + ", \"col\": " + std::to_string(s.col) + ", \"depth\": " +
               std::to_string(s.depth) + ", \"reads\": " + std::to_string(s.reads) + ", \"writes\": " +
               std::to_string(s.writes) + "}";
    }
    out += result.symbols.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

}  // namespace parc::sem
