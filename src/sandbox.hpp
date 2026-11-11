#pragma once
// Module 2: process sandbox engine.

#include "cli.hpp"

#include <sys/types.h>

struct Sandbox {
    pid_t pid;    // host PID of the container's init process
    int sync_fd;  // write end of the sync pipe; child waits until release() is called
};

// clone()s the container init into new PID, mount, UTS and IPC namespaces.
// The child blocks until release() so the parent can attach it to a cgroup first.
Sandbox spawn(const Options& opt);

// Lets the child continue to pivot_root and exec.
void release(Sandbox& sb);
