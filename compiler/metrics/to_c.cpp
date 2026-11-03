#include "metrics/to_c.hpp"

#include <cstdio>
#include <map>
#include <vector>

namespace parc::metrics {

namespace {

using namespace parc::ast;

// No #include: the few library functions are declared here, so libclang can
// parse the text without finding the system's header files.
constexpr const char* kPrelude = R"(typedef __SIZE_TYPE__ parc_size_t;
int printf(const char *, ...);
void *malloc(parc_size_t);
void *calloc(parc_size_t, parc_size_t);
int atoi(const char *);
#define NULL ((void *)0)
typedef _Bool bool;
#define true 1
#define false 0

typedef struct { int n; int *d; } arr_int;
typedef struct { int n; double *d; } arr_float;
typedef struct { int n; bool *d; } arr_bool;

static arr_int *new_int(int n) { arr_int *a = malloc(sizeof *a); a->n = n; a->d = calloc(n ? n : 1, sizeof(int)); return a; }
static arr_float *new_float(int n) { arr_float *a = malloc(sizeof *a); a->n = n; a->d = calloc(n ? n : 1, sizeof(double)); return a; }
static arr_bool *new_bool(int n) { arr_bool *a = malloc(sizeof *a); a->n = n; a->d = calloc(n ? n : 1, sizeof(bool)); return a; }
)";

std::string c_type(const Type& t) {
    std::string base = t.base == BaseType::Int     ? "int"
                       : t.base == BaseType::Float ? "double"
                       : t.base == BaseType::Bool  ? "bool"
                                                   : "void";
    if (!t.is_array) return base;
    return std::string(t.base == BaseType::Int ? "arr_int" : t.base == BaseType::Float ? "arr_float" : "arr_bool") + " *";
}

// A C string literal. Octal escapes, because \x would also swallow the hex digits after it.
std::string c_string(const std::string& text) {
    std::string out = "\"";
    for (unsigned char ch : text) {
        if (ch == '\\' || ch == '"') {
            out += '\\';
            out += static_cast<char>(ch);
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\t') {
            out += "\\t";
        } else if (ch < 0x20 || ch == 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\%03o", ch);
            out += buf;
        } else {
            out += static_cast<char>(ch);
        }
    }
    return out + "\"";
}

std::string c_float(double value) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", value);
    std::string out = buf;
    if (out.find_first_of(".eEn") == std::string::npos) out += ".0";
    return out;
}

// The kinds of value print has to tell apart.
enum class Shown { Int, Float, Bool, String, Array, Void };

class Translator {
public:
    Translator(const Program& program, std::string_view file) : program_(program), file_(file) {
        for (const Function& fn : program.functions) returns_[fn.name] = fn.return_type;
    }

    std::string run() {
        std::string out = kPrelude;
        out += "\n";
        std::vector<std::string> signatures;
        for (const Function& fn : program_.functions) {
            std::string params;
            for (const Param& p : fn.params) params += (params.empty() ? "" : ", ") + c_type(p.type) + " v_" + p.name;
            signatures.push_back("static " + c_type(fn.return_type) + " f_" + fn.name + "(" +
                                 (params.empty() ? "void" : params) + ")");
        }
        for (const auto& s : signatures) out += s + ";\n";

        // The entry point comes before the #line directives, so it keeps C line numbers.
        auto main_fn = returns_.find("main");
        bool returns_int = main_fn != returns_.end() && main_fn->second.base == BaseType::Int && !main_fn->second.is_array;
        out += "\nint main(int argc, char **argv) {\n"
               "    int repeat = argc > 1 ? atoi(argv[1]) : 1;\n"
               "    int code = 0;\n"
               "    for (int r = 0; r < repeat; r++) ";
        out += returns_int ? "code = f_main();\n" : "f_main();\n";
        out += "    return code;\n}\n";

        for (size_t i = 0; i < program_.functions.size(); ++i) {
            const Function& fn = program_.functions[i];
            emit("", 0);
            emit(signatures[i] + " {", fn.pos.line);
            scopes_.push_back({});
            for (const Param& p : fn.params) scopes_.back()[p.name] = p.type;
            block_body(*fn.body);
            scopes_.pop_back();
            emit("}", 0);
        }
        for (const auto& line : lines_) out += line + "\n";
        return out;
    }

private:
    const Program& program_;
    std::string file_;
    std::map<std::string, Type> returns_;
    std::vector<std::map<std::string, Type>> scopes_;
    std::vector<std::string> lines_;
    int depth_ = 0;
    int counter_ = 0;
    int presumed_ = -1;  // the line Clang will think the next line of text is on

