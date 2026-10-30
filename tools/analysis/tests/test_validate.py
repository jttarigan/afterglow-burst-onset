"""validate.py: every docs/04 §2 rule fires on a run that breaks it, and only then."""
import json
import shutil
import tempfile
import unittest
from pathlib import Path

import synth
import validate


class Rules(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="ag_validate_"))
        cls.base = synth.synth_stepbench(cls.tmp / "base", tau_up=0.1, tau_down=1.0, kappa_floor=0.5,
                                         calms_ms=(250, 1000), loads=(50,), targets=("gpu",), repeats=2,
                                         run_id="base_run")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def variant(self, name, edit_prov=None, edit_files=None):
        d = self.tmp / name
        shutil.copytree(self.base, d)
        if edit_prov:
            p = json.loads((d / "run.json").read_text())
            edit_prov(p)
            (d / "run.json").write_text(json.dumps(p))
        if edit_files:
            edit_files(d)
        return validate.check_run(d)

    def assertFlags(self, reasons, text):
        self.assertTrue(any(text in r for r in reasons), f"expected '{text}' in {reasons}")

    def test_base_run_is_valid(self):
        self.assertEqual(validate.check_run(self.base), [])

    def test_rule1_build_and_provenance(self):
        self.assertFlags(self.variant("dirty", lambda p: p["build"].update(git_dirty=True)), "dirty git tree")
        self.assertFlags(self.variant("debug", lambda p: p["build"].update(build_type="Debug")), "not a Release build")
        self.assertFlags(self.variant("fma", lambda p: p["build"].update(fma_canary_contracted=True)), "FMA canary")
        self.assertFlags(self.variant("nokey", lambda p: p["device"].pop("soc")), "provenance incomplete: device.soc")
        self.assertFlags(self.variant("mvk", lambda p: p.update(gfx={"api": "Vulkan (MoltenVK, development only)"})),
                         "MoltenVK")
        self.assertFlags(self.variant("sim", lambda p: p["device"].update(gpu_name="Apple iOS simulator GPU")),
                         "simulator")
        # n/a only where the platform or mode cannot report the value
        self.assertFlags(self.variant("na", lambda p: p["state_start"].update(brightness="n/a: x")),
                         "state_start.brightness")
        self.assertFlags(self.variant("na_level", lambda p: p["run"].update(mode="research")), "workload.level")

    def test_rule2_interrupted(self):
        self.assertFlags(self.variant("nodone", edit_files=lambda d: (d / "DONE").unlink()), "no DONE file")
        self.assertFlags(self.variant("donefail", edit_files=lambda d: (d / "DONE").write_text("status=crash\n")),
                         "DONE status is not ok")
        self.assertFlags(self.variant("bg", lambda p: p["status"].update(interrupted=True)), "interrupted")

        def stall(d):
            lines = (d / "frames.csv").read_text().splitlines()
            hdr = next(i for i, l in enumerate(lines) if l.startswith("frame,"))
            row = lines[hdr + 150].split(",")          # a measured frame (past the first second, D46)
            row[3] = str(int(row[3]) - 200_000_000)   # pull one frame 200 ms back: a 200+ ms gap after it
            lines[hdr + 150] = ",".join(row)
            (d / "frames.csv").write_text("\n".join(lines) + "\n")

        self.assertFlags(self.variant("stall", edit_files=stall), "display link stopped")

        def startup_stall(d):   # the same gap in the first second is not counted (D46)
            lines = (d / "frames.csv").read_text().splitlines()
            hdr = next(i for i, l in enumerate(lines) if l.startswith("frame,"))
            row = lines[hdr + 5].split(",")
            row[3] = str(int(row[3]) - 200_000_000)
            lines[hdr + 5] = ",".join(row)
            (d / "frames.csv").write_text("\n".join(lines) + "\n")

        self.assertEqual(self.variant("startup_stall", edit_files=startup_stall), [])

    def test_rule3_state_mismatch(self):
        self.assertFlags(self.variant("hash", lambda p: p["workload"].update(expected_trace_sha256="a" * 64,
                                                                             actual_trace_sha256="b" * 64)),
                         "workload-trace hash differs")
        self.assertFlags(self.variant("hz", lambda p: p["state_start"].update(display_refresh_hz=120)),
                         "requested display_refresh_hz=60 but got 120")
        self.assertFlags(self.variant("bright", lambda p: p["state_start"].update(brightness=55)),
                         "requested brightness")
        self.assertFlags(self.variant("lpm", lambda p: p["state_start"].update(low_power_mode=True)),
                         "requested low_power_mode")
        self.assertFlags(self.variant("warm", lambda p: p["state_start"].update(thermal_state="fair")),
                         "thermal state above nominal at start")
        self.assertFlags(self.variant("hz_end", lambda p: p["state_end"].update(display_refresh_hz=120)),
                         "it was 120 at the end")

        def fast(frac):
            def f(d):
                lines = (d / "frames.csv").read_text().splitlines()
                hdr = next(i for i, l in enumerate(lines) if l.startswith("frame,"))
                rows = lines[hdr + 1:]
                col = lines[hdr].split(",").index("t_frame_start_ns")
                n = len(rows)
                k = int(n * frac)                   # the last k frames at 120 Hz instead of 60
                t0 = int(rows[n - k - 1].split(",")[col])
                for j, i in enumerate(range(n - k, n)):
                    cells = rows[i].split(",")
                    cells[col] = str(t0 + (j + 1) * 8_333_333)
                    rows[i] = ",".join(cells)
                (d / "frames.csv").write_text("\n".join(lines[:hdr + 1] + rows) + "\n")
            return f

        self.assertFlags(self.variant("hz_fast", edit_files=fast(0.05)), "frames came faster than the requested 60 Hz")
        self.assertEqual(self.variant("hz_fast_ok", edit_files=fast(0.005)), [])

        self.assertEqual(self.variant("bright_ok", lambda p: p["state_start"].update(brightness=51)), [])
        self.assertFlags(self.variant("batt", lambda p: p["state_start"].update(battery_percent=65)),
                         "on the charger at 65 % battery")
        self.assertEqual(self.variant("batt_ok", lambda p: p["state_start"].update(battery_percent=90)), [])

        def ip12(pct):
            return lambda p: (p["state_start"].update(battery_percent=pct), p["device"].update(id="ip12"))
        self.assertEqual(self.variant("batt_ip12_ok", ip12(70)), [])
        self.assertFlags(self.variant("batt_ip12", ip12(69)), "on the charger at 69 % battery (needs >= 70 %)")

    def test_rule4_overheating(self):
        def hot(d):
            text = (d / "device.csv").read_text().replace(",nominal,", ",serious,", 1)
            (d / "device.csv").write_text(text)

        self.assertFlags(self.variant("hot", edit_files=hot), "overheated")
        self.assertFlags(self.variant("hot_android", lambda p: p["state_end"].update(thermal_state="SEVERE")),
                         "overheated")

    def test_rule5_frame_log_gaps(self):
        def drop(frac):
            def f(d):
                lines = (d / "frames.csv").read_text().splitlines()
                hdr = next(i for i, l in enumerate(lines) if l.startswith("frame,"))
                rows = lines[hdr + 1:]
                for i in range(int(len(rows) * frac)):
                    cells = rows[i].split(",")
                    cells[8] = ""                       # gpu_probe_ns
                    rows[i] = ",".join(cells)
                (d / "frames.csv").write_text("\n".join(lines[:hdr + 1] + rows) + "\n")
            return f

        self.assertFlags(self.variant("gaps", edit_files=drop(0.01)), "no GPU timestamp")
        self.assertEqual(self.variant("gaps_ok", edit_files=drop(0.004)), [])

    def test_rule6_energy_runs_not_silently_accepted(self):
        self.assertFlags(self.variant("energy", lambda p: p.update(energy={"session": True})), "energy rules")


if __name__ == "__main__":
    unittest.main()
