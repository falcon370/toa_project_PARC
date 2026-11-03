#include "cli/pipeline.hpp"

#include <algorithm>
#include <chrono>

#include "common/json.hpp"
#include "ir/builder.hpp"
#include "semantic/flow_checks.hpp"

namespace parc {

int Compilation::count(std::string_view severity) const {
    auto matches = [severity](const Diagnostic& d) { return d.severity == severity; };
    return static_cast<int>(std::count_if(findings.begin(), findings.end(), matches) +
                            std::count_if(syntax_errors.begin(), syntax_errors.end(), matches));
}

Compilation compile(std::string_view source, const CompileOptions& options) {
    Compilation c;
    auto add = [&c](const std::vector<Diagnostic>& more) {
        c.findings.insert(c.findings.end(), more.begin(), more.end());
    };
    // Runs one phase and adds its time to c.times.
    auto timed = [&c](const char* phase, auto&& work) {
        auto start = std::chrono::steady_clock::now();
        work();
        std::chrono::duration<double, std::milli> took = std::chrono::steady_clock::now() - start;
        c.times.push_back({phase, took.count()});
    };

    // Front end: text -> tokens -> tree.
    timed("scanner", [&] { c.scanned = scan(source); });
    timed("parser", [&] { c.parsed = parse(c.scanned.tokens); });
    c.syntax_errors = c.scanned.errors;
    c.syntax_errors.insert(c.syntax_errors.end(), c.parsed.errors.begin(), c.parsed.errors.end());
    std::stable_sort(c.syntax_errors.begin(), c.syntax_errors.end(), [](const Diagnostic& a, const Diagnostic& b) {
        return a.line != b.line ? a.line < b.line : a.col < b.col;
    });
    if (!c.syntax_errors.empty()) return c;

    // Names and types.
    timed("semantic", [&] { c.sema = sem::analyze(c.parsed.program); });
    c.analyzed = true;
    add(c.sema.diagnostics);
    if (has_errors(c.findings)) {
        sort_and_number(c.findings);
        return c;
    }

    // Which loops may run in parallel, then the IR and the checks that need its graph.
    timed("dependence", [&] { c.loops = opt::analyze_loops(c.parsed.program, c.sema, options.force_parallel); });
    add(c.loops.findings);
    timed("ir builder", [&] { c.ir = ir::build(c.parsed.program, c.sema, c.loops.parallel); });
    c.lowered = true;
    timed("flow checks", [&] { add(sem::check_flow(c.ir, &c.checked)); });

    // Back end. It runs even when there are warnings, but not when there are errors.
    c.optimized = c.ir;
    if (!has_errors(c.findings)) {
        if (options.optimize) {
            timed("optimizer", [&] { c.passes = opt::optimize(c.optimized); });
            add(opt::missed_optimizations(c.passes));
        }
        codegen::Options cg;
        cg.registers = options.registers;
        timed("code generator", [&] { c.code = codegen::generate(c.optimized, cg); });
        c.runnable = true;
    }
    sort_and_number(c.findings);
    return c;
}

sem::OpCount Compilation::checked_total() const {
    sem::OpCount sum;
    for (const auto& fn : checked) {
        sem::OpCount part = fn.total();
        sum.checked += part.checked;
        sum.at_risk += part.at_risk;
        sum.possible += part.possible;
    }
    return sum;
}

namespace {

std::string count_json(const sem::OpCount& n) {
    return "{\"checked\": " + std::to_string(n.checked) + ", \"at_risk\": " + std::to_string(n.at_risk) +
           ", \"possible\": " + std::to_string(n.possible) + "}";
}

// "checked": the operations that can fail at run time, in total and per function.
std::string checked_json(const Compilation& c) {
    sem::OpCount all = c.checked_total();
    std::string out = "{\"operations\": " + std::to_string(all.checked) + ", \"at_risk\": " +
                      std::to_string(all.at_risk) + ", \"possible\": " + std::to_string(all.possible) +
                      ", \"functions\": [";
    for (size_t i = 0; i < c.checked.size(); ++i) {
        const sem::CheckedOps& fn = c.checked[i];
        sem::OpCount sum = fn.total();
        out += i ? ",\n      " : "\n      ";
        out += "{\"name\": " + json::quote(fn.function) + ", \"operations\": " + std::to_string(sum.checked) +
               ", \"at_risk\": " + std::to_string(sum.at_risk) + ", \"possible\": " + std::to_string(sum.possible) +
               ", \"arithmetic\": " + count_json(fn.arithmetic) + ", \"division\": " + count_json(fn.division) +
               ", \"access\": " + count_json(fn.access) + ", \"allocation\": " + count_json(fn.allocation) + "}";
    }
    return out + (c.checked.empty() ? "]}" : "\n    ]}");
}

}  // namespace

std::string findings_json(const Compilation& c, std::string_view filename) {
    std::string out = "{\n  \"file\": " + json::quote(filename) + ",\n  \"stage\": " + json::quote(c.stage()) +
                      ",\n  \"errors\": " + std::to_string(c.count("error")) + ",\n  \"warnings\": " +
                      std::to_string(c.count("warning")) +
                      // Only present when the program got as far as the value analysis.
                      (c.lowered ? ",\n  \"checked\": " + checked_json(c) : std::string()) + ",\n  \"findings\": [";
    for (size_t i = 0; i < c.findings.size(); ++i) {
        out += i ? ",\n    " : "\n    ";
        out += to_json(c.findings[i]);
    }
    out += c.findings.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

}  // namespace parc
