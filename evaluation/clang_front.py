"""The front end against Clang: do the scanner and the parser agree with a real compiler?

    python evaluation/clang_front.py [--quick]

ParC is not C, so Clang cannot compile a ParC file. But two parts of Clang can
still serve as a reference for ParC's front end:

  1. Tokens.  Clang's lexer does not parse. Run on a ParC file it simply cuts the
     text into tokens, and almost every ParC token is spelled like a C token. So
     both scanners must cut the same text at the same places: same spelling,
     same line and column, same class (word, number, string, punctuation).
     One known difference: C has no '..' operator. It reads "0..n" as one
     (invalid) number and "lo..hi" as two dots. Clang's tokens are re-cut by
     that one rule before comparing, and the report says how often it was used.

  2. Expression trees.  An expression such as  a - b * p[c] < g(d, 2)  is written
     the same way in ParC and in C, and both languages give its operators the
     same precedence and associativity. So both parsers must build the same tree.
     Both trees are printed with every operation in parentheses and compared.

What is not compared: declarations and statements. 'fn f(a: int) -> int' and
'for i in 0..n' have no C spelling, so a comparison would go through a
translation and test the translator, not the parser.
"""

import json
import random
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "explorer"))

import compiler_bridge as bridge  # noqa: E402
import fuzz  # noqa: E402

PARC = ROOT / "build" / "msvc" / "Release" / "parc.exe"
CLANG_PLACES = [Path(r"C:\Program Files\LLVM\bin\clang.exe")]


def find_clang():
    import shutil
    for place in CLANG_PLACES:
        if place.is_file():
            return str(place)
    return shutil.which("clang")


CLANG = find_clang()

# ---------------------------------------------------------------- 1. tokens

TOKEN_LINE = re.compile(r"^(\w+)\s+'(.*)'\s+Loc=<.*:(\d+):(\d+)>")


def token_class(text, clang_kind=""):
    """word, number, string or punctuation: what both scanners must agree on."""
    if clang_kind == "string_literal" or text.startswith('"'):
        return "string"
    if text[:1].isdigit():
        return "number"
    if text[:1].isalpha() or text[:1] == "_":
        return "word"
    return "punctuation"


def clang_tokens(path):
    """Clang's tokens of a file as [(text, line, col, class)], and how often the '..' rule was needed."""
    done = subprocess.run([CLANG, "-x", "c", "-fsyntax-only", "-Xclang", "-dump-tokens", str(path)],
                          capture_output=True, text=True, encoding="utf-8", errors="replace")
    raw = []
    for line in done.stderr.splitlines():
        match = TOKEN_LINE.match(line)
        if match and match.group(1) != "eof":
            # The dump prints a quote as \" and a backslash as \\; undo that to get the spelling in the file.
            spelling = re.sub(r"\\(.)", r"\1", match.group(2))
            raw.append((match.group(1), spelling, int(match.group(3)), int(match.group(4))))

    # C has no '..'. Re-cut Clang's tokens where ParC has one:
    #   "0..n" or "0.."     one number token that contains two dots
    #   "." "."             two dots next to each other
    #   "." ".5"            a dot, then a number that starts with a dot
    out, recut, i = [], 0, 0
    while i < len(raw):
        kind, text, line, col = raw[i]
        follows = raw[i + 1] if i + 1 < len(raw) else None
        next_touches = follows is not None and follows[2] == line and follows[3] == col + 1
        if kind == "numeric_constant" and ".." in text:
            left, right = text.split("..", 1)
            out.append((left, line, col, "number"))
            out.append(("..", line, col + len(left), "punctuation"))
            if right:
                out.append((right, line, col + len(left) + 2, token_class(right)))
            recut += 1
        elif kind == "period" and next_touches and follows[0] == "period":
            out.append(("..", line, col, "punctuation"))
            recut += 1
            i += 1
        elif kind == "period" and next_touches and follows[0] == "numeric_constant" and follows[1].startswith("."):
            out.append(("..", line, col, "punctuation"))
            out.append((follows[1][1:], line, col + 2, "number"))
            recut += 1
            i += 1
        else:
            out.append((text, line, col, token_class(text, kind)))
        i += 1
    return out, recut


