# crun — A Lightweight Linux Container Runtime with Dynamic Memory Cgroup Enforcement

Operating Systems Lab (24B15CS312) project, Jaypee Institute of Information Technology, Noida.

`crun` is a small container runtime written in C++17. It talks directly to Linux kernel
primitives instead of using a high-level container engine:

- `clone()` with namespaces for process isolation
- `pivot_root` for filesystem isolation
- **cgroup v2** for memory limits

On top of that baseline it adds an **elastic memory engine** that mainstream runtimes do not have:

- **Elastic limits:** the memory limit follows the container's real working set instead of a guessed number.
- **OOM soft landing:** before the kernel OOM killer fires, the container is frozen, the runtime explains
  where the memory went, and a policy decides what happens: kill, grant more memory, or ask the app to free some.
- **Flight recorder:** a CSV + HTML timeline of every run, plus an evaluation harness that compares
  static limits against the engine.

> Status: work in progress. All tests (T1–T13) pass on WSL2, kernel 5.15.

## How to run

crun runs on Linux only, because it calls Linux kernel interfaces directly. Use a Linux machine
or VM with kernel 5.14 or newer, or WSL2 on Windows. Your system must use **cgroup v2**: check
that `stat -fc %T /sys/fs/cgroup` prints `cgroup2fs`. Recent Ubuntu, Debian and Fedora releases
already do. On WSL2 you need a one-time fix first (see [WSL2 setup](#wsl2-setup-windows) below).

1. Install a C++17 compiler, `make` and `curl`:
   ```sh
   sudo apt install build-essential curl
   # Fedora: sudo dnf install gcc-c++ make curl glibc-static libstdc++-static
   ```
2. Clone the repository, build `./crun` and run the unit tests (no root needed):
   ```sh
   git clone https://github.com/lakshya076/container-runtime-os.git
   cd container-runtime-os
   make && make test
   ```
3. Download a small Alpine Linux root filesystem for the containers. Keep it on a Linux
   filesystem, not under `/mnt/c` on WSL:
   ```sh
   sudo make rootfs ROOTFS=/var/lib/crun/rootfs
   ```
4. Copy the test programs `memhog` and `phasehog` into that root filesystem:
   ```sh
   sudo make workloads ROOTFS=/var/lib/crun/rootfs
   ```
5. Run a container. Every container needs root.
   - A shell capped at 64 MB, printing live memory usage:
     ```sh
     sudo ./crun run --memory 64M --stats 500 --rootfs /var/lib/crun/rootfs -- /bin/sh
     ```
     Inside it, `ps` shows the shell as PID 1, and `memhog 100` triggers an OOM kill.
     Leave with `exit` or Ctrl+C.
   - Elastic limits with a recorded report:
     ```sh
     sudo ./crun run --elastic --memory 128M --record /tmp/run --rootfs /var/lib/crun/rootfs -- phasehog
     ```
     Open `/tmp/run.html` in a browser to see the charts.
   - The soft landing:
     ```sh
     sudo ./crun run --memory 64M --soft-oom notify --rootfs /var/lib/crun/rootfs -- phasehog 0 40
     ```
6. Run the full test suite and the static-vs-elastic comparison:
   ```sh
   sudo make itest ROOTFS=/var/lib/crun/rootfs   # 13 tests, about 35 s
   sudo make eval ROOTFS=/var/lib/crun/rootfs    # about 45 s
   ```
   The comparison writes `build/results/index.html` with one chart page per scenario.

## What's new compared to Docker

Docker, runc and Podman set a **static** `memory.max` and let the kernel OOM killer end the
container with exit code 137 and no explanation. A static limit forces a bad choice:

- Set it high and the host wastes RAM on idle page cache.
- Set it low and the container gets killed on its first spike.

| Problem | Existing tools | crun |
|---|---|---|
| Limit doesn't follow real usage | Docker `--memory-reservation` is static. Kubernetes VPA adjusts over minutes and restarts the pod | A feedback controller moves `memory.high` every 200 ms, with no restart |
| OOM is a blind kill | Kernel OOM killer; systemd-oomd also only kills | Freeze → diagnose → kill / grant / notify |
| No pressure signal on some kernels | Meta's senpai tunes `memory.high` from PSI, for systemd services, and needs PSI | Uses PSI when available, otherwise its own estimator (WSL2 ships without PSI) |
| How much memory does the app need? | Guesswork | Exit report: working set + recommended `--memory` |

### Elastic limits (`--elastic`)

`memory.max` stays the hard ceiling. Below it, crun controls `memory.high`, a soft limit: above it
the kernel reclaims and throttles instead of killing. Every 200 ms the controller:

- **pressured:** raises `memory.high` to `max(high × 1.25, current × 1.1)`, then holds it for 1.6 s (cooldown);
- **calm:** jumps down to `current × 1.2` if the limit is far above usage. Otherwise it lowers the
  limit by 3 %, which slowly squeezes out cold page cache until pressure appears.

So the limit grows fast and shrinks slowly, the inverse of TCP's AIMD. It settles just above the
working set.

**Pressure signal:**

- With PSI (`memory.pressure`): more than 5 % of time stalled on memory.
- Without PSI, any of these counts as pressure:
  - **refaults** above 256 pages/s: pages evicted recently were needed again, so the working set is being evicted (thrashing);
  - `memory.current` **stuck above** `memory.high` for 2 samples: reclaim is failing (anon memory with no swap);
  - **memory.high reclaim events** above 20/s (from `memory.events high`): allocations are being forced into reclaim, so the app is throttled.

All thresholds are named constants in `src/pressure.hpp` and `src/elastic.hpp`.

The exit report estimates the **working set** the way the kubelet does (`memory.current − inactive_file`).
It then recommends a static limit: `max(p95 × 1.2, peak)`, rounded up to 4 MiB.

### OOM soft landing (`--soft-oom kill|grant|notify`)

crun sets `memory.high` to a **tripwire**, `memory.max − max(10 %, 4 MiB)`. Above the tripwire the
kernel throttles allocations, which gives crun time to act before the hard limit. The landing fires
only if usage reaches the tripwire **and** at least 90 % of it is unreclaimable (anon, shmem, kernel).
A container that is only streaming files never gets frozen.

1. `cgroup.freeze` stops every process in the container.
2. Diagnostics are printed: the anon / file / slab / shmem breakdown, plus the top processes by RSS.
3. A policy decides what happens next:
   - `kill`: `cgroup.kill`, with a report instead of a silent 137.
   - `grant`: raise `memory.max` from a `--reserve` budget, then thaw. The container is killed only
     when the reserve runs out.
   - `notify`: send `SIGUSR1` only to processes that **handle** it (checked through `SigCgt` in
     `/proc/<pid>/status`, since the default action would kill them), thaw, and allow 1 s of grace.
     If usage falls below 90 % of the tripwire, the container has recovered. Otherwise crun falls
     back to grant, then kill.

```
$ sudo ./crun run --memory 64M --soft-oom notify --rootfs /var/lib/crun/rootfs -- phasehog 0 40
[crun] ---- soft landing: container frozen at 57.8 MiB (tripwire 57.6 MiB, max 64.0 MiB) ----
[crun]   anon 57.4 MiB | file 0.0 MiB | slab 0.0 MiB | shmem 0.0 MiB | kernel_stack 0.0 MiB
[crun]   pid 1064   phasehog         RSS 58.2 MiB
[crun]   action: notified 1 process(es) with SIGUSR1, 1000 ms grace
phasehog: released spike on SIGUSR1
[crun] soft landing: app recovered, now 10.2 MiB
[crun] exit code 0, runtime 4.44 s
[crun] soft-oom (notify): 1 freeze(s), 0 grant(s) (+0.0 MiB), kernel OOM kills 0
```

### Flight recorder (`--record PATH`)

`--record PATH` writes `PATH.csv` with one row per 100 ms. Each row has usage, limits, anon/file/slab,
refault and reclaim-event rates, the pressure flag, and controller and soft-landing events.

It also writes `PATH.html`, a self-contained page with SVG charts generated in C++ (no JavaScript,
no internet). It shows usage vs limits, the pressure timeline, and a summary table.

## Results

`sudo make eval` runs the same phased workload (`tests/phasehog.cpp`) under five setups. The workload:

1. 30 MiB working set
2. 100 MiB file written once (cold page cache)
3. 40 MiB spike
4. shrink to 10 MiB

Measured on WSL2, kernel 5.15:

| Scenario | Exit | Time s | Avg MiB | Peak MiB | Avg limit MiB | Freeze / grant | Kernel OOM |
|---|---|---|---|---|---|---|---|
| S1 static 128M | 0 | 8.79 | 88.9 | 128.0 | 128.0 | 0 / 0 | 0 |
| S2 static 64M | **137** | 5.84 | 51.1 | 64.0 | 64.0 | 0 / 0 | **1** |
| S3 elastic, ceiling 128M | 0 | 10.28 | **52.8** | 93.5 | 69.1 | 0 / 0 | 0 |
| S4 64M + soft-oom grant | 0 | 9.06 | 41.8 | 70.7 | 62.6 | 1 / 1 | 0 |
| S5 64M + soft-oom notify | 0 | 7.96 | 37.9 | 58.7 | 57.6 | 1 / 0 | 0 |

- **Elastic vs static 128M:** 41 % less memory on average (52.8 vs 88.9 MiB), for 17 % more runtime.
  That runtime is the cost of reclaiming cache and throttling. It's the core trade-off and should be
  discussed in the report.
- **Soft landing vs static 64M:** both runs that the kernel would have killed complete.
  - `grant` borrowed 16 MiB once.
  - `notify` needed no extra memory: the app released its optional cache.

The per-scenario charts are in `build/results/index.html`.

## Requirements

- Linux kernel 5.14+ with **cgroup v2 only** mounted at `/sys/fs/cgroup` (`stat -fc %T /sys/fs/cgroup` prints `cgroup2fs`)
- root privileges
- `g++` with C++17 support and static libc/libstdc++ (for the test workloads), `make`, `curl`, `tar`

### WSL2 setup (Windows)

WSL2 mounts cgroup v1 by default, which takes the memory controller away from cgroup v2.
Disable v1 once:

1. Add this to `%USERPROFILE%\.wslconfig`:
   ```ini
   [wsl2]
   kernelCommandLine = cgroup_no_v1=all
   ```
2. Run `wsl --shutdown` in PowerShell, then reopen WSL.
3. Check: `stat -fc %T /sys/fs/cgroup` must print `cgroup2fs`.

Keep the container rootfs on the Linux filesystem (e.g. `/var/lib/crun/rootfs`), not under
`/mnt/c`. The source tree itself can stay on `/mnt/c`.

## Build and run

```sh
make                                              # builds ./crun
make test                                         # unit tests (no root needed)
sudo make rootfs ROOTFS=/var/lib/crun/rootfs      # downloads the Alpine minirootfs
sudo make itest  ROOTFS=/var/lib/crun/rootfs      # integration tests T1–T13 (installs memhog + phasehog)
sudo make eval   ROOTFS=/var/lib/crun/rootfs      # evaluation table + build/results/*.html
```

```sh
sudo ./crun run --memory 64M --stats 500 --rootfs /var/lib/crun/rootfs -- /bin/sh
sudo ./crun run --elastic --memory 128M --record /tmp/run --rootfs /var/lib/crun/rootfs -- phasehog
```

```
usage: crun run [options] --rootfs DIR [--] CMD [ARGS...]
  --memory SIZE    hard memory limit (memory.max), e.g. 64M, 1G, max (default: max)
  --swap SIZE      swap limit (memory.swap.max) (default: 0)
  --pids N         max number of processes (pids.max) (default: max)
  --hostname NAME  container hostname (default: container)
  --stats MS       print live memory usage every MS milliseconds (default: off)
  --rootfs DIR     root filesystem for the container (required)
elastic memory engine (all need a finite --memory):
  --elastic        adapt memory.high to memory pressure between --memory-min and --memory
  --memory-min SIZE  elastic floor (default: max(16M, memory/8))
  --soft-oom POLICY  freeze at a tripwire below --memory instead of a kernel OOM kill,
                   then: kill | grant (raise limit from --reserve) | notify (SIGUSR1 the app)
  --reserve SIZE   extra memory soft-oom may grant (default: memory/4)
  --record PATH    write a flight recording to PATH.csv and PATH.html
```

Exit codes: the container's own exit code, `128 + signal` if it was killed, `125` for a
runtime error, `2` for bad arguments.

## Baseline features

- Isolated PID, mount, UTS (hostname) and IPC namespaces (`CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC`)
- Root filesystem switch with `pivot_root(".", ".")`. The host tree is fully detached. Private `/proc`, read-only `/sys`, tmpfs `/tmp`, and a minimal `/dev`
- One cgroup v2 node per container at `/sys/fs/cgroup/container_<pid>`, with:
  - `memory.max`
  - `memory.swap.max` (default `0`)
  - `memory.oom.group=1`
  - optional `pids.max`
- A single `epoll` loop over `signalfd` + `timerfd` + `memory.events` (`EPOLLPRI`). It handles OOM
  detection and telemetry, forwards Ctrl+C / SIGTERM (escalating to SIGKILL after 2 s), and reaps the
  container with `waitpid()`
- Guaranteed cleanup: `cgroup.kill` + `rmdir`. Container mounts disappear with the mount namespace
- Child waits on a sync pipe until it is inside its cgroup, so it never runs unlimited
- Startup overhead under 1 ms from launch to container release

## Project layout

| Area | File | Responsibility |
|---|---|---|
| CLI & command parser | `src/cli.*` | Options and sizes (`64M`, `1G`, `max`), validation |
| Process sandbox | `src/sandbox.*` | `clone()` into new namespaces, sync pipe, clean env, `execvp` |
| Filesystem isolation | `src/fs.*` | Private mounts, `/proc`, `/sys`, `/tmp`, `/dev`, `pivot_root` |
| Cgroup engine + cleanup | `src/cgroup.*` | Node lifecycle (RAII), limits, attach PID, `cgroup.kill` + `rmdir` |
| Event loop | `src/monitor.*` | epoll loop: OOM events, telemetry, signals, reaper; drives the engine |
| Pressure probe | `src/pressure.*` | PSI or fallback estimator from refaults, overrun and reclaim events |
| Elastic controller | `src/elastic.*` | Grow/shrink `memory.high`, limit recommendation (pure logic) |
| Soft landing | `src/softland.*` | Tripwire, freeze, diagnostics, kill/grant/notify policies |
| Flight recorder | `src/recorder.*` | CSV stream, summary statistics, HTML/SVG report |

Shared helpers live in `src/util.hpp`. Tests are in `tests/`:

- `unit_tests.cpp`
- `integration.cpp` (T1–T13)
- `eval.cpp` (evaluation harness)
- `proc.hpp` (shared process helpers)
- workloads: `memhog.cpp` and `phasehog.cpp`

## Tests

| ID | Check |
|---|---|
| T1 | `/sys/fs/cgroup/container_<pid>` is created with `memory.max = 67108864` for `--memory 64M` |
| T2 | `/bin/sh` is PID 1; host processes are invisible |
| T3 | 30 MB allocation in a 64 MB container succeeds; telemetry reports ~30 MB |
| T4 | 100 MB allocation in a 64 MB container is OOM-killed and the breach is logged |
| T5 | With `memory.swap.max = 0` the job is killed at once; with swap allowed it survives |
| T6 | Host `/root` and the old root are not reachable from inside |
| T7 | SIGINT stops the container and removes its cgroup |
| T8 | 5 concurrent containers (32M/64M/128M) each enforce their own limit |
| T9 | Elastic: `memory.high` drops below `memory.max` at runtime; average usage is below a static run |
| T10 | Soft-oom kill: frozen and diagnosed (culprit named), no kernel OOM kill |
| T11 | Soft-oom grant: limit raised from reserve, job completes |
| T12 | Soft-oom notify: only the SIGUSR1-handling process is signalled, the app frees memory, no extra memory used |
| T13 | Flight recorder writes a valid CSV and an HTML report with charts |

## Known limits

- **Only the fallback estimator was tested end to end.** WSL2's kernel has no PSI. The PSI path
  has unit tests for parsing only, and should be run on a stock Ubuntu/Fedora kernel before the demo.
- **The thresholds are heuristics.** They are tuned on one workload, and the elastic controller
  trades runtime for memory (about 17 % in the eval). Tuning them is a good discussion point for the report.
- **Soft landing can still lose the race.** It relies on `memory.high` throttling to beat a very
  fast allocator to `memory.max`. If it loses, crun logs "soft landing missed". It never lost in testing.
- **A kernel quirk can distort timing.** A container that rewrote a file whose page cache belonged
  to an earlier, OOM-killed container wrote 10–20× slower, even with a static limit. The cause
  wasn't confirmed (possibly cgroup-writeback inode ownership). `phasehog` uses a fresh, unlinked
  file per run to keep the eval fair.

## Team

| Name | Enrollment No. | Batch | Area |
|---|---|---|---|
| Ridhi Kapoor | 992401030006 | F1 | CLI, flight recorder and HTML report |
| Anwesha Mohapatra | 992401030008 | F1 | Sandbox, filesystem isolation, soft landing, `phasehog` |
| Lakshya Saxena | 992401030010 | F1 | Cgroup engine, pressure probe, elastic controller |
| Naman L. Sewani | 992401030028 | F1 | Event loop, integration tests, evaluation harness |

Supervisor: Dr. Ambalika Sarkar

## References

- M. Kerrisk, *The Linux Programming Interface*, No Starch Press, 2010.
- Linux man pages: `clone(2)`, `namespaces(7)`, `pivot_root(2)`, `cgroups(7)`, `epoll(7)`, `signalfd(2)`, `timerfd_create(2)`.
- T. Heo, "Control Group v2", Linux kernel documentation: https://docs.kernel.org/admin-guide/cgroup-v2.html
- J. Weiner, "PSI — Pressure Stall Information", Linux kernel documentation: https://docs.kernel.org/accounting/psi.html
- Meta, *senpai* (memory.high auto-tuning with PSI) and *oomd*: https://github.com/facebookincubator/senpai
- Kubernetes, "Vertical Pod Autoscaler" and the kubelet working-set metric (`memory.current − inactive_file`).

## License

[MIT](LICENSE.md)
