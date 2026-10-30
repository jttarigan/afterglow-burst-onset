#!/usr/bin/env python3
"""Resumable, shuffled device matrix (docs/05 M1, docs/04 §1-§2). Pattern after
NeonReactor/scripts/run_device_matrix.sh (resume, shuffle) and NeonTide's run_e1.sh.

  tools/device/run_matrix.py tools/device/campaigns/<campaign>.json --device ip12 [--dry-run]

For one campaign on one device:
  - the runs are the campaign's cells x repeats; a stepbench design can instead be split into runs
    of `trials_per_run` trials of the same shuffled trial list (all runs share --seed, so the
    slices partition it);
  - run order is shuffled with the campaign seed (logged);
  - before each run: at least `cooldown_s` since the last run on the device, and no other
    session's device launch aimed at the same device;
  - each run goes through run_ios.sh / run_android.sh (build and install only on the first);
  - each pulled run is validated (validate.py); invalid runs are re-queued at most `max_requeue`
    times, then reported as missing (docs/04 §2);
  - state lives in results/campaigns/<campaign>_<device>.json, so a stopped campaign resumes.
"""
import argparse
import json
import math
import os
import random
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "analysis"))
import validate  # noqa: E402

IOS = {d: os.environ.get(f"AG_UDID_{d.upper()}", "") for d in ("ip12", "ipmini6", "ippro3", "ks")}  # device UDIDs from the environment
ANDROID = {"poco", "tab", "a56", "s6lite", "fold8"}


def arg_value(args, key, default=None):
    for a in args:
        if a.startswith(f"--{key}="):
            return a.split("=", 1)[1]
    return default


def trial_count(args):
    n = 1
    for key, default in (("calms", "50,100,250,500,1000,2000,4000,8000"), ("loads", "25,50,80"),
                         ("targets", "gpu,cpu,both"), ("l0", "probes,ambient")):
        n *= len(arg_value(args, key, default).split(","))
    return n * int(arg_value(args, "repeats", "15"))


def plan(c):
    """The campaign's runs: (cell, rep, extra args)."""
    runs = []
    split = c.get("split", {}).get("trials_per_run")
    if split:
        n = trial_count(c["common_args"])
        for k in range(math.ceil(n / split)):
            a, b = k * split, min(n, (k + 1) * split)
            runs.append({"cell": f"t{a:04d}-{b:04d}", "rep": 0, "args": [f"--trials={a}:{b}"]})
    else:
        for cell in c.get("cells", [{"cell": "all", "args": []}]):
            for rep in range(c.get("repeats", 1)):
                runs.append({"cell": cell["cell"], "rep": rep, "args": cell.get("args", [])})
    return runs


