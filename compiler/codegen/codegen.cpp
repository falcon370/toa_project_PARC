#include "codegen/codegen.hpp"

#include <algorithm>
#include <limits>
#include <set>

#include "common/util.hpp"
#include "ir/cfg.hpp"
#include "optimizer/analyses.hpp"

namespace parc::codegen {

using ir::Op;
using ir::Operand;
using ir::VT;
using vm::Loc;
using vm::VOp;

// ---------------------------------------------------------------- live intervals

std::vector<Interval> live_intervals(const ir::Function& fn) {
    opt::Solution<BitSet> live = opt::liveness(fn);
    std::vector<bool> reachable = ir::reachable_blocks(fn);
    std::vector<int> first(fn.vars.size(), std::numeric_limits<int>::max());
    std::vector<int> last(fn.vars.size(), -1);
    auto extend = [&](int v, int position) {
        first[static_cast<size_t>(v)] = std::min(first[static_cast<size_t>(v)], position);
        last[static_cast<size_t>(v)] = std::max(last[static_cast<size_t>(v)], position);
    };

    int position = 0;
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        if (!reachable[b]) continue;
        int block_start = position;
        for (const auto& instr : fn.blocks[b].instrs) {
            for (int v : ir::uses(instr)) extend(v, position);
            for (int v : ir::defs(instr)) extend(v, position);
            ++position;
        }
        // A value needed in a later block stays alive through this whole block,
        // even where the numbering of a loop makes that look like a gap.
        for (int v : live.in[b].members()) extend(v, block_start);
        for (int v : live.out[b].members()) extend(v, position - 1);
    }
    for (int param : fn.params)
        if (last[static_cast<size_t>(param)] >= 0) extend(param, 0);  // parameters arrive at the start

    std::vector<Interval> out;
    for (int v = 0; v < isize(fn.vars); ++v)
        if (last[static_cast<size_t>(v)] >= 0) out.push_back({v, first[static_cast<size_t>(v)], last[static_cast<size_t>(v)], {}});
    std::stable_sort(out.begin(), out.end(), [](const Interval& a, const Interval& b) { return a.start < b.start; });
    return out;
}

// ---------------------------------------------------------------- linear scan

int linear_scan(std::vector<Interval>& intervals, int registers) {
    std::set<int> free;
    for (int r = 0; r < registers; ++r) free.insert(r);
    std::vector<Interval*> active;  // intervals that hold a register now, ordered by end
    int slots = 0;

    for (auto& current : intervals) {
        // Expire: intervals that ended before this one starts give their register back.
        while (!active.empty() && active.front()->end < current.start) {
            free.insert(active.front()->loc.index);
            active.erase(active.begin());
        }
        if (free.empty()) {
            // No register left. Spill whichever of the active intervals and the
            // current one ends last: it would block a register the longest.
            Interval* furthest = active.back();
            if (furthest->end > current.end) {
                current.loc = furthest->loc;
                furthest->loc = Loc{true, slots++};
                active.pop_back();
            } else {
                current.loc = Loc{true, slots++};
                continue;
            }
        } else {
            current.loc = Loc{false, *free.begin()};  // the lowest free register
            free.erase(free.begin());
        }
        auto place = std::find_if(active.begin(), active.end(), [&](const Interval* a) { return a->end > current.end; });
        active.insert(place, &current);
    }
    return slots;
}

// ---------------------------------------------------------------- code for one function

namespace {

class FunctionGen {
public:
    FunctionGen(const ir::Function& fn, const vm::Program& out, int registers)
        : fn_(fn), out_(out), registers_(registers) {}

