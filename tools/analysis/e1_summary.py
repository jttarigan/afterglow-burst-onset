#!/usr/bin/env python3
"""One table across devices from e1_fit.py outputs (light: reads JSON, fits nothing).

Applies the 2026-10-17 identifiability rule (commit 8fa6002) to fits written before it: tau_up within
5 % of the 20 s bound, or kappa_inf at its 0.05 bound, is not identifiable. A decay is reported only when
the onset kappa moves by more than 0.05 across calm lengths; otherwise tau_down is not meaningful.

Usage: e1_summary.py e1_fit_<dev>.json... [--out summary.md]
"""
import argparse
import json
import math
from pathlib import Path

TARGET = {1: "gpu", 2: "cpu", 3: "both"}


def fmt_ci(v, ci, unit="", scale=1.0, nd=0):
    return f"{v * scale:.{nd}f}{unit} [{ci[0] * scale:.{nd}f}, {ci[1] * scale:.{nd}f}]"


def summarise(path):
    d = json.loads(Path(path).read_text())
    dev = d["devices"][0]
    rows, grid = [], []
    for proc, p in d["processors"].items():
        k0 = {round(x["calm_s"], 3): x["kappa0"] for x in p["onset_kappa_by_calm"]}
        span = max(k0.values()) - min(k0.values())
        ident = p["tau_up_identifiable"] and p["tau_up_s"] < 20.0 * math.exp(-0.05) and p["kappa_inf"] > 0.051
        tau_up = fmt_ci(p["tau_up_s"], p["tau_up_ci95"], " ms", 1000) if ident else "n.i."
        if span > 0.05:
            tau_down = fmt_ci(p["tau_down_s"], p["tau_down_ci95"], " ms", 1000)
            floor = fmt_ci(p["kappa_floor"], p["kappa_floor_ci95"], nd=2)
        else:
            tau_down, floor = "no calm dependence", f"{p['kappa_floor']:.2f}"
        cp = ", ".join(f"{k} {v}" for k, v in sorted(p["changepoint_classes"].items(), key=lambda kv: -kv[1]))
        rows.append(f"| {dev} | {proc} | {p['kappa_ref_ns'] / 1000:.1f} | {k0[0.05]:.2f} | {k0[8.0]:.2f} | {span:.2f} "
                    f"| {floor} | {tau_down} | {tau_up} | {p['n_trials']} | {cp} |")
        for c in p["cells"]:
            if c["calm_ms"] in (50, 8000):
                grid.append((dev, proc, TARGET.get(c["target"], c["target"]), c["load_pct"], c["calm_ms"],
                             c["P_median"], c["P_ci95"]))
    return rows, grid


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("fits", nargs="+")
    ap.add_argument("--out")
    a = ap.parse_args()
    head = ["| Device | Proc | κ ref µs | κ₀ at 0.05 s | κ₀ at 8 s | κ₀ span | κ_floor [95 % CI] | τ_down [95 % CI] "
            "| τ_up [95 % CI] | Trials | Changepoint classes |", "|---|---|---|---|---|---|---|---|---|---|---|"]
    ghead = ["| Device | Proc | Target | L1 % | P at 0.05 s [CI] | P at 8 s [CI] |", "|---|---|---|---|---|---|"]
    rows, grid = [], []
    for f in a.fits:
        r, g = summarise(f)
        rows += r
        grid += g
    cells = {}
    for dev, proc, tgt, load, calm, p, ci in grid:
        cells.setdefault((dev, proc, tgt, load), {})[calm] = f"{p:.2f} [{ci[0]:.2f}, {ci[1]:.2f}]"
    grows = [f"| {dev} | {proc} | {tgt} | {load} | {v.get(50, '')} | {v.get(8000, '')} |"
             for (dev, proc, tgt, load), v in cells.items()]
    text = "\n".join(["## Fit summary", ""] + head + rows + ["", "## Onset penalty P, shortest and longest calm", ""]
                     + ghead + grows + [""])
    if a.out:
        Path(a.out).write_text(text)
    print(text)


if __name__ == "__main__":
    main()
