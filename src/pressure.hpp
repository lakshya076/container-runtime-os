#pragma once
// Memory pressure probe: decides whether a container is short of memory.
// Uses kernel PSI (memory.pressure) when available, otherwise an estimator built from
// page refaults and memory.high overrun (the only option on kernels without CONFIG_PSI, e.g. WSL2).

#include "cgroup.hpp"

#include <string>

// Tunables, kept together so the report can discuss them.
constexpr double kPsiStallPct = 5.0;        // PSI: >5% of wall time stalled on memory = pressure
constexpr double kRefaultPagesPerSec = 256;  // fallback: >1 MiB/s of evicted pages needed again = thrashing
constexpr double kOverHighRatio = 1.01;      // fallback: usage stuck above memory.high = reclaim failing
constexpr int kOverHighSamples = 2;          // ...for this many consecutive samples
constexpr double kHighEventsPerSec = 20;     // fallback: allocations forced into memory.high reclaim = throttled

// Raw counters read from the cgroup at one instant. Limits are -1 for "max".
struct Counters {
    long long current = 0, high = -1, max = -1;
    long long anon = 0, file = 0, slab = 0;
    long long inactive_file = 0;            // cold page cache the kernel can drop first
    long long refaults = 0, majfaults = 0;  // cumulative, pages / faults
    long long high_events = 0;              // cumulative memory.events "high"
    long long psi_total_us = -1;            // cumulative "some" stall time, -1 without PSI
};

struct Sample {
    long long t_ms = 0;
    Counters c;
    double refault_rate = 0;     // pages/s
    double majfault_rate = 0;    // faults/s
    double high_event_rate = 0;  // memory.high reclaim entries/s
    double psi_pct = -1;       // % of time stalled, -1 without PSI
    bool pressured = false;
};

// "some avg10=0.00 avg60=0.00 avg300=0.00 total=1234\nfull ..." -> 1234, or -1.
long long parse_psi_total(const std::string& text);

// "max" -> -1, otherwise the number.
long long parse_limit(const std::string& text);

// True if current is above memory.high by more than kOverHighRatio.
bool over_high(long long current, long long high);

// Pressure decision for one sample (pure, unit-tested).
bool is_pressured(bool has_psi, double psi_pct, double refault_rate, int over_high_streak, double high_event_rate);

class PressureProbe {
public:
    explicit PressureProbe(const Cgroup& cg);
    Sample sample(long long t_ms);
    bool has_psi() const { return has_psi_; }

private:
    Counters read() const;
    const Cgroup& cg_;
    bool has_psi_;
    Counters prev_;
    long long prev_t_ = -1;
    int over_high_streak_ = 0;
};
