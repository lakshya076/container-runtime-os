// Unit tests for pure logic (no root needed). Run with `make test`.

#include "cgroup.hpp"
#include "cli.hpp"
#include "elastic.hpp"
#include "pressure.hpp"
#include "recorder.hpp"
#include "softland.hpp"

#include <cstdio>
#include <stdexcept>

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

template <typename F>
static bool throws(F f) {
    try { f(); } catch (const std::invalid_argument&) { return true; }
    return false;
}

int main() {
    // parse_size
    CHECK(parse_size("4096") == 4096);
    CHECK(parse_size("64M") == 67108864);
    CHECK(parse_size("64m") == 67108864);
    CHECK(parse_size("512K") == 524288);
    CHECK(parse_size("1G") == 1073741824);
    CHECK(parse_size("0") == 0);
    CHECK(parse_size("max") == -1);
    CHECK(throws([] { parse_size(""); }));
    CHECK(throws([] { parse_size("M"); }));
    CHECK(throws([] { parse_size("-5M"); }));
    CHECK(throws([] { parse_size("12X"); }));
    CHECK(throws([] { parse_size("12MB"); }));
    CHECK(throws([] { parse_size("99999999999999G"); }));

    // parse_run_args
    Options o = parse_run_args({"--memory", "128M", "--rootfs", "/r", "/bin/sh", "-c", "echo hi"});
    CHECK(o.memory == 134217728);
    CHECK(o.swap == 0);
    CHECK(o.pids == -1);
    CHECK(o.rootfs == "/r");
    CHECK(o.argv.size() == 3 && o.argv[0] == "/bin/sh" && o.argv[2] == "echo hi");

    o = parse_run_args({"--rootfs", "/r", "--swap", "max", "--pids", "64", "--stats", "500",
                        "--hostname", "box", "--", "--weird-cmd"});
    CHECK(o.swap == -1);
    CHECK(o.pids == 64);
    CHECK(o.stats_ms == 500);
    CHECK(o.hostname == "box");
    CHECK(o.argv.size() == 1 && o.argv[0] == "--weird-cmd");

    CHECK(throws([] { parse_run_args({"/bin/sh"}); }));                         // no rootfs
    CHECK(throws([] { parse_run_args({"--rootfs", "/r"}); }));                  // no command
    CHECK(throws([] { parse_run_args({"--rootfs"}); }));                        // missing value
    CHECK(throws([] { parse_run_args({"--bogus", "1", "--rootfs", "/r", "x"}); }));
    CHECK(throws([] { parse_run_args({"--memory", "0", "--rootfs", "/r", "x"}); }));
    CHECK(throws([] { parse_run_args({"--pids", "-1", "--rootfs", "/r", "x"}); }));
    CHECK(throws([] { parse_run_args({"--stats", "fast", "--rootfs", "/r", "x"}); }));

    // parse_keyed (memory.events format)
    const std::string events = "low 0\nhigh 0\nmax 12\noom 2\noom_kill 1\n";
    CHECK(parse_keyed(events, "oom_kill") == 1);
    CHECK(parse_keyed(events, "max") == 12);
    CHECK(parse_keyed(events, "oom") == 2);
    CHECK(parse_keyed(events, "missing") == -1);
    CHECK(parse_keyed("", "oom_kill") == -1);

    // elastic engine flags
    o = parse_run_args({"--memory", "128M", "--elastic", "--soft-oom", "grant", "--rootfs", "/r", "x"});
    CHECK(o.elastic && o.soft_oom == SoftOom::grant);
    CHECK(o.memory_min == 16LL << 20);  // max(16M, 128M/8)
    CHECK(o.reserve == 32LL << 20);     // 128M/4
    o = parse_run_args({"--memory", "1G", "--elastic", "--memory-min", "200M", "--record", "out", "--rootfs", "/r", "x"});
    CHECK(o.memory_min == 200LL << 20 && o.record == "out" && o.soft_oom == SoftOom::off);
    o = parse_run_args({"--memory", "16M", "--elastic", "--rootfs", "/r", "x"});
    CHECK(o.memory_min == 8LL << 20);  // capped at memory/2 for small limits
    CHECK(throws([] { parse_run_args({"--elastic", "--rootfs", "/r", "x"}); }));                     // no --memory
    CHECK(throws([] { parse_run_args({"--soft-oom", "kill", "--rootfs", "/r", "x"}); }));            // no --memory
    CHECK(throws([] { parse_run_args({"--memory", "64M", "--soft-oom", "maybe", "--rootfs", "/r", "x"}); }));
    CHECK(throws([] { parse_run_args({"--memory", "64M", "--memory-min", "8M", "--rootfs", "/r", "x"}); }));  // no --elastic
    CHECK(throws([] { parse_run_args({"--memory", "64M", "--reserve", "8M", "--rootfs", "/r", "x"}); }));     // no --soft-oom
    CHECK(throws([] { parse_run_args({"--memory", "64M", "--elastic", "--memory-min", "64M", "--rootfs", "/r", "x"}); }));

    // pressure probe helpers
    CHECK(parse_psi_total("some avg10=0.00 avg60=0.00 avg300=0.00 total=12345\nfull avg10=0 avg60=0 avg300=0 total=7\n") == 12345);
    CHECK(parse_psi_total("full avg10=0 total=7\n") == -1);
    CHECK(parse_psi_total("") == -1);
    CHECK(parse_limit("max\n") == -1);
    CHECK(parse_limit("67108864\n") == 67108864);
    CHECK(over_high(102, 100) && !over_high(100, 100) && !over_high(500, -1));
    CHECK(is_pressured(true, 6.0, 0, 0, 0) && !is_pressured(true, 4.0, 9999, 9, 999));  // PSI decides alone
    CHECK(is_pressured(false, -1, 300, 0, 0));                                         // thrashing
    CHECK(is_pressured(false, -1, 0, 2, 0) && !is_pressured(false, -1, 0, 1, 0));      // reclaim failing
    CHECK(is_pressured(false, -1, 0, 0, 25) && !is_pressured(false, -1, 0, 0, 15));    // throttled
    CHECK(!is_pressured(false, -1, 100, 0, 0));

    // elastic controller
    {
        const long long M = 1LL << 20;
        ElasticController c(10 * M, 100 * M);
        CHECK(c.high() == 100 * M);
        CHECK(c.step(false, 20 * M) == 24 * M);  // collapse to 1.2 x current
        long long h = c.step(false, 20 * M);
        CHECK(h < 24 * M && h > 23 * M);         // then slow 3% shrink
        h = c.step(true, 30 * M);
        CHECK(h == 33 * M);                       // max(h*1.25, 30*1.1) = 33M
        for (int i = 0; i < kCooldownSteps; ++i) CHECK(c.step(false, 5 * M) == 33 * M);  // cooldown
        CHECK(c.step(false, 5 * M) == 10 * M);   // collapse, clamped at floor
        CHECK(c.step(true, 200 * M) == 100 * M);  // clamped at ceiling
        c.set_ceiling(50 * M);
        CHECK(c.high() == 50 * M && c.ceiling() == 50 * M);
        CHECK(c.grows() == 2 && c.shrinks() == 3);
        CHECK(recommend_limit(40 * M, 30 * M) == 48 * M);  // 48M is already a multiple of 4M
        CHECK(recommend_limit(10 * M, 30 * M) == 32 * M);  // never below the peak
    }

    // soft landing helpers
    {
        const long long M = 1LL << 20;
        CHECK(tripwire(64 * M) == 64 * M - 64 * M / 10);
        CHECK(tripwire(20 * M) == 16 * M);  // 4 MiB minimum margin
        CHECK(tripwire(4 * M) == 2 * M);    // margin capped at half for tiny limits
        CHECK(should_land(58 * M, 1 * M, 0, 57 * M));    // anon-full: land
        CHECK(!should_land(58 * M, 40 * M, 0, 57 * M));  // mostly page cache: let reclaim work
        CHECK(should_land(58 * M, 40 * M, 40 * M, 57 * M));  // tmpfs/shmem is not reclaimable
        CHECK(!should_land(50 * M, 0, 0, 57 * M));       // below tripwire
        CHECK(catches_signal("0000000000000400", 11));   // bit 10 = SIGSEGV(11)
        CHECK(catches_signal("0000000000000200", 10) && !catches_signal("0000000000000400", 10));
        CHECK(!catches_signal("zz", 10));
    }

    // recorder
    {
        Row r;
        r.s.t_ms = 100;
        r.s.c.current = 5;
        r.s.c.high = -1;
        r.s.c.max = 64;
        r.s.pressured = true;
        r.event = "grow";
        CHECK(csv_row(r) == "100,5,-1,64,0,0,0,0.0,0.0,0.0,1,-1.00,grow\n");
        std::vector<Row> rows(4, r);
        rows[3].s.c.current = 9;
        rows[3].s.t_ms = 400;
        Summary s = summarize(rows);
        CHECK(s.samples == 4 && s.peak == 9 && s.ws_p95 == 9 && s.ws_peak == 9);
        CHECK(s.avg_current == 6.0 && s.avg_limit == 64.0 && s.runtime_s == 0.4);
        std::string html = render_html(rows, "t<1>", {{"command", "a&b"}});
        CHECK(html.find("<svg") != std::string::npos && html.find("t&lt;1&gt;") != std::string::npos &&
              html.find("a&amp;b") != std::string::npos);
    }

    if (failures == 0) std::printf("all unit tests passed\n");
    return failures == 0 ? 0 : 1;
}
