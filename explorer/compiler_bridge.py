"""The explorer's only link to the compiler: runs the parc binary and reads the JSON it writes.

Three things are offered, one per parc command:

    build(source)    every phase's output: tokens, tree, symbols, findings, IR, graph, passes, VM code
    run(source)      the program's output on the VM, with events, schedule and an optional trace
    call(source, function, inputs)   one function run on many inputs (used by the agent's gate)
    metrics(source)  time, energy and memory of compiling and running, the clang -O2 build, and the
                     complexity of each function (all measured by parc in C++; this only reads the JSON)
"""

import json
import os
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PARC = Path(os.environ.get("PARC_BIN") or ROOT / "build" / "msvc" / "Release" / "parc.exe")
PARC_TIMEOUT_SECONDS = 5
# "parc metrics" compiles the program with clang and repeats each run until it has been measured long enough.
METRICS_TIMEOUT_SECONDS = 90

# The files "parc build" can write, by the name the explorer uses for each.
PHASE_FILES = {
    "tokens": "tokens.json",
    "ast": "ast.json",
    "symbols": "symbols.json",
    "findings": "findings.json",
    "ir": "ir.json",
    "cfg": "cfg.json",
    "dataflow": "dataflow.json",
    "ir_opt": "ir_opt.json",
    "passes": "passes.json",
    "code": "code.json",
}


class CompilerMissing(Exception):
    pass


class CompilerTimeout(Exception):
    pass


class CompilerFailed(Exception):
    pass


def _as_bytes(source):
    return source if isinstance(source, bytes) else source.encode("utf-8")


def _parc(arguments, deadline, limit=PARC_TIMEOUT_SECONDS):
    """Runs parc with the given arguments and returns the finished process."""
    if not PARC.is_file():
        raise CompilerMissing(f"the compiler was not found at {PARC}; build it first (see README.md)")
    try:
        # A list of arguments and no shell, so nothing in a path or in the
        # source can be read as a command. subprocess.run stops parc itself
        # when the time is up.
        done = subprocess.run(
            [str(PARC), *arguments],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=max(0.1, deadline - time.monotonic()),
            check=False,
        )
    except subprocess.TimeoutExpired as problem:
        raise CompilerTimeout(
            f"the compiler took longer than {limit} seconds and was stopped") from problem
    # parc exits with 0 when all is fine and 1 when the program has errors (or
    # stopped with a runtime error). Both are normal. Anything else is parc itself failing.
    if done.returncode not in (0, 1):
        raise CompilerFailed(f"parc {arguments[0]} stopped with exit code {done.returncode}: {done.stderr.strip()}")
    return done


def _read_json(path, command):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as problem:
        raise CompilerFailed(f"parc {command} did not write readable JSON: {problem}") from problem


def _compile_flags(optimize, force_parallel, registers):
    flags = []
    if not optimize:
        flags.append("-O0")
    if force_parallel:
        flags.append("--force-parallel")
    if registers:
        flags += ["--regs", str(int(registers))]
    return flags


def build(source, optimize=True, force_parallel=False, registers=None):
    """Compiles `source` and returns every phase's JSON.

    The result has one entry per phase that ran (see PHASE_FILES), plus:
      "stage"   "syntax", "semantic" or "complete": how far the compiler got
      "errors"  every error, from all phases, in source order
    """
    with tempfile.TemporaryDirectory(prefix="parc_explorer_") as folder:
        source_file = Path(folder) / "program.parc"
        source_file.write_bytes(_as_bytes(source))
        out = Path(folder) / "out"
        deadline = time.monotonic() + PARC_TIMEOUT_SECONDS
        _parc(["build", str(source_file), "--out", str(out), *_compile_flags(optimize, force_parallel, registers)],
              deadline)
        result = {}
        for name, file in PHASE_FILES.items():
            if (out / file).is_file():
                result[name] = _read_json(out / file, "build")

    for needed in ("tokens", "ast", "findings"):
        if needed not in result:
            raise CompilerFailed(f"parc build did not write {PHASE_FILES[needed]}")
    result["stage"] = result["findings"]["stage"]
    # tokens.json holds the scanner's errors, ast.json the parser's, and
    # findings.json everything the later phases report.
    errors = result["tokens"].get("errors", []) + result["ast"].get("errors", [])
    errors += [f for f in result["findings"]["findings"] if f["severity"] == "error"]
    errors.sort(key=lambda e: (e["line"], e["col"]))
    result["errors"] = errors
    return result


