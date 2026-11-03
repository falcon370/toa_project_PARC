#include "scanner/scanner.hpp"

#include <charconv>
#include <cstdio>

#include "common/json.hpp"

namespace parc {
namespace {

constexpr long long kIntMax = 2147483647LL;

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_ident_start(char c) { return is_alpha(c) || c == '_'; }
bool is_ident_char(char c) { return is_ident_start(c) || is_digit(c); }

struct OpEntry {
    std::string_view text;
    Kind kind;
};

// Longest match: two-character operators are tried before one-character ones.
constexpr OpEntry kTwoCharOps[] = {
    {"==", Kind::Eq}, {"!=", Kind::Ne}, {"<=", Kind::Le},    {">=", Kind::Ge},
    {"&&", Kind::And}, {"||", Kind::Or}, {"->", Kind::Arrow}, {"..", Kind::DotDot},
};

constexpr OpEntry kOneCharOps[] = {
    {"+", Kind::Plus},     {"-", Kind::Minus},    {"*", Kind::Star},     {"/", Kind::Slash},
    {"%", Kind::Percent},  {"=", Kind::Assign},   {"<", Kind::Lt},       {">", Kind::Gt},
    {"!", Kind::Not},      {"(", Kind::LParen},   {")", Kind::RParen},   {"{", Kind::LBrace},
    {"}", Kind::RBrace},   {"[", Kind::LBracket}, {"]", Kind::RBracket}, {",", Kind::Comma},
    {":", Kind::Colon},    {";", Kind::Semi},
};

std::string describe_char(char c) {
    auto u = static_cast<unsigned char>(c);
    if (u >= 0x20 && u < 0x7f) return std::string("'") + c + "'";
    char buf[8];
    std::snprintf(buf, sizeof buf, "0x%02X", u);
    return buf;
}

// Hand-coded DFA scanner. Each scan_* method is one group of DFA states.
class Scanner {
public:
    explicit Scanner(std::string_view src) : src_(src) {}

    ScanResult run() {
        while (true) {
            skip_whitespace_and_comments();
            if (at_end()) {
                result_.tokens.push_back(Token{Kind::Eof, "", line_, col_, {}});
                return std::move(result_);
            }
            scan_token();
        }
    }

private:
    std::string_view src_;
    size_t pos_ = 0;
    int line_ = 1;
    int col_ = 1;
    ScanResult result_;

    bool at_end() const { return pos_ >= src_.size(); }

    char peek(size_t ahead = 0) const {
        size_t i = pos_ + ahead;
        return i < src_.size() ? src_[i] : '\0';
    }

    char advance() {
        char c = src_[pos_++];
        if (c == '\n') {
            ++line_;
            col_ = 1;
        } else {
            ++col_;
        }
        return c;
    }

    void error(int line, int col, std::string message, std::string hint = "") {
        Diagnostic d;
        d.phase = "scanner";
        d.severity = "error";
        d.line = line;
        d.col = col;
        d.message = std::move(message);
        d.hint = std::move(hint);
        result_.errors.push_back(std::move(d));
    }

    void emit(Kind kind, size_t start, int line, int col, TokenValue value = {}) {
        result_.tokens.push_back(Token{kind, std::string(src_.substr(start, pos_ - start)), line, col, std::move(value)});
    }

    void skip_whitespace_and_comments() {
        while (!at_end()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                advance();
            } else if (c == '/' && peek(1) == '/') {
                while (!at_end() && peek() != '\n') advance();
            } else if (c == '/' && peek(1) == '*') {
                int line = line_, col = col_;
                advance();
                advance();
                while (!at_end() && !(peek() == '*' && peek(1) == '/')) advance();
                if (at_end()) {
                    error(line, col, "unterminated comment", "a /* comment needs a closing */");
                    return;
                }
                advance();
                advance();
            } else {
                return;
            }
        }
    }

    void scan_token() {
        char c = peek();
        if (is_ident_start(c)) {
            scan_identifier();
        } else if (is_digit(c)) {
            scan_number();
        } else if (c == '"') {
            scan_string();
        } else {
            scan_operator();
        }
    }

    void scan_identifier() {
        size_t start = pos_;
        int line = line_, col = col_;
        while (is_ident_char(peek())) advance();
        auto keyword = keyword_kind(src_.substr(start, pos_ - start));
        emit(keyword.value_or(Kind::Ident), start, line, col);
    }

