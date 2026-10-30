"""fig_excel builds one workbook per figure from tiny CSV sidecars: three sheets, one chart per panel, log axes and
custom error bars where the paper figure has them."""
import csv
import shutil
import tempfile
import unittest
import zipfile
from pathlib import Path

from openpyxl import load_workbook

import fig_excel

CALMS = [50, 100, 250, 500, 1000, 2000, 4000, 8000]


def write(path, header, rows):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)


class FigExcel(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="ag_figxl_"))
        devs, procs = ("ip12", "zz"), ("gpu", "cpu")
        write(self.tmp / "kappa0_calm.csv", ["device", "processor", "calm_s", "kappa0", "decay_fit_drawn"],
              [[d, p, c / 1000, 0.5, "yes"] for d in devs for p in procs for c in CALMS])
        ks = [f"k_{t / 10:.2f}" for t in range(21)]
        write(self.tmp / "kappa_class_medians.csv", ["device", "processor", "class", "n_trials", "share"] + ks,
              [[d, p, c, 10, 0.5] + [0.8] * 20 + ["nan"] for d in devs for p in procs for c in ("warmup", "flat")])
        write(self.tmp / "onset_penalty_cells.csv",
              ["device", "processor", "load_pct", "calm_ms", "n_trials", "P_median", "P_ci_lo", "P_ci_hi"],
              [[d, p, L, c, 30, 1.2, 1.1, 1.4] for d in devs for p in procs for L in (25, 50, 80) for c in CALMS])
        hdr = ["device", "gap_us"]
        for m in ("submit_to_start", "gpu", "round_trip"):
            hdr += ["n", f"{m}_med_us", f"{m}_q1_us", f"{m}_q3_us"]
        write(self.tmp / "idlegap_medians.csv", hdr,
              [[d, g] + [20, 100.0, 90.0, 120.0] * 3 for d in devs for g in (100, 1000, 10000)])

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_workbooks(self):
        fig_excel.main(["--tables", str(self.tmp), "--out", str(self.tmp)])
        expect = {"fig_kappa0_calm.xlsx": (2, True, False), "fig_kappa_class.xlsx": (4, False, False),
                  "fig_onset_penalty.xlsx": (4, True, True), "fig_idlegap.xlsx": (3, True, True)}
        for name, (n_charts, log, err) in expect.items():
            p = self.tmp / name
            self.assertEqual(load_workbook(p).sheetnames, ["data", "chart_data", "charts"])
            z = zipfile.ZipFile(p)
            charts = [n for n in z.namelist() if n.startswith("xl/charts/chart")]
            self.assertEqual(len(charts), n_charts, name)
            x = z.read(charts[0]).decode()
            self.assertEqual("logBase" in x, log, name)
            self.assertEqual("errBars" in x, err, name)
        ws = load_workbook(self.tmp / "fig_onset_penalty.xlsx")["chart_data"]
        self.assertAlmostEqual(ws.cell(3, 3).value, 0.1)   # CI minus = P - lo
        self.assertAlmostEqual(ws.cell(3, 4).value, 0.2)   # CI plus = hi - P


if __name__ == "__main__":
    unittest.main()