    vm::Function run(int& peephole_removed) {
        vm::Function vf;
        vf.name = fn_.name;
        vf.params = isize(fn_.params);
        vf.registers = registers_;

        // Three registers stay out of the allocator's hands: an instruction
        // has at most three operands, and each may need a constant or a
        // spilled value loaded just before it.
        std::vector<Interval> intervals = live_intervals(fn_);
        vf.slots = linear_scan(intervals, registers_ - 3);
        loc_.assign(fn_.vars.size(), Loc{});
        for (const auto& interval : intervals) loc_[static_cast<size_t>(interval.var)] = interval.loc;

        // Arguments are copied to where the allocator put each parameter.
        for (int k = 0; k < isize(fn_.params); ++k) {
            Loc loc = loc_[static_cast<size_t>(fn_.params[static_cast<size_t>(k)])];
            if (!loc.valid()) continue;  // never used
            vm::Instr get;
            get.op = VOp::GETARG;
            get.a = loc.slot ? scratch(0) : loc.index;
            get.imm = k;
            get.line = fn_.pos.line;
            prologue_.push_back(get);
            if (loc.slot) prologue_.push_back(make(VOp::STORES, scratch(0), -1, -1, loc.index, fn_.pos.line));
        }

        std::vector<bool> reachable = ir::reachable_blocks(fn_);
        code_.resize(fn_.blocks.size());
        std::vector<std::vector<int>> offsets(fn_.blocks.size());  // where each IR instruction's code starts in its block
        for (size_t b = 0; b < fn_.blocks.size(); ++b) {
            if (!reachable[b]) continue;
            block_ = static_cast<int>(b);
            for (const auto& instr : fn_.blocks[b].instrs) {
                offsets[b].push_back(isize(code_[b]));
                line_ = instr.pos.line > 0 ? instr.pos.line : instr.stmt.line;
                select(instr, vf);
            }
        }

        // Peephole on jumps: a jump to a block that only jumps on goes straight to the end of the chain...
        for (auto& block : code_) {
            for (auto& instr : block) {
                if (instr.op != VOp::JMP && instr.op != VOp::BRT && instr.op != VOp::BRF) continue;
                for (int hops = 0; hops < 16; ++hops) {
                    const auto& target = code_[static_cast<size_t>(instr.imm)];
                    if (target.size() != 1 || target[0].op != VOp::JMP || target[0].imm == instr.imm) break;
                    instr.imm = target[0].imm;
                    ++peephole_removed;
                }
            }
        }
        // ...and a jump to the code that comes next anyway is not needed. Removing
        // one can empty a block and make another jump pointless, so repeat.
        for (bool again = true; again;) {
            again = false;
            for (size_t b = 0; b < code_.size(); ++b) {
                if (code_[b].empty() || code_[b].back().op != VOp::JMP) continue;
                size_t target = static_cast<size_t>(code_[b].back().imm);
                bool nothing_between = target > b;
                for (size_t between = b + 1; between < target && nothing_between; ++between)
                    nothing_between = code_[between].empty();
                if (!nothing_between) continue;
                code_[b].pop_back();
                ++peephole_removed;
                again = true;
            }
        }
        peephole_removed += removed_;

        // Lay the blocks out and fill in the addresses (backpatching).
        std::vector<int> address(fn_.blocks.size(), 0);
        int pc = isize(prologue_);
        for (size_t b = 0; b < code_.size(); ++b) {
            address[b] = pc;
            pc += isize(code_[b]);
        }
        vf.code = prologue_;
        for (const auto& block : code_) vf.code.insert(vf.code.end(), block.begin(), block.end());
        for (auto& instr : vf.code)
            if (instr.op == VOp::JMP || instr.op == VOp::BRT || instr.op == VOp::BRF)
                instr.imm = address[static_cast<size_t>(instr.imm)];
        for (size_t i = 0; i < vf.loops.size(); ++i) {
            vf.loops[i].head_pc = address[static_cast<size_t>(loop_head_[i])];
            for (size_t at = 0; at < vf.code.size(); ++at)
                if (vf.code[at].op == VOp::PAREND && vf.code[at].imm == static_cast<long long>(i))
                    vf.loops[i].end_pc = static_cast<int>(at);
        }

        // Where each variable lives, for the listing and the debugger.
        // For each IR instruction: the first and the last VM instruction made from it.
        std::vector<int> position_pc;
        std::vector<int> position_last_pc;
        for (size_t b = 0; b < fn_.blocks.size(); ++b) {
            for (size_t k = 0; k < offsets[b].size(); ++k) {
                int first = address[b] + offsets[b][k];
                int next = k + 1 < offsets[b].size() ? address[b] + offsets[b][k + 1] : address[b] + isize(code_[b]);
                position_pc.push_back(first);
                position_last_pc.push_back(std::max(first, next - 1));
            }
        }
        for (const auto& interval : intervals) {
            const ir::Var& var = fn_.vars[static_cast<size_t>(interval.var)];
            vm::VarInfo info;
            info.name = var.name;
            info.type = ast::to_string(var.type);
            info.loc = interval.loc;
            info.from_pc = position_pc[static_cast<size_t>(interval.start)];
            info.to_pc = position_last_pc[static_cast<size_t>(interval.end)];
            vf.vars.push_back(std::move(info));
        }
        return vf;
    }

private:
    const ir::Function& fn_;
    const vm::Program& out_;
    int registers_;
    std::vector<Loc> loc_;
    std::vector<vm::Instr> prologue_;
    std::vector<std::vector<vm::Instr>> code_;  // per block; jumps hold a block number until the end
    std::vector<int> loop_head_;                // per parallel loop: its header block
    std::vector<int> loop_end_block_;           // and the block of its ParEnd
    int block_ = 0;
    int line_ = 0;
    int removed_ = 0;

