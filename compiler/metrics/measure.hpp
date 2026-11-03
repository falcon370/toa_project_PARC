#pragma once

#include <string>
#include <vector>

// Measuring the machine: wall-clock time, processor time, memory and energy.
//
// Energy comes from the processor's own meter (Intel/AMD RAPL), which Windows
// publishes as the "Energy Meter" performance counters. It is the energy of
// the whole processor package, so it includes everything else running at the
// same time; that is why a measurement also reports the idle power and the
// energy above idle. On a system without the counters (or not Windows),
// energy is reported as unavailable instead of being guessed.
namespace parc::metrics {

double now_ms();      // a steady clock, in milliseconds
double cpu_ms();      // processor time this process has used (user + kernel), in milliseconds
long long peak_memory_bytes();  // the largest working set this process has had

class EnergyMeter {
public:
    EnergyMeter();
    ~EnergyMeter();
    EnergyMeter(const EnergyMeter&) = delete;
    EnergyMeter& operator=(const EnergyMeter&) = delete;

    bool available() const { return available_; }
    const std::string& source() const { return source_; }  // which counter, or why there is none
    double joules();  // energy the package has used since the machine started

private:
    bool available_ = false;
    std::string source_;
    void* query_ = nullptr;
    void* counter_ = nullptr;
};

// What a measured piece of work cost, per run.
struct Measurement {
    int runs = 0;            // how many times the work was repeated to get a long enough measurement
    double wall_ms = 0.0;    // per run
    double cpu_ms = -1.0;    // per run; -1 when the measurement was shorter than the processor-time clock's tick
    bool has_energy = false;
    double energy_j = 0.0;   // package energy per run
    double power_w = 0.0;    // average package power while the work ran
    double idle_w = 0.0;     // package power while this process was asleep, just before
    double above_idle_j = 0.0;  // energy per run beyond what the idle machine would have used
};

// A child process that ran to the end.
struct ProcessRun {
    bool started = false;
    int exit_code = -1;
    double wall_ms = 0.0;
    double cpu_ms = 0.0;
    long long peak_memory_bytes = 0;
    std::string error;  // why it did not start
};

// Runs a program with arguments, its output written to stdout_path (or thrown
// away when empty) and its errors to stderr_path (or thrown away).
ProcessRun run_process(const std::vector<std::string>& command, const std::string& stdout_path = "",
                       const std::string& stderr_path = "");

// Package power while this process sleeps for `ms` milliseconds (0 when there is no meter).
double idle_power_w(EnergyMeter& meter, int ms = 250);

}  // namespace parc::metrics
