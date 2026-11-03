"""Explanations and fixes that need no language model.

They serve two purposes. When no model is available, they are what the user
sees. And when one is, the template fix is still always one of the candidates,
because it is cheap and usually right (this is the role the repair templates
of the earlier VulnHGNN project had).

Everything here is built from the fields the compiler puts in a finding:
kind, line, subject, proof, guard, stmt_line. Nothing is guessed.
"""

import re

# For each kind of finding: why it matters, for a student and for an engineer,
# and a question that leads a student to the problem without giving it away.
KINDS = {
    "divide-by-zero": {
        "student": "Dividing by zero has no answer, so the program stops at this line when it happens.",
        "engineer": "An unchecked divisor is a crash on that input.",
        "question": "What value can {guard_subject} have here, and what does line {line} do with that value?",
    },
    "overflow": {
        "student": "An int only holds numbers up to 2147483647. A result above that wraps around to a negative "
                   "number, and the program carries on with a wrong value.",
        "engineer": "The result can exceed INT_MAX; the VM wraps, C would be undefined behaviour.",
        "question": "What is the largest value {subject} can have, and does it still fit in an int?",
    },
    "underflow": {
        "student": "An int only holds numbers down to -2147483648. A result below that wraps around to a large "
                   "positive number, and the program carries on with a wrong value.",
        "engineer": "The result can fall below INT_MIN and wrap.",
        "question": "What is the smallest value {subject} can have, and does it still fit in an int?",
    },
    "null-access": {
        "student": "null means \"no array\". Reading or writing an element of no array stops the program.",
        "engineer": "A null array reference is dereferenced without a check.",
        "question": "Can a caller hand this function null, and what does line {line} do then?",
    },
    "index-out-of-bounds": {
        "student": "An array with n elements has positions 0 to n - 1. Any other position stops the program.",
        "engineer": "The index range is not contained in the array bounds.",
        "question": "What are the smallest and the largest value of the index on line {line}, and how long is the array?",
    },
    "negative-size": {
        "student": "An array can't have fewer than 0 elements, so asking for a negative size stops the program.",
        "engineer": "The allocation size is not checked for a negative value.",
        "question": "Can the size on line {line} be below zero?",
    },
    "uninitialized": {
        "student": "The variable is read before every path has given it a value. The VM then sees 0, which is "
                   "probably not what was meant.",
        "engineer": "Read of a variable that is not definitely assigned.",
        "question": "Follow the path where the condition before line {line} is false: what value does {subject} have?",
    },
    "dead-store": {
        "student": "This value is thrown away: nothing reads the variable before it is assigned again or the "
                   "function ends. Either the line is not needed, or a use of the value is missing.",
        "engineer": "The store is dead; the value is overwritten or never read.",
        "question": "Which line reads the value that line {line} stores in {subject}?",
    },
    "unused-variable": {
        "student": "A variable nobody reads is usually a leftover, or a sign that some code is missing.",
        "engineer": "The variable is never read.",
        "question": "Where is {subject} used after line {line}?",
    },
    "unused-value": {
        "student": "The expression is computed and then thrown away, so the line has no effect.",
        "engineer": "Expression statement with no effect.",
        "question": "What happens to the value computed on line {line}?",
    },
    "unreachable-code": {
        "student": "Every path ends before it gets here, so these lines never run.",
        "engineer": "No path from the function's entry reaches this statement.",
        "question": "Which statement runs just before line {line}, and where does it send the program?",
    },
    "shadowing": {
        "student": "Two different variables have the same name. Inside the block the name means the new one, "
                   "which makes it easy to change or read the wrong one.",
        "engineer": "An inner declaration hides an outer one.",
        "question": "Which {subject} does the code after line {line} use, the new one or the old one?",
    },
    "missing-return": {
        "student": "The function promises to return a value, but some path reaches its end without a return.",
        "engineer": "Not all paths return a value.",
        "question": "Which path through the function ends without reaching a return?",
    },
    "parallel-dependence": {
        "student": "In a parallel loop the iterations run at the same time, in any order. If one iteration needs "
                   "something another one writes, the result depends on which happens to run first. The compiler "
                   "therefore runs this loop one iteration after the other.",
        "engineer": "A loop-carried dependence makes the parallel loop a data race; it is run serially.",
        "question": "Which value does one iteration read that a different iteration writes?",
    },
    "float-reduction": {
        "student": "Each thread adds up its own part and the parts are added at the end. With floats the order of "
                   "additions changes the last digits, so the result can differ slightly from a normal loop's.",
        "engineer": "Floating-point reduction is not associative; the result is not bit-reproducible.",
        "question": "Is (a + b) + c always exactly a + (b + c) for floats?",
    },
    "parallelizable": {
        "student": "No iteration of this loop uses anything another iteration writes, so they could all run at once.",
        "engineer": "No loop-carried dependence was found; the loop is a candidate for parallel for.",
        "question": "Does any iteration of this loop need a result from another iteration?",
    },
    "parallelized": {
        "student": "The compiler checked that no iteration uses anything another iteration writes, so it lets them run at the same time.",
        "engineer": "The dependence tests found no loop-carried dependence.",
        "question": "Why is it safe for the iterations of this loop to run in any order?",
    },
    "loop-dependence": {
        "student": "Each iteration needs a value that an earlier iteration produced, so they have to run in order.",
        "engineer": "A loop-carried dependence prevents parallelization.",
        "question": "Which value does an iteration need from the one before it?",
    },
    "recursion": {
        "student": "A function that calls itself needs a case where it stops, or the calls never end.",
        "engineer": "The function is part of a cycle in the call graph.",
        "question": "For which argument does the function return without calling itself?",
    },
    "unused-function": {
        "student": "Nothing calls this function, so its code never runs.",
        "engineer": "The function is unreachable from main in the call graph.",
        "question": "Which function calls {subject}?",
    },
    "missed-optimization": {
        "student": "The optimizer looked at this line and explains why it had to leave it as it is.",
        "engineer": "A transformation was not applicable; the reason is given.",
        "question": "What would have to be true for this computation to be moved or simplified?",
    },
}

