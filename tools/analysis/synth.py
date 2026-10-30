#!/usr/bin/env python3
"""Synthetic stepbench runs with known clock dynamics (docs/04 E1), in the real run-directory schema.

The clock index follows the first-order model of docs/01 §4 exactly:
  calm:  kappa(t) = floor + (kappa_end - floor) * exp(-t / tau_down)   (t since the calm began)
  burst: kappa(t) = 1 - (1 - kappa_onset) * exp(-t / tau_up)          (t since the onset)
A probe that takes probe_warm_ns at kappa = 1 takes probe_warm_ns / kappa, times lognormal noise.
CPU and GPU share one clock here. The default cell design is E1's: 8 calm lengths x 3 burst sizes x
3 targets x 15 repeats. e1_fit.py must recover tau_up and tau_down within 5 % (M0 acceptance);
validate.py's tests mutate these runs to trigger each invalidity rule.
"""
import argparse
import json
from pathlib import Path

import numpy as np
import pandas as pd

from agcsv import DEVICE_COLUMNS, DEVICE_SCHEMA, FRAME_COLUMNS, FRAME_SCHEMA, write_csv

TARGET_CODE = {"gpu": 1, "cpu": 2, "both": 3}
E1_CALMS_MS = (50, 100, 250, 500, 1000, 2000, 4000, 8000)


def provenance(run_id, device_id, extra=None):
    """A complete device-style provenance block (platform iOS, stepbench mode), marked synthetic."""
    p = {
        "schema": "afterglow.run/1",
        "synthetic": True,
        "run": {"mode": "stepbench", "run_id": run_id, "started_utc": "2026-10-06T00:00:00Z",
                "ended_utc": "2026-10-06T00:10:00Z", "args": []},
        "device": {"id": device_id, "platform": "iOS", "model": "iPhone13,2", "soc": "A14", "os": "iOS 26.0",
                   "os_build": "23A000"},
        "build": {"git_commit": "0" * 40, "git_dirty": False, "build_type": "Release", "compiler": "synthetic",
                  "cxx_flags": "-O3 -ffp-contract=off -fno-fast-math", "fma_canary_contracted": False,
                  "fast_math": False, "optimized": True, "ndebug": True},
        "state_start": {"thermal_state": "nominal", "battery_percent": 95, "charging": True,
                        "power_source": "AC", "low_power_mode": False, "display_refresh_hz": 60,
                        "brightness": 50},
        "state_end": {"thermal_state": "nominal", "battery_percent": 96, "charging": True,
                      "low_power_mode": False},
        "requested": {"display_refresh_hz": 60, "brightness": 50, "charging": True, "low_power_mode": False},
        "workload": {"preset": "n/a: stepbench runs synthetic loads", "policy": "P0",
                     "level": "n/a: stepbench runs synthetic loads", "seed": 1,
                     "expected_trace_sha256": "n/a: stepbench", "actual_trace_sha256": "n/a: stepbench"},
        "input": {"path": "synthetic"},
        "os_game": {"app_category": "game", "game_mode": "n/a: no public API reports Game Mode"},
        "status": {"done": True, "interrupted": False, "invalid_reasons": []},
    }
    if extra:
        p.update(extra)
    return p


