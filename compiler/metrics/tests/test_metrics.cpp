#include <filesystem>
#include <numeric>
#include <source_location>
#include <string>

#include "common/test_framework.hpp"
#include "common/test_support.hpp"
#include "metrics/clang_bridge.hpp"
#include "metrics/complexity.hpp"
#include "metrics/measure.hpp"
#include "metrics/metrics.hpp"
#include "metrics/to_c.hpp"

using parc::metrics::BigO;
using parc::metrics::ComplexityReport;
using parc::metrics::FunctionComplexity;

namespace {

ComplexityReport complexity(const std::string& source) {
    parc::Compilation c = parc::compile(source);
    return parc::metrics::complexity_of_parc(c, "test.parc");
}

const FunctionComplexity* function(const ComplexityReport& r, const std::string& name) {
    for (const auto& f : r.functions)
        if (f.name == name) return &f;
    return nullptr;
}

// Checks one function's bounds; "" skips a bound.
void check_bounds(const ComplexityReport& r, const std::string& name, const std::string& time, const std::string& space,
                  std::source_location where = std::source_location::current()) {
    const FunctionComplexity* f = function(r, name);
    if (!f) {
        ++test::failures();
        std::cerr << where.file_name() << ":" << where.line() << ": no function " << name << " (" << r.error << ")\n";
        return;
    }
    if (!time.empty() && f->time.str() != time) {
        ++test::failures();
        std::cerr << where.file_name() << ":" << where.line() << ": " << name << " time " << f->time.str() << ", expected " << time << "\n";
    }
    if (!space.empty() && f->space.str() != space) {
        ++test::failures();
        std::cerr << where.file_name() << ":" << where.line() << ": " << name << " space " << f->space.str() << ", expected " << space << "\n";
    }
}

// Checks that a function has a suggestion of this kind, with these better bounds.
void check_suggestion(const ComplexityReport& r, const std::string& name, const std::string& kind, const std::string& time,
                      const std::string& space, std::source_location where = std::source_location::current()) {
    const FunctionComplexity* f = function(r, name);
    if (f)
        for (const auto& s : f->suggestions)
            if (s.kind == kind) {
                if (s.time_better == time && s.space_better == space) return;
                ++test::failures();
                std::cerr << where.file_name() << ":" << where.line() << ": " << kind << " in " << name << " gives time "
                          << s.time_better << ", space " << s.space_better << "; expected " << time << ", " << space << "\n";
                return;
            }
    ++test::failures();
    std::cerr << where.file_name() << ":" << where.line() << ": no " << kind << " suggestion in " << name << "\n";
}

bool has_suggestions(const ComplexityReport& r, const std::string& name) {
    const FunctionComplexity* f = function(r, name);
    return f && !f->suggestions.empty();
}

bool clang_available() {
    static const bool available = parc::metrics::parse_c("int f(void) { return 0; }\n", "probe.c", false).ok;
    return available;
}

}  // namespace

// ---------------------------------------------------------------- big-O arithmetic

TEST_CASE(bigo_arithmetic) {
    BigO n = BigO::of("n");
    CHECK_EQ(BigO::one().str(), "O(1)");
    CHECK_EQ(BigO::zero().str(), "O(1)");
    CHECK_EQ(n.times(n).str(), "O(n^2)");
    CHECK_EQ(n.times(n).plus(n).str(), "O(n^2)");
    CHECK_EQ(n.times(n.log()).str(), "O(n log n)");
    CHECK_EQ(n.sqrt().times(n).str(), "O(n sqrt(n))");
    CHECK_EQ(n.plus(BigO::of("m")).str(), "O(m + n)");
    CHECK_EQ(n.times(BigO::of("m")).str(), "O(m * n)");
    CHECK_EQ(n.times(n).substitute({{"n", BigO::of("len(a)")}}).str(), "O(len(a)^2)");
    CHECK_EQ(n.substitute({{"n", BigO::one()}}).str(), "O(1)");
    CHECK(n.times(n).grows_faster_than(n.times(n.log())));
    CHECK(!n.grows_faster_than(n));
    CHECK(!n.grows_faster_than(BigO::of("m")));
}

// ---------------------------------------------------------------- the C translation

TEST_CASE(every_example_parses_as_c) {
    if (!clang_available()) return;
    for (const auto& path : test::example_files()) {
        parc::Compilation c = parc::compile(test::read_file(path));
        std::string name = path.filename().string();
        parc::metrics::CUnit unit = parc::metrics::parse_c(parc::metrics::to_c(c.parsed.program, name), name, true);
        CHECK(unit.ok);
        CHECK_EQ(unit.functions.size(), c.parsed.program.functions.size());
        for (const auto& d : unit.diagnostics) {
            if (d.severity == "error") std::cerr << name << ":" << d.line << ": " << d.message << "\n";
            CHECK(d.severity != "error");
        }
    }
}

