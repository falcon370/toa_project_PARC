#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "common/diagnostic.hpp"
#include "scanner/token.hpp"

namespace parc {

struct ScanResult {
    std::vector<Token> tokens;  // always ends with an EOF token
    std::vector<Diagnostic> errors;
};

ScanResult scan(std::string_view source);

// The tokens.json file: {"file", "tokens": [...], "errors": [...]}
std::string to_json(const ScanResult& result, std::string_view filename);

}  // namespace parc
