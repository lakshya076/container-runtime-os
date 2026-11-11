#include "sandbox.hpp"
#include "fs.hpp"
#include "util.hpp"

#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sched.h>
#include <vector>

namespace {

struct ChildArgs {
    const Options* opt;
    std::string rootfs;  // absolute path
    int sync_read;
    int sync_write;
};

// Without CLONE_VM the child gets a copy-on-write copy of this buffer, so one static stack is enough.
alignas(16) char child_stack[1 << 20];

int child_main(void* raw) {
    auto* a = static_cast<ChildArgs*>(raw);
    try {
        close(a->sync_write);
        char go = 0;
        // Parent writes one byte once we are in our cgroup. EOF means the parent gave up.
        if (read(a->sync_read, &go, 1) != 1) return 1;
        close(a->sync_read);

        // The parent blocks these for its signalfd; the container must start with a clean mask.
        sigset_t empty;
        sigemptyset(&empty);
        check(sigprocmask(SIG_SETMASK, &empty, nullptr), "sigprocmask");

        const Options& o = *a->opt;
        check(sethostname(o.hostname.c_str(), o.hostname.size()), "sethostname");
        setup_rootfs(a->rootfs);

        const char* term = getenv("TERM");
        std::string term_copy = term ? term : "xterm";
        clearenv();
        setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
        setenv("HOME", "/root", 1);
        setenv("TERM", term_copy.c_str(), 1);

        std::vector<char*> argv;
        for (const auto& s : o.argv) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        std::fprintf(stderr, "crun: exec %s: %s\n", argv[0], std::strerror(errno));
        return 127;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "crun: container setup failed: %s\n", e.what());
        return 126;
    }
}

}  // namespace

Sandbox spawn(const Options& opt) {
    char abs[PATH_MAX];
    if (!realpath(opt.rootfs.c_str(), abs)) check(-1, "rootfs " + opt.rootfs);

    int fds[2];
    check(pipe2(fds, O_CLOEXEC), "pipe");
    ChildArgs args{&opt, abs, fds[0], fds[1]};

    const int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | SIGCHLD;
    pid_t pid = clone(child_main, child_stack + sizeof child_stack, flags, &args);
    int err = errno;
    close(fds[0]);
    if (pid < 0) {
        close(fds[1]);
        throw std::system_error(err, std::generic_category(), "clone");
    }
    return {pid, fds[1]};
}

void release(Sandbox& sb) {
    char go = 1;
    check(write(sb.sync_fd, &go, 1), "release child");
    close(sb.sync_fd);
    sb.sync_fd = -1;
}
