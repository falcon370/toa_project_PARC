// ParC explorer, part 1 of 6: the editor, the tabs, and talking to the server.
//
//   app.js       this file
//   views.js     one function per tab that draws a phase's output
//   agent.js     the Hint, Explain and Fix buttons on findings
//   run.js       the Run tab and its step debugger
//   security.js  the Security tab
//   metrics.js   the Metrics tab: time, energy, memory, complexity, Clang's view
//
// Clicking anything that came from a place in the source (a token, a tree
// node, a finding, an IR instruction) highlights that place in the editor.

"use strict";

const $ = (id) => document.getElementById(id);
const source = $("source");
const gutterLines = $("gutter-lines");
const lineBand = $("line-band");
const markLayer = $("mark-layer");
const statusText = $("status");
const exampleSelect = $("example");

const TABS = ["tokens", "tree", "symbols", "findings", "ir", "dataflow", "passes", "code", "run", "metrics", "security"];
const LINE_HEIGHT = parseFloat(getComputedStyle(source).lineHeight);
const AUTO_COMPILE_MS = 500;

// What the page knows about the program in the editor.
const state = {
    compileId: null,
    build: null,          // the last /api/compile answer: every phase's JSON
    tokens: [],           // used to find how long a highlight should be
    errorLines: new Set(),
    warningLines: new Set(),
    chosen: {},           // which function each tab shows
};

let highlighted = null;   // {line, col, length, isError}
let selectedRow = null;
let requestNumber = 0;
let compileTimer = 0;

function make(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
}

// make("div", "x", [child, "text", ...]) for nested content.
function box(tag, className, children) {
    const node = make(tag, className);
    node.append(...children.filter((child) => child !== null && child !== undefined));
    return node;
}

// ---------------------------------------------------------------- editor

function drawGutter() {
    const count = source.value.split("\n").length;
    while (gutterLines.childElementCount < count) {
        gutterLines.append(make("div", "", String(gutterLines.childElementCount + 1)));
    }
    while (gutterLines.childElementCount > count) gutterLines.lastElementChild.remove();
    for (let i = 0; i < count; i++) {
        const row = gutterLines.children[i];
        row.classList.toggle("has-error", state.errorLines.has(i + 1));
        row.classList.toggle("has-warning", !state.errorLines.has(i + 1) && state.warningLines.has(i + 1));
        row.classList.toggle("current", highlighted !== null && highlighted.line === i + 1);
    }
}

// The gutter and the highlight layer do not scroll by themselves; they are
// shifted to follow the textarea.
function followScroll() {
    gutterLines.style.transform = `translateY(${-source.scrollTop}px)`;
    markLayer.style.transform = `translate(${-source.scrollLeft}px, ${-source.scrollTop}px)`;
    if (highlighted) lineBand.style.transform = `translateY(${(highlighted.line - 1) * LINE_HEIGHT - source.scrollTop}px)`;
}

function clearHighlight() {
    highlighted = null;
    lineBand.hidden = true;
    markLayer.replaceChildren();
    if (selectedRow) selectedRow.classList.remove("selected");
    selectedRow = null;
}

// The scanner counts one column per byte of UTF-8, so an "é" inside a string
// takes two columns there but is one character here.
function indexOfColumn(text, col) {
    let bytes = 0;
    let index = 0;
    for (const char of text) {
        if (bytes >= col - 1) break;
        const code = char.codePointAt(0);
        bytes += code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
        index += char.length;
    }
    return index;
}

