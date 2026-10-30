"""Excel workbooks for the paper's data figures: one paper/figures/fig_*.xlsx per
figure, built from the figure's CSV sidecar in results/paper_tables/, so the user can remake a chart in Excel.

Each workbook has three sheets: "data" (the CSV as written by the figure script), "chart_data" (the same numbers
reshaped into one block per panel) and "charts" (native Excel scatter charts, one per panel of the paper figure,
with the paper's device colours, log axes and error bars). Run it after the figure scripts; it reads only the CSVs.

Usage: fig_excel.py [--tables results/paper_tables] [--out paper/figures]
"""
import argparse
import csv
import math
from pathlib import Path

from openpyxl import Workbook
from openpyxl.chart import ScatterChart, Reference, Series
from openpyxl.chart.data_source import NumDataSource, NumRef
from openpyxl.chart.error_bar import ErrorBars
from openpyxl.chart.layout import Layout, ManualLayout

import figstyle as fs

ROOT = Path(__file__).resolve().parents[2]
SYMBOL = {"o": "circle", "s": "square", "^": "triangle", "D": "diamond", "v": "triangle", "P": "plus", "X": "x",
          "*": "star"}
PROC = {"gpu": "GPU", "cpu": "CPU"}
CLASS_COLOR = {"warmup": fs.SLOTS[0], "slowdown": fs.SLOTS[1], "no_steady_state": fs.SLOTS[2], "flat": fs.MUTED}
CHART_W, CHART_H, ROWS_PER_CHART = 12, 6.5, 14   # cm, cm, sheet rows a chart takes in the grid


def num(v):
    """CSV text to a number where it is one; NaN and empty become empty cells."""
    try:
        f = float(v)
    except (TypeError, ValueError):
        return v
    if math.isnan(f):
        return None
    return int(f) if f.is_integer() and "." not in v and "e" not in v.lower() else f


def read_rows(path):
    with open(path, newline="") as f:
        r = csv.reader(f)
        header = next(r)
        return header, [[num(v) for v in row] for row in r]


def uniq(seq):
    return list(dict.fromkeys(seq))


def device_order(devs):
    known = [d for d in fs.NAMES if d in devs]
    return known + [d for d in devs if d not in known]


class Book:
    """One workbook: a raw data sheet, a chart_data sheet of stacked blocks, and a charts sheet in a 2-wide grid."""

    def __init__(self, header, rows):
        self.wb = Workbook()
        raw = self.wb.active
        raw.title = "data"
        raw.append(header)
        for row in rows:
            raw.append(row)
        self.cd = self.wb.create_sheet("chart_data")
        self.ch = self.wb.create_sheet("charts")
        self.next_row, self.n_charts = 1, 0

    def block(self, title, header, rows):
        """Writes a block (title row, header row, data rows); returns (first data row, last data row)."""
        r = self.next_row
        self.cd.cell(r, 1, title)
        for j, h in enumerate(header, 1):
            self.cd.cell(r + 1, j, h)
        for i, row in enumerate(rows):
            for j, v in enumerate(row, 1):
                self.cd.cell(r + 2 + i, j, v)
        self.next_row = r + 3 + len(rows)
        return r + 2, r + 1 + len(rows)

    def ref(self, col, r0, r1):
        return Reference(self.cd, min_col=col, min_row=r0, max_row=r1)

    def abs_ref(self, col, r0, r1):
        c = self.cd.cell(1, col).column_letter
        return f"'chart_data'!${c}${r0}:${c}${r1}"

    def series(self, chart, xcol, ycol, r0, r1, title, color, marker="o", dashed=False, err=None):
        s = Series(self.ref(ycol, r0, r1), self.ref(xcol, r0, r1), title=title)
        s.graphicalProperties.line.solidFill = color.lstrip("#")
        s.graphicalProperties.line.width = 19050   # 1.5 pt
        if dashed:
            s.graphicalProperties.line.dashStyle = "dash"
        s.marker.symbol = SYMBOL.get(marker, "circle")
        s.marker.size = 5
        s.marker.graphicalProperties.solidFill = color.lstrip("#")
        s.marker.graphicalProperties.line.solidFill = "FFFFFF"
        s.smooth = False
        if err is not None:   # (minus column, plus column) as distances from the point
            s.errBars = ErrorBars(errDir="y", errBarType="both", errValType="cust", noEndCap=True,
                                  minus=NumDataSource(numRef=NumRef(f=self.abs_ref(err[0], r0, r1))),
                                  plus=NumDataSource(numRef=NumRef(f=self.abs_ref(err[1], r0, r1))))
        chart.series.append(s)

    def chart(self, title, xlabel, ylabel, xlog=False, ylog=False, ymin=None, ymax=None, xmin=None, xmax=None):
        c = ScatterChart()
        c.title, c.style = title, 2
        c.x_axis.title, c.y_axis.title = xlabel, ylabel
        c.x_axis.delete = c.y_axis.delete = False   # openpyxl 3.1 hides axes in current Excel otherwise
        c.width, c.height = CHART_W, CHART_H
        c.legend.position = "b"
        if xlog:
            c.x_axis.scaling.logBase = 10
        if ylog:
            c.y_axis.scaling.logBase = 10
        c.x_axis.scaling.min, c.x_axis.scaling.max = xmin, xmax
        c.y_axis.scaling.min, c.y_axis.scaling.max = ymin, ymax
        c.layout = Layout(manualLayout=ManualLayout(x=0.02, y=0.12, w=0.95, h=0.62))
        return c

    def place(self, chart):
        row, col = divmod(self.n_charts, 2)
        self.ch.add_chart(chart, f"{'A' if col == 0 else 'J'}{1 + row * ROWS_PER_CHART}")
        self.n_charts += 1

    def save(self, path):
        self.cd.column_dimensions["A"].width = 18
        self.wb.save(path)
        print(f"wrote {path} ({self.n_charts} charts)")


