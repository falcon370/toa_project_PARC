#pragma once

#include <unordered_set>
#include <vector>

#include "common/diagnostic.hpp"
#include "parser/ast.hpp"
#include "semantic/semantic.hpp"

// Loop dependence analysis: can the iterations of a 'for' loop run at the
// same time, in any order, and still give the serial result?
//
// They can when no iteration reads or writes something another iteration
// writes. The analysis looks at two kinds of things:
//
//   scalars  A variable declared outside the loop and written inside it is
//            shared by all iterations. That is only fine for a reduction:
//            "sum = sum + ..." with nothing else touching sum.
//   arrays   For a write a[f(i)] and another access a[g(i)], is there a pair
//            of different iterations that touch the same element? With
//            subscripts of the form c*i + k this is decided by:
//              ZIV test         both subscripts are constants: same element or not
//              strong SIV test  equal coefficients: the distance is (k2 - k1) / c
//              GCD test         c1*i1 - c2*i2 = k2 - k1 has an integer solution
//                               only if gcd(c1, c2) divides k2 - k1
//
// A 'parallel for' that passes runs on threads. One that fails is reported
// with the dependence that makes it unsafe and runs as a normal loop. Normal
// 'for' loops get a note saying whether they could be parallel.
namespace parc::opt {

struct LoopReport {
    std::unordered_set<const ast::For*> parallel;  // the 'parallel for' loops that are safe
    std::vector<Diagnostic> findings;
};

// `force_parallel` keeps unsafe 'parallel for' loops parallel anyway, to
// demonstrate a data race on the VM.
LoopReport analyze_loops(const ast::Program& program, const sem::Result& types, bool force_parallel = false);

}  // namespace parc::opt
