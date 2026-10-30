#!/usr/bin/env python3
"""E1 robustness subsets (04 E1): unplugged, probe-size ablation and 120 Hz runs against the matching
cells of the device's charged 60 Hz campaign (L0 = probes only, like the subsets).

Per trial and processor, P = first burst frame load cost / median load cost of the last 0.5 s of the
burst (as e1_fit.py) and kappa0 = kappa of the first burst frame, kappa referenced to each session's
own 1st-percentile probe time. The calm load L0 is not logged per frame; a trial counts as "ambient"
when most of its calm frames carry load-kernel time, as only the ambient level runs a load kernel in
the calm. Reports medians per cell and a bootstrap 95 % CI of the difference of medians (subset minus
reference).

Usage: e1_robustness.py --ref <charged run dirs...> --sub <subset run dirs...> [--label NAME] [--out f.md]
"""
import argparse
from collections import defaultdict
from pathlib import Path

import warnings

import numpy as np

warnings.filterwarnings("ignore", "All-NaN slice", RuntimeWarning)

from agcsv import read_frames, read_run

PROC = {"gpu": ("gpu_probe_ns", "gpu_main_ns", {1, 3}), "cpu": ("cpu_probe_ns", "cpu_sim_ns", {2, 3})}
TARGET = {1: "gpu", 2: "cpu", 3: "both"}
WARM_TAIL_S = 0.5


def trials(run_dirs):
    out, probes = [], defaultdict(list)
    for run in run_dirs:
        dev = read_run(run)["device"]["id"]
        _, df = read_frames(run)
        df = df[df["sb_trial"].notna() & df["sb_phase"].isin([0, 1])]
        rows = []
        for _, g in df.groupby("sb_trial", sort=True):
            calm, burst = g[g["sb_phase"] == 0], g[g["sb_phase"] == 1]
            if burst.empty:
                continue
            loaded = calm[["gpu_main_ns", "cpu_sim_ns"]].notna().any(axis=1).mean() if len(calm) else 0.0
            t = (burst["t_frame_start_ns"].to_numpy() - burst["t_frame_start_ns"].iloc[0]) / 1e9
            tr = {"dev": dev, "calm_ms": int(burst["sb_calm_ms"].iloc[0]), "load_pct": int(burst["sb_load_pct"].iloc[0]),
                  "target": int(burst["sb_target"].iloc[0]), "l0": "ambient" if loaded > 0.5 else "probes"}
            for p, (probe, load, _) in PROC.items():
                ld = burst[load].to_numpy(dtype=float)
                tail = np.nanmedian(ld[t >= t[-1] - WARM_TAIL_S])
                tr[f"P_{p}"] = ld[0] / tail if np.isfinite(ld[0]) and np.isfinite(tail) else np.nan
                tr[f"probe0_{p}"] = float(burst[probe].iloc[0])
                probes[(run, p)].extend(np.concatenate([calm[probe].to_numpy(float), burst[probe].to_numpy(float)]))
            tr["run"] = run
            rows.append(tr)
        out += rows
    for tr in out:
        for p in PROC:
            tr[f"k0_{p}"] = np.nanpercentile(probes[(tr["run"], p)], 1.0) / tr[f"probe0_{p}"]
    return out


def boot_diff(a, b, n=2000, rng=np.random.default_rng(1)):
    a, b = np.asarray(a)[np.isfinite(a)], np.asarray(b)[np.isfinite(b)]
    if len(a) == 0 or len(b) == 0:
        return np.nan, [np.nan, np.nan]
    d = [np.median(rng.choice(a, len(a))) - np.median(rng.choice(b, len(b))) for _ in range(n)]
    return float(np.median(a) - np.median(b)), [float(np.percentile(d, 2.5)), float(np.percentile(d, 97.5))]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ref", nargs="+", required=True)
    ap.add_argument("--sub", nargs="+", required=True)
    ap.add_argument("--label", default="subset")
    ap.add_argument("--out")
    a = ap.parse_args()
    ref, sub = trials(a.ref), trials(a.sub)
    l0 = defaultdict(int)
    for t in ref:
        l0[(t["calm_ms"], t["load_pct"], t["target"], t["l0"])] += 1
    lines = [f"## {a.label}: {sub[0]['dev'] if sub else '?'}", "",
             f"Reference L0 split per cell (probes/ambient): {sorted(set((v) for k, v in l0.items()))}", "",
             "| Proc | Target | L1 % | Calm ms | n sub / ref | P sub | P ref | ΔP [95 % CI] | κ₀ sub | κ₀ ref | Δκ₀ [95 % CI] |",
             "|---|---|---|---|---|---|---|---|---|---|---|"]
    keys = sorted({(t["calm_ms"], t["load_pct"], t["target"]) for t in sub})
    for p, (_, _, targets) in PROC.items():
        for calm, load, tgt in keys:
            if tgt not in targets:
                continue
            s = [t for t in sub if (t["calm_ms"], t["load_pct"], t["target"]) == (calm, load, tgt) and t["l0"] == "probes"]
            r = [t for t in ref if (t["calm_ms"], t["load_pct"], t["target"]) == (calm, load, tgt) and t["l0"] == "probes"]
            dP, ciP = boot_diff([t[f"P_{p}"] for t in s], [t[f"P_{p}"] for t in r])
            dk, cik = boot_diff([t[f"k0_{p}"] for t in s], [t[f"k0_{p}"] for t in r])
            med = lambda xs: np.nanmedian(xs) if len(xs) else np.nan
            lines.append(f"| {p} | {TARGET[tgt]} | {load} | {calm} | {len(s)} / {len(r)} | {med([t[f'P_{p}'] for t in s]):.2f} "
                         f"| {med([t[f'P_{p}'] for t in r]):.2f} | {dP:+.2f} [{ciP[0]:+.2f}, {ciP[1]:+.2f}] "
                         f"| {med([t[f'k0_{p}'] for t in s]):.2f} | {med([t[f'k0_{p}'] for t in r]):.2f} "
                         f"| {dk:+.2f} [{cik[0]:+.2f}, {cik[1]:+.2f}] |")
    text = "\n".join(lines) + "\n"
    if a.out:
        with open(a.out, "a") as f:
            f.write(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
