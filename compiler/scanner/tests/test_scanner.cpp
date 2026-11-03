#include <filesystem>
#include <fstream>
#include <source_location>
#include <sstream>
#include <string>
#include <vector>

#include "common/json.hpp"
#include "scanner/scanner.hpp"
#include "common/test_framework.hpp"

using parc::Kind;
using parc::scan;

namespace {

std::vector<Kind> kinds(std::string_view src) {
    std::vector<Kind> out;
    for (const auto& t : scan(src).tokens) out.push_back(t.kind);
    return out;
}

std::vector<std::string> messages(std::string_view src) {
    std::vector<std::string> out;
    for (const auto& e : scan(src).errors) out.push_back(e.message);
    return out;
}

std::string kind_list(const std::vector<Kind>& ks) {
    std::string out;
    for (Kind k : ks) out += std::string(out.empty() ? "" : " ") + std::string(parc::kind_name(k));
    return out;
}

std::string string_list(const std::vector<std::string>& xs) {
    std::string out;
    for (const auto& x : xs) out += (out.empty() ? "[" : ", [") + x + "]";
    return out.empty() ? "(none)" : out;
}

void expect_kinds(std::string_view src, const std::vector<Kind>& want,
                  std::source_location loc = std::source_location::current()) {
    auto got = kinds(src);
    if (got == want) return;
    ++test::failures();
    std::cerr << loc.file_name() << ":" << loc.line() << ": tokens for \"" << src << "\"\n    want " << kind_list(want)
              << "\n    got  " << kind_list(got) << "\n";
}

void expect_messages(std::string_view src, const std::vector<std::string>& want,
                     std::source_location loc = std::source_location::current()) {
    auto got = messages(src);
    if (got == want) return;
    ++test::failures();
    std::cerr << loc.file_name() << ":" << loc.line() << ": errors for \"" << src << "\"\n    want "
              << string_list(want) << "\n    got  " << string_list(got) << "\n";
}

const parc::Token* find_text(const parc::ScanResult& r, std::string_view text) {
    for (const auto& t : r.tokens)
        if (t.text == text) return &t;
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------- keywords and identifiers

TEST_CASE(keywords_win_ties_but_longer_identifiers_stay_identifiers) {
    expect_kinds("fn fnx let letter in inside int int32 null",
                 {Kind::Fn, Kind::Ident, Kind::Let, Kind::Ident, Kind::In, Kind::Ident, Kind::IntType, Kind::Ident,
                  Kind::Null, Kind::Eof});
}

TEST_CASE(identifier_characters) {
    auto r = scan("_tmp a_1 x9");
    CHECK_EQ(r.tokens[0].text, std::string("_tmp"));
    CHECK_EQ(r.tokens[1].text, std::string("a_1"));
    CHECK_EQ(r.tokens[2].text, std::string("x9"));
}

// ---------------------------------------------------------------- longest match

TEST_CASE(two_char_operators_are_one_token) {
    expect_kinds("<= < = == -> - .. != ! && ||",
                 {Kind::Le, Kind::Lt, Kind::Assign, Kind::Eq, Kind::Arrow, Kind::Minus, Kind::DotDot, Kind::Ne,
                  Kind::Not, Kind::And, Kind::Or, Kind::Eof});
}

TEST_CASE(no_spaces_needed) {
    expect_kinds("a<=b", {Kind::Ident, Kind::Le, Kind::Ident, Kind::Eof});
    expect_kinds("x->y", {Kind::Ident, Kind::Arrow, Kind::Ident, Kind::Eof});
}

// ---------------------------------------------------------------- numbers

TEST_CASE(range_is_not_a_float) {
    expect_kinds("0..n", {Kind::Int, Kind::DotDot, Kind::Ident, Kind::Eof});
    CHECK(std::get<long long>(scan("0..n").tokens[0].value) == 0);
}

TEST_CASE(floats_and_exponents) {
    auto r = scan("1.5 2.0e-3 7e2 3E+1");
    const double want[] = {1.5, 0.002, 700.0, 30.0};
    for (int i = 0; i < 4; ++i) {
        CHECK(r.tokens[i].kind == Kind::Float);
        CHECK_EQ(std::get<double>(r.tokens[i].value), want[i]);
    }
}

TEST_CASE(int_limits) {
    CHECK_EQ(std::get<long long>(scan("2147483647").tokens[0].value), 2147483647LL);
    expect_messages("2147483648", {"integer literal 2147483648 is out of range"});
}

TEST_CASE(letters_after_a_number) { expect_messages("12abc", {"invalid number '12abc'"}); }

TEST_CASE(exponent_without_digits_is_not_a_float) { expect_messages("1e", {"invalid number '1e'"}); }

// ---------------------------------------------------------------- strings

TEST_CASE(escapes) {
    auto t = scan(R"("a\tb\n\"q\" \\")").tokens[0];
    CHECK(t.kind == Kind::String);
    CHECK_EQ(std::get<std::string>(t.value), std::string("a\tb\n\"q\" \\"));
}

TEST_CASE(unterminated_string_reports_its_start) {
    auto r = scan("let s = \"oops\nlet t = 1;");
    CHECK_EQ(r.errors.size(), size_t{1});
    CHECK_EQ(r.errors[0].message, std::string("unterminated string"));
    CHECK_EQ(r.errors[0].line, 1);
    CHECK_EQ(r.errors[0].col, 9);
    CHECK(r.tokens.size() > 3 && r.tokens[3].kind == Kind::Let);
}

TEST_CASE(unknown_escape) { expect_messages(R"("\q")", {"unknown escape '\\q'"}); }

// ---------------------------------------------------------------- comments and positions

TEST_CASE(comments_are_skipped) {
    expect_kinds("a // line comment\n/* block\ncomment */ b", {Kind::Ident, Kind::Ident, Kind::Eof});
}

TEST_CASE(unterminated_block_comment) {
    auto r = scan("x /* never closed");
    CHECK_EQ(r.errors.size(), size_t{1});
    CHECK_EQ(r.errors[0].message, std::string("unterminated comment"));
    CHECK_EQ(r.errors[0].col, 3);
}

TEST_CASE(line_and_column) {
    auto r = scan("fn main() {\n    let x: int = 1;\n}");
    auto at = [&](std::string_view text, int line, int col) {
        const parc::Token* t = find_text(r, text);
        CHECK(t != nullptr);
        if (!t) return;
        CHECK_EQ(t->line, line);
        CHECK_EQ(t->col, col);
    };
    at("fn", 1, 1);
    at("main", 1, 4);
    at("let", 2, 5);
    at("1", 2, 18);
    at("}", 3, 1);
}

TEST_CASE(eof_position) {
    auto eof = scan("a\nb").tokens.back();
    CHECK(eof.kind == Kind::Eof);
    CHECK_EQ(eof.line, 2);
    CHECK_EQ(eof.col, 2);
}

// ---------------------------------------------------------------- error recovery

TEST_CASE(scanning_continues_after_a_bad_character) {
    expect_messages("let x = 5 @ 3;", {"unexpected character '@'"});
    expect_kinds("let x = 5 @ 3;",
                 {Kind::Let, Kind::Ident, Kind::Assign, Kind::Int, Kind::Int, Kind::Semi, Kind::Eof});
}

TEST_CASE(hints_for_single_ampersand_and_bar) {
    auto r = scan("a & b | c");
    CHECK_EQ(r.errors.size(), size_t{2});
    CHECK_EQ(r.errors[0].hint, std::string("did you mean '&&'?"));
    CHECK_EQ(r.errors[1].hint, std::string("did you mean '||'?"));
}

TEST_CASE(non_ascii_character_is_one_error) {
    expect_messages("x \xC3\xA9 y", {"non-ASCII character"});  // "x é y" in UTF-8
    expect_kinds("x \xC3\xA9 y", {Kind::Ident, Kind::Ident, Kind::Eof});
}

// ---------------------------------------------------------------- examples and JSON

TEST_CASE(every_example_scans_without_errors) {
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(PARC_EXAMPLES_DIR)) {
        if (entry.path().extension() != ".parc") continue;
        ++count;
        std::ifstream in(entry.path(), std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        auto r = scan(buffer.str());
        if (!r.errors.empty()) {
            ++test::failures();
            std::cerr << "    " << entry.path().filename().string() << ": " << parc::format(r.errors[0]) << "\n";
        }
    }
    CHECK(count > 0);
}

TEST_CASE(json_output_shape) {
    std::string out = parc::to_json(scan("let x: int = 1;"), "t.parc");
    CHECK(out.find("\"file\": \"t.parc\"") != std::string::npos);
    CHECK(out.find("{\"kind\": \"LET\", \"text\": \"let\", \"line\": 1, \"col\": 1}") != std::string::npos);
    CHECK(out.find("\"kind\": \"INT\", \"text\": \"1\", \"line\": 1, \"col\": 14, \"value\": 1}") != std::string::npos);
    CHECK(out.find("\"errors\": []") != std::string::npos);
}

TEST_CASE(json_string_escaping) {
    CHECK_EQ(parc::json::quote("a\"b\\\n"), std::string("\"a\\\"b\\\\\\n\""));
    CHECK_EQ(parc::json::number(700.0), std::string("700.0"));
    CHECK_EQ(parc::json::number(0.002), std::string("0.002"));
}

int main() { return test::run_all(); }
