#!/usr/bin/env python3
"""Figure: onset penalty P versus calm length and burst size, per device and processor (paper §4.4, Table 4).

P = load cost of the first burst frame / median load cost of the burst's last 0.5 s (e1_fit.py). One panel per
device and processor, own-target bursts only (GPU bursts for the GPU, CPU bursts for the CPU), one series per
burst size L1 (25, 50, 80 % of the frame budget), the median over 30 trials per cell with its bootstrap 95 % CI
as a thin bar. P = 1 is "no penalty".

Usage: fig_onset_penalty.py <e1_fit_<dev>.json>... [--out paper/figures/fig_onset_penalty.png]
                            [--csv results/paper_tables/onset_penalty_cells.csv]
"""
import argparse
import csv
import json
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import figstyle as fs  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
OWN = {"gpu": 1, "cpu": 2}
LOADS = [25, 50, 80]


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("fits", nargs="+")
    ap.add_argument("--out", default=str(ROOT / "paper" / "figures" / "fig_onset_penalty.png"))
    ap.add_argument("--csv", default=str(ROOT / "results" / "paper_tables" / "onset_penalty_cells.csv"))
    a = ap.parse_args(argv)
    fs.setup()

    panels = []
    for f in a.fits:
        fit = json.load(open(f))
        dev = fit["devices"][0]
        for p in ("gpu", "cpu"):
            panels.append((dev, p, fit["processors"][p]["cells"]))
    devices = list(dict.fromkeys(d for d, _, _ in panels))

    fig, axes = plt.subplots(len(devices), 2, figsize=(6.5, 1.45 * len(devices) + 0.7), sharex=True, sharey=True,
                             dpi=300)
    axes = np.atleast_2d(axes)
    rows, ymax = [], 1.0
    for dev, p, cells in panels:
        ax = axes[devices.index(dev), 0 if p == "gpu" else 1]
        ax.axhline(1.0, color=fs.SPINE, lw=0.8, zorder=1)
        for L in LOADS:
            cs = sorted((c for c in cells if c["target"] == OWN[p] and c["load_pct"] == L and c["P_median"] is not None),
                        key=lambda c: c["calm_ms"])
            if not cs:
                continue
            T = np.array([c["calm_ms"] / 1000 for c in cs])
            P = np.array([c["P_median"] for c in cs])
            lo = np.array([c["P_ci95"][0] for c in cs])
            hi = np.array([c["P_ci95"][1] for c in cs])
            ymax = max(ymax, hi.max())
            ax.errorbar(T, P, yerr=[P - lo, hi - P], fmt="none", ecolor=fs.LOAD_COLOR[L], elinewidth=0.6, capsize=0,
                        alpha=0.8, zorder=3)
            ax.plot(T, P, "-", color=fs.LOAD_COLOR[L], lw=1.3, zorder=4, solid_capstyle="round",
                    label=f"L1 = {L} %")
            ax.scatter(T, P, s=12, marker=fs.LOAD_MARKER[L], color=fs.LOAD_COLOR[L], edgecolors="white",
                       linewidths=0.6, zorder=5)
            for c in cs:
                rows.append([dev, p, L, c["calm_ms"], c["n_trials"], f"{c['P_median']:.4f}", f"{c['P_ci95'][0]:.4f}",
                             f"{c['P_ci95'][1]:.4f}"])
            print(f"{dev} {p} L1={L}: P {P.min():.2f}–{P.max():.2f}")
        fs.style_axis(ax, f"{fs.NAMES.get(dev, dev)}, {p.upper()}")
    fs.calm_axis(axes[0, 0])
    axes[0, 0].set_ylim(0.4, min(ymax + 0.15, 4.0))
    for ax in axes[-1]:
        ax.set_xlabel("calm length before the burst (s)", fontsize=7, color=fs.INK)
    for ax in axes[:, 0]:
        ax.set_ylabel("onset penalty P", fontsize=7, color=fs.INK)
    handles, labels = axes[0, 0].get_legend_handles_labels()
    fig.legend(handles, [f"burst size {l}" for l in labels], loc="upper center", ncol=3, fontsize=6.5, frameon=False,
               bbox_to_anchor=(0.5, 1.0))
    fig.tight_layout(rect=(0, 0, 1, 0.975))
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(a.out, facecolor="white")
    fig.savefig(Path(a.out).with_suffix(".pdf"), facecolor="white")
    print(f"wrote {a.out} (+ .pdf)")

    Path(a.csv).parent.mkdir(parents=True, exist_ok=True)
    with open(a.csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["device", "processor", "load_pct", "calm_ms", "n_trials", "P_median", "P_ci_lo", "P_ci_hi"])
        w.writerows(rows)
    print(f"wrote {a.csv}")


if __name__ == "__main__":
    main()
