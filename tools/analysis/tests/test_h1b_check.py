"""h1b_check.verdict on synthetic trial-level P: the three outcomes of the pre-registered rule."""
import unittest

import numpy as np

import h1b_check

CALMS = [50, 100, 250, 500, 1000, 2000, 4000, 8000]


def cells(levels, spread=0.05, n=30, seed=3):
    rng = np.random.default_rng(seed)
    return {c: lvl + spread * rng.standard_normal(n) for c, lvl in zip(CALMS, levels)}


class VerdictTest(unittest.TestCase):
    def test_supported_exponential_growth(self):
        tau = 0.15
        lv = [2.8 - (2.8 - 1.8) * np.exp(-c / 1000 / tau) for c in CALMS]
        v = h1b_check.verdict(cells(lv), n_boot=200)
        self.assertEqual(v["verdict"], "supported")
        self.assertGreater(v["growth_ci95"][0], 0)
        self.assertGreaterEqual(v["R2"], 0.8)
        self.assertAlmostEqual(v["tau_P_s"], tau, delta=0.08)

    def test_saturated_at_shortest_calm(self):
        v = h1b_check.verdict(cells([2.2] * 8), n_boot=200)
        self.assertTrue(v["verdict"].startswith("not supported: saturated"))
        self.assertIsNone(v["tau_P_s"])

    def test_no_penalty(self):
        v = h1b_check.verdict(cells([0.85] * 8), n_boot=200)
        self.assertEqual(v["verdict"], "no penalty")

    def test_growth_but_not_exponential(self):
        lv = [1.6, 2.0, 2.3, 1.7, 2.4, 2.4, 2.2, 2.1]
        v = h1b_check.verdict(cells(lv, spread=0.02), n_boot=200)
        self.assertTrue(v["verdict"].startswith("not supported: exponential"))


if __name__ == "__main__":
    unittest.main()
