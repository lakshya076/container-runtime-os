// Allocation benchmark for the gcc image. It is compiled *inside* the container, so the
// compiler's own memory use is part of the measurement:
//   sh -c 'g++ -O2 -x c++ -o /tmp/allocbench - && /tmp/allocbench' < tests/bench/allocbench.cpp
// Phases:
//   1. fill a 1M-entry hash map cache          (many small heap allocations)
//   2. stream a 200 MiB file, keep it open      (page cache, reclaimable)
//   3. sort 16M random numbers                  (128 MiB temporary spike)
//   4. clear the cache and return it to the OS  (malloc_trim)

#include <malloc.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

static std::string footprint() {
    std::ifstream f("/proc/self/status");
    std::string line, rss = "?", hwm = "?";
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) rss = line.substr(line.find_first_not_of(" \t", 6));
        if (line.rfind("VmHWM:", 0) == 0) hwm = line.substr(line.find_first_not_of(" \t", 6));
    }
    return "rss " + rss + ", peak " + hwm;
}

static void phase(const char* name, int hold_ms = 0) {
    std::printf("allocbench: %-30s %s\n", name, footprint().c_str());
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
}

int main() {
    phase("start");

    std::unordered_map<std::string, std::string> cache;
    cache.reserve(1'000'000);
    for (int i = 0; i < 1'000'000; ++i) cache.emplace("key-" + std::to_string(i), std::string(48, 'v'));
    phase("filled 1M-entry cache", 2000);

    // Unique, unlinked temp file held open: its cache stays charged until close.
    char path[] = "/var/tmp/allocbench.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { std::perror("mkstemp"); return 1; }
    unlink(path);
    std::vector<char> block(1 << 20, 'x');
    for (int i = 0; i < 200; ++i)
        if (write(fd, block.data(), block.size()) != static_cast<ssize_t>(block.size())) { std::perror("write"); return 1; }
    fsync(fd);
    lseek(fd, 0, SEEK_SET);
    while (read(fd, block.data(), block.size()) > 0) {}
    phase("streamed 200 MiB file", 3000);

    {
        std::vector<std::uint64_t> numbers(16'000'000);
        std::mt19937_64 rng(42);
        for (auto& n : numbers) n = rng();
        std::sort(numbers.begin(), numbers.end());
        phase("sorted 16M numbers", 1000);
    }  // large vector: freed straight back to the OS

    cache.clear();
    cache.rehash(0);
    malloc_trim(0);  // small blocks stay in malloc's free lists until trimmed
    phase("released cache", 2000);

    close(fd);
    std::printf("allocbench: done\n");
    return 0;
}
