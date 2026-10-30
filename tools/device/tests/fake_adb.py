#!/usr/bin/env python3
"""Fake adb for testing run_android.sh's wait loop on the Mac, with no device (test_adb_drop.sh).

State lives in $SIM_STATE: `am start` writes the launch time t0. Behaviour, in seconds after t0:
  SIM_RUN_LEN    the app logs "AGLAB DONE exit=0" and exits (default 30);
  SIM_CRASH_AT   the app dies without DONE;
  SIM_DROP_FROM, SIM_DROP_TO   adb does not answer (as a wireless debugging drop, 2026-10-19): commands fail at
                 once, except `logcat`, which waits for the device to come back, as the real one does (2026-10-21).
A pull after DONE writes run.json, an earlier one only partial.txt (the half-done pull of 10-19).
"""
import os
import sys
import time

st = os.environ["SIM_STATE"]
a = sys.argv[1:]
t0f = os.path.join(st, "t0")


def now():
    t = time.time() - float(open(t0f).read()) if os.path.exists(t0f) else -1.0
    dropped = t >= 0 and float(os.environ.get("SIM_DROP_FROM", 1e9)) <= t < float(os.environ.get("SIM_DROP_TO", 1e9))
    return t, dropped


t, dropped = now()
with open(os.path.join(st, "adb_calls.log"), "a") as log:
    log.write(f"t={t:6.1f} dropped={int(dropped)} {' '.join(a)}\n")
run_len = float(os.environ.get("SIM_RUN_LEN", 30))
crash = float(os.environ.get("SIM_CRASH_AT", 1e9))

if a[:2] == ["devices", "-l"]:
    print("List of devices attached")
    if not dropped:
        print("adb-FAKE (2)._adb-tls-connect._tcp device product:x model:2311DRK48G device:x transport_id:1")
    sys.exit(0)
if dropped and a[0] == "logcat":
    print("- waiting for device -", file=sys.stderr)
    while dropped:
        time.sleep(0.5)
        t, dropped = now()
if dropped:
    print("error: device 'adb-FAKE' not found", file=sys.stderr)
    sys.exit(1)
running = 0 <= t < min(run_len, crash)
done = t >= run_len and crash > run_len
if a[0] == "shell":
    cmd = " ".join(a[1:])
    if "am start" in cmd:
        open(t0f, "w").write(str(time.time()))
    elif "pidof" in cmd:
        if running:
            print("12345")
        elif "echo NONE" in cmd:
            print("NONE")
        else:
            sys.exit(1)
    elif "settings get" in cmd:
        print("0")
    sys.exit(0)
if a[0] == "logcat" and "-d" in a:
    print("10-21 10:00:00.000  1  1 I aglab   : progress")
    if done:
        print("10-21 10:00:30.000  1  1 I aglab   : AGLAB DONE exit=0")
if a[0] == "pull":
    if a[1].endswith("last_run.json"):
        sys.exit(1)
    os.makedirs(a[2], exist_ok=True)
    open(os.path.join(a[2], "run.json" if done else "partial.txt"), "w").write(f"pulled at t={t:.1f}\n")
sys.exit(0)
