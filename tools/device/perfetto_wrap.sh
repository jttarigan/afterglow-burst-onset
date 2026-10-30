#!/bin/bash
# Probe ground truth on Android (docs/04 E0): records a Perfetto trace with the ftrace events
# power/gpu_frequency and power/cpu_frequency (readable without root) around one command, usually
# one tools/device/run_android.sh run, then pulls it to results/<RUN_ID>/perfetto.pftrace.
# Adapted from NeonSwarm/tools/android_gpufreq_trace.sh (commit 0991e5f), see DECISIONS D57.
# The trace runs on the phone in the background, so its cost is outside the app's process.
# Usage: ANDROID_SERIAL=<serial> RUN_ID=<run id> [PERFETTO_EVENTS="a/b c/d"] tools/device/perfetto_wrap.sh <command...>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
[ $# -ge 1 ] && [ -n "${RUN_ID:-}" ] && [ -n "${ANDROID_SERIAL:-}" ] || { sed -n '2,8p' "$0"; exit 2; }
ADB="${ANDROID_HOME:-$HOME/Library/Android/sdk}/platform-tools/adb"
DEV_TRACE=/data/misc/perfetto-traces/afterglow_freq.pftrace
# Extra ftrace events (e.g. PERFETTO_EVENTS="kgsl/kgsl_pwrlevel kgsl/kgsl_clk" on the Tab's Adreno).
FTRACE='ftrace_events: "power/gpu_frequency" ftrace_events: "power/cpu_frequency"'
for e in ${PERFETTO_EVENTS:-}; do FTRACE="$FTRACE ftrace_events: \"$e\""; done
"$ADB" shell rm -f "$DEV_TRACE"
PID=$(printf '%s\n' 'buffers { size_kb: 65536 fill_policy: DISCARD }' \
  "data_sources { config { name: \"linux.ftrace\" ftrace_config { $FTRACE } } }" \
  'write_into_file: true' 'file_write_period_ms: 2500' 'duration_ms: 7200000' \
  | "$ADB" shell perfetto --txt -c - --background -o "$DEV_TRACE" | tr -d '\r' | tail -1)
[[ "$PID" =~ ^[0-9]+$ ]] || { echo "perfetto did not start: $PID" >&2; exit 1; }
echo "perfetto pid $PID, trace started $(date -u +%H:%M:%SZ)"
stop_trace() {
  "$ADB" shell kill -TERM "$PID" 2>/dev/null || true
  for _ in $(seq 1 10); do
    "$ADB" shell pidof perfetto 2>/dev/null | tr -d '\r' | grep -qw "$PID" || return 0
    perl -e 'select(undef, undef, undef, 1)'
  done
}
rc=0
"$@" || rc=$?
stop_trace
DEST="$ROOT/results/$RUN_ID"
mkdir -p "$DEST"
"$ADB" pull "$DEV_TRACE" "$DEST/perfetto.pftrace" >/dev/null
"$ADB" shell rm -f "$DEV_TRACE"
echo "trace: $DEST/perfetto.pftrace (command exit $rc)"
exit "$rc"
