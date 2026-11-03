#pragma once

#include <string>
#include <vector>

// The hooks into Clang and LLVM. Nothing here is written from scratch: the
// syntax tree and the warnings come from libclang (Clang's C interface, the
// same parser clang.exe uses), the optimization remarks from LLVM's remarks
// library reading what clang -O2 -fsave-optimization-record wrote, and the
// native program from clang.exe itself.
//
// The rest of the metrics module only sees the plain structures below, so it
// does not depend on libclang's API. When ParC was built without Clang
// (PARC_HAVE_LIBCLANG not defined), every function here says so in `error`.
namespace parc::metrics {

// One node of Clang's syntax tree, copied out of libclang.
struct CNode {
    std::string kind;      // Clang's cursor kind: "ForStmt", "BinaryOperator", "DeclRefExpr", "CallExpr", ...
    std::string name;      // the cursor's spelling: a variable, function or member name
    std::string op;        // operators: "+", "<", "+=", "++"
    std::string type;      // declarations: the C type, e.g. "int" or "arr_int *"
    long long value = 0;   // IntegerLiteral
    int line = 0;          // the presumed line: the ParC line, thanks to the #line directives
    unsigned decl = 0;     // VarDecl and ParmDecl: an id; DeclRefExpr: the id of the declaration it names
    bool param = false;    // DeclRefExpr naming a parameter
    bool function = false; // DeclRefExpr naming a function
    std::string text;      // the source text of the node, for messages
    std::vector<CNode> children;  // CallExpr: the arguments only
};

struct CFunction {
    std::string name;            // without the f_ that ParC's translation adds
    int line = 0;
    std::vector<CNode> params;   // ParmDecl nodes
    CNode body;                  // CompoundStmt
};

struct ClangDiagnostic {
    std::string severity;  // "warning" or "error"
    int line = 0;
    int col = 0;
    std::string message;
    std::string option;    // the flag that controls it, e.g. "-Wunused-variable"
};

struct CUnit {
    bool ok = false;
    std::string error;
    bool parc = false;     // translated from ParC: names carry v_ and f_, len(a) is a->n
    std::string version;   // libclang's version text
    double parse_ms = 0.0;
    std::vector<CFunction> functions;
    std::vector<ClangDiagnostic> diagnostics;  // only those in the program's own file
};

// Parses C text with libclang. `display_file` is the name the #line
// directives use (the .parc file) or, for a C file, its own name.
CUnit parse_c(const std::string& c_text, const std::string& display_file, bool from_parc);

// What Clang's optimizer reported about one place in the program.
struct Remark {
    std::string type;      // "passed", "missed" or "analysis"
    std::string pass;      // "licm", "loop-vectorize", "inline", ...
    std::string name;      // the remark's name, e.g. "Vectorized"
    std::string function;
    int line = 0;
    std::string message;
};

struct NativeBuild {
    bool ok = false;
    std::string error;
    std::string clang;          // the clang.exe that was used
    std::string exe;            // the program it wrote
    double compile_ms = 0.0;    // clang -O2, including the remarks file and linking
    std::vector<Remark> remarks;
};

// Compiles the C text with clang -O2 into `folder`, saving the optimization
// remarks, and reads them back with LLVM's remarks parser.
NativeBuild build_native(const std::string& c_text, const std::string& display_file, const std::string& folder);

// Where clang.exe is: PARC_CLANG if set, else the one found when ParC was built, else "clang".
std::string clang_path();

// Shows translated names the ParC way: v_x -> x, f_g -> g, a->n -> len(a), a->d[i] -> a[i].
std::string parc_names(const std::string& text);

}  // namespace parc::metrics
