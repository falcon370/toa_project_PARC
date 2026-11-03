"""Tests for the explain-and-fix agent: templates, the compiler gate, and the model path.

No language model is contacted. Where a test needs one, a scripted stand-in
answers, which also shows exactly what the agent does with good and bad answers.

    python -m unittest discover -s explorer/tests -v
"""

import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
os.environ["PARC_AGENT_LLM"] = "none"

import compiler_bridge as bridge  # noqa: E402
from agent import agent, gate, llm, security, templates  # noqa: E402

security.LOG_FILE = None  # the tests do not write to the real security log

MEAN = """fn mean(sum: int, n: int) -> int {
    return sum / n;
}

fn main() {
    print(mean(10, 2));
}
"""

GUARDED = """fn mean(sum: int, n: int) -> int {
    if (n == 0) {
        return 0;
    }
    return sum / n;
}"""


class Scripted:
    """A stand-in language model that gives prepared answers and records what it was asked."""

    name = "scripted"

    def __init__(self, *answers):
        self.answers = list(answers)
        self.prompts = []

    def complete(self, system, prompt):
        self.prompts.append((system, prompt))
        if not self.answers:
            raise llm.ModelError("no more answers")
        return self.answers.pop(0)


def fenced(*functions):
    return "\n".join(f"Version {i}:\n```parc\n{text}\n```" for i, text in enumerate(functions, 1))


def no_cache(test):
    """Runs a test with an empty model cache, so scripted answers are not mixed up between tests."""
    def wrapper(self):
        with tempfile.TemporaryDirectory() as folder, mock.patch.object(llm, "CACHE", Path(folder)):
            test(self)
    return wrapper


@unittest.skipUnless(bridge.PARC.is_file(), f"build the compiler first: {bridge.PARC} is missing")
class GateTests(unittest.TestCase):
    def setUp(self):
        self.build = bridge.build(MEAN)
        self.finding = self.build["findings"]["findings"][0]

    def judge(self, new_function, source=MEAN):
        return gate.judge(source, self.build, self.finding, gate.replace_function(source, "mean", new_function))

    def test_function_span_matches_braces_and_skips_strings_and_comments(self):
        source = 'fn a() {\n    print("}");  // }\n}\n\nfn b(n: int) -> int {\n    if (n > 0) { return 1; }\n    return 0;\n}\n'
        self.assertEqual(gate.function_span(source, "a"), (1, 3))
        self.assertEqual(gate.function_span(source, "b"), (5, 8))
        self.assertIsNone(gate.function_span(source, "c"))
        self.assertEqual(gate.function_text(source, "a").split("\n")[-1], "}")

    def test_a_correct_guard_passes_every_check(self):
        verdict = self.judge(GUARDED)
        self.assertTrue(verdict["passed"], verdict)
        self.assertEqual(verdict["warnings"], 0)
        self.assertEqual(verdict["diff"], 3)
        self.assertIn("F1 is gone", verdict["evidence"])
        self.assertIn("the program prints exactly the same as before", verdict["evidence"])
        self.assertTrue(any("test inputs where the original works" in e for e in verdict["evidence"]))
        self.assertTrue(any("made the original fail now work" in e for e in verdict["evidence"]))

    def test_a_rewrite_that_does_not_compile_is_rejected(self):
        verdict = self.judge("fn mean(sum: int, n: int) -> int {\n    return sum / ;\n}")
        self.assertEqual(verdict["failed_at"], "compiles")
        self.assertIn("does not compile", verdict["reason"])

    def test_a_rewrite_that_keeps_the_problem_is_rejected(self):
        # A check of the wrong variable.
        verdict = self.judge("fn mean(sum: int, n: int) -> int {\n    if (sum == 0) {\n        return 0;\n    }\n    return sum / n;\n}")
        self.assertEqual(verdict["failed_at"], "fixed")
        self.assertIn("divides by zero", verdict["reason"])

    def test_a_rewrite_that_adds_a_warning_is_rejected(self):
        verdict = self.judge("fn mean(sum: int, n: int) -> int {\n    let extra: int = 1;\n    if (n == 0) {\n        return 0;\n    }\n    return sum / n;\n}")
        self.assertEqual(verdict["failed_at"], "no harm")
        self.assertIn("'extra' is never used", verdict["reason"])

    def test_a_rewrite_that_changes_the_result_is_rejected(self):
        # The warning is gone, but so is the division: the function no longer does its job.
        verdict = self.judge("fn mean(sum: int, n: int) -> int {\n    return sum;\n}")
        self.assertEqual(verdict["failed_at"], "behaviour")
        self.assertIn("prints something different", verdict["reason"])
        # Right on main's one call, wrong on other inputs: only the per-function tests catch this.
        verdict = self.judge("fn mean(sum: int, n: int) -> int {\n    if (n == 2) {\n        return sum / 2;\n    }\n    return 0;\n}")
        self.assertEqual(verdict["failed_at"], "behaviour")
        self.assertIn("for the arguments", verdict["reason"])

    def test_changing_another_function_is_out_of_scope(self):
        candidate = gate.replace_function(MEAN, "mean", GUARDED).replace("print(mean(10, 2));", "print(5);")
        verdict = gate.judge(MEAN, self.build, self.finding, candidate)
        self.assertEqual(verdict["failed_at"], "scope")
        self.assertEqual(gate.judge(MEAN, self.build, self.finding, MEAN)["failed_at"], "scope")  # unchanged

    def test_changing_the_parameters_is_rejected(self):
        verdict = self.judge("fn mean(sum: int, n: int, extra: int) -> int {\n    if (n == 0) {\n        return 0;\n    }\n    return sum / n;\n}")
        self.assertFalse(verdict["passed"])

    def test_test_inputs_are_fixed_and_typed(self):
        inputs = gate.test_inputs(["int", "int[]", "bool", "float"])
        self.assertEqual(inputs, gate.test_inputs(["int", "int[]", "bool", "float"]))  # same every time
        self.assertEqual(len(inputs), gate.INPUTS_PER_FUNCTION)
        for line in inputs:
            number, array, flag, real = line.split(" ")
            int(number)
            self.assertTrue(array == "null" or (array.startswith("[") and array.endswith("]")))
            self.assertIn(flag, ("true", "false"))
            float(real)

    def test_ranking_prefers_passing_then_fewer_warnings_then_smaller_changes(self):
        def candidate(passed, warnings, diff):
            return {"verdict": {"passed": passed, "warnings": warnings, "diff": diff}}
        ranked = gate.rank([candidate(False, 0, 1), candidate(True, 2, 1), candidate(True, 0, 9), candidate(True, 0, 3)])
        self.assertEqual([(c["verdict"]["passed"], c["verdict"]["warnings"], c["verdict"]["diff"]) for c in ranked],
                         [(True, 0, 3), (True, 0, 9), (True, 2, 1), (False, 0, 1)])


