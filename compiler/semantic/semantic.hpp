#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/diagnostic.hpp"
#include "parser/ast.hpp"
#include "semantic/symbol_table.hpp"

namespace parc::sem {

// One row of symbols.json: a declared name and how it is used.
struct SymbolInfo {
    std::string name;
    std::string kind;      // "variable", "parameter", "loop variable", "function"
    std::string type;      // "int[]"; for a function, its signature
    std::string function;  // the function it belongs to; empty for functions
    int line = 0;
    int col = 0;
    int depth = 0;   // 0 = top level, 1 = a function's own scope, 2 = a block inside it, ...
    int reads = 0;   // how many times its value is used
    int writes = 0;  // how many times it is assigned, counting an initial value
};

struct Result {
    std::vector<Diagnostic> diagnostics;                    // errors and warnings, in source order
    std::unordered_map<const ast::Expr*, Type> types;       // the type of every expression
    std::vector<SymbolInfo> symbols;
};

// Name resolution and type checking of a parsed program:
//  - a scoped symbol table: undeclared names, redeclarations, shadowing warnings
//  - a type for every expression, with mismatches reported at the exact operand
//  - calls, returns, break/continue, reduce(...) clauses
//  - variables that are never used
// Later phases only run when this reports no errors.
Result analyze(const ast::Program& program);

// The symbols.json file: {"file", "symbols": [...]}
std::string symbols_json(const Result& result, std::string_view filename);

}  // namespace parc::sem
