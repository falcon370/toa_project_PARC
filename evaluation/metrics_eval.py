"""Section 7 of the evaluation: what each example costs, and what its complexity can be reduced to.

All the measuring is done by the compiler ("parc metrics", in C++); this only runs it on every example
and writes the table.

    python evaluation/metrics_eval.py            print the section
    python evaluation/metrics_eval.py --update   also replace section 7 of evaluation/results.md

The times and energies change a little from run to run (other programs share the processor, and the
energy meter covers the whole processor package). The complexity bounds do not change.
"""

import math
import sys
from datetime import date
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "explorer"))

import compiler_bridge as bridge  # noqa: E402

EXAMPLES = sorted((ROOT / "examples").glob("*.parc"))
HEADING = "## 7. Cost: time, energy and complexity"


def fmt_time(ms):
    if ms is None or ms < 0:
        return "n/a"
    if ms >= 1:
        return f"{ms:.2f} ms"
    if ms >= 0.001:
        return f"{ms * 1000:.2f} µs"
    return f"{ms * 1e6:.0f} ns"


def fmt_energy(joules):
    if joules is None:
        return "n/a"
    if joules >= 1e-3:
        return f"{joules * 1e3:.2f} mJ"
    if joules >= 1e-6:
        return f"{joules * 1e6:.1f} µJ"
    return f"{joules * 1e9:.0f} nJ"


def growth(bound):
    """Orders bounds like "O(2^n)" > "O(n^3)" > "O(n^2)" > "O(n log n)" > "O(n)" > "O(1)"."""
    powers = [int(c) for i, c in enumerate(bound) if c.isdigit() and i > 0 and bound[i - 1] == "^"]
    return ("2^" in bound, max(powers, default=0 if bound == "O(1)" else 1), "log" in bound, len(bound))


def section():
    """Returns (lines, summary)."""
    rows, speedups, suggestions, energy_source = [], [], [], ""
    for path in EXAMPLES:
        result = bridge.metrics(path.read_text(encoding="utf-8"), min_ms=200)
        if result is None:
            continue
        energy_source = result["energy"]["source"] if result["energy"]["available"] else ""
        vm, native = result["vm"], result["native"]
        vm_cost = vm.get("cost", {}) if vm["ran"] else {}
        nat_cost = native.get("cost", {}) if native["ran"] else {}
        speedup = native.get("speedup") if native["ran"] else None
        if speedup:
            speedups.append(speedup)
        worst = max(result["complexity"]["functions"], key=lambda f: growth(f["time"]), default=None)
        for fn in result["complexity"]["functions"]:
            for s in fn["suggestions"]:
                suggestions.append((path.name, fn["name"], s))
        if not vm["ran"]:
            vm_text = "not run"
        elif vm["ok"]:
            vm_text = fmt_time(vm_cost.get("wall_ms"))
        else:
            vm_text = "stopped: " + vm["error"]
        rows.append(
            f"| {path.name} | {vm_text} | {fmt_time(nat_cost.get('wall_ms')) if nat_cost else 'not run'} "
            f"| {(f'{speedup:.0f}×' if speedup >= 10 else f'{speedup:.1f}×') if speedup else ''} "
            f"| {fmt_energy(vm_cost.get('energy_j')) if vm_cost.get('has_energy') else 'n/a'} "
            f"| {fmt_energy(nat_cost.get('energy_j')) if nat_cost.get('has_energy') else 'n/a'} "
            f"| {'same' if native.get('same_output') else ('different' if native['ran'] else '')} "
            f"| {worst['name'] + ': ' + worst['time'] if worst else ''} |")

    mean = math.exp(sum(math.log(s) for s in speedups) / len(speedups)) if speedups else 0
    lines = [HEADING, "",
             f"`parc metrics` on every example (`evaluation/metrics_eval.py`, measured on {date.today().isoformat()}). Each run is repeated until it has been "
             "measured for at least 200 ms, and the numbers are per run. *Native* is the same program translated to "
             "C and built with `clang -O2`; its process start-up is measured separately and left out. Energy is the "
             "processor package's, read from its RAPL meter"
             + (f" ({energy_source})" if energy_source else " (not available on the machine that ran this)")
             + ", so it includes whatever else the machine was doing. Times and energies vary a little between runs; "
             "the complexity bounds do not.", "",
             "| Example | VM time | Native time | Speed-up | VM energy | Native energy | Output | Largest bound |",
             "|---|---|---|---|---|---|---|---|", *rows, "",
             f"**Native code was {mean:.1f} times faster than the VM** (geometric mean over {len(speedups)} examples "
             "that ran without a runtime error). Most examples do little work and mostly print, and printing costs "
             "the same in both, so their speed-up is small; the two that compute (09_fibonacci, 10_primes_sieve) show "
             "the real gap between an interpreter and optimized machine code. Examples that stop with a runtime error "
             "on purpose are not run natively.",
             "", "### What the complexity can be reduced to", "",
             "The analysis reads Clang's syntax tree of each function. Where it recognises a rewrite that lowers a "
             "bound, it analyzes the function again with the change made and reports both bounds:", "",
             "| Example | Function | Line | Suggestion | Time now → after | Space now → after |", "|---|---|---|---|---|---|"]
    lines += [f"| {name} | {fn} | {s['line']} | {s['title']} | {s['time_now']} → {s['time_better']} "
              f"| {s['space_now']} → {s['space_better']} |" for name, fn, s in suggestions]
    lines += ["", "The tests in `compiler/metrics/tests/test_metrics.cpp` show the other patterns (prefix sums, a divisor "
              "search stopped at the square root, a costly call moved out of a loop, an allocation moved out of a "
              "loop) on small programs written for them.", ""]
    summary = f"native {mean:.0f}x faster (geometric mean), {len(suggestions)} suggestions"
    return lines, summary


def update_results(lines):
    path = HERE / "results.md"
    text = path.read_text(encoding="utf-8") if path.is_file() else "# ParC evaluation results\n"
    at = text.find(HEADING)
    if at >= 0:
        text = text[:at]
    path.write_text(text.rstrip("\n") + "\n\n" + "\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    section_lines, text = section()
    print("\n".join(section_lines))
    print("7. cost:", text)
    if "--update" in sys.argv:
        update_results(section_lines)
        print("updated evaluation/results.md")
