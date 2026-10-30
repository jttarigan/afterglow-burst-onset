#!/usr/bin/env python3
"""Idle-gap ladder (04 E1 subset): per device and gap, the medians of submit-to-GPU-start, GPU execution
time and round trip of a single tiny command buffer, with the 1st measurement dropped (pipeline start).

Usage: idlegap_summary.py <run dir>... [--out idlegap.md]
"""
import argparse
import json
from pathlib import Path

import numpy as np

from agcsv import read_csv


def summarise(run_dir):
    run_dir = Path(run_dir)
    dev = json.loads((run_dir / "run.json").read_text())["device"]["id"]
    _, df = read_csv(run_dir / "idlegap.csv")
    df = df[df["idx"] > 0]
    out = {}
    for gap, sel in df.groupby("gap_us"):
        med = {k: float(np.median(sel[k])) / 1000 for k in ("submit_to_start_ns", "gpu_ns", "round_trip_ns")}
        out[int(gap)] = (len(sel), med)
    return dev, out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--out")
    a = ap.parse_args()
    res = [summarise(r) for r in a.runs]
    gaps = sorted({g for _, o in res for g in o})
    lines = []
    for key, title in (("submit_to_start_ns", "Submit to GPU start"), ("gpu_ns", "GPU execution"),
                       ("round_trip_ns", "Round trip")):
        lines += [f"## {title}, median µs", "", "| Device | " + " | ".join(f"{g / 1000:g} ms" for g in gaps) + " |",
                  "|---|" + "---|" * len(gaps)]
        for dev, o in res:
            lines.append(f"| {dev} | " + " | ".join(f"{o[g][1][key]:.0f}" if g in o else "" for g in gaps) + " |")
        lines.append("")
    lines.append("n per cell: " + ", ".join(f"{dev} {min(n for n, _ in o.values())}–{max(n for n, _ in o.values())}"
                                          for dev, o in res))
    text = "\n".join(lines)
    if a.out:
        Path(a.out).write_text(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
