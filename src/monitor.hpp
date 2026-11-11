#pragma once
// Module 5 (OOM listener), Module 6 (live telemetry), Module 7 (lifecycle & reaper).

#include "cgroup.hpp"
#include "cli.hpp"

#include <csignal>
#include <sys/types.h>

// Runs the epoll loop until the container exits. `sigs` must already be blocked
// (SIGCHLD, SIGINT, SIGTERM). Returns the container's exit code (128+signal if killed).
int supervise(pid_t pid, const Cgroup& cg, const Options& opt, const sigset_t& sigs);
