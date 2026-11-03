#include "common/diagnostic.hpp"

#include <algorithm>

#include "common/json.hpp"

namespace parc {

std::string format(const Diagnostic& d) {
    std::string out = std::to_string(d.line) + ":" + std::to_string(d.col) + ": " + d.severity + ": " + d.message;
    if (!d.hint.empty()) out += " (" + d.hint + ")";
    return out;
}

std::string to_json(const Diagnostic& d) {
    std::string out = "{";
    if (!d.id.empty()) out += "\"id\": " + json::quote(d.id) + ", ";
    out += "\"phase\": " + json::quote(d.phase) + ", \"severity\": " + json::quote(d.severity) +
           ", \"line\": " + std::to_string(d.line) + ", \"col\": " + std::to_string(d.col) +
           ", \"message\": " + json::quote(d.message);
    auto field = [&](const char* name, const std::string& value) {
        if (!value.empty()) out += std::string(", \"") + name + "\": " + json::quote(value);
    };
    field("hint", d.hint);
    field("kind", d.kind);
    field("function", d.function);
    field("subject", d.subject);
    field("proof", d.proof);
    field("cwe", d.cwe);
    field("guard", d.guard);
    if (d.stmt_line > 0) out += ", \"stmt_line\": " + std::to_string(d.stmt_line);
    return out + "}";
}

void sort_and_number(std::vector<Diagnostic>& findings) {
    std::stable_sort(findings.begin(), findings.end(), [](const Diagnostic& a, const Diagnostic& b) {
        return a.line != b.line ? a.line < b.line : a.col < b.col;
    });
    for (size_t i = 0; i < findings.size(); ++i) findings[i].id = "F" + std::to_string(i + 1);
}

bool has_errors(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& d) { return d.severity == "error"; });
}

}  // namespace parc
