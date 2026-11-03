#include "metrics/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "common/json_writer.hpp"
#include "metrics/to_c.hpp"

namespace parc::metrics {

namespace {

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string without_cr(std::string text) {
    std::erase(text, '\r');
    return text;
}

// Repeats `work` until it has run for at least min_ms, and measures the repetitions.
template <typename Work>
Measurement measure(Work&& work, double first_ms, double min_ms, int max_runs, EnergyMeter& meter, double idle_w,
                    bool energy) {
    Measurement m;
    m.runs = static_cast<int>(std::clamp(std::ceil(min_ms / std::max(first_ms, 0.001)), 1.0, static_cast<double>(max_runs)));
    double wall = 0.0;
    double cpu = 0.0;
    double joules = 0.0;
    // The first run is slow (cold caches), so the count it suggests can be far too low: measure
    // again with more runs until the measurement lasts long enough.
    for (int attempt = 0; attempt < 4; ++attempt) {
        double e0 = energy ? meter.joules() : 0.0;
        double c0 = cpu_ms();
        double w0 = now_ms();
        for (int i = 0; i < m.runs; ++i) work();
        wall = now_ms() - w0;
        cpu = cpu_ms() - c0;
        joules = energy ? meter.joules() - e0 : 0.0;
        if (wall >= 0.8 * min_ms || m.runs >= max_runs) break;
        double factor = wall > 0.5 ? 1.2 * min_ms / wall : 100.0;
        m.runs = static_cast<int>(std::min<double>(max_runs, std::ceil(m.runs * std::clamp(factor, 1.5, 1000.0))));
    }
    m.wall_ms = wall / m.runs;
    // Windows counts processor time in ticks of 15.6 ms; less than a few ticks says nothing.
    m.cpu_ms = cpu >= 50.0 ? cpu / m.runs : -1.0;
    if (energy && joules > 0.0 && wall > 0.0) {
        m.has_energy = true;
        m.energy_j = joules / m.runs;
        m.power_w = joules / (wall / 1000.0);
        m.idle_w = idle_w;
        m.above_idle_j = std::max(0.0, m.energy_j - idle_w * m.wall_ms / 1000.0);
    }
    return m;
}

void measure_native(Report& r, const Compilation& c, const std::string& file, const Options& o, EnergyMeter& meter) {
    namespace fs = std::filesystem;
    NativeMeasurement& n = r.native;
    fs::path folder = o.work_dir.empty()
                          ? fs::temp_directory_path() / ("parc_metrics_" + std::to_string(static_cast<long long>(now_ms())))
                          : fs::path(o.work_dir);
    n.build = build_native(to_c(c.parsed.program, file), file, folder.string());
    if (!n.build.ok) {
        n.error = n.build.error;
    } else {
        fs::path out = folder / "output.txt";
        // Start-up alone: the program with its main run zero times.
        n.startup_ms = 1e300;
        for (int i = 0; i < 3; ++i) n.startup_ms = std::min(n.startup_ms, run_process({n.build.exe, "0"}).wall_ms);
        ProcessRun once = run_process({n.build.exe, "1"}, out.string());
        n.output = without_cr(read_text(out));
        n.same_output = n.output == r.vm.result.output;
        n.peak_memory_bytes = once.peak_memory_bytes;
        if (!once.started || once.exit_code != 0) {
            n.error = once.started ? "the native program stopped with exit code " + std::to_string(once.exit_code) : once.error;
        } else {
            // The program repeats ParC's main itself, so one process measures many runs. Starting a
            // process takes ~100 ms give or take tens, so the count grows until the runs clearly
            // outweigh that. Nothing is freed, so the memory the runs allocate caps the count.
            long long bytes_per_run = r.vm.result.heap_cells * 8 + r.vm.result.heap_arrays * 48 + 64;
            int memory_cap = static_cast<int>(std::clamp(512'000'000LL / bytes_per_run, 1LL, 20'000'000LL));
            int runs = 1;
            for (int attempt = 0; attempt < 8 && runs < memory_cap; ++attempt) {
                double extra = run_process({n.build.exe, std::to_string(runs)}).wall_ms - n.startup_ms;
                if (extra >= o.min_ms) break;
                double factor = extra > 20.0 ? 1.3 * o.min_ms / extra : 100.0;
                runs = static_cast<int>(std::min<double>(memory_cap, std::ceil(runs * std::clamp(factor, 2.0, 1000.0))));
            }
            bool energy = o.energy && meter.available();
            double e0 = energy ? meter.joules() : 0.0;
            ProcessRun many = run_process({n.build.exe, std::to_string(runs)});
            double e1 = energy ? meter.joules() : 0.0;
            ProcessRun empty = run_process({n.build.exe, "0"});
            double e2 = energy ? meter.joules() : 0.0;
            Measurement& m = n.cost;
            m.runs = runs;
            m.wall_ms = std::max(0.0, many.wall_ms - std::min(empty.wall_ms, n.startup_ms)) / runs;
            double cpu = many.cpu_ms - empty.cpu_ms;
            m.cpu_ms = cpu >= 50.0 ? cpu / runs : -1.0;
            double joules = (e1 - e0) - (e2 - e1);
            if (energy && e1 - e0 > 0.0 && many.wall_ms > 0.0) {
                m.has_energy = true;
                m.energy_j = std::max(0.0, joules) / runs;
                m.power_w = (e1 - e0) / (many.wall_ms / 1000.0);
                m.idle_w = r.idle_w;
                m.above_idle_j = std::max(0.0, m.energy_j - r.idle_w * m.wall_ms / 1000.0);
            }
            n.ran = true;
        }
    }
    if (o.work_dir.empty()) {
        std::error_code ignored;
        fs::remove_all(folder, ignored);
    }
}

// ------------------------------------------------------------ text

std::string fixed(double v, int digits) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", digits, v);
    return buf;
}

std::string ms(double v) {
    if (v >= 100) return fixed(v, 0) + " ms";
    if (v >= 1) return fixed(v, 2) + " ms";
    if (v >= 0.001) return fixed(v * 1000.0, 2) + " us";
    return fixed(v * 1e6, 1) + " ns";
}

std::string joules(double j) {
    if (j >= 1) return fixed(j, 2) + " J";
    if (j >= 1e-3) return fixed(j * 1e3, 2) + " mJ";
    if (j >= 1e-6) return fixed(j * 1e6, 2) + " uJ";
    return fixed(j * 1e9, 1) + " nJ";
}

std::string bytes(long long b) {
    if (b >= 1 << 20) return fixed(static_cast<double>(b) / (1 << 20), 1) + " MB";
    if (b >= 1 << 10) return fixed(static_cast<double>(b) / (1 << 10), 1) + " KB";
    return std::to_string(b) + " bytes";
}

std::string pad(std::string s, size_t width) {
    if (s.size() < width) s += std::string(width - s.size(), ' ');
    return s;
}

std::string energy_line(const Measurement& m) {
    if (!m.has_energy) return "not measured";
    return joules(m.energy_j) + " per run; package power " + fixed(m.power_w, 1) + " W (idle " + fixed(m.idle_w, 1) +
           " W), " + joules(m.above_idle_j) + " above idle";
}

// ------------------------------------------------------------ json

void measurement_json(json::Writer& w, const Measurement& m) {
    w.begin_object();
    w.key("runs");
    w.integer(m.runs);
    w.key("wall_ms");
    w.number(m.wall_ms);
    w.key("cpu_ms");
    w.number(m.cpu_ms);
    w.key("has_energy");
    w.boolean(m.has_energy);
    if (m.has_energy) {
        w.key("energy_j");
        w.number(m.energy_j);
        w.key("power_w");
        w.number(m.power_w);
        w.key("idle_w");
        w.number(m.idle_w);
        w.key("above_idle_j");
        w.number(m.above_idle_j);
    }
    w.end_object();
}

void complexity_body(json::Writer& w, const ComplexityReport& report) {
    w.key("ok");
    w.boolean(report.ok);
    if (!report.error.empty()) {
        w.key("error");
        w.string(report.error);
    }
    w.key("functions");
    w.begin_array();
    for (const auto& f : report.functions) {
        w.begin_object();
        w.key("name");
        w.string(f.name);
        w.key("line");
        w.integer(f.line);
        w.key("time");
        w.string(f.time.str());
        w.key("space");
        w.string(f.space.str());
        w.key("recursion");
        w.string(f.recursion);
        w.key("nesting");
        w.integer(f.nesting);
        w.key("pure");
        w.boolean(f.pure);
        w.key("sizes");
        w.begin_object();
        for (const auto& [s, what] : f.sizes) {
            w.key(s);
            w.string(what);
        }
        w.end_object();
        w.key("loops");
        w.begin_array();
        for (const auto& l : f.loops) {
            w.begin_object();
            w.key("line");
            w.integer(l.line);
            w.key("kind");
            w.string(l.kind);
            w.key("depth");
            w.integer(l.depth);
            w.key("runs");
            w.string(l.runs);
            w.key("note");
            w.string(l.note);
            w.end_object();
        }
        w.end_array();
        w.key("notes");
        w.begin_array();
        for (const auto& n : f.notes) w.string(n);
        w.end_array();
        w.key("suggestions");
        w.begin_array();
        for (const auto& s : f.suggestions) {
            w.begin_object();
            w.key("kind");
            w.string(s.kind);
            w.key("line");
            w.integer(s.line);
            w.key("title");
            w.string(s.title);
            w.key("why");
            w.string(s.why);
            w.key("how");
            w.string(s.how);
            w.key("time_now");
            w.string(s.time_now);
            w.key("time_better");
            w.string(s.time_better);
            w.key("space_now");
            w.string(s.space_now);
            w.key("space_better");
            w.string(s.space_better);
            w.end_object();
        }
        w.end_array();
        w.end_object();
    }
    w.end_array();
}

}  // namespace

