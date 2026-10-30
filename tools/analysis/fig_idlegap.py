#!/usr/bin/env python3
"""Figure: idle-gap ladder (paper §4.5): a single tiny command buffer after an idle gap of 0.1 ms to 2 s.

Three panels: submit-to-GPU-start, GPU execution time and round trip (submit to completion), each versus the
gap, median per gap (19-20 measurements, the first dropped as pipeline start) with the interquartile range as
a thin bar. One line per device, with a fixed hue and marker per device (figstyle). The Tab S7+ reports no GPU
start timestamp (§3.2), so it has no submit-to-start line.

Usage: fig_idlegap.py <idlegap run dir>... [--out paper/figures/fig_idlegap.png]
                      [--csv results/paper_tables/idlegap_medians.csv]
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
from agcsv import read_csv  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
METRICS = [("submit_to_start_ns", "submit → GPU start"), ("gpu_ns", "GPU execution"), ("round_trip_ns", "round trip")]


def summarise(run_dir):
    run_dir = Path(run_dir)
    dev = json.loads((run_dir / "run.json").read_text())["device"]["id"]
    _, df = read_csv(run_dir / "idlegap.csv")
    df = df[df["idx"] > 0]
    out = {}
    for gap, sel in df.groupby("gap_us"):
        out[int(gap)] = {k: (len(sel), *[float(v) / 1000 for v in np.percentile(sel[k], [50, 25, 75])])
                         for k, _ in METRICS}
    return dev, out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--out", default=str(ROOT / "paper" / "figures" / "fig_idlegap.png"))
    ap.add_argument("--csv", default=str(ROOT / "results" / "paper_tables" / "idlegap_medians.csv"))
    a = ap.parse_args(argv)
    fs.setup()

    res = [summarise(r) for r in a.runs]
    fig, axes = plt.subplots(1, 3, figsize=(6.5, 2.5), dpi=300, sharex=True)
    rows = []
    for ax, (key, title) in zip(axes, METRICS):
        for dev, o in res:
            gaps = np.array(sorted(o)) / 1000.0   # ms
            med = np.array([o[g][key][1] for g in sorted(o)])
            q1 = np.array([o[g][key][2] for g in sorted(o)])
            q3 = np.array([o[g][key][3] for g in sorted(o)])
            if not np.isfinite(med).any():
                continue
            c, m = fs.DEVICE_COLOR.get(dev, fs.MUTED), fs.DEVICE_MARKER.get(dev, "o")
            ax.errorbar(gaps, med, yerr=[med - q1, q3 - med], fmt="none", ecolor=c, elinewidth=0.6, capsize=0,
                        alpha=0.7, zorder=3)
            ax.plot(gaps, med, "-", color=c, lw=1.3, zorder=4, label=fs.NAMES.get(dev, dev), solid_capstyle="round")
            ax.scatter(gaps, med, s=12, marker=m, color=c, edgecolors="white", linewidths=0.6, zorder=5)
            if key == METRICS[0][0]:
                for g in sorted(o):
                    r = [dev, g]
                    for k, _ in METRICS:
                        n, md, lo, hi = o[g][k]
                        r += [n, f"{md:.1f}", f"{lo:.1f}", f"{hi:.1f}"]
                    rows.append(r)
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xticks([0.1, 1, 10, 100, 1000])
        ax.set_xticklabels(["0.1", "1", "10", "100", "1000"])
        ax.minorticks_off()
        fs.style_axis(ax, title)
    axes[1].set_xlabel("idle gap before submit (ms)", fontsize=7, color=fs.INK)
    axes[0].set_ylabel("time (µs, median and IQR)", fontsize=7, color=fs.INK)
    for ax in axes:
        ax.set_yticks([100, 300, 1000, 3000, 10000])
        ax.set_yticklabels(["100", "300", "1000", "3000", "10000"])
        ax.set_ylim(50, 20000)
    seen = {}
    for ax in axes:
        for h, l in zip(*ax.get_legend_handles_labels()):
            seen.setdefault(l, h)
    fig.legend(list(seen.values()), list(seen.keys()), loc="upper center", ncol=3, fontsize=6, frameon=False,
               bbox_to_anchor=(0.5, 1.0), handlelength=1.6, columnspacing=1.2)
    fig.tight_layout(rect=(0, 0, 1, 0.9))
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(a.out, facecolor="white")
    fig.savefig(Path(a.out).with_suffix(".pdf"), facecolor="white")
    print(f"wrote {a.out} (+ .pdf)")

    Path(a.csv).parent.mkdir(parents=True, exist_ok=True)
    with open(a.csv, "w", newline="") as f:
        w = csv.writer(f)
        head = ["device", "gap_us"]
        for k, _ in METRICS:
            k = k.replace("_ns", "")
            head += [f"n", f"{k}_med_us", f"{k}_q1_us", f"{k}_q3_us"]
        w.writerow(head)
        w.writerows(rows)
    print(f"wrote {a.csv}")


if __name__ == "__main__":
    main()
