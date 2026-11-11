// Phased test workload for the elastic memory engine.
// usage: phasehog [FILE_MIB=100] [SPIKE_MIB=40] [DIR=/var/tmp]
//   1. 30 MiB anon working set, hold 2 s
//   2. write + read a FILE_MIB file once (cold page cache), hold 3 s
//   3. SPIKE_MIB anon "optional cache" for 1 s; released early on SIGUSR1
//   4. shrink to 10 MiB, hold 2 s

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

volatile sig_atomic_t g_release = 0;
void on_usr1(int) { g_release = 1; }

constexpr size_t kMiB = 1 << 20;

// mmap/munmap instead of malloc so freeing really returns memory to the kernel.
void grow(std::vector<char*>& v, int mib, bool stop_on_signal) {
    for (int i = 0; i < mib && !(stop_on_signal && g_release); ++i) {
        void* p = mmap(nullptr, kMiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { std::perror("phasehog: mmap"); std::exit(1); }
        std::memset(p, 0x5A, kMiB);
        v.push_back(static_cast<char*>(p));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));  // ~200 MiB/s, visible on the chart
    }
}

void shrink(std::vector<char*>& v, size_t keep) {
    while (v.size() > keep) { munmap(v.back(), kMiB); v.pop_back(); }
}

void hold(int ms, bool stop_on_signal) {
    for (int i = 0; i < ms / 10 && !(stop_on_signal && g_release); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void say(const char* msg) { std::printf("phasehog: %s\n", msg); std::fflush(stdout); }

}  // namespace

int main(int argc, char** argv) {
    const int file_mib = argc > 1 ? std::atoi(argv[1]) : 100;
    const int spike_mib = argc > 2 ? std::atoi(argv[2]) : 40;
    const std::string dir = argc > 3 ? argv[3] : "/var/tmp";
    std::signal(SIGUSR1, on_usr1);

    std::vector<char*> base, spike;
    say("phase 1: 30 MiB working set");
    grow(base, 30, false);
    hold(2000, false);

    int fd = -1;  // kept open to the end: the cold cache stays charged until then
    if (file_mib > 0) {
        say("phase 2: cold file cache");
        // Unique name, unlinked at once: a killed run leaves nothing behind. Rewriting a file whose
        // cache belonged to an OOM-killed container was observed to make writes 10-20x slower.
        std::string tmpl = dir + "/phasehog.XXXXXX";
        fd = mkstemp(tmpl.data());
        if (fd < 0) { std::perror("phasehog: mkstemp"); return 1; }
        unlink(tmpl.c_str());
        std::vector<char> buf(kMiB, 'x');
        for (int i = 0; i < file_mib; ++i)
            if (write(fd, buf.data(), buf.size()) != static_cast<ssize_t>(buf.size())) { std::perror("phasehog: write"); return 1; }
        fsync(fd);
        lseek(fd, 0, SEEK_SET);
        while (read(fd, buf.data(), buf.size()) > 0) {}
        hold(3000, false);
    }

    say("phase 3: optional cache spike");
    grow(spike, spike_mib, true);
    hold(1000, true);
    if (g_release) say("released spike on SIGUSR1");
    shrink(spike, 0);

    say("phase 4: shrink to 10 MiB");
    shrink(base, 10);
    hold(2000, false);
    if (fd >= 0) close(fd);  // last reference: the file and its cache are freed here
    say("done");
    return 0;
}
