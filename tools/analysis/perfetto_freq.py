#!/usr/bin/env python3
"""GPU and CPU clock counters from a Perfetto trace (tools/device/perfetto_wrap.sh), on the lab clock.

Writes <run>/freq.csv with columns track, cpu, t_ns, khz. t_ns is CLOCK_MONOTONIC (trace_processor's
to_monotonic), the lab clock on Android (lab/include/ag/ag_app.h), so it lines up with frames.csv.
Prints a per-track summary. Exit 1 if the trace has no GPU clock counter.
Usage: perfetto_freq.py <run dir>   (TRACE_PROCESSOR=<path> overrides build/perfetto/trace_processor)
Query pattern from NeonSwarm/tools/gpufreq_by_job.py (commit 0991e5f).
"""
import csv, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TP = os.environ.get("TRACE_PROCESSOR", os.path.join(ROOT, "build", "perfetto", "trace_processor"))
QUERY = ("select t.name, coalesce(extract_arg(t.dimension_arg_set_id, 'cpu'), -1), to_monotonic(c.ts), c.value "
         "from counter c join counter_track t on c.track_id = t.id "
         "where t.name in ('gpufreq', 'cpufreq') order by t.name, 2, c.ts")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    run = sys.argv[1]
    trace = os.path.join(run, "perfetto.pftrace")
    with tempfile.NamedTemporaryFile("w", suffix=".sql", delete=False) as q:
        q.write(QUERY)
    try:
        out = subprocess.run([TP, "-q", q.name, trace], capture_output=True, text=True, check=True).stdout
    finally:
        os.unlink(q.name)
    rows = []
    for line in out.splitlines():
        parts = [p.strip().strip('"') for p in line.split(",")]
        if len(parts) != 4 or not parts[2].lstrip("-").isdigit():
            continue
        rows.append((parts[0], int(float(parts[1])), int(parts[2]), int(float(parts[3]))))
    with open(os.path.join(run, "freq.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["track", "cpu", "t_ns", "khz"])
        w.writerows(rows)
    tracks = {}
    for name, cpu, t, khz in rows:
        tracks.setdefault((name, cpu), []).append(khz)
    for (name, cpu), v in sorted(tracks.items()):
        print(f"{name} cpu={cpu}: {len(v)} samples, {min(v)}..{max(v)} kHz, distinct {len(set(v))}")
    if not any(name == "gpufreq" for name, _ in tracks):
        print("no gpufreq counter in the trace (power/gpu_frequency not emitted on this device?)")
        sys.exit(1)


if __name__ == "__main__":
    main()
