"""The compiler gate: the judge of every proposed rewrite.

A candidate is a whole new source text in which one function was rewritten.
It is accepted only if it passes every check, in this order, stopping at the
first failure:

    1. scope      nothing outside the flagged function changed
    2. compiles   all phases run without an error
    3. fixed      the finding it was written for is gone
    4. no harm    no warning appeared that was not there before
    5. behaviour  on inputs where the original function worked, the rewrite
                  returns and prints exactly the same

Each check is something the compiler measures, so the verdict can be defended.
Passing candidates are then ranked: fewest remaining warnings first, then the
smallest change to the user's code.
"""

import difflib
import random
import re

import compiler_bridge as bridge

INPUTS_PER_FUNCTION = 40
INTERESTING_INTS = [0, 1, 2, 3, 5, 10, -1, -7, 100, 1000, 2147483647, -2147483648]


# ---------------------------------------------------------------- the function a finding is in

def function_span(source, name):
    """(first line, last line) of function `name`, 1-based, found by matching braces.

    Strings and comments are skipped, so a brace inside them does not count.
    Returns None if there is no such function.
    """
    match = re.search(rf"^[ \t]*fn\s+{re.escape(name)}\s*\(", source, flags=re.MULTILINE)
    if not match:
        return None
    first = source.count("\n", 0, match.start()) + 1
    depth = 0
    i = match.end()
    seen_open = False
    while i < len(source):
        ch = source[i]
        if source.startswith("//", i):
            end = source.find("\n", i)
            i = len(source) if end < 0 else end
            continue
        if source.startswith("/*", i):
            end = source.find("*/", i + 2)
            i = len(source) if end < 0 else end + 2
            continue
        if ch == '"':
            i += 1
            while i < len(source) and source[i] not in '"\n':
                i += 2 if source[i] == "\\" else 1
        elif ch == "{":
            depth += 1
            seen_open = True
        elif ch == "}":
            depth -= 1
            if seen_open and depth == 0:
                return first, source.count("\n", 0, i) + 1
        i += 1
    return None


def function_text(source, name):
    span = function_span(source, name)
    if not span:
        return ""
    return "\n".join(source.split("\n")[span[0] - 1: span[1]])


def replace_function(source, name, new_text):
    """The source with function `name` replaced by `new_text`, or None if it has no such function."""
    span = function_span(source, name)
    if not span:
        return None
    lines = source.split("\n")
    return "\n".join(lines[: span[0] - 1] + new_text.strip("\n").split("\n") + lines[span[1]:])


def _outside(source, name):
    span = function_span(source, name)
    if not span:
        return None
    lines = source.split("\n")
    return lines[: span[0] - 1] + ["<function>"] + lines[span[1]:]


# ---------------------------------------------------------------- comparing findings

def identity(finding):
    """What makes two findings "the same" before and after a rewrite: line numbers move, these do not."""
    return finding.get("kind", ""), finding.get("function", ""), finding.get("subject", "")


def _problems(build):
    """The identities of the errors and warnings (notes are not problems)."""
    return [identity(f) for f in build["findings"]["findings"] if f["severity"] in ("error", "warning")]


def _count(items, item):
    return sum(1 for other in items if other == item)


# ---------------------------------------------------------------- test inputs

def _parameter_types(build, function):
    for symbol in build.get("symbols", {}).get("symbols", []):
        if symbol["kind"] == "function" and symbol["name"] == function:
            inside = symbol["type"][symbol["type"].index("(") + 1: symbol["type"].index(")")]
            return [part.split(":")[1].strip() for part in inside.split(",") if ":" in part]
    return None


def _value(rng, type_text):
    if type_text.endswith("[]"):
        if rng.random() < 0.1:
            return "null"
        base = type_text[:-2]
        return "[" + ",".join(_value(rng, base) for _ in range(rng.randint(0, 6))) + "]"
    if type_text == "bool":
        return rng.choice(["true", "false"])
    if type_text == "float":
        return rng.choice(["0.0", "1.0", "-2.5", "0.5", "100.0", "3.25"])
    if rng.random() < 0.6:
        return str(rng.randint(-20, 20))
    return str(rng.choice(INTERESTING_INTS))


def test_inputs(types, count=INPUTS_PER_FUNCTION):
    """Argument lists for a function with these parameter types. Always the same ones (fixed seed)."""
    rng = random.Random(20261001)
    return [" ".join(_value(rng, t) for t in types) for _ in range(count)]


def _clean(result):
    return result["ok"] and result["overflows"] == 0 and result["races"] == 0


def _never_returns(result):
    """The VM gave up on this call after its step limit: an endless loop."""
    return not result["ok"] and "steps and was stopped" in result["error"]


# ---------------------------------------------------------------- the gate

def diff_size(original, candidate):
    """How many lines were added, removed or changed."""
    changed = 0
    for line in difflib.ndiff(original.split("\n"), candidate.split("\n")):
        if line.startswith(("+ ", "- ")):
            changed += 1
    return changed