    // Adds one line of C. A line with a ParC line number gets a #line
    // directive first, unless Clang already counts it as that line.
    void emit(const std::string& text, int line) {
        if (line > 0 && line != presumed_) {
            lines_.push_back("#line " + std::to_string(line) + " " + c_string(file_));
            presumed_ = line;
        }
        lines_.push_back(std::string(static_cast<size_t>(depth_) * 4, ' ') + text);
        if (presumed_ > 0) ++presumed_;
    }

    Type lookup(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return found->second;
        }
        return {BaseType::Int, false};
    }

    // The type of an expression, by the same rules as ParC's type checker.
    Shown shown(const Expr& e) const {
        switch (e.kind) {
            case ExprKind::IntLit: return Shown::Int;
            case ExprKind::FloatLit: return Shown::Float;
            case ExprKind::BoolLit: return Shown::Bool;
            case ExprKind::StringLit: return Shown::String;
            case ExprKind::NullLit: return Shown::Array;
            case ExprKind::NewArray: return Shown::Array;
            case ExprKind::Name: return of(lookup(as<Name>(e).name));
            case ExprKind::Unary: {
                const auto& u = as<Unary>(e);
                return u.op == Kind::Not ? Shown::Bool : shown(*u.operand);
            }
            case ExprKind::Binary: {
                const auto& b = as<Binary>(e);
                switch (b.op) {
                    case Kind::Lt: case Kind::Le: case Kind::Gt: case Kind::Ge:
                    case Kind::Eq: case Kind::Ne: case Kind::And: case Kind::Or:
                        return Shown::Bool;
                    default:
                        return shown(*b.lhs) == Shown::Float || shown(*b.rhs) == Shown::Float ? Shown::Float : Shown::Int;
                }
            }
            case ExprKind::Index: {
                const auto& ix = as<Index>(e);
                if (ix.array->kind == ExprKind::Name) {
                    Type t = lookup(as<Name>(*ix.array).name);
                    return of({t.base, false});
                }
                if (ix.array->kind == ExprKind::Call) {
                    auto found = returns_.find(as<Call>(*ix.array).callee);
                    if (found != returns_.end()) return of({found->second.base, false});
                }
                return Shown::Int;
            }
            case ExprKind::Call: {
                const auto& call = as<Call>(e);
                if (call.callee == "len") return Shown::Int;
                auto found = returns_.find(call.callee);
                return found == returns_.end() ? Shown::Void : of(found->second);
            }
        }
        return Shown::Int;
    }

    static Shown of(const Type& t) {
        if (t.is_array) return Shown::Array;
        switch (t.base) {
            case BaseType::Int: return Shown::Int;
            case BaseType::Float: return Shown::Float;
            case BaseType::Bool: return Shown::Bool;
            case BaseType::Void: return Shown::Void;
        }
        return Shown::Int;
    }

    std::string expr(const Expr& e) {
        switch (e.kind) {
            case ExprKind::IntLit: return std::to_string(as<IntLit>(e).value);
            case ExprKind::FloatLit: return c_float(as<FloatLit>(e).value);
            case ExprKind::BoolLit: return as<BoolLit>(e).value ? "true" : "false";
            case ExprKind::StringLit: return c_string(as<StringLit>(e).value);
            case ExprKind::NullLit: return "NULL";
            case ExprKind::Name: return "v_" + as<Name>(e).name;
            case ExprKind::Unary: {
                const auto& u = as<Unary>(e);
                return "(" + std::string(kind_symbol(u.op)) + expr(*u.operand) + ")";
            }
            case ExprKind::Binary: {
                const auto& b = as<Binary>(e);
                return "(" + expr(*b.lhs) + " " + std::string(kind_symbol(b.op)) + " " + expr(*b.rhs) + ")";
            }
            case ExprKind::Index: {
                const auto& ix = as<Index>(e);
                return expr(*ix.array) + "->d[" + expr(*ix.index) + "]";
            }
            case ExprKind::Call: {
                const auto& call = as<Call>(e);
                if (call.callee == "len" && call.args.size() == 1) return expr(*call.args[0]) + "->n";
                std::string args;
                for (const auto& a : call.args) args += (args.empty() ? "" : ", ") + expr(*a);
                return "f_" + call.callee + "(" + args + ")";
            }
            case ExprKind::NewArray: {
                const auto& n = as<NewArray>(e);
                std::string elem = n.elem == BaseType::Float ? "float" : n.elem == BaseType::Bool ? "bool" : "int";
                return "new_" + elem + "(" + expr(*n.size) + ")";
            }
        }
        return "0";
    }

    // An expression without the parentheses around it, where C does not need them:
    // "if ((a == b))" would make Clang warn about the extra pair.
    std::string bare(const Expr& e) {
        if (e.kind != ExprKind::Binary) return expr(e);
        const auto& b = as<Binary>(e);
        return expr(*b.lhs) + " " + std::string(kind_symbol(b.op)) + " " + expr(*b.rhs);
    }

    // One printf per argument, so the arguments are evaluated left to right as in ParC.
    void print_call(const Call& call, int line) {
        if (call.args.empty()) {
            emit("printf(\"\\n\");", line);
            return;
        }
        for (size_t i = 0; i < call.args.size(); ++i) {
            const Expr& arg = *call.args[i];
            std::string end = i + 1 == call.args.size() ? "\\n" : " ";
            std::string format = "%d";
            std::string value = expr(arg);
            switch (shown(arg)) {
                case Shown::String: format = "%s"; break;
                case Shown::Float: format = "%g"; break;
                case Shown::Bool:
                    format = "%s";
                    value = "(" + value + " ? \"true\" : \"false\")";
                    break;
                case Shown::Array:
                    format = "%s";
                    value = "(" + value + " ? \"array\" : \"null\")";
                    break;
                default: break;
            }
            emit("printf(\"" + format + end + "\", " + value + ");", line);
        }
    }

    void block_body(const Block& block) {
        ++depth_;
        for (const auto& s : block.stmts) stmt(*s);
        --depth_;
    }

    void scoped_block(const Block& block, std::map<std::string, Type> names = {}) {
        scopes_.push_back(std::move(names));
        block_body(block);
        scopes_.pop_back();
    }

    void stmt(const Stmt& s) {
        int line = s.pos.line;
        switch (s.kind) {
            case StmtKind::Block:
                emit("{", line);
                scoped_block(as<Block>(s));
                emit("}", 0);
                break;
            case StmtKind::Let: {
                const auto& let = as<Let>(s);
                // ParC gives a variable without a value 0 (or null), and so does the C version.
                std::string init = let.init ? bare(*let.init) : let.type.is_array ? "NULL" : "0";
                scopes_.back()[let.name] = let.type;
                emit(c_type(let.type) + " v_" + let.name + " = " + init + ";", line);
                break;
            }
            case StmtKind::Assign: {
                const auto& a = as<Assign>(s);
                emit(expr(*a.target) + " = " + bare(*a.value) + ";", line);
                break;
            }
            case StmtKind::ExprStmt: {
                const Expr& e = *as<ExprStmt>(s).expr;
                if (e.kind == ExprKind::Call && as<Call>(e).callee == "print") print_call(as<Call>(e), line);
                else emit("(void)(" + expr(e) + ");", line);
                break;
            }
            case StmtKind::If: {
                const auto& node = as<If>(s);
                emit("if (" + bare(*node.cond) + ") {", line);
                scoped_block(*node.then_block);
                const Stmt* branch = node.else_branch.get();
                while (branch && branch->kind == StmtKind::If) {
                    const auto& elif = as<If>(*branch);
                    emit("} else if (" + bare(*elif.cond) + ") {", branch->pos.line);
                    scoped_block(*elif.then_block);
                    branch = elif.else_branch.get();
                }
                if (branch) {
                    emit("} else {", 0);
                    scoped_block(as<Block>(*branch));
                }
                emit("}", 0);
                break;
            }
            case StmtKind::While: {
                const auto& w = as<While>(s);
                emit("while (" + bare(*w.cond) + ") {", line);
                scoped_block(*w.body);
                emit("}", 0);
                break;
            }
            case StmtKind::For: {
                const auto& f = as<For>(s);
                // The range is computed once, before the loop, as in ParC.
                std::string end = "end_" + std::to_string(++counter_);
                std::string var = "v_" + f.var;
                emit("for (int " + var + " = " + expr(*f.start) + ", " + end + " = " + expr(*f.end) + "; " + var + " < " +
                         end + "; " + var + "++) {",
                     line);
                scoped_block(*f.body, {{f.var, Type{BaseType::Int, false}}});
                emit("}", 0);
                break;
            }
            case StmtKind::Return: {
                const auto& r = as<Return>(s);
                emit(r.value ? "return " + bare(*r.value) + ";" : "return;", line);
                break;
            }
            case StmtKind::Break: emit("break;", line); break;
            case StmtKind::Continue: emit("continue;", line); break;
        }
    }
};

}  // namespace

std::string to_c(const ast::Program& program, std::string_view parc_file) {
    return Translator(program, parc_file).run();
}

}  // namespace parc::metrics
