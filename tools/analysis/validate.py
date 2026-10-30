#!/usr/bin/env python3
"""Run validity (docs/04 §2, R7). Applies the rules to every run directory under the given roots and
writes the valid/invalid list with reasons. Invalid runs are kept, excluded, and counted.

Rules:
  1. bad build or provenance: incomplete provenance, dirty tree, Debug build (plus the FMA canary
     and fast-math, which break R1);
  2. interrupted: no DONE file or DONE not ok, status.interrupted, or a frame-to-frame gap > 100 ms;
  3. state mismatch: workload-trace hash differs from the expected one; a requested condition did not
     apply (refresh rate, brightness, charging, Low Power Mode); a run on the charger started below
     90 % battery (docs/04 §1, D3; 70 % on a device whose charge limit is 80 %, D48); thermal state
     above nominal at start;
  4. overheating: thermal state reached serious (Apple) or SEVERE (Android) during the run;
  5. frame-log gaps: more than 0.5 % of frames without a GPU timestamp;
  6. energy runs: NeonSwarm's frozen energy rules E1/E2 (wired in M5; until then an energy run is
     reported invalid rather than silently accepted).

Usage: validate.py <results-root-or-run-dir>... [--out validity.csv]
"""
import argparse
import csv
import sys
from pathlib import Path

from agcsv import NA_ALLOWED, NA_BY_MODE, REQUIRED_PROVENANCE, at_path, read_device, read_frames, read_run

GAP_LIMIT_NS = 100_000_000          # rule 2: display link stopped for more than 100 ms
STARTUP_FRAMES = 60                 # rule 2 ignores the first second (D46)
MISSING_GPU_LIMIT = 0.005           # rule 5: 0.5 % of frames
START_OK = {"nominal", "none"}      # Apple "nominal", Android THERMAL_STATUS_NONE
OVERHEAT = {"serious", "critical", "severe", "emergency", "shutdown"}
BRIGHTNESS_TOL = 1                  # percent
FAST_FRAME_FACTOR = 0.75            # rule 3 (D58): a frame interval < 0.75 x the requested period
FAST_FRAME_MAX_SHARE = 0.01         # ... on more than 1 % of measured frames = the refresh did not hold
MIN_BATTERY_ON_CHARGER = 90         # rule 3: timing runs on the charger start at >= 90 % (D3)
MIN_BATTERY_BY_DEVICE = {"ip12": 70,   # D48: 80 % charge limit on the iPhone 12
                         "a56": 80,    # D61: borrowed A56 (2026-10-13 evening)
                         "ipmini6": 70,  # D63: iPadOS holds the borrowed iPad mini 6 at 80 % (2026-10-16)
                         "fold8": 80,    # D67: fallback if the borrowed Fold 8 holds below 90 % (2026-10-21)
                         "ippro3": 80}   # D72: waiver for the iPad Pro's night leg (2026-10-28)


def _is_na(v):
    return isinstance(v, str) and v.startswith("n/a:")


def provenance_problems(prov):
    platform = at_path(prov, "device.platform")
    allowed = NA_ALLOWED.get(platform, set()) | NA_BY_MODE.get(at_path(prov, "run.mode"), set())
    missing = []
    for key in REQUIRED_PROVENANCE:
        v = at_path(prov, key)
        ok = v is not None and not isinstance(v, (dict, list)) and not (isinstance(v, str) and v == "")
        if ok and _is_na(v) and key not in allowed:
            ok = False
        if not ok:
            missing.append(key)
    if platform not in NA_ALLOWED:
        missing.append(f"device.platform (unknown platform {platform!r})")
    return missing