// Line and column are 1-based, as the compiler reports them.
function highlight(line, col, length, isError) {
    const lines = source.value.split("\n");
    if (line < 1 || line > lines.length) return;
    highlighted = { line, col, length, isError };
    const text = lines[line - 1];
    const start = indexOfColumn(text, col);

    // The layer repeats the text up to the highlighted piece, so the browser
    // places the mark exactly where the textarea draws those characters.
    const mark = make("mark", isError ? "error" : "");
    // An error just past the end of a line (a missing ';') has no character to cover.
    mark.textContent = text.slice(start, start + length) || " ";
    markLayer.replaceChildren("\n".repeat(line - 1) + text.slice(0, start), mark);

    lineBand.hidden = false;
    lineBand.classList.toggle("error", isError);

    const top = (line - 1) * LINE_HEIGHT;
    if (top < source.scrollTop || top > source.scrollTop + source.clientHeight - 2 * LINE_HEIGHT) {
        source.scrollTop = Math.max(0, top - source.clientHeight / 3);
    }
    const left = mark.offsetLeft;
    if (left < source.scrollLeft + 20 || left + mark.offsetWidth > source.scrollLeft + source.clientWidth - 20) {
        source.scrollLeft = Math.max(0, left - source.clientWidth / 3);
    }
    followScroll();
    drawGutter();
}

// Highlights a whole line of code (from its first character that is not a space).
function highlightLine(line) {
    const text = source.value.split("\n")[line - 1];
    if (text === undefined) return;
    const indent = text.length - text.trimStart().length;
    highlight(line, indent + 1, Math.max(1, text.trim().length), false);
}

// Tree nodes and findings only know where they start. The token that starts
// at the same place tells how many characters to highlight.
function lengthAt(line, col) {
    const token = state.tokens.find((t) => t.line === line && t.col === col);
    return token && token.text.length > 0 ? token.text.length : 1;
}

function setSource(text) {
    source.value = text;
    source.scrollTop = 0;
    source.scrollLeft = 0;
    state.errorLines = new Set();
    state.warningLines = new Set();
    clearHighlight();
    drawGutter();
    followScroll();
}

source.addEventListener("scroll", followScroll);

source.addEventListener("input", () => {
    // Positions from the last compile no longer match the text.
    clearHighlight();
    drawGutter();
    clearTimeout(compileTimer);
    compileTimer = setTimeout(compile, AUTO_COMPILE_MS);
});

source.addEventListener("keydown", (event) => {
    if (event.key === "Enter" && (event.ctrlKey || event.metaKey)) {
        event.preventDefault();
        compile();
    } else if (event.key === "Tab" && !event.shiftKey && !event.ctrlKey && !event.altKey) {
        event.preventDefault();
        source.setRangeText("    ", source.selectionStart, source.selectionEnd, "end");
        source.dispatchEvent(new Event("input"));
    }
});

// ---------------------------------------------------------------- clicking results

// Every clickable row carries data-line and data-col (col 0 = the whole line).
function pickable(row, line, col, isError) {
    row.classList.add("pick");
    row.tabIndex = 0;
    row.dataset.line = line;
    row.dataset.col = col || 0;
    if (isError) row.dataset.error = "1";
    return row;
}

function pick(row) {
    const line = Number(row.dataset.line);
    const col = Number(row.dataset.col);
    if (!line) return;
    if (selectedRow) selectedRow.classList.remove("selected");
    if (col) {
        const length = row.dataset.length ? Number(row.dataset.length) : lengthAt(line, col);
        highlight(line, col, length, row.dataset.error === "1");
    } else {
        highlightLine(line);
    }
    selectedRow = row;
    row.classList.add("selected");
}

for (const name of TABS) {
    const panel = $("panel-" + name);
    panel.addEventListener("click", (event) => {
        if (event.target.closest("button, select, input, summary, a")) {
            const twist = event.target.closest(".twist");
            if (twist && !twist.classList.contains("leaf")) toggleTreeItem(twist);
            return;  // controls do their own thing
        }
        const row = event.target.closest(".pick");
        if (row) pick(row);
    });
    panel.addEventListener("keydown", (event) => {
        if ((event.key === "Enter" || event.key === " ") && event.target.classList.contains("pick")) {
            event.preventDefault();
            pick(event.target);
        }
    });
}

for (const tab of document.querySelectorAll("#tabs [data-tab]")) {
    tab.addEventListener("click", () => showTab(tab.dataset.tab));
}

