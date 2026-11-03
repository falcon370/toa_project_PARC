#include <filesystem>
#include <fstream>
#include <source_location>
#include <sstream>
#include <string>
#include <vector>

#include "common/test_framework.hpp"
#include "parser/parser.hpp"
#include "scanner/scanner.hpp"

using namespace parc;
using namespace parc::ast;

namespace {

ParseResult parse_source(std::string_view src) { return parse(scan(src).tokens); }

// Compact prefix form for checking tree shape: 1 + 2 * 3 -> (+ 1 (* 2 3))
std::string sexpr(const Expr& e) {
    switch (e.kind) {
        case ExprKind::IntLit: return std::to_string(as<IntLit>(e).value);
        case ExprKind::FloatLit: return std::to_string(as<FloatLit>(e).value);
        case ExprKind::StringLit: return "\"" + as<StringLit>(e).value + "\"";
        case ExprKind::BoolLit: return as<BoolLit>(e).value ? "true" : "false";
        case ExprKind::NullLit: return "null";
        case ExprKind::Name: return as<Name>(e).name;
        case ExprKind::Unary: {
            const auto& u = as<Unary>(e);
            return "(" + std::string(kind_symbol(u.op)) + " " + sexpr(*u.operand) + ")";
        }
        case ExprKind::Binary: {
            const auto& b = as<Binary>(e);
            return "(" + std::string(kind_symbol(b.op)) + " " + sexpr(*b.lhs) + " " + sexpr(*b.rhs) + ")";
        }
        case ExprKind::Index: {
            const auto& ix = as<Index>(e);
            return "(index " + sexpr(*ix.array) + " " + sexpr(*ix.index) + ")";
        }
        case ExprKind::Call: {
            const auto& c = as<Call>(e);
            std::string out = "(call " + c.callee;
            for (const auto& a : c.args) out += " " + sexpr(*a);
            return out + ")";
        }
        case ExprKind::NewArray: {
            const auto& n = as<NewArray>(e);
            return "(new " + std::string(to_string(n.elem)) + " " + sexpr(*n.size) + ")";
        }
    }
    return "?";
}

// Parses `expr` as the value of "let r: int = <expr>;" inside a function.
void expect_expr(const std::string& expr, const std::string& want,
                 std::source_location loc = std::source_location::current()) {
    auto r = parse_source("fn f() { let r: int = " + expr + "; }");
    std::string got = "(parse failed)";
    if (r.errors.empty() && r.program.functions.size() == 1) {
        const auto& stmts = r.program.functions[0].body->stmts;
        if (stmts.size() == 1 && stmts[0]->kind == StmtKind::Let) got = sexpr(*as<Let>(*stmts[0]).init);
    }
    if (got == want) return;
    ++test::failures();
    std::cerr << loc.file_name() << ":" << loc.line() << ": " << expr << "\n    want " << want << "\n    got  " << got
              << "\n";
    for (const auto& e : r.errors) std::cerr << "    " << format(e) << "\n";
}

struct Expected {
    int line;
    int col;
    std::string message;
};

void expect_errors(std::string_view src, const std::vector<Expected>& want,
                   std::source_location loc = std::source_location::current()) {
    auto r = parse_source(src);
    bool same = r.errors.size() == want.size();
    for (size_t i = 0; same && i < want.size(); ++i) {
        same = r.errors[i].line == want[i].line && r.errors[i].col == want[i].col &&
               r.errors[i].message == want[i].message;
    }
    if (same) return;
    ++test::failures();
    std::cerr << loc.file_name() << ":" << loc.line() << ": errors for:\n" << src << "\n    want:\n";
    for (const auto& w : want) std::cerr << "      " << w.line << ":" << w.col << ": " << w.message << "\n";
    std::cerr << "    got:\n";
    for (const auto& e : r.errors) std::cerr << "      " << format(e) << "\n";
}

const Block& body_of(const ParseResult& r, size_t fn) { return *r.program.functions.at(fn).body; }

}  // namespace

