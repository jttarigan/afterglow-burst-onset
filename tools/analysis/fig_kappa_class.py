#!/usr/bin/env python3
"""Figure: median clock index kappa(t) after burst onset, per changepoint class (paper §4.3).

One panel per device and processor; in each, the median kappa(t) over the trials of each class
(warmup, flat, slowdown, no_steady_state; e1_fit.classify), on a 10 ms grid over the 2 s burst,
with the class's share of trials. The trial set is exactly the one behind each device's E1 fit
(the run list in its e1_fit_<dev>.json), so the shares match Table S1.

Usage: fig_kappa_class.py <e1_fit_<dev>.json>... [--out paper/figures/fig_kappa_class.png]
                          [--csv results/paper_tables/kappa_class_medians.csv]
"""
import argparse
import csv
import json
import sys
import warnings
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from e1_fit import PROC, classify, kappa_ref, load_trials  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
NAMES = {"ip12": "iPhone 12 (A14)", "ipmini6": "iPad mini 6 (A15)", "ippro3": "iPad Pro 3 (A12X)",
         "poco": "POCO X6 Pro (Dimensity 8300)", "tab": "Galaxy Tab S7+ (SD865+)",
         "s6lite": "Galaxy Tab S6 Lite (Exynos 9611)", "a56": "Galaxy A56 (Exynos 1580)",
         "fold8": "Galaxy Z Fold 8 (SD 8 Elite Gen 5)"}
CLASSES = ["warmup", "flat", "slowdown", "no_steady_state"]
LABELS = {"warmup": "warm-up", "flat": "flat", "slowdown": "slowdown", "no_steady_state": "no steady state"}
# dataviz palette: blue, orange and the darker aqua pass the validator on white for all pairs (CVD
# dE 8.4, normal 21.6, contrast >= 3:1, 2026-10-21); "flat", the class with no change, is a deliberate
# neutral gray, which the validator flags (chroma floor), so identity never rests on colour alone:
# every class has its own line style and each panel prints the class shares.
COLORS = {"warmup": "#2a78d6", "flat": "#52514e", "slowdown": "#eb6834", "no_steady_state": "#199e70"}
STYLES = {"warmup": "-", "flat": "-.", "slowdown": "--", "no_steady_state": ":"}
MIN_SHARE = 0.5   # a grid point needs data from at least half of the class's trials
INK, MUTED, GRID = "#0b0b0b", "#52514e", "#e1e0d9"
GRID_S = np.arange(0.0, 2.0001, 0.01)


def class_medians(trials, p):
    """{class: (n, median kappa on GRID_S)} for the trials of processor p."""
    mine = [t for t in trials if t["target"] in PROC[p]["targets"]]
    ref = kappa_ref(trials, p)
    rows = defaultdict(list)
    for t in mine:
        kb = ref / t[f"burst_{p}"]
        c = classify(kb)
        # resampled to the common grid; frames beyond the trial's last frame are left out
        y = np.interp(GRID_S, t["burst_t"], kb, right=np.nan)
        rows[c].append(y)
    out = {}
    for c in CLASSES:
        if not rows[c]:
            continue
        m = np.vstack(rows[c])
        with warnings.catch_warnings():   # grid points past every trial's end are all-NaN: expected, masked below
            warnings.simplefilter("ignore", RuntimeWarning)
            med = np.nanmedian(m, axis=0)
        med[np.sum(np.isfinite(m), axis=0) < MIN_SHARE * len(rows[c])] = np.nan   # trials end at ~2 s
        out[c] = (len(rows[c]), med)
    return out, len(mine)