function showTab(name) {
    for (const tab of document.querySelectorAll("#tabs [data-tab]")) {
        tab.setAttribute("aria-selected", String(tab.dataset.tab === name));
    }
    for (const tab of TABS) $("panel-" + tab).hidden = tab !== name;
    if (name === "security") refreshSecurityLog();  // attacks may have been detected in the Findings tab meanwhile
}

// A drop-down that picks which function a tab shows. The choice is kept
// across compiles, by name.
function functionPicker(tab, names, onChange) {
    const select = make("select", "picker");
    for (const name of names) select.append(new Option(name, name));
    if (names.includes(state.chosen[tab])) select.value = state.chosen[tab];
    state.chosen[tab] = select.value;
    select.addEventListener("change", () => {
        state.chosen[tab] = select.value;
        onChange();
    });
    return select;
}

function emptyPanel(name, text) {
    $("panel-" + name).replaceChildren(make("p", "empty", text));
}

// ---------------------------------------------------------------- talking to the server

function setStatus(text, bad) {
    statusText.textContent = text;
    statusText.classList.toggle("bad", Boolean(bad));
}

async function postJson(url, body) {
    const response = await fetch(url, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || `the server answered ${response.status}`);
    return result;
}

async function compile() {
    clearTimeout(compileTimer);
    const mine = ++requestNumber;
    setStatus("Compiling...");
    let result;
    try {
        result = await postJson("/api/compile", { source: source.value });
    } catch (problem) {
        if (mine === requestNumber) setStatus("Could not compile: " + problem.message, true);
        return;
    }
    if (mine !== requestNumber) return;  // a newer compile was started while this one ran

    clearHighlight();
    state.compileId = result.compile_id;
    state.build = result;
    state.tokens = result.tokens.tokens;
    showEverything(result);
    drawGutter();

    const findings = result.findings.findings;
    const errors = result.errors.length;
    const warnings = findings.filter((f) => f.severity === "warning").length;
    const plural = (n, word) => `${n} ${word}${n === 1 ? "" : "s"}`;
    if (errors) setStatus(plural(errors, "error"), true);
    else if (warnings) setStatus(`Compiled, with ${plural(warnings, "warning")}`);
    else setStatus(`Compiled: ${plural(state.tokens.length, "token")}, ${plural(result.ast.functions.length, "function")}, no warnings`);
}

// Draws every tab from one compile result. Tabs of phases that did not run
// say which errors stopped the compiler.
function showEverything(build) {
    showTokens(build.tokens.tokens);
    showTree(build.ast);
    showFindings(build);
    const stopped = build.stage === "syntax"
        ? "The program has syntax errors, so the compiler stopped after the parser. See the Findings tab."
        : "The program has errors, so the compiler stopped before this phase. See the Findings tab.";
    if (build.symbols) showSymbols(build.symbols); else emptyPanel("symbols", stopped);
    if (build.ir) showIr(build); else emptyPanel("ir", stopped);
    if (build.dataflow) showDataflow(build.dataflow); else emptyPanel("dataflow", stopped);
    if (build.passes) showPasses(build.passes); else emptyPanel("passes", stopped);
    if (build.code) showCode(build.code); else emptyPanel("code", stopped);
    showRun(build);
    showMetrics(build);
    showSecurity(build);
}

async function loadExample(name) {
    try {
        const response = await fetch("/api/examples/" + encodeURIComponent(name));
        const result = await response.json();
        if (!response.ok) throw new Error(result.error);
        setSource(result.source);
        await compile();
    } catch (problem) {
        setStatus("Could not load the example: " + problem.message, true);
    }
}

async function start() {
    $("compile").addEventListener("click", compile);
    exampleSelect.addEventListener("change", () => loadExample(exampleSelect.value));
    drawGutter();
    await loadAgentStatus();
    try {
        const response = await fetch("/api/examples");
        const result = await response.json();
        for (const name of result.examples) exampleSelect.append(new Option(name, name));
        if (result.examples.includes(result.default)) {
            exampleSelect.value = result.default;
            await loadExample(result.default);
        }
    } catch (problem) {
        setStatus("Could not reach the server: " + problem.message, true);
    }
}