def synth_stepbench(out_root, *, tau_up, tau_down, kappa_floor, calms_ms=E1_CALMS_MS, loads=(25, 50, 80),
                    targets=("gpu", "cpu", "both"), repeats=15, cv=0.02, seed=1, probe_warm_ns=100_000,
                    burst_s=2.0, fps=60, device_id="synthetic", run_id=None):
    """Writes one run directory and returns its path."""
    rng = np.random.default_rng(seed)
    run_id = run_id or f"{device_id}_e1_synthetic_{seed:03d}_20261006T000000Z"
    out = Path(out_root) / run_id
    out.mkdir(parents=True, exist_ok=True)
    dt = 1.0 / fps
    budget_ns = 1e9 / fps
    n_burst = int(round(burst_s * fps))

    order = [(c, l, t) for c in calms_ms for l in loads for t in targets for _ in range(repeats)]
    rng.shuffle(order)

    kap, phase, trial, calm, load, target = [], [], [], [], [], []
    k_end = 1.0   # warm after the warm-up
    for i, (c, l, t) in enumerate(order):
        n_calm = int(round(c / 1000.0 * fps))
        kc = kappa_floor + (k_end - kappa_floor) * np.exp(-(np.arange(n_calm) * dt) / tau_down)
        k_on = kappa_floor + (k_end - kappa_floor) * np.exp(-(n_calm * dt) / tau_down)
        kb = 1.0 - (1.0 - k_on) * np.exp(-(np.arange(n_burst) * dt) / tau_up)
        k_end = 1.0 - (1.0 - k_on) * np.exp(-(n_burst * dt) / tau_up)
        n = n_calm + n_burst
        kap.append(np.concatenate([kc, kb]))
        phase.append(np.r_[np.zeros(n_calm, dtype=np.int64), np.ones(n_burst, dtype=np.int64)])
        trial.append(np.full(n, i))
        calm.append(np.full(n, c))
        load.append(np.full(n, l))
        target.append(np.full(n, TARGET_CODE[t]))
    kap, phase, trial = np.concatenate(kap), np.concatenate(phase), np.concatenate(trial)
    calm, load, target = np.concatenate(calm), np.concatenate(load), np.concatenate(target)
    n = len(kap)

    def noisy(base):
        return np.rint(base / kap * rng.lognormal(0.0, cv, n)).astype(np.int64)

    def masked(values, mask):
        arr = pd.array(values, dtype="Int64")
        arr[~mask] = pd.NA
        return arr

    burst = phase == 1
    work = load / 100.0 * budget_ns
    df = pd.DataFrame({c: pd.array([pd.NA] * n, dtype="Int64") for c in FRAME_COLUMNS})
    df["frame"] = np.arange(n)
    df["tick"] = np.arange(n)
    df["ticks_run"] = 1
    df["t_frame_start_ns"] = np.rint(np.arange(n) * dt * 1e9).astype(np.int64)
    df["gpu_probe_ns"] = noisy(probe_warm_ns)
    df["cpu_probe_ns"] = noisy(0.3 * probe_warm_ns)
    df["gpu_main_ns"] = masked(noisy(work), burst & np.isin(target, (1, 3)))
    df["cpu_sim_ns"] = masked(noisy(work), burst & np.isin(target, (2, 3)))
    for col in ("late", "policy_state", "heater_gpu_us", "heater_cpu_us", "hint_calls"):
        df[col] = 0
    df["sb_trial"], df["sb_phase"], df["sb_calm_ms"] = trial, phase, calm
    df["sb_load_pct"], df["sb_target"] = load, target
    df = df.astype("Int64")

    with open(out / "frames.csv", "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# schema={FRAME_SCHEMA}\n# run_id={run_id}\n# synthetic=true\n")
        df.to_csv(f, index=False)
    t_end = int(df["t_frame_start_ns"].iloc[-1])
    dev_rows = [[i * 1_000_000_000, "nominal", 95, 1, None, None, 50, 0] for i in range(t_end // 1_000_000_000 + 1)]
    write_csv(out / "device.csv", DEVICE_SCHEMA, {"run_id": run_id}, DEVICE_COLUMNS, dev_rows)
    truth = {"tau_up_s": tau_up, "tau_down_s": tau_down, "kappa_floor": kappa_floor, "cv": cv, "seed": seed}
    with open(out / "run.json", "w", encoding="utf-8") as f:
        json.dump(provenance(run_id, device_id, {"synthetic_truth": truth}), f, indent=1)
    (out / "DONE").write_text("status=ok\n", encoding="utf-8")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--tau-up", type=float, default=0.12)
    ap.add_argument("--tau-down", type=float, default=1.5)
    ap.add_argument("--floor", type=float, default=0.45)
    ap.add_argument("--repeats", type=int, default=15)
    ap.add_argument("--cv", type=float, default=0.02)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    print(synth_stepbench(a.out, tau_up=a.tau_up, tau_down=a.tau_down, kappa_floor=a.floor, repeats=a.repeats,
                          cv=a.cv, seed=a.seed))


if __name__ == "__main__":
    main()
