#include "parser/parser.hpp"

#include <algorithm>
#include <string>

namespace parc {
namespace {

using namespace ast;

Pos pos_of(const Token& t) { return {t.line, t.col}; }

// "';'", "a name 'sum'", "end of file"
std::string describe(const Token& t) {
    switch (t.kind) {
        case Kind::Ident: return "a name '" + t.text + "'";
        case Kind::Int:
        case Kind::Float: return "the number " + t.text;
        case Kind::String: return "a string";
        case Kind::Eof: return "the end of the file";
        default: return "'" + t.text + "'";
    }
}

// Operator precedence for precedence climbing; 0 means "not a binary operator".
int precedence(Kind k) {
    switch (k) {
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
        case Kind::Star:
        case Kind::Slash:
        case Kind::Percent: return 6;
        default: return 0;
    }
}

bool starts_statement(Kind k) {
    switch (k) {
        case Kind::Let:
        case Kind::If:
        case Kind::While:
        case Kind::For:
        case Kind::Parallel:
        case Kind::Return:
        case Kind::Break:
        case Kind::Continue:
        case Kind::LBrace: return true;
        default: return false;
    }
}

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens) : toks_(tokens) {}

    ParseResult run() {
        ParseResult result;
        while (!check(Kind::Eof)) {
            if (check(Kind::Fn)) {
                try {
                    result.program.functions.push_back(parse_function());
                } catch (const Bail&) {
                    sync_to_function();
                }
            } else {
                error(peek(), "expected 'fn' to start a function, found " + describe(peek()),
                      "all code must be inside a function");
                advance();
                sync_to_function();
            }
        }
        result.errors = std::move(errors_);
        return result;
    }

private:
    // Thrown to unwind to the nearest recovery point (a statement or a function).
    struct Bail {};

    const std::vector<Token>& toks_;
    size_t pos_ = 0;
    std::vector<Diagnostic> errors_;

    // ------------------------------------------------------------ token helpers

    const Token& peek(size_t ahead = 0) const { return toks_[std::min(pos_ + ahead, toks_.size() - 1)]; }
    const Token& previous() const { return toks_[pos_ ? pos_ - 1 : 0]; }
    bool check(Kind k) const { return peek().kind == k; }

    const Token& advance() {
        const Token& t = peek();
        if (t.kind != Kind::Eof) ++pos_;
        return t;
    }

    bool match(Kind k) {
        if (!check(k)) return false;
        advance();
        return true;
    }

    // ------------------------------------------------------------ errors and recovery

    void error(int line, int col, std::string message, std::string hint = "") {
        // Several recovery points can trip over the same spot; report it once.
        if (!errors_.empty() && errors_.back().line == line && errors_.back().col == col) return;
        Diagnostic d;
        d.phase = "parser";
        d.severity = "error";
        d.line = line;
        d.col = col;
        d.message = std::move(message);
        d.hint = std::move(hint);
        errors_.push_back(std::move(d));
    }

    void error(const Token& at, std::string message, std::string hint = "") {
        error(at.line, at.col, std::move(message), std::move(hint));
    }

    [[noreturn]] void fail(const Token& at, std::string message, std::string hint = "") {
        error(at, std::move(message), std::move(hint));
        throw Bail{};
    }

    [[noreturn]] void fail(Pos at, std::string message, std::string hint = "") {
        error(at.line, at.col, std::move(message), std::move(hint));
        throw Bail{};
    }

    const Token& expect(Kind k, const std::string& message, const std::string& hint = "") {
        if (check(k)) return advance();
        fail(peek(), message + ", found " + describe(peek()), hint);
    }

    // A missing ';' is reported right after the previous token, where it belongs,
    // instead of at the start of the next line.
    void expect_semi(const std::string& after) {
        if (match(Kind::Semi)) return;
        const Token& prev = previous();
        fail(Pos{prev.line, prev.col + static_cast<int>(prev.text.size())}, "expected ';' after " + after,
             "found " + describe(peek()) + " instead");
    }

