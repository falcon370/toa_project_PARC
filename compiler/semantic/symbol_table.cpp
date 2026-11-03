#include "semantic/symbol_table.hpp"

namespace parc::sem {

std::string to_string(Type type) {
    std::string out;
    switch (type.kind) {
        case TypeKind::Int: out = "int"; break;
        case TypeKind::Float: out = "float"; break;
        case TypeKind::Bool: out = "bool"; break;
        case TypeKind::Void: return "no value";
        case TypeKind::String: return "a string";
        case TypeKind::Null: return "null";
        case TypeKind::Error: return "?";
    }
    return type.is_array ? out + "[]" : out;
}

Type from_ast(ast::Type type) {
    Type out;
    switch (type.base) {
        case ast::BaseType::Int: out.kind = TypeKind::Int; break;
        case ast::BaseType::Float: out.kind = TypeKind::Float; break;
        case ast::BaseType::Bool: out.kind = TypeKind::Bool; break;
        case ast::BaseType::Void: out.kind = TypeKind::Void; break;
    }
    out.is_array = type.is_array;
    return out;
}

std::string_view describe(SymbolKind kind) {
    switch (kind) {
        case SymbolKind::Variable: return "variable";
        case SymbolKind::Parameter: return "parameter";
        case SymbolKind::LoopVariable: return "loop variable";
        case SymbolKind::Function: return "function";
        case SymbolKind::Builtin: return "built-in function";
    }
    return "name";
}

const Symbol* SymbolTable::declare(const Symbol& symbol) {
    auto [it, added] = scopes_.back().try_emplace(symbol.name, symbol);
    return added ? nullptr : &it->second;
}

const Symbol* SymbolTable::lookup(std::string_view name) const {
    std::string key(name);
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        auto it = scope->find(key);
        if (it != scope->end()) return &it->second;
    }
    return nullptr;
}

std::vector<std::string> SymbolTable::visible_names(bool functions) const {
    std::vector<std::string> names;
    for (const auto& scope : scopes_) {
        for (const auto& [name, symbol] : scope) {
            bool is_function = symbol.kind == SymbolKind::Function || symbol.kind == SymbolKind::Builtin;
            if (is_function == functions) names.push_back(name);
        }
    }
    return names;
}

}  // namespace parc::sem
