#include "image.hpp"
#include "util.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <sys/wait.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string join(const std::vector<std::string>& argv) {
    std::string s;
    for (const auto& a : argv) s += (s.empty() ? "" : " ") + a;
    return s;
}

// fork + execvp, with stdin/stdout optionally redirected. No shell, so arguments are never re-parsed.
pid_t spawn_cmd(const std::vector<std::string>& argv, int in_fd, int out_fd) {
    pid_t pid = static_cast<pid_t>(check(fork(), "fork"));
    if (pid == 0) {
        if (in_fd >= 0) dup2(in_fd, 0);
        if (out_fd >= 0) dup2(out_fd, 1);
        std::vector<char*> a;
        for (const auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        execvp(a[0], a.data());
        std::fprintf(stderr, "crun: exec %s: %s\n", a[0], std::strerror(errno));
        _exit(127);
    }
    return pid;
}

int wait_exit(pid_t pid) {
    int st = 0;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

// Runs a command and throws if it fails. With `out`, captures its stdout.
void run_ok(const std::vector<std::string>& argv, std::string* out = nullptr) {
    int p[2] = {-1, -1};
    if (out) check(pipe2(p, O_CLOEXEC), "pipe");
    pid_t pid = spawn_cmd(argv, -1, p[1]);
    if (out) {
        close(p[1]);
        char buf[4096];
        ssize_t n;
        while ((n = read(p[0], buf, sizeof buf)) > 0) out->append(buf, static_cast<size_t>(n));
        close(p[0]);
    }
    int code = wait_exit(pid);
    if (code != 0) throw std::runtime_error("'" + join(argv) + "' failed (exit " + std::to_string(code) + ")");
}

}  // namespace

void pull_image(const PullOptions& opt) {
    std::error_code ec;
    if (fs::exists(opt.dest, ec) && !(fs::is_directory(opt.dest, ec) && fs::is_empty(opt.dest, ec)))
        throw std::runtime_error(opt.dest + " already exists and is not empty; remove it to pull again");

    run_ok({"docker", "pull", opt.image});
    std::string id;
    // The container is never started; it only exists so `docker export` can stream its filesystem.
    run_ok({"docker", "create", opt.image, "true"}, &id);
    id.erase(id.find_last_not_of(" \r\n\t") + 1);
    if (id.empty()) throw std::runtime_error("docker create printed no container id");

    try {
        fs::create_directories(opt.dest);
        int p[2];
        check(pipe2(p, O_CLOEXEC), "pipe");
        pid_t exporter = spawn_cmd({"docker", "export", id}, -1, p[1]);
        // --numeric-owner: keep the image's uids/gids instead of remapping by the host's /etc/passwd.
        pid_t untar = spawn_cmd({"tar", "-x", "-p", "--numeric-owner", "-C", opt.dest, "-f", "-"}, p[0], -1);
        close(p[0]);
        close(p[1]);
        int e = wait_exit(exporter), t = wait_exit(untar);
        if (e != 0 || t != 0)
            throw std::runtime_error("docker export | tar failed (exit " + std::to_string(e) + " / " + std::to_string(t) + ")");
    } catch (...) {
        fs::remove_all(opt.dest, ec);  // safe: DEST was absent or empty before this pull
        std::string ignored;
        try { run_ok({"docker", "rm", id}, &ignored); } catch (...) {}
        throw;
    }
    try {
        std::string ignored;  // docker rm echoes the id
        run_ok({"docker", "rm", id}, &ignored);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[crun] warning: could not remove helper container %s: %s\n", id.c_str(), e.what());
    }
}
