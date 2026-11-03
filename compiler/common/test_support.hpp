#pragma once

// Helpers shared by the tests of the later phases: compile a piece of source,
// run it, and pick findings out of the result.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cli/pipeline.hpp"
#include "vm/vm.hpp"

namespace test {

inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// Every .parc file in examples/, sorted by name.
inline std::vector<std::filesystem::path> example_files() {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(PARC_EXAMPLES_DIR))
        if (entry.path().extension() == ".parc") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    return files;
}

// "line:kind" of every finding whose kind is in `kinds` (all findings if empty), in source order.
inline std::vector<std::string> findings(const parc::Compilation& c, const std::vector<std::string>& kinds = {}) {
    std::vector<std::string> out;
    for (const auto& d : c.findings) {
        if (!kinds.empty() && std::find(kinds.begin(), kinds.end(), d.kind) == kinds.end()) continue;
        out.push_back(std::to_string(d.line) + ":" + d.kind);
    }
    return out;
}

inline const parc::Diagnostic* find_kind(const parc::Compilation& c, const std::string& kind) {
    for (const auto& d : c.findings)
        if (d.kind == kind) return &d;
    return nullptr;
}

inline bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

inline int count(const std::string& text, const std::string& part) {
    int n = 0;
    for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) ++n;
    return n;
}

// The IR text of one function ("" if there is none).
inline std::string ir_of(const parc::ir::Program& program, const std::string& name) {
    const parc::ir::Function* fn = program.find(name);
    return fn ? parc::ir::to_text(program, *fn) : "";
}

// Compiles and runs; a program with errors gives "<does not compile>".
inline parc::vm::Result run(const std::string& source, parc::CompileOptions compile = {}, parc::vm::Options options = {}) {
    parc::Compilation c = parc::compile(source, compile);
    if (!c.runnable) {
        parc::vm::Result r;
        r.ok = false;
        r.error = "<does not compile>";
        for (const auto& d : c.syntax_errors) r.error += " " + parc::format(d);
        for (const auto& d : c.findings)
            if (d.severity == "error") r.error += " " + parc::format(d);
        return r;
    }
    return parc::vm::run(c.code, options);
}

// What the program prints, or "error: ..." with whatever it printed before stopping.
inline std::string output(const std::string& source, parc::CompileOptions compile = {}, parc::vm::Options options = {}) {
    parc::vm::Result r = run(source, compile, options);
    return r.ok ? r.output : r.output + "error: " + r.error;
}

}  // namespace test
