"""The Python schema (agcsv.py) equals the C++ writers (aglab --mode=schema)."""
import os
import subprocess
import unittest
from pathlib import Path

import agcsv

ROOT = Path(__file__).resolve().parents[3]


def aglab():
    p = os.environ.get("AGLAB") or str(ROOT / "build" / "mac-release" / "aglab")
    return p if Path(p).exists() else None


@unittest.skipIf(aglab() is None, "aglab not built (set AGLAB)")
class SchemaMatchesCpp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        out = subprocess.run([aglab(), "--mode=schema"], check=True, capture_output=True, text=True).stdout
        cls.lines = dict(line.split(" ", 1) for line in out.strip().splitlines())

    def test_frames(self):
        self.assertEqual(self.lines[agcsv.FRAME_SCHEMA].split(","), agcsv.FRAME_COLUMNS)

    def test_reads_version_1_frames(self):
        import tempfile
        from pathlib import Path
        d = Path(tempfile.mkdtemp())
        agcsv.write_csv(d / "frames.csv", "afterglow.frames/1", {}, agcsv.FRAME_COLUMNS_V1,
                        [[0] * len(agcsv.FRAME_COLUMNS_V1)])
        _, df = agcsv.read_frames(d)
        self.assertEqual(list(df.columns), agcsv.FRAME_COLUMNS)

    def test_device(self):
        self.assertEqual(self.lines[agcsv.DEVICE_SCHEMA].split(","), agcsv.DEVICE_COLUMNS)

    def test_provenance_keys(self):
        self.assertEqual(self.lines["provenance.required"].split(","), agcsv.REQUIRED_PROVENANCE)
        self.assertEqual(sorted(self.lines["provenance.headless_na"].split(",")), agcsv.HEADLESS_NA)
        self.assertEqual(set(self.lines["provenance.nolevel_na"].split(",")), agcsv.NOLEVEL_NA)
        self.assertEqual(self.lines["provenance.nolevel_modes"].split(","), agcsv.NOLEVEL_MODES)


if __name__ == "__main__":
    unittest.main()
