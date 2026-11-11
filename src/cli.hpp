#pragma once
// Module 1: CLI & command parser.

#include <string>
#include <vector>

// What to do when the container crosses its soft-landing tripwire.
enum class SoftOom { off, kill, grant, notify };

struct Options {
    long long memory = -1;          // memory.max in bytes, -1 = unlimited
    long long swap = 0;             // memory.swap.max in bytes, -1 = unlimited
    long long pids = -1;            // pids.max, -1 = unlimited
    int stats_ms = 0;               // live telemetry interval, 0 = off
    std::string hostname = "container";
    std::string rootfs;
    std::vector<std::string> argv;  // command to run inside the container

    bool elastic = false;           // adaptive memory.high controller
    long long memory_min = -1;      // elastic floor in bytes (filled in by the parser)
    SoftOom soft_oom = SoftOom::off;
    long long reserve = -1;         // bytes soft-oom may grant (filled in by the parser)
    std::string record;             // flight recorder path prefix, empty = off
};

// "64M", "1g", "512k", "4096", "max" -> bytes (-1 for "max"). Throws std::invalid_argument.
long long parse_size(const std::string& s);

// Parses the arguments after "run". Throws std::invalid_argument on bad input.
Options parse_run_args(const std::vector<std::string>& args);

const char* soft_oom_name(SoftOom p);

extern const char* const kUsage;
