// ParC explorer, part 2 of 4: drawing each phase's output in its tab.

"use strict";

const TOKEN_PAGE = 2000;        // token rows added at a time
const TREE_OPEN_NODES = 1500;   // tree nodes drawn open before the rest wait to be opened

function table(className, titles) {
    const t = make("table", className);
    const head = t.createTHead().insertRow();
    for (const title of titles) head.append(make("th", "", title));
    t.createTBody();
    return t;
}

// ---------------------------------------------------------------- tokens (scanner)

function showTokens(list) {
    const t = table("grid tokens", ["Line:col", "Kind", "Text"]);
    const body = t.tBodies[0];
    const more = make("button", "more");
    more.type = "button";

    // A very long program has tens of thousands of tokens; drawing them all
    // at once freezes the page, so they are added a page at a time.
    let shown = 0;
    const addPage = () => {
        for (const token of list.slice(shown, shown + TOKEN_PAGE)) {
            const row = pickable(body.insertRow(), token.line, token.col, false);
            row.dataset.length = Math.max(1, token.text.length);
            row.append(make("td", "pos", `${token.line}:${token.col}`), make("td", "kind", token.kind), make("td", "", token.text));
        }
        shown = Math.min(list.length, shown + TOKEN_PAGE);
        more.hidden = shown === list.length;
        more.textContent = `Showing ${shown} of ${list.length} tokens. Show more`;
    };
    more.addEventListener("click", addPage);
    addPage();
    $("panel-tokens").replaceChildren(t, more);
}

// ---------------------------------------------------------------- syntax tree (parser)

const isNode = (value) => value !== null && typeof value === "object" && "line" in value && "col" in value;

// The words shown after a node's kind: "Let sum: int", "Binary +", "Name n".
function describe(node) {
    switch (node.node) {
        case "Function": {
            const params = node.params.map((p) => `${p.name}: ${p.type}`).join(", ");
            return `${node.name}(${params})` + (node.return_type === "void" ? "" : ` -> ${node.return_type}`);
        }
        case "Let": return `${node.name}: ${node.type}`;
        case "For":
        case "ParallelFor":
            return node.var + (node.reduce_vars ? ` reduce(${node.reduce_op}: ${node.reduce_vars.join(", ")})` : "");
        case "String": return JSON.stringify(node.value);
        case undefined: return `${node.name}: ${node.type}`;  // a parameter
        default:
            // Any other plain values (an operator, a name, a number).
            return Object.entries(node)
                .filter(([key, value]) => !["node", "line", "col"].includes(key) && value !== null && typeof value !== "object")
                .map(([, value]) => String(value))
                .join(" ");
    }
}

// The parts of a node that are nodes themselves, each with the name of the
// field it came from ("cond", "then", "args[0]"). Statements in a block need no name.
function childrenOf(node) {
    const children = [];
    for (const [key, value] of Object.entries(node)) {
        if (isNode(value)) {
            children.push([key, value]);
        } else if (Array.isArray(value)) {
            value.forEach((item, i) => {
                if (isNode(item)) children.push([key === "stmts" ? "" : `${key}[${i}]`, item]);
            });
        }
    }
    return children;
}

// Tree items whose parts have not been drawn yet, and those parts.
const undrawn = new WeakMap();

function drawChildren(item, budget) {
    const list = make("ul");
    for (const [name, child] of undrawn.get(item)) list.append(treeItem(name, child, budget));
    undrawn.delete(item);
    item.append(list);
}