def judge(original, original_build, finding, candidate):
    """Runs the gate on one candidate source. Returns a verdict:

        {"passed": bool, "failed_at": "", "reason": "", "evidence": [...],
         "warnings": int, "diff": int}
    """
    function = finding.get("function", "")
    verdict = {"passed": False, "failed_at": "", "reason": "", "evidence": [], "warnings": 0,
               "diff": diff_size(original, candidate)}

    def fail(stage, reason):
        verdict["failed_at"] = stage
        verdict["reason"] = reason
        return verdict

    # 1. Scope: only the flagged function may change.
    before, after = _outside(original, function), _outside(candidate, function)
    if before is None or after is None or before != after:
        return fail("scope", f"the rewrite changes code outside the function '{function}'")
    if verdict["diff"] == 0:
        return fail("scope", "the rewrite is identical to the original")

    # 2. It compiles through every phase.
    try:
        build = bridge.build(candidate)
    except bridge.CompilerTimeout:
        return fail("compiles", "the compiler did not finish on the rewrite")
    if build["stage"] != "complete":
        first = build["errors"][0] if build["errors"] else {"line": 0, "message": "it does not compile"}
        return fail("compiles", f"it does not compile: line {first['line']}: {first['message']}")
    verdict["evidence"].append("compiles through all phases")

    # 3. The finding is gone.
    target = identity(finding)
    is_note = finding["severity"] == "info"
    old_all = [identity(f) for f in original_build["findings"]["findings"]]
    new_all = [identity(f) for f in build["findings"]["findings"]]
    if _count(new_all, target) >= _count(old_all, target):
        return fail("fixed", f"the compiler still reports it: {finding['message']}")
    verdict["evidence"].append(f"{finding['id']} is gone")

    # 4. Nothing new went wrong.
    old_problems = _problems(original_build)
    new_problems = _problems(build)
    for problem in set(new_problems):
        if _count(new_problems, problem) > _count(old_problems, problem):
            message = next(f["message"] for f in build["findings"]["findings"] if identity(f) == problem)
            return fail("no harm", f"a new warning appeared: {message}")
    verdict["warnings"] = len(new_problems)
    verdict["evidence"].append("no new warnings" if not is_note else "no warnings were added")

    # 5. Behaviour. First the whole program, exactly as it is.
    try:
        old_run = bridge.run(original, optimize=False)
        new_run = bridge.run(candidate, optimize=False)
    except bridge.CompilerTimeout:
        return fail("behaviour", "the rewritten program did not finish in time")
    if old_run.get("compiled") and _clean(old_run):
        if not new_run.get("compiled") or not _clean(new_run):
            error = new_run.get("error", {}).get("message", "it overflows or races")
            return fail("behaviour", f"the program ran cleanly before and now does not: {error}")
        # Printing order inside a parallel loop depends on the schedule, so compare lines as a set there.
        same = old_run["output"] == new_run["output"]
        if not same and (old_run["parallel_loops"] or new_run["parallel_loops"]):
            same = sorted(old_run["output"].split("\n")) == sorted(new_run["output"].split("\n"))
        if not same:
            return fail("behaviour", "the program prints something different: expected "
                        f"{old_run['output'][:80]!r}, got {new_run['output'][:80]!r}")
        verdict["evidence"].append("the program prints exactly the same as before")
    elif old_run.get("compiled"):
        problem = old_run.get("error", {}).get("message") or "an int overflow"
        if new_run.get("compiled") and _clean(new_run):
            verdict["evidence"].append(f"the original run had a problem ({problem}); the rewrite runs cleanly")
        else:
            verdict["evidence"].append(f"the original run had a problem ({problem}), so its output is no reference")

    # Then the flagged function alone, on many inputs. Wherever the original
    # worked without trouble, the rewrite must do exactly the same.
    types = _parameter_types(original_build, function)
    if types and function != "main":
        inputs = test_inputs(types)
        try:
            old_results = bridge.call(original, function, inputs, optimize=False)
            new_results = bridge.call(candidate, function, inputs, optimize=False)
        except bridge.CompilerTimeout:
            return fail("behaviour", "the rewritten function did not finish in time on the test inputs")
        if old_results is None:
            return verdict | {"passed": True}  # the function can't be run on its own; the checks above stand
        if new_results is None:
            return fail("behaviour", "the rewrite changed the function's parameters")
        same_on = fixed = 0
        for old, new in zip(old_results, new_results):
            if not _clean(old):
                # The original went wrong here, so there is no result to compare with.
                # One thing is still worse than any error: not coming back at all.
                if _never_returns(new) and not _never_returns(old):
                    return fail("behaviour", f"for the arguments ({old['args']}) the original stops with an error "
                                             "and the rewrite never returns (an endless loop)")
                fixed += 1 if _clean(new) else 0
                continue
            if (not _clean(new) or old["returned"] != new["returned"] or old["output"] != new["output"]
                    or old["arrays"] != new["arrays"]):
                got = new["error"] or new["returned"] or new["output"]
                return fail("behaviour", f"for the arguments ({old['args']}) the original gives {old['returned'] or old['output']!r} "
                                         f"and the rewrite gives {got!r}")
            same_on += 1
        verdict["evidence"].append(f"same result on {same_on} of {len(inputs)} test inputs where the original works")
        if fixed:
            verdict["evidence"].append(f"{fixed} inputs that made the original fail now work")

    verdict["passed"] = True
    return verdict


def rank(candidates):
    """Orders judged candidates: passing ones first, then fewest warnings, then smallest change."""
    return sorted(candidates, key=lambda c: (not c["verdict"]["passed"], c["verdict"]["warnings"], c["verdict"]["diff"]))
