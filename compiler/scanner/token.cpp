#include "scanner/token.hpp"

#include <unordered_map>

namespace parc {

std::string_view kind_name(Kind kind) {
    switch (kind) {
#define PARC_KIND_NAME(name, json_name, text) \
    case Kind::name:                          \
        return json_name;
        PARC_TOKEN_KINDS(PARC_KIND_NAME)
#undef PARC_KIND_NAME
    }
    return "?";
}

std::string_view kind_symbol(Kind kind) {
    switch (kind) {
#define PARC_KIND_SYMBOL(name, json_name, text) \
    case Kind::name:                            \
        return text;
        PARC_TOKEN_KINDS(PARC_KIND_SYMBOL)
#undef PARC_KIND_SYMBOL
    }
    return "?";
}

std::optional<Kind> keyword_kind(std::string_view text) {
    static const std::unordered_map<std::string_view, Kind> keywords = {
        {"fn", Kind::Fn},
        {"let", Kind::Let},
        {"if", Kind::If},
        {"else", Kind::Else},
        {"while", Kind::While},
        {"for", Kind::For},
        {"in", Kind::In},
        {"parallel", Kind::Parallel},
        {"reduce", Kind::Reduce},
        {"return", Kind::Return},
        {"break", Kind::Break},
        {"continue", Kind::Continue},
        {"true", Kind::True},
        {"false", Kind::False},
        {"new", Kind::New},
        {"null", Kind::Null},
        {"int", Kind::IntType},
        {"float", Kind::FloatType},
        {"bool", Kind::BoolType},
    };
    auto it = keywords.find(text);
    if (it == keywords.end()) return std::nullopt;
    return it->second;
}

}  // namespace parc