    void scan_number() {
        size_t start = pos_;
        int line = line_, col = col_;
        while (is_digit(peek())) advance();
        bool is_float = false;

        // A float needs a digit after the dot, so "0..n" stays INT DOTDOT IDENT.
        if (peek() == '.' && is_digit(peek(1))) {
            is_float = true;
            advance();
            while (is_digit(peek())) advance();
        }

        if (peek() == 'e' || peek() == 'E') {
            size_t sign = (peek(1) == '+' || peek(1) == '-') ? 1 : 0;
            if (is_digit(peek(1 + sign))) {
                is_float = true;
                for (size_t i = 0; i < 1 + sign; ++i) advance();
                while (is_digit(peek())) advance();
            }
        }

        if (is_ident_char(peek())) {
            while (is_ident_char(peek())) advance();
            std::string text(src_.substr(start, pos_ - start));
            error(line, col, "invalid number '" + text + "'", "a number can't be followed directly by letters");
            return;
        }

        std::string_view text = src_.substr(start, pos_ - start);
        if (is_float) {
            double value = 0;
            std::from_chars(text.data(), text.data() + text.size(), value);
            emit(Kind::Float, start, line, col, value);
            return;
        }

        long long value = 0;
        for (char d : text) {
            value = value * 10 + (d - '0');
            if (value > kIntMax) {
                error(line, col, "integer literal " + std::string(text) + " is out of range",
                      "the largest int is " + std::to_string(kIntMax));
                return;
            }
        }
        emit(Kind::Int, start, line, col, value);
    }

    void scan_string() {
        size_t start = pos_;
        int line = line_, col = col_;
        advance();
        std::string chars;
        while (true) {
            if (at_end() || peek() == '\n') {
                error(line, col, "unterminated string", "a string must end with \" on the same line");
                return;
            }
            char c = advance();
            if (c == '"') break;
            if (c != '\\') {
                chars += c;
                continue;
            }
            if (at_end()) continue;
            switch (peek()) {
                case 'n': chars += '\n'; advance(); break;
                case 't': chars += '\t'; advance(); break;
                case '\\': chars += '\\'; advance(); break;
                case '"': chars += '"'; advance(); break;
                default:
                    error(line_, col_ - 1, std::string("unknown escape '\\") + peek() + "'",
                          "use \\n, \\t, \\\\ or \\\"");
            }
        }
        emit(Kind::String, start, line, col, chars);
    }

    void scan_operator() {
        size_t start = pos_;
        int line = line_, col = col_;

        std::string_view two = src_.substr(pos_, 2);
        for (const auto& op : kTwoCharOps) {
            if (op.text == two) {
                advance();
                advance();
                emit(op.kind, start, line, col);
                return;
            }
        }

        char c = advance();
        for (const auto& op : kOneCharOps) {
            if (op.text[0] == c) {
                emit(op.kind, start, line, col);
                return;
            }
        }

        if (c == '&') {
            error(line, col, "unexpected '&'", "did you mean '&&'?");
        } else if (c == '|') {
            error(line, col, "unexpected '|'", "did you mean '||'?");
        } else if (c == '.') {
            error(line, col, "unexpected '.'", "did you mean '..'?");
        } else if (static_cast<unsigned char>(c) >= 0x80) {
            // Swallow the rest of a UTF-8 sequence so one character gives one error.
            while (!at_end() && (static_cast<unsigned char>(peek()) & 0xC0) == 0x80) advance();
            error(line, col, "non-ASCII character", "ParC code must be plain ASCII outside strings");
        } else {
            error(line, col, "unexpected character " + describe_char(c));
        }
    }
};

std::string value_json(const TokenValue& value) {
    if (auto* i = std::get_if<long long>(&value)) return std::to_string(*i);
    if (auto* d = std::get_if<double>(&value)) return json::number(*d);
    if (auto* s = std::get_if<std::string>(&value)) return json::quote(*s);
    return "";
}

}  // namespace

ScanResult scan(std::string_view source) { return Scanner(source).run(); }

std::string to_json(const ScanResult& result, std::string_view filename) {
    std::string out = "{\n  \"file\": " + json::quote(filename) + ",\n  \"tokens\": [";
    for (size_t i = 0; i < result.tokens.size(); ++i) {
        const Token& t = result.tokens[i];
        out += i ? ",\n    " : "\n    ";
        out += "{\"kind\": " + json::quote(kind_name(t.kind)) + ", \"text\": " + json::quote(t.text) +
               ", \"line\": " + std::to_string(t.line) + ", \"col\": " + std::to_string(t.col);
        std::string value = value_json(t.value);
        if (!value.empty()) out += ", \"value\": " + value;
        out += "}";
    }
    out += result.tokens.empty() ? "],\n  \"errors\": [" : "\n  ],\n  \"errors\": [";
    for (size_t i = 0; i < result.errors.size(); ++i) {
        out += i ? ",\n    " : "\n    ";
        out += to_json(result.errors[i]);
    }
    out += result.errors.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

}  // namespace parc