// `budget.left` is how many more nodes may be drawn open right now. Once it
// runs out, the remaining nodes start closed and are drawn when first opened,
// so a very long program does not freeze the page.
function treeItem(field, node, budget) {
    const item = make("li");
    const children = childrenOf(node);
    budget.left -= 1;
    const row = pickable(make("div", "node"), node.line, node.col, false);
    const twist = make("button", children.length ? "twist" : "twist leaf");
    twist.type = "button";
    if (children.length) {
        twist.setAttribute("aria-label", "Show or hide the parts of this node");
        twist.setAttribute("aria-expanded", "true");
    } else {
        twist.tabIndex = -1;
        twist.setAttribute("aria-hidden", "true");
    }
    row.append(twist);
    if (field) row.append(make("span", "field", field + ":"));
    row.append(make("span", "kind", node.node || "Param"), make("span", "", describe(node)),
               make("span", "pos", `${node.line}:${node.col}`));
    item.append(row);
    if (children.length) {
        undrawn.set(item, children);
        if (budget.left > 0) {
            drawChildren(item, budget);
        } else {
            item.classList.add("closed");
            twist.setAttribute("aria-expanded", "false");
        }
    }
    return item;
}

function toggleTreeItem(twist) {
    const item = twist.closest("li");
    if (undrawn.has(item)) drawChildren(item, { left: TREE_OPEN_NODES });
    const closed = item.classList.toggle("closed");
    twist.setAttribute("aria-expanded", String(!closed));
}

function showTree(ast) {
    const panel = $("panel-tree");
    if (!ast.functions.length) {
        panel.replaceChildren(make("p", "empty", "The parser found no complete function, so there is no tree to show."));
        return;
    }
    const tree = make("ul", "tree");
    const budget = { left: TREE_OPEN_NODES };
    for (const fn of ast.functions) tree.append(treeItem("", fn, budget));
    panel.replaceChildren(tree);
}

// ---------------------------------------------------------------- symbols (semantic)

function showSymbols(symbols) {
    const t = table("grid", ["Name", "Kind", "Type", "In", "Declared", "Read", "Written"]);
    for (const s of symbols.symbols) {
        const row = pickable(t.tBodies[0].insertRow(), s.line, s.col, false);
        row.dataset.length = s.name.length;
        // Indent by scope depth, so nesting is visible.
        const name = make("td", "", s.name);
        name.style.paddingLeft = `${12 + 14 * Math.max(0, s.depth - 1)}px`;
        row.append(name, make("td", "kind", s.kind), make("td", "", s.type), make("td", "", s.function),
                   make("td", "pos", `${s.line}:${s.col}`), make("td", "num", s.kind === "function" ? `${s.reads} call${s.reads === 1 ? "" : "s"}` : String(s.reads)),
                   make("td", "num", s.kind === "function" ? "" : String(s.writes)));
    }
    const note = make("p", "note", "Every declared name, indented by the scope it is in. Read and Written count its uses; " +
                                   "a variable that is never read gets a warning.");
    $("panel-symbols").replaceChildren(note, t);
}

// ---------------------------------------------------------------- IR and control-flow graph

function showIr(build) {
    const panel = $("panel-ir");
    const names = build.ir.functions.map((f) => f.name);
    const hasOptimized = Boolean(build.ir_opt);
    const draw = () => {
        const optimized = hasOptimized && state.chosen.irVersion === "optimized";
        const program = optimized ? build.ir_opt : build.ir;
        const fn = program.functions.find((f) => f.name === state.chosen.ir) || program.functions[0];
        const graph = build.cfg.functions.find((f) => f.name === fn.name);

        const version = make("select", "picker");
        version.append(new Option("as built from the tree", "built"));
        if (hasOptimized) version.append(new Option("after the optimizer", "optimized"));
        version.value = optimized ? "optimized" : "built";
        version.addEventListener("change", () => {
            state.chosen.irVersion = version.value;
            draw();
        });

        const controls = box("div", "controls", [
            make("label", "", "Function "), functionPicker("ir", names, draw), make("label", "", " IR "), version,
        ]);

        const m = graph.metrics;
        const instructions = fn.blocks.reduce((n, b) => n + b.instrs.length, 0);
        const metrics = make("p", "note",
            `${fn.signature}: ${fn.blocks.length} blocks, ${instructions} instructions here. ` +
            `Measured on the code as built: cyclomatic complexity ${m.cyclomatic_complexity}, nesting depth ${m.nesting_depth}, ` +
            `${m.loops} loop${m.loops === 1 ? "" : "s"}.`);

        const listing = make("div", "listing");
        for (const block of fn.blocks) {
            listing.append(make("div", "label", block.label + ":"));
            for (const instr of block.instrs) {
                const row = make("div", "instr", instr.text);
                if (instr.line > 0) pickable(row, instr.line, instr.col, false);
                listing.append(row);
            }
        }
        panel.replaceChildren(controls, metrics, drawGraph(fn, optimized ? null : graph), listing);
    };
    draw();
}