// ---------------------------------------------------------------- expressions

TEST_CASE(precedence_and_associativity) {
    expect_expr("1 + 2 * 3", "(+ 1 (* 2 3))");
    expect_expr("(1 + 2) * 3", "(* (+ 1 2) 3)");
    expect_expr("1 - 2 - 3", "(- (- 1 2) 3)");
    expect_expr("8 / 4 / 2", "(/ (/ 8 4) 2)");
    expect_expr("a + b % c", "(+ a (% b c))");
    expect_expr("a || b && c", "(|| a (&& b c))");
    expect_expr("a < b == c >= d", "(== (< a b) (>= c d))");
    expect_expr("x + 1 < y * 2 && !done", "(&& (< (+ x 1) (* y 2)) (! done))");
}

TEST_CASE(unary_operators) {
    expect_expr("-a * b", "(* (- a) b)");
    expect_expr("- -a", "(- (- a))");
    expect_expr("!(a < b)", "(! (< a b))");
}

TEST_CASE(postfix_calls_indexing_and_new) {
    expect_expr("a[i] * f(x, 2)", "(* (index a i) (call f x 2))");
    expect_expr("m[i][j]", "(index (index m i) j)");
    expect_expr("f()", "(call f)");
    expect_expr("new int[n + 1]", "(new int (+ n 1))");
    expect_expr("len(a) - 1", "(- (call len a) 1)");
}

TEST_CASE(literals) {
    expect_expr("true", "true");
    expect_expr("null", "null");
    expect_expr("\"hi\"", "\"hi\"");
}

TEST_CASE(binary_position_is_the_operator) {
    auto r = parse_source("fn f() { let r: int = a / b; }");
    const auto& init = *as<Let>(*body_of(r, 0).stmts[0]).init;
    CHECK(init.kind == ExprKind::Binary);
    CHECK_EQ(init.pos.line, 1);
    CHECK_EQ(init.pos.col, 25);
}

// ---------------------------------------------------------------- statements and functions

TEST_CASE(dot_example_structure) {
    auto r = parse_source(
        "fn dot(a: int[], b: int[], n: int) -> int {\n"
        "    let sum: int = 0;\n"
        "    parallel for i in 0..n reduce(+: sum) {\n"
        "        sum = sum + a[i] * b[i];\n"
        "    }\n"
        "    return sum / n;\n"
        "}\n");
    CHECK(r.errors.empty());
    CHECK_EQ(r.program.functions.size(), size_t{1});
    const Function& fn = r.program.functions[0];
    CHECK_EQ(fn.name, std::string("dot"));
    CHECK_EQ(fn.params.size(), size_t{3});
    CHECK_EQ(to_string(fn.params[0].type), std::string("int[]"));
    CHECK_EQ(to_string(fn.return_type), std::string("int"));

    const auto& stmts = fn.body->stmts;
    CHECK_EQ(stmts.size(), size_t{3});
    CHECK(stmts[0]->kind == StmtKind::Let);
    CHECK(stmts[1]->kind == StmtKind::For);
    CHECK(stmts[2]->kind == StmtKind::Return);

    const auto& loop = as<For>(*stmts[1]);
    CHECK(loop.parallel);
    CHECK_EQ(loop.var, std::string("i"));
    CHECK_EQ(sexpr(*loop.start), std::string("0"));
    CHECK_EQ(sexpr(*loop.end), std::string("n"));
    CHECK(loop.reduce_op == Kind::Plus);
    CHECK(loop.reduce_vars == std::vector<std::string>{"sum"});
    CHECK_EQ(loop.body->stmts.size(), size_t{1});

    const auto& assign = as<Assign>(*loop.body->stmts[0]);
    CHECK_EQ(sexpr(*assign.target), std::string("sum"));
    CHECK_EQ(sexpr(*assign.value), std::string("(+ sum (* (index a i) (index b i)))"));
}

