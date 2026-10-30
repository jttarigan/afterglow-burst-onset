"""Smoke tests for the paper figure scripts on tiny synthetic inputs: they run, write PNG + PDF + CSV, and
fig_kappa0_calm's "draw the decay fit" rule matches Table 3 (no calm dependence / non-monotonic / fitted)."""
import csv
import json
import shutil
import tempfile
import unittest
from pathlib import Path

import numpy as np

import fig_idlegap
import fig_kappa0_calm
import fig_onset_penalty

CALMS = [50, 100, 250, 500, 1000, 2000, 4000, 8000]


def proc_block(k0_by_calm, p_level):
    cells = []
    for calm, k0 in zip(CALMS, k0_by_calm):
        for load in (25, 50, 80):
            for target in (1, 2, 3):
                cells.append({"calm_ms": calm, "load_pct": load, "target": target, "n_trials": 30,
                              "kappa0_median": k0 + 0.01 * (load - 50) / 50,
                              "P_median": p_level, "P_ci95": [p_level - 0.1, p_level + 0.1]})
    return {"kappa_ref_ns": 80000.0, "tau_up_s": 0.1, "tau_up_ci95": [0.08, 0.12], "tau_up_identifiable": True,
            "kappa_inf": 0.9, "tau_down_s": 0.05, "tau_down_ci95": [0.03, 0.08], "kappa_floor": min(k0_by_calm),
            "kappa_floor_ci95": [min(k0_by_calm) - 0.01, min(k0_by_calm) + 0.01], "tau_down_direct_s": 0.2,
            "onset_kappa_by_calm": [{"calm_s": c / 1000, "kappa0": k} for c, k in zip(CALMS, k0_by_calm)],
            "cells": cells, "changepoint_classes": {"flat": 100, "warmup": 50}, "n_trials": 1440, "n_boot": 10}


def fit_json(path, dev, gpu_k0, cpu_k0):
    json.dump({"schema": "afterglow.e1fit/1", "runs": [f"{dev}_e1_synthetic"], "devices": [dev],
               "processors": {"gpu": proc_block(gpu_k0, 1.0), "cpu": proc_block(cpu_k0, 2.5)}}, open(path, "w"))
    return str(path)


class Figures(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="ag_figs_"))
        decay = [0.5, 0.4, 0.25, 0.2, 0.2, 0.2, 0.2, 0.2]
        flat = [0.3] * 8
        bumpy = [0.67, 0.8, 0.8, 0.62, 0.62, 0.59, 0.67, 0.67]
        self.fits = [fit_json(self.tmp / "e1_fit_ip12.json", "ip12", flat, decay),
                     fit_json(self.tmp / "e1_fit_zz.json", "zz", [0.97] * 8, bumpy)]   # zz: a device with no style entry

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_decay_rule(self):
        self.assertEqual(fig_kappa0_calm.decay_reported(np.array([0.3] * 8)), (False, "no calm dependence"))
        self.assertEqual(fig_kappa0_calm.decay_reported(np.array([0.67, 0.8, 0.8, 0.62, 0.62, 0.59, 0.67, 0.67])),
                         (False, "non-monotonic"))
        self.assertEqual(fig_kappa0_calm.decay_reported(np.array([0.31, 0.25, 0.2, 0.18, 0.19, 0.18, 0.18, 0.18])),
                         (True, ""))   # a 0.01 wobble is within the tolerance

    def test_kappa0_and_penalty(self):
        out, csvp = self.tmp / "f1.png", self.tmp / "f1.csv"
        fig_kappa0_calm.main(self.fits + ["--out", str(out), "--csv", str(csvp)])
        self.assertTrue(out.exists() and out.with_suffix(".pdf").exists())
        rows = list(csv.DictReader(open(csvp)))
        self.assertEqual(len(rows), 2 * 2 * 8)
        drawn = {(r["device"], r["processor"]): r["decay_fit_drawn"] for r in rows}
        self.assertEqual(drawn, {("ip12", "gpu"): "no calm dependence", ("ip12", "cpu"): "yes",
                                 ("zz", "gpu"): "no drop", ("zz", "cpu"): "non-monotonic"})   # flat near 1 = "no drop"
        out, csvp = self.tmp / "f3.png", self.tmp / "f3.csv"
        fig_onset_penalty.main(self.fits + ["--out", str(out), "--csv", str(csvp)])
        self.assertTrue(out.exists())
        rows = list(csv.DictReader(open(csvp)))
        self.assertEqual(len(rows), 2 * 2 * 3 * 8)     # devices x processors x loads x calms, own target only
        self.assertTrue(all(r["processor"] != "gpu" or float(r["P_median"]) == 1.0 for r in rows))

    def test_idlegap(self):
        runs = []
        for dev in ("ip12", "zz"):
            d = self.tmp / f"{dev}_idlegap_synthetic"
            d.mkdir()
            json.dump({"device": {"id": dev}}, open(d / "run.json", "w"))
            with open(d / "idlegap.csv", "w") as f:
                f.write("# schema=afterglow.idlegap/1\n")
                f.write("idx,gap_us,submit_ns,gpu_start_ns,gpu_end_ns,done_ns,submit_to_start_ns,round_trip_ns,gpu_ns\n")
                i = 0
                for gap in (100, 1000, 100000, 2000000):
                    for k in range(5):
                        s2s, g, rt = 400 + k * 10 + gap // 1000, 80 + k, 800 + k * 10 + gap // 1000
                        f.write(f"{i},{gap},0,{s2s * 1000},{(s2s + g) * 1000},{rt * 1000},{s2s * 1000},{rt * 1000},{g * 1000}\n")
                        i += 1
            runs.append(str(d))
        out, csvp = self.tmp / "f4.png", self.tmp / "f4.csv"
        fig_idlegap.main(runs + ["--out", str(out), "--csv", str(csvp)])
        self.assertTrue(out.exists())
        rows = list(csv.DictReader(open(csvp)))
        self.assertEqual(len(rows), 2 * 4)
        first = next(r for r in rows if r["device"] == "ip12" and r["gap_us"] == "100")
        self.assertEqual(first["n"], "4")               # the first measurement (idx 0, pipeline start) is dropped
        r = next(r for r in rows if r["device"] == "ip12" and r["gap_us"] == "2000000")
        self.assertEqual(r["n"], "5")
        self.assertAlmostEqual(float(r["gpu_med_us"]), 82.0, places=1)


if __name__ == "__main__":
    unittest.main()