// Draws a function's control-flow graph as SVG: one box per block, in rows by
// how far the block is from the entry. Edges that go back up (loops) curve
// around the left side.
function drawGraph(fn, graph) {
    const NS = "http://www.w3.org/2000/svg";
    const svg = (tag, attrs, text) => {
        const node = document.createElementNS(NS, tag);
        for (const [key, value] of Object.entries(attrs)) node.setAttribute(key, value);
        if (text !== undefined) node.textContent = text;
        return node;
    };
    const CHAR = 7.2, LINE = 15, MAX_LINES = 7, GAP_X = 26, GAP_Y = 38;
    const blocks = fn.blocks.map((block, id) => {
        const lines = block.instrs.map((i) => i.text);
        const shown = lines.length > MAX_LINES ? lines.slice(0, MAX_LINES - 1).concat(`... ${lines.length - MAX_LINES + 1} more`) : lines;
        const widest = Math.max(block.label.length + 1, ...shown.map((l) => l.length));
        return { id, label: block.label, lines: shown, succs: block.succs, branch: lines.length && lines[lines.length - 1].startsWith("if "),
                 w: Math.min(46, widest) * CHAR + 18, h: (shown.length + 1) * LINE + 14, layer: 0 };
    });

    // A block's row is one below the lowest row of the blocks that jump forward to it.
    for (const block of blocks) {
        for (const s of block.succs) {
            if (s > block.id) blocks[s].layer = Math.max(blocks[s].layer, block.layer + 1);
        }
    }
    const rows = [];
    for (const block of blocks) (rows[block.layer] = rows[block.layer] || []).push(block);
    const rowWidth = (row) => row.reduce((w, b) => w + b.w, 0) + GAP_X * (row.length - 1);
    const width = Math.max(...rows.filter(Boolean).map(rowWidth)) + 90;
    let y = 14;
    for (const row of rows.filter(Boolean)) {
        let x = (width - rowWidth(row)) / 2 + 20;
        for (const block of row) {
            block.x = x;
            block.y = y;
            x += block.w + GAP_X;
        }
        y += Math.max(...row.map((b) => b.h)) + GAP_Y;
    }

    const root = svg("svg", { class: "graph", viewBox: `0 0 ${width} ${y}`, width, height: y, role: "img",
                              "aria-label": `Control-flow graph of ${fn.name}` });
    const defs = svg("defs", {});
    for (const kind of ["jump", "true", "false"]) {
        const marker = svg("marker", { id: "arrow-" + kind, viewBox: "0 0 8 8", refX: 7, refY: 4, markerWidth: 7, markerHeight: 7, orient: "auto" });
        marker.append(svg("path", { d: "M0,0 L8,4 L0,8 z", class: "arrow-" + kind }));
        defs.append(marker);
    }
    root.append(defs);

    for (const block of blocks) {
        block.succs.forEach((s, n) => {
            const to = blocks[s];
            const kind = !block.branch ? "jump" : n === 0 ? "true" : "false";
            const x1 = block.x + block.w * (block.succs.length === 2 ? (n === 0 ? 0.3 : 0.7) : 0.5);
            const y1 = block.y + block.h;
            const x2 = to.x + to.w / 2;
            let d;
            if (s > block.id && to.layer > block.layer) {
                d = `M${x1},${y1} C${x1},${y1 + 20} ${x2},${to.y - 20} ${x2},${to.y}`;
            } else {
                // A back edge: out of the bottom, around the left, into the side of the target.
                const left = Math.min(block.x, to.x) - 26 - 6 * (block.id % 3);
                d = `M${x1},${y1} C${x1},${y1 + 26} ${left},${y1 + 10} ${left},${(y1 + to.y) / 2} S${to.x - 30},${to.y + to.h / 2} ${to.x},${to.y + to.h / 2}`;
            }
            root.append(svg("path", { d, class: "edge edge-" + kind, "marker-end": `url(#arrow-${kind})` }));
            if (kind !== "jump") root.append(svg("text", { x: x1 + (n === 0 ? -12 : 5), y: y1 + 13, class: "edge-label edge-" + kind }, kind === "true" ? "T" : "F"));
        });
    }
    for (const block of blocks) {
        const reachable = !graph || graph.blocks[block.id] === undefined || graph.blocks[block.id].reachable;
        const group = svg("g", { class: reachable ? "block" : "block dead" });
        group.append(svg("rect", { x: block.x, y: block.y, width: block.w, height: block.h, rx: 5 }));
        group.append(svg("text", { x: block.x + 9, y: block.y + 16, class: "block-label" }, block.label));
        block.lines.forEach((line, i) => {
            group.append(svg("text", { x: block.x + 9, y: block.y + 16 + (i + 1) * LINE }, line.length > 46 ? line.slice(0, 45) + "…" : line));
        });
        root.append(group);
    }
    const holder = make("div", "graph-holder");
    holder.append(root);
    return holder;
}

