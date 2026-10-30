#!/usr/bin/env python3
"""GPU clock steps seen by the GPU probe alone, for devices whose Perfetto gpu_frequency is silent or sparse.

The GPU probe is a fixed kernel, so its time scales with 1/clock (checked against Perfetto on the POCO; S7).
A frame whose probe time falls below FAST_FRAC x the run's median ran at a clearly higher clock than the
run's usual one (on the Tab S7+, 305 -> ~400 MHz cuts the probe from ~78 to ~59 us; run-to-run noise is
under 6 %). Writes, into <out>:
  <device>_gpu_probe_steps.csv     per run: validity, frames, probe percentiles, fast frames, implied clock
                                   ratio of the fast frames (median / fast median);
  <device>_gpu_probe_clusters.csv  per run of consecutive fast frames (gap <= 3): first/last frame, length,
                                   seconds from the run's first frame, stepbench phase/target/calm/load.
Usage: gpu_probe_steps.py <run dir>... --device ID --out DIR
"""
import argparse
from pathlib import Path

import numpy as np
import pandas as pd

from agcsv import read_frames
from validate import check_run

FAST_FRAC = 0.87
GAP = 3


def run_steps(run_dir):
    _, df = read_frames(run_dir)
    d = df[df["gpu_probe_ns"].notna()].copy()
    d["us"] = d["gpu_probe_ns"] / 1000
    med = d["us"].median()
    fast = d[d["us"] < FAST_FRAC * med]
    p = np.percentile(d["us"], [0.1, 50, 99.9]) if len(d) else [np.nan] * 3
    reasons = check_run(run_dir)
    row = {"run_id": Path(run_dir).name, "valid": not reasons, "invalid_reasons": "; ".join(reasons),
           "frames": len(d), "probe_us_p0.1": p[0], "probe_us_median": p[1], "probe_us_p99.9": p[2],
           "fast_frames": len(fast),
           "fast_us_median": fast["us"].median() if len(fast) else np.nan,
           "clock_ratio_fast": med / fast["us"].median() if len(fast) else np.nan}
    clusters = []
    if len(fast):
        t0 = df["t_frame_start_ns"].iloc[0]
        idx = fast.index.to_numpy()
        for b in np.split(idx, np.where(np.diff(fast["frame"].to_numpy()) > GAP)[0] + 1):
            g = fast.loc[b]
            first = g.iloc[0]
            clusters.append({"run_id": row["run_id"], "first_frame": int(first["frame"]),
                             "last_frame": int(g["frame"].iloc[-1]), "fast_frames": len(g),
                             "t_s": round((first["t_frame_start_ns"] - t0) / 1e9, 2),
                             "fast_us_median": g["us"].median(),
                             "sb_phase": first["sb_phase"], "sb_target": first["sb_target"],
                             "sb_calm_ms": first["sb_calm_ms"], "sb_load_pct": first["sb_load_pct"]})
    return row, clusters


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--device", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    rows, clusters = [], []
    for r in sorted(a.runs):
        row, cl = run_steps(r)
        rows.append(row)
        clusters += cl
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    pd.DataFrame(rows).round(3).to_csv(out / f"{a.device}_gpu_probe_steps.csv", index=False)
    pd.DataFrame(clusters, columns=["run_id", "first_frame", "last_frame", "fast_frames", "t_s", "fast_us_median",
                                    "sb_phase", "sb_target", "sb_calm_ms", "sb_load_pct"]).round(3).to_csv(
        out / f"{a.device}_gpu_probe_clusters.csv", index=False)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
