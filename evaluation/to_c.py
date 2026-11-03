"""Translates a ParC program to C, for checking ParC against Clang.

Clang is only ever a reference here: the C text is made from the syntax tree
that ParC's own parser wrote (ast.json), compiled with Clang, and its output
compared with what ParC's VM prints. Nothing in the compiler uses this.

What a ParC construct becomes in C:

    int, float, bool      int, double, bool
    T[]                   a pointer to a struct {int n; T *d;}; null is NULL
    new T[n], len(a)      a call that allocates zeroed elements; a->n
    print(a, b)           printf of each argument by its type, spaces between
    for i in a..b         a for loop whose bounds are computed once
    parallel for          the same loop, run serially (the reference result)

    python evaluation/to_c.py examples/06_dot_parallel.parc > dot.c
"""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PARC = ROOT / "build" / "msvc" / "Release" / "parc.exe"

PRELUDE = r"""#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { int n; int *d; } arr_int;
typedef struct { int n; double *d; } arr_float;
typedef struct { int n; bool *d; } arr_bool;

static arr_int *new_int(int n) { arr_int *a = malloc(sizeof *a); a->n = n; a->d = calloc(n ? n : 1, sizeof(int)); return a; }
static arr_float *new_float(int n) { arr_float *a = malloc(sizeof *a); a->n = n; a->d = calloc(n ? n : 1, sizeof(double)); return a; }
static arr_bool *new_bool(int n) { arr_bool *a = malloc(sizeof *a); a->n = n; a->d = calloc(n ? n : 1, sizeof(bool)); return a; }
"""

C_TYPES = {"int": "int", "float": "double", "bool": "bool", "void": "void",
           "int[]": "arr_int *", "float[]": "arr_float *", "bool[]": "arr_bool *"}