    int scratch(int k) const { return registers_ - 3 + k; }

    static vm::Instr make(VOp op, int a, int b, int c, long long imm, int line) {
        vm::Instr i;
        i.op = op;
        i.a = a;
        i.b = b;
        i.c = c;
        i.imm = imm;
        i.line = line;
        return i;
    }

    void emit(VOp op, int a = -1, int b = -1, int c = -1, long long imm = 0) {
        auto& block = code_[static_cast<size_t>(block_)];
        // Peephole on the fly: a move from a register to itself, and a reload
        // of the value that was just stored from the same register.
        if (op == VOp::MOV && a == b) {
            ++removed_;
            return;
        }
        if (op == VOp::LOADS && !block.empty() && block.back().op == VOp::STORES && block.back().a == a &&
            block.back().imm == imm) {
            ++removed_;
            return;
        }
        block.push_back(make(op, a, b, c, imm, line_));
    }

    void emit_const(int reg, const Operand& o) {
        switch (o.kind) {
            case Operand::Kind::Float: {
                vm::Instr i = make(VOp::LF, reg, -1, -1, 0, line_);
                i.fimm = o.f;
                code_[static_cast<size_t>(block_)].push_back(i);
                break;
            }
            case Operand::Kind::Bool: emit(VOp::LB, reg, -1, -1, o.b ? 1 : 0); break;
            case Operand::Kind::Null: emit(VOp::LNULL, reg); break;
            default: emit(VOp::LI, reg, -1, -1, o.i); break;
        }
    }

    // The register that holds an operand's value. A constant or a spilled
    // variable is first loaded into scratch register k.
    int use(const Operand& o, int k) {
        if (o.is_var()) {
            Loc loc = loc_[static_cast<size_t>(o.var)];
            if (!loc.slot) return loc.index;
            emit(VOp::LOADS, scratch(k), -1, -1, loc.index);
            return scratch(k);
        }
        emit_const(scratch(k), o);
        return scratch(k);
    }

    // The register to compute a result into, and what to do once it is there.
    int def(const Operand& dst) const {
        Loc loc = loc_[static_cast<size_t>(dst.var)];
        return loc.slot || !loc.valid() ? scratch(0) : loc.index;
    }

    void def_done(const Operand& dst) {
        Loc loc = loc_[static_cast<size_t>(dst.var)];
        if (loc.slot) emit(VOp::STORES, scratch(0), -1, -1, loc.index);
    }

    static VOp int_op(Op op) {
        switch (op) {
            case Op::Add: return VOp::ADD;
            case Op::Sub: return VOp::SUB;
            case Op::Mul: return VOp::MUL;
            case Op::Div: return VOp::DIV;
            case Op::Mod: return VOp::MOD;
            case Op::Lt: return VOp::LT;
            case Op::Le: return VOp::LE;
            case Op::Gt: return VOp::GT;
            case Op::Ge: return VOp::GE;
            case Op::Eq: return VOp::EQ;
            default: return VOp::NE;
        }
    }

