#include "recorder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <sstream>

namespace {

double mib(double b) { return b / (1 << 20); }

long long effective_limit(const Counters& c) { return c.high > 0 ? c.high : c.max; }

std::string esc(const std::string& s) {
    std::string out;
    for (char ch : s) {
        switch (ch) {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            default: out += ch;
        }
    }
    return out;
}

std::string fmt(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

// Picks a "nice" axis step (1, 2, 5 x 10^n) for about `ticks` ticks over `range`.
double nice_step(double range, int ticks) {
    if (range <= 0) return 1;
    double raw = range / ticks, mag = std::pow(10, std::floor(std::log10(raw)));
    double n = raw / mag;
    return (n < 1.5 ? 1 : n < 3.5 ? 2 : n < 7.5 ? 5 : 10) * mag;
}

struct Plot {
    double x0, y0, w, h, tmax, vmax;
    double x(double t_ms) const { return x0 + (tmax > 0 ? t_ms / tmax : 0) * w; }
    double y(double v) const { return y0 + h - (vmax > 0 ? std::min(v, vmax) / vmax : 0) * h; }
};

// Polyline(s) for one series; values < 0 (unlimited / missing) break the line.
std::string series(const std::vector<Row>& rows, size_t stride, const Plot& p,
                   const std::function<double(const Row&)>& get, const std::string& cls) {
    std::string out, pts;
    auto flush = [&] {
        if (!pts.empty()) out += "<polyline class=\"" + cls + "\" points=\"" + pts + "\"/>\n";
        pts.clear();
    };
    for (size_t i = 0; i < rows.size(); i += stride) {
        double v = get(rows[i]);
        if (v < 0) { flush(); continue; }
        pts += fmt("%.1f", p.x(static_cast<double>(rows[i].s.t_ms))) + "," + fmt("%.1f", p.y(v)) + " ";
    }
    flush();
    return out;
}

std::string axes(const Plot& p, const std::string& unit, double vstep) {
    std::string out;
    for (double v = 0; v <= p.vmax + 1e-9; v += vstep) {
        out += "<line class=\"grid\" x1=\"" + fmt("%.1f", p.x0) + "\" x2=\"" + fmt("%.1f", p.x0 + p.w) + "\" y1=\"" +
               fmt("%.1f", p.y(v)) + "\" y2=\"" + fmt("%.1f", p.y(v)) + "\"/>\n";
        out += "<text class=\"tick\" text-anchor=\"end\" x=\"" + fmt("%.1f", p.x0 - 6) + "\" y=\"" +
               fmt("%.1f", p.y(v) + 4) + "\">" + fmt(vstep < 1 ? "%.1f" : "%.0f", v) + unit + "</text>\n";
    }
    double tstep = nice_step(p.tmax / 1000, 8);
    for (double t = 0; t <= p.tmax / 1000 + 1e-9; t += tstep)
        out += "<text class=\"tick\" text-anchor=\"middle\" x=\"" + fmt("%.1f", p.x(t * 1000)) + "\" y=\"" +
               fmt("%.1f", p.y0 + p.h + 16) + "\">" + fmt(tstep < 1 ? "%.1fs" : "%.0fs", t) + "</text>\n";
    return out;
}

}  // namespace

Summary summarize(const std::vector<Row>& rows) {
    Summary s;
    s.samples = rows.size();
    if (rows.empty()) return s;
    s.runtime_s = static_cast<double>(rows.back().s.t_ms) / 1000.0;
    double sum = 0, lim_sum = 0;
    size_t lim_n = 0;
    std::vector<long long> ws;
    for (const auto& r : rows) {
        sum += static_cast<double>(r.s.c.current);
        s.peak = std::max(s.peak, r.s.c.current);
        ws.push_back(std::max(0LL, r.s.c.current - r.s.c.inactive_file));
        s.ws_peak = std::max(s.ws_peak, ws.back());
        long long lim = effective_limit(r.s.c);
        if (lim > 0) { lim_sum += static_cast<double>(lim); ++lim_n; }
    }
    s.avg_current = sum / static_cast<double>(rows.size());
    if (lim_n == rows.size()) s.avg_limit = lim_sum / static_cast<double>(lim_n);
    std::sort(ws.begin(), ws.end());
    s.ws_p95 = ws[std::min(ws.size() - 1, ws.size() * 95 / 100)];
    return s;
}

std::string csv_header() {
    return "t_ms,current,high,max,anon,file,slab,refault_rate,majfault_rate,high_event_rate,pressured,psi_pct,event\n";
}

std::string csv_row(const Row& r) {
    const auto& c = r.s.c;
    std::ostringstream o;
    o << r.s.t_ms << ',' << c.current << ',' << c.high << ',' << c.max << ',' << c.anon << ',' << c.file << ','
      << c.slab << ',' << fmt("%.1f", r.s.refault_rate) << ',' << fmt("%.1f", r.s.majfault_rate) << ','
      << fmt("%.1f", r.s.high_event_rate) << ','
      << (r.s.pressured ? 1 : 0) << ',' << fmt("%.2f", r.s.psi_pct) << ',' << r.event << '\n';
    return o.str();
}

Recorder::Recorder(std::string prefix) : prefix_(std::move(prefix)) {
    if (prefix_.empty()) return;
    csv_.open(prefix_ + ".csv");
    if (!csv_) std::fprintf(stderr, "[crun] warning: cannot write %s.csv\n", prefix_.c_str());
    else csv_ << csv_header();
}

void Recorder::add(const Sample& s, const std::string& event) {
    rows_.push_back({s, event});
    if (csv_) csv_ << csv_row(rows_.back());
}

bool Recorder::write_html(const std::string& title, const std::vector<std::pair<std::string, std::string>>& info) const {
    if (prefix_.empty()) return true;
    std::ofstream f(prefix_ + ".html");
    f << render_html(rows_, title, info);
    return static_cast<bool>(f);
}

std::string render_html(const std::vector<Row>& rows, const std::string& title,
                        const std::vector<std::pair<std::string, std::string>>& info) {
    const double W = 960, H1 = 340, H2 = 170, L = 70, R = 20, T = 16, B = 30;
    double tmax = rows.empty() ? 1 : std::max<double>(1, static_cast<double>(rows.back().s.t_ms));
    double vmax = 1, rmax = 1;
    bool psi = false;
    for (const auto& r : rows) {
        vmax = std::max({vmax, mib(r.s.c.current), mib(r.s.c.high), mib(r.s.c.max)});
        rmax = std::max(rmax, r.s.psi_pct >= 0 ? r.s.psi_pct : std::max(r.s.refault_rate, r.s.high_event_rate));
        psi = psi || r.s.psi_pct >= 0;
    }
    double vstep = nice_step(vmax * 1.05, 6);
    vmax = std::ceil(vmax * 1.05 / vstep) * vstep;
    double rstep = nice_step(rmax * 1.1, 4);
    rmax = std::ceil(rmax * 1.1 / rstep) * rstep;
    size_t stride = std::max<size_t>(1, rows.size() / 1500);

    Plot p1{L, T, W - L - R, H1 - T - B, tmax, vmax};
    Plot p2{L, T, W - L - R, H2 - T - B, tmax, rmax};

    std::ostringstream h;
    h << "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
         "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      << "<title>" << esc(title) << "</title>\n<style>\n"
      << ":root{--bg:#fbfbfa;--fg:#1d1d1b;--muted:#6b6b66;--grid:#e4e3df;--cur:#2f6fd6;--high:#d9822b;"
         "--max:#c93b3b;--anon:#2e9d6a;--file:#8a63c9;--press:#c93b3b;--event:#555}\n"
         "@media (prefers-color-scheme:dark){:root{--bg:#161615;--fg:#ecebe7;--muted:#9c9b95;--grid:#2c2c2a;"
         "--cur:#6ea2ff;--high:#f0a050;--max:#ff6b6b;--anon:#5fd39b;--file:#b493f0;--press:#ff6b6b;--event:#bbb}}\n"
         "body{margin:0;padding:24px 16px;background:var(--bg);color:var(--fg);"
         "font:14px/1.5 system-ui,-apple-system,Segoe UI,sans-serif}\n"
         "main{max-width:980px;margin:0 auto}h1{font-size:20px;margin:0 0 4px}h2{font-size:15px;margin:24px 0 6px}\n"
         ".sub{color:var(--muted);margin:0 0 16px;word-break:break-all}svg{width:100%;height:auto;display:block}\n"
         "polyline{fill:none;stroke-width:1.5}.cur{stroke:var(--cur);stroke-width:2}.high{stroke:var(--high)}"
         ".max{stroke:var(--max);stroke-dasharray:6 4}.anon{stroke:var(--anon);stroke-dasharray:2 3}"
         ".file{stroke:var(--file);stroke-dasharray:2 3}.rate{stroke:var(--press)}\n"
         ".grid{stroke:var(--grid)}.tick{fill:var(--muted);font-size:11px}.ev{stroke:var(--event);stroke-dasharray:3 3}"
         ".evt{fill:var(--fg);font-size:11px}.pr{fill:var(--press);opacity:.18}"
         ".gr{fill:var(--anon)}.sh{fill:var(--high)}\n"
         ".legend{display:flex;flex-wrap:wrap;gap:14px;color:var(--muted);font-size:12px;margin:6px 0}"
         ".sw{display:inline-block;width:18px;height:3px;vertical-align:middle;margin-right:6px}\n"
         "table{border-collapse:collapse;margin-top:8px}td{padding:3px 16px 3px 0;vertical-align:top}"
         "td:first-child{color:var(--muted)}\n</style></head><body><main>\n"
      << "<h1>" << esc(title) << "</h1>\n";

    for (const auto& kv : info)
        if (kv.first == "command") h << "<p class=\"sub\">" << esc(kv.second) << "</p>\n";

    h << "<h2>Memory (MiB)</h2>\n<div class=\"legend\">"
         "<span><i class=\"sw\" style=\"background:var(--cur)\"></i>memory.current</span>"
         "<span><i class=\"sw\" style=\"background:var(--high)\"></i>memory.high</span>"
         "<span><i class=\"sw\" style=\"background:var(--max)\"></i>memory.max</span>"
         "<span><i class=\"sw\" style=\"background:var(--anon)\"></i>anon</span>"
         "<span><i class=\"sw\" style=\"background:var(--file)\"></i>file cache</span></div>\n"
      << "<svg viewBox=\"0 0 " << W << ' ' << H1 << "\" role=\"img\" aria-label=\"memory usage and limits over time\">\n"
      << axes(p1, "", vstep)
      << series(rows, stride, p1, [](const Row& r) { return r.s.c.max > 0 ? mib(r.s.c.max) : -1; }, "max")
      << series(rows, stride, p1, [](const Row& r) { return r.s.c.high > 0 ? mib(r.s.c.high) : -1; }, "high")
      << series(rows, stride, p1, [](const Row& r) { return mib(r.s.c.anon); }, "anon")
      << series(rows, stride, p1, [](const Row& r) { return mib(r.s.c.file); }, "file")
      << series(rows, stride, p1, [](const Row& r) { return mib(r.s.c.current); }, "cur");

    // Events: grow/shrink as small marks on the memory.high line, everything else as labelled lines.
    int label_row = 0;
    for (const auto& r : rows) {
        if (r.event.empty()) continue;
        double x = p1.x(static_cast<double>(r.s.t_ms));
        std::string major;
        std::istringstream ev(r.event);
        std::string tok;
        while (std::getline(ev, tok, ';')) {
            if (tok == "grow" || tok == "shrink") {
                double y = p1.y(r.s.c.high > 0 ? mib(r.s.c.high) : 0);
                h << "<circle class=\"" << (tok == "grow" ? "gr" : "sh") << "\" cx=\"" << fmt("%.1f", x) << "\" cy=\""
                  << fmt("%.1f", y) << "\" r=\"2.5\"/>\n";
            } else {
                major += (major.empty() ? "" : "+") + tok;
            }
        }
        if (major.empty()) continue;
        double ty = T + 12 + 13 * (label_row++ % 4);
        h << "<line class=\"ev\" x1=\"" << fmt("%.1f", x) << "\" x2=\"" << fmt("%.1f", x) << "\" y1=\"" << T
          << "\" y2=\"" << fmt("%.1f", T + p1.h) << "\"/>\n<text class=\"evt\" x=\"" << fmt("%.1f", x + 4)
          << "\" y=\"" << fmt("%.1f", ty) << "\">" << esc(major) << "</text>\n";
    }
    h << "</svg>\n";

    h << "<h2>Memory pressure</h2>\n<div class=\"legend\">";
    if (psi) h << "<span><i class=\"sw\" style=\"background:var(--press)\"></i>PSI stall %</span>";
    else
        h << "<span><i class=\"sw\" style=\"background:var(--press)\"></i>refaults/s</span>"
             "<span><i class=\"sw\" style=\"background:var(--high)\"></i>memory.high reclaim events/s</span>";
    h << "<span><i class=\"sw\" style=\"background:var(--press);opacity:.3;height:10px\"></i>pressured</span></div>\n"
      << "<svg viewBox=\"0 0 " << W << ' ' << H2 << "\" role=\"img\" aria-label=\"memory pressure over time\">\n";
    if (rows.size() > 1) {
        double bw = std::max(1.0, p2.w / static_cast<double>(rows.size()));
        for (const auto& r : rows)
            if (r.s.pressured)
                h << "<rect class=\"pr\" x=\"" << fmt("%.1f", p2.x(static_cast<double>(r.s.t_ms)) - bw / 2) << "\" y=\""
                  << T << "\" width=\"" << fmt("%.1f", bw) << "\" height=\"" << fmt("%.1f", p2.h) << "\"/>\n";
    }
    h << axes(p2, psi ? "%" : "", rstep)
      << series(rows, stride, p2, [psi](const Row& r) { return psi ? r.s.psi_pct : r.s.refault_rate; }, "rate");
    if (!psi) h << series(rows, stride, p2, [](const Row& r) { return r.s.high_event_rate; }, "high");
    h << "</svg>\n";

    h << "<h2>Summary</h2>\n<table>\n";
    for (const auto& kv : info)
        if (kv.first != "command") h << "<tr><td>" << esc(kv.first) << "</td><td>" << esc(kv.second) << "</td></tr>\n";
    h << "</table>\n</main></body></html>\n";
    return h.str();
}
