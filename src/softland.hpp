#pragma once
// OOM soft landing: memory.high is a tripwire below memory.max. Above it the kernel throttles
// allocations (instead of killing), which gives the runtime time to freeze the container
// (cgroup.freeze), explain where the memory went, and pick a policy: kill, grant more memory
// from a reserve, or ask the app to free memory (SIGUSR1) and resume.

#include "cgroup.hpp"
#include "cli.hpp"

#include <chrono>
#include <string>

constexpr double kRecoveredRatio = 0.9;                      // notify: below 90% of tripwire = recovered
constexpr double kUnreclaimableRatio = 0.9;                  // land only if >=90% of tripwire cannot be reclaimed
constexpr auto kNotifyGrace = std::chrono::milliseconds(1000);

// memory.max minus a margin: max(max/10, min(4 MiB, max/2)).
long long tripwire(long long max);

// Land when usage reached the tripwire and reclaim cannot help: page cache (file minus shmem)
// is reclaimable, so a container that is only streaming files never triggers a freeze.
bool should_land(long long current, long long file, long long shmem, long long tripwire);

// True if bit `sig` is set in a /proc/<pid>/status "SigCgt" hex mask (the process handles it).
bool catches_signal(const std::string& sigcgt_hex, int sig);

class SoftLanding {
public:
    using Clock = std::chrono::steady_clock;
    enum class Outcome { none, killed, granted, notified, recovered };

    SoftLanding(const Cgroup& cg, SoftOom policy, long long max, long long reserve);

    // Call on every memory.events change and every tick. Reads memory.current and memory.stat.
    Outcome check(Clock::time_point now);

    void thaw();
    bool frozen() const { return frozen_; }
    long long tripwire() const { return tripwire_; }
    long long max() const { return max_; }
    int freezes() const { return freezes_; }
    int grants() const { return grants_; }
    long long granted() const { return granted_; }
    bool killed() const { return killed_; }

private:
    void freeze();
    void report(long long current) const;
    Outcome grant_or_kill();
    int notify();  // returns how many processes were signalled

    const Cgroup& cg_;
    SoftOom policy_;
    long long max_, tripwire_, reserve_left_, grant_step_;
    bool frozen_ = false, killed_ = false, in_grace_ = false;
    Clock::time_point grace_end_{};
    int freezes_ = 0, grants_ = 0;
    long long granted_ = 0;
};
