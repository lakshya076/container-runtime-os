"""Memory footprint workload for a real Python image under crun.

Usage (inside the container):  python3 - [RECORDS] < tests/py/footprint.py
Phases mirror a typical data job:
  1. load RECORDS dict records                        (Python heap, anon memory)
  2. write + read a 100 MiB temp file, keep it open    (page cache, reclaimable)
  3. build a copied name index                         (temporary spike)
  4. drop the index, keep 25% of the records
"""
import gc
import os
import sys
import tempfile
import time


def footprint():
    with open("/proc/self/status") as f:
        fields = dict(line.split(":", 1) for line in f if line.startswith(("VmRSS", "VmHWM")))
    return f"rss {fields['VmRSS'].strip():>10}, peak {fields['VmHWM'].strip():>10}"


def phase(name, hold_s=0.0):
    print(f"footprint: {name:<30} {footprint()}", flush=True)
    time.sleep(hold_s)


records_n = int(sys.argv[1]) if len(sys.argv) > 1 else 200_000
phase("start")

records = [{"id": i, "name": f"user{i}", "score": i % 97} for i in range(records_n)]
phase(f"loaded {records_n} records", 2)

# Unlinked temp file held open: its page cache stays charged to the container until closed.
with tempfile.TemporaryFile(dir="/var/tmp") as f:
    chunk = os.urandom(1 << 20)
    for _ in range(100):
        f.write(chunk)
    f.flush()
    os.fsync(f.fileno())
    f.seek(0)
    total = sum(len(block) for block in iter(lambda: f.read(1 << 20), b""))
    phase(f"processed {total >> 20} MiB file", 3)

    index = {r["name"]: dict(r) for r in records}
    phase("built name index", 1)
    del index
    gc.collect()
    phase("dropped index")

del records[records_n // 4:]
gc.collect()
phase("kept 25% of records", 2)
print("footprint: done", flush=True)
