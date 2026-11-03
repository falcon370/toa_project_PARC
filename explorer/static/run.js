// ParC explorer, part 4 of 4: running the program on the VM, and stepping through it.

"use strict";

const runOptions = { threads: 4, seed: 1, force_parallel: false };
let debug = null;  // {steps, code, at} while the step debugger is open

function showRun(build) {
    const panel = $("panel-run");
    debug = null;
    if (build.stage !== "complete") {
        panel.replaceChildren(make("p", "empty", "The program has errors, so there is nothing to run. See the Findings tab."));
        return;
    }
    const number = (name, min, max) => {
        const input = make("input");
        input.type = "number";
        input.min = min;
        input.max = max;
        input.value = runOptions[name];
        input.addEventListener("change", () => {
            runOptions[name] = Math.min(max, Math.max(min, Math.round(Number(input.value) || min)));
            input.value = runOptions[name];
        });
        return input;
    };
    const force = make("input");
    force.type = "checkbox";
    force.checked = runOptions.force_parallel;
    force.addEventListener("change", () => { runOptions.force_parallel = force.checked; });
    const forceLabel = box("label", "check", [force, " run unsafe parallel loops in parallel anyway (to see a data race)"]);

    const result = make("div", "run-result");
    const button = (text, className, trace) => {
        const b = make("button", className, text);
        b.type = "button";
        b.addEventListener("click", async () => {
            result.replaceChildren(make("p", "working", "Running..."));
            try {
                const outcome = await postJson("/api/run", { compile_id: state.compileId, ...runOptions, trace });
                if (trace) showDebugger(result, outcome); else showOutcome(result, outcome);
            } catch (problem) {
                result.replaceChildren(make("p", "bad", problem.message));
            }
        });
        return b;
    };
    panel.replaceChildren(
        box("div", "controls", [
            button("Run", "small primary", false), button("Step through", "small", true),
            make("label", "", " Threads "), number("threads", 1, 64), make("label", "", " Scheduler seed "), number("seed", 0, 99999),
        ]),
        box("div", "controls", [forceLabel]),
        make("p", "note", "The program runs on the register VM. A parallel loop is split over the threads; a scheduler picks which thread runs next from the seed, so the same seed always gives the same order."),
        result);
}

// What a run printed, why it stopped, and what the VM noticed along the way.
function outcomeParts(outcome) {
    const parts = [];
    if (!outcome.compiled) {
        parts.push(make("p", "bad", "The program did not compile: " + outcome.error.message));
        return parts;
    }
    parts.push(make("div", "field", "Output"));
    parts.push(make("pre", "output", outcome.output || "(nothing was printed)"));
    if (!outcome.ok) {
        const where = outcome.error.line ? ` at line ${outcome.error.line}, in ${outcome.error.function}` : "";
        const stop = pickable(make("p", "bad", `The program stopped${where}: ${outcome.error.message}.`), outcome.error.line, 0, true);
        parts.push(stop);
    }
    for (const event of outcome.events) {
        parts.push(pickable(make("p", "event", `Line ${event.line}: ${event.text}`), event.line, 0, false));
    }
    let summary = `${outcome.steps} VM instructions executed.`;
    if (outcome.overflows) summary += ` ${outcome.overflows} int overflow${outcome.overflows === 1 ? "" : "s"}.`;
    if (outcome.races) summary += ` ${outcome.races} data race${outcome.races === 1 ? "" : "s"}.`;
    if (outcome.parallel_loops) {
        const turns = outcome.schedule.map(([thread, passes]) => `T${thread}×${passes}`).join(" ");
        summary += ` ${outcome.parallel_loops} parallel loop${outcome.parallel_loops === 1 ? "" : "s"} ran on threads.`;
        parts.push(make("p", "note", summary));
        parts.push(make("div", "field", "Thread schedule (thread × passes of the loop before the next switch)"));
        parts.push(make("pre", "schedule", turns || "(one thread)"));
    } else {
        parts.push(make("p", "note", summary));
    }
    return parts;
}

function showOutcome(result, outcome) {
    debug = null;
    result.replaceChildren(...outcomeParts(outcome));
}

// ---------------------------------------------------------------- step debugger