TEST_CASE(line_directives_give_parc_lines) {
    if (!clang_available()) return;
    std::string source = "fn main() {\n    let x: int = 1;\n\n    print(x);\n}\n";
    parc::Compilation c = parc::compile(source);
    std::string text = parc::metrics::to_c(c.parsed.program, "lines.parc");
    // Line 2 follows line 1 without a directive; line 4 comes after a blank line, so it needs one.
    CHECK(test::contains(text, "#line 1 \"lines.parc\""));
    CHECK(test::contains(text, "#line 4 \"lines.parc\""));
    parc::metrics::CUnit unit = parc::metrics::parse_c(text, "lines.parc", true);
    CHECK_EQ(unit.functions.size(), size_t{1});
    if (!unit.functions.empty()) {
        CHECK_EQ(unit.functions[0].name, "main");
        CHECK_EQ(unit.functions[0].line, 1);
    }
}

TEST_CASE(native_build_prints_what_the_vm_prints) {
    if (!clang_available()) return;
    namespace fs = std::filesystem;
    fs::path folder = fs::temp_directory_path() / "parc_test_metrics";
    for (const auto& path : test::example_files()) {
        std::string name = path.filename().string();
        parc::Compilation c = parc::compile(test::read_file(path));
        if (!c.runnable) continue;
        parc::vm::Result vm = parc::vm::run(c.code);
        if (!vm.ok || vm.races) continue;  // a runtime error, or a race whose output depends on the schedule
        parc::metrics::NativeBuild b = parc::metrics::build_native(parc::metrics::to_c(c.parsed.program, name), name, folder.string());
        CHECK(b.ok);
        if (!b.ok) {
            std::cerr << name << ": " << b.error << "\n";
            continue;
        }
        fs::path out = folder / "out.txt";
        parc::metrics::ProcessRun run = parc::metrics::run_process({b.exe}, out.string());
        CHECK_EQ(run.exit_code, 0);
        std::string got = test::read_file(out);
        std::erase(got, '\r');
        if (got != vm.output) std::cerr << name << ": the native program printed something else\n";
        CHECK_EQ(got, vm.output);
    }
    std::error_code ignored;
    fs::remove_all(folder, ignored);
}

