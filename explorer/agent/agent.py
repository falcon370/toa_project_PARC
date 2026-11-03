"""explain() and fix(): what the explorer asks the agent to do with one finding.

Both start from a build (the result of compiler_bridge.build) and a finding
from its findings list. The language model only ever sees the source of the
flagged function and a short list of facts; it never sees the token or tree
JSON, which would be thousands of tokens of noise.

Everything that goes to the model and everything that comes back passes the
security layer (security.py) first: the user's code may try to instruct the
model, and the model's answer may not be honest.
"""

import json
import re

import compiler_bridge as bridge

from . import gate, llm, security, templates

MAX_TOOL_STEPS = 4   # how often the model may ask a tool for more information
MAX_RETRIES = 2      # how often a failed candidate is sent back with the reason

EXPLAIN_SYSTEM = (
    "You explain findings of the ParC compiler to a {audience}. ParC is a small C-like language.\n"
    "Rules:\n"
    "- Say only things that the listed facts support. Do not invent other problems.\n"
    "- End every sentence with the id of the fact it rests on, in square brackets, like [F3].\n"
    "- {style}\n"
    "- Do not write any code and do not give the fix; explain what goes wrong and why."
)

FIX_SYSTEM = (
    "You repair one function of a ParC program. ParC is a small C-like language: types are int, float, bool and\n"
    "arrays like int[]; variables are declared as `let x: int = 0;`; loops are `for i in 0..n {{ }}` and\n"
    "`while (c) {{ }}`; a parallel loop is `parallel for i in 0..n reduce(+: sum) {{ }}`; conditions need\n"
    "parentheses and blocks need braces; arrays can be null and len(a) gives their length; there is no cast.\n"
    "Rules:\n"
    "- Change only the function named in the request. Keep its name and parameters.\n"
    "- Give {count} different complete versions of that function, each in its own ```parc code block.\n"
    "- The finding must be gone, and the function must behave the same on inputs that worked before.\n"
    "- Before answering you may ask for more information by replying with exactly one line:\n"
    "  TOOL: report            (all findings of the function)\n"
    "  TOOL: show_ir           (the compiler's intermediate code for the function)\n"
    "  You will get the result and can then answer."
)


# ---------------------------------------------------------------- facts and context

def find_finding(build, finding_id):
    for finding in build["findings"]["findings"]:
        if finding.get("id") == finding_id:
            return finding
    return None


def fact_line(finding):
    """One finding as a line the model can cite: F3 (semantic, line 6, divide-by-zero): message. Proof: ..."""
    text = f"{finding['id']} ({finding['phase']}, line {finding['line']}, {finding.get('kind', 'error')}): {finding['message']}."
    if finding.get("proof"):
        text += f" Proof: {finding['proof']}."
    return text


def facts_for(build, finding):
    """The finding itself first, then the other findings in the same function."""
    others = [f for f in build["findings"]["findings"]
              if f is not finding and f.get("function") == finding.get("function") and f.get("id")]
    return [finding] + others[:7]


def numbered(text, first_line):
    return "\n".join(f"{first_line + i:4} | {line}" for i, line in enumerate(text.split("\n")))


def context(source, build, finding):
    """The part of the program the model is shown: the flagged function, with line numbers,
    and the signatures of the functions it calls."""
    function = finding.get("function", "")
    span = gate.function_span(source, function)
    if not span:
        return numbered(source, 1), ""
    text = gate.function_text(source, function)
    called = set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", text)) - {function, "print", "len", "if", "while"}
    signatures = [s["type"] for s in build.get("symbols", {}).get("symbols", [])
                  if s["kind"] == "function" and s["name"] in called]
    return numbered(text, span[0]), "\n".join(signatures)


def guarded_context(source, build, finding, facts):
    """context(), after the security layer has looked at what the model would see.

    Returns (code, signatures, use_model, security events). If a comment tries
    to instruct the model, the code is shown without comments; if a string or a
    name does, the model is not used at all.
    """
    function = finding.get("function", "")
    code, signatures = context(source, build, finding)
    text = gate.function_text(source, function) or source
    decision = security.check_input(text, [signatures] + [f.get("subject", "") for f in facts], function,
                                    finding.get("id", ""))
    if decision["strip_comments"]:
        span = gate.function_span(source, function)
        code = numbered(security.strip_comments(text), span[0] if span else 1)
    return code, signatures, decision["use_model"], decision["events"]


# ---------------------------------------------------------------- explain

def grounded(text, allowed_ids):
    """Keeps only the sentences that cite one of the given facts.

    Returns (kept sentences, number dropped). A sentence without a citation, or
    citing a fact that was not given, is an unsupported claim.
    """
    # Models write "... zero [F3]." or "... zero. [F3]"; put the citation inside the sentence either way.
    text = re.sub(r"([.!?])\s*((?:\[F\d+\][,\s]*)+)", lambda m: " " + m.group(2).strip() + m.group(1) + " ", text)
    kept, dropped = [], 0
    for sentence in re.split(r"(?<=[.!?])\s+", text.replace("\n", " ")):
        sentence = sentence.strip()
        if len(sentence) < 4:
            continue
        cited = re.findall(r"\[(F\d+)\]", sentence)
        if cited and all(c in allowed_ids for c in cited):
            kept.append(sentence)
        else:
            dropped += 1
    return kept, dropped


