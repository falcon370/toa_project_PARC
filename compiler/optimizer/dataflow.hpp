#pragma once

#include <algorithm>
#include <functional>
#include <set>
#include <vector>

#include "ir/cfg.hpp"
#include "ir/ir.hpp"

// One engine for every data-flow analysis (Kildall's monotone framework).
//
// An analysis describes itself with four things:
//   - a State: what is known at one point in the program
//   - a direction: forward (facts flow along the edges) or backward
//   - meet: how to combine the facts of several paths that join
//   - transfer: how one block changes the facts
// The solver then repeats "combine the neighbours, apply the block" until
// nothing changes. Visiting blocks in reverse postorder makes that quick.
namespace parc::opt {

template <typename State>
struct Problem {
    bool forward = true;
    State boundary;  // the facts at the function's entry (forward) or at every exit (backward)
    State initial;   // the first guess everywhere else: the neutral value of meet
    std::function<State(const State&, const State&)> meet;
    // Forward: the state at the block's start -> the state at its end. Backward: end -> start.
    std::function<State(int block, const State&)> transfer;
    // Optional, forward only: sharpen the facts along one edge (for example "x < 10" on a true branch).
    std::function<State(int from, int to, const State&)> edge;
    // Optional: called when a block's input changes on a repeat visit, so an analysis with
    // endlessly growing states (intervals) can jump ahead and stop.
    std::function<State(int block, const State& before, const State& now, int visits)> widen;
};

template <typename State>
struct Solution {
    std::vector<State> in;   // at each block's start
    std::vector<State> out;  // at each block's end
    int visits = 0;          // how many times a block was processed
};

template <typename State>
Solution<State> solve(const ir::Function& fn, const Problem<State>& problem) {
    size_t n = fn.blocks.size();
    Solution<State> s;
    s.in.assign(n, problem.initial);
    s.out.assign(n, problem.initial);

    std::vector<int> order = ir::reverse_postorder(fn);
    if (!problem.forward) std::reverse(order.begin(), order.end());
    std::vector<int> priority(n, -1);
    for (size_t i = 0; i < order.size(); ++i) priority[static_cast<size_t>(order[i])] = static_cast<int>(i);

    // The worklist holds priorities, so the next block is always the earliest in visiting order.
    std::set<int> worklist;
    for (size_t i = 0; i < order.size(); ++i) worklist.insert(static_cast<int>(i));
    std::vector<int> block_visits(n, 0);

    while (!worklist.empty()) {
        int block = order[static_cast<size_t>(*worklist.begin())];
        worklist.erase(worklist.begin());
        size_t b = static_cast<size_t>(block);
        ++s.visits;
        int visit = ++block_visits[b];

        const ir::Block& node = fn.blocks[b];
        const std::vector<int>& sources = problem.forward ? node.preds : node.succs;
        std::vector<State>& joined = problem.forward ? s.in : s.out;
        std::vector<State>& result = problem.forward ? s.out : s.in;

        // 1. Combine what flows in from the neighbours.
        bool at_boundary = problem.forward ? block == 0 : node.succs.empty();
        State now = at_boundary ? problem.boundary : problem.initial;
        bool first = !at_boundary;
        for (int source : sources) {
            if (priority[static_cast<size_t>(source)] < 0) continue;  // unreachable
            const State& from = problem.forward ? s.out[static_cast<size_t>(source)] : s.in[static_cast<size_t>(source)];
            State value = problem.forward && problem.edge ? problem.edge(source, block, from) : from;
            now = first ? value : problem.meet(now, value);
            first = false;
        }
        if (problem.widen && visit > 1) now = problem.widen(block, joined[b], now, visit);
        joined[b] = now;

        // 2. Apply the block. If its result changed, the blocks it feeds must be redone.
        State after = problem.transfer(block, now);
        if (visit > 1 && after == result[b]) continue;
        result[b] = std::move(after);
        for (int next : problem.forward ? node.succs : node.preds) {
            int p = priority[static_cast<size_t>(next)];
            if (p >= 0) worklist.insert(p);
        }
    }
    return s;
}

}  // namespace parc::opt
