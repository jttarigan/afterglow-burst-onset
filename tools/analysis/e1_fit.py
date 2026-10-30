#!/usr/bin/env python3
"""E1 step-response fit (docs/04 E1; model docs/01 §4).

Per device and processor (GPU, CPU), from stepbench run directories:
  kappa      = fastest probe time of the session (1st percentile, D47) / this frame's probe time
               (docs/03 §6);
  tau_up     = joint fit over cells of kappa(t) = k_inf - (k_inf - k0_cell) * exp(-t / tau_up) after
               the onset (one tau_up and k_inf, one k0 per cell; k_inf is free, so the percentile-based
               kappa reference does not bias tau);
  tau_down   = fit of the onset clock index versus calm length,
               k0(T) = k_floor + (k_inf - k_floor) * exp(-T / tau_down);
  tau_down_direct = the same decay fitted directly to kappa(t) logged during long calms (S6);
  P(T, L1)   = first burst frame load cost / warm cost (median of the last 0.5 s of the burst);
  bootstrap 95 % CIs by resampling trials within cells; changepoint class of every burst trace
  (Barrett et al., OOPSLA 2017; docs/01 §7).
Figures are added in M1, with real data.

Usage: e1_fit.py <run-dir-or-root>... [--device ID] [--boot N] [--out fit.json]
"""
import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
from scipy.optimize import least_squares

from agcsv import read_frames, read_run
from validate import find_runs

PROC = {
    "gpu": {"probe": "gpu_probe_ns", "load": "gpu_main_ns", "targets": {1, 3}},
    "cpu": {"probe": "cpu_probe_ns", "load": "cpu_sim_ns", "targets": {2, 3}},
}
WARM_TAIL_S = 0.5
DIRECT_MIN_CALM_MS = 2000


# ---------------------------------------------------------------- data
def load_trials(run_dirs, device=None):
    """Trials as dicts; one entry per (run, sb_trial)."""
    trials = []
    for run in run_dirs:
        prov = read_run(run)
        dev = prov.get("device", {}).get("id", "unknown")
        if device is not None and dev != device:
            continue
        _, df = read_frames(run)
        df = df[df["sb_trial"].notna() & df["sb_phase"].isin([0, 1])]
        for trial, g in df.groupby("sb_trial", sort=True):
            calm = g[g["sb_phase"] == 0]
            burst = g[g["sb_phase"] == 1]
            if burst.empty:
                continue
            t0b = burst["t_frame_start_ns"].iloc[0]
            t0c = calm["t_frame_start_ns"].iloc[0] if not calm.empty else t0b
            tr = {
                "device": dev, "run": Path(run).name, "trial": int(trial),
                "calm_ms": int(burst["sb_calm_ms"].iloc[0]), "load_pct": int(burst["sb_load_pct"].iloc[0]),
                "target": int(burst["sb_target"].iloc[0]),
                "calm_t": (calm["t_frame_start_ns"].to_numpy() - t0c) / 1e9,
                "burst_t": (burst["t_frame_start_ns"].to_numpy() - t0b) / 1e9,
            }
            for p, spec in PROC.items():
                tr[f"calm_{p}"] = calm[spec["probe"]].to_numpy(dtype=float)
                tr[f"burst_{p}"] = burst[spec["probe"]].to_numpy(dtype=float)
                tr[f"load_{p}"] = burst[spec["load"]].to_numpy(dtype=float)
            trials.append(tr)
    return trials


KAPPA_REF_PERCENTILE = 1.0         # D47: a robust "fastest", not the single minimum


def kappa_ref(trials, p):
    """Fastest probe time of the session (docs/03 §6), taken as the 1st percentile of all its calm
    and burst probe times so one outlier cannot set the scale (D47)."""
    vals = np.concatenate([np.concatenate([t[f"calm_{p}"], t[f"burst_{p}"]]) for t in trials])
    return float(np.nanpercentile(vals, KAPPA_REF_PERCENTILE))


# ---------------------------------------------------------------- model fits
def _median_trace(rows):
    n = min(len(r) for r in rows)
    return np.nanmedian(np.vstack([r[:n] for r in rows]), axis=0)


