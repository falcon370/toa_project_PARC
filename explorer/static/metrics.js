// ParC explorer, part 6 of 6: the Metrics tab.
//
// Everything here is measured by the compiler, in C++ ("parc metrics"); the
// page only asks for it (POST /api/metrics) and draws it:
//
//   time and energy   the program on ParC's VM and the same program built by clang -O2
//   compile time      each of ParC's phases, and libclang parsing the same program as C
//   profile           how the VM's instructions were spread over the functions
//   complexity        time and space per function, from Clang's syntax tree, with rewrites that lower them
//   Clang             its optimizer's remarks and its warnings, by ParC line

"use strict";

const metricsOptions = { native: true };

function showMetrics(build) {
    const panel = $("panel-metrics");
    if (build.stage === "syntax") {
        panel.replaceChildren(make("p", "empty", "The program has syntax errors, so there is nothing to measure. See the Findings tab."));
        return;
    }
    const result = make("div", "metrics-result");
    const native = make("input");
    native.type = "checkbox";
    native.checked = metricsOptions.native;
    native.addEventListener("change", () => { metricsOptions.native = native.checked; });
    const measure = make("button", "small primary", "Measure");
    measure.type = "button";
    measure.addEventListener("click", async () => {
        measure.disabled = true;
        result.replaceChildren(make("p", "working", metricsOptions.native
            ? "Measuring: running on the VM, compiling with clang -O2, running natively. This takes a few seconds..."
            : "Measuring on the VM..."));
        try {
            const metrics = await postJson("/api/metrics", { compile_id: state.compileId, native: metricsOptions.native });
            drawMetrics(result, metrics);
        } catch (problem) {
            result.replaceChildren(make("p", "bad", problem.message));
        } finally {
            measure.disabled = false;
        }
    });
    panel.replaceChildren(
        box("div", "controls", [measure, box("label", "check", [native, " also build it with clang -O2 and run it natively"])]),
        make("p", "note", "Short programs are repeated until each measurement lasts 200 ms; numbers are per run. "
            + "Energy is read from the processor's own meter (RAPL), so it is the whole processor package; "
            + "the idle power just before is shown so you can see what the program added."),
        result);
}

// ---------------------------------------------------------------- formatting

function fmtMs(ms) {
    if (ms === undefined || ms === null || ms < 0) return "n/a";
    if (ms >= 100) return ms.toFixed(0) + " ms";
    if (ms >= 1) return ms.toFixed(2) + " ms";
    if (ms >= 0.001) return (ms * 1000).toFixed(2) + " µs";
    return (ms * 1e6).toFixed(1) + " ns";
}

function fmtJ(j) {
    if (j === undefined || j === null) return "n/a";
    if (j >= 1) return j.toFixed(2) + " J";
    if (j >= 1e-3) return (j * 1e3).toFixed(2) + " mJ";
    if (j >= 1e-6) return (j * 1e6).toFixed(2) + " µJ";
    return (j * 1e9).toFixed(1) + " nJ";
}

function fmtBytes(b) {
    if (b >= 1 << 20) return (b / (1 << 20)).toFixed(1) + " MB";
    if (b >= 1 << 10) return (b / (1 << 10)).toFixed(1) + " KB";
    return b + " bytes";
}

function tile(number, label) {
    return box("div", "tile", [make("div", "tile-number", number), make("div", "tile-label", label)]);
}

// A row of horizontal bars, longest first: [{label, value, text}]
function bars(items) {
    const max = Math.max(...items.map((i) => i.value), 1e-12);
    const list = make("div", "bars");
    for (const item of items) {
        const bar = make("span", "bar-fill");
        bar.style.width = Math.max(0.5, (100 * item.value) / max).toFixed(2) + "%";
        list.append(make("span", "bar-label", item.label), box("span", "bar-track", [bar]), make("span", "bar-value", item.text));
    }
    return list;
}

// ---------------------------------------------------------------- the cards

function drawMetrics(holder, m) {
    holder.replaceChildren(box("div", "metrics-page", [
        speedCard(m), complexityCard(m.complexity), compileCard(m), profileCard(m), clangCard(m),
    ]));
}

