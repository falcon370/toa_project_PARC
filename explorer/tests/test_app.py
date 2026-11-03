"""Tests for the explorer's web API and its bridge to the compiler. They run the real parc binary, so build it first.

    python -m unittest discover -s explorer/tests -v
"""

import os
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
os.environ["PARC_AGENT_LLM"] = "none"  # the tests never talk to a language model

import app as explorer  # noqa: E402
import compiler_bridge as bridge  # noqa: E402
from agent import security  # noqa: E402

security.LOG_FILE = None  # the tests do not write to the real security log

BROKEN = 'fn main() {\n    let x: int = 1 +;\n    let s = "abc;\n}\n'
DIVIDE = "fn mean(sum: int, n: int) -> int {\n    return sum / n;\n}\n\nfn main() {\n    print(mean(10, 2));\n}\n"


def example(name):
    return (explorer.EXAMPLES / name).read_text(encoding="utf-8").replace("\r\n", "\n")


@unittest.skipUnless(bridge.PARC.is_file(), f"build the compiler first: {bridge.PARC} is missing")
class CompileTests(unittest.TestCase):
    def setUp(self):
        self.client = explorer.app.test_client()
        explorer.agent_calls.clear()

    def compile(self, source):
        return self.client.post("/api/compile", json={"source": source})

    def test_good_program_has_every_phase(self):
        result = self.compile(example(explorer.DEFAULT_EXAMPLE)).get_json()
        self.assertEqual(result["errors"], [])
        self.assertEqual(result["stage"], "complete")
        self.assertEqual(result["tokens"]["tokens"][0]["kind"], "FN")
        self.assertEqual(result["tokens"]["tokens"][-1]["kind"], "EOF")
        self.assertEqual([f["name"] for f in result["ast"]["functions"]], ["dot", "main"])
        for phase in ("symbols", "findings", "ir", "cfg", "dataflow", "ir_opt", "passes", "code"):
            self.assertIn(phase, result, phase)
        self.assertEqual([f["name"] for f in result["ir"]["functions"]], ["dot", "main"])
        self.assertIn("cyclomatic_complexity", result["cfg"]["functions"][0]["metrics"])
        self.assertTrue(any(f["kind"] == "divide-by-zero" for f in result["findings"]["findings"]))
        self.assertRegex(result["compile_id"], r"^[0-9a-f]{16}$")

    def test_every_example_compiles_without_errors(self):
        names = explorer.example_names()
        self.assertIn(explorer.DEFAULT_EXAMPLE, names)
        self.assertEqual(len(names), 20)
        for name in names:
            source = self.client.get(f"/api/examples/{name}").get_json()["source"]
            result = self.compile(source).get_json()
            self.assertEqual(result["errors"], [], name)
            self.assertEqual(result["stage"], "complete", name)

    def test_broken_program_lists_scanner_and_parser_errors_in_source_order(self):
        response = self.compile(BROKEN)
        self.assertEqual(response.status_code, 200)
        result = response.get_json()
        self.assertEqual([(e["line"], e["col"], e["phase"]) for e in result["errors"]],
                         [(2, 21, "parser"), (3, 11, "parser"), (3, 13, "scanner")])
        self.assertEqual(result["errors"][1]["hint"], "ParC needs a type: let x: int = 0;")
        self.assertEqual(result["stage"], "syntax")
        self.assertNotIn("ir", result)  # later phases did not run

    def test_semantic_errors_stop_before_the_ir(self):
        result = self.compile("fn main() { let x: int = true; print(y); }").get_json()
        self.assertEqual(result["stage"], "semantic")
        self.assertEqual([e["kind"] for e in result["errors"]], ["type-mismatch", "undeclared"])
        self.assertIn("symbols", result)
        self.assertNotIn("code", result)

    def test_tree_fields_keep_the_compilers_order(self):
        text = self.compile("fn f(n: int) { for i in 0..n { } }").get_data(as_text=True)
        self.assertLess(text.index('"start"'), text.index('"end"'))

    def test_source_size_limit_is_100_kb(self):
        self.assertEqual(self.compile("/" * explorer.MAX_SOURCE_BYTES).status_code, 200)
        too_big = self.compile("/" * (explorer.MAX_SOURCE_BYTES + 1))
        self.assertEqual(too_big.status_code, 413)
        self.assertIn("100 KB", too_big.get_json()["error"])
        # The limit counts bytes, and "é" is two bytes.
        self.assertEqual(self.compile("é" * (explorer.MAX_SOURCE_BYTES // 2 + 1)).status_code, 413)
        self.assertEqual(self.compile("x" * (5 * 1024 * 1024)).status_code, 413)

    def test_request_must_be_json_with_a_source_string(self):
        self.assertEqual(self.client.post("/api/compile", data="source=fn").status_code, 400)
        self.assertEqual(self.client.post("/api/compile", json={"source": 5}).status_code, 400)
        self.assertEqual(self.client.post("/api/compile", json=["fn"]).status_code, 400)

    def test_slow_compiler_is_reported_as_a_timeout(self):
        slow = subprocess.TimeoutExpired(cmd="parc", timeout=bridge.PARC_TIMEOUT_SECONDS)
        with mock.patch.object(bridge.subprocess, "run", side_effect=slow) as run:
            response = self.compile("fn main() { }")
        self.assertEqual(bridge.PARC_TIMEOUT_SECONDS, 5)
        self.assertLessEqual(run.call_args.kwargs["timeout"], 5)
        self.assertGreater(run.call_args.kwargs["timeout"], 4)
        self.assertEqual(response.status_code, 504)
        self.assertIn("5 seconds", response.get_json()["error"])

    def test_compiler_crash_is_reported(self):
        crashed = subprocess.CompletedProcess(args=[], returncode=3, stdout="", stderr="boom")
        with mock.patch.object(bridge.subprocess, "run", return_value=crashed):
            response = self.compile("fn main() { }")
        self.assertEqual(response.status_code, 500)
        self.assertIn("boom", response.get_json()["error"])

    def test_missing_compiler_is_explained(self):
        with mock.patch.object(bridge, "PARC", Path("no_such_parc.exe")):
            response = self.compile("fn main() { }")
        self.assertEqual(response.status_code, 500)
        self.assertIn("build it first", response.get_json()["error"])

    # ------------------------------------------------------------ /api/phase

    def test_a_phase_can_be_fetched_again_by_compile_id(self):
        compiled = self.compile(DIVIDE).get_json()
        again = self.client.get(f"/api/phase/ir?compile_id={compiled['compile_id']}")
        self.assertEqual(again.status_code, 200)
        self.assertEqual(again.get_json(), compiled["ir"])
        self.assertEqual(self.client.get(f"/api/phase/nonsense?compile_id={compiled['compile_id']}").status_code, 404)
        self.assertEqual(self.client.get("/api/phase/ir?compile_id=0000000000000000").status_code, 404)
        self.assertEqual(self.client.get("/api/phase/ir").status_code, 404)
        broken = self.compile(BROKEN).get_json()
        missing = self.client.get(f"/api/phase/ir?compile_id={broken['compile_id']}")
        self.assertEqual(missing.status_code, 404)
        self.assertIn("stopped before ir", missing.get_json()["error"])

    def test_only_the_latest_compiles_are_kept(self):
        first = self.compile("fn main() { }").get_json()["compile_id"]
        for _ in range(explorer.KEPT_COMPILES):
            self.compile("fn main() { }")
        self.assertEqual(self.client.get(f"/api/phase/tokens?compile_id={first}").status_code, 404)
        self.assertLessEqual(len(explorer.compiles), explorer.KEPT_COMPILES)

    # ------------------------------------------------------------ /api/run

    def test_run_gives_the_programs_output(self):
        compiled = self.compile(example("06_dot_parallel.parc")).get_json()
        result = self.client.post("/api/run", json={"compile_id": compiled["compile_id"]}).get_json()
        self.assertTrue(result["ok"])
        self.assertEqual(result["output"], "dot = 4\n")
        self.assertEqual(result["parallel_loops"], 1)
        self.assertEqual(result["trace"], [])
        # By source, with options.
        direct = self.client.post("/api/run", json={"source": "fn main() { print(6 * 7); }", "threads": 1}).get_json()
        self.assertEqual(direct["output"], "42\n")

    def test_run_reports_runtime_errors_and_events(self):
        stopped = self.client.post("/api/run", json={"source": example("04_cwe369_divzero.parc")}).get_json()
        self.assertFalse(stopped["ok"])
        self.assertEqual(stopped["error"]["message"], "division by zero")
        self.assertEqual(stopped["error"]["line"], 3)
        wrapped = self.client.post("/api/run", json={"source": example("02_cwe190_overflow.parc")}).get_json()
        self.assertTrue(wrapped["ok"])
        self.assertEqual(wrapped["overflows"], 1)
        self.assertEqual(wrapped["events"][0]["kind"], "overflow")

    def test_run_can_record_a_trace_and_force_a_race(self):
        traced = self.client.post("/api/run", json={"source": "fn main() { let x: int = 2; print(x + 1); }",
                                                    "trace": True, "optimize": False}).get_json()
        self.assertGreater(len(traced["trace"]), 2)
        self.assertEqual(traced["trace"][0]["function"], "main")
        self.assertIn("regs", traced["trace"][0])
        race = example("17_parallel_race.parc")
        safe = self.client.post("/api/run", json={"source": race}).get_json()
        self.assertEqual((safe["races"], safe["output"]), (0, "a[7] = 8 count = 8\n"))
        forced = self.client.post("/api/run", json={"source": race, "force_parallel": True}).get_json()
        self.assertGreater(forced["races"], 0)

    def test_run_of_a_program_with_errors_says_so(self):
        result = self.client.post("/api/run", json={"source": "fn main() { print(y); }"}).get_json()
        self.assertFalse(result["ok"])
        self.assertFalse(result["compiled"])
        self.assertIn("'y' is not declared", result["error"]["message"])

    def test_an_endless_program_is_stopped(self):
        with mock.patch.object(bridge, "PARC_TIMEOUT_SECONDS", 1):
            response = self.client.post("/api/run", json={"source": "fn main() { let i: int = 0; while (true) { i = i + 1; } }"})
        # Either the VM's own step limit or the 1-second limit ends it.
        if response.status_code == 200:
            self.assertIn("steps and was stopped", response.get_json()["error"]["message"])
        else:
            self.assertEqual(response.status_code, 504)

    # ------------------------------------------------------------ /api/metrics

    def test_metrics_measure_the_run_and_the_complexity(self):
        compiled = self.compile(example("09_fibonacci.parc")).get_json()
        result = self.client.post("/api/metrics", json={"compile_id": compiled["compile_id"], "min_ms": 20}).get_json()
        self.assertEqual(result["compile"]["phases"][0]["phase"], "scanner")
        self.assertTrue(result["vm"]["ran"])
        self.assertGreater(result["vm"]["cost"]["wall_ms"], 0)
        self.assertEqual(set(result["vm"]["function_steps"]), {"fib_loop", "fib_rec", "main"})
        if result["clang"]["ok"]:  # built with libclang
            functions = {f["name"]: f for f in result["complexity"]["functions"]}
            self.assertEqual(functions["fib_rec"]["time"], "O(2^n)")
            self.assertEqual(functions["fib_loop"]["time"], "O(n)")
            self.assertEqual(functions["fib_rec"]["suggestions"][0]["time_better"], "O(n)")
            self.assertTrue(result["native"]["ran"])
            self.assertTrue(result["native"]["same_output"])

    def test_metrics_of_a_program_with_syntax_errors_are_refused(self):
        response = self.client.post("/api/metrics", json={"source": BROKEN})
        self.assertEqual(response.status_code, 409)

    # ------------------------------------------------------------ /api/explain and /api/fix

    def agent(self, route, compile_id, finding_id, **more):
        return self.client.post(route, json={"compile_id": compile_id, "finding_id": finding_id, **more})

    def test_explain_cites_the_finding(self):
        compiled = self.compile(DIVIDE).get_json()
        finding = compiled["findings"]["findings"][0]
        self.assertEqual((finding["id"], finding["kind"]), ("F1", "divide-by-zero"))
        result = self.agent("/api/explain", compiled["compile_id"], "F1").get_json()
        self.assertEqual(result["source"], "template")
        self.assertIn("divides by zero when n is 0", result["explanation"])
        self.assertIn("[F1]", result["explanation"])
        self.assertEqual(result["cited"], ["F1"])
        engineer = self.agent("/api/explain", compiled["compile_id"], "F1", level="engineer").get_json()
        self.assertIn("CWE-369", engineer["explanation"])
        hint = self.agent("/api/explain", compiled["compile_id"], "F1", hint=True).get_json()
        self.assertTrue(hint["hint"])
        self.assertTrue(hint["explanation"].endswith("?"))
        self.assertNotIn("if (", hint["explanation"])  # a hint does not give the fix away

    def test_fix_returns_a_checked_rewrite(self):
        compiled = self.compile(DIVIDE).get_json()
        result = self.agent("/api/fix", compiled["compile_id"], "F1").get_json()
        self.assertTrue(result["fixed"])
        self.assertIn("if (n == 0) {", result["function"])
        self.assertIn("F1 is gone", result["evidence"])
        self.assertTrue(result["candidates"][0]["passed"])
        # The rewritten program really has no warning left.
        again = self.compile(result["source"]).get_json()
        self.assertEqual([f for f in again["findings"]["findings"] if f["severity"] == "warning"], [])

    def test_agent_requests_are_checked(self):
        compiled = self.compile(DIVIDE).get_json()
        self.assertEqual(self.agent("/api/explain", compiled["compile_id"], "F99").status_code, 404)
        self.assertEqual(self.agent("/api/fix", "0000000000000000", "F1").status_code, 404)
        self.assertEqual(self.client.post("/api/fix", json={"compile_id": compiled["compile_id"]}).status_code, 400)
        broken = self.compile(BROKEN).get_json()
        self.assertEqual(self.agent("/api/explain", broken["compile_id"], "F1").status_code, 409)

    def test_agent_requests_are_rate_limited(self):
        compiled = self.compile(DIVIDE).get_json()
        with mock.patch.object(explorer, "AGENT_CALLS_PER_MINUTE", 3):
            codes = [self.agent("/api/explain", compiled["compile_id"], "F1", hint=True).status_code for _ in range(5)]
        self.assertEqual(codes, [200, 200, 200, 429, 429])

    def test_agent_status_names_no_key(self):
        with mock.patch.dict(os.environ, {"GEMINI_API_KEY": "secret-key-value", "PARC_AGENT_LLM": "gemini"}):
            status = self.client.get("/api/agent").get_data(as_text=True)
        self.assertIn("gemini", status)
        self.assertNotIn("secret-key-value", status)


class PageAndExampleTests(unittest.TestCase):
    def setUp(self):
        self.client = explorer.app.test_client()

    def test_page_is_served(self):
        response = self.client.get("/")
        self.assertEqual(response.status_code, 200)
        self.assertIn(b"ParC explorer", response.data)
        response.close()

    def test_examples_are_listed_with_the_default(self):
        listing = self.client.get("/api/examples").get_json()
        self.assertEqual(listing["default"], "06_dot_parallel.parc")
        self.assertEqual(listing["examples"], sorted(listing["examples"]))

    def test_only_listed_example_names_are_served(self):
        for name in ("nope.parc", "..%5CCLAUDE.md", "..%2FCLAUDE.md", "%2e%2e%2fREADME.md"):
            self.assertEqual(self.client.get(f"/api/examples/{name}").status_code, 404, name)

    def test_other_host_names_are_refused(self):
        response = self.client.get("/api/examples", headers={"Host": "evil.example:5000"})
        self.assertEqual(response.status_code, 403)
        self.assertEqual(self.client.get("/api/examples", headers={"Host": "localhost:5000"}).status_code, 200)

    def test_server_listens_on_this_machine_only(self):
        self.assertEqual(explorer.HOST, "127.0.0.1")


if __name__ == "__main__":
    unittest.main()
