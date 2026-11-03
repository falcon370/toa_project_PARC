#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "cli/pipeline.hpp"
#include "common/json.hpp"
#include "common/json_writer.hpp"
#include "ir/cfg.hpp"
#include "metrics/metrics.hpp"
#include "optimizer/analyses.hpp"
#include "vm/vm.hpp"

namespace {

using parc::Compilation;

int usage() {
    std::cerr <<
        "usage: parc COMMAND FILE [options]\n"
        "\n"
        "  tokens FILE [--json OUT]    the scanner's tokens\n"
        "  ast    FILE [--json OUT]    the parser's syntax tree\n"
        "  check  FILE [--proof]       every error, warning and note, from all phases\n"
        "  ir     FILE                 the three-address code\n"
        "  passes FILE                 what each optimizer pass changed, and what it could not\n"
        "  asm    FILE                 the VM code\n"
        "  run    FILE [--json OUT]    compile and run on the VM\n"
        "  build  FILE --out DIR       write every phase's JSON file into DIR\n"
        "  call   FILE --fn NAME --inputs FILE   run one function on many inputs (JSON result)\n"
        "  metrics FILE [--json OUT]   time, energy and memory of compiling and running, the same program\n"
        "                              built by clang -O2, complexity per function, Clang's remarks\n"
        "  complexity FILE [--json OUT]  time and space complexity per function (FILE may also be a .c file)\n"
        "\n"
        "options for ir, asm, run, build and call:\n"
        "  -O0               do not optimize\n"
        "  --regs N          registers per VM frame (default 12)\n"
        "  --force-parallel  run unsafe 'parallel for' loops in parallel anyway\n"
        "options for run and call:\n"
        "  --threads N       threads per parallel loop (default 4)\n"
        "  --seed N          seed of the thread scheduler (default 1)\n"
        "  --max-steps N     stop after this many VM instructions (default 50000000)\n"
        "  --trace N         with --json: record the first N instructions for the debugger\n"
        "options for metrics:\n"
        "  --min-ms N        repeat short runs until a measurement lasts N milliseconds (default 200)\n"
        "  --no-native       do not build and run the program with clang -O2\n"
        "  --no-energy       do not read the energy meter\n"
        "  --keep DIR        keep clang's files (program.c, the remarks, the program) in DIR\n";
    return 2;
}

struct Args {
    std::string command;
    std::string path;
    std::string json_out;
    std::string out_dir;
    std::string function;
    std::string inputs;
    bool proof = false;
    parc::CompileOptions compile;
    parc::vm::Options run;
    parc::metrics::Options metrics;
};

bool parse_args(int argc, char** argv, Args& args) {
    if (argc < 3) return false;
    args.command = argv[1];
    args.path = argv[2];
    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        auto value = [&](std::string& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        auto number = [&](long long& out) {
            std::string text;
            if (!value(text)) return false;
            char* end = nullptr;
            out = std::strtoll(text.c_str(), &end, 10);
            return end != text.c_str() && *end == '\0';
        };
        long long n = 0;
        if (arg == "--json") {
            if (!value(args.json_out)) return false;
        } else if (arg == "--out") {
            if (!value(args.out_dir)) return false;
        } else if (arg == "--fn") {
            if (!value(args.function)) return false;
        } else if (arg == "--inputs") {
            if (!value(args.inputs)) return false;
        } else if (arg == "--proof") {
            args.proof = true;
        } else if (arg == "-O0") {
            args.compile.optimize = false;
        } else if (arg == "--force-parallel") {
            args.compile.force_parallel = true;
        } else if (arg == "--regs") {
            if (!number(n) || n < 4 || n > 64) return false;
            args.compile.registers = static_cast<int>(n);
        } else if (arg == "--threads") {
            if (!number(n) || n < 1 || n > 64) return false;
            args.run.threads = static_cast<int>(n);
        } else if (arg == "--seed") {
            if (!number(n) || n < 0) return false;
            args.run.seed = static_cast<std::uint32_t>(n);
        } else if (arg == "--max-steps") {
            if (!number(n) || n < 1) return false;
            args.run.max_steps = n;
        } else if (arg == "--trace") {
            if (!number(n) || n < 0 || n > 100000) return false;
            args.run.trace_steps = static_cast<int>(n);
        } else if (arg == "--min-ms") {
            if (!number(n) || n < 1 || n > 60000) return false;
            args.metrics.min_ms = static_cast<double>(n);
        } else if (arg == "--no-native") {
            args.metrics.native = false;
        } else if (arg == "--no-energy") {
            args.metrics.energy = false;
        } else if (arg == "--keep") {
            if (!value(args.metrics.work_dir)) return false;
        } else {
            return false;
        }
    }
    return true;
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

bool write_file(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::cerr << "parc: cannot write " << path << "\n";
        return false;
    }
    out << text;
    return true;
}

void report(const std::string& file, const std::vector<parc::Diagnostic>& diags) {
    for (const auto& d : diags) std::cerr << file << ":" << parc::format(d) << "\n";
}

// Errors stop the back end. Prints them and returns false if there are any.
bool usable(const std::string& name, const Compilation& c) {
    if (c.runnable) return true;
    report(name, c.syntax_errors);
    for (const auto& d : c.findings)
        if (d.severity == "error") std::cerr << name << ":" << parc::format(d) << "\n";
    std::cerr << "parc: " << name << " has errors, so there is no code to show or run\n";
    return false;
}

int cmd_tokens(const std::string& name, const std::string& source, const std::string& json_out) {
    parc::ScanResult result = parc::scan(source);
    if (!json_out.empty()) {
        if (!write_file(json_out, parc::to_json(result, name))) return 2;
        std::cout << "wrote " << result.tokens.size() << " tokens to " << json_out << "\n";
    } else {
        for (const auto& t : result.tokens) {
            std::string shown = t.kind == parc::Kind::String ? parc::json::quote(std::get<std::string>(t.value)) : t.text;
            std::printf("%4d:%-4d %-10s %s\n", t.line, t.col, std::string(parc::kind_name(t.kind)).c_str(), shown.c_str());
        }
        std::fflush(stdout);
    }
    report(name, result.errors);
    return result.errors.empty() ? 0 : 1;
}

int cmd_ast(const std::string& name, const std::string& source, const std::string& json_out) {
    parc::ScanResult scanned = parc::scan(source);
    parc::ParseResult parsed = parc::parse(scanned.tokens);
    if (!json_out.empty()) {
        if (!write_file(json_out, parc::ast::to_json(parsed.program, name, parsed.errors))) return 2;
        std::cout << "wrote " << parsed.program.functions.size() << " functions to " << json_out << "\n";
    } else {
        std::cout << parc::ast::to_tree(parsed.program) << std::flush;
    }
    report(name, scanned.errors);
    report(name, parsed.errors);
    return scanned.errors.empty() && parsed.errors.empty() ? 0 : 1;
}

int cmd_check(const std::string& name, const Compilation& c, bool proof) {
    auto show = [&](const parc::Diagnostic& d) {
        std::cout << name << ":" << parc::format(d);
        if (!d.id.empty()) std::cout << "  [" << d.id << (d.cwe.empty() ? "" : ", " + d.cwe) << "]";
        std::cout << "\n";
        if (proof && !d.proof.empty()) std::cout << "    because: " << d.proof << "\n";
    };
    for (const auto& d : c.syntax_errors) show(d);
    for (const auto& d : c.findings) show(d);
    int errors = c.count("error");
    int warnings = c.count("warning");
    int notes = c.count("info");
    auto plural = [](int n, const char* word) { return std::to_string(n) + " " + word + (n == 1 ? "" : "s"); };
    std::cout << name << ": " << plural(errors, "error") << ", " << plural(warnings, "warning") << ", "
              << plural(notes, "note") << "\n";
    if (!c.analyzed) std::cout << name << ": fix the syntax errors first; the later checks did not run\n";
    // The vulnerability percentage: of the operations that can fail when the
    // program runs, how many could the compiler not prove safe?
    parc::sem::OpCount ops = c.checked_total();
    if (c.lowered && ops.checked > 0) {
        std::cout << name << ": vulnerability " << (100 * ops.at_risk + ops.checked / 2) / ops.checked << "%: "
                  << ops.at_risk << " of " << plural(ops.checked, "checked operation") << " can fail at run time";
        if (ops.possible) std::cout << ", " << ops.possible << " more only possibly";
        std::cout << "\n";
    }
    return errors ? 1 : 0;
}

int cmd_passes(const Compilation& c) {
    if (c.passes.runs.empty()) std::cout << "the optimizer found nothing to change\n";
    for (const auto& run : c.passes.runs) {
        std::cout << "== " << run.function << ": " << run.pass << "\n   " << run.what << "\n";
        for (const auto& note : run.changes)
            std::cout << "   " << (note.line ? "line " + std::to_string(note.line) + ": " : "") << note.text << "\n";
        for (const auto& note : run.missed)
            std::cout << "   not done, " << (note.line ? "line " + std::to_string(note.line) + ": " : "") << note.text << "\n";
    }
    std::cout << "instructions: " << c.passes.instructions_before << " before, " << c.passes.instructions_after
              << " after\n";
    return 0;
}

int cmd_run(const std::string& name, const Compilation& c, const Args& args) {
    parc::vm::Result result = parc::vm::run(c.code, args.run);
    std::cout << result.output << std::flush;
    for (const auto& e : result.events)
        std::cerr << name << ":" << e.line << ": note: " << e.text << " (in " << e.function << ")\n";
    if (!result.ok) {
        std::cerr << name << ":" << result.error_line << ": runtime error: " << result.error;
        if (!result.error_function.empty()) std::cerr << " (in " << result.error_function << ")";
        std::cerr << "\n";
    }
    if (!args.json_out.empty() && !write_file(args.json_out, parc::vm::to_json(result, c.code))) return 2;
    return result.ok ? 0 : 1;
}

int cmd_build(const std::string& name, const std::string& source, const Compilation& c, const std::string& dir) {
    std::error_code ignored;
    std::filesystem::create_directories(dir, ignored);
    auto path = [&dir](const char* file) { return (std::filesystem::path(dir) / file).string(); };
    bool ok = write_file(path("tokens.json"), parc::to_json(c.scanned, name)) &&
              write_file(path("ast.json"), parc::ast::to_json(c.parsed.program, name, c.parsed.errors)) &&
              write_file(path("findings.json"), parc::findings_json(c, name));
    (void)source;
    if (ok && c.analyzed) ok = write_file(path("symbols.json"), parc::sem::symbols_json(c.sema, name));
    if (ok && c.lowered) {
        ok = write_file(path("ir.json"), parc::ir::to_json(c.ir, name)) &&
             write_file(path("cfg.json"), parc::ir::cfg_json(c.ir, name)) &&
             write_file(path("dataflow.json"), parc::opt::dataflow_json(c.ir, name));
    }
    if (ok && c.runnable) {
        ok = write_file(path("ir_opt.json"), parc::ir::to_json(c.optimized, name)) &&
             write_file(path("passes.json"), parc::opt::passes_json(c.passes, name)) &&
             write_file(path("code.json"), parc::vm::to_json(c.code, name));
    }
    if (!ok) return 2;
    std::cout << "stage: " << c.stage() << ", " << c.count("error") << " errors, " << c.count("warning") << " warnings\n";
    return c.count("error") ? 1 : 0;
}

// One argument of a test call, written as 5, 2.5, true, null or [1,2,3].
bool parse_arg(const std::string& text, const parc::ast::Type& type, parc::vm::Arg& out) {
    using parc::vm::Value;
    auto scalar = [](const std::string& t, parc::ast::BaseType base, Value& v) {
        char* end = nullptr;
        if (base == parc::ast::BaseType::Bool) {
            if (t != "true" && t != "false") return false;
            v = Value::of_bool(t == "true");
            return true;
        }
        if (base == parc::ast::BaseType::Float) {
            v = Value::of_float(std::strtod(t.c_str(), &end));
        } else {
            long long n = std::strtoll(t.c_str(), &end, 10);
            if (n < -2147483648LL || n > 2147483647LL) return false;
            v = Value::of_int(static_cast<std::int32_t>(n));
        }
        return end != t.c_str() && *end == '\0';
    };
    if (!type.is_array) return scalar(text, type.base, out.value);
    out.is_array = true;
    out.elem = type.base == parc::ast::BaseType::Float ? 1 : type.base == parc::ast::BaseType::Bool ? 2 : 0;
    if (text == "null") {
        out.is_null = true;
        return true;
    }
    if (text.size() < 2 || text.front() != '[' || text.back() != ']') return false;
    std::stringstream items(text.substr(1, text.size() - 2));
    std::string item;
    while (std::getline(items, item, ',')) {
        Value v;
        if (!scalar(item, type.base, v)) return false;
        out.items.push_back(v);
    }
    return true;
}

// Runs one function once per line of the inputs file and prints the results
// as JSON. The agent's gate uses this to compare a function with its rewrite.
int cmd_call(const Compilation& c, const Args& args) {
    int index = c.code.find(args.function);
    const parc::ir::Function* fn = c.optimized.find(args.function);
    if (index < 0 || !fn) {
        std::cerr << "parc: there is no function named " << args.function << "\n";
        return 2;
    }
    std::string text;
    if (!read_file(args.inputs, text)) {
        std::cerr << "parc: cannot read " << args.inputs << "\n";
        return 2;
    }
    parc::json::Writer w;
    w.begin_object();
    w.key("function");
    w.string(args.function);
    w.key("results");
    w.begin_array();
    std::stringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::vector<parc::vm::Arg> call_args;
        std::stringstream words(line);
        std::string word;
        bool valid = true;
        for (size_t k = 0; k < fn->params.size() && valid; ++k) {
            parc::vm::Arg arg;
            valid = static_cast<bool>(words >> word) &&
                    parse_arg(word, fn->vars[static_cast<size_t>(fn->params[k])].type, arg);
            call_args.push_back(std::move(arg));
        }
        if (!valid || (words >> word)) {
            std::cerr << "parc: this line does not match the parameters of " << args.function << ": " << line << "\n";
            return 2;
        }
        parc::vm::Result r = parc::vm::run_function(c.code, index, call_args, args.run);
        w.begin_object();
        w.key("args");
        w.string(line);
        w.key("ok");
        w.boolean(r.ok);
        w.key("error");
        w.string(r.error);
        w.key("returned");
        w.string(r.returned);
        w.key("output");
        w.string(r.output);
        w.key("arrays");
        w.begin_array();
        for (const auto& a : r.arrays) w.string(a);
        w.end_array();
        w.key("overflows");
        w.integer(r.overflows);
        w.key("races");
        w.integer(r.races);
        w.end_object();
    }
    w.end_array();
    w.end_object();
    std::cout << w.str() << "\n";
    return 0;
}

