#include "ir/ir.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>

#include "common/json.hpp"
#include "common/json_writer.hpp"

namespace parc::ir {

bool Operand::operator==(const Operand& other) const {
    if (kind != other.kind) return false;
    switch (kind) {
        case Kind::None:
        case Kind::Null: return true;
        case Kind::Var: return var == other.var;
        case Kind::Int:
        case Kind::Str: return i == other.i;
        // Bit patterns, so 0.0 and -0.0 stay different and NaN equals itself.
        case Kind::Float: return std::bit_cast<std::uint64_t>(f) == std::bit_cast<std::uint64_t>(other.f);
        case Kind::Bool: return b == other.b;
    }
    return false;
}

bool is_terminator(Op op) { return op == Op::Jump || op == Op::Branch || op == Op::Return; }

bool is_compare(Op op) {
    return op == Op::Lt || op == Op::Le || op == Op::Gt || op == Op::Ge || op == Op::Eq || op == Op::Ne;
}

bool is_arithmetic(Op op) {
    return op == Op::Add || op == Op::Sub || op == Op::Mul || op == Op::Div || op == Op::Mod;
}

std::string_view op_symbol(Op op) {
    switch (op) {
        case Op::Add: return "+";
        case Op::Sub: return "-";
        case Op::Mul: return "*";
        case Op::Div: return "/";
        case Op::Mod: return "%";
        case Op::Neg: return "-";
        case Op::Lt: return "<";
        case Op::Le: return "<=";
        case Op::Gt: return ">";
        case Op::Ge: return ">=";
        case Op::Eq: return "==";
        case Op::Ne: return "!=";
        case Op::Not: return "!";
        default: return "?";
    }
}

int Function::add_var(std::string var_name, ast::Type type, VarKind kind, Pos var_pos) {
    Var v;
    v.name = std::move(var_name);
    v.type = type;
    v.kind = kind;
    v.pos = var_pos;
    vars.push_back(std::move(v));
    return static_cast<int>(vars.size()) - 1;
}

int Function::instruction_count() const {
    int count = 0;
    for (const auto& block : blocks) count += static_cast<int>(block.instrs.size());
    return count;
}

const Function* Program::find(std::string_view name) const {
    for (const auto& fn : functions)
        if (fn.name == name) return &fn;
    return nullptr;
}

void link(Function& fn) {
    for (auto& block : fn.blocks) {
        block.preds.clear();
        block.succs.clear();
    }
    for (size_t i = 0; i < fn.blocks.size(); ++i) {
        Block& block = fn.blocks[i];
        if (block.instrs.empty()) continue;
        const Instr& last = block.instrs.back();
        if (last.op == Op::Jump) {
            block.succs = {last.target};
        } else if (last.op == Op::Branch) {
            block.succs = {last.target};
            if (last.target2 != last.target) block.succs.push_back(last.target2);
        }
        for (int s : block.succs) fn.blocks[static_cast<size_t>(s)].preds.push_back(static_cast<int>(i));
    }
}

std::vector<Operand*> use_operands(Instr& instr) {
    std::vector<Operand*> out;
    switch (instr.op) {
        case Op::Copy:
        case Op::Neg:
        case Op::Not:
        case Op::IntToFloat:
        case Op::NewArray:
        case Op::Len:
        case Op::Branch: out = {&instr.a}; break;
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
        case Op::Ne:
        case Op::Load: out = {&instr.a, &instr.b}; break;
        case Op::Store: out = {&instr.a, &instr.b, &instr.c}; break;
        case Op::Return:
            if (!instr.a.is_none()) out = {&instr.a};
            break;
        case Op::Call:
        case Op::Print:
            for (auto& arg : instr.args) out.push_back(&arg);
            break;
        case Op::Jump:
        case Op::ParFor:
        case Op::ParEnd: break;
    }
    return out;
}

std::vector<int> uses(const Instr& instr) {
    std::vector<int> out;
    for (const Operand* o : use_operands(const_cast<Instr&>(instr)))
        if (o->is_var()) out.push_back(o->var);
    if (instr.op == Op::ParFor) {
        out.push_back(instr.a.var);
        out.push_back(instr.b.var);
    }
    if (instr.op == Op::ParFor || instr.op == Op::ParEnd)
        for (const auto& r : instr.reductions) out.push_back(r.var);
    return out;
}

std::vector<int> defs(const Instr& instr) {
    std::vector<int> out;
    if (instr.dst.is_var()) out.push_back(instr.dst.var);
    // Each thread of a parallel loop starts with its own loop variable, its own
    // end of range and fresh reduction variables, so the fork writes all of
    // them. The join writes the combined reduction results.
    if (instr.op == Op::ParFor) {
        out.push_back(instr.a.var);
        out.push_back(instr.b.var);
    }
    if (instr.op == Op::ParFor || instr.op == Op::ParEnd)
        for (const auto& r : instr.reductions) out.push_back(r.var);
    return out;
}

bool has_side_effect(const Instr& instr) {
    switch (instr.op) {
        case Op::Store:
        case Op::Call:
        case Op::Print:
        case Op::Jump:
        case Op::Branch:
        case Op::Return:
        case Op::ParFor:
        case Op::ParEnd: return true;
        default: return false;
    }
}

bool can_trap(const Instr& instr) {
    switch (instr.op) {
        case Op::Div:
        case Op::Mod: return instr.type == VT::Int && !(instr.b.is_int() && instr.b.i != 0);
        case Op::NewArray: return !(instr.a.is_int() && instr.a.i >= 0);
        case Op::Load:
        case Op::Store:
        case Op::Len:
        case Op::Call: return true;
        default: return false;
    }
}

bool is_pure(const Instr& instr) {
    switch (instr.op) {
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Mod:
        case Op::Neg:
        case Op::Lt:
        case Op::Le:
        case Op::Gt:
        case Op::Ge:
        case Op::Eq:
        case Op::Ne:
        case Op::Not:
        case Op::IntToFloat: return true;
        default: return false;
    }
}

// ---------------------------------------------------------------- text

std::string operand_text(const Program& program, const Function& fn, const Operand& operand) {
    switch (operand.kind) {
        case Operand::Kind::None: return "";
        case Operand::Kind::Var: return fn.vars[static_cast<size_t>(operand.var)].name;
        case Operand::Kind::Int: return std::to_string(operand.i);
        case Operand::Kind::Float: return json::number(operand.f);
        case Operand::Kind::Bool: return operand.b ? "true" : "false";
        case Operand::Kind::Null: return "null";
        case Operand::Kind::Str: return json::quote(program.strings[static_cast<size_t>(operand.i)]);
    }
    return "?";
}

namespace {

std::string reductions_text(const Function& fn, const Instr& instr) {
    if (instr.reductions.empty()) return "";
    std::string out = " reduce(" + std::string(1, instr.reductions[0].op) + ":";
    for (size_t i = 0; i < instr.reductions.size(); ++i)
        out += (i ? ", " : " ") + fn.vars[static_cast<size_t>(instr.reductions[i].var)].name;
    return out + ")";
}

}  // namespace

std::string instr_text(const Program& program, const Function& fn, const Instr& instr) {
    auto text = [&](const Operand& o) { return operand_text(program, fn, o); };
    auto label = [&](int block) { return fn.blocks[static_cast<size_t>(block)].label; };
    std::string dst = instr.dst.is_var() ? text(instr.dst) + " = " : "";
    auto list = [&](const std::vector<Operand>& args) {
        std::string out;
        for (size_t i = 0; i < args.size(); ++i) out += (i ? ", " : "") + text(args[i]);
        return out;
    };
    switch (instr.op) {
        case Op::Copy: return dst + text(instr.a);
        case Op::Neg:
        case Op::Not: return dst + std::string(op_symbol(instr.op)) + text(instr.a);
        case Op::IntToFloat: return dst + "float(" + text(instr.a) + ")";
        case Op::NewArray: return dst + "new " + std::string(ast::to_string(instr.elem)) + "[" + text(instr.a) + "]";
        case Op::Load: return dst + text(instr.a) + "[" + text(instr.b) + "]";
        case Op::Store: return text(instr.a) + "[" + text(instr.b) + "] = " + text(instr.c);
        case Op::Len: return dst + "len(" + text(instr.a) + ")";
        case Op::Call: return dst + "call " + instr.callee + "(" + list(instr.args) + ")";
        case Op::Print: return instr.args.empty() ? "print" : "print " + list(instr.args);
        case Op::Jump: return "goto " + label(instr.target);
        case Op::Branch: return "if " + text(instr.a) + " goto " + label(instr.target) + " else " + label(instr.target2);
        case Op::Return: return instr.a.is_none() ? "return" : "return " + text(instr.a);
        case Op::ParFor:
            return "parfor " + text(instr.a) + " .. " + text(instr.b) + reductions_text(fn, instr);
        case Op::ParEnd: return "parend" + reductions_text(fn, instr);
        default: return dst + text(instr.a) + " " + std::string(op_symbol(instr.op)) + " " + text(instr.b);
    }
}

std::string signature(const Function& fn) {
    std::string out = "fn " + fn.name + "(";
    for (size_t i = 0; i < fn.params.size(); ++i) {
        const Var& v = fn.vars[static_cast<size_t>(fn.params[i])];
        out += (i ? ", " : "") + v.name + ": " + ast::to_string(v.type);
    }
    out += ")";
    if (fn.return_type.base != ast::BaseType::Void) out += " -> " + ast::to_string(fn.return_type);
    return out;
}

std::string to_text(const Program& program, const Function& fn) {
    std::string out = signature(fn) + "\n";
    for (const auto& block : fn.blocks) {
        out += block.label + ":\n";
        for (const auto& instr : block.instrs) out += "    " + instr_text(program, fn, instr) + "\n";
    }
    return out;
}

std::string to_text(const Program& program) {
    std::string out;
    for (const auto& fn : program.functions) out += (out.empty() ? "" : "\n") + to_text(program, fn);
    return out;
}

// ---------------------------------------------------------------- JSON

std::string to_json(const Program& program, std::string_view filename) {
    json::Writer w;
    w.begin_object();
    w.key("file");
    w.string(filename);
    w.key("functions");
    w.begin_array();
    for (const auto& fn : program.functions) {
        w.begin_object();
        w.key("name");
        w.string(fn.name);
        w.key("signature");
        w.string(signature(fn));
        w.key("line");
        w.integer(fn.pos.line);
        w.key("vars");
        w.begin_array();
        for (const auto& v : fn.vars) {
            w.begin_object();
            w.key("name");
            w.string(v.name);
            w.key("type");
            w.string(ast::to_string(v.type));
            w.key("kind");
            w.string(v.kind == VarKind::Param ? "parameter" : v.kind == VarKind::Local ? "variable" : "temporary");
            w.end_object();
        }
        w.end_array();
        w.key("blocks");
        w.begin_array();
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            const Block& block = fn.blocks[b];
            w.begin_object();
            w.key("id");
            w.integer(static_cast<long long>(b));
            w.key("label");
            w.string(block.label);
            w.key("preds");
            w.begin_array();
            for (int p : block.preds) w.integer(p);
            w.end_array();
            w.key("succs");
            w.begin_array();
            for (int s : block.succs) w.integer(s);
            w.end_array();
            w.key("instrs");
            w.begin_array();
            for (const auto& instr : block.instrs) {
                w.begin_object();
                w.key("text");
                w.string(instr_text(program, fn, instr));
                w.key("line");
                w.integer(instr.pos.line);
                w.key("col");
                w.integer(instr.pos.col);
                w.end_object();
            }
            w.end_array();
            w.end_object();
        }
        w.end_array();
        w.end_object();
    }
    w.end_array();
    w.end_object();
    return w.str() + "\n";
}

}  // namespace parc::ir
