#pragma once

#include <vector>

#include "common/diagnostic.hpp"
#include "parser/ast.hpp"
#include "scanner/token.hpp"

namespace parc {

struct ParseResult {
    ast::Program program;  // partial when there were errors
    std::vector<Diagnostic> errors;
};

// tokens must end with an EOF token, as scan() guarantees.
ParseResult parse(const std::vector<Token>& tokens);

}  // namespace parc
