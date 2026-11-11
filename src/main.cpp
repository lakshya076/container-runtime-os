// crun: a lightweight Linux container runtime with cgroup v2 memory enforcement.

#include "cgroup.hpp"
#include "cli.hpp"
#include "monitor.hpp"
#include "sandbox.hpp"
#include "softland.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

static std::string limit(long long v) { return v < 0 ? "max" : std::to_string(v); }

int main(int argc, char** argv) {
    const auto t0 = std::chrono::steady_clock::now();
    if (argc < 2 || std::string(argv[1]) != "run") {
        std::cerr << kUsage;
        return 2;
    }
    Options opt;
    try {
        opt = parse_run_args(std::vector<std::string>(argv + 2, argv + argc));
    } catch (const std::invalid_argument& e) {
        std::cerr << "crun: " << e.what() << "\n" << kUsage;
        return 2;
    }

    try {
        if (geteuid() != 0) throw std::runtime_error("must run as root");
        if (!cgroup2_mounted())
            throw std::runtime_error(std::string(kCgroupRoot) + " is not a cgroup v2 mount (see README: WSL2 setup)");

        // Block before clone() so an early SIGCHLD stays pending for the signalfd.
        sigset_t sigs;
        sigemptyset(&sigs);
        sigaddset(&sigs, SIGCHLD);
        sigaddset(&sigs, SIGINT);
        sigaddset(&sigs, SIGTERM);
        sigprocmask(SIG_BLOCK, &sigs, nullptr);

        Sandbox sb = spawn(opt);
        try {
            Cgroup cg("container_" + std::to_string(sb.pid));
            cg.set("memory.max", limit(opt.memory));
            if (cg.has("memory.swap.max")) cg.set("memory.swap.max", limit(opt.swap));
            else std::fprintf(stderr, "[crun] warning: kernel has no swap accounting, --swap ignored\n");
            cg.set("memory.oom.group", "1");  // OOM kills the whole container, not one random task
            cg.set("pids.max", limit(opt.pids));
            if (opt.soft_oom != SoftOom::off) {
                if (!cg.has("cgroup.freeze")) throw std::runtime_error("--soft-oom needs cgroup.freeze (kernel 5.2+)");
                // Tripwire armed before the workload starts.
                cg.set("memory.high", std::to_string(tripwire(opt.memory)));
            }
            cg.add(sb.pid);
            release(sb);

            auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::fprintf(stderr, "[crun] cgroup %s\n", cg.path().c_str());
            std::fprintf(stderr, "[crun] container pid %d started in %.2f ms (memory.max=%s)\n", sb.pid, ms,
                         limit(opt.memory).c_str());
            return supervise(sb.pid, cg, opt, sigs);
        } catch (...) {
            kill(sb.pid, SIGKILL);
            waitpid(sb.pid, nullptr, 0);
            throw;
        }
    } catch (const std::exception& e) {
        std::cerr << "crun: " << e.what() << "\n";
        return 125;
    }
}