def level_and_t90(y):
    """Level = median over 1-2 s; t90 = first time the trace reaches 90 % of it (NaN if never)."""
    level = float(np.nanmedian(y[GRID_S >= 1.0]))
    hit = np.flatnonzero(y >= 0.9 * level)
    return level, (float(GRID_S[hit[0]]) if len(hit) else float("nan"))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("fits", nargs="+")
    ap.add_argument("--out", default=str(ROOT / "paper" / "figures" / "fig_kappa_class.png"))
    ap.add_argument("--csv", default=str(ROOT / "results" / "paper_tables" / "kappa_class_medians.csv"))
    a = ap.parse_args(argv)

    panels = []   # (device, processor, {class: (n, median)}, n_total)
    for f in a.fits:
        fit = json.load(open(f))
        dev = fit["devices"][0]
        runs = [ROOT / "results" / r for r in fit["runs"]]
        missing = [r for r in runs if not (r / "frames.csv").exists()]
        if missing:
            sys.exit(f"{f}: missing runs {missing[:3]}")
        trials = load_trials(runs, dev)
        for p in ("gpu", "cpu"):
            med, n = class_medians(trials, p)
            panels.append((dev, p, med, n))
            print(f"{dev} {p}: " + ", ".join(f"{c} {med[c][0] / n:.1%}" for c in med))

    devices = list(dict.fromkeys(d for d, _, _, _ in panels))
    fig, axes = plt.subplots(len(devices), 2, figsize=(6.5, 1.55 * len(devices) + 0.6), sharex=True, sharey=True,
                             dpi=300)
    axes = np.atleast_2d(axes)
    plt.rcParams["font.family"] = "sans-serif"
    for (dev, p, med, n) in panels:
        ax = axes[devices.index(dev), 0 if p == "gpu" else 1]
        for c in CLASSES:
            if c not in med:
                continue
            k, y = med[c]
            ax.plot(GRID_S, y, STYLES[c], color=COLORS[c], lw=1.4, label=LABELS[c], solid_capstyle="round")
        text = " · ".join(f"{LABELS[c]} {med[c][0] / n:.0%}" for c in CLASSES if c in med)
        ax.text(0.02, 0.05, text, transform=ax.transAxes, fontsize=6, color=MUTED, va="bottom")
        ax.set_title(f"{NAMES.get(dev, dev)}, {p.upper()}", fontsize=7, color=INK, loc="left", pad=3)
        ax.grid(True, color=GRID, lw=0.5)
        ax.tick_params(labelsize=6, colors=MUTED, length=2)
        for s in ("top", "right"):
            ax.spines[s].set_visible(False)
        for s in ("left", "bottom"):
            ax.spines[s].set_color("#c3c2b7")
    axes[0, 0].set_ylim(0, 1.15)
    axes[0, 0].set_xlim(0, 2)
    axes[0, 0].set_yticks([0, 0.5, 1.0])
    for ax in axes[-1]:
        ax.set_xlabel("time since burst onset (s)", fontsize=7, color=INK)
    for ax in axes[:, 0]:
        ax.set_ylabel("κ (clock index)", fontsize=7, color=INK)
    handles, labels = axes[0, 0].get_legend_handles_labels()
    seen = {}
    for h, l in zip(handles, labels):
        seen.setdefault(l, h)
    for ax in axes.flat:   # every class may not appear in the first panel: collect from all
        for h, l in zip(*ax.get_legend_handles_labels()):
            seen.setdefault(l, h)
    order = [LABELS[c] for c in CLASSES if LABELS[c] in seen]
    fig.legend([seen[l] for l in order], order, loc="upper center", ncol=4, fontsize=6.5, frameon=False,
               bbox_to_anchor=(0.5, 1.0))
    fig.tight_layout(rect=(0, 0, 1, 0.97))
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(a.out, facecolor="white")
    fig.savefig(Path(a.out).with_suffix(".pdf"), facecolor="white")
    print(f"wrote {a.out} (+ .pdf)")

    Path(a.csv).parent.mkdir(parents=True, exist_ok=True)
    with open(a.csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["device", "processor", "class", "n_trials", "share", "level_1to2s", "t90_s"]
                   + [f"k_{t:.2f}" for t in GRID_S[::10]])
        for dev, p, med, n in panels:
            for c in CLASSES:
                if c in med:
                    k, y = med[c]
                    level, t90 = level_and_t90(y)
                    w.writerow([dev, p, c, k, f"{k / n:.4f}", f"{level:.3f}", f"{t90:.2f}"] + [f"{v:.4f}" for v in y[::10]])
    print(f"wrote {a.csv} (medians every 0.1 s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
