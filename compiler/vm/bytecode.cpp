#include "vm/bytecode.hpp"

#include <cstdio>

#include "common/json.hpp"
#include "common/json_writer.hpp"

namespace parc::vm {

std::string_view op_name(VOp op) {
    switch (op) {
#define PARC_VM_NAME(name, operands) \
    case VOp::name: return #name;
        PARC_VM_OPS(PARC_VM_NAME)
#undef PARC_VM_NAME
    }
    return "?";
}

std::string_view op_operands(VOp op) {
    switch (op) {
#define PARC_VM_OPERANDS(name, operands) \
    case VOp::name: return operands;
        PARC_VM_OPS(PARC_VM_OPERANDS)
#undef PARC_VM_OPERANDS
    }
    return "";
}

int Program::find(std::string_view name) const {
    for (size_t i = 0; i < functions.size(); ++i)
        if (functions[i].name == name) return static_cast<int>(i);
    return -1;
}

std::string loc_text(Loc loc) { return (loc.slot ? "slot " : "r") + std::to_string(loc.index); }

std::string instr_text(const Program& program, const Instr& instr) {
    auto r = [](int n) { return "r" + std::to_string(n); };
    std::string name(op_name(instr.op));
    switch (instr.op) {
        case VOp::LI: return name + " " + r(instr.a) + ", " + std::to_string(instr.imm);
        case VOp::LF: return name + " " + r(instr.a) + ", " + json::number(instr.fimm);
        case VOp::LB: return name + " " + r(instr.a) + ", " + (instr.imm ? "true" : "false");
        case VOp::LNULL: return name + " " + r(instr.a);
        case VOp::MOV:
        case VOp::NEG:
        case VOp::FNEG:
        case VOp::NOT:
        case VOp::I2F:
        case VOp::LEN: return name + " " + r(instr.a) + ", " + r(instr.b);
        case VOp::ADDI:
        case VOp::SUBI:
        case VOp::MULI:
        case VOp::LTI:
        case VOp::LEI:
        case VOp::GTI:
        case VOp::GEI:
        case VOp::EQI:
        case VOp::NEI: return name + " " + r(instr.a) + ", " + r(instr.b) + ", " + std::to_string(instr.imm);
        case VOp::NEWARR: {
            const char* types[] = {"int", "float", "bool"};
            return name + " " + r(instr.a) + ", " + r(instr.b) + ", " + types[instr.imm];
        }
        case VOp::JMP: return name + " @" + std::to_string(instr.imm);
        case VOp::BRT:
        case VOp::BRF: return name + " " + r(instr.a) + ", @" + std::to_string(instr.imm);
        case VOp::ARG: return name + " " + r(instr.a);
        case VOp::CALL:
            return name + " " + (instr.a >= 0 ? r(instr.a) + ", " : "") +
                   program.functions[static_cast<size_t>(instr.imm)].name;
        case VOp::RET: return instr.a >= 0 ? name + " " + r(instr.a) : name;
        case VOp::GETARG: return name + " " + r(instr.a) + ", " + std::to_string(instr.imm);
        case VOp::LOADS:
        case VOp::STORES: return name + " " + r(instr.a) + ", slot " + std::to_string(instr.imm);
        case VOp::PRINT: return name + " " + r(instr.a);
        case VOp::PRINTS: return name + " " + json::quote(program.strings[static_cast<size_t>(instr.imm)]);
        case VOp::PRINTLN: return name;
        case VOp::PARFOR:
        case VOp::PAREND: return name + " loop " + std::to_string(instr.imm);
        default: return name + " " + r(instr.a) + ", " + r(instr.b) + ", " + r(instr.c);
    }
}

std::string to_text(const Program& program) {
    std::string out;
    for (const auto& fn : program.functions) {
        out += (out.empty() ? "" : "\n") + fn.name + ":  (" + std::to_string(fn.registers) + " registers, " +
               std::to_string(fn.slots) + " slots)\n";
        for (size_t pc = 0; pc < fn.code.size(); ++pc) {
            char prefix[32];
            std::snprintf(prefix, sizeof prefix, "%5zu  ", pc);
            std::string text = instr_text(program, fn.code[pc]);
            out += prefix + text;
            if (fn.code[pc].line > 0) {
                out.append(text.size() < 28 ? 28 - text.size() : 1, ' ');
                out += "; line " + std::to_string(fn.code[pc].line);
            }
            out += "\n";
        }
    }
    return out;
}

std::string to_json(const Program& program, std::string_view filename) {
    json::Writer w;
    w.begin_object();
    w.key("file");
    w.string(filename);
    w.key("registers");
    w.integer(program.registers);
    w.key("peephole_removed");
    w.integer(program.peephole_removed);
    w.key("functions");
    w.begin_array();
    for (const auto& fn : program.functions) {
        w.begin_object();
        w.key("name");
        w.string(fn.name);
        w.key("params");
        w.integer(fn.params);
        w.key("registers");
        w.integer(fn.registers);
        w.key("slots");
        w.integer(fn.slots);
        w.key("code");
        w.begin_array();
        for (size_t pc = 0; pc < fn.code.size(); ++pc) {
            w.begin_object();
            w.key("pc");
            w.integer(static_cast<long long>(pc));
            w.key("text");
            w.string(instr_text(program, fn.code[pc]));
            w.key("line");
            w.integer(fn.code[pc].line);
            w.end_object();
        }
        w.end_array();
        w.key("vars");
        w.begin_array();
        for (const auto& v : fn.vars) {
            w.begin_object();
            w.key("name");
            w.string(v.name);
            w.key("type");
            w.string(v.type);
            w.key("where");
            w.string(loc_text(v.loc));
            w.key("spilled");
            w.boolean(v.loc.slot);
            w.key("from");
            w.integer(v.from_pc);
            w.key("to");
            w.integer(v.to_pc);
            w.end_object();
        }
        w.end_array();
        w.key("loops");
        w.begin_array();
        for (const auto& loop : fn.loops) {
            w.begin_object();
            w.key("line");
            w.integer(loop.line);
            w.key("var");
            w.string(loc_text(loop.var));
            w.key("end");
            w.string(loc_text(loop.end));
            w.key("head");
            w.integer(loop.head_pc);
            w.key("parend");
            w.integer(loop.end_pc);
            w.key("reductions");
            w.begin_array();
            for (const auto& r : loop.reductions) w.string(std::string(1, r.op) + " " + loc_text(r.loc));
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

}  // namespace parc::vm
