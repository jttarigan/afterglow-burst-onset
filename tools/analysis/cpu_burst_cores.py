#!/usr/bin/env python3
"""Where the CPU onset penalty's two frames ran: core and clock of the first burst frame against the
warm tail (last 0.5 s), per calm, for CPU-target bursts at one load (Android: cpu_id and cpu_freq_khz
are logged per frame, D45). Explains a P < 1 that comes from the tail running on a slower core or clock
than the onset frame, which P = load[0] / median(tail) (e1_fit.py) cannot see.

Writes <out>/<device>_cpu_burst_cores.csv (one row per calm) and prints it.
Usage: cpu_burst_cores.py <run-dir-or-root>... --device ID --out DIR [--big-from N] [--load 50]
  --big-from N: cpus >= N are the big cluster (S6 Lite: 4, cpus 4-7 are the A73s).
"""
import argparse
import sys
from pathlib import Path

import numpy as np
import pandas as pd

from e1_fit import PROC, WARM_TAIL_S
from agcsv import read_frames, read_run
from validate import find_runs


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--device", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--big-from", type=int, default=4)
    ap.add_argument("--load", type=int, default=50)
    a = ap.parse_args()

    rows = []
    for run in find_runs(a.paths):
        if read_run(run).get("device", {}).get("id") != a.device:
            continue
        _, df = read_frames(run)
        b = df[(df["sb_phase"] == 1) & df["sb_target"].isin(PROC["cpu"]["targets"])
               & (df["sb_load_pct"] == a.load)]
        for _, g in b.groupby("sb_trial", sort=True):
            t = (g["t_frame_start_ns"].to_numpy() - g["t_frame_start_ns"].iloc[0]) / 1e9
            tail = g[t >= t[-1] - WARM_TAIL_S]
            load = g["cpu_sim_ns"].to_numpy(dtype=float)
            rows.append({
                "calm_ms": int(g["sb_calm_ms"].iloc[0]),
                "P": load[0] / np.nanmedian(tail["cpu_sim_ns"]),
                "onset_big": int(g["cpu_id"].iloc[0]) >= a.big_from,
                "onset_mhz": g["cpu_freq_khz"].iloc[0] / 1000,
                "tail_big_share": float((tail["cpu_id"] >= a.big_from).mean()),
                "tail_mhz": float(tail["cpu_freq_khz"].median()) / 1000,
                # Cost per MHz: if the probe tracks frequency (s6lite_cpu_probe_by_freq.csv), load x MHz is
                # the clock-free cost, so its ratio isolates what the clock does not explain.
                "P_clockfree": (load[0] * g["cpu_freq_khz"].iloc[0])
                               / np.nanmedian(tail["cpu_sim_ns"] * tail["cpu_freq_khz"]),
            })
    if not rows:
        sys.exit(f"no CPU-target bursts at load {a.load} for device {a.device}")
    d = pd.DataFrame(rows)
    out = d.groupby("calm_ms").agg(
        n=("P", "size"), P_median=("P", "median"), onset_big_share=("onset_big", "mean"),
        onset_mhz_median=("onset_mhz", "median"), tail_big_share=("tail_big_share", "mean"),
        tail_mhz_median=("tail_mhz", "median"), P_clockfree_median=("P_clockfree", "median"),
    ).round(3).reset_index()
    Path(a.out).mkdir(parents=True, exist_ok=True)
    f = Path(a.out) / f"{a.device}_cpu_burst_cores.csv"
    out.to_csv(f, index=False)
    print(out.to_string(index=False))
    print(f"-> {f}")


if __name__ == "__main__":
    main()