    // Panic mode: skip to a point where a new statement can start.
    void sync_statement() {
        while (!check(Kind::Eof)) {
            if (match(Kind::Semi)) return;
            if (check(Kind::RBrace) || check(Kind::Fn) || starts_statement(peek().kind)) return;
            advance();
        }
    }

    void sync_to_function() {
        while (!check(Kind::Fn) && !check(Kind::Eof)) advance();
    }

    // ------------------------------------------------------------ declarations

    Type parse_type() {
        Type t;
        if (match(Kind::IntType)) {
            t.base = BaseType::Int;
        } else if (match(Kind::FloatType)) {
            t.base = BaseType::Float;
        } else if (match(Kind::BoolType)) {
            t.base = BaseType::Bool;
        } else {
            fail(peek(), "expected a type (int, float or bool), found " + describe(peek()));
        }
        if (match(Kind::LBracket)) {
            expect(Kind::RBracket, "expected ']' after '[' in an array type");
            t.is_array = true;
        }
        return t;
    }

    Function parse_function() {
        Function fn;
        fn.pos = pos_of(advance());  // 'fn'
        const Token& fn_name = expect(Kind::Ident, "expected a function name after 'fn'");
        fn.name = fn_name.text;
        fn.name_pos = pos_of(fn_name);
        expect(Kind::LParen, "expected '(' after the function name");
        if (!check(Kind::RParen)) {
            do {
                const Token& name = expect(Kind::Ident, "expected a parameter name");
                expect(Kind::Colon, "expected ':' and a type after the parameter name", "parameters look like n: int");
                fn.params.push_back(Param{name.text, parse_type(), pos_of(name)});
            } while (match(Kind::Comma));
        }
        expect(Kind::RParen, "expected ')' to close the parameter list");
        if (match(Kind::Arrow)) fn.return_type = parse_type();
        fn.body = parse_block("to start the function body");
        return fn;
    }

    // ------------------------------------------------------------ statements

    std::unique_ptr<Block> parse_block(const std::string& purpose) {
        const Token& open = expect(Kind::LBrace, "expected '{' " + purpose);
        auto block = std::make_unique<Block>(pos_of(open));
        while (!check(Kind::RBrace) && !check(Kind::Eof) && !check(Kind::Fn)) {
            try {
                block->stmts.push_back(parse_statement());
            } catch (const Bail&) {
                sync_statement();
            }
        }
        if (!match(Kind::RBrace)) {
            error(peek(),
                  "expected '}' to close the block opened at " + std::to_string(open.line) + ":" +
                      std::to_string(open.col) + ", found " + describe(peek()));
        }
        return block;
    }

    StmtPtr parse_statement() {
        const Token& t = peek();
        switch (t.kind) {
            case Kind::Let: return parse_let();
            case Kind::If: return parse_if();
            case Kind::While: return parse_while();
            case Kind::For:
                advance();
                return parse_for(pos_of(t), false);
            case Kind::Parallel:
                advance();
                expect(Kind::For, "expected 'for' after 'parallel'");
                return parse_for(pos_of(t), true);
            case Kind::Return: {
                advance();
                ExprPtr value = check(Kind::Semi) ? nullptr : parse_expr();
                expect_semi("the return statement");
                return std::make_unique<Return>(pos_of(t), std::move(value));
            }
            case Kind::Break:
                advance();
                expect_semi("'break'");
                return std::make_unique<Break>(pos_of(t));
            case Kind::Continue:
                advance();
                expect_semi("'continue'");
                return std::make_unique<Continue>(pos_of(t));
            case Kind::LBrace: return parse_block("");
            default: return parse_simple_statement();
        }
    }