    static VOp float_op(Op op) {
        switch (op) {
            case Op::Add: return VOp::FADD;
            case Op::Sub: return VOp::FSUB;
            case Op::Mul: return VOp::FMUL;
            case Op::Div: return VOp::FDIV;
            case Op::Lt: return VOp::FLT;
            case Op::Le: return VOp::FLE;
            case Op::Gt: return VOp::FGT;
            case Op::Ge: return VOp::FGE;
            case Op::Eq: return VOp::EQ;
            default: return VOp::NE;
        }
    }

    // The form of an int operation that takes a constant as its second operand, if there is one.
    static bool immediate_op(Op op, VOp& out) {
        switch (op) {
            case Op::Add: out = VOp::ADDI; return true;
            case Op::Sub: out = VOp::SUBI; return true;
            case Op::Mul: out = VOp::MULI; return true;
            case Op::Lt: out = VOp::LTI; return true;
            case Op::Le: out = VOp::LEI; return true;
            case Op::Gt: out = VOp::GTI; return true;
            case Op::Ge: out = VOp::GEI; return true;
            case Op::Eq: out = VOp::EQI; return true;
            case Op::Ne: out = VOp::NEI; return true;
            default: return false;
        }
    }

    // "5 < x" is "x > 5": the operation to use when the operands are swapped.
    static bool swapped(Op op, Op& out) {
        switch (op) {
            case Op::Add:
            case Op::Mul:
            case Op::Eq:
            case Op::Ne: out = op; return true;
            case Op::Lt: out = Op::Gt; return true;
            case Op::Le: out = Op::Ge; return true;
            case Op::Gt: out = Op::Lt; return true;
            case Op::Ge: out = Op::Le; return true;
            default: return false;
        }
    }

