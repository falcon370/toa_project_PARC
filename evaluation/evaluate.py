"""The automatic part of the evaluation. Writes evaluation/results.md.

    python evaluation/evaluate.py            everything
    python evaluation/evaluate.py --quick    fewer random programs

Seven measurements, each answering one question:

  1. Same output as Clang?   Every example, and random programs, are translated to C,
                             compiled by Clang, and must print what ParC's VM prints.
  2. Same output with and    The optimizer, the register allocator and the threads must
     without the optimizer?  not change what a program prints (evaluation/fuzz.py).
  3. Which warnings does     ParC's findings next to Clang's -Wall -Wextra and its
     ParC find that Clang    static analyzer, on the same programs.
     does not?
  4. How many warnings get   For every warning: did the agent produce a rewrite that
     a checked fix?          passed the compiler gate? (Templates only, so it is repeatable.)
  5. Can the agent be        Simulated attacks on the agent (prompt injection, a lying
     attacked?               model, a fix with something hidden in it): how many are
                             detected, and how many succeed (evaluation/security_eval.py).
  6. Do the scanner and      Clang's lexer on the same ParC text must give the same tokens,
     the parser agree with   and Clang's parser the same tree for the same expressions
     Clang?                  (evaluation/clang_front.py).
  7. What does each program  Time, energy and memory on the VM and natively (clang -O2), and
     cost?                   each function's complexity with what it can be reduced to
                             ("parc metrics", evaluation/metrics_eval.py).

Clang is found at C:\\Program Files\\LLVM\\bin or on PATH. Without it, parts 1 and 3 are skipped.
"""

import os
import random
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "explorer"))
os.environ["PARC_AGENT_LLM"] = "none"

import compiler_bridge as bridge  # noqa: E402
import clang_front  # noqa: E402
import fuzz  # noqa: E402
import metrics_eval  # noqa: E402
import security_eval  # noqa: E402
import to_c  # noqa: E402
from agent import agent  # noqa: E402

PARC = ROOT / "build" / "msvc" / "Release" / "parc.exe"
EXAMPLES = sorted((ROOT / "examples").glob("*.parc"))


def find_clang():
    known = Path(r"C:\Program Files\LLVM\bin\clang.exe")
    return str(known) if known.is_file() else shutil.which("clang")


CLANG = find_clang()


def vm_run(path):
    done = subprocess.run([str(PARC), "run", str(path), "-O0", "--threads", "1", "--max-steps", "5000000"],
                          capture_output=True, text=True, timeout=60)
    return done.returncode == 0, done.stdout  # False: it stopped with a runtime error


def clang_run(c_text, folder):
    """Compiles C text with Clang and runs it. Returns (ok, output or the compiler's message)."""
    c_file = Path(folder) / "program.c"
    exe = Path(folder) / "program.exe"
    c_file.write_text(c_text, encoding="utf-8")
    # -fwrapv: int overflow wraps around, as in ParC's VM. -O1 so that Clang's own optimizer takes part.
    built = subprocess.run([CLANG, "-std=c11", "-O1", "-fwrapv", "-w", str(c_file), "-o", str(exe)],
                           capture_output=True, text=True, timeout=120)
    if built.returncode != 0:
        return False, built.stderr[:400]
    done = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    return done.returncode == 0, done.stdout


def compare_with_clang(sources):
    """[(name, source)] -> (agree, skipped, [(name, reason)])."""
    agree, skipped, different = 0, 0, []
    with tempfile.TemporaryDirectory(prefix="parc_eval_") as folder:
        for name, source in sources:
            path = Path(folder) / "program.parc"
            path.write_text(source, encoding="utf-8")
            clean, expected = vm_run(path)
            if not clean:
                skipped += 1  # the program stops with a runtime error: C has no defined result to compare with
                continue
            ok, got = clang_run(to_c.translate_source(source), folder)
            if ok and got == expected:
                agree += 1
            else:
                different.append((name, "Clang could not build or run it: " + got[:200] if not ok
                                  else f"VM printed {expected[:60]!r}, Clang's program printed {got[:60]!r}"))
    return agree, skipped, different


