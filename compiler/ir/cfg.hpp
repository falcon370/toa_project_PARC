#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ir/ir.hpp"

// Questions about a function's control-flow graph: which blocks can run, in
// what order to visit them, which blocks dominate which, and where the loops are.
namespace parc::ir {

// For each block: can it be reached from the entry? (Depth-first search.)
std::vector<bool> reachable_blocks(const Function& fn);

// The reachable blocks in reverse postorder: every block comes before the
// blocks it jumps forward to. Forward data-flow analyses visit in this order.
std::vector<int> reverse_postorder(const Function& fn);

// Immediate dominators by the Cooper-Harvey-Kennedy algorithm. Block d
// dominates block b when every path from the entry to b passes through d;
// the immediate dominator is the closest one. The entry and unreachable
// blocks get -1.
std::vector<int> immediate_dominators(const Function& fn);

// Does block a dominate block b? (A block dominates itself.)
bool dominates(const std::vector<int>& idom, int a, int b);

struct Loop {
    int header = -1;
    std::vector<int> blocks;   // sorted, includes the header
    std::vector<int> latches;  // blocks with a back edge to the header
    int depth = 1;             // 1 for an outermost loop
    bool parallel = false;     // entered through a parfor
};

// Natural loops: a back edge t -> h (h dominates t) makes a loop of h and
// every block that can reach t without passing through h. Loops that share a
// header are merged. Outer loops come before the loops inside them.
std::vector<Loop> natural_loops(const Function& fn, const std::vector<int>& idom);

// McCabe's cyclomatic complexity, E - N + 2, on the reachable part of the graph.
int cyclomatic_complexity(const Function& fn);

// The cfg.json file: per function its blocks, edges, dominators, loops and metrics.
std::string cfg_json(const Program& program, std::string_view filename);

}  // namespace parc::ir
