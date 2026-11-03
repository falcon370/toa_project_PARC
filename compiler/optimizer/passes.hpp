#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "common/diagnostic.hpp"
#include "ir/ir.hpp"

// The optimizer: a list of passes, each of which rewrites one function's IR
// and writes down, in plain sentences, what it changed and what it could not
// change and why. The program must behave exactly the same after every pass.
namespace parc::opt {

struct Note {
    int line = 0;  // source line, 0 if the change has none
    std::string text;
};

// The record of one pass on one function.
struct PassRun {
    std::string function;
    std::string pass;            // "constant propagation"
    std::string what;            // one sentence: what this pass is for
    std::vector<Note> changes;   // what it did
    std::vector<Note> missed;    // what it could not do, and why
    std::string before;          // the function's IR going in
    std::string after;           // and coming out
};

struct PassLog {
    std::vector<PassRun> runs;  // only passes that changed something or have a note
    int instructions_before = 0;
    int instructions_after = 0;
};

// Each pass returns true if it changed the function.

// Finds variables that always hold one known constant (Kildall's lattice:
// unknown / constant / varying), puts the constant in their place, computes
// operations on constants at compile time, simplifies x + 0, x * 1, x * 0,
// x - x, and turns a branch on a constant into a jump.
bool constant_propagation(const ir::Program& program, ir::Function& fn, PassRun& run);

// After "x = y", uses of x become y for as long as neither changes.
bool copy_propagation(const ir::Program& program, ir::Function& fn, PassRun& run);

// Inside one block, gives equal computations the same number and reuses the first result.
bool local_value_numbering(const ir::Program& program, ir::Function& fn, PassRun& run);

// Across blocks: an expression that every path has already computed
// (available expressions) is not computed again.
bool common_subexpressions(const ir::Program& program, ir::Function& fn, PassRun& run);

// Mark and sweep: marks instructions with a visible effect, then everything
// their operands were computed by, and deletes the rest.
bool dead_code_elimination(const ir::Program& program, ir::Function& fn, PassRun& run);

// Removes blocks that can't run, skips blocks that only jump on, and joins a
// block to the single block that follows it.
bool simplify_cfg(const ir::Program& program, ir::Function& fn, PassRun& run);

// Moves a computation whose operands don't change in a loop to just before the loop.
bool loop_invariant_code_motion(const ir::Program& program, ir::Function& fn, PassRun& run);

// Replaces i * c inside a loop, where i grows by a fixed step, with a value
// that grows by c * step: a multiplication becomes an addition.
bool strength_reduction(const ir::Program& program, ir::Function& fn, PassRun& run);

// Runs the whole pass list on every function.
PassLog optimize(ir::Program& program);

// Checks that the IR is well formed (every block ends in exactly one
// terminator, every jump goes to a real block). Returns "" if it is.
std::string verify(const ir::Program& program);

// The passes.json file.
std::string passes_json(const PassLog& log, std::string_view filename);

// The "could not" notes as info findings, for the report and the agent.
std::vector<Diagnostic> missed_optimizations(const PassLog& log);

// Shared by the loop passes: the block that runs once just before the loop
// with this header, created if the loop has no single such block. Returns its
// index; the header's index may have moved, so it is passed by reference.
int ensure_preheader(ir::Function& fn, int& header, const std::vector<int>& loop_blocks);

// Removes the blocks whose flag is false and renumbers the jumps.
void remove_blocks(ir::Function& fn, const std::vector<bool>& keep);

// Adds two ints the way the VM does: wrapping around at 32 bits.
long long wrap32(long long value);

}  // namespace parc::opt
