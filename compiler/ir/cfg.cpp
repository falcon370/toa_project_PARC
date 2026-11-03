#include "ir/cfg.hpp"

#include <algorithm>

#include "common/json_writer.hpp"
#include "common/util.hpp"

namespace parc::ir {

namespace {

void postorder_visit(const Function& fn, int block, std::vector<bool>& seen, std::vector<int>& order) {
    seen[static_cast<size_t>(block)] = true;
    for (int s : fn.blocks[static_cast<size_t>(block)].succs)
        if (!seen[static_cast<size_t>(s)]) postorder_visit(fn, s, seen, order);
    order.push_back(block);
}

}  // namespace

std::vector<bool> reachable_blocks(const Function& fn) {
    std::vector<bool> seen(fn.blocks.size(), false);
    std::vector<int> order;
    if (!fn.blocks.empty()) postorder_visit(fn, 0, seen, order);
    return seen;
}

std::vector<int> reverse_postorder(const Function& fn) {
    std::vector<bool> seen(fn.blocks.size(), false);
    std::vector<int> order;
    if (!fn.blocks.empty()) postorder_visit(fn, 0, seen, order);
    std::reverse(order.begin(), order.end());
    return order;
}

std::vector<int> immediate_dominators(const Function& fn) {
    std::vector<int> rpo = reverse_postorder(fn);
    std::vector<int> number(fn.blocks.size(), -1);  // position in reverse postorder
    for (size_t i = 0; i < rpo.size(); ++i) number[static_cast<size_t>(rpo[i])] = static_cast<int>(i);

    std::vector<int> idom(fn.blocks.size(), -1);
    if (rpo.empty()) return idom;
    idom[0] = 0;  // the entry dominates itself while the algorithm runs

    // Walks two blocks up the dominator tree until they meet.
    auto intersect = [&](int a, int b) {
        while (a != b) {
            while (number[static_cast<size_t>(a)] > number[static_cast<size_t>(b)]) a = idom[static_cast<size_t>(a)];
            while (number[static_cast<size_t>(b)] > number[static_cast<size_t>(a)]) b = idom[static_cast<size_t>(b)];
        }
        return a;
    };

    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 1; i < rpo.size(); ++i) {
            int block = rpo[i];
            int best = -1;
            for (int p : fn.blocks[static_cast<size_t>(block)].preds) {
                if (idom[static_cast<size_t>(p)] == -1) continue;  // not processed yet, or unreachable
                best = best == -1 ? p : intersect(p, best);
            }
            if (best != idom[static_cast<size_t>(block)]) {
                idom[static_cast<size_t>(block)] = best;
                changed = true;
            }
        }
    }
    idom[0] = -1;
    return idom;
}

bool dominates(const std::vector<int>& idom, int a, int b) {
    while (b != -1) {
        if (a == b) return true;
        b = idom[static_cast<size_t>(b)];
    }
    return false;
}

std::vector<Loop> natural_loops(const Function& fn, const std::vector<int>& idom) {
    std::vector<bool> reachable = reachable_blocks(fn);
    std::vector<Loop> loops;
    for (int h = 0; h < isize(fn.blocks); ++h) {
        if (!reachable[static_cast<size_t>(h)]) continue;
        Loop loop;
        loop.header = h;
        std::vector<bool> inside(fn.blocks.size(), false);
        inside[static_cast<size_t>(h)] = true;
        std::vector<int> work;
        for (int t : fn.blocks[static_cast<size_t>(h)].preds) {
            if (!reachable[static_cast<size_t>(t)] || !dominates(idom, h, t)) continue;
            loop.latches.push_back(t);  // t -> h is a back edge
            if (!inside[static_cast<size_t>(t)]) {
                inside[static_cast<size_t>(t)] = true;
                work.push_back(t);
            }
        }
        if (loop.latches.empty()) continue;
        // Everything that reaches a latch without passing the header is in the loop.
        while (!work.empty()) {
            int block = work.back();
            work.pop_back();
            for (int p : fn.blocks[static_cast<size_t>(block)].preds) {
                if (inside[static_cast<size_t>(p)] || !reachable[static_cast<size_t>(p)]) continue;
                inside[static_cast<size_t>(p)] = true;
                work.push_back(p);
            }
        }
        for (int b = 0; b < isize(fn.blocks); ++b)
            if (inside[static_cast<size_t>(b)]) loop.blocks.push_back(b);
        // A parallel loop is entered from a block that ends in "parfor; goto header".
        for (int p : fn.blocks[static_cast<size_t>(h)].preds) {
            if (inside[static_cast<size_t>(p)]) continue;
            for (const auto& instr : fn.blocks[static_cast<size_t>(p)].instrs)
                if (instr.op == Op::ParFor && instr.target == h) loop.parallel = true;
        }
        loops.push_back(std::move(loop));
    }
    // Larger loops first, so a loop always comes after the loops that contain it.
    std::stable_sort(loops.begin(), loops.end(),
                     [](const Loop& a, const Loop& b) { return a.blocks.size() > b.blocks.size(); });
    for (size_t i = 0; i < loops.size(); ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (std::binary_search(loops[j].blocks.begin(), loops[j].blocks.end(), loops[i].header) &&
                loops[j].header != loops[i].header)
                ++loops[i].depth;
        }
    }
    return loops;
}