def check_run(run_dir):
    """Returns a list of reasons; empty means valid."""
    run_dir = Path(run_dir)
    reasons = []
    try:
        prov = read_run(run_dir)
    except (OSError, ValueError) as e:
        return [f"run.json unreadable: {e}"]

    # 1. build and provenance
    missing = provenance_problems(prov)
    if missing:
        reasons.append("provenance incomplete: " + " ".join(missing))
    if at_path(prov, "build.git_dirty") is not False:
        reasons.append("built from a dirty git tree")
    if (at_path(prov, "build.build_type") != "Release" or at_path(prov, "build.optimized") is not True
            or at_path(prov, "build.ndebug") is not True):
        reasons.append("not a Release build")
    if at_path(prov, "build.fma_canary_contracted") is not False:
        reasons.append("FMA canary: CPU core contracts a*b+c")
    if at_path(prov, "build.fast_math") is not False:
        reasons.append("core compiled with fast-math")
    if "MoltenVK" in str(at_path(prov, "gfx.api") or ""):
        reasons.append("MoltenVK: Vulkan translated to Metal, development only, never research data")
    if "simulator" in str(at_path(prov, "device.gpu_name") or "").lower():
        reasons.append("iOS simulator: development only, never research data")

    # 2. interrupted
    done = run_dir / "DONE"
    if not done.exists():
        reasons.append("no DONE file (the run did not finish)")
    elif done.read_text(encoding="utf-8").strip() != "status=ok":
        reasons.append("DONE status is not ok: " + done.read_text(encoding="utf-8").strip())
    if at_path(prov, "status.interrupted") is not False:
        reasons.append("app was backgrounded or interrupted")

    frames = None
    try:
        _, frames = read_frames(run_dir)
    except (OSError, ValueError) as e:
        reasons.append(f"frames.csv unreadable: {e}")
    if frames is not None and len(frames) > 1:
        # Only measured frames count (D46): the warm-up (sb_phase 2) is not analysed (04 §1), and
        # neither is the first second of a run (start-up).
        gaps = frames["t_frame_start_ns"].diff()
        measured = (frames["sb_phase"] != 2) & (frames["frame"] >= STARTUP_FRAMES)
        gaps = gaps[measured].dropna()
        if (gaps > GAP_LIMIT_NS).any():
            reasons.append(f"display link stopped for more than 100 ms ({int(gaps.max() / 1e6)} ms)")
        # The requested refresh rate must hold for the whole run, not only at its start (D58: the Tab
        # on "Tinggi" ran at up to 120 frames/s under a 60 Hz request). Present times where the
        # backend has them, frame starts otherwise.
        hz = (prov.get("requested") or {}).get("display_refresh_hz")
        if isinstance(hz, (int, float)) and not isinstance(hz, bool) and hz > 0:
            pres = frames["present_actual_ns"].astype("Float64")
            pres = pres.where(pres > 0)
            t = pres if pres.notna().sum() > 1 else frames["t_frame_start_ns"].astype("Float64")
            iv = t.diff()[measured].dropna()
            share = float((iv < FAST_FRAME_FACTOR * 1e9 / hz).mean()) if len(iv) else 0.0
            if share > FAST_FRAME_MAX_SHARE:
                reasons.append(f"frames came faster than the requested {hz:g} Hz on {share:.1%} of measured frames")

    # 3. state mismatch
    exp = at_path(prov, "workload.expected_trace_sha256")
    act = at_path(prov, "workload.actual_trace_sha256")
    if exp is not None and not _is_na(exp) and exp != act:
        reasons.append("workload-trace hash differs from the level's expected hash")
    requested = prov.get("requested") or {}
    start = prov.get("state_start") or {}
    end = prov.get("state_end") or {}
    hz_req = requested.get("display_refresh_hz")
    if hz_req is not None and end.get("display_refresh_hz") not in (None, hz_req):
        reasons.append(f"requested display_refresh_hz={hz_req} but it was {end.get('display_refresh_hz')} at the end")
    for key, val in requested.items():
        got = start.get(key)
        if key == "brightness" and isinstance(val, (int, float)) and isinstance(got, (int, float)):
            if abs(got - val) > BRIGHTNESS_TOL:
                reasons.append(f"requested brightness {val} but got {got}")
        elif got != val:
            reasons.append(f"requested {key}={val} but got {got}")
    batt = start.get("battery_percent")
    need = MIN_BATTERY_BY_DEVICE.get(at_path(prov, "device.id"), MIN_BATTERY_ON_CHARGER)
    if requested.get("charging") is True and isinstance(batt, (int, float)) and batt < need:
        reasons.append(f"on the charger at {batt:g} % battery (needs >= {need} %)")
    t0 = str(start.get("thermal_state", "")).lower()
    if t0 not in START_OK and not t0.startswith("n/a"):
        reasons.append(f"thermal state above nominal at start ({t0})")

    # 4. overheating (device.csv every second, plus the end state)
    _, dev = read_device(run_dir)
    states = {str(s).lower() for s in dev.get("thermal_state", [])}
    states.add(str(at_path(prov, "state_end.thermal_state") or "").lower())
    hot = sorted(states & OVERHEAT)
    if hot:
        reasons.append("overheated during the run (" + ", ".join(hot) + ")")

    # 5. frame-log gaps
    if frames is not None and len(frames) > 0:
        frac = frames["gpu_probe_ns"].isna().mean()
        if frac > MISSING_GPU_LIMIT:
            reasons.append(f"{frac * 100:.2f} % of frames have no GPU timestamp (limit 0.5 %)")

    # 6. energy runs
    if at_path(prov, "energy.session") is True:
        reasons.append("energy rules E1/E2 not wired yet (M5): energy run not accepted")
    return reasons


def find_runs(roots):
    for root in roots:
        root = Path(root)
        if (root / "run.json").exists():
            yield root
        else:
            yield from sorted(p.parent for p in root.glob("*/run.json"))


def main(argv=None):
    ap = argparse.ArgumentParser(description="Apply the docs/04 §2 run-validity rules.")
    ap.add_argument("roots", nargs="+")
    ap.add_argument("--out", help="write validity.csv here")
    a = ap.parse_args(argv)
    rows = []
    for run in find_runs(a.roots):
        reasons = check_run(run)
        rows.append((run.name, "valid" if not reasons else "invalid", "; ".join(reasons)))
        print(f"{'VALID  ' if not reasons else 'INVALID'} {run.name}" + (f"  -- {'; '.join(reasons)}" if reasons else ""))
    if a.out:
        with open(a.out, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(["run_id", "status", "reasons"])
            w.writerows(rows)
    n_valid = sum(1 for r in rows if r[1] == "valid")
    print(f"validate: {n_valid} valid, {len(rows) - n_valid} invalid")
    return 0


if __name__ == "__main__":
    sys.exit(main())
