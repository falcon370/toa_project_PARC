#include <filesystem>
#include <fstream>
#include <source_location>
#include <sstream>
#include <string>
#include <vector>

#include "common/test_framework.hpp"
#include "parser/parser.hpp"
#include "scanner/scanner.hpp"
#include "semantic/semantic.hpp"

using namespace parc;

namespace {

struct Expected {
    int line;
    int col;
    std::string severity;
    std::string message;
};

struct Analyzed {
    ParseResult parsed;  // owns the tree the result's type table points into
    sem::Result result;
};

Analyzed analyze_source(std::string_view src) {
    Analyzed out;
    out.parsed = parse(scan(src).tokens);
    out.result = sem::analyze(out.parsed.program);
    return out;
}

void expect(std::string_view src, const std::vector<Expected>& want,
            std::source_location loc = std::source_location::current()) {
    Analyzed a = analyze_source(src);
    const auto& got = a.result.diagnostics;
    bool same = a.parsed.errors.empty() && got.size() == want.size();
    for (size_t i = 0; same && i < want.size(); ++i) {
        same = got[i].line == want[i].line && got[i].col == want[i].col && got[i].severity == want[i].severity &&
               got[i].message == want[i].message;
    }
    if (same) return;
    ++test::failures();
    std::cerr << loc.file_name() << ":" << loc.line() << ": diagnostics for:\n" << src << "\n    want:\n";
    for (const auto& w : want)
        std::cerr << "      " << w.line << ":" << w.col << ": " << w.severity << ": " << w.message << "\n";
    std::cerr << "    got:\n";
    for (const auto& e : a.parsed.errors) std::cerr << "      (parser) " << format(e) << "\n";
    for (const auto& d : got) std::cerr << "      " << format(d) << "\n";
}

void expect_clean(std::string_view src, std::source_location loc = std::source_location::current()) {
    expect(src, {}, loc);
}

// The type the checker gave to <expr> in "let r: <type> = <expr>;".
std::string type_of(const std::string& decls, const std::string& expr) {
    Analyzed a = analyze_source("fn g() -> int { return 1; } fn f(n: int, x: float, a: int[], ok: bool) { " + decls +
                                " print(" + expr + "); }");
    const auto& body = a.parsed.program.functions.at(1).body->stmts;
    const auto& call = ast::as<ast::Call>(*ast::as<ast::ExprStmt>(*body.back()).expr);
    return sem::to_string(a.result.types.at(call.args.at(0).get()));
}

const sem::SymbolInfo* find_symbol(const sem::Result& r, const std::string& name) {
    for (const auto& s : r.symbols)
        if (s.name == name) return &s;
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------- scopes

TEST_CASE(undeclared_name_is_reported_once_with_a_suggestion) {
    expect("fn f() { let total: int = 1; print(totl); print(totl); }",
           {{1, 36, "error", "'totl' is not declared"}});
    Analyzed a = analyze_source("fn f() { let total: int = 1; print(totl); }");
    CHECK_EQ(a.result.diagnostics.at(0).hint, "did you mean 'total'?");
    a = analyze_source("fn f() { print(zzz); }");
    CHECK_EQ(a.result.diagnostics.at(0).hint, "declare it first, for example: let zzz: int = 0;");
}

TEST_CASE(redeclaration_in_the_same_scope) {
    expect("fn f() { let x: int = 1; let x: int = 2; print(x); }",
           {{1, 30, "error", "'x' is already declared in this scope as a variable at 1:14"}});
    expect("fn f(n: int) { let n: int = 2; print(n); }",
           {{1, 20, "error", "'n' is already declared in this scope as a parameter at 1:6"}});
    expect("fn f(a: int, a: int) { print(a); }",
           {{1, 14, "error", "'a' is already declared in this scope as a parameter at 1:6"}});
}

TEST_CASE(inner_scope_shadows_with_a_warning) {
    expect("fn f(n: int) { if (n > 0) { let n: int = 2; print(n); } }",
           {{1, 33, "warning", "this 'n' hides the parameter 'n' at 1:6"}});
    expect("fn f() { let x: int = 1; { let x: float = 2.0; print(x); } print(x); }",
           {{1, 32, "warning", "this 'x' hides the variable 'x' at 1:14"}});
    expect("fn f() { let len: int = 1; print(len); }",
           {{1, 14, "warning", "this 'len' hides the built-in function 'len'"}});
}

TEST_CASE(a_name_ends_with_its_block) {
    expect("fn f() { { let x: int = 1; print(x); } print(x); }", {{1, 46, "error", "'x' is not declared"}});
    expect_clean("fn f() { { let x: int = 1; print(x); } { let x: int = 2; print(x); } }");
}

TEST_CASE(loop_variable_scope) {
    expect_clean("fn f(n: int) { for i in 0..n { print(i); } for i in 0..n { print(i); } }");
    expect("fn f(n: int) { for i in 0..n { } print(i); }", {{1, 40, "error", "'i' is not declared"}});
    expect("fn f(n: int) { for i in 0..n { let i: int = 1; print(i); } }",
           {{1, 36, "error", "'i' is already declared in this scope as a loop variable at 1:20"}});
    expect("fn f(n: int) { for i in 0..n { i = 3; } }", {{1, 32, "error", "can't assign to the loop variable 'i'"}});
}

TEST_CASE(functions_can_be_called_before_they_are_defined) {
    expect_clean("fn main() { print(twice(4)); } fn twice(n: int) -> int { return n * 2; }");
}

TEST_CASE(duplicate_and_built_in_function_names) {
    expect("fn f() { } fn f() { }", {{1, 15, "error", "function 'f' is already defined at 1:4"}});
    expect("fn print() { }", {{1, 4, "error", "'print' is a built-in function and can't be defined again"}});
}

TEST_CASE(function_names_are_not_variables) {
    expect("fn g() { } fn f() { let x: int = g; print(x); }", {{1, 34, "error", "'g' is a function, not a variable"}});
    expect("fn f() { let x: int = 1; x(); }", {{1, 26, "error", "'x' is a variable, not a function"}});
    expect("fn f() { nope(1); }", {{1, 10, "error", "function 'nope' is not declared"}});
}

// ---------------------------------------------------------------- types

TEST_CASE(every_expression_gets_a_type) {
    CHECK_EQ(type_of("", "1 + 2"), "int");
    CHECK_EQ(type_of("", "n + x"), "float");  // int is promoted
    CHECK_EQ(type_of("", "x * 2"), "float");
    CHECK_EQ(type_of("", "n % 2"), "int");
    CHECK_EQ(type_of("", "-x"), "float");
    CHECK_EQ(type_of("", "n < x"), "bool");
    CHECK_EQ(type_of("", "!ok || n == 1"), "bool");
    CHECK_EQ(type_of("", "a[n]"), "int");
    CHECK_EQ(type_of("", "len(a)"), "int");
    CHECK_EQ(type_of("", "g()"), "int");
    CHECK_EQ(type_of("", "a == null"), "bool");
    CHECK_EQ(type_of("let b: float[] = new float[n];", "b[0]"), "float");
    CHECK_EQ(type_of("", "\"text\""), "a string");
}

TEST_CASE(arithmetic_needs_numbers_and_points_at_the_operand) {
    expect("fn f(n: int, ok: bool) { print(n + ok); }",
           {{1, 36, "error", "'+' needs a number (int or float), but this is bool"}});
    expect("fn f(n: int, ok: bool) { print(ok * n); }",
           {{1, 32, "error", "'*' needs a number (int or float), but this is bool"}});
    expect("fn f(a: int[]) { print(a - 1); }", {{1, 24, "error", "'-' needs a number (int or float), but this is int[]"}});
    expect("fn f(x: float) { print(7 % x); }", {{1, 28, "error", "'%' needs int on both sides, but this is float"}});
    expect("fn f(ok: bool) { print(-ok); }", {{1, 25, "error", "'-' needs a number (int or float), but this is bool"}});
}

TEST_CASE(one_mistake_gives_one_error) {
    // The undeclared name must not also produce a type error for the '+' or the let.
    expect("fn f() { let x: int = missing + 1; print(x); }", {{1, 23, "error", "'missing' is not declared"}});
    // Two wrong operands of one operator: the first is enough.
    expect("fn f(a: bool, b: bool) { print(a + b); }",
           {{1, 32, "error", "'+' needs a number (int or float), but this is bool"}});
}

TEST_CASE(no_conversion_between_int_and_bool) {
    expect("fn f(n: int) { if (n) { } }", {{1, 20, "error", "a condition must be bool, but this is int"}});
    expect("fn f(n: int) { while (n - 1) { } }", {{1, 23, "error", "a condition must be bool, but this is int"}});
    expect("fn f(n: int, ok: bool) { print(ok && n); }", {{1, 38, "error", "'&&' needs bool, but this is int"}});
    expect("fn f(n: int) { print(!n); }", {{1, 23, "error", "'!' needs bool, but this is int"}});
    expect("fn f(n: int, ok: bool) { print(n == ok); }", {{1, 37, "error", "can't compare int with bool"}});
    expect("fn f() { let b: bool = 1; print(b); }",
           {{1, 24, "error", "'b' is declared as bool, but its initial value is int"}});
}

TEST_CASE(int_is_promoted_to_float_but_not_back) {
    expect_clean("fn f(n: int) -> float { let x: float = n; x = 3; print(x); return n; }");
    expect_clean("fn g(x: float) { print(x); } fn f() { g(2); }");
    expect("fn f(x: float) { let n: int = x; print(n); }",
           {{1, 31, "error", "'n' is declared as int, but its initial value is float"}});
    expect("fn f(x: float) { let n: int = 0; n = x * 2; print(n); }",
           {{1, 38, "error", "can't assign float here: the left side is int"}});
    Analyzed a = analyze_source("fn f(x: float) { let n: int = x; print(n); }");
    CHECK_EQ(a.result.diagnostics.at(0).hint,
             "a float can't become an int automatically, because the fraction would be lost");
}

TEST_CASE(arrays_and_null) {
    expect_clean("fn f(n: int) { let a: int[] = null; a = new int[n]; if (a != null) { a[0] = 1; } }");
    expect("fn f() { let n: int = null; print(n); }",
           {{1, 23, "error", "'n' is declared as int, but its initial value is null"}});
    expect("fn f(n: int) { print(n == null); }", {{1, 27, "error", "can't compare int with null"}});
    expect("fn f(n: int) { print(n[0]); }", {{1, 22, "error", "only an array can be indexed, but this is int"}});
    expect("fn f(a: int[], x: float) { print(a[x]); }", {{1, 36, "error", "an array index must be int, but this is float"}});
    expect("fn f(a: int[]) { a[0] = 1.5; }", {{1, 25, "error", "can't assign float here: the left side is int"}});
    expect("fn f(a: int[]) { let b: float[] = a; print(b[0]); }",
           {{1, 35, "error", "'b' is declared as float[], but its initial value is int[]"}});
    expect("fn f(x: float) { let a: int[] = new int[x]; print(a[0]); }",
           {{1, 41, "error", "an array size must be int, but this is float"}});
}

TEST_CASE(strings_are_only_print_arguments) {
    expect_clean("fn f() { print(\"hello\", 1, 2.5, true); print(); }");
    expect("fn f() { let s: int = \"hi\"; print(s); }",
           {{1, 23, "error", "a string can only be used as an argument to print"}});
}

// ---------------------------------------------------------------- calls and returns

TEST_CASE(argument_count_and_types) {
    std::string dot = "fn dot(a: int[], b: int[], n: int) -> int { return n; } ";
    expect(dot + "fn f(a: int[]) { print(dot(a, a)); }", {{1, 80, "error", "'dot' takes 3 arguments, but 2 were given"}});
    expect(dot + "fn f(a: int[]) { print(dot(a, 3, 3)); }",
           {{1, 87, "error", "argument 2 of 'dot' must be int[], but this is int"}});
    Analyzed a = analyze_source(dot + "fn f(a: int[]) { print(dot(a, a)); }");
    CHECK_EQ(a.result.diagnostics.at(0).hint, "its signature is fn dot(a: int[], b: int[], n: int) -> int");
}

TEST_CASE(built_in_calls) {
    expect("fn f(a: int[]) { print(a); }", {{1, 24, "error", "print can't show a whole array"}});
    expect("fn f(n: int) { print(len(n)); }", {{1, 26, "error", "len needs an array, but this is int"}});
    expect("fn f(a: int[]) { print(len(a, a)); }",
           {{1, 24, "error", "'len' takes 1 argument (an array), but 2 were given"}});
}

TEST_CASE(a_call_that_returns_nothing_has_no_value) {
    expect("fn g() { } fn f() { let x: int = g(); print(x); }",
           {{1, 34, "error", "'g' returns no value, so it can't be used here"}});
    expect("fn f() { let x: int = print(1); print(x); }",
           {{1, 23, "error", "'print' returns no value, so it can't be used here"}});
    expect_clean("fn g() { } fn f() { g(); }");
}

TEST_CASE(return_must_match_the_function) {
    expect("fn f() -> int { return; }", {{1, 17, "error", "'f' returns int, so 'return' needs a value"}});
    expect("fn f() { return 1; }", {{1, 17, "error", "'f' has no return type, so it can't return a value"}});
    expect("fn f() -> int { return 1.5; }", {{1, 24, "error", "'f' returns int, but this value is float"}});
    expect("fn f(a: int[]) -> int[] { return null; }", {});
    expect_clean("fn f() -> float { return 1; }");
}

TEST_CASE(main_has_a_fixed_shape) {
    expect("fn main(n: int) { print(n); }", {{1, 9, "error", "'main' takes no parameters"}});
    expect("fn main() -> float { return 1.0; }", {{1, 4, "error", "'main' can only return int or nothing"}});
    expect_clean("fn main() -> int { return 0; }");
}

// ---------------------------------------------------------------- loops

TEST_CASE(break_and_continue_need_a_loop) {
    expect("fn f() { break; }", {{1, 10, "error", "'break' can only be used inside a loop"}});
    expect("fn f() { continue; }", {{1, 10, "error", "'continue' can only be used inside a loop"}});
    expect_clean("fn f(n: int) { while (n > 0) { if (n == 3) { break; } continue; } }");
}

TEST_CASE(parallel_loops_cannot_be_left_early) {
    expect("fn f(n: int) { parallel for i in 0..n { break; } }",
           {{1, 41, "error", "'break' can't leave a 'parallel for' loop"}});
    expect("fn f(n: int) -> int { parallel for i in 0..n { return i; } return 0; }",
           {{1, 48, "error", "'return' can't leave a 'parallel for' loop"}});
    expect_clean("fn f(n: int) { parallel for i in 0..n { if (i == 2) { continue; } print(i); } }");
    // A normal loop nested inside may still use break.
    expect_clean("fn f(n: int) { parallel for i in 0..n { for j in 0..n { break; } } }");
}

TEST_CASE(loop_ranges_are_int) {
    expect("fn f(x: float) { for i in 0..x { } }", {{1, 30, "error", "a loop range must be int, but this is float"}});
}

TEST_CASE(reduce_variables_must_exist_and_be_numeric) {
    expect("fn f(n: int) { parallel for i in 0..n reduce(+: s) { } }",
           {{1, 49, "error", "'s' in reduce(...) is not declared before the loop"}});
    expect("fn f(n: int, ok: bool) { parallel for i in 0..n reduce(+: ok) { } }",
           {{1, 59, "error", "reduce(+: ok) needs an int or float variable, but 'ok' is bool"}});
    expect("fn f(n: int, a: int[]) { parallel for i in 0..n reduce(*: a) { } }",
           {{1, 59, "error", "reduce(*: a) needs an int or float variable, but 'a' is int[]"}});
    expect("fn f(n: int) { let s: int = 0; parallel for i in 0..n reduce(+: s, s) { s = s + i; } print(s); }",
           {{1, 68, "error", "'s' is listed twice in reduce(...)"}});
    expect_clean("fn f(n: int) -> int { let s: int = 0; parallel for i in 0..n reduce(+: s) { s = s + i; } return s; }");
}

TEST_CASE(float_reduction_is_a_warning) {
    expect("fn f(n: int) -> float { let s: float = 0.0; parallel for i in 0..n reduce(+: s) { s = s + 1.5; } return s; }",
           {{1, 78, "warning", "the parallel result in float 's' can differ slightly from a normal loop's"}});
}

// ---------------------------------------------------------------- unused things

TEST_CASE(unused_variables) {
    expect("fn f() { let x: int = 1; }", {{1, 14, "warning", "variable 'x' is never used"}});
    expect("fn f() { let x: int = 1; x = 2; }",
           {{1, 14, "warning", "variable 'x' is assigned, but its value is never used"}});
    expect_clean("fn f() { let _scratch: int = 1; }");
    expect_clean("fn f(unused: int) { }");  // parameters are part of the function's shape
    expect("fn f(n: int) { n + 1; }", {{1, 16, "warning", "this expression's value is not used"}});
}

// ---------------------------------------------------------------- symbols and examples

TEST_CASE(symbol_rows_record_kind_type_and_use) {
    Analyzed a = analyze_source("fn dot(a: int[], n: int) -> int {\n  let sum: int = 0;\n  for i in 0..n { sum = sum + a[i]; }\n  return sum;\n}");
    CHECK(a.result.diagnostics.empty());
    const auto* dot = find_symbol(a.result, "dot");
    CHECK(dot && dot->kind == "function" && dot->type == "fn dot(a: int[], n: int) -> int");
    const auto* sum = find_symbol(a.result, "sum");
    CHECK(sum && sum->kind == "variable" && sum->type == "int" && sum->function == "dot");
    CHECK(sum && sum->line == 2 && sum->col == 7 && sum->reads == 2 && sum->writes == 2);
    const auto* i = find_symbol(a.result, "i");
    CHECK(i && i->kind == "loop variable" && i->depth == 2);
    std::string json = sem::symbols_json(a.result, "t.parc");
    CHECK(json.find("{\"name\": \"a\", \"kind\": \"parameter\", \"type\": \"int[]\", \"function\": \"dot\"") !=
          std::string::npos);
}

TEST_CASE(all_examples_have_no_semantic_errors) {
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(PARC_EXAMPLES_DIR)) {
        if (entry.path().extension() != ".parc") continue;
        ++count;
        std::ifstream in(entry.path(), std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        Analyzed a = analyze_source(text.str());
        for (const auto& d : a.result.diagnostics) {
            if (d.severity != "error") continue;
            ++test::failures();
            std::cerr << "    " << entry.path().filename().string() << ": " << format(d) << "\n";
        }
    }
    CHECK(count > 0);
}

int main() { return test::run_all(); }
