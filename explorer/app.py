"""ParC explorer: a local web page that shows what each compiler phase produced.

The page sends the source text here; compiler_bridge.py runs the parc binary
on it, and the agent package explains findings and proposes checked fixes.

    python explorer/app.py        then open http://127.0.0.1:5000

Routes:
    POST /api/compile        {source}                       every phase's JSON, and a compile_id
    GET  /api/phase/<name>   ?compile_id=                   one phase's JSON again
    POST /api/run            {compile_id | source, ...}     the program's output on the VM
    POST /api/metrics        {compile_id | source, native?, energy?, min_ms?}   time, energy, memory,
                             the clang -O2 build, complexity per function (from "parc metrics")
    POST /api/explain        {compile_id, finding_id, ...}  an explanation, or a hint question
    POST /api/fix            {compile_id, finding_id}       the best rewrite that passed the compiler's checks
    GET  /api/agent          which language model answers, if any
    GET  /api/security       what the agent's security layer detected and blocked lately
    GET  /api/security/metrics   how well that layer did in the evaluation (evaluation/security_results.json)
    GET  /api/examples, /api/examples/<name>
"""

import json
import os
import secrets
import time
from collections import OrderedDict, deque

from flask import Flask, jsonify, render_template, request

import compiler_bridge
from agent import agent, llm, security

EXAMPLES = compiler_bridge.ROOT / "examples"
DEFAULT_EXAMPLE = "06_dot_parallel.parc"

HOST = "127.0.0.1"
PORT = int(os.environ.get("PARC_EXPLORER_PORT", "5000"))
MAX_SOURCE_BYTES = 100 * 1024
KEPT_COMPILES = 40             # how many compile results the server remembers
AGENT_CALLS_PER_MINUTE = 20    # each call may spend language-model quota
TRACE_STEPS = 3000             # instructions recorded for the step debugger

app = Flask(__name__)
# Flask sorts JSON keys alphabetically by default. The compiler's own order
# (start before end, params before body) is the one worth showing.
app.json.sort_keys = False
# Make the browser ask for the page's files again on every load, so an edit to
# app.js or style.css shows up on refresh.
app.config["SEND_FILE_MAX_AGE_DEFAULT"] = 0
# JSON escaping can make the request body several times larger than the source
# (a newline becomes \n, a control character becomes \u0000), so this is only
# an outer bound. The exact 100 KB check is on the decoded source below.
app.config["MAX_CONTENT_LENGTH"] = 6 * MAX_SOURCE_BYTES + 1024

# compile_id -> {"source": text, "build": every phase's JSON}. Later calls name
# the id, so the source is not sent again and the phases are not recomputed.
compiles = OrderedDict()
agent_calls = deque()


def fail(status, message):
    return jsonify(error=message), status


@app.before_request
def only_local_host_names():
    # A web page on another site could point its own domain name at 127.0.0.1
    # to reach this server (DNS rebinding). Its requests carry that name here.
    name = request.host.split(":")[0]
    if name not in ("127.0.0.1", "localhost"):
        return fail(403, "the explorer only answers on 127.0.0.1")
    return None


@app.errorhandler(413)
def body_too_large(_error):
    return fail(413, f"the source is larger than the {MAX_SOURCE_BYTES // 1024} KB limit")


@app.errorhandler(compiler_bridge.CompilerTimeout)
def compiler_timeout(problem):
    return fail(504, str(problem))


@app.errorhandler(compiler_bridge.CompilerMissing)
@app.errorhandler(compiler_bridge.CompilerFailed)
def compiler_problem(problem):
    return fail(500, str(problem))


@app.get("/")
def index():
    return render_template("index.html")


# ---------------------------------------------------------------- examples

def example_names():
    return sorted(p.name for p in EXAMPLES.glob("*.parc"))


@app.get("/api/examples")
def list_examples():
    return jsonify(examples=example_names(), default=DEFAULT_EXAMPLE)


@app.get("/api/examples/<name>")
def get_example(name):
    # Only names from the directory listing are accepted, so a name like
    # "..\secret.txt" can never reach the file system.
    if name not in example_names():
        return fail(404, f"no example named {name}")
    source = (EXAMPLES / name).read_text(encoding="utf-8").replace("\r\n", "\n")
    return jsonify(name=name, source=source)


# ---------------------------------------------------------------- compiling

def body_json():
    body = request.get_json(silent=True)
    return body if isinstance(body, dict) else None


def checked_source(body):
    """The request's source as text, or an error response."""
    if body is None or not isinstance(body.get("source"), str):
        return None, fail(400, 'send JSON like {"source": "fn main() { }"}')
    try:
        size = len(body["source"].encode("utf-8"))
    except UnicodeEncodeError:
        return None, fail(400, "the source is not valid text")
    if size > MAX_SOURCE_BYTES:
        return None, fail(413, f"the source is larger than the {MAX_SOURCE_BYTES // 1024} KB limit")
    return body["source"], None


def remember(source, build):
    compile_id = secrets.token_hex(8)
    compiles[compile_id] = {"source": source, "build": build}
    while len(compiles) > KEPT_COMPILES:
        compiles.popitem(last=False)  # forget the oldest
    return compile_id


