#include "monitor.hpp"
#include "elastic.hpp"
#include "pressure.hpp"
#include "recorder.hpp"
#include "softland.hpp"
#include "util.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/wait.h>

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kGrace = std::chrono::seconds(2);  // SIGTERM -> SIGKILL escalation delay
constexpr int kTickMs = 100;                      // sampling / recording period
constexpr int kControlTicks = 2;                  // elastic controller runs every 200 ms

double mib(double bytes) { return bytes / (1 << 20); }

long long oom_kills(int events_fd) {
    char buf[512];
    ssize_t n = pread(events_fd, buf, sizeof buf - 1, 0);
    if (n <= 0) return -1;
    return parse_keyed(std::string(buf, static_cast<size_t>(n)), "oom_kill");
}

long long read_current(const Cgroup& cg) {
    try { return std::stoll(cg.get("memory.current")); } catch (...) { return 0; }
}

std::string mib_str(double bytes) { char b[32]; std::snprintf(b, sizeof b, "%.1f MiB", mib(bytes)); return b; }

}  // namespace

int supervise(pid_t pid, const Cgroup& cg, const Options& opt, const sigset_t& sigs) {
    int sfd = static_cast<int>(check(signalfd(-1, &sigs, SFD_CLOEXEC), "signalfd"));
    int efd = static_cast<int>(check(open((cg.path() + "/memory.events").c_str(), O_RDONLY | O_CLOEXEC), "open memory.events"));
    int tfd = static_cast<int>(check(timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC), "timerfd_create"));
    int ep = static_cast<int>(check(epoll_create1(EPOLL_CLOEXEC), "epoll_create1"));

    itimerspec its{};
    its.it_interval.tv_nsec = kTickMs * 1000000L;
    its.it_value = its.it_interval;
    check(timerfd_settime(tfd, 0, &its, nullptr), "timerfd_settime");

    // cgroup v2 notifies changes of memory.events as EPOLLPRI on the open file.
    epoll_event ev{};
    ev.events = EPOLLIN; ev.data.fd = sfd; check(epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &ev), "epoll add signalfd");
    ev.events = EPOLLPRI; ev.data.fd = efd; check(epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev), "epoll add memory.events");
    ev.events = EPOLLIN; ev.data.fd = tfd; check(epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev), "epoll add timerfd");

    const auto start = Clock::now();
    PressureProbe probe(cg);
    std::optional<SoftLanding> land;
    if (opt.soft_oom != SoftOom::off) land.emplace(cg, opt.soft_oom, opt.memory, opt.reserve);
    std::optional<ElasticController> ctl;
    if (opt.elastic) ctl.emplace(opt.memory_min, land ? land->tripwire() : opt.memory);
    Recorder rec(opt.record);

    long long ooms = std::max(0LL, oom_kills(efd)), kernel_ooms = 0;
    constexpr auto kNever = Clock::time_point::max();
    Clock::time_point kill_at = kNever;  // when to escalate SIGTERM to SIGKILL
    std::string pending;  // events to attach to the next recorded row
    int status = 0, tick = 0;
    bool exited = false, pressured_since_step = false;
    long long next_stats_ms = opt.stats_ms;

    auto note = [&](const char* e) { pending += (pending.empty() ? "" : ";") + std::string(e); };
    auto set_high = [&](long long v) { try { cg.set("memory.high", std::to_string(v)); } catch (...) {} };

    auto check_oom = [&] {
        long long now = oom_kills(efd);
        if (now > ooms) {
            std::fprintf(stderr, "[crun] OOM: kernel OOM killer fired (%lld kill(s)); memory limit %.1f MiB breached\n",
                         now - ooms, mib(static_cast<double>(land ? land->max() : opt.memory)));
            if (land) std::fprintf(stderr, "[crun] soft landing missed: kernel killed before the freeze\n");
            kernel_ooms += now - ooms;
            ooms = now;
            note("oom_kill");
        }
    };

    auto handle = [&](SoftLanding::Outcome o, long long current) {
        switch (o) {
            case SoftLanding::Outcome::killed: note("freeze"); note("kill"); break;
            case SoftLanding::Outcome::notified: note("freeze"); note("notify"); break;
            case SoftLanding::Outcome::recovered: note("recovered"); break;
            case SoftLanding::Outcome::granted:
                note("freeze");
                note("grant");
                // Move the tripwire up with the new memory.max.
                if (ctl) {
                    ctl->set_ceiling(land->tripwire());
                    ctl->step(true, current);
                    set_high(ctl->high());
                } else {
                    set_high(land->tripwire());
                }
                break;
            case SoftLanding::Outcome::none: break;
        }
    };

    while (!exited) {
        int timeout = -1;
        if (kill_at != kNever) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(kill_at - Clock::now()).count();
            timeout = static_cast<int>(std::max<long long>(0, left));
        }
        epoll_event evs[4];
        int n = epoll_wait(ep, evs, 4, timeout);
        if (n < 0 && errno == EINTR) continue;
        check(n, "epoll_wait");
        if (kill_at != kNever && Clock::now() >= kill_at) {
            std::fprintf(stderr, "[crun] container did not stop, sending SIGKILL\n");
            kill(pid, SIGKILL);
            kill_at = kNever;
        }
        for (int i = 0; i < n; ++i) {
            int fd = evs[i].data.fd;
            if (fd == efd) {
                check_oom();
                // memory.high breaches also land here: react before the next tick.
                if (land) handle(land->check(Clock::now()), read_current(cg));
            } else if (fd == tfd) {
                uint64_t ticks;
                if (read(tfd, &ticks, sizeof ticks) < 0) continue;
                ++tick;
                const long long t_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
                Sample s = probe.sample(t_ms);
                pressured_since_step = pressured_since_step || s.pressured;
                if (ctl && tick % kControlTicks == 0) {
                    long long before = ctl->high();
                    long long now_high = ctl->step(pressured_since_step, s.c.current);
                    pressured_since_step = false;
                    if (now_high != before) {
                        set_high(now_high);
                        note(now_high > before ? "grow" : "shrink");
                    }
                }
                if (land) handle(land->check(Clock::now()), s.c.current);
                rec.add(s, pending);
                pending.clear();
                if (opt.stats_ms > 0 && t_ms >= next_stats_ms) {
                    next_stats_ms = t_ms + opt.stats_ms;
                    std::fprintf(stderr, "[crun] mem %.1f MiB / %.1f MiB (%.0f%%)", mib(static_cast<double>(s.c.current)),
                                 mib(static_cast<double>(opt.memory > 0 ? opt.memory : 0)),
                                 opt.memory > 0 ? 100.0 * static_cast<double>(s.c.current) / static_cast<double>(opt.memory) : 0.0);
                    if (ctl) std::fprintf(stderr, " high %.1f MiB%s", mib(static_cast<double>(ctl->high())), s.pressured ? " [pressure]" : "");
                    std::fprintf(stderr, "\n");
                }
            } else if (fd == sfd) {
                signalfd_siginfo si;
                if (read(sfd, &si, sizeof si) != sizeof si) continue;
                if (si.ssi_signo == SIGCHLD) {
                    if (waitpid(pid, &status, WNOHANG) == pid) exited = true;
                } else if (kill_at != kNever) {  // second Ctrl+C: stop waiting
                    kill(pid, SIGKILL);
                    kill_at = kNever;
                } else {
                    if (land) land->thaw();  // a frozen container cannot react to SIGTERM
                    std::fprintf(stderr, "[crun] got signal %u, sending SIGTERM to container\n", si.ssi_signo);
                    kill(pid, SIGTERM);
                    kill_at = Clock::now() + kGrace;
                }
            }
        }
    }

    check_oom();  // OOM event and SIGCHLD can land in the same wakeup
    close(ep); close(tfd); close(efd); close(sfd);

    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    const double runtime = std::chrono::duration<double>(Clock::now() - start).count();
    const Summary sm = summarize(rec.rows());

    if (WIFSIGNALED(status)) {
        const char* why = "";
        if (WTERMSIG(status) == SIGKILL && land && land->killed()) why = " (soft-landing kill)";
        else if (WTERMSIG(status) == SIGKILL && kernel_ooms > 0) why = " (out of memory)";
        std::fprintf(stderr, "[crun] container killed by signal %d%s\n", WTERMSIG(status), why);
    }
    std::fprintf(stderr, "[crun] exit code %d, runtime %.2f s\n", code, runtime);

    std::vector<std::pair<std::string, std::string>> info;
    std::string cmd;
    for (const auto& a : opt.argv) cmd += (cmd.empty() ? "" : " ") + a;
    info.push_back({"command", cmd});
    info.push_back({"exit code", std::to_string(code)});
    char rt[32];
    std::snprintf(rt, sizeof rt, "%.2f s", runtime);
    info.push_back({"runtime", rt});
    info.push_back({"memory.max", opt.memory > 0 ? mib_str(static_cast<double>(opt.memory)) : "max"});
    info.push_back({"pressure source", probe.has_psi() ? "PSI (memory.pressure)" : "estimator: refaults, memory.high overrun and reclaim events (no PSI)"});

    if (sm.samples > 0) {
        std::string lim = sm.avg_limit > 0 ? mib_str(sm.avg_limit) : "unlimited";
        std::fprintf(stderr, "[crun] memory: avg %.1f MiB, peak %.1f MiB, avg limit %s\n", mib(sm.avg_current),
                     mib(static_cast<double>(sm.peak)), lim.c_str());
        info.push_back({"avg memory.current", mib_str(sm.avg_current)});
        info.push_back({"peak memory.current", mib_str(static_cast<double>(sm.peak))});
        info.push_back({"avg effective limit", lim});
    } else {
        std::fprintf(stderr, "[crun] memory: no samples (exited in under %d ms)\n", kTickMs);
    }
    if (ctl) {
        long long rec_limit = recommend_limit(sm.ws_p95, sm.ws_peak);
        std::fprintf(stderr, "[crun] elastic: %d grow / %d shrink, working set ~%.1f MiB, recommended --memory %lldM\n",
                     ctl->grows(), ctl->shrinks(), mib(static_cast<double>(sm.ws_p95)), rec_limit >> 20);
        info.push_back({"elastic floor / ceiling", mib_str(static_cast<double>(ctl->floor())) + " / " +
                                                       mib_str(static_cast<double>(ctl->ceiling()))});
        info.push_back({"elastic grow / shrink", std::to_string(ctl->grows()) + " / " + std::to_string(ctl->shrinks())});
        info.push_back({"working set p95 (current - inactive_file)", mib_str(static_cast<double>(sm.ws_p95))});
        info.push_back({"recommended --memory", std::to_string(rec_limit >> 20) + "M"});
    }
    if (land) {
        std::fprintf(stderr, "[crun] soft-oom (%s): %d freeze(s), %d grant(s) (+%.1f MiB), kernel OOM kills %lld\n",
                     soft_oom_name(opt.soft_oom), land->freezes(), land->grants(),
                     mib(static_cast<double>(land->granted())), kernel_ooms);
        info.push_back({"soft-oom policy", soft_oom_name(opt.soft_oom)});
        info.push_back({"freezes / grants", std::to_string(land->freezes()) + " / " + std::to_string(land->grants()) +
                                                " (+" + mib_str(static_cast<double>(land->granted())) + ")"});
    }
    info.push_back({"kernel OOM kills", std::to_string(kernel_ooms)});

    if (rec.enabled()) {
        if (rec.write_html("crun flight recording", info))
            std::fprintf(stderr, "[crun] recording: %s.csv, %s.html\n", rec.prefix().c_str(), rec.prefix().c_str());
        else
            std::fprintf(stderr, "[crun] warning: cannot write %s.html\n", rec.prefix().c_str());
    }
    return code;
}
