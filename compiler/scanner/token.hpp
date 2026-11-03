#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace parc {

// One list drives the enum, the names in tokens.json, and how errors print each kind.
//  X(enum name, json name, display text)
#define PARC_TOKEN_KINDS(X)                     \
    X(Ident, "IDENT", "a name")                 \
    X(Int, "INT", "an integer")                 \
    X(Float, "FLOAT", "a number")               \
    X(String, "STRING", "a string")             \
    X(Fn, "FN", "fn")                           \
    X(Let, "LET", "let")                        \
    X(If, "IF", "if")                           \
    X(Else, "ELSE", "else")                     \
    X(While, "WHILE", "while")                  \
    X(For, "FOR", "for")                        \
    X(In, "IN", "in")                           \
    X(Parallel, "PARALLEL", "parallel")         \
    X(Reduce, "REDUCE", "reduce")               \
    X(Return, "RETURN", "return")               \
    X(Break, "BREAK", "break")                  \
    X(Continue, "CONTINUE", "continue")         \
    X(True, "TRUE", "true")                     \
    X(False, "FALSE", "false")                  \
    X(New, "NEW", "new")                        \
    X(Null, "NULL", "null")                     \
    X(IntType, "INT_TYPE", "int")               \
    X(FloatType, "FLOAT_TYPE", "float")         \
    X(BoolType, "BOOL_TYPE", "bool")            \
    X(Plus, "PLUS", "+")                        \
    X(Minus, "MINUS", "-")                      \
    X(Star, "STAR", "*")                        \
    X(Slash, "SLASH", "/")                      \
    X(Percent, "PERCENT", "%")                  \
    X(Assign, "ASSIGN", "=")                    \
    X(Eq, "EQ", "==")                           \
    X(Ne, "NE", "!=")                           \
    X(Lt, "LT", "<")                            \
    X(Le, "LE", "<=")                           \
    X(Gt, "GT", ">")                            \
    X(Ge, "GE", ">=")                           \
    X(And, "AND", "&&")                         \
    X(Or, "OR", "||")                           \
    X(Not, "NOT", "!")                          \
    X(Arrow, "ARROW", "->")                     \
    X(DotDot, "DOTDOT", "..")                   \
    X(LParen, "LPAREN", "(")                    \
    X(RParen, "RPAREN", ")")                    \
    X(LBrace, "LBRACE", "{")                    \
    X(RBrace, "RBRACE", "}")                    \
    X(LBracket, "LBRACKET", "[")                \
    X(RBracket, "RBRACKET", "]")                \
    X(Comma, "COMMA", ",")                      \
    X(Colon, "COLON", ":")                      \
    X(Semi, "SEMI", ";")                        \
    X(Eof, "EOF", "end of file")

enum class Kind {
#define PARC_KIND_ENUM(name, json_name, text) name,
    PARC_TOKEN_KINDS(PARC_KIND_ENUM)
#undef PARC_KIND_ENUM
};

std::string_view kind_name(Kind kind);    // "SEMI"
std::string_view kind_symbol(Kind kind);  // ";"
std::optional<Kind> keyword_kind(std::string_view text);

// INT tokens carry a long long, FLOAT a double, STRING the decoded text.
using TokenValue = std::variant<std::monostate, long long, double, std::string>;

struct Token {
    Kind kind;
    std::string text;
    int line;
    int col;
    TokenValue value;
};

}  // namespace parc