@unittest.skipUnless(bridge.PARC.is_file(), f"build the compiler first: {bridge.PARC} is missing")
class TemplateTests(unittest.TestCase):
    def fix(self, source, kind):
        build = bridge.build(source)
        finding = next(f for f in build["findings"]["findings"] if f["kind"] == kind)
        return agent.fix(source, build, finding["id"], models=[])

    def test_explanations_come_from_the_findings_fields(self):
        build = bridge.build(MEAN)
        finding = build["findings"]["findings"][0]
        student = templates.explanation(finding, "student")
        self.assertIn("Line 2: 'sum / n' divides by zero when n is 0. [F1]", student)
        self.assertIn("n is a parameter, so it can be any int", student)
        self.assertEqual(student.count("[F1]"), 4)  # every sentence cites the fact
        self.assertIn("CWE-369", templates.explanation(finding, "engineer"))
        self.assertEqual(templates.question(finding), "What value can n have here, and what does line 2 do with that value?")

    def test_every_kind_of_finding_has_an_explanation_and_a_question(self):
        for kind, info in templates.KINDS.items():
            finding = {"id": "F1", "kind": kind, "line": 3, "message": "m", "subject": "x"}
            self.assertIn("[F1]", templates.explanation(finding), kind)
            self.assertTrue(templates.question(finding).endswith("?"), kind)
            self.assertEqual(set(info), {"student", "engineer", "question"}, kind)

    def test_guard_fixes(self):
        for source, kind, expected in [
            (MEAN, "divide-by-zero", "if (n == 0) {"),
            ("fn bump(x: int) -> int {\n    return x + 1000;\n}\nfn main() { print(bump(1)); }\n", "overflow", "if (x > 2147483647 - 1000) {"),
            ("fn first(a: int[]) -> int {\n    return a[0];\n}\nfn main() { print(first(null)); }\n", "null-access", "if (a == null) {"),
        ]:
            result = self.fix(source, kind)
            self.assertTrue(result["fixed"], result.get("reason"))
            self.assertIn(expected, result["function"])
            self.assertEqual(result["origin"], "template")

    def test_a_void_function_returns_nothing_from_its_guard(self):
        result = self.fix("fn show(a: int, b: int) {\n    print(a / b);\n}\nfn main() { show(6, 3); }\n", "divide-by-zero")
        self.assertTrue(result["fixed"])
        self.assertIn("        return;\n", result["function"])

    def test_cleanup_fixes(self):
        source = ("fn f(flag: bool) -> int {\n    let unused: int = 42;\n    let value: int;\n    if (flag) {\n        value = 7;\n    }\n"
                  "    return value;\n    print(1);\n}\nfn main() { print(f(true)); }\n")
        removed = self.fix(source, "unused-variable")
        self.assertTrue(removed["fixed"])
        self.assertNotIn("unused", removed["function"])
        initialized = self.fix(source, "uninitialized")
        self.assertTrue(initialized["fixed"])
        self.assertIn("let value: int = 0;", initialized["function"])
        reachable = self.fix(source, "unreachable-code")
        self.assertTrue(reachable["fixed"])
        self.assertNotIn("print(1);", reachable["function"])

    def test_loop_fixes(self):
        racy = ("fn main() {\n    let a: int[] = new int[8];\n    let count: int = 0;\n    parallel for i in 0..8 {\n"
                "        count = count + 1;\n    }\n    print(count, len(a));\n}\n")
        result = self.fix(racy, "parallel-dependence")
        self.assertTrue(result["fixed"])
        # Two candidates pass (declare the reduction, or make it serial); the smaller change wins.
        self.assertEqual(sum(1 for c in result["candidates"] if c["passed"]), 2)
        self.assertIn("reduce(+: count)", result["function"])
        serial = "fn main() {\n    let a: int[] = new int[8];\n    for i in 0..8 {\n        a[i] = i * 2;\n    }\n    print(a[7]);\n}\n"
        result = self.fix(serial, "parallelizable")
        self.assertTrue(result["fixed"])
        self.assertIn("parallel for i in 0..8 {", result["function"])

    def test_no_template_and_no_model_is_reported_plainly(self):
        result = self.fix("fn f(n: int) -> int {\n    if (n > 0) {\n        let n: int = 5;\n        return n;\n    }\n    return 0;\n}\nfn main() { print(f(1)); }\n",
                          "shadowing")
        self.assertFalse(result["fixed"])
        self.assertIn("no language model is set up", result["reason"])

    def test_a_template_that_cannot_work_is_reported_with_the_reason(self):
        # The guard would need to know the array's contents, which the compiler does not track.
        source = "fn f(a: int[]) -> int {\n    if (a == null) { return 0; }\n    if (len(a) < 1) { return 0; }\n    return a[0] + 1;\n}\nfn main() { print(f(new int[2])); }\n"
        result = self.fix(source, "overflow")
        self.assertFalse(result["fixed"])
        self.assertIn("the compiler still reports it", result["reason"])
        self.assertEqual(result["candidates"][0]["failed_at"], "fixed")