int cyclomatic_complexity(const Function& fn) {
    std::vector<bool> reachable = reachable_blocks(fn);
    int nodes = 0;
    int edges = 0;
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        if (!reachable[b]) continue;
        ++nodes;
        edges += isize(fn.blocks[b].succs);
    }
    // Every returning block has an edge to one shared exit in McCabe's model;
    // with that exit node the formula is E - N + 2.
    int returns = 0;
    for (size_t b = 0; b < fn.blocks.size(); ++b)
        if (reachable[b] && fn.blocks[b].succs.empty()) ++returns;
    return (edges + returns) - (nodes + 1) + 2;
}

std::string cfg_json(const Program& program, std::string_view filename) {
    json::Writer w;
    w.begin_object();
    w.key("file");
    w.string(filename);
    w.key("functions");
    w.begin_array();
    for (const auto& fn : program.functions) {
        std::vector<bool> reachable = reachable_blocks(fn);
        std::vector<int> idom = immediate_dominators(fn);
        std::vector<Loop> loops = natural_loops(fn, idom);

        w.begin_object();
        w.key("name");
        w.string(fn.name);
        w.key("blocks");
        w.begin_array();
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            w.begin_object();
            w.key("id");
            w.integer(static_cast<long long>(b));
            w.key("label");
            w.string(fn.blocks[b].label);
            w.key("reachable");
            w.boolean(reachable[b]);
            w.key("idom");
            w.integer(idom[b]);
            w.key("code");
            w.begin_array();
            for (const auto& instr : fn.blocks[b].instrs) w.string(instr_text(program, fn, instr));
            w.end_array();
            w.end_object();
        }
        w.end_array();

        w.key("edges");
        w.begin_array();
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            if (fn.blocks[b].instrs.empty()) continue;
            const Instr& last = fn.blocks[b].instrs.back();
            for (int s : fn.blocks[b].succs) {
                w.begin_object();
                w.key("from");
                w.integer(static_cast<long long>(b));
                w.key("to");
                w.integer(s);
                w.key("kind");
                w.string(last.op != Op::Branch ? "jump" : s == last.target ? "true" : "false");
                w.key("back");
                w.boolean(reachable[b] && dominates(idom, s, static_cast<int>(b)));
                w.end_object();
            }
        }
        w.end_array();

        w.key("loops");
        w.begin_array();
        for (const auto& loop : loops) {
            w.begin_object();
            w.key("header");
            w.integer(loop.header);
            w.key("blocks");
            w.begin_array();
            for (int b : loop.blocks) w.integer(b);
            w.end_array();
            w.key("depth");
            w.integer(loop.depth);
            w.key("parallel");
            w.boolean(loop.parallel);
            w.end_object();
        }
        w.end_array();

        w.key("metrics");
        w.begin_object();
        w.key("cyclomatic_complexity");
        w.integer(cyclomatic_complexity(fn));
        w.key("nesting_depth");
        w.integer(fn.max_nesting);
        w.key("blocks");
        w.integer(isize(fn.blocks));
        w.key("instructions");
        w.integer(fn.instruction_count());
        w.key("loops");
        w.integer(isize(loops));
        w.end_object();
        w.end_object();
    }
    w.end_array();
    w.end_object();
    return w.str() + "\n";
}

}  // namespace parc::ir