def fig1_kappa0_calm(src, out):
    """Figure 1: κ₀ at burst onset vs calm length, one chart per processor, one series per device."""
    header, rows = read_rows(src)
    H = {h: i for i, h in enumerate(header)}
    b = Book(header, rows)
    devs = device_order(uniq(r[H["device"]] for r in rows))
    calms = sorted(uniq(r[H["calm_s"]] for r in rows))
    for p in ("gpu", "cpu"):
        k0 = {(r[H["device"]], r[H["calm_s"]]): r[H["kappa0"]] for r in rows if r[H["processor"]] == p}
        r0, r1 = b.block(f"{PROC[p]}: κ0 at onset by calm length (s)", ["calm_s"] + [fs.NAMES.get(d, d) for d in devs],
                         [[c] + [k0.get((d, c)) for d in devs] for c in calms])
        ch = b.chart(f"{PROC[p]} clock index at burst onset", "calm length before the burst (s)", "κ0 at onset",
                     xlog=True, ymin=0, ymax=1.1)
        for j, d in enumerate(devs, 2):
            b.series(ch, 1, j, r0, r1, fs.NAMES.get(d, d), fs.DEVICE_COLOR.get(d, fs.MUTED),
                     fs.DEVICE_MARKER.get(d, "o"))
        b.place(ch)
    b.save(out)


def fig2_kappa_class(src, out):
    """Figure 2: median κ after onset per change-point class, one chart per device and processor."""
    header, rows = read_rows(src)
    H = {h: i for i, h in enumerate(header)}
    kcols = [(i, float(h[2:])) for i, h in enumerate(header) if h.startswith("k_")]
    b = Book(header, rows)
    for d in device_order(uniq(r[H["device"]] for r in rows)):
        for p in ("gpu", "cpu"):
            sel = [r for r in rows if r[H["device"]] == d and r[H["processor"]] == p]
            if not sel:
                continue
            names = [f"{r[H['class']]} ({r[H['share']]:.0%})" for r in sel]
            r0, r1 = b.block(f"{fs.NAMES.get(d, d)} {PROC[p]}: median κ by time since onset (s)", ["t_s"] + names,
                             [[t] + [r[i] for r in sel] for i, t in kcols])
            ch = b.chart(f"{fs.NAMES.get(d, d)}, {PROC[p]}", "time since burst onset (s)", "κ (clock index)",
                         ymin=0, ymax=1.15, xmin=0, xmax=2)
            for j, r in enumerate(sel, 2):
                b.series(ch, 1, j, r0, r1, names[j - 2], CLASS_COLOR.get(r[H["class"]], fs.MUTED), "o")
            b.place(ch)
    b.save(out)


