#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "vm/bytecode.hpp"

// The interpreter for the VM code: a fetch-decode-execute loop, a call stack
// of frames, a heap of arrays, and threads for parallel loops.
//
// Parallel loops run on VM threads, not operating-system threads. PARFOR
// copies the current frame once per thread and gives each copy its own part of
// the loop range. The scheduler lets one thread run for one pass of the loop,
// then picks the next thread with a random number generator that starts from a
// seed, so the same seed always gives the same interleaving. The schedule is
// recorded so a run can be explained or repeated.
namespace parc::vm {

struct Value {
    enum class Tag : unsigned char { Int, Float, Bool, Ref };
    Tag tag = Tag::Int;
    std::int32_t i = 0;  // the int, the bool (0 or 1), or the array number (0 = null)
    double f = 0.0;

    static Value of_int(std::int32_t v) { return {Tag::Int, v, 0.0}; }
    static Value of_float(double v) { return {Tag::Float, 0, v}; }
    static Value of_bool(bool v) { return {Tag::Bool, v ? 1 : 0, 0.0}; }
    static Value of_ref(std::int32_t v) { return {Tag::Ref, v, 0.0}; }
    bool operator==(const Value&) const = default;
};

std::string to_string(const Value& v);  // as print shows it; an array as "null" or "array#3"

struct Options {
    int threads = 4;                    // threads per parallel loop
    std::uint32_t seed = 1;             // for the scheduler
    long long max_steps = 50'000'000;   // stops endless loops
    int trace_steps = 0;                // record this many executed instructions for the debugger
};

// Something worth knowing that did not stop the program.
struct Event {
    std::string kind;  // "overflow" or "race"
    int line = 0;
    std::string function;
    std::string text;
};

// One executed instruction, as the debugger shows it.
struct TraceStep {
    int thread = 0;  // 0 = main; workers of a parallel loop are 1, 2, ...
    int function = 0;
    int pc = 0;
    int line = 0;
    int depth = 1;                     // call stack depth
    std::vector<std::string> regs;     // the frame's registers after the instruction
    std::vector<std::string> slots;
    std::string output;                // text printed by this instruction
    std::string heap;                  // "new #2 [5]" or "#2[3] = 7", when it touched an array
};

struct Result {
    bool ok = true;
    std::string output;
    std::string error;           // why it stopped, when !ok
    int error_line = 0;
    std::string error_function;
    std::string returned;        // the value main (or the called function) returned, if any
    long long steps = 0;
    std::vector<long long> function_steps;  // steps spent in each function, by function number
    long long calls = 0;         // CALL instructions executed
    int max_depth = 0;           // deepest call stack of any thread
    long long heap_cells = 0;    // array elements allocated; arrays are never freed, so this is also the peak
    long long heap_arrays = 0;   // arrays allocated
    std::vector<Event> events;   // the first few; the counts below are complete
    long long overflows = 0;
    long long races = 0;
    std::vector<std::pair<int, int>> schedule;  // (thread, passes of the loop it ran before the next switch)
    int parallel_loops = 0;      // how many times a PARFOR started threads
    std::vector<TraceStep> trace;
    std::vector<std::string> arrays;  // run_function: the argument arrays after the call
};

// Runs main.
Result run(const Program& program, const Options& options = {});

// An argument for run_function.
struct Arg {
    Value value;                 // for int, float and bool
    bool is_array = false;       // an array, or null when is_null
    bool is_null = false;
    int elem = 0;                // 0 int, 1 float, 2 bool
    std::vector<Value> items;
};

// Calls one function with the given arguments, in a fresh machine. Used to
// compare an original function with a rewritten one on many inputs.
Result run_function(const Program& program, int function, const std::vector<Arg>& args, const Options& options = {});

// The run.json file: {"ok", "output", "error", "events", "schedule", "trace", ...}
std::string to_json(const Result& result, const Program& program);

}  // namespace parc::vm
