#pragma once

#include <vector>

#include "ir/ir.hpp"
#include "vm/bytecode.hpp"

// The code generator: three-address IR in, register-VM code out.
//
//   1. Live intervals. Instructions are numbered in layout order; a
//      variable's interval runs from the first to the last number where it
//      holds a value that is still needed (from liveness analysis).
//   2. Linear-scan register allocation (Poletto and Sarkar). Intervals are
//      visited in order of their start; each takes a free register, and when
//      none is free the interval that ends last is spilled to a slot in the frame.
//   3. Macro expansion. Each IR instruction becomes one VM instruction, plus
//      loads and stores around it for spilled operands and constants.
//   4. Backpatching. Jumps are emitted with the block they go to; once every
//      block's address is known, the addresses are filled in.
//   5. Peephole. Jumps to the next instruction, jumps to jumps and moves from
//      a register to itself are removed.
namespace parc::codegen {

struct Options {
    int registers = 12;  // per frame; the last three are kept for constants and spilled values
};

struct Interval {
    int var = -1;
    int start = 0;
    int end = 0;
    vm::Loc loc;
};

// The live interval of every variable that is ever live, sorted by start.
std::vector<Interval> live_intervals(const ir::Function& fn);

// Gives every interval a register (0 .. registers-1) or a slot. Returns the number of slots used.
int linear_scan(std::vector<Interval>& intervals, int registers);

vm::Program generate(const ir::Program& program, const Options& options = {});

}  // namespace parc::codegen
