#!/usr/bin/env python3
"""H1b as pre-registered (docs/01 §2, frozen 2026-10-09; paper §4.7).

H1b: "the penalty grows with calm length and saturates; an exponential fit in calm length explains it
with R² ≥ 0.8 per device. If the penalty is already saturated at the shortest calm length (50 ms), H1b
counts as not supported and the decay time is reported as < 50 ms."

Operational rule (this script; the frozen text names the quantities but not the test):
  per device and processor, the H1a cells (own-target bursts, L1 = 50 %), trial-level
  P = first burst frame load cost / median load cost of the last 0.5 s (e1_fit.py);
  growth   = median P over the calms ≥ 1 s minus median P at 50 ms, bootstrap 95 % CI from
             resampling trials within cells;
  no penalty    if the median P at every calm ≥ 1 s is ≤ 1 (H1a fails there)      -> "no penalty";
  saturated     if the growth CI includes 0                                        -> "not supported, saturated at 50 ms, decay < 50 ms";
  otherwise fit P(T) = P_inf - (P_inf - P_0) exp(-T / tau_P) to the eight cell medians:
             R² ≥ 0.8 -> "supported" (tau_P with a bootstrap CI), else "not supported (shape)".

Usage: h1b_check.py <e1_fit_<dev>.json>... [--boot 2000] [--out results/paper_tables/h1b_check.md]
"""
import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
from scipy.optimize import least_squares

sys.path.insert(0, str(Path(__file__).resolve().parent))
from e1_fit import WARM_TAIL_S, load_trials  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
OWN = {"gpu": 1, "cpu": 2}
LOAD_PCT = 50
R2_MIN = 0.8
LONG_CALM_MS = 1000


def penalty(t, p):
    load = t[f"load_{p}"]
    tail = load[t["burst_t"] >= t["burst_t"][-1] - WARM_TAIL_S]
    m = np.nanmedian(tail) if len(tail) else np.nan
    return load[0] / m if np.isfinite(load[0]) and np.isfinite(m) and m > 0 else np.nan


def fit_exp(T, P):
    """P(T) = P_inf - (P_inf - P_0) exp(-T / tau). Returns tau_s, P_inf, P_0, R²."""
    def resid(x):
        return x[1] - (x[1] - x[2]) * np.exp(-T / np.exp(x[0])) - P
    r = least_squares(resid, [np.log(0.1), P[-1], P[0]],
                      bounds=([np.log(0.001), 0.0, 0.0], [np.log(20.0), 10.0, 10.0]))
    res = resid(r.x)
    ss_tot = float(np.sum((P - P.mean()) ** 2))
    r2 = 1.0 - float(np.sum(res ** 2)) / ss_tot if ss_tot > 0 else float("nan")
    return float(np.exp(r.x[0])), float(r.x[1]), float(r.x[2]), r2


def verdict(cells, n_boot=2000, seed=1):
    """cells: {calm_ms: array of trial-level P}. Returns a dict with the numbers and the verdict."""
    rng = np.random.default_rng(seed)
    calms = sorted(cells)
    med = {c: float(np.nanmedian(cells[c])) for c in calms}
    T = np.array(calms, dtype=float) / 1000.0
    P = np.array([med[c] for c in calms])
    long = [c for c in calms if c >= LONG_CALM_MS]
    short = calms[0]

    def resample():
        return {c: cells[c][rng.integers(0, len(cells[c]), len(cells[c]))] for c in calms}

    growth = float(np.nanmedian(np.concatenate([cells[c] for c in long]))) - med[short]
    gb = []
    for _ in range(n_boot):
        s = resample()
        gb.append(float(np.nanmedian(np.concatenate([s[c] for c in long]))) - float(np.nanmedian(s[short])))
    g_lo, g_hi = (float(np.percentile(gb, 2.5)), float(np.percentile(gb, 97.5)))
    out = {"P_by_calm": med, "P_short": med[short], "P_long_min": min(med[c] for c in long),
           "P_long_max": max(med[c] for c in long), "growth": growth, "growth_ci95": [g_lo, g_hi],
           "tau_P_s": None, "tau_P_ci95": None, "R2": None}
    if out["P_long_max"] <= 1.0:
        out["verdict"] = "no penalty"
        return out
    if g_lo <= 0.0 <= g_hi or growth <= 0.0:
        out["verdict"] = "not supported: saturated at 50 ms, decay < 50 ms"
        return out
    tau, p_inf, p0, r2 = fit_exp(T, P)
    taus = []
    for _ in range(min(n_boot, 500)):
        s = resample()
        Pb = np.array([float(np.nanmedian(s[c])) for c in calms])
        taus.append(fit_exp(T, Pb)[0])
    out.update({"tau_P_s": tau, "tau_P_ci95": [float(np.percentile(taus, 2.5)), float(np.percentile(taus, 97.5))],
                "P_inf": p_inf, "P_0": p0, "R2": r2})
    out["verdict"] = "supported" if r2 >= R2_MIN else f"not supported: exponential R² {r2:.2f} < {R2_MIN}"
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("fits", nargs="+")
    ap.add_argument("--boot", type=int, default=2000)
    ap.add_argument("--out", default=str(ROOT / "results" / "paper_tables" / "h1b_check.md"))
    a = ap.parse_args(argv)
    lines = ["| Device | Proc | P at 50 ms | P at calms ≥ 1 s | Growth (≥ 1 s − 50 ms) [95 % CI] | τ_P [95 % CI] | R² | H1b |",
             "|---|---|---|---|---|---|---|---|"]
    rows = []
    for f in a.fits:
        fit = json.load(open(f))
        dev = fit["devices"][0]
        trials = load_trials([ROOT / "results" / r for r in fit["runs"]], dev)
        for p in ("gpu", "cpu"):
            cells = defaultdict(list)
            for t in trials:
                if t["target"] == OWN[p] and t["load_pct"] == LOAD_PCT:
                    v = penalty(t, p)
                    if np.isfinite(v):
                        cells[t["calm_ms"]].append(v)
            cells = {c: np.asarray(v) for c, v in cells.items()}
            v = verdict(cells, a.boot)
            tau = (f"{v['tau_P_s'] * 1000:.0f} ms [{v['tau_P_ci95'][0] * 1000:.0f}, {v['tau_P_ci95'][1] * 1000:.0f}]"
                   if v["tau_P_s"] is not None else "")
            r2 = f"{v['R2']:.2f}" if v["R2"] is not None else ""
            lines.append(f"| {dev} | {p} | {v['P_short']:.2f} | {v['P_long_min']:.2f}–{v['P_long_max']:.2f} | "
                         f"{v['growth']:+.2f} [{v['growth_ci95'][0]:+.2f}, {v['growth_ci95'][1]:+.2f}] | {tau} | {r2} | {v['verdict']} |")
            print(lines[-1])
            rows.append({"device": dev, "processor": p, **{k: val for k, val in v.items() if k != "P_by_calm"},
                         "P_by_calm": {str(k): val for k, val in v["P_by_calm"].items()}})
    text = "\n".join(lines) + (f"\n\nOwn-target bursts at L1 = {LOAD_PCT} %, trial-level P as in e1_fit.py; n_boot {a.boot}; "
                               f"fits: " + ", ".join(a.fits) + "\n")
    Path(a.out).write_text(text)
    Path(a.out).with_suffix(".json").write_text(json.dumps(rows, indent=1))
    print(f"wrote {a.out}")


if __name__ == "__main__":
    main()
