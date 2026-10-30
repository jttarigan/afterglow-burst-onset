#!/usr/bin/env python3
"""M1 GO/NO-GO gate (docs/04 E1 "M1 pilot", docs/05 M1) and the κ(t) figures.

Per device and processor, from the E1 pilot runs (stepbench, valid runs only):
  P(T_calm) = cost of the first burst frame's load / its warm cost, where the warm cost is the
              median load cost of the last 0.5 s of the 2 s burst (GPU: gpu_main_ns on GPU-target
              trials; CPU: cpu_sim_ns on CPU-target trials). Median per T_calm with a bootstrap
              95 % CI (10,000 resamples, docs/01 §7).
  A device passes if, for GPU or CPU, the median P at T_calm = 4 s is >= 1.15 and its CI excludes 1.0.
  GO if at least one Apple and one Android device pass. The user decides (docs/05 M1).
Figures (light mode, one-hue ordinal ramp per calm length; palette checked with the dataviz
validator): kappa_onset_<device>.png (κ after the onset, GPU | CPU) and penalty_<device>.png.

Usage: m1_gate.py <runs or roots>... --out <dir> [--boot 10000]
"""
import argparse
import json
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

import e1_fit  # noqa: E402
import validate  # noqa: E402

THRESHOLD = 1.15
GATE_CALM_MS = 4000
WARM_TAIL_S = 0.5
APPLE = {"ip12", "ipmini6", "ippro3", "ks", "mac"}
ANDROID = {"poco", "tab", "a56"}
# Light-mode palette (dataviz reference palette): text and grid tokens, an ordinal blue ramp for
# calm lengths (validated: 3 steps 300/500/700, 4 steps 250/400/550/700), categorical slots for
# processors (slot 1 blue, slot 2 orange).
INK, INK2, GRID, SURFACE = "#0b0b0b", "#52514e", "#e4e3df", "#fcfcfb"
RAMP = {3: ["#6da7ec", "#256abf", "#0d366b"], 4: ["#86b6ef", "#3987e5", "#1c5cab", "#0d366b"]}
PROC_COLOR = {"gpu": "#2a78d6", "cpu": "#eb6834"}
PROC_TARGET = {"gpu": 1, "cpu": 2}


def penalty(trial, p):
    load = trial[f"load_{p}"]
    t = trial["burst_t"]
    tail = load[t >= t[-1] - WARM_TAIL_S]
    warm = np.nanmedian(tail)
    return float(load[0] / warm) if np.isfinite(load[0]) and warm > 0 else np.nan


def boot_ci(x, n, rng):
    x = np.asarray([v for v in x if np.isfinite(v)])
    if len(x) == 0:
        return np.nan, (np.nan, np.nan)
    meds = np.median(rng.choice(x, size=(n, len(x)), replace=True), axis=1)
    return float(np.median(x)), (float(np.percentile(meds, 2.5)), float(np.percentile(meds, 97.5)))


def style(ax):
    ax.set_facecolor(SURFACE)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(GRID)
    ax.tick_params(colors=INK2, labelsize=8)
    ax.grid(True, color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)


def kappa_figure(device, trials, out):
    procs = [p for p in ("gpu", "cpu") if any(t["target"] == PROC_TARGET[p] for t in trials)]
    calms = sorted({t["calm_ms"] for t in trials})
    if len(calms) > 4:   # an 8-step ramp is not separable: show four representative calm lengths
        calms = [calms[i] for i in np.linspace(0, len(calms) - 1, 4).round().astype(int)]
    colors = RAMP.get(len(calms), RAMP[4][-len(calms):])
    fig, axes = plt.subplots(1, len(procs), figsize=(3.4 * len(procs), 2.8), sharey=True, facecolor=SURFACE)
    axes = np.atleast_1d(axes)
    for ax, p in zip(axes, procs):
        style(ax)
        mine = [t for t in trials if t["target"] == PROC_TARGET[p]]
        ref = e1_fit.kappa_ref(trials, p)
        for calm, col in zip(calms, colors):
            rows = [ref / t[f"burst_{p}"] for t in mine if t["calm_ms"] == calm]
            if not rows:
                continue
            n = min(len(r) for r in rows)
            k = np.nanmedian(np.vstack([r[:n] for r in rows]), axis=0)
            tt = mine[0]["burst_t"][:n] if len(mine[0]["burst_t"]) >= n else np.arange(n) / 60.0
            ax.plot(tt, k, color=col, linewidth=2, solid_capstyle="round", label=f"{calm / 1000:g} s")
            # Direct label at the onset, where the curves are apart (they converge once warm).
            ax.annotate(f"{calm / 1000:g} s", (tt[0], k[0]), xytext=(-5, 0), textcoords="offset points",
                        color=INK2, fontsize=7, va="center", ha="right")
        ax.set_title(f"{p.upper()} probe", color=INK, fontsize=9, loc="left")
        ax.set_xlabel("time since burst onset (s)", color=INK2, fontsize=8)
        ax.set_xlim(-0.3, 2.05)
        ax.set_xticks([0, 0.5, 1.0, 1.5, 2.0])
    axes[0].set_ylabel("clock index κ (1 = fastest)", color=INK2, fontsize=8)
    leg = axes[-1].legend(title="calm length", loc="lower right", fontsize=7, title_fontsize=7, frameon=False)
    for t in leg.get_texts():
        t.set_color(INK2)
    leg.get_title().set_color(INK2)
    fig.suptitle(f"{device}: clock index after a calm period (median of trials; line label = calm length)",
                 color=INK, fontsize=9, x=0.02, ha="left")
    fig.tight_layout()
    fig.savefig(out, dpi=160, facecolor=SURFACE)
    plt.close(fig)