DEFAULT = {
    "student": "The compiler found a problem on this line.",
    "engineer": "Reported by the compiler.",
    "question": "What does line {line} do, and what does the compiler say is wrong with it?",
}


def _fill(text, finding):
    guard = finding.get("guard", "")
    # "n == 0" is about n; "i < 0 || i >= len(a)" is about i.
    guard_subject = re.split(r"\s*(==|<|>|!=|<=|>=)\s*", guard)[0] if guard else finding.get("subject", "this value")
    return text.format(line=finding.get("line", 0), subject=finding.get("subject") or "this value",
                       guard_subject=guard_subject or "this value")


def explanation(finding, level="student"):
    """A few sentences, each ending in the id of the fact that supports it."""
    info = KINDS.get(finding.get("kind", ""), DEFAULT)
    cite = f"[{finding['id']}]"
    sentences = [f"Line {finding['line']}: {finding['message']}. {cite}"]
    sentences.append(f"{info['engineer' if level == 'engineer' else 'student']} {cite}")
    if finding.get("proof"):
        proof = finding["proof"]
        sentences.append(f"How the compiler knows: {proof[0].lower() + proof[1:]}. {cite}")
    if finding.get("cwe") and level == "engineer":
        sentences.append(f"This is weakness class {finding['cwe']}. {cite}")
    if finding.get("hint"):
        hint = finding["hint"]
        sentences.append(f"What to do: {hint[0].lower() + hint[1:]}. {cite}")
    return " ".join(sentences)


def question(finding):
    """Hint mode: a question that points at the problem without stating the fix."""
    return _fill(KINDS.get(finding.get("kind", ""), DEFAULT)["question"], finding)


# ---------------------------------------------------------------- fixes

def _indent_of(line):
    return line[: len(line) - len(line.lstrip())]


def _bail(return_type):
    """The statement that leaves the function early with a harmless value."""
    if not return_type:
        return "return;"
    if return_type.endswith("[]"):
        return "return null;"
    return {"int": "return 0;", "float": "return 0.0;", "bool": "return false;"}.get(return_type, "return;")


def _default_value(type_text):
    if type_text.endswith("[]"):
        return "null"
    return {"int": "0", "float": "0.0", "bool": "false"}.get(type_text, "0")


def _block_end(lines, start):
    """Index of the last line of the statements that follow `start` in the same block."""
    depth = 0
    for i in range(start, len(lines)):
        code = lines[i].split("//")[0]
        for ch in code:
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth < 0:
                    return i - 1
    return len(lines) - 1