def parc_tokens(source):
    """ParC's tokens as [(text, line, col, class)], or None if the scanner reported an error."""
    tokens = bridge.build(source)["tokens"]
    if tokens["errors"]:
        return None
    return [(t["text"], t["line"], t["col"], "number" if t["kind"] in ("INT", "FLOAT") else token_class(t["text"]))
            for t in tokens["tokens"] if t["kind"] != "EOF"]


def compare_tokens(source, folder):
    """Returns (number of tokens, uses of the '..' rule, first difference or '')."""
    ours = parc_tokens(source)
    if ours is None:
        return 0, 0, "ParC's scanner reported an error"
    path = Path(folder) / "program.parc"
    path.write_bytes(source.replace("\r\n", "\n").encode("utf-8"))
    theirs, recut = clang_tokens(path)
    for mine, other in zip(ours, theirs):
        if mine != other:
            return len(ours), recut, f"ParC has {mine[0]!r} ({mine[3]}) at {mine[1]}:{mine[2]}, Clang has {other[0]!r} ({other[3]}) at {other[1]}:{other[2]}"
    if len(ours) != len(theirs):
        return len(ours), recut, f"ParC has {len(ours)} tokens, Clang has {len(theirs)}"
    return len(ours), recut, ""


# Random text that is made of valid tokens but is not a program: it tests the
# scanner alone, with odd spacing, glued tokens, comments and every kind of literal.
WORDS = ["fn", "let", "if", "else", "while", "for", "in", "parallel", "reduce", "return", "break", "continue", "true",
         "false", "null", "new", "int", "float", "bool", "print", "len", "x", "y1", "_tmp", "sum", "lettuce", "format",
         "i", "int_value", "returned"]
NUMBERS = ["0", "7", "42", "2147483647", "100000", "1.5", "0.25", "2.0e-3", "6.02E23", "3e8", "10.0"]
STRINGS = ['"a"', '"hello world"', r'"tab\there"', r'"quote \" inside"', r'"back\\slash"', '"// not a comment"',
           '"/* nor this */"', '""']
OPERATORS = ["+", "-", "*", "/", "%", "=", "<", ">", "!", "==", "!=", "<=", ">=", "&&", "||", "->", ":"]
BRACKETS = ["(", ")", "{", "}", "[", "]", ",", ";"]
RANGES = ["0..n", "1..10", "lo..hi", "lo..10", "0 .. n", "i..len"]
COMMENTS = ["// a line comment\n", "/* a block comment */", "/* over\n   two lines */", "// let x = 1.5e3;\n"]


def token_soup(rng, units=70):
    """One random text, as a string."""
    out, previous = [], "space"
    for _ in range(units):
        kind = rng.choice(["word"] * 4 + ["number"] * 3 + ["string", "operator", "operator", "bracket", "bracket",
                                                          "range", "comment"])
        text = rng.choice({"word": WORDS, "number": NUMBERS, "string": STRINGS, "operator": OPERATORS,
                           "bracket": BRACKETS, "range": RANGES, "comment": COMMENTS}[kind])
        # Two tokens may touch only where both languages read them the same way.
        # Never two operators (C has ++, --, +=, <<, <: and more), never two
        # words or numbers, and nothing glued to a range, a comment or a string.
        simple = {"word", "number"}
        may_touch = ((previous in simple and kind in ("operator", "bracket")) or
                     (previous in ("operator", "bracket") and kind in simple) or
                     (previous == "bracket" and kind == "bracket"))
        out.append("" if may_touch and rng.random() < 0.5 else rng.choice([" ", " ", "  ", "\t", "\n", "\n    "]))
        out.append(text)
        previous = "space" if text.endswith("\n") else kind
    return "".join(out) + "\n"


# ---------------------------------------------------------------- 2. expression trees

BINARY = ["||", "&&", "==", "!=", "<", "<=", ">", ">=", "+", "-", "*", "/", "%"]


def random_expression(rng, depth=0):
    """A random expression that reads the same in ParC and in C."""
    roll = rng.random()
    if depth >= 4 or roll < 0.22:
        return rng.choice(["a", "b", "c", "d", "1", "2", "7", "10"])
    if roll < 0.30:
        return f"p[{random_expression(rng, depth + 1)}]"
    if roll < 0.37:
        return f"g({random_expression(rng, depth + 1)}, {random_expression(rng, depth + 1)})"
    if roll < 0.47:
        operand = random_expression(rng, depth + 1)
        op = rng.choice(["-", "!"])
        # Anything but a plain name or number gets parentheses: "--a" would be C's
        # decrement, and "-a + b" must stay "(-a) + b" in both languages anyway.
        return op + (operand if re.fullmatch(r"\w+", operand) else f"({operand})")
    left, right = random_expression(rng, depth + 1), random_expression(rng, depth + 1)
    text = f"{left} {rng.choice(BINARY)} {right}"
    return f"({text})" if rng.random() < 0.25 else text