def run(source, threads=4, seed=1, optimize=True, force_parallel=False, registers=None, trace=0, max_steps=None):
    """Compiles and runs `source` on the VM.

    Returns the run.json contents, or {"ok": False, "compiled": False, ...}
    when the program has errors and there is nothing to run.
    """
    with tempfile.TemporaryDirectory(prefix="parc_explorer_") as folder:
        source_file = Path(folder) / "program.parc"
        source_file.write_bytes(_as_bytes(source))
        out = Path(folder) / "run.json"
        arguments = ["run", str(source_file), "--json", str(out), "--threads", str(int(threads)), "--seed", str(int(seed)),
                     *_compile_flags(optimize, force_parallel, registers)]
        if trace:
            arguments += ["--trace", str(int(trace))]
        if max_steps:
            arguments += ["--max-steps", str(int(max_steps))]
        done = _parc(arguments, time.monotonic() + PARC_TIMEOUT_SECONDS)
        if not out.is_file():
            # parc printed compile errors instead of running.
            return {"ok": False, "compiled": False, "output": "", "error": {"message": done.stderr.strip(), "line": 0}}
        result = _read_json(out, "run")
    result["compiled"] = True
    return result


def call(source, function, input_lines, threads=4, seed=1, optimize=True):
    """Runs one function once per input line ("5 [1,2,3] true") and returns the list of results.

    Returns None when the program does not compile or the inputs don't fit the function.
    """
    with tempfile.TemporaryDirectory(prefix="parc_explorer_") as folder:
        source_file = Path(folder) / "program.parc"
        source_file.write_bytes(_as_bytes(source))
        inputs = Path(folder) / "inputs.txt"
        inputs.write_text("\n".join(input_lines) + "\n", encoding="utf-8")
        arguments = ["call", str(source_file), "--fn", function, "--inputs", str(inputs), "--threads", str(int(threads)),
                     "--seed", str(int(seed)), "--max-steps", "2000000", *_compile_flags(optimize, False, None)]
        try:
            done = _parc(arguments, time.monotonic() + PARC_TIMEOUT_SECONDS)
        except CompilerFailed:
            return None  # exit code 2: the inputs do not match the function's parameters
        if done.returncode != 0:
            return None
        try:
            return json.loads(done.stdout)["results"]
        except (ValueError, KeyError):
            return None


def metrics(source, native=True, energy=True, min_ms=200, optimize=True):
    """Runs "parc metrics" and returns its JSON: compile times per phase, the VM run's time, energy and
    memory, the same program built by clang -O2 and measured the same way, Clang's remarks and warnings,
    and the time and space complexity of each function with suggested rewrites.

    Returns None when the program has syntax errors (there is nothing to measure).
    """
    with tempfile.TemporaryDirectory(prefix="parc_explorer_") as folder:
        source_file = Path(folder) / "program.parc"
        source_file.write_bytes(_as_bytes(source))
        out = Path(folder) / "metrics.json"
        arguments = ["metrics", str(source_file), "--json", str(out), "--min-ms", str(int(min_ms)),
                     *_compile_flags(optimize, False, None)]
        if not native:
            arguments.append("--no-native")
        if not energy:
            arguments.append("--no-energy")
        _parc(arguments, time.monotonic() + METRICS_TIMEOUT_SECONDS, METRICS_TIMEOUT_SECONDS)
        if not out.is_file():
            return None
        return _read_json(out, "metrics")


def compile_source(source):
    """The first explorer's entry point, kept for its callers: tokens, tree and errors."""
    return build(source)
