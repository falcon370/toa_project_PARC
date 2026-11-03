#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "parser/ast.hpp"

namespace parc::sem {

// The type of an expression. It has three kinds the syntax tree's Type does
// not need: a string (only a print argument), null, and "error", which is
// given to an expression that was already reported so it causes no second error.
enum class TypeKind { Int, Float, Bool, Void, String, Null, Error };

struct Type {
    TypeKind kind = TypeKind::Error;
    bool is_array = false;
    bool operator==(const Type&) const = default;
};

inline constexpr Type kInt{TypeKind::Int, false};
inline constexpr Type kFloat{TypeKind::Float, false};
inline constexpr Type kBool{TypeKind::Bool, false};
inline constexpr Type kVoid{TypeKind::Void, false};
inline constexpr Type kString{TypeKind::String, false};
inline constexpr Type kNull{TypeKind::Null, false};
inline constexpr Type kError{TypeKind::Error, false};

std::string to_string(Type type);  // "int", "float[]", "null", "a string"
Type from_ast(ast::Type type);

inline bool is_error(Type t) { return t.kind == TypeKind::Error; }
inline bool is_numeric(Type t) { return !t.is_array && (t.kind == TypeKind::Int || t.kind == TypeKind::Float); }

enum class SymbolKind { Variable, Parameter, LoopVariable, Function, Builtin };

std::string_view describe(SymbolKind kind);  // "variable", "parameter", "loop variable", ...

struct Symbol {
    std::string name;
    SymbolKind kind = SymbolKind::Variable;
    Type type;                             // for a function: its return type
    std::vector<Type> params;              // functions only
    std::vector<std::string> param_names;  // functions only
    ast::Pos pos;                          // where it was declared; 0:0 for built-ins
    int index = -1;                        // its row in symbols.json, or -1
};

// A stack of hash maps, one per open scope. Entering a block pushes a map,
// leaving it pops the map, and a lookup searches from the innermost outward.
class SymbolTable {
public:
    SymbolTable() { push_scope(); }  // scope 0 holds functions and built-ins

    void push_scope() { scopes_.emplace_back(); }
    void pop_scope() { scopes_.pop_back(); }
    int depth() const { return static_cast<int>(scopes_.size()) - 1; }

    // Adds the symbol to the innermost scope. If that scope already has the
    // name, nothing is added and the existing symbol is returned.
    const Symbol* declare(const Symbol& symbol);

    // The innermost declaration of the name, or nullptr.
    const Symbol* lookup(std::string_view name) const;

    // Every visible name of one sort, for "did you mean" hints.
    std::vector<std::string> visible_names(bool functions) const;

private:
    std::vector<std::unordered_map<std::string, Symbol>> scopes_;
};

}  // namespace parc::sem
