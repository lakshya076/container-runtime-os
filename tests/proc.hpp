#pragma once
// Helpers shared by the integration tests and the eval harness: run crun and capture its output.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

inline std::string g_crun, g_rootfs;

// A running crun process with stdout+stderr merged into one pipe.
struct Proc {
    pid_t pid = -1;
    FILE* out = nullptr;
};

inline Proc start(std::vector<std::string> args) {
    args.insert(args.begin(), {g_crun, "run", "--rootfs", g_rootfs});
    int fds[2];
    if (pipe(fds) < 0) { perror("pipe"); _exit(1); }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        perror("execv");
        _exit(127);
    }
    close(fds[1]);
    return {pid, fdopen(fds[0], "r")};
}

inline bool read_line(Proc& p, std::string& line) {
    char* buf = nullptr;
    size_t cap = 0;
    ssize_t n = getline(&buf, &cap, p.out);
    if (n >= 0) line.assign(buf, static_cast<size_t>(n));
    free(buf);
    return n >= 0;
}

// Reads output until a "[crun] cgroup <path>" line appears; returns the path.
inline std::string wait_cgroup(Proc& p, std::string& log) {
    std::string line;
    while (read_line(p, line)) {
        log += line;
        auto pos = line.find("[crun] cgroup ");
        if (pos != std::string::npos) return line.substr(pos + 14, line.find('\n') - pos - 14);
    }
    return "";
}

inline int finish(Proc& p, std::string& log) {
    std::string line;
    while (read_line(p, line)) log += line;
    fclose(p.out);
    int st = 0;
    waitpid(p.pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

inline int run(std::vector<std::string> args, std::string& log) {
    Proc p = start(std::move(args));
    return finish(p, log);
}

inline bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// Number right after `key` in the log (e.g. key "avg " in "memory: avg 41.2 MiB"), or -1.
inline double number_after(const std::string& log, const std::string& key) {
    auto pos = log.find(key);
    if (pos == std::string::npos) return -1;
    return std::atof(log.c_str() + pos + key.size());
}
