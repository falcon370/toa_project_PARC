// The pass list, the pass log and its JSON form.

#include <set>

#include "common/json_writer.hpp"
#include "common/util.hpp"
#include "optimizer/passes.hpp"

namespace parc::opt {

namespace {

struct PassInfo {
    const char* name;
    const char* what;
    bool (*run)(const ir::Program&, ir::Function&, PassRun&);
};

const PassInfo kConstants{"constant propagation",
                          "Replaces variables that always hold one constant, and computes operations on constants now.",
                          constant_propagation};
const PassInfo kCopies{"copy propagation", "After x = y, uses y directly where x was used.", copy_propagation};
const PassInfo kValueNumbering{"local value numbering",
                               "Reuses a result that the same block has already computed.", local_value_numbering};
const PassInfo kSubexpressions{"common subexpression elimination",
                               "Reuses a result that every path to this point has already computed.",
                               common_subexpressions};
const PassInfo kDeadCode{"dead code elimination", "Deletes computations whose results nothing uses.",
                         dead_code_elimination};
const PassInfo kCleanup{"control-flow cleanup",
                        "Removes blocks that can't run and joins blocks that always run one after the other.",
                        simplify_cfg};
const PassInfo kInvariants{"loop-invariant code motion",
                           "Moves computations that give the same result on every pass out of the loop.",
                           loop_invariant_code_motion};
const PassInfo kStrength{"strength reduction",
                         "Turns a multiplication by the loop counter into a running addition.", strength_reduction};

// The order matters: propagation exposes repeated and dead computations, the
// cleanup straightens the graph for the loop passes, and a second round picks
// up what the loop passes left behind.
const PassInfo* const kPipeline[] = {
    &kConstants, &kCopies,   &kValueNumbering, &kSubexpressions, &kCopies,   &kDeadCode, &kCleanup,
    &kInvariants, &kStrength, &kConstants,      &kCopies,         &kDeadCode, &kCleanup,
};

}  // namespace

PassLog optimize(ir::Program& program) {
    PassLog log;
    for (auto& fn : program.functions) {
        log.instructions_before += fn.instruction_count();
        for (const PassInfo* pass : kPipeline) {
            PassRun run;
            run.function = fn.name;
            run.pass = pass->name;
            run.what = pass->what;
            run.before = ir::to_text(program, fn);
            bool changed = pass->run(program, fn, run);
            ir::link(fn);
            run.after = ir::to_text(program, fn);
            if (changed || !run.missed.empty()) log.runs.push_back(std::move(run));
        }
        log.instructions_after += fn.instruction_count();
    }
    return log;
}

std::string verify(const ir::Program& program) {
    for (const auto& fn : program.functions) {
        auto fail = [&fn](const std::string& what) { return "function " + fn.name + ": " + what; };
        if (fn.blocks.empty()) return fail("has no blocks");
        int blocks = isize(fn.blocks);
        for (const auto& block : fn.blocks) {
            if (block.instrs.empty()) return fail("block " + block.label + " is empty");
            for (size_t i = 0; i < block.instrs.size(); ++i) {
                const ir::Instr& instr = block.instrs[i];
                bool last = i + 1 == block.instrs.size();
                if (ir::is_terminator(instr.op) != last)
                    return fail("block " + block.label + " must end with its only jump, branch or return");
                bool has_target = instr.op == ir::Op::Jump || instr.op == ir::Op::Branch || instr.op == ir::Op::ParFor;
                if (has_target && (instr.target < 0 || instr.target >= blocks))
                    return fail("block " + block.label + " jumps to a block that does not exist");
                bool two_targets = instr.op == ir::Op::Branch || instr.op == ir::Op::ParFor;
                if (two_targets && (instr.target2 < 0 || instr.target2 >= blocks))
                    return fail("block " + block.label + " branches to a block that does not exist");
                for (int v : ir::uses(instr))
                    if (v < 0 || v >= isize(fn.vars)) return fail("block " + block.label + " uses an unknown variable");
                for (int v : ir::defs(instr))
                    if (v < 0 || v >= isize(fn.vars)) return fail("block " + block.label + " writes an unknown variable");
            }
        }
    }
    return "";
}

std::string passes_json(const PassLog& log, std::string_view filename) {
    json::Writer w;
    auto notes = [&w](const std::vector<Note>& list) {
        w.begin_array();
        for (const auto& note : list) {
            w.begin_object();
            w.key("line");
            w.integer(note.line);
            w.key("text");
            w.string(note.text);
            w.end_object();
        }
        w.end_array();
    };
    w.begin_object();
    w.key("file");
    w.string(filename);
    w.key("instructions_before");
    w.integer(log.instructions_before);
    w.key("instructions_after");
    w.integer(log.instructions_after);
    w.key("runs");
    w.begin_array();
    for (const auto& run : log.runs) {
        w.begin_object();
        w.key("function");
        w.string(run.function);
        w.key("pass");
        w.string(run.pass);
        w.key("what");
        w.string(run.what);
        w.key("changes");
        notes(run.changes);
        w.key("missed");
        notes(run.missed);
        w.key("before");
        w.string(run.before);
        w.key("after");
        w.string(run.after);
        w.end_object();
    }
    w.end_array();
    w.end_object();
    return w.str() + "\n";
}

std::vector<Diagnostic> missed_optimizations(const PassLog& log) {
    std::vector<Diagnostic> out;
    std::set<std::pair<int, std::string>> seen;
    for (const auto& run : log.runs) {
        for (const auto& note : run.missed) {
            if (!seen.insert({note.line, note.text}).second) continue;
            Diagnostic d;
            d.phase = "optimizer";
            d.severity = "info";
            d.kind = "missed-optimization";
            d.line = note.line;
            d.col = 1;
            d.stmt_line = note.line;
            d.function = run.function;
            d.message = note.text;
            d.proof = run.pass + ": " + run.what;
            out.push_back(std::move(d));
        }
    }
    return out;
}

}  // namespace parc::opt
