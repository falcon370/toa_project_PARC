#include "parser/ast.hpp"

#include "common/json.hpp"
#include "common/json_writer.hpp"

namespace parc::ast {

std::string_view to_string(BaseType base) {
    switch (base) {
        case BaseType::Int: return "int";
        case BaseType::Float: return "float";
        case BaseType::Bool: return "bool";
        case BaseType::Void: return "void";
    }
    return "?";
}

std::string to_string(const Type& type) {
    std::string out(to_string(type.base));
    return type.is_array ? out + "[]" : out;
}

Pos start_of(const Expr& expr) {
    switch (expr.kind) {
        case ExprKind::Binary: return start_of(*as<Binary>(expr).lhs);
        case ExprKind::Index: return start_of(*as<Index>(expr).array);
        default: return expr.pos;
    }
}

namespace {

// Binding strength, matching the parser: a higher number binds tighter.
int source_precedence(const Expr& e) {
    if (e.kind == ExprKind::Unary) return 7;
    if (e.kind != ExprKind::Binary) return 8;
    switch (as<Binary>(e).op) {
        case Kind::Or: return 1;
        case Kind::And: return 2;
        case Kind::Eq:
        case Kind::Ne: return 3;
        case Kind::Lt:
        case Kind::Le:
        case Kind::Gt:
        case Kind::Ge: return 4;
        case Kind::Plus:
        case Kind::Minus: return 5;
        default: return 6;
    }
}

// Adds parentheses only where the tree needs them: (a + b) * c.
std::string source_operand(const Expr& e, int min_precedence) {
    std::string text = to_source(e);
    return source_precedence(e) < min_precedence ? "(" + text + ")" : text;
}

}  // namespace

std::string to_source(const Expr& expr) {
    switch (expr.kind) {
        case ExprKind::IntLit: return std::to_string(as<IntLit>(expr).value);
        case ExprKind::FloatLit: return json::number(as<FloatLit>(expr).value);
        case ExprKind::StringLit: return json::quote(as<StringLit>(expr).value);
        case ExprKind::BoolLit: return as<BoolLit>(expr).value ? "true" : "false";
        case ExprKind::NullLit: return "null";
        case ExprKind::Name: return as<Name>(expr).name;
        case ExprKind::Unary: {
            const auto& u = as<Unary>(expr);
            return std::string(kind_symbol(u.op)) + source_operand(*u.operand, 7);
        }
        case ExprKind::Binary: {
            const auto& b = as<Binary>(expr);
            int p = source_precedence(expr);
            // Operators are left-associative, so the right side needs parentheses at equal precedence.
            return source_operand(*b.lhs, p) + " " + std::string(kind_symbol(b.op)) + " " +
                   source_operand(*b.rhs, p + 1);
        }
        case ExprKind::Index: {
            const auto& ix = as<Index>(expr);
            return source_operand(*ix.array, 8) + "[" + to_source(*ix.index) + "]";
        }
        case ExprKind::Call: {
            const auto& c = as<Call>(expr);
            std::string out = c.callee + "(";
            for (size_t i = 0; i < c.args.size(); ++i) out += (i ? ", " : "") + to_source(*c.args[i]);
            return out + ")";
        }
        case ExprKind::NewArray: {
            const auto& n = as<NewArray>(expr);
            return "new " + std::string(to_string(n.elem)) + "[" + to_source(*n.size) + "]";
        }
    }
    return "?";
}

namespace {

std::string pos_suffix(Pos p) { return "  @" + std::to_string(p.line) + ":" + std::to_string(p.col); }

std::string reduce_text(const For& f) {
    std::string out = "reduce(" + std::string(kind_symbol(f.reduce_op)) + ": ";
    for (size_t i = 0; i < f.reduce_vars.size(); ++i) out += (i ? ", " : "") + f.reduce_vars[i];
    return out + ")";
}

// ---------------------------------------------------------------- text tree

class TreePrinter {
public:
    std::string out;

