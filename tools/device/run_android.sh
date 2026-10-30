#!/usr/bin/env bash
# Build, install, launch and pull one Afterglow run on an Android device (wireless adb or USB).
# Adapted from NeonTide/tools/device/run_android.sh (commit 3dc7266), itself from NeonSwarm and SnakeBuild.
#
#   tools/device/run_android.sh <device-id> <mode> [--flag=value ...]
#
#   device-id   poco | tab | a56 | s6lite | fold8   (found by model in `adb devices -l`, or set ANDROID_SERIAL)
#   mode        stepbench | smoke | e0
#
# Environment: EXP, CELL, REP, RUN_ID, NO_BUILD, NO_INSTALL, TIMEOUT, REQ_CHARGING, BRIGHTNESS,
# REFRESH_HZ (60 primary; 120 for the D53 subset) (as run_ios.sh).
#
# Gotchas from the sibling projects: on HyperOS (POCO) every install needs a tap on the phone
# within ~10 s, and a second `am start` while the app is in front is swallowed (we force-stop
# first); Wi-Fi pulls can stall, so each pull has a timeout and retries.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PKG="org.afterglow.app"
DEVICE_ID="${1:?usage: run_android.sh <device-id> <mode> [--flag=value ...]}"
MODE="${2:?usage: run_android.sh <device-id> <mode> [--flag=value ...]}"
shift 2

export ANDROID_HOME="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
export ANDROID_NDK="${ANDROID_NDK:-$ANDROID_HOME/ndk/27.2.12479018}"
export JAVA_HOME="${JAVA_HOME:-$(ls -d "$HOME"/tools/jdk-17*/Contents/Home 2>/dev/null | head -1)}"
export PATH="$HOME/Library/Python/3.9/bin:$ANDROID_HOME/platform-tools:$PATH"

case "$DEVICE_ID" in
  poco) MODEL="2311DRK48G" ;;
  tab)  MODEL="SM_T975" ;;
  a56)  MODEL="SM_A566B" ;;
  s6lite) MODEL="SM_P615" ;;   # Galaxy Tab S6 Lite LTE (Mali-G72 MP3, 60 Hz only)
  fold8) MODEL="SM_F971B" ;;   # Galaxy Z Fold 8 (Snapdragon 8 Elite Gen 5, Adreno 840), borrowed, D67
  *) echo "unknown device id: $DEVICE_ID" >&2; exit 2 ;;
esac
if [ -z "${ANDROID_SERIAL:-}" ]; then
  # mDNS serials can contain spaces ("adb-XXXX (2)._adb-tls-connect._tcp"): split on the state field.
  ANDROID_SERIAL="$(adb devices -l | sed -n "s/^\(.*[^[:space:]]\)[[:space:]][[:space:]]*device .*model:$MODEL .*/\1/p" | head -1)"
  [ -n "$ANDROID_SERIAL" ] || { echo "$DEVICE_ID ($MODEL) not connected: adb devices -l"; adb devices -l; exit 1; }
fi
export ANDROID_SERIAL

# Pre-flight (exit 3 = a precondition the user must fix; run_matrix.py stops the campaign on it).
# Tab S7+: on "Kemulusan Gerakan" = Tinggi the display ignores the app's 60 Hz request
# (2026-10-09: up to 120 frames/s), so Afterglow's runs need Standar (refresh_rate_mode 0), set by
# the user by hand (D58).
# The Galaxy A56 is also Samsung (One UI "Motion smoothness"), so the same rule applies (D59); its
# setting may be absent ("null"), then the run goes ahead and validity rule 3 judges the cadence.
# The Galaxy Z Fold 8 (One UI 9) gets the same rule (D67).
# For the 120 Hz subset (D53, REFRESH_HZ=120) the opposite holds: the setting must be Tinggi/Adaptive.
if [ "$DEVICE_ID" = tab ] || [ "$DEVICE_ID" = a56 ] || [ "$DEVICE_ID" = fold8 ]; then
  RRM="$(adb shell settings get secure refresh_rate_mode | tr -d '\r')"
  if [ "$RRM" = null ] && [ "$DEVICE_ID" != tab ]; then
    echo "warning: $DEVICE_ID has no refresh_rate_mode setting; rule 3 checks the cadence after the run"
  elif [ "${REFRESH_HZ:-60}" = 60 ] && [ "$RRM" != 0 ]; then
    echo "PRECONDITION: $DEVICE_ID Motion smoothness (Kemulusan Gerakan) is not on Standar (refresh_rate_mode=$RRM); set it to Standar by hand (D58, D59)" >&2
    exit 3
  elif [ "${REFRESH_HZ:-60}" != 60 ] && [ "$RRM" = 0 ]; then
    echo "PRECONDITION: $DEVICE_ID Motion smoothness is on Standar but REFRESH_HZ=$REFRESH_HZ needs Tinggi/Adaptive (D53); set it by hand" >&2
    exit 3
  fi