TEST_CASE(else_if_chain) {
    auto r = parse_source("fn f(x: int) { if (x < 0) { } else if (x == 0) { } else { } }");
    CHECK(r.errors.empty());
    const auto& first = as<If>(*body_of(r, 0).stmts[0]);
    CHECK(first.else_branch && first.else_branch->kind == StmtKind::If);
    const auto& second = as<If>(*first.else_branch);
    CHECK(second.else_branch && second.else_branch->kind == StmtKind::Block);
}

TEST_CASE(other_statements) {
    auto r = parse_source(
        "fn f(a: int[]) {\n"
        "  let x: float;\n"
        "  a[0] = 1;\n"
        "  print(\"x\", x);\n"
        "  while (true) { break; continue; }\n"
        "  for i in 0..len(a) { }\n"
        "  { return; }\n"
        "}");
    CHECK(r.errors.empty());
    const auto& s = body_of(r, 0).stmts;
    CHECK_EQ(s.size(), size_t{6});
    CHECK(s[0]->kind == StmtKind::Let && !as<Let>(*s[0]).init);
    CHECK(s[1]->kind == StmtKind::Assign);
    CHECK(s[2]->kind == StmtKind::ExprStmt);
    CHECK(s[3]->kind == StmtKind::While);
    CHECK(s[4]->kind == StmtKind::For && !as<For>(*s[4]).parallel);
    CHECK(s[5]->kind == StmtKind::Block);
}

TEST_CASE(function_without_params_or_return_type) {
    auto r = parse_source("fn main() { }");
    CHECK(r.errors.empty());
    CHECK(r.program.functions[0].params.empty());
    CHECK(r.program.functions[0].return_type.base == BaseType::Void);
}

// ---------------------------------------------------------------- errors and recovery

TEST_CASE(missing_semicolon_points_after_the_previous_token) {
    expect_errors("fn main() { let x: int = 1\n let y: int = 2; }",
                  {{1, 27, "expected ';' after the let statement"}});
    auto r = parse_source("fn main() { let x: int = 1\n let y: int = 2; }");
    CHECK_EQ(body_of(r, 0).stmts.size(), size_t{1});  // the broken let is dropped, the next one survives
}

TEST_CASE(missing_type_in_let) {
    expect_errors("fn main() { let x = 5; }",
                  {{1, 19, "expected ':' and a type after the variable name, found '='"}});
    auto r = parse_source("fn main() { let x = 5; }");
    CHECK_EQ(r.errors[0].hint, std::string("ParC needs a type: let x: int = 0;"));
}

TEST_CASE(several_errors_in_one_run) {
    auto src = "fn main() {\n  let = 1;\n  let y: int = ;\n  let z: int = 3;\n}";
    expect_errors(src, {{2, 7, "expected a variable name after 'let', found '='"},
                        {3, 16, "expected an expression, found ';'"}});
    auto r = parse_source(src);
    const auto& s = body_of(r, 0).stmts;
    CHECK(s.size() == 1 && as<Let>(*s[0]).name == "z");
}

TEST_CASE(bad_assignment_target) {
    expect_errors("fn main() { 1 = 2; }", {{1, 13, "can't assign to this expression"}});
    expect_errors("fn main() { f(x) = 2; }", {{1, 13, "can't assign to this expression"}});
}

TEST_CASE(if_needs_parentheses_and_braces) {
    expect_errors("fn main() { if x > 0 { } }", {{1, 16, "expected '(' after 'if', found a name 'x'"}});
    expect_errors("fn main() { if (x) y = 1; }", {{1, 20, "expected '{' after the condition, found a name 'y'"}});
}

