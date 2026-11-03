#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "codegen/codegen.hpp"
#include "common/diagnostic.hpp"
#include "ir/ir.hpp"
#include "optimizer/dependence.hpp"
#include "optimizer/passes.hpp"
#include "parser/parser.hpp"
#include "scanner/scanner.hpp"
#include "semantic/flow_checks.hpp"
#include "semantic/semantic.hpp"
#include "vm/bytecode.hpp"

// Runs the phases in order and keeps what each one produced. This is the one
// place that knows the order; the parc command and the tests both use it.
//
//   source -> scanner -> parser -> semantic checks -> loop dependence analysis
//          -> IR builder -> flow checks -> optimizer -> code generator
//
// A phase only runs if the phases before it found no errors.
namespace parc {

struct CompileOptions {
    bool optimize = true;
    bool force_parallel = false;  // keep unsafe 'parallel for' loops parallel, to show a race
    int registers = 12;
};

// How long one phase took, measured with a steady clock around the call.
struct PhaseTime {
    std::string phase;  // "scanner", "parser", ...
    double ms = 0.0;
};

struct Compilation {
    std::vector<PhaseTime> times;  // in the order the phases ran
    ScanResult scanned;
    ParseResult parsed;
    sem::Result sema;
    opt::LoopReport loops;
    ir::Program ir;         // as built from the tree
    ir::Program optimized;  // after the optimizer; equal to ir when optimization is off
    opt::PassLog passes;
    vm::Program code;

    std::vector<Diagnostic> syntax_errors;  // from the scanner and the parser
    std::vector<Diagnostic> findings;       // everything later phases report, numbered F1, F2, ...
    std::vector<sem::CheckedOps> checked;   // per function: operations that can fail at run time, and how many may

    // All functions together. at_risk / checked is the vulnerability percentage.
    sem::OpCount checked_total() const;

    bool analyzed = false;  // there were no syntax errors, so the semantic checks ran
    bool lowered = false;   // there were no semantic errors, so the IR exists
    bool runnable = false;  // there were no errors at all, so the code exists

    // "syntax", "semantic" or "complete": the last stage that ran.
    std::string stage() const { return runnable ? "complete" : analyzed ? "semantic" : "syntax"; }
    int count(std::string_view severity) const;
};

Compilation compile(std::string_view source, const CompileOptions& options = {});

// The findings.json file: {"file", "stage", "errors", "warnings", "checked": {...}, "findings": [...]}
std::string findings_json(const Compilation& c, std::string_view filename);

}  // namespace parc
