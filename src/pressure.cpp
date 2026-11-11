#include "pressure.hpp"

#include <sstream>

long long parse_psi_total(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("some ", 0) != 0) continue;
        auto pos = line.find("total=");
        if (pos == std::string::npos) return -1;
        try { return std::stoll(line.substr(pos + 6)); } catch (...) { return -1; }
    }
    return -1;
}

long long parse_limit(const std::string& text) {
    if (text.rfind("max", 0) == 0) return -1;
    try { return std::stoll(text); } catch (...) { return -1; }
}

bool over_high(long long current, long long high) {
    return high > 0 && static_cast<double>(current) > static_cast<double>(high) * kOverHighRatio;
}

bool is_pressured(bool has_psi, double psi_pct, double refault_rate, int over_high_streak, double high_event_rate) {
    if (has_psi) return psi_pct > kPsiStallPct;
    return refault_rate > kRefaultPagesPerSec || over_high_streak >= kOverHighSamples ||
           high_event_rate > kHighEventsPerSec;
}

PressureProbe::PressureProbe(const Cgroup& cg) : cg_(cg), has_psi_(cg.has("memory.pressure")) {
    prev_ = read();
}

Counters PressureProbe::read() const {
    Counters c;
    try {
        c.current = std::stoll(cg_.get("memory.current"));
        c.high = parse_limit(cg_.get("memory.high"));
        c.max = parse_limit(cg_.get("memory.max"));
        const std::string stat = cg_.get("memory.stat");
        c.anon = parse_keyed(stat, "anon");
        c.file = parse_keyed(stat, "file");
        c.slab = parse_keyed(stat, "slab");
        c.inactive_file = parse_keyed(stat, "inactive_file");
        c.refaults = parse_keyed(stat, "workingset_refault_anon") + parse_keyed(stat, "workingset_refault_file");
        c.majfaults = parse_keyed(stat, "pgmajfault");
        c.high_events = parse_keyed(cg_.get("memory.events"), "high");
        if (has_psi_) c.psi_total_us = parse_psi_total(cg_.get("memory.pressure"));
    } catch (...) {
        // The cgroup can vanish while the container is exiting; report what we have.
    }
    return c;
}

Sample PressureProbe::sample(long long t_ms) {
    Sample s;
    s.t_ms = t_ms;
    s.c = read();
    double dt = prev_t_ < 0 ? 0 : static_cast<double>(t_ms - prev_t_) / 1000.0;
    if (dt > 0) {
        s.refault_rate = static_cast<double>(s.c.refaults - prev_.refaults) / dt;
        s.majfault_rate = static_cast<double>(s.c.majfaults - prev_.majfaults) / dt;
        s.high_event_rate = static_cast<double>(s.c.high_events - prev_.high_events) / dt;
        if (has_psi_ && s.c.psi_total_us >= 0 && prev_.psi_total_us >= 0)
            s.psi_pct = static_cast<double>(s.c.psi_total_us - prev_.psi_total_us) / (dt * 1e6) * 100.0;
    }
    over_high_streak_ = over_high(s.c.current, s.c.high) ? over_high_streak_ + 1 : 0;
    s.pressured = dt > 0 && is_pressured(has_psi_, s.psi_pct, s.refault_rate, over_high_streak_, s.high_event_rate);
    prev_ = s.c;
    prev_t_ = t_ms;
    return s;
}