def work_figure(run_name, trials, out):
    """Proposed work index (STATUS finding 1): warm load cost / load cost per burst frame, i.e. the
    load's own speed relative to warm, after the onset. One figure per run (contexts differ)."""
    procs = [p for p in ("gpu", "cpu") if any(t["target"] == PROC_TARGET[p] for t in trials)]
    if not procs:
        return
    fig, axes = plt.subplots(1, len(procs), figsize=(3.4 * len(procs), 2.8), sharey=True, facecolor=SURFACE)
    axes = np.atleast_1d(axes)
    for ax, p in zip(axes, procs):
        style(ax)
        mine = [t for t in trials if t["target"] == PROC_TARGET[p]]
        calms = sorted({t["calm_ms"] for t in mine})
        if len(calms) > 4:
            calms = [calms[i] for i in np.linspace(0, len(calms) - 1, 4).round().astype(int)]
        colors = RAMP.get(len(calms), RAMP[4][-len(calms):]) if len(calms) > 2 else RAMP[3][-len(calms):]
        for calm, col in zip(calms, colors):
            rows = []
            for t in mine:
                if t["calm_ms"] != calm:
                    continue
                load = t[f"load_{p}"]
                warm = np.nanmedian(load[t["burst_t"] >= t["burst_t"][-1] - WARM_TAIL_S])
                rows.append(warm / load)
            n = min(len(r) for r in rows)
            k = np.nanmedian(np.vstack([r[:n] for r in rows]), axis=0)
            tt = np.arange(n) / 60.0
            ax.plot(tt[:31], k[:31], color=col, linewidth=2, marker="o", markersize=3, label=f"{calm / 1000:g} s")
        ax.set_title(f"{p.upper()} load", color=INK, fontsize=9, loc="left")
        ax.set_xlabel("time since burst onset (s)", color=INK2, fontsize=8)
        ax.set_xlim(-0.02, 0.52)
    axes[0].set_ylabel("warm cost ÷ frame cost (1 = warm)", color=INK2, fontsize=8)
    leg = axes[-1].legend(title="calm length", loc="lower right", fontsize=7, title_fontsize=7, frameon=False)
    for t in leg.get_texts():
        t.set_color(INK2)
    leg.get_title().set_color(INK2)
    fig.suptitle(f"{run_name}: work index after the onset (median of trials; first 0.5 s)", color=INK, fontsize=9,
                 x=0.02, ha="left")
    fig.tight_layout()
    fig.savefig(out, dpi=160, facecolor=SURFACE)
    plt.close(fig)


