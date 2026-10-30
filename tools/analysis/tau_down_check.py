#!/usr/bin/env python3
"""Robustness check of tau_down (paper §4.6): the pre-registered decay fit takes kappa_inf from the ramp fit
(docs/01 §4; e1_fit.py), which is unbounded when tau_up is not identifiable. This refits the same onset medians
kappa0(T) = floor + (k_start - floor) exp(-T / tau_down) with the start level free (three parameters), with
bootstrap 95 % CIs from resampling trials within cells, and prints both tau_down values side by side.

Usage: tau_down_check.py <e1_fit_<dev>.json>... [--boot 300] [--out results/paper_tables/tau_down_free_start.md]
"""
import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
from scipy.optimize import least_squares

sys.path.insert(0, str(Path(__file__).resolve().parent))
from e1_fit import PROC, kappa_ref, load_trials  # noqa: E402
from fig_kappa0_calm import decay_reported  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def fit_free(T, k0):
    def resid(x):
        return x[1] + (x[2] - x[1]) * np.exp(-T / np.exp(x[0])) - k0
    r = least_squares(resid, [np.log(0.05), k0.min(), k0.max()],
                      bounds=([np.log(0.001), 0.0, 0.0], [np.log(20.0), 1.5, 3.0]))
    return float(np.exp(r.x[0])), float(r.x[1]), float(r.x[2]), float(np.sqrt(np.mean(resid(r.x) ** 2)))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("fits", nargs="+")
    ap.add_argument("--boot", type=int, default=300)
    ap.add_argument("--out", default=str(ROOT / "results" / "paper_tables" / "tau_down_free_start.md"))
    a = ap.parse_args(argv)
    rng = np.random.default_rng(1)
    lines = ["| Device | Proc | τ_down, paper (κ_∞ from ramp) [95 % CI] | κ_∞ (ramp) | τ_down, free start [95 % CI] "
             "| κ_start | κ_floor | RMSE paper / free |", "|---|---|---|---|---|---|---|---|"]
    for f in a.fits:
        fit = json.load(open(f))
        dev = fit["devices"][0]
        trials = load_trials([ROOT / "results" / r for r in fit["runs"]], dev)
        for p in ("gpu", "cpu"):
            q = fit["processors"][p]
            T = np.array([x["calm_s"] for x in q["onset_kappa_by_calm"]])
            k0 = np.array([x["kappa0"] for x in q["onset_kappa_by_calm"]])
            ok, why = decay_reported(k0)
            if not ok:
                lines.append(f"| {dev} | {p} | {why} | | | | | |")
                continue
            kf, ki, td = q["kappa_floor"], q["kappa_inf"], q["tau_down_s"]
            rmse_paper = float(np.sqrt(np.mean((kf + (ki - kf) * np.exp(-T / td) - k0) ** 2)))
            tau, fl, ks, rmse_free = fit_free(T, k0)
            # bootstrap: resample trials within cells, medians per calm, refit
            mine = [t for t in trials if t["target"] in PROC[p]["targets"]]
            ref = kappa_ref(trials, p)
            cells = defaultdict(list)
            for t in mine:
                cells[(t["calm_ms"], t["load_pct"], t["target"])].append(ref / t[f"burst_{p}"][0])
            keys = sorted(cells)
            taus = []
            for _ in range(a.boot):
                by_calm = defaultdict(list)
                for k in keys:
                    v = np.asarray(cells[k])
                    by_calm[k[0]].extend(v[rng.integers(0, len(v), len(v))])
                Tb = np.array(sorted(by_calm)) / 1000.0
                kb = np.array([np.median(by_calm[c]) for c in sorted(by_calm)])
                taus.append(fit_free(Tb, kb)[0])
            lo, hi = np.percentile(taus, [2.5, 97.5])
            lines.append(f"| {dev} | {p} | {td * 1000:.0f} ms [{q['tau_down_ci95'][0] * 1000:.0f}, "
                         f"{q['tau_down_ci95'][1] * 1000:.0f}] | {ki:.2f} | {tau * 1000:.0f} ms [{lo * 1000:.0f}, {hi * 1000:.0f}] "
                         f"| {ks:.2f} | {fl:.2f} | {rmse_paper:.3f} / {rmse_free:.3f} |")
            print(lines[-1])
    text = "\n".join(lines) + f"\n\nn_boot {a.boot}; fits: " + ", ".join(a.fits) + "\n"
    Path(a.out).write_text(text)
    print(f"wrote {a.out}")


if __name__ == "__main__":
    main()
