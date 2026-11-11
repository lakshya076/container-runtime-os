#include "softland.hpp"
#include "util.hpp"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <sstream>
#include <thread>
#include <vector>

namespace {

double mib(long long b) { return static_cast<double>(b) / (1 << 20); }

std::vector<pid_t> procs(const Cgroup& cg) {
    std::vector<pid_t> out;
    try {
        std::istringstream in(cg.get("cgroup.procs"));
        pid_t p;
        while (in >> p) out.push_back(p);
    } catch (...) {}
    return out;
}

// Value of a "Key:\tvalue" line in /proc/<pid>/status, or "".
std::string status_field(pid_t pid, const std::string& key) {
    try {
        std::istringstream in(read_file("/proc/" + std::to_string(pid) + "/status"));
        std::string line;
        while (std::getline(in, line))
            if (line.rfind(key + ":", 0) == 0) {
                auto v = line.substr(key.size() + 1);
                v.erase(0, v.find_first_not_of(" \t"));
                return v;
            }
    } catch (...) {}
    return "";
}

}  // namespace

long long tripwire(long long max) {
    long long margin = std::max(max / 10, std::min(4LL << 20, max / 2));
    return max - margin;
}

bool should_land(long long current, long long file, long long shmem, long long tripwire) {
    long long unreclaimable = current - std::max(0LL, file - shmem);
    return current >= tripwire &&
           static_cast<double>(unreclaimable) >= static_cast<double>(tripwire) * kUnreclaimableRatio;
}

bool catches_signal(const std::string& sigcgt_hex, int sig) {
    try {
        unsigned long long mask = std::stoull(sigcgt_hex, nullptr, 16);
        return (mask >> (sig - 1)) & 1ULL;
    } catch (...) {
        return false;
    }
}

SoftLanding::SoftLanding(const Cgroup& cg, SoftOom policy, long long max, long long reserve)
    : cg_(cg), policy_(policy), max_(max), tripwire_(::tripwire(max)), reserve_left_(reserve),
      grant_step_(std::max(1LL << 20, max / 4)) {}

void SoftLanding::freeze() {
    cg_.set("cgroup.freeze", "1");
    frozen_ = true;
    ++freezes_;
    // Freezing is asynchronous; wait briefly so the diagnostics see a still container.
    for (int i = 0; i < 20; ++i) {
        try {
            if (parse_keyed(cg_.get("cgroup.events"), "frozen") == 1) return;
        } catch (...) { return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void SoftLanding::thaw() {
    if (!frozen_) return;
    try { cg_.set("cgroup.freeze", "0"); } catch (...) {}
    frozen_ = false;
}

void SoftLanding::report(long long current) const {
    std::fprintf(stderr, "[crun] ---- soft landing: container frozen at %.1f MiB (tripwire %.1f MiB, max %.1f MiB) ----\n",
                 mib(current), mib(tripwire_), mib(max_));
    try {
        const std::string stat = cg_.get("memory.stat");
        std::fprintf(stderr, "[crun]   anon %.1f MiB | file %.1f MiB | slab %.1f MiB | shmem %.1f MiB | kernel_stack %.1f MiB\n",
                     mib(parse_keyed(stat, "anon")), mib(parse_keyed(stat, "file")), mib(parse_keyed(stat, "slab")),
                     mib(parse_keyed(stat, "shmem")), mib(parse_keyed(stat, "kernel_stack")));
    } catch (...) {}

    struct Proc { pid_t pid; long long rss_kb; std::string name; };
    std::vector<Proc> top;
    for (pid_t p : procs(cg_)) {
        long long kb = 0;
        try { kb = std::stoll(status_field(p, "VmRSS")); } catch (...) {}
        top.push_back({p, kb, status_field(p, "Name")});
    }
    std::sort(top.begin(), top.end(), [](const Proc& a, const Proc& b) { return a.rss_kb > b.rss_kb; });
    if (top.size() > 5) top.resize(5);
    for (const auto& t : top)
        std::fprintf(stderr, "[crun]   pid %-6d %-16s RSS %.1f MiB\n", t.pid, t.name.c_str(),
                     static_cast<double>(t.rss_kb) / 1024);
}

SoftLanding::Outcome SoftLanding::grant_or_kill() {
    if (reserve_left_ <= 0) {
        std::fprintf(stderr, "[crun]   action: kill (reserve exhausted)\n");
        try { cg_.set("cgroup.kill", "1"); } catch (...) {}
        thaw();
        killed_ = true;
        return Outcome::killed;
    }
    long long g = std::min(reserve_left_, grant_step_);
    reserve_left_ -= g;
    max_ += g;
    tripwire_ = ::tripwire(max_);
    cg_.set("memory.max", std::to_string(max_));
    ++grants_;
    granted_ += g;
    std::fprintf(stderr, "[crun]   action: granted +%.1f MiB (memory.max now %.1f MiB, reserve left %.1f MiB)\n", mib(g),
                 mib(max_), mib(reserve_left_));
    thaw();
    return Outcome::granted;
}

int SoftLanding::notify() {
    int n = 0;
    for (pid_t p : procs(cg_))
        // Only signal processes that handle SIGUSR1; for the rest its default action is to die.
        if (catches_signal(status_field(p, "SigCgt"), SIGUSR1) && kill(p, SIGUSR1) == 0) ++n;
    return n;
}

SoftLanding::Outcome SoftLanding::check(Clock::time_point now) {
    if (killed_) return Outcome::none;
    long long current = 0, file = 0, shmem = 0;
    try {
        current = std::stoll(cg_.get("memory.current"));
        const std::string stat = cg_.get("memory.stat");
        file = parse_keyed(stat, "file");
        shmem = parse_keyed(stat, "shmem");
    } catch (...) {
        return Outcome::none;  // cgroup going away
    }
    if (in_grace_) {
        if (now < grace_end_) return Outcome::none;
        in_grace_ = false;
        if (static_cast<double>(current) < static_cast<double>(tripwire_) * kRecoveredRatio) {
            std::fprintf(stderr, "[crun] soft landing: app recovered, now %.1f MiB\n", mib(current));
            return Outcome::recovered;
        }
        std::fprintf(stderr, "[crun] soft landing: app did not free enough memory (%.1f MiB)\n", mib(current));
        freeze();
        return grant_or_kill();
    }
    if (!should_land(current, file, shmem, tripwire_)) return Outcome::none;

    freeze();
    report(current);
    switch (policy_) {
        case SoftOom::kill:
            std::fprintf(stderr, "[crun]   action: kill\n");
            try { cg_.set("cgroup.kill", "1"); } catch (...) {}
            thaw();
            killed_ = true;
            return Outcome::killed;
        case SoftOom::grant:
            return grant_or_kill();
        case SoftOom::notify: {
            int n = notify();
            if (n == 0) {
                std::fprintf(stderr, "[crun]   no process handles SIGUSR1\n");
                return grant_or_kill();
            }
            std::fprintf(stderr, "[crun]   action: notified %d process(es) with SIGUSR1, %lld ms grace\n", n,
                         static_cast<long long>(kNotifyGrace.count()));
            thaw();
            in_grace_ = true;
            grace_end_ = now + kNotifyGrace;
            return Outcome::notified;
        }
        default:
            thaw();
            return Outcome::none;
    }
}
