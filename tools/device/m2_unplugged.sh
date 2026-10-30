#!/bin/bash
# The E1 unplugged subset (docs/04 E1, campaign m2_unplugged) on one device, with the charger OUT,
# from the build pinned at 21910df (04 §1: one build per campaign). One ~20 min run. 04 sets no order
# between the charged leg and this subset, so it can run first (fewer touches for the user, D59).
#
#   caffeinate -is tools/device/m2_unplugged.sh <ip12|ipmini6|ippro3|poco|tab|a56|s6lite> >> results/builds/m2u_<dev>.log 2>&1
#
# Android: refuses to start while the device reports AC/USB power (exit 3), installs the pinned APK only
# if the installed one differs (the POCO then needs a tap on Install). iOS: installs the pinned app.
set -uo pipefail
DEV="${1:?usage: m2_unplugged.sh <ip12|ipmini6|ippro3|poco|tab|a56|s6lite>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
export PATH="$HOME/Library/Python/3.9/bin:$HOME/Library/Android/sdk/platform-tools:$PATH"
export ANDROID_HOME="$HOME/Library/Android/sdk" NO_BUILD=1 NO_INSTALL=1 REQ_CHARGING=false PULL_LAST_RUN=0
M2_SHA=21910dfb31dfaaf633a70f33e663b7de25e15ef1
ts() { TZ=Asia/Jakarta date '+%H:%M:%S WIB'; }
case "$DEV" in
  ip12)    UDID="${AG_UDID_IP12:?set AG_UDID_IP12 to the device UDID}" ;;
  ipmini6) UDID="${AG_UDID_IPMINI6:?set AG_UDID_IPMINI6 to the device UDID}" ;;
  ippro3)  UDID="${AG_UDID_IPPRO3:?set AG_UDID_IPPRO3 to the device UDID}" ;;
  poco)    MODEL=2311DRK48G ;;
  tab)     MODEL=SM_T975 ;;
  a56)     MODEL=SM_A566B ;;
  s6lite)  MODEL=SM_P615 ;;
  *) echo "device? (ks is never part of M2)"; exit 2 ;;
esac
if [ -n "${UDID:-}" ]; then
  export APP="$ROOT/results/builds/m2_21910df_ios/Afterglow.app"
  [ -d "$APP" ] || { echo "[$(ts)] no pinned iOS app at $APP; stopping"; exit 1; }
  N=$(LC_ALL=C strings "$APP/Afterglow" | LC_ALL=C grep -c "$M2_SHA")
  [ "${N:-0}" -ge 1 ] || { echo "[$(ts)] $APP is not built at the pinned M2 commit; stopping"; exit 1; }
  echo "[$(ts)] unplugged subset on $DEV ($UDID), app pinned 21910df; the charger must be OUT"
  echo "[$(ts)] installing the pinned app"
  xcrun devicectl device install app --device "$UDID" "$APP" --quiet || { echo "[$(ts)] install failed; stopping"; exit 1; }
perl -e 'select(undef, undef, undef, 10)'   # a launch right after an install failed on the iPhone 12 (2026-10-13)
else
  ANDROID_SERIAL="$(adb devices -l | sed -n "s/^\(.*[^[:space:]]\)[[:space:]][[:space:]]*device .*model:$MODEL .*/\1/p" | head -1)"
  [ -n "$ANDROID_SERIAL" ] || { echo "[$(ts)] $DEV ($MODEL) not on adb; stopping"; adb devices -l; exit 1; }
  export ANDROID_SERIAL
  set -- $(adb shell dumpsys battery 2>/dev/null | tr -d '\r' | awk '/^[[:space:]]*level:/{l=$2} /AC powered: true|USB powered: true|Wireless powered: true/{c=1} END{print l+0, c+0}')
  echo "[$(ts)] unplugged subset on $DEV ($ANDROID_SERIAL): battery $1 %, powered=$2"
  [ "$2" = 0 ] || { echo "[$(ts)] PRECONDITION: $DEV is still on the charger; pull it, then rerun"; exit 3; }
  APK="$ROOT/results/builds/m2_21910df.apk"
  # D62: the Tab S6 Lite's pinned build is 21910df + only the NaN thermal-headroom guard.
  if [ "$DEV" = s6lite ]; then M2_SHA=035407308c3c9bf9fbeb205b2febb07d0102acfa; APK="$ROOT/results/builds/m2_s6lite_0354073.apk"; fi
  N=$(unzip -p "$APK" lib/arm64-v8a/libafterglow.so | LC_ALL=C strings | LC_ALL=C grep -c "$M2_SHA")
  [ "${N:-0}" -ge 1 ] || { echo "[$(ts)] $APK is not the pinned M2 build; stopping"; exit 1; }
  DEVAPK="$(adb shell pm path org.afterglow.app 2>/dev/null | tr -d '\r' | sed -n 's/^package://p' | head -1)"
  HAVE="$( [ -n "$DEVAPK" ] && adb shell sha256sum "$DEVAPK" 2>/dev/null | awk '{print $1}')"
  WANT="$(shasum -a 256 "$APK" | awk '{print $1}')"
  if [ "$HAVE" = "$WANT" ]; then
    echo "[$(ts)] the pinned APK is already installed (sha256 ${WANT:0:12})"
  else
    echo "[$(ts)] installing the pinned APK$([ "$DEV" = poco ] && echo ' (tap Install on the POCO)')"
    adb install -r "$APK" || { echo "[$(ts)] install failed; stopping"; exit 1; }
  fi
fi
python3 tools/device/run_matrix.py tools/device/campaigns/m2_unplugged.json --device "$DEV" 2>&1 | grep --line-buffered -v '^running'
rc=${PIPESTATUS[0]}
echo "[$(ts)] m2_unplugged on $DEV exit $rc"
exit "$rc"
