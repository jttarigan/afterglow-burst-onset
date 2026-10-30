"""Shared look for the paper figures (tools/analysis/fig_*.py).

Colours are the dataviz reference palette (validated 2026-10-21/22 with its validate_palette.js: the five
device slots pass the adjacent-pair gates on white; aqua, yellow and magenta sit below 3:1 contrast, so every
figure that uses them also carries a legend, a marker shape per device and a CSV table). Device hues are fixed
per device, never assigned by position, so a device keeps its colour when the set of devices changes.
"""
import matplotlib.pyplot as plt

NAMES = {"ip12": "iPhone 12 (A14)", "ipmini6": "iPad mini 6 (A15)", "ippro3": "iPad Pro 3 (A12X)",
         "poco": "POCO X6 Pro (Dimensity 8300)", "tab": "Galaxy Tab S7+ (SD865+)",
         "s6lite": "Galaxy Tab S6 Lite (Exynos 9611)", "a56": "Galaxy A56 (Exynos 1580)",
         "fold8": "Galaxy Z Fold 8 (SD 8 Elite Gen 5)"}
SHORT = {"ip12": "A14", "ipmini6": "A15", "ippro3": "A12X", "poco": "Dimensity 8300", "tab": "SD865+",
         "s6lite": "Exynos 9611", "a56": "Exynos 1580", "fold8": "SD 8 Elite Gen 5"}
GPU = {"ip12": "Apple GPU", "ipmini6": "Apple GPU", "ippro3": "Apple GPU", "poco": "Mali-G615", "tab": "Adreno 650",
       "s6lite": "Mali-G72", "a56": "Xclipse 540", "fold8": "Adreno 840"}

# categorical slots 1-8 of the reference palette, light surface
SLOTS = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]
DEVICE_COLOR = {"ip12": SLOTS[0], "ipmini6": SLOTS[1], "poco": SLOTS[2], "tab": SLOTS[3], "s6lite": SLOTS[4],
                "ippro3": SLOTS[5], "a56": SLOTS[6], "fold8": SLOTS[7]}
DEVICE_MARKER = {"ip12": "o", "ipmini6": "s", "poco": "^", "tab": "D", "s6lite": "v", "ippro3": "P", "a56": "X",
                 "fold8": "*"}
# series for "burst size" (three levels: all-pairs safe)
LOAD_COLOR = {25: SLOTS[0], 50: SLOTS[1], 80: SLOTS[2]}
LOAD_MARKER = {25: "o", 50: "s", 80: "^"}

INK, MUTED, GRID, SPINE, FAINT = "#0b0b0b", "#52514e", "#e1e0d9", "#c3c2b7", "#b8b7ad"


def setup():
    plt.rcParams["font.family"] = "sans-serif"
    plt.rcParams["axes.unicode_minus"] = False


def style_axis(ax, title=None):
    if title is not None:
        ax.set_title(title, fontsize=7, color=INK, loc="left", pad=3)
    ax.grid(True, color=GRID, lw=0.5)
    ax.set_axisbelow(True)
    ax.tick_params(labelsize=6, colors=MUTED, length=2)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(SPINE)


def calm_axis(ax):
    """Calm length on a log axis, 50 ms to 8 s, ticks at the grid's values."""
    ax.set_xscale("log")
    ax.set_xlim(0.04, 10)
    ax.set_xticks([0.05, 0.1, 0.25, 0.5, 1, 2, 4, 8])
    ax.set_xticklabels(["0.05", "0.1", "0.25", "0.5", "1", "2", "4", "8"])
    ax.minorticks_off()
