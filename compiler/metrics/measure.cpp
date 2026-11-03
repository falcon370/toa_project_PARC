#include "metrics/measure.hpp"

#include <chrono>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <pdh.h>
#include <psapi.h>
#else
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fstream>
#endif

namespace parc::metrics {

double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

#ifdef _WIN32

namespace {

// FILETIME counts 100-nanosecond steps.
double filetime_ms(const FILETIME& t) {
    ULARGE_INTEGER v;
    v.LowPart = t.dwLowDateTime;
    v.HighPart = t.dwHighDateTime;
    return static_cast<double>(v.QuadPart) / 10'000.0;
}

double process_cpu_ms(HANDLE process) {
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0.0;
    return filetime_ms(kernel) + filetime_ms(user);
}

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

// One argument as CreateProcess's command line expects it: quoted when it
// has spaces, with backslashes doubled only where they come before a quote.
std::wstring quote_arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += ch;
            backslashes = 0;
            continue;
        } else {
            backslashes = 0;
        }
        out += ch;
    }
    out.append(backslashes, L'\\');
    return out + L"\"";
}

HANDLE open_output(const std::string& path) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    std::wstring name = path.empty() ? L"NUL" : wide(path);
    return CreateFileW(name.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

}  // namespace

double cpu_ms() { return process_cpu_ms(GetCurrentProcess()); }

long long peak_memory_bytes() {
    PROCESS_MEMORY_COUNTERS pmc{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return 0;
    return static_cast<long long>(pmc.PeakWorkingSetSize);
}

// The counter counts picowatt-hours: 1 pWh = 3.6e-9 J.
constexpr double kJoulesPerUnit = 3.6e-9;

EnergyMeter::EnergyMeter() {
    PDH_HQUERY query = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
        source_ = "the performance counter service is not available";
        return;
    }
    PDH_HCOUNTER counter = nullptr;
    if (PdhAddEnglishCounterW(query, L"\\Energy Meter(RAPL_Package0_PKG)\\Energy", 0, &counter) != ERROR_SUCCESS ||
        PdhCollectQueryData(query) != ERROR_SUCCESS) {
        PdhCloseQuery(query);
        source_ = "this machine has no RAPL energy counter (Windows \"Energy Meter\")";
        return;
    }
    query_ = query;
    counter_ = counter;
    available_ = true;
    source_ = "RAPL package energy, Windows performance counter \\Energy Meter(RAPL_Package0_PKG)\\Energy";
}

EnergyMeter::~EnergyMeter() {
    if (query_) PdhCloseQuery(static_cast<PDH_HQUERY>(query_));
}

double EnergyMeter::joules() {
    if (!available_) return 0.0;
    PDH_RAW_COUNTER raw{};
    if (PdhCollectQueryData(static_cast<PDH_HQUERY>(query_)) != ERROR_SUCCESS ||
        PdhGetRawCounterValue(static_cast<PDH_HCOUNTER>(counter_), nullptr, &raw) != ERROR_SUCCESS)
        return 0.0;
    return static_cast<double>(raw.FirstValue) * kJoulesPerUnit;
}

ProcessRun run_process(const std::vector<std::string>& command, const std::string& stdout_path,
                       const std::string& stderr_path) {
    ProcessRun result;
    std::wstring line;
    for (const auto& arg : command) line += (line.empty() ? L"" : L" ") + quote_arg(wide(arg));

    HANDLE out = open_output(stdout_path);
    HANDLE err = open_output(stderr_path);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = out;
    si.hStdError = err;
    PROCESS_INFORMATION pi{};
    double start = now_ms();
    BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (ok) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        result.wall_ms = now_ms() - start;
        result.started = true;
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        result.exit_code = static_cast<int>(code);
        result.cpu_ms = process_cpu_ms(pi.hProcess);
        PROCESS_MEMORY_COUNTERS pmc{};
        if (GetProcessMemoryInfo(pi.hProcess, &pmc, sizeof pmc))
            result.peak_memory_bytes = static_cast<long long>(pmc.PeakWorkingSetSize);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        result.error = "could not start " + (command.empty() ? std::string("a program") : command[0]) +
                       " (Windows error " + std::to_string(GetLastError()) + ")";
    }
    for (HANDLE h : {out, err, in})
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return result;
}

#else  // POSIX

namespace {

double rusage_ms(int who) {
    rusage u{};
    getrusage(who, &u);
    return (u.ru_utime.tv_sec + u.ru_stime.tv_sec) * 1000.0 + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1000.0;
}

const char* kRaplFile = "/sys/class/powercap/intel-rapl:0/energy_uj";

}  // namespace

double cpu_ms() { return rusage_ms(RUSAGE_SELF); }

long long peak_memory_bytes() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return static_cast<long long>(u.ru_maxrss) * 1024;  // Linux reports kilobytes
}

EnergyMeter::EnergyMeter() {
    std::ifstream in(kRaplFile);
    long long value = 0;
    if (in >> value) {
        available_ = true;
        source_ = std::string("RAPL package energy, ") + kRaplFile;
    } else {
        source_ = std::string("cannot read ") + kRaplFile + " (no RAPL, or it needs root)";
    }
}

EnergyMeter::~EnergyMeter() = default;

double EnergyMeter::joules() {
    std::ifstream in(kRaplFile);
    long long value = 0;
    return in >> value ? static_cast<double>(value) * 1e-6 : 0.0;
}

ProcessRun run_process(const std::vector<std::string>& command, const std::string& stdout_path,
                       const std::string& stderr_path) {
    ProcessRun result;
    if (command.empty()) {
        result.error = "no program to run";
        return result;
    }
    double cpu_before = rusage_ms(RUSAGE_CHILDREN);
    double start = now_ms();
    pid_t pid = fork();
    if (pid == 0) {
        int out = open(stdout_path.empty() ? "/dev/null" : stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int err = open(stderr_path.empty() ? "/dev/null" : stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(out, 1);
        dup2(err, 2);
        std::vector<char*> argv;
        for (const auto& a : command) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    if (pid < 0) {
        result.error = "could not start " + command[0];
        return result;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    result.started = true;
    result.wall_ms = now_ms() - start;
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    result.cpu_ms = rusage_ms(RUSAGE_CHILDREN) - cpu_before;
    return result;
}

#endif

double idle_power_w(EnergyMeter& meter, int ms) {
    if (!meter.available()) return 0.0;
    double e0 = meter.joules();
    double t0 = now_ms();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    double seconds = (now_ms() - t0) / 1000.0;
    return seconds > 0 ? (meter.joules() - e0) / seconds : 0.0;
}

}  // namespace parc::metrics