    void function(const Function& fn) {
        std::string sig = "Function " + fn.name + "(";
        for (size_t i = 0; i < fn.params.size(); ++i)
            sig += (i ? ", " : "") + fn.params[i].name + ": " + to_string(fn.params[i].type);
        sig += ") -> " + to_string(fn.return_type);
        line(0, "", sig, fn.pos);
        stmt(*fn.body, 1, "");
    }

private:
    void line(int depth, std::string_view label, const std::string& text, Pos p) {
        out.append(2 * depth, ' ');
        if (!label.empty()) out += std::string(label) + ": ";
        out += text + pos_suffix(p) + "\n";
    }

    void expr(const Expr& e, int depth, std::string_view label) {
        switch (e.kind) {
            case ExprKind::IntLit: line(depth, label, "Int " + std::to_string(as<IntLit>(e).value), e.pos); break;
            case ExprKind::FloatLit: line(depth, label, "Float " + json::number(as<FloatLit>(e).value), e.pos); break;
            case ExprKind::StringLit: line(depth, label, "String " + json::quote(as<StringLit>(e).value), e.pos); break;
            case ExprKind::BoolLit: line(depth, label, as<BoolLit>(e).value ? "Bool true" : "Bool false", e.pos); break;
            case ExprKind::NullLit: line(depth, label, "Null", e.pos); break;
            case ExprKind::Name: line(depth, label, "Name " + as<Name>(e).name, e.pos); break;
            case ExprKind::Unary: {
                const auto& u = as<Unary>(e);
                line(depth, label, "Unary " + std::string(kind_symbol(u.op)), e.pos);
                expr(*u.operand, depth + 1, "operand");
                break;
            }
            case ExprKind::Binary: {
                const auto& b = as<Binary>(e);
                line(depth, label, "Binary " + std::string(kind_symbol(b.op)), e.pos);
                expr(*b.lhs, depth + 1, "lhs");
                expr(*b.rhs, depth + 1, "rhs");
                break;
            }
            case ExprKind::Index: {
                const auto& ix = as<Index>(e);
                line(depth, label, "Index", e.pos);
                expr(*ix.array, depth + 1, "array");
                expr(*ix.index, depth + 1, "index");
                break;
            }
            case ExprKind::Call: {
                const auto& c = as<Call>(e);
                line(depth, label, "Call " + c.callee, e.pos);
                for (const auto& a : c.args) expr(*a, depth + 1, "arg");
                break;
            }
            case ExprKind::NewArray: {
                const auto& n = as<NewArray>(e);
                line(depth, label, "New " + std::string(to_string(n.elem)) + "[]", e.pos);
                expr(*n.size, depth + 1, "size");
                break;
            }
        }
    }