def fig3_onset_penalty(src, out):
    """Figure 3: onset penalty P vs calm length per burst size with 95 % CIs, one chart per device and processor."""
    header, rows = read_rows(src)
    H = {h: i for i, h in enumerate(header)}
    b = Book(header, rows)
    loads = sorted(uniq(r[H["load_pct"]] for r in rows))
    for d in device_order(uniq(r[H["device"]] for r in rows)):
        for p in ("gpu", "cpu"):
            sel = {(r[H["load_pct"]], r[H["calm_ms"]]): r for r in rows if r[H["device"]] == d and r[H["processor"]] == p}
            if not sel:
                continue
            calms = sorted(uniq(c for _, c in sel))
            hdr, data = ["calm_s"], [[c / 1000] for c in calms]
            for L in loads:
                hdr += [f"P, burst {L} %", f"CI minus {L} %", f"CI plus {L} %"]
                for row, c in zip(data, calms):
                    r = sel.get((L, c))
                    P = r[H["P_median"]] if r else None
                    row += [P, P - r[H["P_ci_lo"]] if r else None, r[H["P_ci_hi"]] - P if r else None]
            r0, r1 = b.block(f"{fs.NAMES.get(d, d)} {PROC[p]}: onset penalty by calm length (s)", hdr, data)
            ch = b.chart(f"{fs.NAMES.get(d, d)}, {PROC[p]}", "calm length before the burst (s)", "onset penalty P",
                         xlog=True)
            for k, L in enumerate(loads):
                y = 2 + 3 * k
                b.series(ch, 1, y, r0, r1, f"burst {L} %", fs.LOAD_COLOR.get(L, fs.MUTED), fs.LOAD_MARKER.get(L, "o"),
                         err=(y + 1, y + 2))
            b.place(ch)
    b.save(out)


def fig4_idlegap(src, out):
    """Figure 4: idle-gap medians with IQR, one chart per measure (log-log), one series per device."""
    header, rows = read_rows(src)
    H = {}
    for i, h in enumerate(header):   # the CSV repeats "n"; the measures are named by their _med_us columns
        H.setdefault(h, i)
    b = Book(header, rows)
    devs = device_order(uniq(r[H["device"]] for r in rows))
    gaps = sorted(uniq(r[H["gap_us"]] for r in rows))
    for m, title in (("submit_to_start", "submit to GPU start"), ("gpu", "GPU execution"),
                     ("round_trip", "round trip")):
        hdr, data = ["gap_ms"], [[g / 1000] for g in gaps]
        for d in devs:
            hdr += [f"{fs.NAMES.get(d, d)} median (µs)", "IQR minus", "IQR plus"]
            byg = {r[H["gap_us"]]: r for r in rows if r[H["device"]] == d}
            for row, g in zip(data, gaps):
                r = byg.get(g)
                med = r[H[f"{m}_med_us"]] if r else None
                row += [med, med - r[H[f"{m}_q1_us"]] if r else None, r[H[f"{m}_q3_us"]] - med if r else None]
        r0, r1 = b.block(f"{title}: median and IQR (µs) by idle gap (ms)", hdr, data)
        ch = b.chart(title, "idle gap before submit (ms)", "time (µs, median and IQR)", xlog=True, ylog=True)
        for k, d in enumerate(devs):
            y = 2 + 3 * k
            b.series(ch, 1, y, r0, r1, fs.NAMES.get(d, d), fs.DEVICE_COLOR.get(d, fs.MUTED),
                     fs.DEVICE_MARKER.get(d, "o"), err=(y + 1, y + 2))
        b.place(ch)
    b.save(out)


FIGURES = [("kappa0_calm.csv", "fig_kappa0_calm.xlsx", fig1_kappa0_calm),
           ("kappa_class_medians.csv", "fig_kappa_class.xlsx", fig2_kappa_class),
           ("onset_penalty_cells.csv", "fig_onset_penalty.xlsx", fig3_onset_penalty),
           ("idlegap_medians.csv", "fig_idlegap.xlsx", fig4_idlegap)]


def main(argv=None):
    ap = argparse.ArgumentParser(description="Excel workbooks (data + native charts) for the paper figures.")
    ap.add_argument("--tables", default=str(ROOT / "results" / "paper_tables"))
    ap.add_argument("--out", default=str(ROOT / "paper" / "figures"))
    a = ap.parse_args(argv)
    for csv_name, xlsx_name, build in FIGURES:
        build(Path(a.tables) / csv_name, Path(a.out) / xlsx_name)


if __name__ == "__main__":
    main()
