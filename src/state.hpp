#pragma once
// Runtime state for `crun ps`. Each running supervisor keeps /run/crun/<pid> up to date
// ("key=value" lines); `ps` combines it with the live cgroup files.

#include <map>
#include <string>
#include <sys/types.h>
#include <vector>

constexpr const char* kStateDir = "/run/crun";

using StateMap = std::map<std::string, std::string>;

std::string format_state(const StateMap& kv);       // newlines inside values become spaces
StateMap parse_state(const std::string& text);

// Atomic (temp file + rename) so `ps` never reads half a file. Best effort: never throws.
void write_state(pid_t pid, const StateMap& kv);
void remove_state(pid_t pid);

struct PsRow {
    std::string id, name, image, status, mode, actions, command;
    long long mem = 0, high = -1, max = -1;  // bytes, -1 = "max"
    long long uptime_s = 0;
};

std::string format_ps(const std::vector<PsRow>& rows);

// Prints the table of running containers. Needs no root.
int cmd_ps();