def explain(source, build, finding_id, level="student", hint=False, models=None):
    """Explains one finding.

    Returns {"explanation", "cited", "dropped", "source", "level", "hint", "security"}.
    With hint=True the explanation is only a guiding question. "security" lists
    what the security layer noticed and did.
    """
    finding = find_finding(build, finding_id)
    if finding is None:
        return {"error": f"there is no finding {finding_id}"}
    level = "engineer" if level == "engineer" else "student"
    if hint:
        return {"explanation": templates.question(finding), "cited": [finding["id"]], "dropped": 0,
                "source": "template", "level": level, "hint": True, "security": []}

    facts = facts_for(build, finding)
    allowed = {f["id"] for f in facts}
    code, signatures, use_model, events = guarded_context(source, build, finding, facts)
    system = EXPLAIN_SYSTEM.format(
        audience="student who is learning to program" if level == "student" else "experienced engineer",
        style="Use plain words and short sentences, 3 to 5 of them." if level == "student"
        else "Be brief and precise, 2 to 4 sentences; name the weakness class if a fact gives one.")
    prompt = (f"Explain finding {finding['id']}.\n\n{code}\n\n" + (f"Functions it calls:\n{signatures}\n\n" if signatures else "")
              + "Facts:\n" + "\n".join(fact_line(f) + (f" {f['cwe']}." if f.get("cwe") else "") for f in facts))
    text, model = llm.complete(system, prompt, models) if use_model else (None, "none")
    if text:
        # Some models write names as $n$ or `n`; the page shows plain text.
        text = re.sub(r"[$`]([^$`\n]{1,40})[$`]", r"\1", text)
        honest, more = security.check_explanation(text, finding, finding.get("function", ""))
        events += more
        kept, dropped = grounded(text, allowed) if honest else ([], 0)
        if kept:
            return {"explanation": " ".join(kept), "cited": sorted(set(re.findall(r"F\d+", " ".join(kept)))),
                    "dropped": dropped, "source": model, "level": level, "hint": False, "security": events}
    # No model, an answer the security layer refused, or nothing in it backed by a
    # fact: the template says what the compiler knows.
    return {"explanation": templates.explanation(finding, level), "cited": [finding["id"]], "dropped": 0,
            "source": "template", "level": level, "hint": False, "security": events}


# ---------------------------------------------------------------- tools the model may call

def tool_report(build, function):
    """All findings of one function, as fact lines."""
    lines = [fact_line(f) for f in build["findings"]["findings"] if f.get("function") == function and f.get("id")]
    return "\n".join(lines) or "no findings"


def tool_show_ir(build, function):
    """The three-address code of one function."""
    for fn in build.get("ir", {}).get("functions", []):
        if fn["name"] == function:
            return "\n".join(f"{block['label']}:\n" + "\n".join("    " + i["text"] for i in block["instrs"])
                             for block in fn["blocks"])
    return "no intermediate code (the program has errors)"


def tool_compile(source):
    """{ok, errors} for a source text."""
    result = bridge.build(source)
    return {"ok": result["stage"] == "complete",
            "errors": [{"line": e["line"], "col": e["col"], "message": e["message"]} for e in result["errors"]]}


def tool_run_tests(original, rewritten, build, finding):
    """{match, reason}: does the rewrite pass the gate?"""
    verdict = gate.judge(original, build, finding, rewritten)
    return {"match": verdict["passed"], "reason": verdict["reason"], "evidence": verdict["evidence"]}


# ---------------------------------------------------------------- fix

def code_blocks(text):
    """The contents of the ``` blocks in a model's answer."""
    return [block.strip("\n") for block in re.findall(r"```[a-zA-Z]*\n(.*?)```", text, flags=re.DOTALL)]


def _ask_for_rewrites(system, prompt, build, function, models, finding_id, events):
    """Asks the model, answering its tool requests, until it gives code or the step budget runs out."""
    transcript = prompt
    used = "none"
    for _ in range(MAX_TOOL_STEPS + 1):
        text, used = llm.complete(system, transcript, models)
        if not text:
            return [], used
        request = re.match(r"\s*TOOL:\s*(\w+)", text)
        if not request:
            return code_blocks(text), used
        name = request.group(1)
        # There are two tools and both only read. A request for anything else
        # (say "disable_gate") is refused and logged.
        known, more = security.check_tool(name, function, finding_id)
        events += more
        result = tool_report(build, function) if name == "report" else \
            tool_show_ir(build, function) if name == "show_ir" else f"there is no tool named {name}"
        transcript += f"\n\nYou asked: TOOL: {name}\nRESULT:\n{result}\n\nNow give the rewritten versions."
    events += security.tool_budget_used(function, finding_id)
    return [], used


