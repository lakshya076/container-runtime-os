// Integration tests. T1-T8 from the synopsis, T9-T13 for the elastic memory engine.
// Needs root and cgroup v2. usage: integration CRUN_BINARY ROOTFS   (run via `sudo make itest`)

#include "proc.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

int g_failures = 0;

using Clock = std::chrono::steady_clock;

std::string slurp(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && s.back() == '\n') s.pop_back();
    return s;
}

bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

void expect(bool ok, const std::string& what, const std::string& log) {
    if (ok) return;
    ++g_failures;
    std::printf("    FAIL: %s\n    --- output ---\n%s    --------------\n", what.c_str(), log.c_str());
}

void test(const char* name, const std::function<void()>& body) {
    int before = g_failures;
    auto t0 = Clock::now();
    body();
    double s = std::chrono::duration<double>(Clock::now() - t0).count();
    std::printf("%s %s (%.2fs)\n", g_failures == before ? "PASS" : "FAIL", name, s);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: integration CRUN ROOTFS\n");
        return 2;
    }
    g_crun = argv[1];
    g_rootfs = argv[2];
    if (geteuid() != 0) {
        std::fprintf(stderr, "integration tests need root\n");
        return 2;
    }

    test("T1 cgroup node created with memory.max=67108864", [] {
        std::string log;
        Proc p = start({"--memory", "64M", "--", "/bin/sleep", "1"});
        std::string cg = wait_cgroup(p, log);
        expect(contains(cg, "/sys/fs/cgroup/container_"), "cgroup path reported", log);
        std::string max = slurp(cg + "/memory.max");
        std::string swap = slurp(cg + "/memory.swap.max");
        int code = finish(p, log);
        expect(max == "67108864", "memory.max is 67108864 (got '" + max + "')", log);
        expect(swap == "0", "memory.swap.max is 0 (got '" + swap + "')", log);
        expect(code == 0, "exit code 0", log);
        expect(!cg.empty() && !exists(cg), "cgroup removed after exit", log);
    });

    test("T2 shell is PID 1, host processes invisible", [] {
        std::string log;
        int code = run({"--", "/bin/sh", "-c", "ps -o pid,args; true"}, log);
        std::istringstream in(log);
        std::string line;
        int procs = 0;
        bool sh_is_1 = false;
        while (std::getline(in, line)) {
            if (contains(line, "[crun]") || contains(line, "PID")) continue;
            ++procs;
            std::istringstream l(line);
            int pid = 0;
            l >> pid;
            if (pid == 1 && contains(line, "/bin/sh")) sh_is_1 = true;
        }
        expect(code == 0, "exit code 0", log);
        expect(sh_is_1, "/bin/sh is PID 1", log);
        expect(procs == 2, "only sh and ps visible (saw " + std::to_string(procs) + ")", log);
    });

    test("T3 30MB allocation inside 64MB limit succeeds", [] {
        std::string log;
        int code = run({"--memory", "64M", "--stats", "200", "--", "/usr/local/bin/memhog", "30", "1"}, log);
        expect(code == 0, "exit code 0", log);
        expect(contains(log, "memhog: allocated 30 MiB"), "allocation completed", log);
        expect(contains(log, "[crun] mem 3"), "telemetry reported ~30 MiB", log);
        expect(!contains(log, "OOM"), "no OOM", log);
    });

    test("T4 100MB allocation inside 64MB limit is OOM killed", [] {
        std::string log;
        int code = run({"--memory", "64M", "--", "/usr/local/bin/memhog", "100"}, log);
        expect(code == 137, "exit code 137 (SIGKILL)", log);
        expect(contains(log, "[crun] OOM"), "runtime logged OOM breach", log);
        expect(!contains(log, "memhog: allocated"), "allocation did not complete", log);
    });

    test("T5 swap 0 kills immediately; with swap allowed the same job survives", [] {
        std::string log;
        auto t0 = Clock::now();
        int code = run({"--memory", "64M", "--swap", "0", "--", "/usr/local/bin/memhog", "100"}, log);
        double s = std::chrono::duration<double>(Clock::now() - t0).count();
        expect(code == 137 && s < 5.0, "killed within 5s with no swap", log);
        std::string log2;
        int code2 = run({"--memory", "64M", "--swap", "256M", "--", "/usr/local/bin/memhog", "100"}, log2);
        expect(code2 == 0, "survives when 256M swap allowed (shows swap limit is what kills)", log2);
    });

    test("T6 host filesystem is not reachable", [] {
        const std::string marker = "/root/.crun_host_marker";
        { std::ofstream(marker) << "secret"; }
        std::string log;
        int code = run({"--", "/bin/sh", "-c", "cat " + marker + " || echo NO_ACCESS; test -e /.oldroot && echo OLDROOT_VISIBLE; true"}, log);
        unlink(marker.c_str());
        expect(code == 0, "exit code 0", log);
        expect(contains(log, "NO_ACCESS") && !contains(log, "secret"), "host /root not visible", log);
        expect(!contains(log, "OLDROOT_VISIBLE"), "old root detached", log);
    });

    test("T7 SIGINT stops container and removes cgroup", [] {
        std::string log;
        Proc p = start({"--", "/bin/sleep", "30"});
        std::string cg = wait_cgroup(p, log);
        auto t0 = Clock::now();
        kill(p.pid, SIGINT);
        finish(p, log);
        double s = std::chrono::duration<double>(Clock::now() - t0).count();
        expect(s < 5.0, "stopped within 5s", log);
        expect(!cg.empty() && !exists(cg), "cgroup removed", log);
    });

    test("T8 5 concurrent containers enforce their own limits", [] {
        // 48 MiB job: must die under 32M, must survive under 64M/128M.
        const std::vector<std::string> limits = {"32M", "64M", "128M", "32M", "64M"};
        std::vector<Proc> procs;
        for (const auto& l : limits) procs.push_back(start({"--memory", l, "--", "/usr/local/bin/memhog", "48", "1"}));
        for (size_t i = 0; i < procs.size(); ++i) {
            std::string log;
            int code = finish(procs[i], log);
            bool should_die = limits[i] == "32M";
            expect(should_die ? code == 137 : code == 0,
                   "container " + std::to_string(i) + " (" + limits[i] + ") " + (should_die ? "OOM killed" : "succeeded"),
                   log);
        }
    });

    test("T9 elastic limit shrinks to the working set and uses less memory than static", [] {
        std::string log;
        Proc p = start({"--elastic", "--memory", "128M", "--", "/usr/local/bin/phasehog"});
        std::string cg = wait_cgroup(p, log);
        bool squeezed = false;
        for (int i = 0; i < 200 && !squeezed && exists(cg); ++i) {  // watch memory.high from the host
            std::string high = slurp(cg + "/memory.high");
            squeezed = high != "max" && !high.empty() && std::stoll(high) < (128LL << 20);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        int code = finish(p, log);
        std::string static_log;
        int static_code = run({"--memory", "128M", "--", "/usr/local/bin/phasehog"}, static_log);
        double elastic_avg = number_after(log, "memory: avg ");
        double static_avg = number_after(static_log, "memory: avg ");
        expect(code == 0 && static_code == 0, "both runs exit 0", log + static_log);
        expect(squeezed, "memory.high lowered below memory.max at runtime", log);
        expect(number_after(log, " grow / ") > 0, "controller shrank the limit", log);
        expect(contains(log, "recommended --memory"), "right-size recommendation printed", log);
        expect(elastic_avg > 0 && elastic_avg < static_avg,
               "elastic avg " + std::to_string(elastic_avg) + " MiB < static avg " + std::to_string(static_avg) + " MiB",
               log + static_log);
    });

    test("T10 soft-oom kill: frozen and diagnosed before the kernel OOM killer", [] {
        std::string log;
        int code = run({"--memory", "64M", "--soft-oom", "kill", "--", "/usr/local/bin/memhog", "100"}, log);
        expect(code == 137, "exit code 137", log);
        expect(contains(log, "soft landing: container frozen"), "container frozen", log);
        expect(contains(log, "memhog") && contains(log, "RSS"), "diagnostics name the culprit", log);
        expect(contains(log, "kernel OOM kills 0"), "kernel OOM killer never fired", log);
    });

    test("T11 soft-oom grant: limit raised from reserve, job completes", [] {
        std::string log;
        int code = run({"--memory", "64M", "--soft-oom", "grant", "--reserve", "32M", "--",
                        "/usr/local/bin/memhog", "70"}, log);
        expect(code == 0, "exit code 0", log);
        expect(contains(log, "granted +"), "memory granted", log);
        expect(contains(log, "memhog: allocated 70 MiB"), "allocation completed", log);
        expect(contains(log, "kernel OOM kills 0"), "no kernel OOM", log);
    });

    test("T12 soft-oom notify: app frees memory on SIGUSR1 and recovers", [] {
        std::string log;
        int code = run({"--memory", "64M", "--soft-oom", "notify", "--", "/usr/local/bin/phasehog", "0", "40"}, log);
        expect(code == 0, "exit code 0", log);
        expect(contains(log, "notified 1 process"), "only the handler-owning process signalled", log);
        expect(contains(log, "released spike on SIGUSR1"), "app released memory", log);
        expect(contains(log, "app recovered"), "runtime saw recovery", log);
        expect(contains(log, "grant(s) (+0.0 MiB)") && contains(log, "kernel OOM kills 0"), "no extra memory, no OOM", log);
    });

    test("T13 flight recorder writes CSV and HTML", [] {
        const std::string prefix = "/tmp/crun-t13-" + std::to_string(getpid());
        std::string log;
        int code = run({"--memory", "64M", "--record", prefix, "--", "/usr/local/bin/memhog", "20", "1"}, log);
        std::string csv = slurp(prefix + ".csv"), html = slurp(prefix + ".html");
        int rows = 0;
        for (char c : csv) rows += c == '\n';
        expect(code == 0, "exit code 0", log);
        expect(csv.rfind("t_ms,current,high,max", 0) == 0 && rows > 5, "CSV has header and rows", log);
        expect(contains(html, "<svg") && contains(html, "memory.current"), "HTML has charts", log);
        unlink((prefix + ".csv").c_str());
        unlink((prefix + ".html").c_str());
    });

    std::printf(g_failures == 0 ? "all integration tests passed\n" : "%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
