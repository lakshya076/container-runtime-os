// Evaluation harness: runs the same phased workload under static limits and under the elastic
// memory engine, prints a comparison table and writes per-scenario flight recordings + index.html.
// usage: eval CRUN ROOTFS OUTDIR   (run via `sudo make eval`)

#include "proc.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

struct Scenario {
    std::string id, what;
    std::vector<std::string> flags;
};

struct Result {
    int code;
    double runtime, avg, peak, avg_limit, freezes, grants;
    long long kernel_ooms;
};

std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) o += c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '&' ? "&amp;" : std::string(1, c);
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: eval CRUN ROOTFS OUTDIR\n");
        return 2;
    }
    g_crun = argv[1];
    g_rootfs = argv[2];
    const std::string out = argv[3];
    struct stat st;
    if (mkdir(out.c_str(), 0755) < 0 && (stat(out.c_str(), &st) < 0 || !S_ISDIR(st.st_mode))) {
        std::perror(("eval: cannot create " + out).c_str());
        return 1;
    }

    const std::vector<Scenario> scenarios = {
        {"S1-static-128", "static 128M limit", {"--memory", "128M"}},
        {"S2-static-64", "static 64M limit", {"--memory", "64M"}},
        {"S3-elastic-128", "elastic, ceiling 128M, floor 16M", {"--elastic", "--memory", "128M", "--memory-min", "16M"}},
        {"S4-soft-grant-64", "64M + soft-oom grant (32M reserve)", {"--memory", "64M", "--soft-oom", "grant", "--reserve", "32M"}},
        {"S5-soft-notify-64", "64M + soft-oom notify", {"--memory", "64M", "--soft-oom", "notify"}},
    };

    std::printf("workload: phasehog (30 MiB working set, 100 MiB cold file, 40 MiB spike, shrink to 10 MiB)\n\n");
    std::printf("%-18s %-36s %5s %8s %10s %10s %10s %8s %5s\n", "scenario", "setup", "exit", "time s", "avg MiB",
                "peak MiB", "limit MiB", "frz/grt", "OOMs");

    std::vector<Result> results;
    for (const auto& sc : scenarios) {
        std::vector<std::string> args = sc.flags;
        args.insert(args.end(), {"--record", out + "/" + sc.id, "--", "/usr/local/bin/phasehog"});
        std::string log;
        Result r{};
        r.code = run(args, log);
        r.runtime = number_after(log, "runtime ");
        r.avg = number_after(log, "memory: avg ");
        r.peak = number_after(log, "peak ");
        r.avg_limit = number_after(log, "avg limit ");
        r.freezes = number_after(log, "): ");
        r.grants = number_after(log, "freeze(s), ");
        r.kernel_ooms = contains(log, "[crun] OOM: kernel") ? 1 : 0;
        if (r.freezes < 0) r.freezes = 0;
        if (r.grants < 0) r.grants = 0;
        results.push_back(r);
        std::printf("%-18s %-36s %5d %8.2f %10.1f %10.1f %10.1f %4.0f/%-3.0f %5lld\n", sc.id.c_str(), sc.what.c_str(), r.code,
                    r.runtime, r.avg, r.peak, r.avg_limit, r.freezes, r.grants, r.kernel_ooms);
        std::ofstream(out + "/" + sc.id + ".log") << log;
    }

    std::ofstream html(out + "/index.html");
    html << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>crun evaluation</title>"
            "<style>:root{--bg:#fbfbfa;--fg:#1d1d1b;--muted:#6b6b66;--line:#e4e3df;--link:#2f6fd6}"
            "@media (prefers-color-scheme:dark){:root{--bg:#161615;--fg:#ecebe7;--muted:#9c9b95;--line:#2c2c2a;--link:#6ea2ff}}"
            "body{margin:0;padding:24px 16px;background:var(--bg);color:var(--fg);font:14px/1.5 system-ui,sans-serif}"
            "main{max-width:980px;margin:0 auto}table{border-collapse:collapse;width:100%}"
            "th,td{text-align:left;padding:6px 10px;border-bottom:1px solid var(--line)}"
            "th{color:var(--muted);font-weight:500}td.n{text-align:right;font-variant-numeric:tabular-nums}"
            "a{color:var(--link)}.wrap{overflow-x:auto}</style></head><body><main>"
            "<h1>crun evaluation: static limits vs elastic memory engine</h1>"
            "<p>Workload: phasehog (30 MiB working set, 100 MiB cold file cache, 40 MiB spike, shrink to 10 MiB).</p>"
            "<div class=\"wrap\"><table><tr><th>scenario</th><th>setup</th><th>exit</th><th>time s</th><th>avg MiB</th>"
            "<th>peak MiB</th><th>avg limit MiB</th><th>freezes / grants</th><th>kernel OOM</th></tr>";
    for (size_t i = 0; i < scenarios.size(); ++i) {
        const auto& r = results[i];
        char row[512];
        std::snprintf(row, sizeof row,
                      "<td class=\"n\">%d</td><td class=\"n\">%.2f</td><td class=\"n\">%.1f</td><td class=\"n\">%.1f</td>"
                      "<td class=\"n\">%.1f</td><td class=\"n\">%.0f / %.0f</td><td class=\"n\">%lld</td>",
                      r.code, r.runtime, r.avg, r.peak, r.avg_limit, r.freezes, r.grants, r.kernel_ooms);
        html << "<tr><td><a href=\"" << scenarios[i].id << ".html\">" << scenarios[i].id << "</a></td><td>"
             << esc(scenarios[i].what) << "</td>" << row << "</tr>";
    }
    html << "</table></div></main></body></html>\n";
    std::printf("\nwrote %s/index.html and one .csv/.html/.log per scenario\n", out.c_str());
    return 0;
}
