#pragma once
// Flight recorder: one row per tick of memory usage, limits and pressure, plus the controller's
// and soft landing's actions. Streams PATH.csv and renders a self-contained PATH.html (inline SVG).

#include "pressure.hpp"

#include <fstream>
#include <string>
#include <utility>
#include <vector>

struct Row {
    Sample s;
    std::string event;  // e.g. "grow", "shrink", "freeze;grant", "oom_kill"; empty if none
};

struct Summary {
    size_t samples = 0;
    double runtime_s = 0;
    double avg_current = 0;  // bytes
    double avg_limit = -1;   // bytes; effective limit = memory.high if set, else memory.max; -1 = unlimited
    long long peak = 0;      // bytes, max memory.current
    // Working set as the kubelet defines it: memory.current - inactive_file (cold cache excluded).
    long long ws_p95 = 0;    // bytes, 95th percentile
    long long ws_peak = 0;   // bytes, max
};

Summary summarize(const std::vector<Row>& rows);
std::string csv_header();
std::string csv_row(const Row& r);
std::string render_html(const std::vector<Row>& rows, const std::string& title,
                        const std::vector<std::pair<std::string, std::string>>& info);

class Recorder {
public:
    explicit Recorder(std::string prefix);  // empty prefix: keep rows for the summary, write nothing
    void add(const Sample& s, const std::string& event);
    const std::vector<Row>& rows() const { return rows_; }
    bool enabled() const { return !prefix_.empty(); }
    const std::string& prefix() const { return prefix_; }
    // Writes PATH.html; returns false on I/O error.
    bool write_html(const std::string& title, const std::vector<std::pair<std::string, std::string>>& info) const;

private:
    std::string prefix_;
    std::ofstream csv_;
    // shortcut: all rows stay in memory (~100 B per 100 ms tick, ~3.6 MB/hour); stream to disk if runs get longer.
    std::vector<Row> rows_;
};