TEST_CASE(remarks_point_at_parc_lines) {
    if (!clang_available()) return;
    std::filesystem::path path = std::filesystem::path(PARC_EXAMPLES_DIR) / "18_matrix.parc";
    parc::Compilation c = parc::compile(test::read_file(path));
    std::string folder = (std::filesystem::temp_directory_path() / "parc_test_remarks").string();
    parc::metrics::NativeBuild b = parc::metrics::build_native(parc::metrics::to_c(c.parsed.program, "18_matrix.parc"), "18_matrix.parc", folder);
    CHECK(b.ok);
    CHECK(!b.remarks.empty());
    int lines = test::count(test::read_file(path), "\n");
    for (const auto& r : b.remarks) {
        CHECK(r.line >= 1 && r.line <= lines);
        CHECK(!test::contains(r.message, "v_"));   // names are shown the ParC way
        CHECK(!test::contains(r.message, "''"));
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}

// ---------------------------------------------------------------- complexity

TEST_CASE(loops_and_nesting) {
    if (!clang_available()) return;
    ComplexityReport r = complexity(R"(
fn linear(n: int) -> int {
    let s: int = 0;
    for i in 0..n { s = s + i; }
    return s;
}
fn square(a: int[]) -> int {
    let s: int = 0;
    for i in 0..len(a) {
        for j in i..len(a) { s = s + a[i] * a[j]; }
    }
    return s;
}
fn counted(n: int) -> int {
    let i: int = 0;
    while (i < n) { i = i + 1; }
    return i;
}
fn halving(n: int) -> int {
    let steps: int = 0;
    while (n > 1) { n = n / 2; steps = steps + 1; }
    return steps;
}
fn fixed() -> int {
    let s: int = 0;
    for i in 0..10 { s = s + i; }
    return s;
}
fn main() {
    let a: int[] = new int[5];
    print(linear(5), square(a), counted(3), halving(8), fixed());
}
)");
    CHECK(r.ok);
    check_bounds(r, "linear", "O(n)", "O(1)");
    check_bounds(r, "square", "O(len(a)^2)", "O(1)");
    check_bounds(r, "counted", "O(n)", "O(1)");
    check_bounds(r, "halving", "O(log n)", "O(1)");
    check_bounds(r, "fixed", "O(1)", "O(1)");
    check_bounds(r, "main", "O(1)", "O(1)");
    const FunctionComplexity* sq = function(r, "square");
    CHECK(sq && sq->nesting == 2 && sq->loops.size() == 2);
}

TEST_CASE(examples_have_the_textbook_bounds) {
    if (!clang_available()) return;
    auto example = [](const char* file) {
        return complexity(test::read_file(std::filesystem::path(PARC_EXAMPLES_DIR) / file));
    };
    ComplexityReport fib = example("09_fibonacci.parc");
    check_bounds(fib, "fib_loop", "O(n)", "O(1)");
    check_bounds(fib, "fib_rec", "O(2^n)", "O(n)");
    check_suggestion(fib, "fib_rec", "exponential-recursion", "O(n)", "O(1)");
    CHECK(!has_suggestions(fib, "fib_loop"));

    ComplexityReport fact = example("08_factorial_recursion.parc");
    check_bounds(fact, "factorial", "O(n)", "O(n)");
    check_suggestion(fact, "factorial", "linear-recursion", "O(n)", "O(1)");

    ComplexityReport matrix = example("18_matrix.parc");
    check_bounds(matrix, "multiply", "O(n^3)", "O(n^2)");

    ComplexityReport sort = example("19_bubble_sort.parc");
    check_bounds(sort, "sort", "O(len(a)^2)", "O(1)");
    check_suggestion(sort, "sort", "quadratic-sort", "O(len(a) log len(a))", "O(1)");

    ComplexityReport sieve = example("10_primes_sieve.parc");
    check_bounds(sieve, "count_primes", "O(limit log limit)", "O(limit)");
    CHECK(!has_suggestions(sieve, "count_primes"));

    ComplexityReport dot = example("06_dot_parallel.parc");
    check_bounds(dot, "dot", "O(n)", "O(1)");
}

TEST_CASE(divide_and_conquer) {
    if (!clang_available()) return;
    ComplexityReport r = complexity(R"(
fn find(a: int[], x: int) -> int {
    let lo: int = 0;
    let hi: int = len(a) - 1;
    while (lo <= hi) {
        let mid: int = (lo + hi) / 2;
        if (a[mid] == x) { return mid; }
        if (a[mid] < x) { lo = mid + 1; } else { hi = mid - 1; }
    }
    return -1;
}
fn msort(a: int[], lo: int, hi: int) {
    if (hi - lo < 2) { return; }
    let mid: int = (lo + hi) / 2;
    msort(a, lo, mid);
    msort(a, mid, hi);
    for i in lo..hi { a[i] = a[i] + 0; }
}
fn power(x: int, n: int) -> int {
    if (n == 0) { return 1; }
    let half: int = power(x, n / 2);
    return half * half;
}
fn main() {
    let a: int[] = new int[8];
    msort(a, 0, len(a));
    print(find(a, 3), power(2, 10));
}
)");
    check_bounds(r, "find", "O(log len(a))", "O(1)");
    check_bounds(r, "msort", "O(len(a) log len(a))", "O(log len(a))");
    check_bounds(r, "power", "O(log n)", "O(log n)");
}

TEST_CASE(suggestions_with_better_bounds) {
    if (!clang_available()) return;
    ComplexityReport r = complexity(R"(
fn is_prime(x: int) -> bool {
    if (x < 2) { return false; }
    for d in 2..x {
        if (x % d == 0) { return false; }
    }
    return true;
}
fn running(a: int[]) {
    for i in 0..len(a) {
        let s: int = 0;
        for j in 0..i { s = s + a[j]; }
        print(s);
    }
}
fn total(a: int[]) -> int {
    let s: int = 0;
    for i in 0..len(a) { s = s + a[i]; }
    return s;
}
fn above(a: int[]) -> int {
    let c: int = 0;
    for i in 0..len(a) {
        if (a[i] * len(a) > total(a)) { c = c + 1; }
    }
    return c;
}
fn copies(n: int) {
    for i in 0..n {
        let t: int[] = new int[n];
        t[0] = i;
        print(t[0]);
    }
}
fn main() {
    let a: int[] = new int[4];
    running(a);
    print(is_prime(7), above(a));
    copies(3);
}
)");
    check_bounds(r, "is_prime", "O(x)", "O(1)");
    check_suggestion(r, "is_prime", "trial-division", "O(sqrt(x))", "O(1)");
    check_bounds(r, "running", "O(len(a)^2)", "O(1)");
    check_suggestion(r, "running", "prefix-sums", "O(len(a))", "O(len(a))");
    check_bounds(r, "above", "O(len(a)^2)", "O(1)");
    check_suggestion(r, "above", "invariant-call", "O(len(a))", "O(1)");
    check_bounds(r, "copies", "O(n^2)", "O(n^2)");
    check_suggestion(r, "copies", "allocation-in-loop", "O(n^2)", "O(n)");
    CHECK(!has_suggestions(r, "total"));
}

TEST_CASE(unknown_counts_are_shown_as_unknown) {
    if (!clang_available()) return;
    ComplexityReport r = complexity(R"(
fn spin(n: int) -> int {
    let k: int = n;
    while (true) {
        if (k == 1) { break; }
        if (k % 2 == 0) { k = k / 2; } else { k = 3 * k + 1; }
    }
    return k;
}
fn main() { print(spin(6)); }
)");
    const FunctionComplexity* f = function(r, "spin");
    CHECK(f && test::contains(f->time.str(), "?"));
    CHECK(f && !f->loops.empty() && !f->loops[0].note.empty());
}

TEST_CASE(complexity_of_a_c_file) {
    if (!clang_available()) return;
    std::filesystem::path path = std::filesystem::temp_directory_path() / "parc_test_complexity.c";
    {
        std::ofstream out(path);
        out << "int sum(int *a, int n) { int s = 0; for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) s += a[i] * a[j]; return s; }\n";
    }
    ComplexityReport r = parc::metrics::complexity_of_c(path.string());
    check_bounds(r, "sum", "O(n^2)", "O(1)");
    std::filesystem::remove(path);
}

// ---------------------------------------------------------------- measurement

TEST_CASE(clocks_and_energy_meter) {
    double t0 = parc::metrics::now_ms();
    double c0 = parc::metrics::cpu_ms();
    volatile double x = 0;
    for (int i = 0; i < 20'000'000; ++i) x = x + i * 0.5;
    CHECK(parc::metrics::now_ms() > t0);
    CHECK(parc::metrics::cpu_ms() >= c0);
    CHECK(parc::metrics::peak_memory_bytes() > 0);
    parc::metrics::EnergyMeter meter;
    CHECK(!meter.source().empty());
    if (meter.available()) {
        double e0 = meter.joules();
        CHECK(e0 > 0.0);
        double idle = parc::metrics::idle_power_w(meter, 300);
        CHECK(idle > 0.0 && idle < 1000.0);  // watts: a processor package, not a power station
    }
}

TEST_CASE(vm_counts_calls_depth_and_heap) {
    parc::Compilation c = parc::compile(R"(
fn f(n: int) -> int {
    if (n <= 1) { return 1; }
    return n * f(n - 1);
}
fn main() {
    let a: int[] = new int[6];
    let b: float[] = new float[4];
    a[0] = f(5);
    b[0] = 1.5;
    print(a[0], b[0]);
}
)");
    CHECK(c.runnable);
    parc::vm::Result r = parc::vm::run(c.code);
    CHECK(r.ok);
    CHECK_EQ(r.calls, 5LL);
    CHECK_EQ(r.max_depth, 6);  // main and five calls of f
    CHECK_EQ(r.heap_cells, 10LL);
    CHECK_EQ(std::accumulate(r.function_steps.begin(), r.function_steps.end(), 0LL), r.steps);
}

TEST_CASE(every_phase_is_timed) {
    parc::Compilation c = parc::compile("fn main() { print(1); }\n");
    std::vector<std::string> phases;
    for (const auto& t : c.times) {
        phases.push_back(t.phase);
        CHECK(t.ms >= 0.0);
    }
    CHECK_EQ(phases, (std::vector<std::string>{"scanner", "parser", "semantic", "dependence", "ir builder", "flow checks",
                                               "optimizer", "code generator"}));
}

TEST_CASE(metrics_report_end_to_end) {
    std::filesystem::path path = std::filesystem::path(PARC_EXAMPLES_DIR) / "09_fibonacci.parc";
    parc::Compilation c = parc::compile(test::read_file(path));
    parc::metrics::Options o;
    o.min_ms = 30;
    parc::metrics::Report r = parc::metrics::collect("09_fibonacci.parc", c, o);
    CHECK(r.vm.ran && r.vm.result.ok);
    CHECK(r.vm.cost.runs >= 1 && r.vm.cost.wall_ms > 0.0);
    CHECK(r.compile_ms > 0.0);
    if (clang_available()) {
        CHECK(r.native.ran);
        CHECK(r.native.same_output);
        CHECK(r.native.cost.wall_ms < r.vm.cost.wall_ms);
    }
    std::string text = parc::metrics::to_text(r);
    CHECK(test::contains(text, "compile time"));
    CHECK(test::contains(text, "fib_rec"));
    std::string json = parc::metrics::to_json(r);
    CHECK(test::contains(json, "\"complexity\""));
    CHECK(test::contains(json, "\"function_steps\""));
}

int main() { return test::run_all(); }
