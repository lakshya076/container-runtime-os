// Test workload: allocates N MiB and touches every page so it is really resident.
// usage: memhog MIB [HOLD_SECONDS]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: memhog MIB [HOLD_SECONDS]\n");
        return 2;
    }
    const int mib = std::atoi(argv[1]);
    const int hold = argc > 2 ? std::atoi(argv[2]) : 0;
    std::vector<char*> chunks;
    for (int i = 0; i < mib; ++i) {
        char* p = static_cast<char*>(std::malloc(1 << 20));
        if (!p) {
            std::fprintf(stderr, "memhog: malloc failed after %d MiB\n", i);
            return 1;
        }
        std::memset(p, 0xAB, 1 << 20);
        chunks.push_back(p);
    }
    std::printf("memhog: allocated %d MiB\n", mib);
    std::fflush(stdout);
    if (hold > 0) sleep(static_cast<unsigned>(hold));
    return 0;
}