function speedCard(m) {
    if (!m.vm.ran) {
        return card("Time and energy", [make("p", "card-note",
            "The program has errors, so it was not run; only its complexity is shown below.")]);
    }
    const vm = m.vm.cost;
    const nat = m.native.ran ? m.native.cost : null;
    const tiles = [tile(fmtMs(vm.wall_ms), "per run on the VM")];
    if (nat) {
        tiles.push(tile(fmtMs(nat.wall_ms), "per run, clang -O2"));
        tiles.push(tile(m.native.speedup >= 10 ? m.native.speedup.toFixed(0) + "×" : m.native.speedup.toFixed(1) + "×", "faster natively"));
    }
    if (vm.has_energy) {
        tiles.push(tile(fmtJ(vm.energy_j), "energy per VM run"));
        tiles.push(tile(vm.power_w.toFixed(1) + " W", "package power while running"));
    }
    tiles.push(tile(fmtBytes(m.vm.heap_bytes), "VM heap (arrays)"));

    const rows = make("table", "plain");
    rows.append(headRow(["", "ParC VM", "clang -O2, native"]));
    const add = (name, a, b) => {
        const row = make("tr");
        row.append(make("td", "strong", name), make("td", "numeric", a), make("td", "numeric", b));
        rows.append(row);
    };
    const or = (value) => (nat ? value : "not run");
    add("time per run", fmtMs(vm.wall_ms), or(nat && fmtMs(nat.wall_ms)));
    add("processor time per run", vm.cpu_ms >= 0 ? fmtMs(vm.cpu_ms) : "too short", or(nat && (nat.cpu_ms >= 0 ? fmtMs(nat.cpu_ms) : "too short")));
    add("energy per run", vm.has_energy ? fmtJ(vm.energy_j) : "n/a", or(nat && (nat.has_energy ? fmtJ(nat.energy_j) : "n/a")));
    add("energy above idle", vm.has_energy ? fmtJ(vm.above_idle_j) : "n/a", or(nat && (nat.has_energy ? fmtJ(nat.above_idle_j) : "n/a")));
    add("average power", vm.has_energy ? vm.power_w.toFixed(1) + " W" : "n/a", or(nat && (nat.has_energy ? nat.power_w.toFixed(1) + " W" : "n/a")));
    add("peak memory (process)", fmtBytes(m.vm.peak_memory_bytes), or(nat && fmtBytes(m.native.peak_memory_bytes)));
    add("runs measured", String(vm.runs), or(nat && String(nat.runs)));

    const notes = [];
    notes.push(make("p", "card-note", m.energy.available
        ? `Energy: ${m.energy.source}. The machine drew ${m.energy.idle_w.toFixed(1)} W while idle just before.`
        : `Energy is not available here: ${m.energy.source}.`));
    if (nat) {
        notes.push(make("p", "card-note", `The native program repeats ParC's main itself, so starting the process (${fmtMs(m.native.startup_ms)}) is not counted. `
            + `Its output was ${m.native.same_output ? "the same as the VM's" : "DIFFERENT from the VM's"}. clang -O2 took ${fmtMs(m.native.compile_ms)} to build it.`));
    } else if (m.native.error) {
        notes.push(make("p", "card-note", "Native run: " + m.native.error));
    }
    return card("Time and energy", [box("div", "tiles", tiles), box("div", "table-scroll", [rows]), ...notes]);
}

function compileCard(m) {
    const items = m.compile.phases.map((p) => ({ label: p.phase, value: p.ms, text: fmtMs(p.ms) }));
    const parts = [bars(items)];
    let note = `ParC's phases took ${fmtMs(m.compile.total_ms)} in total.`;
    if (m.compile.ir_before) note += ` The optimizer took the IR from ${m.compile.ir_before} to ${m.compile.ir_after} instructions.`;
    if (m.clang.ok) note += ` For comparison, libclang (${m.clang.version}) parsed the same program as C in ${fmtMs(m.clang.parse_ms)}.`;
    parts.push(make("p", "card-note", note));
    return card("Compile time by phase", parts);
}

function profileCard(m) {
    if (!m.vm.ran) return null;
    const total = Math.max(1, m.vm.steps);
    const items = Object.entries(m.vm.function_steps)
        .filter(([, steps]) => steps > 0)
        .sort((a, b) => b[1] - a[1])
        .map(([name, steps]) => ({ label: name, value: steps, text: `${steps} (${((100 * steps) / total).toFixed(1)}%)` }));
    return card("Where the VM's instructions went", [
        bars(items),
        make("p", "card-note", `${m.vm.steps} instructions, ${plural(m.vm.calls, "call")}, a call stack at most ${m.vm.max_depth} deep, `
            + `${m.vm.heap_cells} array elements allocated (${fmtBytes(m.vm.heap_bytes)}).`),
    ]);
}

