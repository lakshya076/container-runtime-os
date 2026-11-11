#pragma once
// Module 4 (cgroup memory allocation engine) and Module 8 (resource cleanup).

#include <string>
#include <sys/types.h>

constexpr const char* kCgroupRoot = "/sys/fs/cgroup";

// True if /sys/fs/cgroup is a pure cgroup v2 mount.
bool cgroup2_mounted();

// Value of `key` in a cgroup "key value" file such as memory.events; -1 if missing.
long long parse_keyed(const std::string& text, const std::string& key);

// RAII cgroup node: created in the constructor, killed and removed in the destructor.
class Cgroup {
public:
    explicit Cgroup(const std::string& name);
    ~Cgroup();
    Cgroup(const Cgroup&) = delete;
    Cgroup& operator=(const Cgroup&) = delete;

    void set(const std::string& file, const std::string& value) const;
    std::string get(const std::string& file) const;
    bool has(const std::string& file) const;
    void add(pid_t pid) const;
    const std::string& path() const { return path_; }

private:
    std::string path_;
};