    void stmt(const Stmt& s, int depth, std::string_view label) {
        switch (s.kind) {
            case StmtKind::Block: {
                line(depth, label, "Block", s.pos);
                for (const auto& child : as<Block>(s).stmts) stmt(*child, depth + 1, "");
                break;
            }
            case StmtKind::Let: {
                const auto& l = as<Let>(s);
                line(depth, label, "Let " + l.name + ": " + to_string(l.type), s.pos);
                if (l.init) expr(*l.init, depth + 1, "init");
                break;
            }
            case StmtKind::Assign: {
                const auto& a = as<Assign>(s);
                line(depth, label, "Assign", s.pos);
                expr(*a.target, depth + 1, "target");
                expr(*a.value, depth + 1, "value");
                break;
            }
            case StmtKind::ExprStmt:
                line(depth, label, "ExprStmt", s.pos);
                expr(*as<ExprStmt>(s).expr, depth + 1, "");
                break;
            case StmtKind::If: {
                const auto& i = as<If>(s);
                line(depth, label, "If", s.pos);
                expr(*i.cond, depth + 1, "cond");
                stmt(*i.then_block, depth + 1, "then");
                if (i.else_branch) stmt(*i.else_branch, depth + 1, "else");
                break;
            }
            case StmtKind::While: {
                const auto& w = as<While>(s);
                line(depth, label, "While", s.pos);
                expr(*w.cond, depth + 1, "cond");
                stmt(*w.body, depth + 1, "body");
                break;
            }
            case StmtKind::For: {
                const auto& f = as<For>(s);
                std::string text = (f.parallel ? "ParallelFor " : "For ") + f.var;
                if (!f.reduce_vars.empty()) text += " " + reduce_text(f);
                line(depth, label, text, s.pos);
                expr(*f.start, depth + 1, "start");
                expr(*f.end, depth + 1, "end");
                stmt(*f.body, depth + 1, "body");
                break;
            }
            case StmtKind::Return: {
                line(depth, label, "Return", s.pos);
                if (const auto& v = as<Return>(s).value) expr(*v, depth + 1, "value");
                break;
            }
            case StmtKind::Break: line(depth, label, "Break", s.pos); break;
            case StmtKind::Continue: line(depth, label, "Continue", s.pos); break;
        }
    }
};

// ---------------------------------------------------------------- JSON

class JsonPrinter {
public:
    json::Writer w;

    void function(const Function& fn) {
        w.begin_object();
        header("Function", fn.pos);
        w.key("name");
        w.string(fn.name);
        w.key("params");
        w.begin_array();
        for (const auto& p : fn.params) {
            w.begin_object();
            w.key("name");
            w.string(p.name);
            w.key("type");
            w.string(to_string(p.type));
            w.key("line");
            w.integer(p.pos.line);
            w.key("col");
            w.integer(p.pos.col);
            w.end_object();
        }
        w.end_array();
        w.key("return_type");
        w.string(to_string(fn.return_type));
        w.key("body");
        stmt(*fn.body);
        w.end_object();
    }

private:
    void header(std::string_view node, Pos p) {
        w.key("node");
        w.string(node);
        w.key("line");
        w.integer(p.line);
        w.key("col");
        w.integer(p.col);
    }

    void field(std::string_view name, const Expr* e) {
        w.key(name);
        if (e)
            expr(*e);
        else
            w.null();
    }

    void expr(const Expr& e) {
        w.begin_object();
        switch (e.kind) {
            case ExprKind::IntLit:
                header("Int", e.pos);
                w.key("value");
                w.integer(as<IntLit>(e).value);
                break;
            case ExprKind::FloatLit:
                header("Float", e.pos);
                w.key("value");
                w.number(as<FloatLit>(e).value);
                break;
            case ExprKind::StringLit:
                header("String", e.pos);
                w.key("value");
                w.string(as<StringLit>(e).value);
                break;
            case ExprKind::BoolLit:
                header("Bool", e.pos);
                w.key("value");
                w.boolean(as<BoolLit>(e).value);
                break;
            case ExprKind::NullLit: header("Null", e.pos); break;
            case ExprKind::Name:
                header("Name", e.pos);
                w.key("name");
                w.string(as<Name>(e).name);
                break;
            case ExprKind::Unary: {
                const auto& u = as<Unary>(e);
                header("Unary", e.pos);
                w.key("op");
                w.string(kind_symbol(u.op));
                field("operand", u.operand.get());
                break;
            }
            case ExprKind::Binary: {
                const auto& b = as<Binary>(e);
                header("Binary", e.pos);
                w.key("op");
                w.string(kind_symbol(b.op));
                field("lhs", b.lhs.get());
                field("rhs", b.rhs.get());
                break;
            }
            case ExprKind::Index: {
                const auto& ix = as<Index>(e);
                header("Index", e.pos);
                field("array", ix.array.get());
                field("index", ix.index.get());
                break;
            }
            case ExprKind::Call: {
                const auto& c = as<Call>(e);
                header("Call", e.pos);
                w.key("callee");
                w.string(c.callee);
                w.key("args");
                w.begin_array();
                for (const auto& a : c.args) expr(*a);
                w.end_array();
                break;
            }
            case ExprKind::NewArray: {
                const auto& n = as<NewArray>(e);
                header("New", e.pos);
                w.key("elem");
                w.string(to_string(n.elem));
                field("size", n.size.get());
                break;
            }
        }
        w.end_object();
    }

