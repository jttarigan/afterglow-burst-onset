"""M0 acceptance: e1_fit.py recovers synthetic tau values within 5 % (docs/05 M0)."""
import shutil
import tempfile
import unittest

import numpy as np

import e1_fit
import synth

TOLERANCE = 0.05
# (tau_up s, tau_down s, kappa_floor): a mid case, slow ramp + fast decay, fast ramp + slow decay.
TRUTHS = [(0.12, 1.5, 0.45), (0.30, 0.6, 0.6), (0.05, 3.0, 0.35)]


class RecoversKnownTau(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="ag_e1fit_")
        cls.results = {}
        for tu, td, fl in TRUTHS:
            run = synth.synth_stepbench(cls.tmp, tau_up=tu, tau_down=td, kappa_floor=fl, seed=7,
                                        run_id=f"synthetic_e1_{tu}_{td}")
            trials = e1_fit.load_trials([run])
            cls.results[(tu, td, fl)] = {p: e1_fit.analyse(trials, p, n_boot=20, seed=3) for p in ("gpu", "cpu")}

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_tau_within_5_percent(self):
        for (tu, td, fl), per_proc in self.results.items():
            for p, r in per_proc.items():
                with self.subTest(truth=(tu, td, fl), processor=p):
                    self.assertLess(abs(r["tau_up_s"] - tu) / tu, TOLERANCE)
                    self.assertLess(abs(r["tau_down_s"] - td) / td, TOLERANCE)
                    self.assertLess(abs(r["tau_down_direct_s"] - td) / td, TOLERANCE)
                    self.assertTrue(r["tau_up_identifiable"])

    def test_floor_relative_to_warm(self):
        # kappa is relative to the fastest probe of the session, so compare floor / k_inf.
        for (tu, td, fl), per_proc in self.results.items():
            for p, r in per_proc.items():
                with self.subTest(truth=(tu, td, fl), processor=p):
                    self.assertAlmostEqual(r["kappa_floor"] / r["kappa_inf"], fl, delta=0.02)

    def test_bootstrap_intervals_are_ordered(self):
        for per_proc in self.results.values():
            for r in per_proc.values():
                lo, hi = r["tau_up_ci95"]
                self.assertLessEqual(lo, hi)
                lo, hi = r["tau_down_ci95"]
                self.assertLessEqual(lo, hi)

    def test_onset_penalty_grows_with_calm(self):
        r = self.results[TRUTHS[0]]["gpu"]
        by_calm = {}
        for c in r["cells"]:
            by_calm.setdefault(c["calm_ms"], []).append(c["P_median"])
        med = [np.median(by_calm[c]) for c in sorted(by_calm)]
        self.assertGreater(med[-1], 1.8)          # 1 / 0.45 at full decay
        self.assertLess(med[0], 1.1)              # 50 ms: still warm
        self.assertTrue(all(b >= a - 0.02 for a, b in zip(med, med[1:])))


class Changepoints(unittest.TestCase):
    def test_classes(self):
        rng = np.random.default_rng(1)
        noise = rng.normal(0, 0.01, 120)
        self.assertEqual(e1_fit.classify(1.0 + noise), "flat")
        self.assertEqual(e1_fit.classify(np.r_[np.full(30, 0.5), np.full(90, 1.0)] + noise), "warmup")
        self.assertEqual(e1_fit.classify(np.r_[np.full(30, 1.0), np.full(90, 0.6)] + noise), "slowdown")
        self.assertEqual(e1_fit.classify(np.r_[np.full(110, 1.0), np.full(10, 0.6)] + noise), "no_steady_state")


if __name__ == "__main__":
    unittest.main()
