#!/usr/bin/env python3
"""R1 guard (copied from NeonTide/tools/check_core_numeric.py, commit 3dc7266): core/ is integer-only. Fails if any file under core/ (other than the FMA canary, whose
job is to test the compiler's float behaviour) mentions float, double, <cmath> or a float literal
outside comments and strings."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ALLOWED = {"core/src/fma_canary.cpp"}
BAD = [
    (re.compile(r"\b(float|double)\b"), "float/double type"),
    (re.compile(r"#\s*include\s*<(cmath|math\.h|cfloat|float\.h)>"), "float header"),
    (re.compile(r"(?<![\w.])\d+\.\d*([eE][+-]?\d+)?[fF]?\b|(?<![\w.])\d+[eE][+-]?\d+[fF]?\b"), "float literal"),
]


def strip(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r'"(\\.|[^"\\])*"', '""', text)
    return text


def main():
    problems = []
    for path in sorted((ROOT / "core").rglob("*")):
        if path.suffix not in (".h", ".hpp", ".cpp", ".cc"):
            continue
        rel = path.relative_to(ROOT).as_posix()
        if rel in ALLOWED:
            continue
        for n, line in enumerate(strip(path.read_text(encoding="utf-8")).splitlines(), 1):
            for rx, what in BAD:
                if rx.search(line):
                    problems.append(f"{rel}:{n}: {what}: {line.strip()}")
    for p in problems:
        print(p)
    if problems:
        print(f"R1 violated: {len(problems)} float use(s) in core/")
        return 1
    print("core/ is integer-only (R1)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
