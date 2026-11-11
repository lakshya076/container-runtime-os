#include "cgroup.hpp"
#include "util.hpp"

#include <cstdio>
#include <cstring>
#include <linux/magic.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <time.h>

bool cgroup2_mounted() {
    struct statfs fs;
    return statfs(kCgroupRoot, &fs) == 0 && fs.f_type == CGROUP2_SUPER_MAGIC;
}

long long parse_keyed(const std::string& text, const std::string& key) {
    std::istringstream in(text);
    std::string k;
    long long v;
    while (in >> k >> v)
        if (k == key) return v;
    return -1;
}

Cgroup::Cgroup(const std::string& name) : path_(std::string(kCgroupRoot) + "/" + name) {
    // Children of the root only get memory/pids files if the root delegates those controllers.
    write_file(std::string(kCgroupRoot) + "/cgroup.subtree_control", "+memory +pids");
    check(mkdir(path_.c_str(), 0755), "mkdir " + path_);
}

Cgroup::~Cgroup() {
    // Kill anything still inside (e.g. orphans if setup failed), then remove the node.
    try { write_file(path_ + "/cgroup.kill", "1"); } catch (...) {}
    for (int i = 0; i < 200; ++i) {  // up to ~2s for dying tasks to leave the cgroup
        if (rmdir(path_.c_str()) == 0 || errno == ENOENT) return;
        if (errno != EBUSY) break;
        timespec ts{0, 10 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    std::fprintf(stderr, "[crun] warning: could not remove %s: %s\n", path_.c_str(), std::strerror(errno));
}

void Cgroup::set(const std::string& file, const std::string& value) const { write_file(path_ + "/" + file, value); }
std::string Cgroup::get(const std::string& file) const { return read_file(path_ + "/" + file); }
bool Cgroup::has(const std::string& file) const { return access((path_ + "/" + file).c_str(), F_OK) == 0; }
void Cgroup::add(pid_t pid) const { set("cgroup.procs", std::to_string(pid)); }