ComplexityReport complexity_of_parc(const Compilation& c, const std::string& file, CUnit* unit) {
    CUnit parsed = parse_c(to_c(c.parsed.program, file), file, true);
    ComplexityReport r = analyze_complexity(parsed);
    if (unit) *unit = std::move(parsed);
    return r;
}

ComplexityReport complexity_of_c(const std::string& path, CUnit* unit) {
    CUnit parsed = parse_c(read_text(path), path, false);
    ComplexityReport r = analyze_complexity(parsed);
    if (unit) *unit = std::move(parsed);
    return r;
}

Report collect(const std::string& file, const Compilation& c, const Options& o) {
    Report r;
    r.file = file;
    r.stage = c.stage();
    r.compile_times = c.times;
    for (const auto& t : c.times) r.compile_ms += t.ms;
    r.ir_before = c.passes.instructions_before;
    r.ir_after = c.passes.instructions_after;

    EnergyMeter meter;
    r.energy_available = meter.available();
    r.energy_source = meter.source();
    bool energy = o.energy && meter.available();
    if (energy) r.idle_w = idle_power_w(meter);

    if (c.analyzed) r.complexity = complexity_of_parc(c, file, &r.clang);

    if (c.runnable) {
        for (const auto& fn : c.code.functions) r.functions.push_back(fn.name);
        VmMeasurement& v = r.vm;
        double start = now_ms();
        v.result = vm::run(c.code, o.run);
        double first = now_ms() - start;
        v.ran = true;
        v.cost = measure([&] { (void)vm::run(c.code, o.run); }, first, o.min_ms, 100'000, meter, r.idle_w, energy);
        v.peak_memory_bytes = peak_memory_bytes();
        if (o.native && v.result.ok) measure_native(r, c, file, o, meter);
        else if (o.native) r.native.error = "the program stopped with a runtime error on the VM, so it is not run natively";
    }
    return r;
}

std::string complexity_text(const ComplexityReport& report) {
    std::string out;
    if (!report.ok) return "   not available: " + report.error + "\n";
    size_t width = 8;
    for (const auto& f : report.functions) width = std::max(width, f.name.size() + 2);
    for (const auto& f : report.functions) {
        out += "   " + pad(f.name, width) + "time " + pad(f.time.str(), 20) + " space " + pad(f.space.str(), 14);
        if (!f.recursion.empty()) out += " recursion: " + f.recursion;
        out += "\n";
        for (const auto& l : f.loops)
            if (!l.note.empty())
                out += "   " + pad("", width) + "line " + std::to_string(l.line) + ", " + l.kind + " loop runs " + l.runs +
                       ": " + l.note + "\n";
        for (const auto& n : f.notes) out += "   " + pad("", width) + n + "\n";
        for (const auto& s : f.suggestions) {
            out += "   " + pad("", width) + "can be: time " + pad(s.time_better, 15) + " space " + pad(s.space_better, 9) +
                   " " + s.title + " (line " + std::to_string(s.line) + ")\n";
            out += "   " + pad("", width) + "        because " + s.why + ". " + s.how + "\n";
        }
    }
    return out;
}

std::string to_text(const Report& r) {
    std::string out;
    out += "== compile time: ParC's own phases, " + ms(r.compile_ms) + " in total\n";
    for (const auto& t : r.compile_times) out += "   " + pad(t.phase, 16) + ms(t.ms) + "\n";
    if (r.ir_before) out += "   the optimizer took the IR from " + std::to_string(r.ir_before) + " to " + std::to_string(r.ir_after) + " instructions\n";
    if (r.clang.ok) out += "   for comparison, libclang (" + r.clang.version + ") parsed the program as C in " + ms(r.clang.parse_ms) + "\n";

    if (r.vm.ran) {
        const vm::Result& res = r.vm.result;
        const Measurement& m = r.vm.cost;
        out += "== run on the VM (" + std::to_string(m.runs) + " runs measured)\n";
        if (!res.ok) out += "   stopped: " + res.error + " (line " + std::to_string(res.error_line) + ")\n";
        out += "   time     " + ms(m.wall_ms) + " per run, processor time " +
               (m.cpu_ms >= 0 ? ms(m.cpu_ms) : std::string("too short to measure (raise --min-ms)")) + "\n";
        double rate = m.wall_ms > 0 ? static_cast<double>(res.steps) / (m.wall_ms / 1000.0) / 1e6 : 0.0;
        out += "   work     " + std::to_string(res.steps) + " VM instructions (" + fixed(rate, 1) + " million a second), " +
               std::to_string(res.calls) + (res.calls == 1 ? " call" : " calls") + ", deepest call stack " +
               std::to_string(res.max_depth) + "\n";
        out += "   memory   " + std::to_string(res.heap_cells) + " array elements on the VM heap (" +
               bytes(res.heap_cells * static_cast<long long>(sizeof(vm::Value))) + "); parc's peak working set " +
               bytes(r.vm.peak_memory_bytes) + "\n";
        out += "   energy   " + (r.energy_available ? energy_line(m) : "not available: " + r.energy_source) + "\n";
        if (m.has_energy && res.steps > 0)
            out += "            " + joules(m.energy_j / static_cast<double>(res.steps)) + " per VM instruction\n";
        // Where the instructions went: a profile by function.
        std::vector<std::pair<long long, size_t>> hot;
        for (size_t i = 0; i < res.function_steps.size() && i < r.functions.size(); ++i)
            if (res.function_steps[i] > 0) hot.push_back({res.function_steps[i], i});
        std::sort(hot.rbegin(), hot.rend());
        std::string names;
        for (size_t i = 0; i < hot.size() && i < 5; ++i)
            names += (i ? ", " : "") + r.functions[hot[i].second] + " " +
                     fixed(100.0 * static_cast<double>(hot[i].first) / static_cast<double>(std::max(1LL, res.steps)), 1) + "%";
        if (!names.empty()) out += "   profile  " + names + " of the instructions\n";
    }

    if (!r.native.build.clang.empty() || !r.native.error.empty()) {
        const NativeMeasurement& n = r.native;
        out += "== native program from clang -O2";
        if (n.ran) out += " (" + std::to_string(n.cost.runs) + " runs in one process; start-up " + ms(n.startup_ms) + " not counted)";
        out += "\n";
        if (n.build.compile_ms > 0) out += "   compile  clang -O2 took " + ms(n.build.compile_ms) + "\n";
        if (n.ran) {
            out += "   time     " + ms(n.cost.wall_ms) + " per run";
            if (n.cost.wall_ms > 0 && r.vm.cost.wall_ms > 0)
                out += ", " + fixed(r.vm.cost.wall_ms / n.cost.wall_ms, 0) + " times faster than the VM";
            out += "\n";
            out += "   energy   " + (r.energy_available ? energy_line(n.cost) : "not available") + "\n";
            out += "   memory   peak working set " + bytes(n.peak_memory_bytes) + "\n";
            out += std::string("   output   ") + (n.same_output ? "the same as the VM's" : "DIFFERENT from the VM's") + "\n";
        }
        if (!n.error.empty()) out += "   note     " + n.error + "\n";
    }

    out += "== complexity per function, from Clang's syntax tree\n";
    out += complexity_text(r.complexity);

    if (!r.native.build.remarks.empty()) {
        out += "== Clang's optimizer at -O2 (its remarks, by ParC line)\n";
        int shown = 0;
        for (const auto& rm : r.native.build.remarks) {
            if (++shown > 25) {
                out += "   ... " + std::to_string(r.native.build.remarks.size() - 25) + " more in the JSON output\n";
                break;
            }
            out += "   line " + pad(std::to_string(rm.line), 4) + pad(rm.pass + " " + rm.type, 24) + rm.message + "\n";
        }
    }
    if (r.clang.ok) {
        out += "== Clang's warnings on the same program (-Wall -Wextra)\n";
        if (r.clang.diagnostics.empty()) out += "   none\n";
        for (const auto& d : r.clang.diagnostics)
            out += "   line " + pad(std::to_string(d.line), 4) + d.severity + ": " + d.message +
                   (d.option.empty() ? "" : " [" + d.option + "]") + "\n";
    }
    return out;
}

std::string complexity_json(const ComplexityReport& report, const std::string& file) {
    json::Writer w;
    w.begin_object();
    w.key("file");
    w.string(file);
    complexity_body(w, report);
    w.end_object();
    return w.str() + "\n";
}

std::string to_json(const Report& r) {
    json::Writer w;
    w.begin_object();
    w.key("file");
    w.string(r.file);
    w.key("stage");
    w.string(r.stage);

    w.key("compile");
    w.begin_object();
    w.key("total_ms");
    w.number(r.compile_ms);
    w.key("phases");
    w.begin_array();
    for (const auto& t : r.compile_times) {
        w.begin_object();
        w.key("phase");
        w.string(t.phase);
        w.key("ms");
        w.number(t.ms);
        w.end_object();
    }
    w.end_array();
    w.key("ir_before");
    w.integer(r.ir_before);
    w.key("ir_after");
    w.integer(r.ir_after);
    w.end_object();

    w.key("energy");
    w.begin_object();
    w.key("available");
    w.boolean(r.energy_available);
    w.key("source");
    w.string(r.energy_source);
    w.key("idle_w");
    w.number(r.idle_w);
    w.end_object();

    w.key("vm");
    w.begin_object();
    w.key("ran");
    w.boolean(r.vm.ran);
    if (r.vm.ran) {
        const vm::Result& res = r.vm.result;
        w.key("ok");
        w.boolean(res.ok);
        w.key("error");
        w.string(res.error);
        w.key("steps");
        w.integer(res.steps);
        w.key("calls");
        w.integer(res.calls);
        w.key("max_depth");
        w.integer(res.max_depth);
        w.key("heap_cells");
        w.integer(res.heap_cells);
        w.key("heap_arrays");
        w.integer(res.heap_arrays);
        w.key("heap_bytes");
        w.integer(res.heap_cells * static_cast<long long>(sizeof(vm::Value)));
        w.key("peak_memory_bytes");
        w.integer(r.vm.peak_memory_bytes);
        w.key("function_steps");
        w.begin_object();
        for (size_t i = 0; i < res.function_steps.size() && i < r.functions.size(); ++i) {
            w.key(r.functions[i]);
            w.integer(res.function_steps[i]);
        }
        w.end_object();
        w.key("cost");
        measurement_json(w, r.vm.cost);
    }
    w.end_object();

    w.key("native");
    w.begin_object();
    w.key("ran");
    w.boolean(r.native.ran);
    w.key("error");
    w.string(r.native.error);
    w.key("compile_ms");
    w.number(r.native.build.compile_ms);
    if (r.native.ran) {
        w.key("startup_ms");
        w.number(r.native.startup_ms);
        w.key("same_output");
        w.boolean(r.native.same_output);
        w.key("peak_memory_bytes");
        w.integer(r.native.peak_memory_bytes);
        w.key("cost");
        measurement_json(w, r.native.cost);
        w.key("speedup");
        w.number(r.native.cost.wall_ms > 0 ? r.vm.cost.wall_ms / r.native.cost.wall_ms : 0.0);
    }
    w.key("remarks");
    w.begin_array();
    for (const auto& rm : r.native.build.remarks) {
        w.begin_object();
        w.key("line");
        w.integer(rm.line);
        w.key("type");
        w.string(rm.type);
        w.key("pass");
        w.string(rm.pass);
        w.key("name");
        w.string(rm.name);
        w.key("function");
        w.string(rm.function);
        w.key("message");
        w.string(rm.message);
        w.end_object();
    }
    w.end_array();
    w.end_object();

    w.key("clang");
    w.begin_object();
    w.key("ok");
    w.boolean(r.clang.ok);
    w.key("error");
    w.string(r.clang.error);
    w.key("version");
    w.string(r.clang.version);
    w.key("parse_ms");
    w.number(r.clang.parse_ms);
    w.key("warnings");
    w.begin_array();
    for (const auto& d : r.clang.diagnostics) {
        w.begin_object();
        w.key("line");
        w.integer(d.line);
        w.key("col");
        w.integer(d.col);
        w.key("severity");
        w.string(d.severity);
        w.key("message");
        w.string(d.message);
        w.key("option");
        w.string(d.option);
        w.end_object();
    }
    w.end_array();
    w.end_object();

    w.key("complexity");
    w.begin_object();
    complexity_body(w, r.complexity);
    w.end_object();

    w.end_object();
    return w.str() + "\n";
}

}  // namespace parc::metrics