fi

EXP="${EXP:-$MODE}"
RUN_ID="${RUN_ID:-${DEVICE_ID}_${EXP}_${CELL:-all}_${REP:-0}_$(date -u +%Y%m%dT%H%M%SZ)}"
TIMEOUT="${TIMEOUT:-3600}"
DEST="$ROOT/results/$RUN_ID"
APK="$ROOT/platform/android/app/build/outputs/apk/release/app-release.apk"
DEV_FILES="/sdcard/Android/data/$PKG/files"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ "${NO_BUILD:-0}" != "1" ]; then
  echo "== build (libafterglow.so + assets, then the APK)"
  [ -f "$ROOT/build/android-release/build.ninja" ] || cmake --preset android-release >/dev/null
  cmake --build --preset android-release >/dev/null
  echo "sdk.dir=$ANDROID_HOME" > "$ROOT/platform/android/local.properties"
  (cd "$ROOT/platform/android" && ./gradlew --no-daemon -q assembleRelease) >"$TMP/gradle.log" 2>&1 ||
    { tail -40 "$TMP/gradle.log"; echo "gradle failed" >&2; exit 1; }
fi
[ -f "$APK" ] || { echo "no APK at $APK" >&2; exit 1; }
if [ -n "$(git -C "$ROOT" status --porcelain)" ]; then
  echo "warning: the git tree is dirty; this run will be marked invalid (R7)"
fi

if [ "${NO_INSTALL:-0}" != "1" ]; then
  echo "== install on $DEVICE_ID ($ANDROID_SERIAL)$([ "$DEVICE_ID" = poco ] && echo ': tap Install on the phone')"
  adb install -r "$APK" >/dev/null
fi

ARGS="--mode=$MODE --run-id=$RUN_ID --device-id=$DEVICE_ID --exp=$EXP --brightness=${BRIGHTNESS:-50} --req-display_refresh_hz=${REFRESH_HZ:-60} --req-charging=${REQ_CHARGING:-true} --req-low_power_mode=false $*"
echo "== launch: $ARGS"
adb shell am force-stop "$PKG"
adb logcat -c
START="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
adb shell am start -n "$PKG/.MainActivity" --es args "'$ARGS'" >/dev/null
sleep 2

