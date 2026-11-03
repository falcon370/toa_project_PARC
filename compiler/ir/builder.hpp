#pragma once

#include <unordered_set>

#include "ir/ir.hpp"
#include "parser/ast.hpp"
#include "semantic/semantic.hpp"

namespace parc::ir {

// Lowers a type-checked program to three-address code and a control-flow graph.
//
// `types` is the semantic phase's result for the same tree, and the program
// must have no errors. `parallel` lists the 'parallel for' loops that were
// proved safe to run on several threads; any other loop becomes a normal loop.
Program build(const ast::Program& program, const sem::Result& types,
              const std::unordered_set<const ast::For*>& parallel);

}  // namespace parc::ir
