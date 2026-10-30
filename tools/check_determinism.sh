#!/bin/sh
# M0 acceptance: every research level's workload-trace hash is identical across 3 runs and across
# the Debug and Release builds, and equals the hash recorded in its level file (R1).
# Usage: tools/check_determinism.sh [levels-dir]   (run from the repository root)
set -eu
LEVELS=${1:-bench/levels}
PATH="$HOME/Library/Python/3.9/bin:$PATH"
for preset in mac-release mac-debug; do
  cmake --preset "$preset" >/dev/null
  cmake --build --preset "$preset" >/dev/null
done
fail=0
n=0
for f in "$LEVELS"/single_*.json "$LEVELS"/repeat_*.json; do
  expected=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['expected']['trace_sha256'])" "$f")
  hashes=""
  for preset in mac-release mac-debug; do
    h=$(build/$preset/aglab --mode=sim --level="$f" --repeats=3 | awk '/^run /{print $3}' | sed 's/trace_sha256=//' | sort -u)
    hashes="$hashes $h"
  done
  distinct=$(echo $hashes | tr ' ' '\n' | sort -u)
  n=$((n + 1))
  if [ "$distinct" = "$expected" ]; then
    echo "same   $(basename "$f" .json)  $expected  (3 runs x Release, Debug)"
  else
    echo "DIFFER $(basename "$f" .json)  expected $expected got:$hashes"
    fail=1
  fi
done
echo "determinism: $n levels, $([ $fail = 0 ] && echo 'all identical' || echo 'MISMATCH')"
exit $fail
