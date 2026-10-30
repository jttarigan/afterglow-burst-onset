"""Shared schema and readers for Afterglow run directories (docs/03 §9).

One schema on every platform: frames.csv, device.csv, run.json, DONE. The column and key lists here
must equal the C++ writers in lab/src/framelog.h and lab/src/provenance.cpp; tests/test_schema.py
checks them against `aglab --mode=schema`.
"""
import json
from pathlib import Path

import pandas as pd

FRAME_SCHEMA = "afterglow.frames/2"   # /2 adds cpu_freq_khz after cpu_id (D45)
FRAME_COLUMNS = [
    "frame", "tick", "ticks_run",
    "t_frame_start_ns", "cpu_probe_ns", "cpu_sim_ns", "cpu_forecast_ns", "cpu_encode_ns",
    "gpu_probe_ns", "gpu_main_ns", "gpu_heater_ns",
    "present_target_ns", "present_actual_ns", "late",
    "live_blasts", "detonations", "live_sparks", "smoke_on",
    "policy_state", "heater_gpu_us", "heater_cpu_us", "hint_calls",
    "cpu_id", "cpu_freq_khz",
    "sb_trial", "sb_phase", "sb_calm_ms", "sb_load_pct", "sb_target",
    "gpu_probe_start_ns",
]

DEVICE_SCHEMA = "afterglow.device/1"
DEVICE_COLUMNS = ["t_ns", "thermal_state", "battery_pct", "charging", "current_ua", "voltage_mv",
                  "brightness_pct", "low_power_mode"]

# Stepbench markers (D25). In stepbench, cpu_sim_ns holds the synthetic CPU load and gpu_main_ns
# the synthetic GPU load.
SB_PHASE = {0: "calm", 1: "burst", 2: "warmup"}
SB_TARGET = {1: "gpu", 2: "cpu", 3: "both"}

REQUIRED_PROVENANCE = [
    "schema", "run.mode", "run.run_id", "run.started_utc", "run.ended_utc",
    "device.id", "device.platform", "device.model", "device.soc", "device.os", "device.os_build",
    "build.git_commit", "build.git_dirty", "build.build_type", "build.compiler", "build.cxx_flags",
    "build.fma_canary_contracted", "build.fast_math",
    "state_start.thermal_state", "state_start.battery_percent", "state_start.charging",
    "state_start.power_source", "state_start.low_power_mode", "state_start.display_refresh_hz",
    "state_start.brightness",
    "state_end.thermal_state", "state_end.battery_percent", "state_end.charging",
    "state_end.low_power_mode",
    "workload.preset", "workload.policy", "workload.level", "workload.seed",
    "input.path", "os_game.app_category", "os_game.game_mode",
    "status.done", "status.interrupted",
]

# Keys a platform may report as "n/a: <reason>". macOS is the headless lab runner; the device
# lists hold only what the platform has no public API for.
NA_ALLOWED = {
    "macOS": {
        "device.id",
        "state_start.battery_percent", "state_start.charging", "state_start.display_refresh_hz",
        "state_start.brightness", "state_end.battery_percent", "state_end.charging",
        "workload.preset", "workload.policy", "workload.level", "workload.seed",
        "input.path", "os_game.app_category", "os_game.game_mode",
    },
    "iOS": {"os_game.game_mode"},          # no public API reports whether Game Mode is active
    "Android": set(),
}
HEADLESS_NA = sorted(NA_ALLOWED["macOS"] - {"device.id"})
# Keys a mode has no value for, on any platform: these modes run synthetic loads or self-checks,
# not a level (D27).
NOLEVEL_MODES = ["stepbench", "smoke", "e0", "idlegap"]
NOLEVEL_NA = {"workload.preset", "workload.level"}
NA_BY_MODE = {m: NOLEVEL_NA for m in NOLEVEL_MODES}


def _meta_and_skip(path):
    meta, skip = {}, 0
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            if not line.startswith("#"):
                break
            skip += 1
            body = line[1:].strip()
            if "=" in body:
                k, v = body.split("=", 1)
                meta[k.strip()] = v.strip()
    return meta, skip


def read_csv(path):
    """Returns (meta, DataFrame). Empty fields are NaN."""
    meta, skip = _meta_and_skip(path)
    df = pd.read_csv(path, skiprows=skip)
    return meta, df


FRAME_COLUMNS_V1 = [c for c in FRAME_COLUMNS if c != "cpu_freq_khz"]


def read_frames(run_dir):
    """frames.csv in the current schema; version 1 files (no cpu_freq_khz) are upgraded on read."""
    meta, df = read_csv(Path(run_dir) / "frames.csv")
    schema = meta.get("schema")
    if schema == "afterglow.frames/1" and list(df.columns) == FRAME_COLUMNS_V1:
        df.insert(FRAME_COLUMNS.index("cpu_freq_khz"), "cpu_freq_khz", float("nan"))
        return meta, df
    if schema != FRAME_SCHEMA:
        raise ValueError(f"{run_dir}: frames.csv schema {schema!r} is not {FRAME_SCHEMA}")
    if list(df.columns) != FRAME_COLUMNS:
        raise ValueError(f"{run_dir}: frames.csv columns differ from the schema")
    return meta, df


def read_device(run_dir):
    path = Path(run_dir) / "device.csv"
    if not path.exists():
        return {}, pd.DataFrame(columns=DEVICE_COLUMNS)
    return read_csv(path)


def read_run(run_dir):
    with open(Path(run_dir) / "run.json", "r", encoding="utf-8") as f:
        return json.load(f)


def at_path(obj, dotted):
    cur = obj
    for key in dotted.split("."):
        if not isinstance(cur, dict) or key not in cur:
            return None
        cur = cur[key]
    return cur


def write_csv(path, schema, meta, columns, rows):
    """Writes a schema'd CSV like lab/src/runio.cpp (used by the synthetic generators)."""
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# schema={schema}\n")
        for k, v in meta.items():
            f.write(f"# {k}={v}\n")
        f.write(",".join(columns) + "\n")
        for r in rows:
            f.write(",".join("" if v is None else str(v) for v in r) + "\n")