    void stmt(const Stmt& s) {
        w.begin_object();
        switch (s.kind) {
            case StmtKind::Block:
                header("Block", s.pos);
                w.key("stmts");
                w.begin_array();
                for (const auto& child : as<Block>(s).stmts) stmt(*child);
                w.end_array();
                break;
            case StmtKind::Let: {
                const auto& l = as<Let>(s);
                header("Let", s.pos);
                w.key("name");
                w.string(l.name);
                w.key("type");
                w.string(to_string(l.type));
                field("init", l.init.get());
                break;
            }
            case StmtKind::Assign: {
                const auto& a = as<Assign>(s);
                header("Assign", s.pos);
                field("target", a.target.get());
                field("value", a.value.get());
                break;
            }
            case StmtKind::ExprStmt:
                header("ExprStmt", s.pos);
                field("expr", as<ExprStmt>(s).expr.get());
                break;
            case StmtKind::If: {
                const auto& i = as<If>(s);
                header("If", s.pos);
                field("cond", i.cond.get());
                w.key("then");
                stmt(*i.then_block);
                w.key("else");
                if (i.else_branch)
                    stmt(*i.else_branch);
                else
                    w.null();
                break;
            }
            case StmtKind::While: {
                const auto& wh = as<While>(s);
                header("While", s.pos);
                field("cond", wh.cond.get());
                w.key("body");
                stmt(*wh.body);
                break;
            }
            case StmtKind::For: {
                const auto& f = as<For>(s);
                header(f.parallel ? "ParallelFor" : "For", s.pos);
                w.key("var");
                w.string(f.var);
                field("start", f.start.get());
                field("end", f.end.get());
                if (!f.reduce_vars.empty()) {
                    w.key("reduce_op");
                    w.string(kind_symbol(f.reduce_op));
                    w.key("reduce_vars");
                    w.begin_array();
                    for (const auto& v : f.reduce_vars) w.string(v);
                    w.end_array();
                }
                w.key("body");
                stmt(*f.body);
                break;
            }
            case StmtKind::Return:
                header("Return", s.pos);
                field("value", as<Return>(s).value.get());
                break;
            case StmtKind::Break: header("Break", s.pos); break;
            case StmtKind::Continue: header("Continue", s.pos); break;
        }
        w.end_object();
    }
};

}  // namespace

std::string to_tree(const Program& program) {
    TreePrinter p;
    for (const auto& fn : program.functions) p.function(fn);
    return p.out;
}

std::string to_json(const Program& program, std::string_view filename, const std::vector<Diagnostic>& errors) {
    JsonPrinter p;
    p.w.begin_object();
    p.w.key("file");
    p.w.string(filename);
    p.w.key("functions");
    p.w.begin_array();
    for (const auto& fn : program.functions) p.function(fn);
    p.w.end_array();
    p.w.key("errors");
    p.w.begin_array();
    for (const auto& e : errors) {
        p.w.begin_object();
        p.w.key("phase");
        p.w.string(e.phase);
        p.w.key("severity");
        p.w.string(e.severity);
        p.w.key("line");
        p.w.integer(e.line);
        p.w.key("col");
        p.w.integer(e.col);
        p.w.key("message");
        p.w.string(e.message);
        if (!e.hint.empty()) {
            p.w.key("hint");
            p.w.string(e.hint);
        }
        p.w.end_object();
    }
    p.w.end_array();
    p.w.end_object();
    return p.w.str() + "\n";
}

}  // namespace parc::ast
