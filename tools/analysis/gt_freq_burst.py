#!/usr/bin/env python3
"""Probe against traced clock in an E0 ground-truth run (Perfetto freq.csv, perfetto_freq.py; S7).

For each frame, the GPU clock in force when the GPU probe started (gpu_probe_start_ns) and the CPU
clock of the core the frame's thread ran on (cpu_id) at the frame start, both from freq.csv, read as
step functions on the lab clock. Writes, into <out>:
  <device>_gt_probe_by_freq.csv  probe time and probe x clock (cycles, should be ~constant if the
                                 probe tracks the clock) per GPU clock step and per CPU core/clock;
  <device>_gt_burst.csv          per (processor, calm): P as e1_fit.py computes it, and the traced
                                 clock at the onset frame and in the warm tail (last 0.5 s).
Usage: gt_freq_burst.py <run dir> --device ID --out DIR [--load 50]
"""
import argparse
from pathlib import Path

import numpy as np
import pandas as pd

from e1_fit import PROC, WARM_TAIL_S
from agcsv import read_frames


def step_lookup(track, t):
    """Value of a step-function counter (t_ns, khz sorted by t_ns) at times t; NaN before its first sample."""
    i = np.searchsorted(track["t_ns"].to_numpy(), t, side="right") - 1
    v = track["khz"].to_numpy(dtype=float)[np.clip(i, 0, None)]
    return np.where(i >= 0, v, np.nan)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("run")
    ap.add_argument("--device", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--load", type=int, default=50)
    a = ap.parse_args()

    _, df = read_frames(a.run)
    f = pd.read_csv(Path(a.run) / "freq.csv").sort_values("t_ns")
    gpu = f[f["track"] == "gpufreq"]
    # The Tab S7+ driver gives no host-GPU time correlation (gpu_probe_start_ns empty): fall back to the
    # frame start, which precedes the GPU probe by about one frame.
    t_gpu = df["gpu_probe_start_ns"].fillna(df["t_frame_start_ns"]).to_numpy()
    df["gpu_mhz"] = step_lookup(gpu, t_gpu) / 1000
    df["cpu_mhz"] = np.nan
    for cpu, tr in f[f["track"] == "cpufreq"].groupby("cpu"):
        m = df["cpu_id"] == cpu
        df.loc[m, "cpu_mhz"] = step_lookup(tr, df.loc[m, "t_frame_start_ns"].to_numpy()) / 1000

    by = []
    for p, mhz, extra in (("gpu", "gpu_mhz", []), ("cpu", "cpu_mhz", ["cpu_id"])):
        col = PROC[p]["probe"]
        d = df.dropna(subset=[mhz, col])
        g = d.groupby(extra + [mhz])[col]
        t = pd.DataFrame({"n": g.size(), "probe_us_median": g.median() / 1000,
                          "probe_kcycles_median": (d[col] * d[mhz] / 1e6).groupby([d[c] for c in extra + [mhz]]).median()})
        t = t.reset_index().rename(columns={mhz: "mhz"})
        t.insert(0, "processor", p)
        by.append(t)

    rows = []
    for p, mhz in (("gpu", "gpu_mhz"), ("cpu", "cpu_mhz")):
        load_col = PROC[p]["load"]
        b = df[(df["sb_phase"] == 1) & (df["sb_target"].isin(PROC[p]["targets"])) & (df["sb_load_pct"] == a.load)]
        for _, g in b.groupby("sb_trial", sort=True):
            t = (g["t_frame_start_ns"].to_numpy() - g["t_frame_start_ns"].iloc[0]) / 1e9
            tail = g[t >= t[-1] - WARM_TAIL_S]
            rows.append({"processor": p, "calm_ms": int(g["sb_calm_ms"].iloc[0]),
                         "P": g[load_col].iloc[0] / tail[load_col].median(),
                         "onset_mhz": g[mhz].iloc[0], "tail_mhz": tail[mhz].median(),
                         "max_mhz_in_burst": g[mhz].max()})
    burst = pd.DataFrame(rows).groupby(["processor", "calm_ms"]).agg(
        n=("P", "size"), P_median=("P", "median"), onset_mhz_median=("onset_mhz", "median"),
        tail_mhz_median=("tail_mhz", "median"), max_mhz_median=("max_mhz_in_burst", "median")).round(3).reset_index()

    Path(a.out).mkdir(parents=True, exist_ok=True)
    pd.concat(by).round(3).to_csv(Path(a.out) / f"{a.device}_gt_probe_by_freq.csv", index=False)
    burst.to_csv(Path(a.out) / f"{a.device}_gt_burst.csv", index=False)
    print(pd.concat(by).round(2).to_string(index=False))
    print(burst.to_string(index=False))


if __name__ == "__main__":
    main()
