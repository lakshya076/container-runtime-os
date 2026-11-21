#!/usr/bin/env bash
# Pulls python and gcc with Docker (download only), then runs one benchmark per image in crun,
# each with a static limit and with the elastic engine, all four at once. Shows a live
# `crun ps` dashboard and finishes with a static-vs-elastic comparison.
#
# usage: sudo bash scripts/bench.sh
set -euo pipefail
cd "$(dirname "$0")/.."

if [ "$(id -u)" -ne 0 ]; then
    echo "bench.sh needs root (containers and cgroups): sudo bash scripts/bench.sh" >&2
    exit 1
fi

PY_IMAGE=python:3.12-slim
GCC_IMAGE=gcc:14
PY_ROOT=/var/lib/crun/images/python_3.12-slim
GCC_ROOT=/var/lib/crun/images/gcc_14
PY_LIMIT=256M
GCC_LIMIT=512M
OUT=build/bench

make -s crun
[ -d "$PY_ROOT" ] || ./crun pull "$PY_IMAGE"
[ -d "$GCC_ROOT" ] || ./crun pull "$GCC_IMAGE"
mkdir -p "$OUT"
rm -f "$OUT"/*.log "$OUT"/*.csv "$OUT"/*.html

names=(py-static py-elastic gcc-static gcc-elastic)
pids=()

# start NAME INPUT CRUN-RUN-ARGS... ; INPUT is fed to the container on stdin.
# The redirect must sit on the background command itself: a script's background jobs
# otherwise get /dev/null as stdin.
start() {
    local name=$1 input=$2
    shift 2
    ./crun run --name "$name" --record "$OUT/$name" "$@" < "$input" > "$OUT/$name.log" 2>&1 &
    pids+=($!)
}

stop_all() {
    echo
    echo "stopping containers..."
    kill -INT "${pids[@]}" 2>/dev/null || true
    wait || true
    exit 130
}
trap stop_all INT TERM

GCC_CMD='g++ -O2 -x c++ -o /tmp/allocbench - && /tmp/allocbench'
PY_IN=tests/py/footprint.py
GCC_IN=tests/bench/allocbench.cpp
start py-static   "$PY_IN"  --memory "$PY_LIMIT"            --rootfs "$PY_ROOT"  -- python3 -
start py-elastic  "$PY_IN"  --memory "$PY_LIMIT"  --elastic --rootfs "$PY_ROOT"  -- python3 -
start gcc-static  "$GCC_IN" --memory "$GCC_LIMIT"           --rootfs "$GCC_ROOT" -- sh -c "$GCC_CMD"
start gcc-elastic "$GCC_IN" --memory "$GCC_LIMIT" --elastic --rootfs "$GCC_ROOT" -- sh -c "$GCC_CMD"

running() {
    for p in "${pids[@]}"; do kill -0 "$p" 2>/dev/null && return 0; done
    return 1
}

# Latest benchmark phase line of a container's log.
last_phase() {
    grep -E '^(footprint|allocbench):' "$OUT/$1.log" 2>/dev/null | tail -1 || true
}

draw() {
    [ -t 1 ] && printf '\033[H\033[2J'
    echo "crun live benchmark  |  elapsed ${SECONDS}s  |  python ${PY_LIMIT}, gcc ${GCC_LIMIT}  |  Ctrl+C to stop"
    echo
    ./crun ps
    echo
    for n in "${names[@]}"; do
        printf '%-12s %s\n' "$n" "$(last_phase "$n")"
    done
}

SECONDS=0
while running; do
    draw
    sleep 1
done
draw
wait || true

# value LOG SED-EXPR : first match of a sed capture in a log, "-" if absent
value() { sed -n "$2" "$OUT/$1.log" | head -1 | grep . || echo "-"; }

echo
echo "results (memory as charged to each container's cgroup)"
printf '%-12s %5s %9s %10s %10s %11s %12s\n' NAME EXIT TIME_S AVG_MIB PEAK_MIB LIMIT_MIB RECOMMENDED
for n in "${names[@]}"; do
    exit_code=$(value "$n" 's/.*exit code \([0-9]*\),.*/\1/p')
    runtime=$(value "$n" 's/.*runtime \([0-9.]*\) s.*/\1/p')
    avg=$(value "$n" 's/.*memory: avg \([0-9.]*\) MiB.*/\1/p')
    peak=$(value "$n" 's/.*, peak \([0-9.]*\) MiB.*/\1/p')
    limit=$(value "$n" 's/.*avg limit \([0-9.]*\) MiB.*/\1/p')
    rec=$(value "$n" 's/.*recommended --memory \([0-9]*M\).*/\1/p')
    printf '%-12s %5s %9s %10s %10s %11s %12s\n' "$n" "$exit_code" "$runtime" "$avg" "$peak" "$limit" "$rec"
done

echo
for img in py gcc; do
    s_avg=$(value "$img-static" 's/.*memory: avg \([0-9.]*\) MiB.*/\1/p')
    e_avg=$(value "$img-elastic" 's/.*memory: avg \([0-9.]*\) MiB.*/\1/p')
    s_peak=$(value "$img-static" 's/.*, peak \([0-9.]*\) MiB.*/\1/p')
    e_peak=$(value "$img-elastic" 's/.*, peak \([0-9.]*\) MiB.*/\1/p')
    s_time=$(value "$img-static" 's/.*runtime \([0-9.]*\) s.*/\1/p')
    e_time=$(value "$img-elastic" 's/.*runtime \([0-9.]*\) s.*/\1/p')
    awk -v n="$img" -v sa="$s_avg" -v ea="$e_avg" -v sp="$s_peak" -v ep="$e_peak" -v st="$s_time" -v et="$e_time" 'BEGIN {
        if (sa == "-" || ea == "-" || sa == 0 || sp == 0 || st == 0) { printf "%-4s elastic vs static: not enough data\n", n; exit }
        printf "%-4s elastic vs static: avg memory %+.0f%%, peak memory %+.0f%%, runtime %+.0f%%\n",
               n, (ea - sa) / sa * 100, (ep - sp) / sp * 100, (et - st) / st * 100 }'
done
echo
echo "logs and charts: $OUT/<name>.log, $OUT/<name>.html"