def penalty_figure(device, table, out):
    fig, ax = plt.subplots(figsize=(4.2, 2.8), facecolor=SURFACE)
    style(ax)
    ax.axhline(1.0, color=INK2, linewidth=1)
    ax.axhline(THRESHOLD, color=INK2, linewidth=1, linestyle=(0, (3, 3)))
    ax.annotate(f"gate {THRESHOLD}", (0.0, THRESHOLD), xycoords=("axes fraction", "data"), xytext=(3, 3),
                textcoords="offset points", ha="left", color=INK2, fontsize=7)
    ends = {}
    for p in ("gpu", "cpu"):
        rows = [r for r in table if r["processor"] == p]
        if not rows:
            continue
        x = np.array([r["calm_ms"] / 1000 for r in rows])
        y = np.array([r["P_median"] for r in rows])
        lo = y - np.array([r["P_ci95"][0] for r in rows])
        hi = np.array([r["P_ci95"][1] for r in rows]) - y
        ax.errorbar(x, y, yerr=[lo, hi], color=PROC_COLOR[p], linewidth=2, marker="o", markersize=5,
                    capsize=3, markeredgecolor=SURFACE, markeredgewidth=1.5, label=p.upper())
        ends[p] = (x[-1], y[-1])
    if len(ends) == 2 and abs(ends["gpu"][1] - ends["cpu"][1]) < 0.04 * max(ends["gpu"][1], ends["cpu"][1]):
        offsets = {"gpu": 7, "cpu": -7}   # stagger labels that would collide
    else:
        offsets = {"gpu": 0, "cpu": 0}
    for p, (xe, ye) in ends.items():
        ax.annotate(p.upper(), (xe, ye), xytext=(8, offsets[p]), textcoords="offset points", color=INK2,
                    fontsize=7, va="center")
    leg = ax.legend(loc="upper left", fontsize=7, frameon=False)
    for t in leg.get_texts():
        t.set_color(INK2)
    calms = sorted({r["calm_ms"] / 1000 for r in table})
    ax.set_xscale("log")
    ax.set_xticks(calms)
    ax.set_xticklabels([f"{c:g}" for c in calms])
    ax.minorticks_off()
    ax.set_xlim(min(calms) / 1.6, max(calms) * 1.9)
    ax.set_xlabel("calm length before the burst, T_calm (s)", color=INK2, fontsize=8)
    ax.set_ylabel("first burst frame ÷ warm cost", color=INK2, fontsize=8)
    ax.set_title(f"{device}: onset penalty P (median, 95 % CI)", color=INK, fontsize=9, loc="left")
    fig.tight_layout()
    fig.savefig(out, dpi=160, facecolor=SURFACE)
    plt.close(fig)


def main(argv=None):
    ap = argparse.ArgumentParser(description="M1 GO/NO-GO gate from the E1 pilot.")
    ap.add_argument("roots", nargs="+")
    ap.add_argument("--out", required=True)
    ap.add_argument("--boot", type=int, default=10000)
    ap.add_argument("--include-invalid", action="store_true", help="development only; never for the gate")
    a = ap.parse_args(argv)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(1)

    runs, skipped = [], []
    for r in validate.find_runs(a.roots):
        reasons = validate.check_run(r)
        if reasons and not a.include_invalid:
            skipped.append({"run": r.name, "reasons": reasons})
        else:
            runs.append(r)
    trials = e1_fit.load_trials(runs)
    for r in runs:
        work_figure(r.name, [t for t in trials if t["run"] == r.name], out / f"work_onset_{r.name}.png")
    result = {"schema": "afterglow.m1gate/1", "threshold": THRESHOLD, "gate_calm_ms": GATE_CALM_MS,
              "runs": [r.name for r in runs], "skipped_invalid": skipped, "devices": {}}
    for device in sorted({t["device"] for t in trials}):
        mine = [t for t in trials if t["device"] == device]
        table, passes = [], {}
        for p in ("gpu", "cpu"):
            sel = [t for t in mine if t["target"] == PROC_TARGET[p]]
            for calm in sorted({t["calm_ms"] for t in sel}):
                med, ci = boot_ci([penalty(t, p) for t in sel if t["calm_ms"] == calm], a.boot, rng)
                table.append({"processor": p, "calm_ms": calm, "n": sum(t["calm_ms"] == calm for t in sel),
                              "P_median": med, "P_ci95": list(ci)})
            gate = [r for r in table if r["processor"] == p and r["calm_ms"] == GATE_CALM_MS]
            passes[p] = bool(gate and gate[0]["P_median"] >= THRESHOLD and gate[0]["P_ci95"][0] > 1.0)
        kappa_figure(device, mine, out / f"kappa_onset_{device}.png")
        penalty_figure(device, table, out / f"penalty_{device}.png")
        result["devices"][device] = {"family": "apple" if device in APPLE else ("android" if device in ANDROID else "other"),
                                     "penalty": table, "pass_gpu": passes.get("gpu", False),
                                     "pass_cpu": passes.get("cpu", False),
                                     "pass": passes.get("gpu", False) or passes.get("cpu", False)}
        for r in table:
            print(f"{device} {r['processor']} T_calm={r['calm_ms'] / 1000:g}s n={r['n']}: P={r['P_median']:.3f} "
                  f"[{r['P_ci95'][0]:.3f}, {r['P_ci95'][1]:.3f}]")
        print(f"{device}: {'PASS' if result['devices'][device]['pass'] else 'no pass'}")
    fams = {d["family"] for d in result["devices"].values() if d["pass"]}
    result["go"] = "apple" in fams and "android" in fams
    result["verdict"] = ("GO criterion met" if result["go"] else
                         "GO criterion not met yet: needs a passing Apple and a passing Android device "
                         f"(passing families so far: {sorted(fams) or 'none'})")
    (out / "m1_gate.json").write_text(json.dumps(result, indent=1))
    print(result["verdict"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
