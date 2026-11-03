#pragma once

#include <string>
#include <vector>

#include "common/diagnostic.hpp"
#include "ir/ir.hpp"

// Semantic checks that need the control-flow graph, so they run on the IR
// (before any optimization) using the data-flow framework:
//
//   reachability          code that can never run; a function that can end without returning
//   definite assignment   a variable read before it has a value on some path
//   liveness              a value that is assigned and never read (dead store)
//   call graph            recursion (Tarjan's strongly connected components), functions never called
//   value analysis        the range of every int and whether an array can be null:
//                         overflow, underflow, divide by zero, index out of bounds, null array
//
// The value analysis generalizes the four CWE checks of the earlier VulnHGNN
// project: CWE-190, CWE-191, CWE-369 and CWE-476.
namespace parc::sem {

// How many operations of one kind the value analysis looked at, and how many
// of them it could not prove safe.
struct OpCount {
    int checked = 0;   // operations of this kind in code that can run
    int at_risk = 0;   // with a warning: the compiler can say how it fails
    int possible = 0;  // with only a note: it could fail, but nothing in the code points at it
};

// The operations of one function that can fail when the program runs.
// "1 of 5 checked operations can fail" is what the vulnerability percentage means.
struct CheckedOps {
    std::string function;
    OpCount arithmetic;  // int + - * and unary minus written in the source: overflow, underflow
    OpCount division;    // int / and %: divide by zero
    OpCount access;      // a[i] and len(a): null array, index out of range
    OpCount allocation;  // new T[n]: negative size

    OpCount total() const {
        OpCount sum;
        for (const OpCount* part : {&arithmetic, &division, &access, &allocation}) {
            sum.checked += part->checked;
            sum.at_risk += part->at_risk;
            sum.possible += part->possible;
        }
        return sum;
    }
};

// 'checked', if given, receives one entry per function from the value analysis.
std::vector<Diagnostic> check_flow(const ir::Program& program, std::vector<CheckedOps>* checked = nullptr);

// The individual checks, for tests.
std::vector<Diagnostic> check_reachability(const ir::Program& program);
std::vector<Diagnostic> check_definite_assignment(const ir::Program& program);
std::vector<Diagnostic> check_dead_stores(const ir::Program& program);
std::vector<Diagnostic> check_call_graph(const ir::Program& program);
std::vector<Diagnostic> check_values(const ir::Program& program, std::vector<CheckedOps>* checked = nullptr);

}  // namespace parc::sem
