#!/usr/bin/env python3
"""GPU onset and warm-tail cost per run: which of P's two terms moves between sessions. For GPU-target
and combined bursts at one load, per run: median first-frame GPU load time, median of the last 0.5 s
(the warm cost, as e1_fit.py), median P = onset / tail, and median frames per burst (a burst lasts
2 s, so fewer frames means dropped frames). L0 = probes trials only, as e1_robustness.py, whose rule
for telling the two calm loads apart this repeats.

Written for the POCO 120 Hz subset (D53) against the full 120 Hz grid (D65): one session's combined
bursts did not replicate. Writes the CSV given by --out and prints it.
Usage: gpu_tail_by_run.py <run-dir-or-root>... --device ID --out f.csv [--load 50] [--calms 250,1000,4000]
"""
import argparse
from pathlib import Path

import numpy as np
import pandas as pd

from e1_fit import PROC, WARM_TAIL_S
from agcsv import read_frames, read_run
from validate import find_runs

TARGET = {1: "gpu", 3: "both"}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--device", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--load", type=int, default=50)
    ap.add_argument("--calms", default="250,1000,4000")
    a = ap.parse_args()
    calms = {int(c) for c in a.calms.split(",")}

    rows = []
    for run in find_runs(a.paths):
        if read_run(run).get("device", {}).get("id") != a.device:
            continue
        _, df = read_frames(run)
        df = df[df["sb_trial"].notna() & df["sb_phase"].isin([0, 1])]
        per = {t: [] for t in TARGET}
        for _, g in df.groupby("sb_trial", sort=True):
            calm, burst = g[g["sb_phase"] == 0], g[g["sb_phase"] == 1]
            if burst.empty or int(burst["sb_target"].iloc[0]) not in PROC["gpu"]["targets"]:
                continue
            if int(burst["sb_load_pct"].iloc[0]) != a.load or int(burst["sb_calm_ms"].iloc[0]) not in calms:
                continue
            if len(calm) and calm[["gpu_main_ns", "cpu_sim_ns"]].notna().any(axis=1).mean() > 0.5:
                continue   # ambient L0
            t = (burst["t_frame_start_ns"].to_numpy() - burst["t_frame_start_ns"].iloc[0]) / 1e9
            ld = burst[PROC["gpu"]["load"]].to_numpy(dtype=float)
            tail = np.nanmedian(ld[t >= t[-1] - WARM_TAIL_S])
            per[int(burst["sb_target"].iloc[0])].append((ld[0], tail, len(burst)))
        for tgt, xs in per.items():
            if not xs:
                continue
            on, tail, n = (np.array(c, dtype=float) for c in zip(*xs))
            rows.append({"run": Path(run).name, "target": TARGET[tgt], "load_pct": a.load, "trials": len(xs),
                         "onset_us": round(np.median(on) / 1e3, 1), "tail_us": round(np.median(tail) / 1e3, 1),
                         "P": round(float(np.median(on / tail)), 3), "burst_frames": float(np.median(n))})
    out = pd.DataFrame(rows).sort_values(["target", "run"])
    out.to_csv(a.out, index=False)
    print(out.to_string(index=False))


if __name__ == "__main__":
    main()
