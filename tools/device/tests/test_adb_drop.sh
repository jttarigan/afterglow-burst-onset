#!/usr/bin/env bash
# run_android.sh against a fake adb (fake_adb.py): no device is touched. ~2 min.
#
#   tools/device/tests/test_adb_drop.sh
#
# Copies run_android.sh into a throwaway git tree with a stub validate.py and an empty APK, puts the
# fake adb first on PATH through ANDROID_HOME, and checks five cases, among them the 2026-10-19 POCO
# failure (a 10 s wireless adb drop mid-run must not end the wait and pull a half-done run).
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/sdk/platform-tools" "$T/tree/tools/device" "$T/tree/tools/analysis" \
         "$T/tree/platform/android/app/build/outputs/apk/release"
cp "$HERE/fake_adb.py" "$T/sdk/platform-tools/adb"
cp "$ROOT/tools/device/run_android.sh" "$T/tree/tools/device/"
chmod +x "$T/sdk/platform-tools/adb" "$T/tree/tools/device/run_android.sh"
cat > "$T/tree/tools/analysis/validate.py" <<'PY'
import os, sys
print("VALID" if os.path.exists(os.path.join(sys.argv[1], "run.json")) else "INVALID: no run.json")
PY
: > "$T/tree/platform/android/app/build/outputs/apk/release/app-release.apk"
echo 'results/' > "$T/tree/.gitignore"
(cd "$T/tree" && git init -q && git add -A && git -c user.name=test -c user.email=test@localhost commit -qm test)

fails=0
# check <name> <expected exit> <expected file in the pulled run, or "-" for none> [SAYS=<text in its output>] [VAR=value ...]
# Each case gets 300 s of wall clock: a hang (2026-10-21: logcat waiting for a lost device) is a failure.
# ONLY=<name> runs one case.
check() {
  local name="$1" want_rc="$2" want_file="$3" says="" rc
  shift 3
  if [ "${1:-}" != "${1#SAYS=}" ]; then says="${1#SAYS=}"; shift; fi
  [ -z "${ONLY:-}" ] || [ "$ONLY" = "$name" ] || return 0
  rm -rf "$T/state"; mkdir -p "$T/state"
  perl -e 'alarm shift; exec @ARGV' 300 \
    env -i HOME="$HOME" PATH="/usr/bin:/bin:/usr/sbin:/sbin" ANDROID_HOME="$T/sdk" ANDROID_NDK=x JAVA_HOME=x \
    SIM_STATE="$T/state" NO_BUILD=1 NO_INSTALL=1 TIMEOUT=120 RUN_ID="sim_$name" "$@" \
    "$T/tree/tools/device/run_android.sh" poco stepbench > "$T/state/out.txt" 2>&1
  rc=$?
  if [ "$rc" = "$want_rc" ] && { [ "$want_file" = - ] || [ -f "$T/tree/results/sim_$name/$want_file" ]; } &&
     { [ -z "$says" ] || grep -qF "$says" "$T/state/out.txt"; }; then
    echo "PASS $name (exit $rc)"
  else
    echo "FAIL $name: exit $rc (want $want_rc${says:+, output with \"$says\"}), pulled: $(ls "$T/tree/results/sim_$name" 2>/dev/null | tr '\n' ' ')"
    sed 's/^/    /' "$T/state/out.txt"
    fails=$((fails + 1))
  fi
}

check normal 0 run.json SIM_RUN_LEN=12
check drop_mid_run 0 run.json "SAYS=adb back" SIM_RUN_LEN=40 SIM_DROP_FROM=6 SIM_DROP_TO=30
check done_during_drop 0 run.json SIM_RUN_LEN=12 SIM_DROP_FROM=6 SIM_DROP_TO=20
check crash 1 partial.txt SIM_RUN_LEN=30 SIM_CRASH_AT=8
check lost_for_good 1 - "SAYS=giving up" SIM_RUN_LEN=60 SIM_DROP_FROM=6 SIM_DROP_TO=1000 ADB_LOST_MAX=10
[ "$fails" = 0 ] && echo "all passed" || { echo "$fails failed"; exit 1; }