# Wait for "AGLAB DONE" or for the app to be gone. 2026-10-19 (POCO): a brief wireless adb drop made one
# failed `pidof` look like the app dying, so a half-done run was pulled while the app kept running. Now
# only the device's own answer counts ("up", "gone"; "?" = adb did not answer), the app must be gone on
# GONE_CHECKS checks in a row, and a lost adb is waited for up to ADB_LOST_MAX s (then the pull fails and
# run_matrix.py requeues the run). A failed logcat read keeps the last good copy instead of emptying it.
# `adb logcat` waits for a missing device instead of failing (POCO, 2026-10-21), which held this loop inside
# one call for a whole drop and would hang it past TIMEOUT if adb never came back: each read gets LOGCAT_MAX s.
GONE_CHECKS="${GONE_CHECKS:-3}"
ADB_LOST_MAX="${ADB_LOST_MAX:-600}"
LOGCAT_MAX="${LOGCAT_MAX:-15}"
app_state() {
  local out
  out="$(adb shell "pidof $PKG || echo NONE" 2>/dev/null | tr -d '\r')" || out=""
  if [[ "$out" == *NONE* ]]; then echo gone
  elif [[ "$out" == *[0-9]* ]]; then echo up
  else echo "?"
  fi
}
read_logcat() {
  # The group's 2>/dev/null also hides bash's "Alarm clock" notice when a read is cut off.
  if { perl -e 'alarm shift; exec @ARGV' "$LOGCAT_MAX" adb logcat -d -s aglab:I > "$TMP/logcat.new"; } 2>/dev/null; then
    mv "$TMP/logcat.new" "$TMP/logcat.txt"
  fi
  if grep -q "AGLAB DONE exit=" "$TMP/logcat.txt"; then
    EXIT_CODE="$(grep -o 'AGLAB DONE exit=[0-9]*' "$TMP/logcat.txt" | tail -1 | sed 's/.*=//')"
  fi
}
: > "$TMP/logcat.txt"
deadline=$(( $(date +%s) + TIMEOUT ))
EXIT_CODE=""
gone=0
lost_since=""
while [ "$(date +%s)" -lt "$deadline" ]; do
  read_logcat
  [ -z "$EXIT_CODE" ] || break
  state="$(app_state)"
  if [ "$state" = "?" ]; then
    if [ -z "$lost_since" ]; then
      lost_since="$(date +%s)"
      echo "adb lost at $(date -u +%H:%M:%SZ); waiting up to $ADB_LOST_MAX s for it to come back"
    elif [ $(( $(date +%s) - lost_since )) -ge "$ADB_LOST_MAX" ]; then
      echo "adb lost for $ADB_LOST_MAX s; giving up on this run"
      break
    fi
  else
    if [ -n "$lost_since" ]; then
      echo "adb back at $(date -u +%H:%M:%SZ) after $(( $(date +%s) - lost_since )) s (app $state)"
      lost_since=""
    fi
    if [ "$state" = gone ]; then gone=$((gone + 1)); else gone=0; fi
    if [ "$gone" -ge "$GONE_CHECKS" ]; then
      sleep 2
      read_logcat
      break
    fi
  fi
  sleep 2
done
END="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
sed -n 's/^.*aglab *: //p' "$TMP/logcat.txt" | tail -5 || true
[ -n "$EXIT_CODE" ] || echo "run did not report DONE (crash or timeout)"

echo "== pull -> ${DEST#$ROOT/}"
pulled=0
for i in 1 2 3; do
  rm -rf "$DEST"
  if perl -e 'alarm shift; exec @ARGV' 300 adb pull "$DEV_FILES/results/$RUN_ID" "$DEST" >"$TMP/pull.log" 2>&1; then
    pulled=1
    break
  fi
  sleep 5
done
[ "$pulled" = 1 ] || { cat "$TMP/pull.log"; mkdir -p "$DEST"; cp "$TMP/logcat.txt" "$DEST/console.log"; echo "pull failed" >&2; exit 1; }
adb pull "$DEV_FILES/last_run.json" "$DEST/device_last_run.json" >/dev/null 2>&1 || true
cp "$TMP/logcat.txt" "$DEST/console.log"
python3 - "$DEST" "$DEVICE_ID" "$ANDROID_SERIAL" "$MODE" "$START" "$END" "${EXIT_CODE:--1}" "$(git -C "$ROOT" rev-parse HEAD)" $ARGS <<'PY'
import json, sys
dest, dev, serial, mode, start, end, code, sha, *args = sys.argv[1:]
json.dump({"device_id": dev, "serial": serial, "mode": mode, "args": args, "host_start_utc": start,
           "host_end_utc": end, "exit_code": int(code), "host_git_sha": sha,
           "transport": "wifi" if (":" in serial or "_adb-tls-connect" in serial) else "usb"},
          open(f"{dest}/host_run.json", "w"), indent=2)
PY
python3 "$ROOT/tools/analysis/validate.py" "$DEST" | tail -2
echo "device exit code: ${EXIT_CODE:-unknown}"
echo "done: ${DEST#$ROOT/}"
[ -n "$EXIT_CODE" ] && [ "$EXIT_CODE" = 0 ]
