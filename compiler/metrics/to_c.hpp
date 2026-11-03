#pragma once

#include <string>
#include <string_view>

#include "parser/ast.hpp"

// Writes a ParC program as C, so that Clang can read it. This is how the
// metrics module hands a ParC program to Clang: libclang parses the C text
// for the complexity analysis, and clang.exe compiles it for its optimization
// remarks and for a native run to compare with the VM.
//
// Every statement is preceded by a "#line N \"file.parc\"" directive, so
// everything Clang reports about the C text (syntax tree positions, warnings,
// remarks) carries the ParC line it came from.
//
// What a ParC construct becomes in C (the same rules as evaluation/to_c.py):
//
//     int, float, bool      int, double, bool
//     T[]                   a pointer to a struct {int n; T *d;}; null is NULL
//     new T[n], len(a)      new_T(n), which allocates zeroed elements; a->n
//     print(a, b)           one printf per argument, spaces between
//     for i in a..b         for (int v_i = a, end_K = b; v_i < end_K; v_i++)
//     parallel for          the same loop, run serially
//     names                 v_name for variables, f_name for functions
//
// The C program needs no header files. Its main runs ParC's main as many
// times as its first argument says (default once), so a native run can be
// timed without the cost of starting the process.
namespace parc::metrics {

std::string to_c(const ast::Program& program, std::string_view parc_file);

}  // namespace parc::metrics