    StmtPtr parse_let() {
        Pos at = pos_of(advance());  // 'let'
        const Token& name_token = expect(Kind::Ident, "expected a variable name after 'let'");
        std::string name = name_token.text;
        Pos name_pos = pos_of(name_token);
        expect(Kind::Colon, "expected ':' and a type after the variable name", "ParC needs a type: let x: int = 0;");
        Type type = parse_type();
        ExprPtr init = match(Kind::Assign) ? parse_expr() : nullptr;
        expect_semi("the let statement");
        return std::make_unique<Let>(at, std::move(name), name_pos, type, std::move(init));
    }

    std::unique_ptr<If> parse_if() {
        Pos at = pos_of(advance());  // 'if'
        expect(Kind::LParen, "expected '(' after 'if'", "conditions go in parentheses: if (x > 0) { ... }");
        ExprPtr cond = parse_expr();
        expect(Kind::RParen, "expected ')' after the condition");
        auto then_block = parse_block("after the condition", "ParC always needs braces: if (x) { ... }");
        StmtPtr else_branch;
        if (match(Kind::Else)) {
            if (check(Kind::If))
                else_branch = parse_if();
            else
                else_branch = parse_block("after 'else'", "ParC always needs braces: else { ... }");
        }
        return std::make_unique<If>(at, std::move(cond), std::move(then_block), std::move(else_branch));
    }

    std::unique_ptr<Block> parse_block(const std::string& purpose, const std::string& hint) {
        if (!check(Kind::LBrace)) fail(peek(), "expected '{' " + purpose + ", found " + describe(peek()), hint);
        return parse_block(purpose);
    }

    StmtPtr parse_while() {
        Pos at = pos_of(advance());  // 'while'
        expect(Kind::LParen, "expected '(' after 'while'", "conditions go in parentheses: while (i < n) { ... }");
        ExprPtr cond = parse_expr();
        expect(Kind::RParen, "expected ')' after the condition");
        auto body = parse_block("after the condition", "ParC always needs braces: while (x) { ... }");
        return std::make_unique<While>(at, std::move(cond), std::move(body));
    }

    // Called after "for" or "parallel for" has been consumed.
    StmtPtr parse_for(Pos at, bool parallel) {
        auto loop = std::make_unique<For>(at);
        loop->parallel = parallel;
        const Token& var = expect(Kind::Ident, "expected a loop variable after 'for'");
        loop->var = var.text;
        loop->var_pos = pos_of(var);
        expect(Kind::In, "expected 'in' after the loop variable", "loops look like: for i in 0..n { ... }");
        loop->start = parse_expr();
        expect(Kind::DotDot, "expected '..' in the loop range", "ranges are written start..end");
        loop->end = parse_expr();

        if (check(Kind::Reduce)) {
            if (!parallel) fail(peek(), "'reduce' only works on a 'parallel for' loop");
            advance();
            expect(Kind::LParen, "expected '(' after 'reduce'");
            if (match(Kind::Plus) || match(Kind::Star)) {
                loop->reduce_op = previous().kind;
            } else {
                fail(peek(), "expected '+' or '*' in reduce(...), found " + describe(peek()),
                     "reductions look like reduce(+: sum)");
            }
            expect(Kind::Colon, "expected ':' after the reduce operator");
            do {
                const Token& name = expect(Kind::Ident, "expected a variable name in reduce(...)");
                loop->reduce_vars.push_back(name.text);
                loop->reduce_pos.push_back(pos_of(name));
            } while (match(Kind::Comma));
            expect(Kind::RParen, "expected ')' to close reduce(...)");
        }

        loop->body = parse_block("to start the loop body", "loops look like: for i in 0..n { ... }");
        return loop;
    }

    StmtPtr parse_simple_statement() {
        Pos at = pos_of(peek());
        ExprPtr lhs = parse_expr();
        if (match(Kind::Assign)) {
            if (lhs->kind != ExprKind::Name && lhs->kind != ExprKind::Index) {
                fail(lhs->pos, "can't assign to this expression",
                     "the left side of '=' must be a variable or an array element like a[i]");
            }
            ExprPtr value = parse_expr();
            expect_semi("the assignment");
            return std::make_unique<Assign>(at, std::move(lhs), std::move(value));
        }
        expect_semi("the expression");
        return std::make_unique<ExprStmt>(at, std::move(lhs));
    }