def device_busy(device):
    """Another session's launch aimed at this device (never touch it)."""
    out = subprocess.run(["pgrep", "-fl", "devicectl device process launch|xctrace|run_device_matrix"],
                         capture_output=True, text=True).stdout
    me = str(os.getpid())
    for line in out.splitlines():
        if line.split(" ", 1)[0] == me or "run_matrix.py" in line:
            continue
        if device in IOS and IOS[device] and IOS[device] in line:
            return line[:200]
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("campaign")
    ap.add_argument("--device", required=True)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--wait-busy-s", type=int, default=0, help="wait this long for a busy device, then stop")
    ap.add_argument("--deadline-epoch", type=float, default=0,
                    help="don't start a run that would end after this Unix time (the device's slot ends); exit 4")
    a = ap.parse_args()

    c = json.loads(Path(a.campaign).read_text())
    missing = [k for k in ("campaign", "experiment", "mode", "common_args") if k not in c]
    if missing:
        sys.exit(f"{a.campaign}: campaign spec lacks {', '.join(missing)}")
    if a.device == "ks" and os.environ.get("AG_ALLOW_KS") != "1":
        sys.exit("ks is an optional, borrowed device: set AG_ALLOW_KS=1 to use it")
    if a.device not in IOS and a.device not in ANDROID:
        sys.exit(f"unknown device {a.device}")
    script = ROOT / "tools" / "device" / ("run_ios.sh" if a.device in IOS else "run_android.sh")
    state_path = ROOT / "results" / "campaigns" / f"{c['campaign']}_{a.device}.json"
    state_path.parent.mkdir(parents=True, exist_ok=True)
    state = json.loads(state_path.read_text()) if state_path.exists() else {
        "campaign": c["campaign"], "device": a.device, "seed": c.get("seed", 1), "spec": c, "runs": []}
    if not state["runs"]:
        runs = plan(c)
        random.Random(c.get("seed", 1)).shuffle(runs)
        for r in runs:
            r.update(attempts=[], final=None)
        state["runs"] = runs
    save = lambda: state_path.write_text(json.dumps(state, indent=1))
    save()

    queue = [r for r in state["runs"] if r["final"] is None]
    print(f"{c['campaign']} on {a.device}: {len(state['runs'])} runs, {len(queue)} to do (state {state_path.relative_to(ROOT)})")
    if a.dry_run:
        for r in queue:
            print("  ", r["cell"], r["rep"], " ".join(c["common_args"] + r["args"]))
        return 0

    first = True
    last_end = 0.0
    longest = float(c.get("est_run_s", 900))  # a run's wall time incl. pull; raised to the longest seen
    max_requeue = c.get("max_requeue", 2)
    while queue:
        r = queue.pop(0)
        waited = 0
        while (busy := device_busy(a.device)) is not None:
            if waited >= a.wait_busy_s:
                save()
                sys.exit(f"{a.device} is in use by another session ({busy}); stopping, state saved")
            time.sleep(30)
            waited += 30
        gap = c.get("cooldown_s", 60) - (time.time() - last_end)
        if a.deadline_epoch and time.time() + max(gap, 0) + longest > a.deadline_epoch:
            queue.insert(0, r)
            save()
            print(f"{a.device}: the next run (~{longest / 60:.0f} min) would end after the slot's deadline; "
                  f"stopping, state saved, {len(queue)} runs to do", file=sys.stderr)
            sys.exit(4)
        if last_end > 0 and gap > 0:
            print(f"cool-down {gap:.0f} s")
            time.sleep(gap)
        utc = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        run_id = f"{a.device}_{c['experiment']}_{r['cell']}_{r['rep']}_{utc}"
        env = dict(os.environ, RUN_ID=run_id, EXP=c["experiment"], CELL=r["cell"], REP=str(r["rep"]),
                   TIMEOUT=str(c.get("timeout_s", 1800)))
        env.update({k: str(v) for k, v in c.get("env", {}).items()})   # e.g. REFRESH_HZ=120 (D53)
        if not first:
            env.update(NO_BUILD="1", NO_INSTALL="1")
        cmd = [str(script), a.device, c["mode"]] + c["common_args"] + [f"--seed={c.get('seed', 1)}"] + r["args"]
        print(f"== {run_id}: {' '.join(cmd[3:])}", flush=True)
        t0 = time.time()
        rc = subprocess.run(cmd, env=env).returncode
        longest = max(longest, time.time() - t0) if rc == 0 else longest
        if rc == 3:  # a precondition the user must fix (e.g. the Tab not on Standar, D58): not an attempt
            queue.insert(0, r)
            save()
            sys.exit(f"{a.device}: precondition failed (see above); stopping, state saved, {len(queue)} runs to do")
        last_end = time.time()
        first = False
        run_dir = ROOT / "results" / run_id
        reasons = validate.check_run(run_dir) if run_dir.exists() else [f"no results (script exit {rc})"]
        r["attempts"].append({"run_id": run_id, "script_exit": rc, "valid": not reasons, "reasons": reasons})
        if not reasons:
            r["final"] = "valid"
        elif len(r["attempts"]) <= max_requeue:
            queue.append(r)
        else:
            r["final"] = "missing"
        print(f"   -> {'valid' if not reasons else 'INVALID: ' + '; '.join(reasons)}", flush=True)
        save()
    done = sum(1 for r in state["runs"] if r["final"] == "valid")
    print(f"{c['campaign']} on {a.device}: {done}/{len(state['runs'])} valid")
    return 0


if __name__ == "__main__":
    sys.exit(main())
