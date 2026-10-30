#!/bin/bash
# M2 on one iOS device, unattended, with the iOS app pinned at 21910df (04 §1: one build per campaign):
# install the pinned app once -> [m1_e0 if not done yet] -> m2_e1 -> m2_idlegap -> m2_ablation, 60 s apart. The iOS counterpart of
# results/builds/m2_chain.sh (Android). The unplugged subset (m2_unplugged) runs separately, after the
# user pulls the charger.
#
#   caffeinate -is tools/device/m2_chain_ios.sh <ip12|ipmini6|ippro3> [wait-busy minutes] [slot end HH:MM WIB] \
#     >> results/builds/m2_<dev>.log 2>&1
#
# With a slot end, no run starts that would end after it (run_matrix.py --deadline-epoch); the rest
# resumes in a later slot from the saved campaign state.
#
# The pinned app is a copy at results/builds/m2_21910df_ios/Afterglow.app, built in a clean worktree at
# the pin. If another session's launch is aimed at the device, the chain waits up to the given minutes
# (default 0), then stops with its campaign state saved (run_matrix.py resumes it).
set -uo pipefail
DEV="${1:?usage: m2_chain_ios.sh <ip12|ipmini6|ippro3> [wait-busy minutes] [slot end HH:MM WIB]}"
WAIT_MIN="${2:-0}"
SLOT_END="${3:-}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
export PATH="$HOME/Library/Python/3.9/bin:$PATH"
case "$DEV" in
  ip12)    UDID="${AG_UDID_IP12:?set AG_UDID_IP12 to the device UDID}" ;;
  ipmini6) UDID="${AG_UDID_IPMINI6:?set AG_UDID_IPMINI6 to the device UDID}" ;;
  ippro3)  UDID="${AG_UDID_IPPRO3:?set AG_UDID_IPPRO3 to the device UDID}" ;;
  *) echo "device? (ip12, ipmini6 or ippro3; ks is never part of M2)"; exit 2 ;;
esac
M2_SHA=21910dfb31dfaaf633a70f33e663b7de25e15ef1
export APP="$ROOT/results/builds/m2_21910df_ios/Afterglow.app" NO_BUILD=1 NO_INSTALL=1 REQ_CHARGING=true PULL_LAST_RUN=0
DEADLINE=()
if [ -n "$SLOT_END" ]; then
  END_EPOCH="$(TZ=Asia/Jakarta date -j -f '%Y-%m-%d %H:%M:%S' "$(TZ=Asia/Jakarta date +%Y-%m-%d) $SLOT_END:00" +%s)" || exit 2
  DEADLINE=(--deadline-epoch "$END_EPOCH")
fi
ts() { TZ=Asia/Jakarta date '+%H:%M:%S WIB'; }
pause() { perl -e 'select(undef, undef, undef, 60)'; }
busy() { pgrep -fl "devicectl device process launch|xctrace|run_device_matrix" | grep -v run_matrix.py | grep -c "$UDID"; }

[ -d "$APP" ] || { echo "[$(ts)] no pinned iOS app at $APP; stopping"; exit 1; }
N=$(LC_ALL=C strings "$APP/Afterglow" | LC_ALL=C grep -c "$M2_SHA")
[ "${N:-0}" -ge 1 ] || { echo "[$(ts)] $APP is not built at the pinned M2 commit $M2_SHA; stopping"; exit 1; }
echo "[$(ts)] M2 chain on $DEV ($UDID), app pinned 21910df, host HEAD $(git rev-parse --short HEAD)${SLOT_END:+, slot ends $SLOT_END WIB}"

for i in $(seq 0 $((WAIT_MIN * 2))); do
  [ "$(busy)" = 0 ] && break
  [ "$i" = $((WAIT_MIN * 2)) ] && { echo "[$(ts)] $DEV still in use by another session; stopping before the install"; exit 1; }
  perl -e 'select(undef, undef, undef, 30)'
done
echo "[$(ts)] installing the pinned app"
xcrun devicectl device install app --device "$UDID" "$APP" --quiet || { echo "[$(ts)] install failed; stopping"; exit 1; }
perl -e 'select(undef, undef, undef, 10)'   # a launch right after an install failed on the iPhone 12 (2026-10-13)

# E0 self-checks first on a device that has none yet (the iPad Pro, 2026-10-18; as the S6 Lite's chain,
# D62). Informational: a failed E0 is reported and the timed campaigns still run. run_matrix.py skips it
# where the saved state is already complete.
echo "[$(ts)] campaign m1_e0"
python3 tools/device/run_matrix.py tools/device/campaigns/m1_e0.json --device "$DEV" --wait-busy-s $((WAIT_MIN * 60)) ${DEADLINE[@]+"${DEADLINE[@]}"} 2>&1 \
  | grep --line-buffered -v '^running'
rc=${PIPESTATUS[0]}
echo "[$(ts)] campaign m1_e0 exit $rc"
[ "$rc" = 4 ] && { echo "[$(ts)] slot ends $SLOT_END WIB: stopping the chain; the rest resumes later"; exit 4; }
pause

for c in m2_e1 m2_idlegap m2_ablation; do
  echo "[$(ts)] campaign $c"
  python3 tools/device/run_matrix.py "tools/device/campaigns/$c.json" --device "$DEV" --wait-busy-s $((WAIT_MIN * 60)) ${DEADLINE[@]+"${DEADLINE[@]}"} 2>&1 \
    | grep --line-buffered -v '^running'
  rc=${PIPESTATUS[0]}
  echo "[$(ts)] campaign $c exit $rc"
  [ "$rc" = 4 ] && { echo "[$(ts)] slot ends $SLOT_END WIB: stopping the chain; the rest resumes later"; exit 4; }
  [ "$rc" = 0 ] || { echo "[$(ts)] stopping the chain"; exit "$rc"; }
  pause
done
echo "[$(ts)] M2 chain on $DEV finished"
