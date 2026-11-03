#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "parser/ast.hpp"

// ParC's intermediate representation: three-address code. Every instruction
// does one small step (t1 = a + b), and instructions are grouped into basic
// blocks that only branch at their end. Nothing here depends on the VM or on
// any real machine, so a different back end can start from the same IR.
namespace parc::ir {

using ast::Pos;

// What sort of value an operation works on. Ref is an array reference.
enum class VT { Int, Float, Bool, Ref };

struct Operand {
    enum class Kind { None, Var, Int, Float, Bool, Null, Str };
    Kind kind = Kind::None;
    int var = -1;     // Var: index into Function::vars
    long long i = 0;  // Int: the value. Str: index into Program::strings
    double f = 0.0;
    bool b = false;

    static Operand of_var(int id) {
        Operand o;
        o.kind = Kind::Var;
        o.var = id;
        return o;
    }
    static Operand of_int(long long value) {
        Operand o;
        o.kind = Kind::Int;
        o.i = value;
        return o;
    }
    static Operand of_float(double value) {
        Operand o;
        o.kind = Kind::Float;
        o.f = value;
        return o;
    }
    static Operand of_bool(bool value) {
        Operand o;
        o.kind = Kind::Bool;
        o.b = value;
        return o;
    }
    static Operand null() {
        Operand o;
        o.kind = Kind::Null;
        return o;
    }
    static Operand str(int index) {
        Operand o;
        o.kind = Kind::Str;
        o.i = index;
        return o;
    }

    bool is_none() const { return kind == Kind::None; }
    bool is_var() const { return kind == Kind::Var; }
    bool is_var(int id) const { return kind == Kind::Var && var == id; }
    bool is_int() const { return kind == Kind::Int; }
    bool is_const() const {
        return kind == Kind::Int || kind == Kind::Float || kind == Kind::Bool || kind == Kind::Null;
    }
    bool operator==(const Operand& other) const;
};

enum class Op {
    Copy,        // dst = a
    Add, Sub, Mul, Div, Mod,  // dst = a op b
    Neg,         // dst = -a
    Lt, Le, Gt, Ge, Eq, Ne,   // dst = a op b, giving a bool
    Not,         // dst = !a
    IntToFloat,  // dst = float(a)
    NewArray,    // dst = new T[a]
    Load,        // dst = a[b]
    Store,       // a[b] = c
    Len,         // dst = len(a)
    Call,        // dst = callee(args), or without dst
    Print,       // print args
    Jump,        // goto target
    Branch,      // if a goto target else target2
    Return,      // return a, or return
    ParFor,      // start of a parallel loop: each thread gets its own part of the range a..b
    ParEnd,      // end of a parallel loop: threads stop here and their results are combined
};

bool is_terminator(Op op);
bool is_compare(Op op);
bool is_arithmetic(Op op);         // Add, Sub, Mul, Div, Mod
std::string_view op_symbol(Op op);  // "+", "<", ...

struct Reduction {
    int var = -1;
    char op = '+';  // '+' or '*'
};

struct Instr {
    Op op = Op::Copy;
    Operand dst;  // a variable, or none
    Operand a, b, c;
    VT type = VT::Int;                  // what arithmetic and comparisons work on
    ast::BaseType elem = ast::BaseType::Int;  // NewArray: the element type
    std::vector<Operand> args;          // Call and Print
    std::string callee;                 // Call
    int target = -1;                    // Jump: the block. Branch: the block when true. ParFor: the loop header
    int target2 = -1;                   // Branch: the block when false. ParFor: the block with its ParEnd
    std::vector<Reduction> reductions;  // ParFor and ParEnd
    Pos pos;                            // where it comes from; line 0 when the compiler added it on its own
    Pos stmt;                           // the statement it belongs to
    const ast::Expr* expr = nullptr;    // the source expression, for messages
    bool decl_init = false;             // it sets a variable's initial value in a 'let'
};

enum class VarKind { Param, Local, Temp };

struct Var {
    std::string name;
    ast::Type type;
    VarKind kind = VarKind::Temp;
    Pos pos;
    bool declared_without_value = false;  // "let x: int;"
};

struct Block {
    std::string label;
    std::vector<Instr> instrs;  // the last one is a terminator
    std::vector<int> preds;
    std::vector<int> succs;
};

struct Function {
    std::string name;
    Pos pos;
    std::vector<int> params;  // indices into vars
    ast::Type return_type;
    std::vector<Var> vars;
    std::vector<Block> blocks;  // blocks[0] is the entry
    int max_nesting = 0;        // deepest if/while/for nesting in the source

    int add_var(std::string name, ast::Type type, VarKind kind, Pos pos = {});
    int instruction_count() const;
};

struct Program {
    std::vector<Function> functions;
    std::vector<std::string> strings;

    const Function* find(std::string_view name) const;
};

// Recomputes every block's preds and succs from the terminators.
void link(Function& fn);

// The variables an instruction reads and writes.
std::vector<int> uses(const Instr& instr);
std::vector<int> defs(const Instr& instr);

// The operands an optimization may replace with another value. A parallel
// loop's own operands are left out: they must stay variables.
std::vector<Operand*> use_operands(Instr& instr);

// Does it do something besides computing dst (write memory, print, call, jump)?
bool has_side_effect(const Instr& instr);
// Can it stop the program (divide by zero, null array, bad index, bad size)?
bool can_trap(const Instr& instr);
// A pure computation: the same operands always give the same result.
bool is_pure(const Instr& instr);

// Text forms, used by 'parc ir', by the pass log and by messages.
std::string operand_text(const Program& program, const Function& fn, const Operand& operand);
std::string instr_text(const Program& program, const Function& fn, const Instr& instr);
std::string signature(const Function& fn);
std::string to_text(const Program& program, const Function& fn);
std::string to_text(const Program& program);

// The ir.json file: {"file", "functions": [{"name", "vars", "blocks": [...]}]}
std::string to_json(const Program& program, std::string_view filename);

}  // namespace parc::ir