def remembered(compile_id):
    entry = compiles.get(compile_id) if isinstance(compile_id, str) else None
    if entry is None:
        return None, fail(404, "that compile result is no longer kept; compile again")
    return entry, None


@app.post("/api/compile")
def compile_source():
    source, problem = checked_source(body_json())
    if problem:
        return problem
    build = compiler_bridge.build(source)
    return jsonify(compile_id=remember(source, build), **build)


@app.get("/api/phase/<name>")
def phase(name):
    entry, problem = remembered(request.args.get("compile_id"))
    if problem:
        return problem
    if name not in compiler_bridge.PHASE_FILES:
        return fail(404, f"there is no phase named {name}; the phases are {', '.join(compiler_bridge.PHASE_FILES)}")
    if name not in entry["build"]:
        return fail(404, f"the compiler stopped before {name} (stage: {entry['build']['stage']})")
    return jsonify(entry["build"][name])


def small_int(body, name, default, low, high):
    value = body.get(name, default)
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        return default
    return value


@app.post("/api/run")
def run_program():
    body = body_json()
    if body is None:
        return fail(400, 'send JSON like {"compile_id": "..."} or {"source": "..."}')
    if "compile_id" in body:
        entry, problem = remembered(body["compile_id"])
        if problem:
            return problem
        source = entry["source"]
    else:
        source, problem = checked_source(body)
        if problem:
            return problem
    options = {
        "optimize": body.get("optimize", True) is not False,
        "force_parallel": body.get("force_parallel") is True,
        "registers": small_int(body, "registers", 12, 4, 64),
    }
    tracing = body.get("trace") is True
    # The 5-second limit of the bridge also stops a program that never ends.
    result = compiler_bridge.run(
        source,
        threads=small_int(body, "threads", 4, 1, 64),
        seed=small_int(body, "seed", 1, 0, 2**31 - 1),
        trace=TRACE_STEPS if tracing else 0,
        **options,
    )
    if tracing and result.get("compiled"):
        # The debugger shows each step's instruction and which variable is in
        # which register, so it needs the code listing for these same options.
        result["code"] = compiler_bridge.build(source, **options)["code"]
    return jsonify(result)


@app.post("/api/metrics")
def metrics():
    body = body_json()
    if body is None:
        return fail(400, 'send JSON like {"compile_id": "..."} or {"source": "..."}')
    if "compile_id" in body:
        entry, problem = remembered(body["compile_id"])
        if problem:
            return problem
        source = entry["source"]
    else:
        source, problem = checked_source(body)
        if problem:
            return problem
    result = compiler_bridge.metrics(
        source,
        native=body.get("native", True) is not False,
        energy=body.get("energy", True) is not False,
        min_ms=small_int(body, "min_ms", 200, 10, 2000),
    )
    if result is None:
        return fail(409, "the program has syntax errors, so there is nothing to measure")
    return jsonify(result)


# ---------------------------------------------------------------- the agent

def agent_request():
    """Checks an explain or fix request. Returns (entry, body, None) or (None, None, error response)."""
    now = time.monotonic()
    while agent_calls and now - agent_calls[0] > 60:
        agent_calls.popleft()
    if len(agent_calls) >= AGENT_CALLS_PER_MINUTE:
        return None, None, fail(429, "too many explain and fix requests in the last minute; wait a little")
    body = body_json()
    if body is None or not isinstance(body.get("finding_id"), str):
        return None, None, fail(400, 'send JSON like {"compile_id": "...", "finding_id": "F1"}')
    entry, problem = remembered(body.get("compile_id"))
    if problem:
        return None, None, problem
    if entry["build"]["stage"] == "syntax":
        return None, None, fail(409, "the program has syntax errors; the agent works on findings of the later phases")
    agent_calls.append(now)
    return entry, body, None


@app.post("/api/explain")
def explain():
    entry, body, problem = agent_request()
    if problem:
        return problem
    result = agent.explain(entry["source"], entry["build"], body["finding_id"],
                           level="engineer" if body.get("level") == "engineer" else "student",
                           hint=body.get("hint") is True)
    return fail(404, result["error"]) if "error" in result else jsonify(result)


@app.post("/api/fix")
def fix():
    entry, body, problem = agent_request()
    if problem:
        return problem
    result = agent.fix(entry["source"], entry["build"], body["finding_id"])
    return fail(404, result["error"]) if "error" in result else jsonify(result)


@app.get("/api/agent")
def agent_status():
    # Only the model's name is sent. The API key stays on the server.
    return jsonify(llm.status())


@app.get("/api/security")
def security_log():
    # The latest events of the agent's security layer, newest first. The full
    # log is the file explorer/.cache/security_log.jsonl.
    return jsonify(security.recent())


SECURITY_RESULTS = compiler_bridge.ROOT / "evaluation" / "security_results.json"


@app.get("/api/security/metrics")
def security_metrics():
    # Written by "python evaluation/security_eval.py". The page shows a hint if it is missing.
    try:
        return jsonify(available=True, **json.loads(SECURITY_RESULTS.read_text(encoding="utf-8")))
    except (OSError, ValueError):
        return jsonify(available=False)


if __name__ == "__main__":
    # debug stays off: Flask's debugger lets anyone who can reach the page run Python.
    app.run(host=HOST, port=PORT, debug=False)
