#pragma once

#include <string>
#include <string_view>
#include <vector>

// The instruction set of the ParC register VM.
//
// Every function call gets its own frame with a fixed number of registers
// (r0, r1, ...) and a few numbered slots for values that did not fit in a
// register. Instructions take registers as operands; a few take a small
// constant instead of their last register (ADDI r1, r0, 5).
//
// A value is an int (32 bits, wraps on overflow), a float (64 bits), a bool,
// or a reference to an array (or null).
namespace parc::vm {

// X(name, operands shown in the listing)
#define PARC_VM_OPS(X)                                                              \
    X(LI, "rd, int")          /* rd = constant int */                               \
    X(LF, "rd, float")        /* rd = constant float */                             \
    X(LB, "rd, bool")         /* rd = true or false */                              \
    X(LNULL, "rd")            /* rd = null */                                       \
    X(MOV, "rd, rs")          /* rd = rs */                                         \
    X(ADD, "rd, ra, rb")      /* int arithmetic */                                  \
    X(SUB, "rd, ra, rb")                                                            \
    X(MUL, "rd, ra, rb")                                                            \
    X(DIV, "rd, ra, rb")      /* stops the program when rb is 0 */                  \
    X(MOD, "rd, ra, rb")                                                            \
    X(NEG, "rd, ra")                                                                \
    X(ADDI, "rd, ra, int")    /* int arithmetic with a constant */                  \
    X(SUBI, "rd, ra, int")                                                          \
    X(MULI, "rd, ra, int")                                                          \
    X(FADD, "rd, ra, rb")     /* float arithmetic */                                \
    X(FSUB, "rd, ra, rb")                                                           \
    X(FMUL, "rd, ra, rb")                                                           \
    X(FDIV, "rd, ra, rb")                                                           \
    X(FNEG, "rd, ra")                                                               \
    X(LT, "rd, ra, rb")       /* int comparisons, giving a bool */                  \
    X(LE, "rd, ra, rb")                                                             \
    X(GT, "rd, ra, rb")                                                             \
    X(GE, "rd, ra, rb")                                                             \
    X(LTI, "rd, ra, int")     /* int comparisons with a constant */                 \
    X(LEI, "rd, ra, int")                                                           \
    X(GTI, "rd, ra, int")                                                           \
    X(GEI, "rd, ra, int")                                                           \
    X(EQI, "rd, ra, int")                                                           \
    X(NEI, "rd, ra, int")                                                           \
    X(FLT, "rd, ra, rb")      /* float comparisons */                               \
    X(FLE, "rd, ra, rb")                                                            \
    X(FGT, "rd, ra, rb")                                                            \
    X(FGE, "rd, ra, rb")                                                            \
    X(EQ, "rd, ra, rb")       /* equality of any two values of the same type */     \
    X(NE, "rd, ra, rb")                                                             \
    X(NOT, "rd, ra")                                                                \
    X(I2F, "rd, ra")          /* int to float */                                    \
    X(NEWARR, "rd, rn, type") /* rd = new array of rn elements */                   \
    X(ALOAD, "rd, ra, ri")    /* rd = ra[ri] */                                     \
    X(ASTORE, "ra, ri, rv")   /* ra[ri] = rv */                                     \
    X(LEN, "rd, ra")                                                                \
    X(JMP, "@pc")                                                                   \
    X(BRT, "rc, @pc")         /* jump when rc is true */                            \
    X(BRF, "rc, @pc")         /* jump when rc is false */                           \
    X(ARG, "rs")              /* adds rs to the arguments of the next call */       \
    X(CALL, "rd, function")   /* calls, then puts the result in rd */               \
    X(RET, "rs")              /* returns rs, or nothing */                          \
    X(GETARG, "rd, n")        /* rd = this call's argument number n */              \
    X(LOADS, "rd, slot")      /* rd = a value kept in the frame (spilled) */        \
    X(STORES, "rs, slot")     /* keeps rs in the frame */                           \
    X(PRINT, "rs")            /* prints a value */                                  \
    X(PRINTS, "string")       /* prints a string constant */                        \
    X(PRINTLN, "")            /* ends the line */                                   \
    X(PARFOR, "loop")         /* starts the threads of a parallel loop */           \
    X(PAREND, "loop")         /* a thread's end of a parallel loop */

enum class VOp {
#define PARC_VM_ENUM(name, operands) name,
    PARC_VM_OPS(PARC_VM_ENUM)
#undef PARC_VM_ENUM
};

std::string_view op_name(VOp op);
std::string_view op_operands(VOp op);  // the pattern shown in the instruction table

struct Instr {
    VOp op = VOp::LI;
    int a = -1;         // usually the destination register
    int b = -1;
    int c = -1;         // PRINT and PRINTS: 1 = end the line after it, 0 = a space after it
    long long imm = 0;  // constant, slot, pc, function number, string number or loop number
    double fimm = 0.0;
    int line = 0;       // source line, for errors and the debugger
};

// A place in a frame: a register or a slot.
struct Loc {
    bool slot = false;
    int index = -1;
    bool valid() const { return index >= 0; }
};

struct Reduction {
    Loc loc;
    char op = '+';
    bool is_float = false;
};

// What PARFOR and PAREND need to know about one parallel loop.
struct ParLoop {
    Loc var;  // the loop variable
    Loc end;  // the end of the range
    std::vector<Reduction> reductions;
    int head_pc = 0;  // where each pass through the loop starts
    int end_pc = 0;   // the PAREND
    int line = 0;
};

// Where a source variable lives, for the listing and the debugger.
struct VarInfo {
    std::string name;
    std::string type;
    Loc loc;
    int from_pc = 0;  // the first and last instruction where it holds a value
    int to_pc = 0;
};

struct Function {
    std::string name;
    int params = 0;
    int registers = 0;
    int slots = 0;
    std::vector<Instr> code;
    std::vector<ParLoop> loops;
    std::vector<VarInfo> vars;
};

struct Program {
    std::vector<Function> functions;
    std::vector<std::string> strings;
    int registers = 12;        // registers per frame
    int peephole_removed = 0;  // instructions the peephole step deleted

    int find(std::string_view name) const;
};

std::string loc_text(Loc loc);                                             // "r3" or "slot 1"
std::string instr_text(const Program& program, const Instr& instr);        // "ADD r2, r0, r1"
std::string to_text(const Program& program);                               // the whole listing

// The code.json file.
std::string to_json(const Program& program, std::string_view filename);

}  // namespace parc::vm