def random_programs(count, seed):
    """Random programs whose functions do not print, so that C's freedom to
    evaluate operands in any order cannot change the output."""
    rng = random.Random(seed)
    out = []
    for n in range(count):
        generator = fuzz.Generator(rng)
        generator.quiet_functions = True
        out.append((f"random program {n} (seed {seed})", generator.program()))
    return out


def clang_warnings(source, folder):
    """What Clang says about the C version: (warnings, analyzer findings), as short texts."""
    c_file = Path(folder) / "program.c"
    c_file.write_text(to_c.translate_source(source, default_values=False), encoding="utf-8")

    def messages(arguments):
        done = subprocess.run([CLANG, *arguments, str(c_file)], capture_output=True, text=True, timeout=120, cwd=folder)
        found = []
        for line in done.stderr.splitlines():
            if " warning: " in line:
                text = line.split(" warning: ", 1)[1]
                if text not in found:
                    found.append(text)
        return found

    return (messages(["-std=c11", "-fsyntax-only", "-Wall", "-Wextra", "-Wno-unused-function"]),
            messages(["-std=c11", "--analyze", "-Wno-unused-function"]))


def main():
    quick = "--quick" in sys.argv
    if not PARC.is_file():
        sys.exit(f"build the compiler first: {PARC} is missing")
    started = time.time()
    out = ["# ParC evaluation results", "",
           f"Written by `python evaluation/evaluate.py` on {time.strftime('%Y-%m-%d')}. "
           "Run it again after changing the compiler; do not edit this file by hand.", ""]
    examples = [(p.name, p.read_text(encoding="utf-8").replace("\r\n", "\n")) for p in EXAMPLES]

    # ---------------------------------------------------------------- 1. differential testing against Clang
    out += ["## 1. Same output as Clang", ""]
    if CLANG:
        version = subprocess.run([CLANG, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
        count = 60 if quick else 300
        agree, skipped, different = compare_with_clang(examples)
        r_agree, r_skipped, r_different = compare_with_clang(random_programs(count, 11))
        out += [f"Each program is translated to C (`evaluation/to_c.py`), built with `{version}` at `-O1 -fwrapv`, "
                "and must print exactly what ParC's VM prints. `-fwrapv` makes int overflow wrap around in C as it does "
                "on the VM. Programs that stop with a runtime error on the VM are skipped, because C gives those no "
                "defined result.", "",
                "| Programs | Compared | Same output | Different | Skipped |", "|---|---|---|---|---|",
                f"| The {len(examples)} examples | {agree + len(different)} | {agree} | {len(different)} | {skipped} |",
                f"| {count} random programs | {r_agree + len(r_different)} | {r_agree} | {len(r_different)} | {r_skipped} |", ""]
        for name, reason in different + r_different:
            out.append(f"- **{name}**: {reason}")
        print(f"1. Clang: examples {agree} agree, {len(different)} differ, {skipped} skipped; "
              f"random {r_agree} agree, {len(r_different)} differ, {r_skipped} skipped")
    else:
        out += ["Skipped: Clang was not found.", ""]

    # ---------------------------------------------------------------- 2. the compiler against itself
    out += ["", "## 2. Same output in every configuration", ""]
    count = 100 if quick else 500
    done = subprocess.run([sys.executable, str(HERE / "fuzz.py"), str(count), "21"], capture_output=True, text=True)
    out += ["Random programs, each run four ways (optimizer on and off, 12 and 4 or 5 registers, 1 to 4 threads); "
            "all four must print the same.", "", "```", done.stdout.strip(), "```", ""]
    print("2.", done.stdout.strip().splitlines()[-1])

    # ---------------------------------------------------------------- 3. warnings: ParC and Clang
    out += ["## 3. Warnings: ParC and Clang on the same programs", ""]
    builds = {name: bridge.build(source) for name, source in examples}
    if CLANG:
        out += ["Clang sees the C translation, with `-Wall -Wextra` and with its static analyzer (`--analyze`). "
                "ParC's column lists its warnings (not its notes). Two things to keep in mind when reading the table: "
                "a `parallel for` is a plain loop in the C version, so Clang has no parallel loop to judge; and the C "
                "version calls the functions the same way the ParC program does, so Clang's analyzer, which follows "
                "calls, sees the same argument values. Clang's memory-leak and extra-parentheses warnings are about the "
                "translation (ParC has no `free`, and the translator puts parentheses around every operation), not about "
                "the ParC program.", "",
                "| Example | ParC warnings | Clang -Wall -Wextra | Clang --analyze |", "|---|---|---|---|"]
        with tempfile.TemporaryDirectory(prefix="parc_eval_") as folder:
            totals = [0, 0, 0]
            for name, source in examples:
                ours = [f"{f['kind']} (line {f['line']})" for f in builds[name]["findings"]["findings"] if f["severity"] == "warning"]
                wall, analyzer = clang_warnings(source, folder)
                totals = [totals[0] + len(ours), totals[1] + len(wall), totals[2] + len(analyzer)]
                cell = lambda items: "<br>".join(i.replace("|", "\\|") for i in items) or "none"
                out.append(f"| {name} | {cell(ours)} | {cell(wall)} | {cell(analyzer)} |")
        out += [f"| **Total** | **{totals[0]}** | **{totals[1]}** | **{totals[2]}** |", ""]
        print(f"3. warnings: ParC {totals[0]}, clang -Wall {totals[1]}, clang --analyze {totals[2]}")
    else:
        out += ["Skipped: Clang was not found.", ""]

    # ---------------------------------------------------------------- 4. the agent
    out += ["## 4. Warnings with a checked fix", "",
            "For every warning in the examples the agent is asked for a fix, using its templates only (no language "
            "model), so the numbers are the same on every run. A fix counts only if it passed the compiler gate: "
            "it compiles, the warning is gone, no new warning appeared, and the function behaves the same on the "
            "test inputs where the original worked.", "",
            "| Example | Finding | Result |", "|---|---|---|"]
    fixed = total = 0
    by_kind = {}
    for name, source in examples:
        for finding in builds[name]["findings"]["findings"]:
            if finding["severity"] != "warning":
                continue
            total += 1
            result = agent.fix(source, builds[name], finding["id"], models=[])
            ok = bool(result.get("fixed"))
            fixed += ok
            stats = by_kind.setdefault(finding["kind"], [0, 0])
            stats[0] += ok
            stats[1] += 1
            text = "fixed: " + result["description"] if ok else "no checked fix: " + result["reason"].split(":")[0]
            out.append(f"| {name} | {finding['kind']} (line {finding['line']}) | {text} |")
    out += ["", f"**{fixed} of {total} warnings got a checked fix.** By kind:", "",
            "| Kind | Fixed | Of |", "|---|---|---|"]
    out += [f"| {kind} | {stats[0]} | {stats[1]} |" for kind, stats in sorted(by_kind.items())]
    out += ["", "A language model adds candidates for the kinds that have no template (for example renaming a "
            "variable that hides another); those are judged by the same gate.", ""]
    print(f"4. agent: {fixed} of {total} warnings fixed")

    # ---------------------------------------------------------------- 5. attacks on the agent
    lines, numbers = security_eval.section()
    out += lines
    print(f"5. security: precision {numbers['precision']:.2f}, recall {numbers['recall']:.2f}, "
          f"false-positive rate {numbers['false_positive_rate']:.2f}; attacks that succeed: "
          f"{numbers['asr_gate_only']:.0%} with the gate only, {numbers['asr_with_layer']:.0%} with the security layer")

    # ---------------------------------------------------------------- 6. the front end against Clang
    lines, summary = clang_front.section(quick)
    out += lines
    print("6. front end:", summary)

    # ---------------------------------------------------------------- 7. cost and complexity
    lines, summary = metrics_eval.section()
    out += lines
    print("7. cost:", summary)

    (HERE / "results.md").write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"wrote evaluation/results.md in {time.time() - started:.0f} s")


if __name__ == "__main__":
    main()