def parc_shape(node):
    """ParC's tree with every operation in parentheses."""
    kind = node["node"]
    if kind == "Binary":
        return f"({parc_shape(node['lhs'])} {node['op']} {parc_shape(node['rhs'])})"
    if kind == "Unary":
        return f"({node['op']}{parc_shape(node['operand'])})"
    if kind == "Index":
        return f"{parc_shape(node['array'])}[{parc_shape(node['index'])}]"
    if kind == "Call":
        return f"{node['callee']}({', '.join(parc_shape(a) for a in node['args'])})"
    return str(node["name"] if kind == "Name" else node["value"])


def clang_shape(node):
    """Clang's tree, printed the same way. Casts and written parentheses are looked through."""
    kind = node["kind"]
    inner = node.get("inner", [])
    if kind in ("ImplicitCastExpr", "ParenExpr"):
        return clang_shape(inner[0])
    if kind == "BinaryOperator":
        return f"({clang_shape(inner[0])} {node['opcode']} {clang_shape(inner[1])})"
    if kind == "UnaryOperator":
        return f"({node['opcode']}{clang_shape(inner[0])})"
    if kind == "ArraySubscriptExpr":
        return f"{clang_shape(inner[0])}[{clang_shape(inner[1])}]"
    if kind == "CallExpr":
        return f"{clang_shape(inner[0])}({', '.join(clang_shape(a) for a in inner[1:])})"
    if kind == "DeclRefExpr":
        return node["referencedDecl"]["name"]
    if kind == "IntegerLiteral":
        return node["value"]
    raise ValueError(f"a Clang node this script does not know: {kind}")


def find(node, kind):
    if node.get("kind") == kind:
        return node
    for child in node.get("inner", []):
        found = find(child, kind)
        if found:
            return found
    return None


def compare_expressions(expressions, folder):
    """Returns [(expression, ParC's tree, Clang's tree)] for the ones that differ."""
    parc_source = "".join(f"fn f{i}(a: int, b: int, c: int, d: int, p: int[]) -> int {{\n    return {e};\n}}\n"
                          for i, e in enumerate(expressions))
    c_source = "int g(int x, int y);\n" + "".join(
        f"int f{i}(int a, int b, int c, int d, int* p) {{\n    return {e};\n}}\n" for i, e in enumerate(expressions))

    # Only the scanner and the parser run here ('parc ast'), so an expression
    # that mixes int and bool is fine: its shape is all that is compared.
    parc_file, ast_file, c_file = Path(folder) / "e.parc", Path(folder) / "e.json", Path(folder) / "e.c"
    parc_file.write_text(parc_source, encoding="utf-8")
    c_file.write_text(c_source, encoding="utf-8")
    subprocess.run([str(PARC), "ast", str(parc_file), "--json", str(ast_file)], capture_output=True, text=True)
    ours = json.loads(ast_file.read_text(encoding="utf-8"))
    if ours.get("errors"):
        raise ValueError(f"ParC could not parse a generated expression: {ours['errors'][0]}")
    done = subprocess.run([CLANG, "-x", "c", "-w", "-fsyntax-only", "-Xclang", "-ast-dump=json", str(c_file)],
                          capture_output=True, text=True, encoding="utf-8", errors="replace")
    theirs = {n["name"]: n for n in json.loads(done.stdout)["inner"] if n.get("kind") == "FunctionDecl"}

    different = []
    for i, expression in enumerate(expressions):
        mine = parc_shape(ours["functions"][i]["body"]["stmts"][0]["value"])
        other = clang_shape(find(theirs[f"f{i}"], "ReturnStmt")["inner"][0])
        if mine != other:
            different.append((expression, mine, other))
    return different


# ---------------------------------------------------------------- report

