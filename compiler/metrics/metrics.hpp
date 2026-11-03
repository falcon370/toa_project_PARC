#pragma once

#include <string>
#include <vector>

#include "cli/pipeline.hpp"
#include "metrics/clang_bridge.hpp"
#include "metrics/complexity.hpp"
#include "metrics/measure.hpp"
#include "vm/vm.hpp"

// "parc metrics": how much time, energy and memory a ParC program costs, and
// how its cost grows.
//
//   compile    the time of each of ParC's phases (measured in parc::compile)
//   VM run     time, processor time, instructions, calls, heap, energy, power
//   native     the same program as C, compiled by clang -O2 and measured the same way
//   complexity time and space per function from Clang's syntax tree, with rewrites that lower them
//   Clang      its optimizer's remarks at -O2 and its warnings, mapped to ParC lines
//
// Short programs are repeated until a measurement lasts at least min_ms, and
// the numbers are given per run.
namespace parc::metrics {

struct Options {
    bool energy = true;
    bool native = true;
    double min_ms = 200.0;
    std::string work_dir;  // where clang writes its files; a temporary folder (removed after) when empty
    vm::Options run;
};

struct VmMeasurement {
    bool ran = false;
    vm::Result result;      // of the first run
    Measurement cost;
    long long peak_memory_bytes = 0;  // of the whole parc process
};

struct NativeMeasurement {
    bool ran = false;
    std::string error;
    NativeBuild build;
    Measurement cost;            // per run of ParC's main, without process start-up
    double startup_ms = 0.0;     // starting and ending the process with no work
    bool same_output = false;
    std::string output;
    long long peak_memory_bytes = 0;
};

struct Report {
    std::string file;
    std::vector<PhaseTime> compile_times;
    double compile_ms = 0.0;
    int ir_before = 0;           // IR instructions before and after ParC's optimizer
    int ir_after = 0;
    std::string stage;
    std::string energy_source;
    bool energy_available = false;
    double idle_w = 0.0;
    std::vector<std::string> functions;  // the VM's function names, by function number
    VmMeasurement vm;
    NativeMeasurement native;
    CUnit clang;                 // libclang's view: version, parse time, warnings
    ComplexityReport complexity;
};

Report collect(const std::string& file, const Compilation& c, const Options& options);

// Complexity only, for a ParC program or a C file.
ComplexityReport complexity_of_parc(const Compilation& c, const std::string& file, CUnit* unit = nullptr);
ComplexityReport complexity_of_c(const std::string& path, CUnit* unit = nullptr);

std::string to_text(const Report& report);
std::string to_json(const Report& report);
std::string complexity_text(const ComplexityReport& report);
std::string complexity_json(const ComplexityReport& report, const std::string& file);

}  // namespace parc::metrics