    // Instruction selection: one IR instruction to VM instructions.
    void select(const ir::Instr& instr, vm::Function& vf) {
        switch (instr.op) {
            case Op::Copy: {
                Loc dst = loc_[static_cast<size_t>(instr.dst.var)];
                if (instr.a.is_var()) {
                    Loc src = loc_[static_cast<size_t>(instr.a.var)];
                    if (src.slot && !dst.slot && dst.valid()) {
                        emit(VOp::LOADS, dst.index, -1, -1, src.index);
                    } else if (!src.slot && dst.slot) {
                        emit(VOp::STORES, src.index, -1, -1, dst.index);
                    } else {
                        emit(VOp::MOV, def(instr.dst), use(instr.a, 1));
                        def_done(instr.dst);
                    }
                } else {
                    emit_const(def(instr.dst), instr.a);
                    def_done(instr.dst);
                }
                break;
            }
            case Op::Add:
            case Op::Sub:
            case Op::Mul:
            case Op::Div:
            case Op::Mod:
            case Op::Lt:
            case Op::Le:
            case Op::Gt:
            case Op::Ge:
            case Op::Eq:
            case Op::Ne: {
                if (instr.type == VT::Int) {
                    VOp with_constant;
                    Op mirrored;
                    if (instr.b.is_int() && immediate_op(instr.op, with_constant)) {
                        int a = use(instr.a, 1);
                        emit(with_constant, def(instr.dst), a, -1, instr.b.i);
                    } else if (instr.a.is_int() && swapped(instr.op, mirrored) && immediate_op(mirrored, with_constant)) {
                        int b = use(instr.b, 1);
                        emit(with_constant, def(instr.dst), b, -1, instr.a.i);
                    } else {
                        int a = use(instr.a, 1);
                        int b = use(instr.b, 2);
                        emit(int_op(instr.op), def(instr.dst), a, b);
                    }
                } else {
                    int a = use(instr.a, 1);
                    int b = use(instr.b, 2);
                    emit(instr.type == VT::Float ? float_op(instr.op) : int_op(instr.op), def(instr.dst), a, b);
                }
                def_done(instr.dst);
                break;
            }
            case Op::Neg:
                emit(instr.type == VT::Float ? VOp::FNEG : VOp::NEG, def(instr.dst), use(instr.a, 1));
                def_done(instr.dst);
                break;
            case Op::Not:
                emit(VOp::NOT, def(instr.dst), use(instr.a, 1));
                def_done(instr.dst);
                break;
            case Op::IntToFloat:
                emit(VOp::I2F, def(instr.dst), use(instr.a, 1));
                def_done(instr.dst);
                break;
            case Op::NewArray: {
                int n = use(instr.a, 1);
                long long type = instr.elem == ast::BaseType::Float ? 1 : instr.elem == ast::BaseType::Bool ? 2 : 0;
                emit(VOp::NEWARR, def(instr.dst), n, -1, type);
                def_done(instr.dst);
                break;
            }
            case Op::Load: {
                int a = use(instr.a, 1);
                int i = use(instr.b, 2);
                emit(VOp::ALOAD, def(instr.dst), a, i);
                def_done(instr.dst);
                break;
            }
            case Op::Store: {
                int a = use(instr.a, 0);
                int i = use(instr.b, 1);
                int v = use(instr.c, 2);
                emit(VOp::ASTORE, a, i, v);
                break;
            }
            case Op::Len:
                emit(VOp::LEN, def(instr.dst), use(instr.a, 1));
                def_done(instr.dst);
                break;
            case Op::Call: {
                for (const auto& arg : instr.args) emit(VOp::ARG, use(arg, 1));
                int callee = out_.find(instr.callee);
                emit(VOp::CALL, instr.dst.is_var() ? def(instr.dst) : -1, isize(instr.args), -1, callee);
                if (instr.dst.is_var()) def_done(instr.dst);
                break;
            }
            case Op::Print:
                if (instr.args.empty()) emit(VOp::PRINTLN);
                for (size_t k = 0; k < instr.args.size(); ++k) {
                    int ends_line = k + 1 == instr.args.size() ? 1 : 0;
                    if (instr.args[k].kind == Operand::Kind::Str) {
                        emit(VOp::PRINTS, -1, -1, ends_line, string_index(instr.args[k]));
                    } else {
                        emit(VOp::PRINT, use(instr.args[k], 1), -1, ends_line);
                    }
                }
                break;
            case Op::Jump: emit(VOp::JMP, -1, -1, -1, instr.target); break;
            case Op::Branch: {
                int cond = use(instr.a, 1);
                // Fall through to whichever target comes next in the layout.
                if (instr.target == block_ + 1) {
                    emit(VOp::BRF, cond, -1, -1, instr.target2);
                } else {
                    emit(VOp::BRT, cond, -1, -1, instr.target);
                    emit(VOp::JMP, -1, -1, -1, instr.target2);
                }
                break;
            }
            case Op::Return:
                emit(VOp::RET, instr.a.is_none() ? -1 : use(instr.a, 1));
                break;
            case Op::ParFor: {
                vm::ParLoop loop;
                loop.var = loc_[static_cast<size_t>(instr.a.var)];
                loop.end = loc_[static_cast<size_t>(instr.b.var)];
                for (const auto& r : instr.reductions) {
                    const ast::Type& type = fn_.vars[static_cast<size_t>(r.var)].type;
                    loop.reductions.push_back({loc_[static_cast<size_t>(r.var)], r.op, type.base == ast::BaseType::Float});
                }
                loop.line = line_;
                emit(VOp::PARFOR, -1, -1, -1, isize(vf.loops));
                vf.loops.push_back(std::move(loop));
                loop_head_.push_back(instr.target);
                loop_end_block_.push_back(instr.target2);
                break;
            }
            case Op::ParEnd:
                for (size_t i = 0; i < loop_end_block_.size(); ++i)
                    if (loop_end_block_[i] == block_) emit(VOp::PAREND, -1, -1, -1, static_cast<long long>(i));
                break;
        }
    }

    // VM strings are the IR's strings, in the same order.
    static long long string_index(const Operand& o) { return o.i; }
};

}  // namespace

vm::Program generate(const ir::Program& program, const Options& options) {
    vm::Program out;
    out.registers = std::max(4, options.registers);
    out.strings = program.strings;
    // Function numbers must exist before any call is generated.
    for (const auto& fn : program.functions) {
        vm::Function stub;
        stub.name = fn.name;
        out.functions.push_back(std::move(stub));
    }
    for (size_t i = 0; i < program.functions.size(); ++i)
        out.functions[i] = FunctionGen(program.functions[i], out, out.registers).run(out.peephole_removed);
    return out;
}

}  // namespace parc::codegen