function complexityCard(c) {
    if (!c.ok) return card("Complexity", [make("p", "card-note", "Not available: " + c.error)]);
    const t = make("table", "plain");
    t.append(headRow(["Function", "Time", "Space", "Recursion", "Can be reduced to"]));
    const details = [];
    for (const fn of c.functions) {
        const row = pickable(make("tr"), fn.line, 0, false);
        const better = fn.suggestions.length
            ? fn.suggestions.map((s) => `time ${s.time_better}, space ${s.space_better}`).join("; ")
            : "—";
        row.append(make("td", "strong", fn.name), make("td", "mono", fn.time), make("td", "mono", fn.space),
            make("td", "", fn.recursion || ""), make("td", fn.suggestions.length ? "mono good" : "muted", better));
        t.append(row);
        for (const s of fn.suggestions) {
            const item = pickable(make("div", "suggestion"), s.line, 0, false);
            item.append(
                make("div", "strong", `${fn.name}, line ${s.line}: ${s.title}`),
                box("div", "compare-bounds", [
                    make("span", "mono", `time ${s.time_now} → ${s.time_better}`),
                    make("span", "mono", `space ${s.space_now} → ${s.space_better}`),
                ]),
                make("div", "", `Why: ${s.why}.`),
                make("div", "muted", `How: ${s.how}`));
            details.push(item);
        }
        for (const loop of fn.loops.filter((l) => l.note)) {
            details.push(pickable(make("div", "card-note small-text", `${fn.name}, line ${loop.line}: the ${loop.kind} loop runs ${loop.runs} times; ${loop.note}.`), loop.line, 0, false));
        }
        for (const note of fn.notes) details.push(make("div", "card-note small-text", `${fn.name}: ${note}.`));
    }
    const sizes = new Map();
    for (const fn of c.functions) for (const [size, what] of Object.entries(fn.sizes)) sizes.set(size, what);
    const legendText = [...sizes].map(([size, what]) => `${size} is ${what}`).join("; ");
    return card("Complexity per function", [
        box("div", "table-scroll", [t]),
        legendText ? make("p", "card-note", legendText + ".") : null,
        details.length ? make("h3", "card-sub", "Suggestions and how the counts were found") : null,
        ...details,
        make("p", "card-note", "Read from Clang's syntax tree (libclang) of the program translated to C. A big-O bound is an "
            + "upper estimate from the program text; ? marks a loop count the analysis could not work out."),
    ]);
}

function clangCard(m) {
    const parts = [];
    const remarks = m.native.remarks || [];
    if (remarks.length) {
        const t = make("table", "plain");
        t.append(headRow(["Line", "Pass", "", "What Clang's optimizer did"]));
        for (const r of remarks.slice(0, 80)) {
            const row = pickable(make("tr"), r.line, 0, false);
            row.append(make("td", "numeric", String(r.line)), make("td", "mono", r.pass),
                make("td", r.type === "passed" ? "good" : r.type === "missed" ? "warn" : "muted", r.type),
                make("td", "", r.message));
            t.append(row);
        }
        parts.push(make("h3", "card-sub", "Optimizer remarks at -O2"), box("div", "table-scroll", [t]));
        if (remarks.length > 80) parts.push(make("p", "card-note", `${remarks.length - 80} more remarks are in the JSON.`));
    } else if (m.native.ran === false && !m.native.error) {
        parts.push(make("p", "card-note", "Build it with clang -O2 (the check box above) to see what Clang's optimizer does with it."));
    }
    parts.push(make("h3", "card-sub", "Clang's warnings on the same program (-Wall -Wextra)"));
    if (!m.clang.ok) parts.push(make("p", "card-note", "Not available: " + m.clang.error));
    else if (!m.clang.warnings.length) parts.push(make("p", "card-note", "none"));
    for (const w of m.clang.warnings) {
        parts.push(pickable(make("p", "event", `Line ${w.line}: ${w.message}${w.option ? " [" + w.option + "]" : ""}`), w.line, w.col, w.severity === "error"));
    }
    return card("Clang on the same program", parts);
}