TEST_CASE(missing_closing_brace_keeps_the_next_function) {
    auto src = "fn a() {\n  let x: int = 1;\nfn b() { }";
    expect_errors(src, {{3, 1, "expected '}' to close the block opened at 1:8, found 'fn'"}});
    auto r = parse_source(src);
    CHECK_EQ(r.program.functions.size(), size_t{2});
}

TEST_CASE(code_outside_a_function) {
    auto src = "let x: int = 1;\nfn main() { }";
    expect_errors(src, {{1, 1, "expected 'fn' to start a function, found 'let'"}});
    CHECK_EQ(parse_source(src).program.functions.size(), size_t{1});
}

TEST_CASE(reduce_needs_parallel) {
    expect_errors("fn f() { for i in 0..9 reduce(+: s) { } }",
                  {{1, 24, "'reduce' only works on a 'parallel for' loop"}});
    expect_errors("fn f() { parallel for i in 0..9 reduce(-: s) { } }",
                  {{1, 40, "expected '+' or '*' in reduce(...), found '-'"}});
}

TEST_CASE(loop_syntax_errors) {
    expect_errors("fn f() { for i = 0..9 { } }", {{1, 16, "expected 'in' after the loop variable, found '='"}});
    expect_errors("fn f() { for i in 0, 9 { } }", {{1, 20, "expected '..' in the loop range, found ','"}});
    expect_errors("fn f() { parallel i in 0..9 { } }", {{1, 19, "expected 'for' after 'parallel', found a name 'i'"}});
}

TEST_CASE(only_named_functions_can_be_called) {
    expect_errors("fn f() { a[0](1); }", {{1, 14, "only a named function can be called"}});
}

TEST_CASE(unclosed_parenthesis_and_end_of_file) {
    expect_errors("fn f() { let r: int = (1 + 2; }",
                  {{1, 29, "expected ')' to close the '(' at 1:23, found ';'"}});
    expect_errors("fn f() {", {{1, 9, "expected '}' to close the block opened at 1:8, found the end of the file"}});
}

// ---------------------------------------------------------------- examples and output

TEST_CASE(every_example_parses_without_errors) {
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(PARC_EXAMPLES_DIR)) {
        if (entry.path().extension() != ".parc") continue;
        ++count;
        std::ifstream in(entry.path(), std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        auto r = parse_source(buffer.str());
        for (const auto& e : r.errors) {
            ++test::failures();
            std::cerr << "    " << entry.path().filename().string() << ": " << format(e) << "\n";
        }
        CHECK(!r.program.functions.empty());
    }
    CHECK(count > 0);
}

TEST_CASE(json_and_tree_output) {
    auto r = parse_source("fn f(n: int) -> int { parallel for i in 0..n reduce(+: s) { } return n; }");
    std::string json = to_json(r.program, "t.parc");
    CHECK(json.find("\"node\": \"ParallelFor\"") != std::string::npos);
    CHECK(json.find("\"reduce_op\": \"+\"") != std::string::npos);
    CHECK(json.find("\"return_type\": \"int\"") != std::string::npos);

    std::string tree = to_tree(r.program);
    CHECK(tree.find("Function f(n: int) -> int  @1:1") != std::string::npos);
    CHECK(tree.find("ParallelFor i reduce(+: s)") != std::string::npos);
    CHECK(tree.find("value: Name n") != std::string::npos);
}

TEST_CASE(json_lists_parser_errors) {
    auto good = parse_source("fn f() { }");
    CHECK(to_json(good.program, "t.parc", good.errors).find("\"errors\": []") != std::string::npos);

    auto bad = parse_source("fn f() { let x = 1; }");
    std::string json = to_json(bad.program, "t.parc", bad.errors);
    CHECK(json.find("\"phase\": \"parser\"") != std::string::npos);
    CHECK(json.find("\"col\": 16") != std::string::npos);
    CHECK(json.find("\"hint\": \"ParC needs a type: let x: int = 0;\"") != std::string::npos);
}

int main() { return test::run_all(); }