    // ------------------------------------------------------------ expressions

    // Precedence climbing: parse operators that bind at least as tightly as min_prec.
    ExprPtr parse_expr(int min_prec = 1) {
        ExprPtr lhs = parse_unary();
        while (true) {
            int prec = precedence(peek().kind);
            if (prec == 0 || prec < min_prec) return lhs;
            const Token& op = advance();
            ExprPtr rhs = parse_expr(prec + 1);  // prec + 1 makes operators left-associative
            lhs = std::make_unique<Binary>(pos_of(op), op.kind, std::move(lhs), std::move(rhs));
        }
    }

    ExprPtr parse_unary() {
        if (check(Kind::Minus) || check(Kind::Not)) {
            const Token& op = advance();
            ExprPtr operand = parse_unary();
            return std::make_unique<Unary>(pos_of(op), op.kind, std::move(operand));
        }
        return parse_postfix();
    }

    ExprPtr parse_postfix() {
        ExprPtr e = parse_primary();
        while (true) {
            if (check(Kind::LBracket)) {
                Pos at = pos_of(advance());
                ExprPtr index = parse_expr();
                expect(Kind::RBracket, "expected ']' after the index");
                e = std::make_unique<Index>(at, std::move(e), std::move(index));
            } else if (check(Kind::LParen)) {
                if (e->kind != ExprKind::Name) fail(peek(), "only a named function can be called");
                advance();
                std::vector<ExprPtr> args;
                if (!check(Kind::RParen)) {
                    do {
                        args.push_back(parse_expr());
                    } while (match(Kind::Comma));
                }
                expect(Kind::RParen, "expected ')' after the call's arguments");
                const auto& callee = as<Name>(*e);
                e = std::make_unique<Call>(callee.pos, callee.name, std::move(args));
            } else {
                return e;
            }
        }
    }

    ExprPtr parse_primary() {
        const Token& t = peek();
        Pos at = pos_of(t);
        switch (t.kind) {
            case Kind::Int: advance(); return std::make_unique<IntLit>(at, std::get<long long>(t.value));
            case Kind::Float: advance(); return std::make_unique<FloatLit>(at, std::get<double>(t.value));
            case Kind::String: advance(); return std::make_unique<StringLit>(at, std::get<std::string>(t.value));
            case Kind::True: advance(); return std::make_unique<BoolLit>(at, true);
            case Kind::False: advance(); return std::make_unique<BoolLit>(at, false);
            case Kind::Null: advance(); return std::make_unique<NullLit>(at);
            case Kind::Ident: advance(); return std::make_unique<Name>(at, t.text);
            case Kind::New: {
                advance();
                BaseType elem = BaseType::Int;
                if (match(Kind::IntType)) {
                    elem = BaseType::Int;
                } else if (match(Kind::FloatType)) {
                    elem = BaseType::Float;
                } else if (match(Kind::BoolType)) {
                    elem = BaseType::Bool;
                } else {
                    fail(peek(), "expected int, float or bool after 'new', found " + describe(peek()),
                         "arrays are created like new int[n]");
                }
                expect(Kind::LBracket, "expected '[' and a size after the type", "arrays are created like new int[n]");
                ExprPtr size = parse_expr();
                expect(Kind::RBracket, "expected ']' after the array size");
                return std::make_unique<NewArray>(at, elem, std::move(size));
            }
            case Kind::LParen: {
                advance();
                ExprPtr inner = parse_expr();
                expect(Kind::RParen, "expected ')' to close the '(' at " + std::to_string(at.line) + ":" +
                                         std::to_string(at.col));
                return inner;
            }
            default: fail(t, "expected an expression, found " + describe(t));
        }
    }
};

}  // namespace

ParseResult parse(const std::vector<Token>& tokens) { return Parser(tokens).run(); }

}  // namespace parc
