#!/usr/bin/env python3
"""Copied from NeonTide/tools/check_spirv_contraction.py (commit 3dc7266). Under Slang -fp-mode precise every float add/sub/mul/div result in the SPIR-V
carries the NoContraction decoration; under -fp-mode fast none does.

Known gap (NeonTide DECISIONS D22): Slang 2026.18.2 leaves OpDot undecorated even in precise mode,
although the SPIR-V spec lists OpDot among the arithmetic instructions NoContraction applies to.
Undecorated OpDot results are reported, not failed, so the gap stays visible in every test log.

Usage: check_spirv_contraction.py <kernel-dir>/spirv
Reads the .spvasm listings the build writes next to each .spv (cmake/NtSlang.cmake).
"""
import re
import sys
from pathlib import Path

FLOAT_ARITH = ("OpFAdd", "OpFSub", "OpFMul", "OpFDiv", "OpFRem", "OpFMod", "OpVectorTimesScalar",
               "OpMatrixTimesScalar")
KNOWN_GAP = ("OpDot",)


def analyse(path):
    text = path.read_text(encoding="utf-8")
    decorated = set(re.findall(r"OpDecorate (%\S+) NoContraction", text))
    results, gap = [], []
    for m in re.finditer(r"^\s*(%\S+) = (Op\w+)", text, flags=re.M):
        if m.group(2) in FLOAT_ARITH:
            results.append(m.group(1))
        elif m.group(2) in KNOWN_GAP and m.group(1) not in decorated:
            gap.append(m.group(1))
    missing = [r for r in results if r not in decorated]
    return len(results), len(decorated), missing, gap


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    root = Path(sys.argv[1])
    ok = True
    for variant in ("fast", "precise"):
        files = sorted((root / variant).glob("*.spvasm"))
        if not files:
            print(f"no .spvasm files under {root / variant}")
            return 1
        for f in files:
            n, dec, missing, gap = analyse(f)
            if variant == "precise" and missing:
                ok = False
                print(f"FAIL {variant}/{f.name}: {len(missing)} of {n} float ops lack NoContraction")
            elif variant == "fast" and dec:
                ok = False
                print(f"FAIL {variant}/{f.name}: {dec} NoContraction decorations in fast mode")
            else:
                print(f"ok   {variant}/{f.name}: {n} float arithmetic ops, {dec} NoContraction")
            if variant == "precise" and gap:
                print(f"     known gap: {len(gap)} OpDot without NoContraction in {f.name} (D22)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
