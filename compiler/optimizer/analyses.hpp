#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "common/util.hpp"
#include "ir/ir.hpp"
#include "optimizer/dataflow.hpp"

// The classic bit-vector analyses, each a few lines on top of the solver in
// dataflow.hpp. A set is a BitSet with one bit per variable, definition or
// expression.
namespace parc::opt {

// ---------------------------------------------------------------- live variables
// Backward, meet = union. A variable is live at a point when some path from
// there reads it before writing it. in/out hold one bit per variable.
Solution<BitSet> liveness(const ir::Function& fn);

// ---------------------------------------------------------------- reaching definitions
// Forward, meet = union. A definition (one assignment to one variable)
// reaches a point when some path gets there without the variable being
// assigned again.
struct Definition {
    int block = -1;  // -1 for a parameter's value on entry
    int index = -1;  // position in the block
    int var = -1;
};

struct ReachingDefs {
    std::vector<Definition> defs;
    std::vector<std::vector<int>> of_var;  // for each variable, the numbers of its definitions
    Solution<BitSet> solution;             // one bit per definition

    // The definitions of `var` that can reach instruction `index` of `block`.
    std::vector<int> reaching(const ir::Function& fn, int block, int index, int var) const;
};

ReachingDefs reaching_definitions(const ir::Function& fn);

// ---------------------------------------------------------------- available expressions
// Forward, meet = intersection. An expression is available at a point when
// every path there has computed it and none of its operands changed since.
struct Expression {
    ir::Op op = ir::Op::Add;
    ir::VT type = ir::VT::Int;
    ir::Operand a, b;
    bool operator==(const Expression&) const = default;
};

// The expression an instruction computes, with the operands of + * == != in
// one fixed order so that a + b and b + a are the same expression.
// Returns false for instructions that are not pure computations.
bool expression_of(const ir::Instr& instr, Expression& out);

struct AvailableExprs {
    std::vector<Expression> exprs;
    Solution<BitSet> solution;  // one bit per expression

    int find(const Expression& e) const;  // its number, or -1
};

AvailableExprs available_expressions(const ir::Function& fn);

// Applies one instruction to a set of available expressions (used by the
// analysis and again by the pass that walks through a block).
void step_available(const AvailableExprs& info, const ir::Instr& instr, BitSet& available);

std::string expression_text(const ir::Program& program, const ir::Function& fn, const Expression& e);

// The dataflow.json file: IN and OUT sets of the three analyses for every
// block, so hand-worked answers can be checked against it.
std::string dataflow_json(const ir::Program& program, std::string_view filename);

}  // namespace parc::opt
