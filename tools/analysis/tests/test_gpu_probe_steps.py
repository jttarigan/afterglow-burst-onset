"""gpu_probe_steps.run_steps: a constant-clock run has no fast frames; injected fast frames come back as clusters."""
import shutil
import tempfile
import unittest
from pathlib import Path

import pandas as pd

import gpu_probe_steps
import synth


class Steps(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="ag_probe_steps_"))
        cls.base = synth.synth_stepbench(cls.tmp / "base", tau_up=0.1, tau_down=1.0, kappa_floor=0.5,
                                         calms_ms=(250, 1000), loads=(50,), targets=("gpu",), repeats=2,
                                         run_id="base_run")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_constant_clock(self):
        row, clusters = gpu_probe_steps.run_steps(self.base)
        self.assertTrue(row["valid"])
        self.assertEqual(row["fast_frames"], 0)
        self.assertEqual(clusters, [])

    def test_injected_steps(self):
        d = self.tmp / "fast"
        shutil.copytree(self.base, d)
        f = d / "frames.csv"
        head = [ln for ln in f.read_text().splitlines(keepends=True) if ln.startswith("#")]
        df = pd.read_csv(f, comment="#", dtype="Int64")
        fast = list(range(200, 230)) + [300]
        df.loc[fast, "gpu_probe_ns"] = int(df["gpu_probe_ns"].median() * 3 // 4)
        with open(f, "w", encoding="utf-8", newline="\n") as out:
            out.writelines(head)
            df.to_csv(out, index=False)
        row, clusters = gpu_probe_steps.run_steps(d)
        self.assertEqual(row["fast_frames"], 31)
        self.assertAlmostEqual(row["clock_ratio_fast"], 4 / 3, delta=0.05)
        self.assertEqual([(c["first_frame"], c["last_frame"], c["fast_frames"]) for c in clusters],
                         [(200, 229, 30), (300, 300, 1)])


if __name__ == "__main__":
    unittest.main()
