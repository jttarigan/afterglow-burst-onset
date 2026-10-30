#!/usr/bin/env bash
# Build, install, launch and pull one Afterglow run on an iOS device over Wi-Fi (or USB).
# Adapted from NeonTide/tools/device/run_ios.sh (commit 3dc7266), itself from NeonSwarm and SnakeBuild.
#
#   tools/device/run_ios.sh <device-id> <mode> [--flag=value ...]
#
#   device-id   ip12 | ipmini6 | ippro3 | ks   (ks needs AG_ALLOW_KS=1)
#   mode        stepbench | smoke | e0
#   flags       passed to the app (lab/src/app.h); e.g. --calms=100,1000 --trials=0:40
#
# Environment:
#   EXP=<mode> CELL=all REP=0   build the run id <device>_<exp>_<cell>_<rep>_<utc> (docs/03 §12)
#   RUN_ID=...                  override the run id
#   NO_BUILD=1 NO_INSTALL=1     skip steps
#   APP=<path>.app              install this app instead of the local build (a pinned campaign build)
#   TIMEOUT=3600                launch timeout in seconds
#   REQ_CHARGING=true           requested power state (D3: timing runs on the charger)
#   BRIGHTNESS=50               set by the app (docs/04 §1)
#
# Gotchas (from the sibling projects): a locked screen silently breaks launches, so the script
# waits for unlock; the "--" before app arguments is required; if the console link drops, the app
# keeps running and the script polls until it exits; never kill a devicectl process you did not start.
set -euo pipefail
# Device UDIDs come from the environment: AG_UDID_IP12, AG_UDID_IPMINI6, AG_UDID_IPPRO3, AG_UDID_KS.

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUNDLE_ID="org.afterglow.app"
DEVICE_ID="${1:?usage: run_ios.sh <device-id> <mode> [--flag=value ...]}"
MODE="${2:?usage: run_ios.sh <device-id> <mode> [--flag=value ...]}"
shift 2

case "$DEVICE_ID" in
  ip12)    UDID="${AG_UDID_IP12:?set AG_UDID_IP12 to the device UDID}" ;;
  ipmini6) UDID="${AG_UDID_IPMINI6:?set AG_UDID_IPMINI6 to the device UDID}" ;;
  ippro3)  UDID="${AG_UDID_IPPRO3:?set AG_UDID_IPPRO3 to the device UDID}" ;;
  ks)
    if [ "${AG_ALLOW_KS:-0}" != "1" ]; then
      echo "ks is an optional, borrowed device: set AG_ALLOW_KS=1 to use it" >&2
      exit 2
    fi
    UDID="${AG_UDID_KS:?set AG_UDID_KS to the device UDID}" ;;
  *) echo "unknown device id: $DEVICE_ID" >&2; exit 2 ;;
esac

EXP="${EXP:-$MODE}"
RUN_ID="${RUN_ID:-${DEVICE_ID}_${EXP}_${CELL:-all}_${REP:-0}_$(date -u +%Y%m%dT%H%M%SZ)}"
TIMEOUT="${TIMEOUT:-3600}"
DEST="$ROOT/results/$RUN_ID"
APP="${APP:-$ROOT/build/ios-app/Release-iphoneos/Afterglow.app}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export PATH="$HOME/Library/Python/3.9/bin:$PATH"

if [ "${NO_BUILD:-0}" != "1" ]; then
  echo "== build (iOS static libs + kernels, then the app)"
  [ -f "$ROOT/build/ios-release/build.ninja" ] || cmake --preset ios-release >/dev/null
  cmake --build --preset ios-release >/dev/null
  xcodebuild -project "$ROOT/platform/ios/Afterglow.xcodeproj" -target Afterglow -configuration Release \
    -sdk iphoneos SYMROOT="$ROOT/build/ios-app" -allowProvisioningUpdates -allowProvisioningDeviceRegistration \
    build >"$TMP/xcodebuild.log" 2>&1 || { tail -40 "$TMP/xcodebuild.log"; echo "xcodebuild failed" >&2; exit 1; }
fi
[ -d "$APP" ] || { echo "no app at $APP" >&2; exit 1; }
if [ -n "$(git -C "$ROOT" status --porcelain)" ]; then
  echo "warning: the git tree is dirty; this run will be marked invalid (R7)"
fi

if [ "${NO_INSTALL:-0}" != "1" ]; then
  echo "== install on $DEVICE_ID ($UDID)"
  xcrun devicectl device install app --device "$UDID" "$APP" --quiet