class Translator:
    def __init__(self, ast, default_values=True):
        self.ast = ast
        # ParC gives "let x: int;" the value 0. For comparing output the C version does
        # the same; for comparing warnings it leaves the variable without a value, so
        # that Clang can see the same mistake ParC sees.
        self.default_values = default_values
        self.returns = {fn["name"]: fn["return_type"] for fn in ast["functions"]}
        self.scopes = []
        self.lines = []
        self.depth = 0
        self.counter = 0

    # ------------------------------------------------------------ types

    def lookup(self, name):
        for scope in reversed(self.scopes):
            if name in scope:
                return scope[name]
        return "int"

    def type_of(self, e):
        """The ParC type of an expression, by the same rules as ParC's type checker."""
        kind = e["node"]
        if kind == "Int":
            return "int"
        if kind == "Float":
            return "float"
        if kind == "Bool":
            return "bool"
        if kind == "String":
            return "string"
        if kind == "Null":
            return "null"
        if kind == "Name":
            return self.lookup(e["name"])
        if kind == "Unary":
            return "bool" if e["op"] == "!" else self.type_of(e["operand"])
        if kind == "Binary":
            if e["op"] in ("<", "<=", ">", ">=", "==", "!=", "&&", "||"):
                return "bool"
            return "float" if "float" in (self.type_of(e["lhs"]), self.type_of(e["rhs"])) else "int"
        if kind == "Index":
            return self.type_of(e["array"])[:-2]
        if kind == "Call":
            return "int" if e["callee"] == "len" else self.returns.get(e["callee"], "void")
        if kind == "New":
            return e["elem"] + "[]"
        return "int"

    # ------------------------------------------------------------ expressions

    def expr(self, e):
        kind = e["node"]
        if kind == "Int":
            return str(e["value"])
        if kind == "Float":
            return repr(float(e["value"]))
        if kind == "Bool":
            return "true" if e["value"] else "false"
        if kind == "String":
            return json.dumps(e["value"])
        if kind == "Null":
            return "NULL"
        if kind == "Name":
            return "v_" + e["name"]
        if kind == "Unary":
            return f"({e['op']}{self.expr(e['operand'])})"
        if kind == "Binary":
            return f"({self.expr(e['lhs'])} {e['op']} {self.expr(e['rhs'])})"
        if kind == "Index":
            return f"{self.expr(e['array'])}->d[{self.expr(e['index'])}]"
        if kind == "Call":
            if e["callee"] == "len":
                return f"{self.expr(e['args'][0])}->n"
            return f"f_{e['callee']}({', '.join(self.expr(a) for a in e['args'])})"
        if kind == "New":
            return f"new_{e['elem']}({self.expr(e['size'])})"
        raise ValueError(f"unknown expression {kind}")

    # ------------------------------------------------------------ statements

    def emit(self, text):
        self.lines.append("    " * self.depth + text)

    def block(self, block, scope=None):
        self.scopes.append(scope or {})
        self.depth += 1
        for stmt in block["stmts"]:
            self.stmt(stmt)
        self.depth -= 1
        self.scopes.pop()

    def print_call(self, args):
        if not args:
            self.emit('printf("\\n");')
            return
        formats, values = [], []
        for arg in args:
            t = self.type_of(arg)
            if t == "string":
                formats.append("%s")
                values.append(self.expr(arg))
            elif t == "float":
                formats.append("%g")
                values.append(self.expr(arg))
            elif t == "bool":
                formats.append("%s")
                values.append(f'({self.expr(arg)} ? "true" : "false")')
            else:
                formats.append("%d")
                values.append(self.expr(arg))
        # One printf per argument, so arguments are evaluated in ParC's order (left to right).
        for i, (fmt, value) in enumerate(zip(formats, values)):
            end = "\\n" if i + 1 == len(args) else " "
            self.emit(f'printf("{fmt}{end}", {value});')

    def stmt(self, s):
        kind = s["node"]
        if kind == "Block":
            self.emit("{")
            self.block(s)
            self.emit("}")
        elif kind == "Let":
            self.scopes[-1][s["name"]] = s["type"]
            if s["init"] or self.default_values:
                init = self.expr(s["init"]) if s["init"] else ("NULL" if s["type"].endswith("[]") else "0")
                self.emit(f"{C_TYPES[s['type']]} v_{s['name']} = {init};")
            else:
                self.emit(f"{C_TYPES[s['type']]} v_{s['name']};")
        elif kind == "Assign":
            self.emit(f"{self.expr(s['target'])} = {self.expr(s['value'])};")
        elif kind == "ExprStmt":
            e = s["expr"]
            if e["node"] == "Call" and e["callee"] == "print":
                self.print_call(e["args"])
            else:
                self.emit(f"(void)({self.expr(e)});")
        elif kind == "If":
            self.emit(f"if ({self.expr(s['cond'])}) {{")
            self.block(s["then"])
            branch = s["else"]
            while branch is not None and branch["node"] == "If":
                self.emit(f"}} else if ({self.expr(branch['cond'])}) {{")
                self.block(branch["then"])
                branch = branch["else"]
            if branch is not None:
                self.emit("} else {")
                self.block(branch)
            self.emit("}")
        elif kind == "While":
            self.emit(f"while ({self.expr(s['cond'])}) {{")
            self.block(s["body"])
            self.emit("}")
        elif kind in ("For", "ParallelFor"):
            self.counter += 1
            end = f"end_{self.counter}"
            # The range is computed once, before the loop, as in ParC.
            self.emit("{")
            self.depth += 1
            self.emit(f"int v_{s['var']} = {self.expr(s['start'])};")
            self.emit(f"int {end} = {self.expr(s['end'])};")
            self.emit(f"for (; v_{s['var']} < {end}; v_{s['var']}++) {{")
            self.block(s["body"], {s["var"]: "int"})
            self.emit("}")
            self.depth -= 1
            self.emit("}")
        elif kind == "Return":
            self.emit(f"return {self.expr(s['value'])};" if s["value"] else "return;")
        elif kind == "Break":
            self.emit("break;")
        elif kind == "Continue":
            self.emit("continue;")
        else:
            raise ValueError(f"unknown statement {kind}")

    def program(self):
        out = [PRELUDE]
        signatures = []
        for fn in self.ast["functions"]:
            params = ", ".join(f"{C_TYPES[p['type']]} v_{p['name']}" for p in fn["params"]) or "void"
            signatures.append(f"static {C_TYPES[fn['return_type']]} f_{fn['name']}({params})")
        out += [s + ";" for s in signatures] + [""]
        for fn, signature in zip(self.ast["functions"], signatures):
            self.lines = [signature + " {"]
            self.depth = 0
            self.block(fn["body"], {p["name"]: p["type"] for p in fn["params"]})
            self.lines.append("}")
            out += self.lines + [""]
        main_type = self.returns.get("main", "void")
        out.append("int main(void) { " + ("return f_main(); }" if main_type == "int" else "f_main(); return 0; }"))
        return "\n".join(out) + "\n"


def translate_source(source, default_values=True):
    """ParC source text -> C source text. Raises ValueError if the program has syntax errors."""
    with tempfile.TemporaryDirectory(prefix="parc_to_c_") as folder:
        path = Path(folder) / "program.parc"
        path.write_text(source, encoding="utf-8")
        out = Path(folder) / "ast.json"
        subprocess.run([str(PARC), "ast", str(path), "--json", str(out)], capture_output=True, check=False)
        ast = json.loads(out.read_text(encoding="utf-8"))
    if ast["errors"]:
        raise ValueError("the program has syntax errors")
    return Translator(ast, default_values).program()


if __name__ == "__main__":
    sys.stdout.write(translate_source(Path(sys.argv[1]).read_text(encoding="utf-8")))
