// Passes and helpers that change the shape of the control-flow graph.

#include <algorithm>

#include "common/util.hpp"
#include "ir/cfg.hpp"
#include "optimizer/passes.hpp"

namespace parc::opt {

using ir::Function;
using ir::Instr;
using ir::Op;

namespace {

// Applies f to every block number stored in an instruction.
template <typename F>
void each_target(Function& fn, F&& f) {
    for (auto& block : fn.blocks) {
        for (auto& instr : block.instrs) {
            if (instr.target >= 0) instr.target = f(instr.target, instr);
            if (instr.target2 >= 0) instr.target2 = f(instr.target2, instr);
        }
    }
}

bool is_parallel_header(const Function& fn, int block) {
    for (const auto& b : fn.blocks)
        for (const auto& instr : b.instrs)
            if (instr.op == Op::ParFor && instr.target == block) return true;
    return false;
}

}  // namespace

void remove_blocks(Function& fn, const std::vector<bool>& keep) {
    std::vector<int> new_index(fn.blocks.size(), -1);
    std::vector<ir::Block> kept;
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        if (!keep[b]) continue;
        new_index[b] = isize(kept);
        kept.push_back(std::move(fn.blocks[b]));
    }
    fn.blocks = std::move(kept);
    each_target(fn, [&new_index](int target, const Instr&) { return new_index[static_cast<size_t>(target)]; });
    ir::link(fn);
}

int ensure_preheader(Function& fn, int& header, const std::vector<int>& loop_blocks) {
    ir::link(fn);
    std::vector<int> outside;
    for (int p : fn.blocks[static_cast<size_t>(header)].preds)
        if (!std::binary_search(loop_blocks.begin(), loop_blocks.end(), p)) outside.push_back(p);
    if (outside.size() == 1 && fn.blocks[static_cast<size_t>(outside[0])].instrs.back().op == Op::Jump)
        return outside[0];  // one block that does nothing but enter the loop: it is the preheader

    // Make a new block right before the header, and send every entry from outside through it.
    int at = header;
    each_target(fn, [at](int target, const Instr&) { return target >= at ? target + 1 : target; });
    ir::Block pre;
    pre.label = "pre." + fn.blocks[static_cast<size_t>(header)].label;
    Instr enter;
    enter.op = Op::Jump;
    enter.target = at + 1;
    pre.instrs.push_back(enter);
    fn.blocks.insert(fn.blocks.begin() + at, std::move(pre));
    header = at + 1;
    for (int p : outside) {
        int moved = p >= at ? p + 1 : p;
        Instr& last = fn.blocks[static_cast<size_t>(moved)].instrs.back();
        if (last.target == header) last.target = at;
        if (last.target2 == header) last.target2 = at;
    }
    ir::link(fn);
    return at;
}

bool simplify_cfg(const ir::Program&, Function& fn, PassRun& run) {
    bool changed = false;
    bool again = true;
    while (again) {
        again = false;
        ir::link(fn);

        // A branch with the same block on both sides is a jump.
        for (auto& block : fn.blocks) {
            if (block.instrs.empty()) continue;
            Instr& last = block.instrs.back();
            if (last.op != Op::Branch || last.target != last.target2) continue;
            last.op = Op::Jump;
            last.a = ir::Operand{};
            last.target2 = -1;
            changed = again = true;
        }
        if (again) continue;

        // Blocks no path reaches.
        std::vector<bool> reachable = ir::reachable_blocks(fn);
        std::string gone;
        for (size_t b = 0; b < fn.blocks.size(); ++b)
            if (!reachable[b]) gone += (gone.empty() ? "" : ", ") + fn.blocks[b].label;
        if (!gone.empty()) {
            remove_blocks(fn, reachable);
            run.changes.push_back({0, "removed " + gone + ": no path leads there"});
            changed = again = true;
            continue;
        }

        // A block that only jumps on: whoever jumps to it can jump straight past it.
        for (int b = 1; b < isize(fn.blocks) && !again; ++b) {
            const auto& instrs = fn.blocks[static_cast<size_t>(b)].instrs;
            if (instrs.size() != 1 || instrs[0].op != Op::Jump || instrs[0].target == b) continue;
            int to = instrs[0].target;
            std::string label = fn.blocks[static_cast<size_t>(b)].label;
            each_target(fn, [b, to](int target, const Instr&) { return target == b ? to : target; });
            run.changes.push_back({0, "skipped " + label + ": it only jumped on to " + fn.blocks[static_cast<size_t>(to)].label});
            changed = again = true;
        }
        if (again) continue;

        // A block whose only way in is one unconditional jump is joined to the block before it.
        for (int a = 0; a < isize(fn.blocks) && !again; ++a) {
            Instr& last = fn.blocks[static_cast<size_t>(a)].instrs.back();
            if (last.op != Op::Jump) continue;
            int b = last.target;
            if (b == 0 || b == a || fn.blocks[static_cast<size_t>(b)].preds.size() != 1) continue;
            if (is_parallel_header(fn, b)) continue;  // threads start at a parallel loop's header
            std::string joined = fn.blocks[static_cast<size_t>(b)].label + " was joined onto the end of " +
                                 fn.blocks[static_cast<size_t>(a)].label;
            auto& into = fn.blocks[static_cast<size_t>(a)].instrs;
            into.pop_back();
            auto& from = fn.blocks[static_cast<size_t>(b)].instrs;
            into.insert(into.end(), from.begin(), from.end());
            std::vector<bool> keep(fn.blocks.size(), true);
            keep[static_cast<size_t>(b)] = false;
            remove_blocks(fn, keep);
            run.changes.push_back({0, joined + ": nothing else jumps there"});
            changed = again = true;
        }
    }
    ir::link(fn);
    return changed;
}

}  // namespace parc::opt