def fix(source, build, finding_id, models=None):
    """Finds the best checked rewrite for one finding.

    Returns {"fixed": bool, "source": new program, "function": new function text,
             "description", "evidence", "candidates": [...], "model", "reason", "security"}.
    Every candidate, passing or not, is listed with the verdict of the security
    layer or of the compiler gate.
    """
    finding = find_finding(build, finding_id)
    if finding is None:
        return {"error": f"there is no finding {finding_id}"}
    function = finding.get("function", "")
    span = gate.function_span(source, function)
    if not span:
        return {"fixed": False, "reason": "the finding is not inside a function, so there is nothing to rewrite",
                "candidates": [], "model": "none", "security": []}
    signature = next((s["type"] for s in build.get("symbols", {}).get("symbols", [])
                      if s["kind"] == "function" and s["name"] == function), "")
    return_type = signature.split("->")[1].strip() if "->" in signature else ""

    judged = []
    seen = set()

    def consider(description, candidate_source, origin):
        if candidate_source is None or candidate_source in seen:
            return None
        seen.add(candidate_source)
        entry = {"description": description, "origin": origin, "source": candidate_source,
                 "function": gate.function_text(candidate_source, function),
                 "verdict": gate.judge(source, build, finding, candidate_source)}
        judged.append(entry)
        return entry

    # The template fix is always a candidate.
    for description, candidate in templates.fixes(source, finding, return_type, span):
        consider(description, candidate, "template")

    # The model's candidates, with failed ones sent back together with the reason.
    original_function = gate.function_text(source, function)
    code, signatures, use_model, events = guarded_context(source, build, finding, facts_for(build, finding))
    system = FIX_SYSTEM.format(count="2 or 3")
    prompt = (f"Fix finding {finding['id']} in the function '{function}'. Change only this function.\n\n{code}\n\n"
              + (f"Functions it calls:\n{signatures}\n\n" if signatures else "")
              + "Facts:\n" + "\n".join(fact_line(f) for f in facts_for(build, finding)))
    model_used = "none"
    for attempt in range(MAX_RETRIES + 1 if use_model else 0):
        rewrites, model_used = _ask_for_rewrites(system, prompt, build, function, models, finding["id"], events)
        if not rewrites:
            break
        failures = []
        for number, rewrite in enumerate(rewrites, 1):
            label = f"rewrite {number} by the model" + (f" (attempt {attempt + 1})" if attempt else "")
            # First the security layer: does the rewrite do more than fix the finding?
            harmless, more = security.check_rewrite(original_function, rewrite, function, finding["id"])
            events += more
            if not harmless:
                reason = "refused by the security check: " + more[-1]["evidence"]
                judged.append({"description": label, "origin": model_used, "source": None, "function": rewrite,
                               "verdict": {"passed": False, "failed_at": "security", "reason": reason, "evidence": [],
                                           "warnings": 0, "diff": 0}})
                failures.append(f"Version {number} was {reason}.")
                continue
            # A rewrite must be exactly the one function; splicing it in keeps the rest of the file untouched.
            names = re.findall(r"^\s*fn\s+(\w+)", rewrite, flags=re.MULTILINE)
            if names != [function]:
                failures.append(f"Version {number} was rejected: it must contain exactly the function '{function}'.")
                continue
            entry = consider(label, gate.replace_function(source, function, rewrite), model_used)
            if entry and not entry["verdict"]["passed"]:
                failures.append(f"Version {number} was rejected at the '{entry['verdict']['failed_at']}' check: "
                                f"{entry['verdict']['reason']}.")
        if any(c["verdict"]["passed"] and c["origin"] != "template" for c in judged) or not failures:
            break
        prompt += "\n\nYour earlier versions did not pass the compiler's checks:\n" + "\n".join(failures) + \
                  "\nGive new versions that avoid these problems."

    ranked = gate.rank(judged)
    summary = [{"description": c["description"], "origin": c["origin"], "passed": c["verdict"]["passed"],
                "failed_at": c["verdict"]["failed_at"], "reason": c["verdict"]["reason"],
                "warnings": c["verdict"]["warnings"], "diff": c["verdict"]["diff"], "function": c["function"]}
               for c in ranked]
    passed = [c for c in ranked if c["verdict"]["passed"]]
    if not passed:
        reason = "no rewrite passed the compiler's checks" if judged else \
            "there is no template for this kind of finding, and no language model is set up to propose a rewrite"
        if judged:
            reason += ": " + "; ".join(f"{c['description']}: {c['verdict']['reason']}" for c in ranked[:3])
        return {"fixed": False, "reason": reason, "candidates": summary, "model": model_used, "security": events}
    best = passed[0]
    evidence = list(best["verdict"]["evidence"])
    evidence.append(f"{len(passed)} of {len(judged)} candidates passed; "
                    + ("this one leaves the fewest warnings and changes the fewest lines" if len(passed) > 1
                       else "it changes " + str(best["verdict"]["diff"]) + " lines"))
    return {"fixed": True, "source": best["source"], "function": best["function"], "description": best["description"],
            "origin": best["origin"], "evidence": evidence, "candidates": summary, "model": model_used,
            "original_function": original_function, "first_line": span[0], "security": events}


def describe(result):
    """A fix result as JSON text (for the command line and the logs)."""
    return json.dumps({k: v for k, v in result.items() if k != "source"}, indent=2)