// The trace is a list of executed instructions, each with the registers of the
// frame it ran in. Stepping is just showing another entry of that list.
function showDebugger(result, outcome) {
    if (!outcome.compiled || !outcome.trace.length) {
        showOutcome(result, outcome);
        return;
    }
    debug = { steps: outcome.trace, code: outcome.code, at: 0, outcome };

    const position = make("span", "pos");
    const slider = make("input");
    slider.type = "range";
    slider.min = 0;
    slider.max = debug.steps.length - 1;
    slider.value = 0;
    const view = make("div", "debug-view");

    const go = (to) => {
        debug.at = Math.min(debug.steps.length - 1, Math.max(0, to));
        slider.value = debug.at;
        position.textContent = ` step ${debug.at + 1} of ${debug.steps.length}`;
        drawStep(view);
    };
    slider.addEventListener("input", () => go(Number(slider.value)));
    const button = (text, title, target) => {
        const b = make("button", "small", text);
        b.type = "button";
        b.title = title;
        b.addEventListener("click", () => go(target()));
        return b;
    };
    // "Next line" runs until the source line changes, which is how one usually wants to step.
    const nextLine = () => {
        const here = debug.steps[debug.at];
        let i = debug.at + 1;
        while (i < debug.steps.length - 1 && debug.steps[i].line === here.line && debug.steps[i].thread === here.thread) i++;
        return i;
    };
    const truncated = outcome.steps > debug.steps.length
        ? ` Only the first ${debug.steps.length} of ${outcome.steps} instructions were recorded.` : "";
    result.replaceChildren(
        box("div", "controls", [
            button("⏮", "First instruction", () => 0), button("◀", "One instruction back", () => debug.at - 1),
            button("▶", "One instruction forward", () => debug.at + 1), button("Next line", "Run until the source line changes", nextLine),
            button("⏭", "Last recorded instruction", () => debug.steps.length - 1), position,
        ]),
        slider, view,
        make("p", "note", "Each step is one VM instruction that ran, with the registers of the call it ran in afterwards." + truncated),
        ...outcomeParts(outcome));
    go(0);
}

function drawStep(view) {
    const step = debug.steps[debug.at];
    const fn = debug.code.functions.find((f) => f.name === step.function);
    const instruction = fn && fn.code[step.pc] ? fn.code[step.pc].text : "";
    if (step.line > 0) highlightLine(step.line); else clearHighlight();

    const who = step.thread === 0 ? "main thread" : `thread ${step.thread} of the parallel loop`;
    const head = make("p", "step-head");
    head.append(make("span", "kind", `${step.function} @${step.pc}`), "  ", make("code", "", instruction),
                make("span", "field", `   ${who}, call depth ${step.depth}` + (step.line ? `, line ${step.line}` : "")));

    // Which variable is in which register at this instruction (from the allocator's intervals).
    const names = {};
    if (fn) {
        for (const v of fn.vars) {
            if (step.pc >= v.from && step.pc <= v.to) names[v.where] = (names[v.where] ? names[v.where] + ", " : "") + v.name;
        }
    }
    const previous = debug.at > 0 ? debug.steps[debug.at - 1] : null;
    const sameFrame = previous && previous.function === step.function && previous.depth === step.depth && previous.thread === step.thread;
    const cells = (values, label) => {
        const t = table("grid regs", [label, "Value", "Holds"]);
        values.forEach((value, i) => {
            const where = label === "Register" ? `r${i}` : `slot ${i}`;
            if (value === "" && !names[where]) return;
            const row = t.tBodies[0].insertRow();
            const before = sameFrame ? (label === "Register" ? previous.regs[i] : previous.slots[i]) : value;
            if (before !== value) row.className = "changed";  // written by this instruction
            row.append(make("td", "kind", where), make("td", "", value === "" ? "–" : value), make("td", "field", names[where] || ""));
        });
        return t;
    };

    // Arrays and printed text are rebuilt by replaying the steps up to here.
    const arrays = {};
    let printed = "";
    for (let i = 0; i <= debug.at; i++) {
        const s = debug.steps[i];
        if (s.output) printed += s.output;
        if (!s.heap) continue;
        const made = s.heap.match(/^new #(\d+) \[(\d+)\]$/);
        const stored = s.heap.match(/^#(\d+)\[(\d+)\] = (.*)$/);
        if (made) arrays[made[1]] = new Array(Math.min(Number(made[2]), 64)).fill("·");
        if (stored && arrays[stored[1]] && Number(stored[2]) < 64) arrays[stored[1]][Number(stored[2])] = stored[3];
    }
    const heap = make("div", "heap");
    for (const [id, items] of Object.entries(arrays)) {
        heap.append(make("div", "", `#${id}  [${items.join(", ")}]`));
    }

    const parts = [head, cells(step.regs, "Register")];
    if (step.slots.some((v) => v !== "")) parts.push(cells(step.slots, "Slot"));
    if (step.heap) parts.push(make("p", "event", "This instruction: " + step.heap));
    if (Object.keys(arrays).length) parts.push(make("div", "field", "Arrays so far (· is an element not written yet)"), heap);
    parts.push(make("div", "field", "Printed so far"), make("pre", "output", printed || "(nothing yet)"));
    view.replaceChildren(...parts);
}
