#!/usr/bin/env python3
"""Figure: onset clock index kappa0 versus calm length, per device and processor (paper §4.2, Table 3).

One panel per device and processor. Dots: the median kappa of the first burst frame over all trials of a calm
length (e1_fit's onset_kappa_by_calm, 180 trials per calm). Small gray dots: the same median per cell (burst
size x target, 30 trials each), so the spread across burst sizes is visible. Dashed line: the fitted decay
k0(T) = k_floor + (k_inf - k_floor) exp(-T / tau_down), drawn only where Table 3 reports a tau_down, i.e. where
kappa0 moves by more than 0.05 across calms (e1_summary.py's rule) AND does not rise with calm length by more
than 0.03 (the Exynos 9611 CPU is non-monotonic: a core-migration effect, §4.2, no single exponential).

Usage: fig_kappa0_calm.py <e1_fit_<dev>.json>... [--out paper/figures/fig_kappa0_calm.png]
                          [--csv results/paper_tables/kappa0_calm.csv]
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
SPAN_MIN, RISE_MAX = 0.05, 0.03
OWN = {"gpu": 1, "cpu": 2}


def decay_reported(k0):
    """Table 3's rule: a decay is reported when kappa0 spans > 0.05 and never rises by > 0.03 with calm length."""
    span = max(k0) - min(k0)
    rises = max(0.0, max(np.diff(k0))) if len(k0) > 1 else 0.0
    if span <= SPAN_MIN:
        return False, "no calm dependence"
    if rises > RISE_MAX:
        return False, "non-monotonic"
    return True, ""


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("fits", nargs="+")
    ap.add_argument("--out", default=str(ROOT / "paper" / "figures" / "fig_kappa0_calm.png"))
    ap.add_argument("--csv", default=str(ROOT / "results" / "paper_tables" / "kappa0_calm.csv"))
    a = ap.parse_args(argv)
    fs.setup()

    panels = []
    for f in a.fits:
        fit = json.load(open(f))
        dev = fit["devices"][0]
        for p in ("gpu", "cpu"):
            panels.append((dev, p, fit["processors"][p]))
    devices = list(dict.fromkeys(d for d, _, _ in panels))

    fig, axes = plt.subplots(len(devices), 2, figsize=(6.5, 1.45 * len(devices) + 0.5), sharex=True, sharey=True,
                             dpi=300)
    axes = np.atleast_2d(axes)
    rows = []
    for dev, p, q in panels:
        ax = axes[devices.index(dev), 0 if p == "gpu" else 1]
        T = np.array([x["calm_s"] for x in q["onset_kappa_by_calm"]])
        k0 = np.array([x["kappa0"] for x in q["onset_kappa_by_calm"]])
        # per-cell medians (burst size x target), light gray behind the line
        cells = [(c["calm_ms"] / 1000, c["kappa0_median"]) for c in q["cells"]]
        ax.scatter([c[0] for c in cells], [c[1] for c in cells], s=5, color=fs.FAINT, zorder=2, linewidths=0)
        ax.axhline(1.0, color=fs.SPINE, lw=0.6, zorder=1)
        ok, why = decay_reported(k0)
        if ok:
            tt = np.geomspace(0.03, 10, 200)
            kf, ki, td = q["kappa_floor"], q["kappa_inf"], q["tau_down_s"]
            ax.plot(tt, kf + (ki - kf) * np.exp(-tt / td), "--", color=fs.MUTED, lw=0.9, zorder=3)
            lo, hi = q["tau_down_ci95"]
            note = f"τ_down {td * 1000:.0f} ms [{lo * 1000:.0f}, {hi * 1000:.0f}]\nκ_floor {kf:.2f}"
        else:
            lo_s, hi_s = f"{k0.min():.2f}", f"{k0.max():.2f}"
            rng = lo_s if lo_s == hi_s else f"{lo_s}–{hi_s}"
            if why == "no calm dependence" and k0.mean() > 0.9:
                why = "no drop"
            note = f"{why}\nκ₀ {rng}"
        ax.plot(T, k0, "-", color=fs.SLOTS[0], lw=1.6, zorder=4, solid_capstyle="round")
        ax.scatter(T, k0, s=16, color=fs.SLOTS[0], edgecolors="white", linewidths=0.8, zorder=5)
        high = k0.mean() > 0.8   # the note moves below a line that sits near kappa = 1
        ax.text(0.98, 0.06 if high else 0.95, note, transform=ax.transAxes, fontsize=5.5, color=fs.MUTED,
                va="bottom" if high else "top", ha="right")
        fs.style_axis(ax, f"{fs.NAMES.get(dev, dev)}, {p.upper()}")
        for t, k in zip(T, k0):
            rows.append([dev, p, f"{t:g}", f"{k:.4f}", q["kappa_floor"], q["kappa_inf"], q["tau_down_s"],
                         q["tau_down_ci95"][0], q["tau_down_ci95"][1], "yes" if ok else why])
        print(f"{dev} {p}: kappa0 {k0.min():.2f}–{k0.max():.2f}, fit drawn: {ok or why}")
    fs.calm_axis(axes[0, 0])
    axes[0, 0].set_ylim(0, 1.1)
    axes[0, 0].set_yticks([0, 0.5, 1.0])
    for ax in axes[-1]:
        ax.set_xlabel("calm length before the burst (s)", fontsize=7, color=fs.INK)
    for ax in axes[:, 0]:
        ax.set_ylabel("κ₀ at onset", fontsize=7, color=fs.INK)
    fig.tight_layout()
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(a.out, facecolor="white")
    fig.savefig(Path(a.out).with_suffix(".pdf"), facecolor="white")
    print(f"wrote {a.out} (+ .pdf)")

    Path(a.csv).parent.mkdir(parents=True, exist_ok=True)
    with open(a.csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["device", "processor", "calm_s", "kappa0", "kappa_floor", "kappa_inf", "tau_down_s",
                    "tau_down_ci_lo", "tau_down_ci_hi", "decay_fit_drawn"])
        w.writerows(rows)
    print(f"wrote {a.csv}")


if __name__ == "__main__":
    main()