// ---------------------------------------------------------------- data flow

const ANALYSES = {
    live: { title: "Live variables", in: "live_in", out: "live_out",
            note: "Backward analysis, union at joins. A variable is live where some path ahead reads it before writing it." },
    reaching: { title: "Reaching definitions", in: "reach_in", out: "reach_out",
                note: "Forward analysis, union at joins. A definition reaches a point if some path gets there without the variable being assigned again." },
    available: { title: "Available expressions", in: "avail_in", out: "avail_out",
                 note: "Forward analysis, intersection at joins. An expression is available where every path has computed it and its operands have not changed since." },
};

function showDataflow(dataflow) {
    const panel = $("panel-dataflow");
    const names = dataflow.functions.map((f) => f.name);
    const draw = () => {
        const fn = dataflow.functions.find((f) => f.name === state.chosen.dataflow) || dataflow.functions[0];
        const which = ANALYSES[state.chosen.analysis] ? state.chosen.analysis : "live";
        const analysis = ANALYSES[which];
        const select = make("select", "picker");
        for (const [key, a] of Object.entries(ANALYSES)) select.append(new Option(a.title, key));
        select.value = which;
        select.addEventListener("change", () => {
            state.chosen.analysis = select.value;
            draw();
        });
        const controls = box("div", "controls", [make("label", "", "Function "), functionPicker("dataflow", names, draw),
                                                 make("label", "", " Analysis "), select]);
        const note = make("p", "note", `${analysis.note} The solver visited blocks ${fn.solver_visits[which]} times for ${fn.blocks.length} blocks.`);
        const t = table("grid sets", ["Block", "IN (at its start)", "OUT (at its end)"]);
        const chips = (items) => {
            const cell = make("td");
            if (!items.length) cell.append(make("span", "none", "empty"));
            for (const item of items) cell.append(make("span", "chip", item));
            return cell;
        };
        for (const block of fn.blocks) {
            const row = t.tBodies[0].insertRow();
            row.append(make("td", "kind", block.label), chips(block[analysis.in]), chips(block[analysis.out]));
        }
        panel.replaceChildren(controls, note, t);
    };
    draw();
}

// ---------------------------------------------------------------- optimizer passes

// The lines of two texts, marked as kept, removed or added (longest common subsequence).
function lineDiff(before, after) {
    const a = before.split("\n"), b = after.split("\n");
    const lcs = Array.from({ length: a.length + 1 }, () => new Array(b.length + 1).fill(0));
    for (let i = a.length - 1; i >= 0; i--) {
        for (let j = b.length - 1; j >= 0; j--) {
            lcs[i][j] = a[i] === b[j] ? lcs[i + 1][j + 1] + 1 : Math.max(lcs[i + 1][j], lcs[i][j + 1]);
        }
    }
    const out = [];
    let i = 0, j = 0;
    while (i < a.length || j < b.length) {
        if (i < a.length && j < b.length && a[i] === b[j]) { out.push([" ", a[i]]); i++; j++; }
        else if (j < b.length && (i === a.length || lcs[i][j + 1] >= lcs[i + 1][j])) { out.push(["+", b[j]]); j++; }
        else { out.push(["-", a[i]]); i++; }
    }
    return out;
}