fi

wait_unlocked() {
  for i in $(seq 1 100); do
    xcrun devicectl device info lockState --device "$UDID" --json-output "$TMP/lock.json" >/dev/null 2>&1 || return 0
    python3 - "$TMP/lock.json" <<'PY' && return 0
import json, sys
r = json.load(open(sys.argv[1])).get("result", {})
locked = r.get("passcodeRequired", r.get("lockState", {}).get("passcodeRequired", False))
sys.exit(1 if locked else 0)
PY
    [ "$i" = 1 ] && echo "device is locked: unlock it to launch (waiting up to 5 min)"
    sleep 3
  done
  echo "still locked after 5 min; giving up" >&2
  return 1
}

app_running() {
  xcrun devicectl device info processes --device "$UDID" --json-output "$TMP/procs.json" >/dev/null 2>&1 || return 0
  python3 - "$TMP/procs.json" <<'PY'
import json, sys
procs = json.load(open(sys.argv[1])).get("result", {}).get("runningProcesses", [])
sys.exit(0 if any("Afterglow.app" in p.get("executable", "") for p in procs) else 1)
PY
}

wait_unlocked
ARGS=("--mode=$MODE" "--run-id=$RUN_ID" "--device-id=$DEVICE_ID" "--exp=$EXP" "--brightness=${BRIGHTNESS:-50}"
      "--req-display_refresh_hz=${REFRESH_HZ:-60}" "--req-charging=${REQ_CHARGING:-true}" "--req-low_power_mode=false" "$@")
echo "== launch: ${ARGS[*]}"
START="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
set +e
# 2026-10-13: on the iPhone 12 a launch sometimes fails outright with CoreDeviceError 10002 (EINVAL)
# and works seconds later, so a launch that never started the app is retried up to 3 times.
for attempt in 1 2 3; do
  xcrun devicectl device process launch --device "$UDID" --terminate-existing --console --timeout "$TIMEOUT" \
    "$BUNDLE_ID" -- "${ARGS[@]}" 2>&1 | tee "$TMP/console.log"
  LAUNCH_STATUS=${PIPESTATUS[0]}
  if [ "$LAUNCH_STATUS" = 0 ] || ! grep -q 'failed to launch' "$TMP/console.log"; then break; fi
  [ "$attempt" = 3 ] || { echo "launch attempt $attempt failed before the app started; retrying in 10 s"; sleep 10; }
done
set -e
if [ "$LAUNCH_STATUS" != "0" ]; then
  echo "console ended with status $LAUNCH_STATUS; waiting for the app to finish"
  while app_running; do sleep 10; done
fi
END="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

echo "== pull -> ${DEST#$ROOT/}"
mkdir -p "$DEST"
for i in 1 2 3; do
  if xcrun devicectl device copy from --device "$UDID" --domain-type appDataContainer \
       --domain-identifier "$BUNDLE_ID" --source "Documents/results/$RUN_ID" \
       --destination "$DEST" >"$TMP/copy.log" 2>&1; then
    break
  fi
  [ "$i" = 3 ] && { cat "$TMP/copy.log"; echo "pull failed" >&2; exit 1; }
  sleep 5
done
if [ "${PULL_LAST_RUN:-1}" = "1" ]; then  # diagnostic only; the chain skips it (one Wi-Fi round trip less)
  xcrun devicectl device copy from --device "$UDID" --domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
    --source "Documents/last_run.json" --destination "$DEST/device_last_run.json" >/dev/null 2>&1 || true
fi
cp "$TMP/console.log" "$DEST/console.log"
python3 - "$DEST" "$DEVICE_ID" "$UDID" "$MODE" "$START" "$END" "$LAUNCH_STATUS" "$(git -C "$ROOT" rev-parse HEAD)" "${ARGS[@]}" <<'PY'
import json, sys
dest, dev, udid, mode, start, end, status, sha, *args = sys.argv[1:]
json.dump({"device_id": dev, "udid": udid, "mode": mode, "args": args, "host_start_utc": start,
           "host_end_utc": end, "console_status": int(status), "host_git_sha": sha,
           "transport": "devicectl"}, open(f"{dest}/host_run.json", "w"), indent=2)
PY
python3 "$ROOT/tools/analysis/validate.py" "$DEST" | tail -2
echo "done: ${DEST#$ROOT/}"
