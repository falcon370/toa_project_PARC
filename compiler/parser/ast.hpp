#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/diagnostic.hpp"
#include "scanner/token.hpp"

namespace parc::ast {

struct Pos {
    int line = 0;
    int col = 0;
};

enum class BaseType { Int, Float, Bool, Void };

struct Type {
    BaseType base = BaseType::Void;
    bool is_array = false;
};

std::string to_string(const Type& type);  // "int", "float[]", "void"
std::string_view to_string(BaseType base);

// ---------------------------------------------------------------- expressions

enum class ExprKind { IntLit, FloatLit, StringLit, BoolLit, NullLit, Name, Unary, Binary, Index, Call, NewArray };

struct Expr {
    ExprKind kind;
    Pos pos;
    Expr(ExprKind k, Pos p) : kind(k), pos(p) {}
    virtual ~Expr() = default;
};
using ExprPtr = std::unique_ptr<Expr>;

struct IntLit : Expr {
    long long value;
    IntLit(Pos p, long long v) : Expr(ExprKind::IntLit, p), value(v) {}
};

struct FloatLit : Expr {
    double value;
    FloatLit(Pos p, double v) : Expr(ExprKind::FloatLit, p), value(v) {}
};

struct StringLit : Expr {
    std::string value;
    StringLit(Pos p, std::string v) : Expr(ExprKind::StringLit, p), value(std::move(v)) {}
};

struct BoolLit : Expr {
    bool value;
    BoolLit(Pos p, bool v) : Expr(ExprKind::BoolLit, p), value(v) {}
};

struct NullLit : Expr {
    explicit NullLit(Pos p) : Expr(ExprKind::NullLit, p) {}
};

struct Name : Expr {
    std::string name;
    Name(Pos p, std::string n) : Expr(ExprKind::Name, p), name(std::move(n)) {}
};

struct Unary : Expr {
    Kind op;  // Minus or Not
    ExprPtr operand;
    Unary(Pos p, Kind o, ExprPtr e) : Expr(ExprKind::Unary, p), op(o), operand(std::move(e)) {}
};

// pos is the operator's position, so "divide by zero" can point at the '/'.
struct Binary : Expr {
    Kind op;
    ExprPtr lhs;
    ExprPtr rhs;
    Binary(Pos p, Kind o, ExprPtr l, ExprPtr r)
        : Expr(ExprKind::Binary, p), op(o), lhs(std::move(l)), rhs(std::move(r)) {}
};

struct Index : Expr {
    ExprPtr array;
    ExprPtr index;
    Index(Pos p, ExprPtr a, ExprPtr i) : Expr(ExprKind::Index, p), array(std::move(a)), index(std::move(i)) {}
};

struct Call : Expr {
    std::string callee;
    std::vector<ExprPtr> args;
    Call(Pos p, std::string c, std::vector<ExprPtr> a)
        : Expr(ExprKind::Call, p), callee(std::move(c)), args(std::move(a)) {}
};

struct NewArray : Expr {
    BaseType elem;
    ExprPtr size;
    NewArray(Pos p, BaseType e, ExprPtr s) : Expr(ExprKind::NewArray, p), elem(e), size(std::move(s)) {}
};

// ---------------------------------------------------------------- statements

enum class StmtKind { Let, Assign, ExprStmt, If, While, For, Return, Break, Continue, Block };

struct Stmt {
    StmtKind kind;
    Pos pos;
    Stmt(StmtKind k, Pos p) : kind(k), pos(p) {}
    virtual ~Stmt() = default;
};
using StmtPtr = std::unique_ptr<Stmt>;

struct Block : Stmt {
    std::vector<StmtPtr> stmts;
    explicit Block(Pos p) : Stmt(StmtKind::Block, p) {}
};

struct Let : Stmt {
    std::string name;
    Pos name_pos;
    Type type;
    ExprPtr init;  // may be null
    Let(Pos p, std::string n, Pos np, Type t, ExprPtr i)
        : Stmt(StmtKind::Let, p), name(std::move(n)), name_pos(np), type(t), init(std::move(i)) {}
};

struct Assign : Stmt {
    ExprPtr target;  // Name or Index
    ExprPtr value;
    Assign(Pos p, ExprPtr t, ExprPtr v) : Stmt(StmtKind::Assign, p), target(std::move(t)), value(std::move(v)) {}
};

struct ExprStmt : Stmt {
    ExprPtr expr;
    ExprStmt(Pos p, ExprPtr e) : Stmt(StmtKind::ExprStmt, p), expr(std::move(e)) {}
};

struct If : Stmt {
    ExprPtr cond;
    std::unique_ptr<Block> then_block;
    StmtPtr else_branch;  // null, a Block, or another If for "else if"
    If(Pos p, ExprPtr c, std::unique_ptr<Block> t, StmtPtr e)
        : Stmt(StmtKind::If, p), cond(std::move(c)), then_block(std::move(t)), else_branch(std::move(e)) {}
};

struct While : Stmt {
    ExprPtr cond;
    std::unique_ptr<Block> body;
    While(Pos p, ExprPtr c, std::unique_ptr<Block> b)
        : Stmt(StmtKind::While, p), cond(std::move(c)), body(std::move(b)) {}
};

// Both "for" and "parallel for". The range start..end is half-open.
struct For : Stmt {
    bool parallel = false;
    std::string var;
    Pos var_pos;
    ExprPtr start;
    ExprPtr end;
    Kind reduce_op = Kind::Eof;  // Plus or Star when a reduce(...) clause is present
    std::vector<std::string> reduce_vars;
    std::vector<Pos> reduce_pos;  // one per reduce variable
    std::unique_ptr<Block> body;
    explicit For(Pos p) : Stmt(StmtKind::For, p) {}
};

struct Return : Stmt {
    ExprPtr value;  // may be null
    Return(Pos p, ExprPtr v) : Stmt(StmtKind::Return, p), value(std::move(v)) {}
};

struct Break : Stmt {
    explicit Break(Pos p) : Stmt(StmtKind::Break, p) {}
};

struct Continue : Stmt {
    explicit Continue(Pos p) : Stmt(StmtKind::Continue, p) {}
};

// ---------------------------------------------------------------- program

struct Param {
    std::string name;
    Type type;
    Pos pos;
};

struct Function {
    std::string name;
    Pos pos;       // the 'fn' keyword
    Pos name_pos;
    std::vector<Param> params;
    Type return_type;  // Void when there is no "-> type"
    std::unique_ptr<Block> body;
};

struct Program {
    std::vector<Function> functions;
};

// Read-only downcast once the kind tag has been checked: as<Binary>(expr)
template <typename T, typename Node>
const T& as(const Node& node) {
    return static_cast<const T&>(node);
}

// The expression written back as source text, for messages: "sum / n".
std::string to_source(const Expr& expr);

// Where an expression begins. A Binary node's own position is its operator,
// so for "a + b" this is the position of a.
Pos start_of(const Expr& expr);

// Dumps for the explorer and for students reading the tree.
// The ast.json file: {"file", "functions": [...], "errors": [...]}
std::string to_json(const Program& program, std::string_view filename, const std::vector<Diagnostic>& errors = {});
std::string to_tree(const Program& program);

}  // namespace parc::ast