def section(quick=False):
    """Section 6 of evaluation/results.md, and a one-line summary."""
    out = ["## 6. The front end against Clang: tokens and expression trees", ""]
    if not CLANG:
        return out + ["Skipped: Clang was not found.", ""], "skipped"
    version = subprocess.run([CLANG, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    rng = random.Random(31)
    examples = [(p.name, p.read_text(encoding="utf-8").replace("\r\n", "\n")) for p in sorted((ROOT / "examples").glob("*.parc"))]
    programs = [(f"random program {n}", fuzz.Generator(rng).program()) for n in range(30 if quick else 100)]
    soups = [(f"random token text {n}", token_soup(rng)) for n in range(60 if quick else 200)]

    out += [f"ParC is not C, so Clang (`{version}`) cannot compile a ParC file. Two parts of it can still check ParC's "
            "front end.", "",
            "### Tokens: ParC's scanner and Clang's lexer on the same text", "",
            "Clang's lexer does not parse, so it can cut a ParC file into tokens (`clang -x c -fsyntax-only -Xclang "
            "-dump-tokens FILE.parc`). Every token must have the same spelling, the same line and column, and the same "
            "class (word, number, string, punctuation) in both. One difference is known and allowed for: C has no `..` "
            "operator, so it reads `0..n` as one number and `lo..hi` as two dots. Clang's tokens are re-cut by that one "
            "rule before comparing.", "",
            "| Text | Files | Tokens compared | Files that agree | Times the `..` rule was needed |", "|---|---|---|---|---|"]
    problems = []
    with tempfile.TemporaryDirectory(prefix="parc_front_") as folder:
        totals = {}
        for label, group in (("The examples", examples), ("Random programs", programs),
                             ("Random token text (valid tokens, not a program)", soups)):
            tokens = agree = recuts = 0
            for name, source in group:
                count, recut, difference = compare_tokens(source, folder)
                tokens += count
                recuts += recut
                agree += not difference
                if difference:
                    problems.append(f"- **{name}**: {difference}")
            totals[label] = (len(group), tokens, agree)
            out.append(f"| {label} | {len(group)} | {tokens} | {agree} | {recuts} |")
        out += [""] + problems[:12]

        count = 150 if quick else 600
        expressions = [random_expression(rng) for _ in range(count)]
        different = compare_expressions(expressions, folder)
    longest = max(expressions, key=len)
    out += ["", "### Expression trees: ParC's parser and Clang's parser on the same expressions", "",
            "An expression made of names, numbers, `+ - * / %`, comparisons, `&&`, `||`, `!`, unary minus, `p[...]` and "
            "`g(...)` is written the same way in both languages, and both give the operators the same precedence and "
            "associativity. Each random expression is parsed by ParC (`parc ast`) and by Clang (`-ast-dump=json`); both "
            "trees are printed with every operation in parentheses and must be equal.", "",
            "| Expressions | Same tree | Different |", "|---|---|---|",
            f"| {count} | {count - len(different)} | {len(different)} |", ""]
    for expression, mine, other in different[:8]:
        out.append(f"- `{expression}`: ParC `{mine}`, Clang `{other}`")
    example = next(e for e in expressions if 25 < len(e) < 60 and "(" not in e)
    with tempfile.TemporaryDirectory(prefix="parc_front_") as folder:
        compare_expressions([example], folder)
        shape = parc_shape(json.loads((Path(folder) / "e.json").read_text(encoding="utf-8"))["functions"][0]["body"]["stmts"][0]["value"])
    out += [f"For example `{example}` is read by both as `{shape}`. The longest expression had {len(longest)} characters.", "",
            "Not compared: declarations and statements. `fn f(a: int) -> int` and `for i in 0..n` have no C spelling, "
            "so a comparison would go through the translator (`to_c.py`) and test the translator, not the parser. "
            "Those are covered from the other end: the translated programs must print what ParC prints (section 1).", ""]
    all_files = sum(n for n, _, _ in totals.values())
    all_agree = sum(a for _, _, a in totals.values())
    all_tokens = sum(t for _, t, _ in totals.values())
    summary = (f"tokens: {all_agree} of {all_files} texts agree ({all_tokens} tokens); "
               f"expression trees: {count - len(different)} of {count} agree")
    return out, summary


if __name__ == "__main__":
    lines, summary = section("--quick" in sys.argv)
    print("\n".join(lines))
    print(summary, file=sys.stderr)
