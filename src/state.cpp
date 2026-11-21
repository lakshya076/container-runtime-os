#include "state.hpp"
#include "cgroup.hpp"
#include "pressure.hpp"
#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <sys/stat.h>

namespace fs = std::filesystem;

std::string format_state(const StateMap& kv) {
    std::string out;
    for (const auto& [k, v] : kv) {
        std::string clean = v;
        std::replace(clean.begin(), clean.end(), '\n', ' ');
        out += k + "=" + clean + "\n";
    }
    return out;
}

StateMap parse_state(const std::string& text) {
    StateMap kv;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

void write_state(pid_t pid, const StateMap& kv) {
    mkdir(kStateDir, 0755);
    const std::string path = std::string(kStateDir) + "/" + std::to_string(pid);
    const std::string tmp = std::string(kStateDir) + "/." + std::to_string(pid) + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    const std::string body = format_state(kv);
    bool ok = std::fwrite(body.data(), 1, body.size(), f) == body.size();
    ok = std::fclose(f) == 0 && ok;
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) std::remove(tmp.c_str());
}

void remove_state(pid_t pid) { std::remove((std::string(kStateDir) + "/" + std::to_string(pid)).c_str()); }

namespace {

std::string mib_or_max(long long b) {
    if (b < 0) return "max";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1fM", static_cast<double>(b) / (1 << 20));
    return buf;
}

std::string uptime(long long s) {
    char buf[32];
    if (s < 60) std::snprintf(buf, sizeof buf, "%llds", s);
    else if (s < 3600) std::snprintf(buf, sizeof buf, "%lldm%02llds", s / 60, s % 60);
    else std::snprintf(buf, sizeof buf, "%lldh%02lldm", s / 3600, (s / 60) % 60);
    return buf;
}

std::string clip(const std::string& s, size_t n) { return s.size() <= n ? s : s.substr(0, n - 1) + "~"; }

long long num(const StateMap& kv, const std::string& k) {
    auto it = kv.find(k);
    if (it == kv.end()) return 0;
    try { return std::stoll(it->second); } catch (...) { return 0; }
}

std::string get(const StateMap& kv, const std::string& k) {
    auto it = kv.find(k);
    return it == kv.end() ? "" : it->second;
}

}  // namespace

std::string format_ps(const std::vector<PsRow>& rows) {
    char line[512];
    std::string out;
    std::snprintf(line, sizeof line, "%-7s %-12s %-18s %-9s %-14s %8s %8s %8s %-11s %-7s %s\n", "ID", "NAME", "IMAGE",
                  "STATUS", "MODE", "MEM", "HIGH", "MAX", "ACTIONS", "UP", "COMMAND");
    out += line;
    for (const auto& r : rows) {
        std::snprintf(line, sizeof line, "%-7s %-12s %-18s %-9s %-14s %8s %8s %8s %-11s %-7s %s\n", r.id.c_str(),
                      clip(r.name, 12).c_str(), clip(r.image, 18).c_str(), r.status.c_str(), clip(r.mode, 14).c_str(),
                      mib_or_max(r.mem).c_str(), mib_or_max(r.high).c_str(), mib_or_max(r.max).c_str(),
                      clip(r.actions, 11).c_str(), uptime(r.uptime_s).c_str(), clip(r.command, 40).c_str());
        out += line;
    }
    return out;
}

int cmd_ps() {
    std::vector<std::pair<long long, PsRow>> found;  // (start time, row)
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(kStateDir, ec)) {
        const std::string pid = entry.path().filename().string();
        if (pid.empty() || !std::all_of(pid.begin(), pid.end(), [](unsigned char c) { return std::isdigit(c); }))
            continue;
        const std::string cg = std::string(kCgroupRoot) + "/container_" + pid;
        if (!fs::exists(cg, ec)) continue;  // stale file from a crashed supervisor
        StateMap kv;
        try { kv = parse_state(read_file(entry.path().string())); } catch (...) { continue; }

        PsRow r;
        r.id = pid;
        r.name = get(kv, "name");
        r.image = fs::path(get(kv, "rootfs")).filename().string();
        r.mode = get(kv, "mode");
        r.command = get(kv, "command");
        r.uptime_s = std::max(0LL, static_cast<long long>(std::time(nullptr)) - num(kv, "started"));
        try {
            r.mem = std::stoll(read_file(cg + "/memory.current"));
            r.high = parse_limit(read_file(cg + "/memory.high"));
            r.max = parse_limit(read_file(cg + "/memory.max"));
            r.status = parse_keyed(read_file(cg + "/cgroup.events"), "frozen") == 1 ? "frozen"
                       : num(kv, "pressured")                                          ? "pressure"
                                                                                       : "running";
        } catch (...) {
            continue;  // exited between the checks
        }
        if (r.mode.find("elastic") != std::string::npos)
            r.actions = "+" + std::to_string(num(kv, "grows")) + " -" + std::to_string(num(kv, "shrinks"));
        if (r.mode.find("soft") != std::string::npos)
            r.actions += (r.actions.empty() ? "" : " ") + std::string("frz") + std::to_string(num(kv, "freezes"));
        if (num(kv, "kernel_ooms") > 0) r.actions += " oom" + std::to_string(num(kv, "kernel_ooms"));
        if (r.actions.empty()) r.actions = "-";
        found.push_back({num(kv, "started"), r});
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second.id < b.second.id;
    });
    std::vector<PsRow> rows;
    for (auto& f : found) rows.push_back(std::move(f.second));
    std::fputs(format_ps(rows).c_str(), stdout);
    return 0;
}