int cmd_metrics(const std::string& name, const Compilation& c, const Args& args) {
    if (!c.analyzed) {
        report(name, c.syntax_errors);
        std::cerr << "parc: " << name << " has syntax errors, so there is nothing to measure\n";
        return 1;
    }
    parc::metrics::Options options = args.metrics;
    options.run = args.run;
    parc::metrics::Report r = parc::metrics::collect(name, c, options);
    if (!args.json_out.empty()) {
        if (!write_file(args.json_out, parc::metrics::to_json(r))) return 2;
        std::cout << "wrote the metrics to " << args.json_out << "\n";
    } else {
        std::cout << parc::metrics::to_text(r) << std::flush;
    }
    if (!c.runnable) std::cerr << "parc: " << name << " has errors, so it was not run; only the complexity is shown\n";
    return 0;
}

int cmd_complexity(const std::string& path, const std::string& name, const std::string& source, const Args& args) {
    parc::metrics::ComplexityReport r;
    if (std::filesystem::path(path).extension() == ".c") {
        r = parc::metrics::complexity_of_c(path);
    } else {
        Compilation c = parc::compile(source, args.compile);
        if (!c.analyzed) {
            report(name, c.syntax_errors);
            return 1;
        }
        r = parc::metrics::complexity_of_parc(c, name);
    }
    if (!args.json_out.empty()) {
        if (!write_file(args.json_out, parc::metrics::complexity_json(r, name))) return 2;
        std::cout << "wrote the complexity of " << r.functions.size() << " functions to " << args.json_out << "\n";
    } else {
        std::cout << parc::metrics::complexity_text(r) << std::flush;
    }
    return r.ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) return usage();
    static const char* const commands[] = {"tokens", "ast",   "check", "ir",      "passes",    "asm",
                                           "run",    "build", "call",  "metrics", "complexity"};
    bool known = false;
    for (const char* command : commands) known = known || args.command == command;
    if (!known) return usage();
    if (args.command == "build" && args.out_dir.empty()) return usage();
    if (args.command == "call" && (args.function.empty() || args.inputs.empty())) return usage();

    std::string source;
    if (!read_file(args.path, source)) {
        std::cerr << "parc: cannot read " << args.path << "\n";
        return 2;
    }
    std::string name = std::filesystem::path(args.path).filename().string();
    if (args.command == "tokens") return cmd_tokens(name, source, args.json_out);
    if (args.command == "ast") return cmd_ast(name, source, args.json_out);
    if (args.command == "complexity") return cmd_complexity(args.path, name, source, args);

    Compilation c = parc::compile(source, args.compile);
    if (args.command == "check") return cmd_check(name, c, args.proof);
    if (args.command == "metrics") return cmd_metrics(name, c, args);
    if (args.command == "build") return cmd_build(name, source, c, args.out_dir);
    if (!usable(name, c)) return 1;
    if (args.command == "ir") {
        std::cout << parc::ir::to_text(c.optimized) << std::flush;
        return 0;
    }
    if (args.command == "passes") return cmd_passes(c);
    if (args.command == "asm") {
        std::cout << parc::vm::to_text(c.code) << std::flush;
        return 0;
    }
    if (args.command == "call") return cmd_call(c, args);
    return cmd_run(name, c, args);
}