@unittest.skipUnless(bridge.PARC.is_file(), f"build the compiler first: {bridge.PARC} is missing")
class GateHardeningTests(unittest.TestCase):
    """Gaps in the gate that the simulated attacks on the agent found."""

    def test_a_rewrite_that_never_returns_where_the_original_failed_is_refused(self):
        # When n is 0 the original stops with an error, so the gate has no result to
        # compare with there. Looping forever must still not count as a fix.
        build = bridge.build(MEAN)
        looping = MEAN.replace("    return sum / n;", "    if (n == 0) {\n        while (true) {\n        }\n    }\n    return sum / n;")
        verdict = gate.judge(MEAN, build, build["findings"]["findings"][0], looping)
        self.assertFalse(verdict["passed"])
        self.assertEqual(verdict["failed_at"], "behaviour")
        self.assertIn("never returns", verdict["reason"])


@unittest.skipUnless(bridge.PARC.is_file(), f"build the compiler first: {bridge.PARC} is missing")
class ModelPathTests(unittest.TestCase):
    def setUp(self):
        self.build = bridge.build(MEAN)

    def test_grounding_drops_sentences_without_a_valid_citation(self):
        text = ("The division on line 2 fails when n is 0 [F1]. This happens for an empty list. [F1] "
                "The function is also slow. It leaks memory [F7]. Use a check!")
        kept, dropped = agent.grounded(text, {"F1"})
        self.assertEqual(kept, ["The division on line 2 fails when n is 0 [F1].", "This happens for an empty list [F1]."])
        self.assertEqual(dropped, 3)

    @no_cache
    def test_explain_uses_the_model_and_keeps_only_grounded_sentences(self):
        model = Scripted("When n is 0 the division has no answer [F1]. Your code is ugly. The compiler proved n can be 0 [F1].")
        result = agent.explain(MEAN, self.build, "F1", models=[model])
        self.assertEqual(result["source"], "scripted")
        self.assertEqual(result["explanation"], "When n is 0 the division has no answer [F1]. The compiler proved n can be 0 [F1].")
        self.assertEqual(result["dropped"], 1)
        system, prompt = model.prompts[0]
        self.assertIn("student", system)
        # The model sees the function with line numbers and the facts, not the JSON dumps.
        self.assertIn("   2 |     return sum / n;", prompt)
        self.assertIn("F1 (semantic, line 2, divide-by-zero)", prompt)
        self.assertNotIn("tokens", prompt)
        self.assertNotIn("fn main", prompt)  # only the flagged function is sent

    @no_cache
    def test_explain_shows_names_as_plain_text(self):
        result = agent.explain(MEAN, self.build, "F1", models=[Scripted("The value $n$ can be 0 in `sum / n` [F1].")])
        self.assertEqual(result["explanation"], "The value n can be 0 in sum / n [F1].")

    @no_cache
    def test_explain_falls_back_to_the_template_when_nothing_is_grounded(self):
        result = agent.explain(MEAN, self.build, "F1", models=[Scripted("It is broken. Fix it.")])
        self.assertEqual(result["source"], "template")
        self.assertIn("[F1]", result["explanation"])
        failing = agent.explain(MEAN, self.build, "F1", models=[Scripted()])  # the model errors out
        self.assertEqual(failing["source"], "template")

    @no_cache
    def test_fix_judges_the_models_candidates_and_picks_the_smallest_passing_one(self):
        longer = GUARDED.replace("    return sum / n;", "    let result: int = sum / n;\n    return result;")
        broken = "fn mean(sum: int, n: int) -> int {\n    return sum / n\n}"
        model = Scripted(fenced(broken, longer))
        result = agent.fix(MEAN, self.build, "F1", models=[model])
        self.assertTrue(result["fixed"])
        by_origin = {(c["origin"], c["passed"]) for c in result["candidates"]}
        self.assertEqual(by_origin, {("template", True), ("scripted", True), ("scripted", False)})
        self.assertEqual(result["origin"], "template")  # 3 changed lines beat 5
        self.assertIn("2 of 3 candidates passed", result["evidence"][-1])
        self.assertEqual(result["model"], "scripted")

    @no_cache
    def test_failed_candidates_go_back_to_the_model_with_the_reason(self):
        shadow = "fn f(n: int) -> int {\n    if (n > 0) {\n        let n: int = 5;\n        return n;\n    }\n    return 0;\n}\nfn main() { print(f(1), f(0)); }\n"
        build = bridge.build(shadow)
        finding = next(f for f in build["findings"]["findings"] if f["kind"] == "shadowing")
        wrong = "fn f(n: int) -> int {\n    if (n > 0) {\n        return 6;\n    }\n    return 0;\n}"    # changes the result
        right = "fn f(n: int) -> int {\n    if (n > 0) {\n        let five: int = 5;\n        return five;\n    }\n    return 0;\n}"
        model = Scripted(fenced(wrong), fenced(right))
        result = agent.fix(shadow, build, finding["id"], models=[model])
        self.assertTrue(result["fixed"])
        self.assertIn("let five: int = 5;", result["function"])
        self.assertEqual(len(model.prompts), 2)
        retry = model.prompts[1][1]
        self.assertIn("did not pass the compiler's checks", retry)
        self.assertIn("rejected at the 'behaviour' check", retry)

    @no_cache
    def test_the_model_can_ask_a_tool_before_answering(self):
        model = Scripted("TOOL: show_ir", "TOOL: report", fenced(GUARDED))
        result = agent.fix(MEAN, self.build, "F1", models=[model])
        self.assertTrue(result["fixed"])
        self.assertEqual(len(model.prompts), 3)
        self.assertIn("RESULT:\nentry:\n    t1 = sum / n", model.prompts[1][1])  # the IR it asked for
        self.assertIn("F1 (semantic, line 2, divide-by-zero)", model.prompts[2][1])

    @no_cache
    def test_a_model_answer_that_touches_other_functions_is_refused(self):
        sneaky = GUARDED + "\n\nfn main() {\n    print(0);\n}"
        shadow_free = Scripted(fenced(sneaky), fenced(sneaky), fenced(sneaky))
        result = agent.fix(MEAN, self.build, "F1", models=[shadow_free])
        # The model's answers never reached the gate: the security layer refused each one.
        refused = [c for c in result["candidates"] if c["origin"] == "scripted"]
        self.assertEqual([c["failed_at"] for c in refused], ["security"] * 3)
        self.assertTrue(result["fixed"])
        self.assertEqual(result["origin"], "template")
        self.assertIn("refused by the security check: defines mean, main", shadow_free.prompts[1][1])
        self.assertEqual({e["attack"] for e in result["security"]}, {"scope-escape"})

    @no_cache
    def test_without_the_security_layer_the_scope_check_still_refuses_it(self):
        sneaky = GUARDED + "\n\nfn main() {\n    print(0);\n}"
        model = Scripted(fenced(sneaky), fenced(sneaky), fenced(sneaky))
        with mock.patch.dict(os.environ, {"PARC_AGENT_SECURITY": "off"}):
            result = agent.fix(MEAN, self.build, "F1", models=[model])
        self.assertEqual([c["origin"] for c in result["candidates"]], ["template"])
        self.assertIn("must contain exactly the function 'mean'", model.prompts[1][1])
        self.assertEqual(result["security"], [])

    @no_cache
    def test_answers_are_cached_by_request(self):
        first = Scripted("Dividing by zero stops the program [F1].")
        agent.explain(MEAN, self.build, "F1", models=[first])
        second = Scripted("A different answer [F1].")
        again = agent.explain(MEAN, self.build, "F1", models=[second])
        self.assertEqual(again["explanation"], "Dividing by zero stops the program [F1].")
        self.assertEqual(second.prompts, [])  # the model was not asked again

    def test_model_choice_follows_the_settings(self):
        with mock.patch.dict(os.environ, {"PARC_AGENT_LLM": "none", "GEMINI_API_KEY": "k"}):
            self.assertEqual(llm.available_models(), [])
            self.assertEqual(llm.status()["model"], "none")
        with mock.patch.dict(os.environ, {"PARC_AGENT_LLM": "gemini", "GEMINI_API_KEY": "k", "GEMINI_MODEL": "some-model"}):
            self.assertEqual([m.name for m in llm.available_models()], ["gemini (some-model)"])
        with mock.patch.dict(os.environ, {"PARC_AGENT_LLM": "ollama", "OLLAMA_MODEL": "tiny"}):
            self.assertEqual([m.name for m in llm.available_models()], ["ollama (tiny)"])

    def test_a_failing_model_falls_through_to_the_next(self):
        text, name = llm.complete("s", "p", models=[Scripted(), Scripted("answer")], use_cache=False)
        self.assertEqual((text, name), ("answer", "scripted"))
        self.assertEqual(llm.complete("s", "p", models=[Scripted()], use_cache=False), (None, "none"))

    def test_env_file_is_read_without_overriding(self):
        with tempfile.TemporaryDirectory() as folder:
            env = Path(folder) / ".env"
            env.write_text('# comment\nPARC_TEST_A="from file"\nPARC_TEST_B=file\n', encoding="utf-8")
            with mock.patch.dict(os.environ, {"PARC_TEST_B": "already set"}):
                llm.load_env_file(env)
                self.assertEqual(os.environ["PARC_TEST_A"], "from file")
                self.assertEqual(os.environ["PARC_TEST_B"], "already set")
                del os.environ["PARC_TEST_A"]


if __name__ == "__main__":
    unittest.main()