function diffView(before, after) {
    const view = make("pre", "diff");
    for (const [mark, text] of lineDiff(before, after)) {
        view.append(make("span", mark === "+" ? "added" : mark === "-" ? "removed" : "", `${mark} ${text}\n`));
    }
    return view;
}

function showPasses(passes) {
    const panel = $("panel-passes");
    const summary = make("p", "note",
        passes.runs.length
            ? `The optimizer took the program from ${passes.instructions_before} to ${passes.instructions_after} instructions. ` +
              "Each entry below is one pass on one function, with what it changed and why. The program prints the same before and after every pass."
            : "The optimizer found nothing to change in this program.");
    const list = make("div", "runs");
    for (const run of passes.runs) {
        const entry = make("div", "run");
        entry.append(box("div", "run-title", [make("span", "kind", run.pass), make("span", "field", " in " + run.function)]));
        entry.append(make("div", "what", run.what));
        const notes = make("ul", "notes");
        for (const change of run.changes) {
            const item = make("li", "", (change.line ? `Line ${change.line}: ` : "") + change.text);
            if (change.line) pickable(item, change.line, 0, false);
            notes.append(item);
        }
        for (const missed of run.missed) {
            const item = make("li", "missed", "Not done" + (missed.line ? `, line ${missed.line}: ` : ": ") + missed.text);
            if (missed.line) pickable(item, missed.line, 0, false);
            notes.append(item);
        }
        entry.append(notes);
        if (run.before !== run.after) {
            const details = make("details");
            details.append(make("summary", "", "The function's IR before and after this pass"));
            details.addEventListener("toggle", () => {
                if (details.open && details.childElementCount === 1) details.append(diffView(run.before, run.after));
            });
            entry.append(details);
        }
        list.append(entry);
    }
    panel.replaceChildren(summary, list);
}

// ---------------------------------------------------------------- VM code

function showCode(code) {
    const panel = $("panel-code");
    const names = code.functions.map((f) => f.name);
    const draw = () => {
        const fn = code.functions.find((f) => f.name === state.chosen.code) || code.functions[0];
        const controls = box("div", "controls", [make("label", "", "Function "), functionPicker("code", names, draw)]);
        const spilled = fn.vars.filter((v) => v.spilled).length;
        const note = make("p", "note",
            `Each call of ${fn.name} gets ${code.registers} registers (the last three are kept for constants and spilled values) and ${fn.slots} frame slot${fn.slots === 1 ? "" : "s"}. ` +
            `${spilled ? spilled + " of its values did not fit in a register and live in a slot." : "Every value fits in a register."} ` +
            `The peephole step removed ${code.peephole_removed} instructions from the whole program.`);

        const t = table("grid code", ["At", "Instruction", "Source line"]);
        for (const instr of fn.code) {
            const row = t.tBodies[0].insertRow();
            row.id = `code-${fn.name}-${instr.pc}`;
            if (instr.line > 0) pickable(row, instr.line, 0, false);
            row.append(make("td", "pos", String(instr.pc)), make("td", "", instr.text), make("td", "pos", instr.line > 0 ? String(instr.line) : ""));
        }
        const where = table("grid", ["Value", "Type", "Lives in", "From", "To"]);
        for (const v of fn.vars) {
            where.tBodies[0].insertRow().append(make("td", "", v.name), make("td", "kind", v.type),
                make("td", v.spilled ? "spilled" : "", v.where), make("td", "pos", String(v.from)), make("td", "pos", String(v.to)));
        }
        panel.replaceChildren(controls, note, t,
                              make("p", "note", "Where the register allocator (linear scan) put each value, and between which instructions it is alive:"), where);
    };
    draw();
}