def fixes(source, finding, return_type, function_span):
    """Candidate rewrites of the whole program, each a (description, new source) pair.

    `return_type` is the flagged function's ("" for none) and `function_span`
    its (first line, last line), both 1-based. Each candidate changes only
    lines inside that span. The gate decides whether any of them is good.
    """
    lines = source.split("\n")
    kind = finding.get("kind", "")
    line = finding.get("stmt_line") or finding.get("line", 0)
    if not 1 <= line <= len(lines):
        return []
    at = line - 1
    indent = _indent_of(lines[at])
    out = []

    def candidate(description, new_lines):
        text = "\n".join(new_lines)
        if text != source:
            out.append((description, text))

    guard = finding.get("guard", "")
    if kind in ("divide-by-zero", "null-access", "index-out-of-bounds", "overflow", "underflow", "negative-size") and guard:
        check = [f"{indent}if ({guard}) {{", f"{indent}    {_bail(return_type)}", f"{indent}}}"]
        candidate(f"check `{guard}` before line {line} and leave the function early", lines[:at] + check + lines[at:])

    elif kind == "uninitialized":
        name = finding.get("subject", "")
        first, last = function_span
        for i in range(first - 1, min(last, len(lines))):
            match = re.match(rf"^(\s*let\s+{re.escape(name)}\s*:\s*([A-Za-z]+(?:\[\])?))\s*;(.*)$", lines[i])
            if match:
                fixed = f"{match.group(1)} = {_default_value(match.group(2))};{match.group(3)}"
                candidate(f"give `{name}` a value where it is declared", lines[:i] + [fixed] + lines[i + 1:])
                break

    elif kind == "unused-variable":
        name = finding.get("subject", "")
        if re.match(rf"^\s*let\s+{re.escape(name)}\s*:[^;()]*;\s*(//.*)?$", lines[at]):
            candidate(f"remove the unused variable `{name}`", lines[:at] + lines[at + 1:])

    elif kind == "dead-store":
        name = finding.get("subject", "")
        declared = re.match(rf"^(\s*let\s+{re.escape(name)}\s*:\s*[A-Za-z]+(?:\[\])?)\s*=[^;()]*;(.*)$", lines[at])
        if declared:
            # The variable is still needed later, only this first value is not.
            candidate(f"declare `{name}` without the value that is never used",
                      lines[:at] + [f"{declared.group(1)};{declared.group(2)}"] + lines[at + 1:])
        elif re.match(rf"^\s*{re.escape(name)}\s*=[^;()]*;\s*(//.*)?$", lines[at]):
            candidate(f"remove the assignment to `{name}` that is never used", lines[:at] + lines[at + 1:])

    elif kind == "unused-value":
        if lines[at].strip().endswith(";") or "//" in lines[at]:
            candidate("remove the statement that has no effect", lines[:at] + lines[at + 1:])

    elif kind == "unreachable-code":
        end = _block_end(lines, at)
        if end >= at:
            candidate(f"remove the code that can never run (lines {line} to {end + 1})", lines[:at] + lines[end + 1:])

    elif kind == "missing-return":
        first, last = function_span
        closing = last - 1
        body_indent = _indent_of(lines[first]) if first < len(lines) and lines[first].strip() else "    "
        candidate("return a default value at the end of the function",
                  lines[:closing] + [f"{body_indent}{_bail(return_type)}"] + lines[closing:])

    elif kind in ("parallel-dependence", "float-reduction"):
        needs = re.search(r"needs (reduce\([+*]: \w+\))", finding.get("message", ""))
        header = lines[at]
        if needs and "reduce(" not in header and "{" in header:
            with_clause = header.replace("{", needs.group(1) + " {", 1)
            candidate(f"declare the reduction: {needs.group(1)}", lines[:at] + [with_clause] + lines[at + 1:])
        serial = re.sub(r"\bparallel\s+for\b", "for", header, count=1)
        serial = re.sub(r"\s*reduce\([^)]*\)", "", serial)
        candidate("make it a normal loop, which runs one iteration after the other", lines[:at] + [serial] + lines[at + 1:])

    elif kind == "parallelizable":
        header = lines[at]
        if re.search(r"\bfor\b", header) and "parallel" not in header and "{" in header:
            parallel = re.sub(r"\bfor\b", "parallel for", header, count=1)
            if guard:
                parallel = parallel.replace("{", guard + " {", 1)
            candidate("run the loop in parallel", lines[:at] + [parallel] + lines[at + 1:])

    return out
