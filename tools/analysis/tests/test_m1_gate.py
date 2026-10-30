"""m1_gate.py: the GO/NO-GO statistic on synthetic pilots with a large and a negligible penalty."""
import json
import shutil
import tempfile
import unittest
from pathlib import Path

import m1_gate
import synth


def pilot(root, device, floor):
    return synth.synth_stepbench(root, tau_up=0.12, tau_down=1.5, kappa_floor=floor, calms_ms=(100, 1000, 4000),
                                 loads=(50,), targets=("gpu", "cpu"), repeats=10, device_id=device,
                                 run_id=f"{device}_e1pilot_all_0_synthetic")


class Gate(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="ag_gate_"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_gate(self):
        m1_gate.main([str(self.tmp / "runs"), "--out", str(self.tmp / "out"), "--boot", "500"])
        return json.loads((self.tmp / "out" / "m1_gate.json").read_text())

    def test_large_penalty_passes_small_does_not(self):
        pilot(self.tmp / "runs", "ip12", 0.45)    # P(4 s) ~ 2.05
        pilot(self.tmp / "runs", "poco", 0.95)    # P(4 s) ~ 1.05: below the 1.15 gate
        g = self.run_gate()
        self.assertTrue(g["devices"]["ip12"]["pass"])
        self.assertFalse(g["devices"]["poco"]["pass"])
        self.assertFalse(g["go"])
        p4 = [r for r in g["devices"]["ip12"]["penalty"] if r["processor"] == "gpu" and r["calm_ms"] == 4000][0]
        self.assertAlmostEqual(p4["P_median"], 1 / (0.45 + 0.55 * 0.0695), delta=0.06)
        self.assertTrue((self.tmp / "out" / "kappa_onset_ip12.png").exists())

    def test_go_needs_apple_and_android(self):
        pilot(self.tmp / "runs", "ip12", 0.45)
        pilot(self.tmp / "runs", "tab", 0.5)
        self.assertTrue(self.run_gate()["go"])


if __name__ == "__main__":
    unittest.main()