def fit_ramp(cells):
    """cells: list of (t, kappa_median). Returns tau_up, k_inf, k0 per cell, identifiable flag."""
    k_inf0 = float(np.median([np.median(k[-max(1, len(k) // 4):]) for _, k in cells]))
    x0 = np.concatenate([[np.log(0.1), k_inf0], [float(k[0]) for _, k in cells]])
    lo = np.concatenate([[np.log(1e-3), 0.05], np.full(len(cells), 0.01)])
    hi = np.concatenate([[np.log(20.0), 3.0], np.full(len(cells), 3.0)])
    x0 = np.clip(x0, lo + 1e-9, hi - 1e-9)

    def resid(x):
        tau, kinf = np.exp(x[0]), x[1]
        return np.concatenate([kinf - (kinf - x[2 + i]) * np.exp(-t / tau) - k for i, (t, k) in enumerate(cells)])

    r = least_squares(resid, x0, bounds=(lo, hi), x_scale="jac")
    k_inf = float(r.x[1])
    k0 = r.x[2:]
    # Not identifiable without a ramp (k_inf ~ k0) or with tau or k_inf pinned to a bound (2026-10-17:
    # the Tab and iPhone 12 CPU fits sat at the 20 s bound yet were flagged identifiable).
    at_bound = r.x[0] > np.log(20.0) - 0.05 or r.x[1] < 0.05 + 1e-3
    return float(np.exp(r.x[0])), k_inf, k0, bool(np.max(k_inf - k0) > 0.05 and not at_bound)


def fit_decay(T, k0, k_inf):
    """k0(T) = floor + (k_inf - floor) * exp(-T / tau_down), k_inf fixed from the ramp fit."""
    x0 = np.array([np.log(max(np.median(T), 0.05)), float(np.min(k0))])

    def resid(x):
        return x[1] + (k_inf - x[1]) * np.exp(-T / np.exp(x[0])) - k0

    r = least_squares(resid, x0, bounds=([np.log(1e-3), 0.0], [np.log(100.0), 3.0]))
    return float(np.exp(r.x[0])), float(r.x[1])


def fit_decay_direct(t, k):
    """kappa(t) = floor + (k_start - floor) * exp(-t / tau) during a calm."""
    x0 = np.array([np.log(1.0), float(np.min(k)), float(k[0])])

    def resid(x):
        return x[1] + (x[2] - x[1]) * np.exp(-t / np.exp(x[0])) - k

    r = least_squares(resid, x0, bounds=([np.log(1e-3), 0.0, 0.0], [np.log(100.0), 3.0, 3.0]))
    return float(np.exp(r.x[0])), float(r.x[1])


# ---------------------------------------------------------------- changepoints
def changepoints(x, min_size=3):
    """Binary segmentation for mean shifts with a BIC-style penalty (sigma from first differences)."""
    x = np.asarray(x, dtype=float)
    n = len(x)
    if n < 2 * min_size:
        return []
    sigma = np.median(np.abs(np.diff(x) - np.median(np.diff(x)))) / 0.6745 / np.sqrt(2.0)
    sigma = max(sigma, 1e-9)
    penalty = 2.0 * sigma * sigma * np.log(n) * 2.0

    def cost(a, b):
        seg = x[a:b]
        return float(np.sum((seg - seg.mean()) ** 2))

    cps, stack = [], [(0, n)]
    while stack:
        a, b = stack.pop()
        if b - a < 2 * min_size:
            continue
        base = cost(a, b)
        best, best_s = 0.0, None
        for s in range(a + min_size, b - min_size + 1):
            gain = base - cost(a, s) - cost(s, b)
            if gain > best:
                best, best_s = gain, s
        if best_s is not None and best > penalty:
            cps.append(best_s)
            stack += [(a, best_s), (best_s, b)]
    return sorted(cps)


def classify(kappa_series):
    """flat / warmup / slowdown / no_steady_state, after Barrett et al. (OOPSLA 2017)."""
    x = np.asarray(kappa_series, dtype=float)
    cps = changepoints(x)
    if not cps:
        return "flat"
    if cps[-1] > 0.75 * len(x):
        return "no_steady_state"
    bounds = [0] + cps + [len(x)]
    means = [x[a:b].mean() for a, b in zip(bounds[:-1], bounds[1:])]
    return "warmup" if means[-1] >= max(means) - 1e-12 else "slowdown"


# ---------------------------------------------------------------- per-processor analysis
def analyse(trials, p, n_boot=1000, seed=1):
    mine = [t for t in trials if t["target"] in PROC[p]["targets"]]
    if not mine:
        return None
    ref = kappa_ref(trials, p)
    for t in mine:
        t["kb"] = ref / t[f"burst_{p}"]
        t["kc"] = ref / t[f"calm_{p}"]

    cells = defaultdict(list)
    for t in mine:
        cells[(t["calm_ms"], t["load_pct"], t["target"])].append(t)
    keys = sorted(cells)

    def fit_all(sample):
        ramp_cells = [(sample[k][0]["burst_t"][: min(len(x["kb"]) for x in sample[k])],
                       _median_trace([x["kb"] for x in sample[k]])) for k in keys]
        tau_up, k_inf, _, ident = fit_ramp(ramp_cells)
        by_calm = defaultdict(list)
        for k in keys:
            by_calm[k[0]].extend(x["kb"][0] for x in sample[k])
        T = np.array(sorted(by_calm), dtype=float) / 1000.0
        k0 = np.array([np.median(by_calm[c]) for c in sorted(by_calm)])
        tau_down, k_floor = fit_decay(T, k0, k_inf)
        return tau_up, k_inf, ident, tau_down, k_floor, T, k0

    tau_up, k_inf, ident, tau_down, k_floor, T, k0 = fit_all(cells)

    rng = np.random.default_rng(seed)
    boots = []
    for _ in range(n_boot):
        sample = {k: [cells[k][i] for i in rng.integers(0, len(cells[k]), len(cells[k]))] for k in keys}
        b = fit_all(sample)
        boots.append((b[0], b[3], b[4]))
    boots = np.array(boots) if boots else np.full((1, 3), np.nan)

    def ci(col):
        return [float(np.percentile(boots[:, col], 2.5)), float(np.percentile(boots[:, col], 97.5))]

    # Direct decay from long calms: median kappa(t) over trials whose calm reaches t.
    long_calm = [t for t in mine if t["calm_ms"] >= DIRECT_MIN_CALM_MS and len(t["kc"]) > 0]
    tau_down_direct = None
    if long_calm:
        n = max(len(t["kc"]) for t in long_calm)
        med = np.array([np.median([t["kc"][i] for t in long_calm if len(t["kc"]) > i]) for i in range(n)])
        tt = np.arange(n) * np.median(np.diff(long_calm[0]["calm_t"]))
        tau_down_direct, _ = fit_decay_direct(tt, med)

    # Onset penalty P(T, L1) per cell.
    cell_rows = []
    for k in keys:
        Ps = []
        for x in cells[k]:
            load = x[f"load_{p}"]
            tail = load[x["burst_t"] >= x["burst_t"][-1] - WARM_TAIL_S]
            if np.isfinite(load[0]) and np.isfinite(np.nanmedian(tail)):
                Ps.append(load[0] / np.nanmedian(tail))
        Ps = np.array(Ps)
        pb = [np.median(rng.choice(Ps, len(Ps))) for _ in range(min(n_boot, 2000))] if len(Ps) else []
        cell_rows.append({
            "calm_ms": k[0], "load_pct": k[1], "target": k[2], "n_trials": len(cells[k]),
            "kappa0_median": float(np.median([x["kb"][0] for x in cells[k]])),
            "P_median": float(np.median(Ps)) if len(Ps) else None,
            "P_ci95": [float(np.percentile(pb, 2.5)), float(np.percentile(pb, 97.5))] if len(pb) else None,
        })

    classes = defaultdict(int)
    for t in mine:
        classes[classify(t["kb"])] += 1

    return {
        "kappa_ref_ns": ref,
        "tau_up_s": tau_up, "tau_up_ci95": ci(0), "tau_up_identifiable": ident,
        "kappa_inf": k_inf,
        "tau_down_s": tau_down, "tau_down_ci95": ci(1),
        "kappa_floor": k_floor, "kappa_floor_ci95": ci(2),
        "tau_down_direct_s": tau_down_direct,
        "onset_kappa_by_calm": [{"calm_s": float(a), "kappa0": float(b)} for a, b in zip(T, k0)],
        "cells": cell_rows,
        "changepoint_classes": dict(classes),
        "n_trials": len(mine), "n_boot": n_boot,
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description="Fit the E1 step response (tau_up, tau_down, kappa_floor).")
    ap.add_argument("roots", nargs="+")
    ap.add_argument("--device")
    ap.add_argument("--boot", type=int, default=1000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out")
    a = ap.parse_args(argv)
    runs = list(find_runs(a.roots))
    trials = load_trials(runs, a.device)
    if not trials:
        print("e1_fit: no stepbench trials found", file=sys.stderr)
        return 1
    result = {"schema": "afterglow.e1fit/1", "runs": [Path(r).name for r in runs],
              "devices": sorted({t["device"] for t in trials}), "processors": {}}
    for p in PROC:
        res = analyse(trials, p, a.boot, a.seed)
        if res is not None:
            result["processors"][p] = res
            print(f"{p}: tau_up={res['tau_up_s'] * 1000:.1f} ms [{res['tau_up_ci95'][0] * 1000:.1f}, "
                  f"{res['tau_up_ci95'][1] * 1000:.1f}]  tau_down={res['tau_down_s']:.3f} s "
                  f"[{res['tau_down_ci95'][0]:.3f}, {res['tau_down_ci95'][1]:.3f}]  "
                  f"kappa_floor={res['kappa_floor']:.3f}  tau_down_direct={res['tau_down_direct_s']}")
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
