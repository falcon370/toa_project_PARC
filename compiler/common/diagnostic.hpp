#pragma once

#include <string>
#include <vector>

namespace parc {

// The error, warning and note format shared by every compiler phase.
struct Diagnostic {
    std::string phase;
    std::string severity;  // "error", "warning" or "info"
    int line = 0;
    int col = 0;
    std::string message;
    std::string hint;

    // Filled in from the semantic phase on. Together they make a finding a
    // "fact" that the explorer can list and the agent can cite.
    std::string kind;      // short machine-readable name: "divide-by-zero"
    std::string function;  // the function it is in
    std::string subject;   // the source text it is about: "sum / n"
    std::string proof;     // what the compiler knows that makes it true
    std::string cwe;       // "CWE-369" when the finding matches a known weakness
    std::string guard;     // the condition under which it goes wrong, as ParC code: "n == 0"
    int stmt_line = 0;     // line of the statement that contains it
    std::string id;        // "F3", given once all findings are collected
};

// "3:9: error: unterminated string (a string must end with ...)"
std::string format(const Diagnostic& d);

std::string to_json(const Diagnostic& d);

// Orders by position and numbers the findings F1, F2, ...
void sort_and_number(std::vector<Diagnostic>& findings);

bool has_errors(const std::vector<Diagnostic>& diagnostics);

}  // namespace parc
