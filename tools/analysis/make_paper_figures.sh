#!/bin/bash
# Rebuild every figure (paper/figures/fig_*.png + .pdf + .xlsx) and its CSV sidecar in results/paper_tables/
# from the fits and runs the draft cites. Rerun after every new E1 fit (the last legs), then paper/build_docx.py.
# Inputs are listed here on purpose: they are the paper's data, so a change here is a change to the paper.
set -euo pipefail
cd "$(dirname "$0")/../.."

# E1 fits (one JSON per device; §4 of the draft names the folders)
FITS=(
  results/m2_final_2026-10-30/e1_fit_ippro3.json
  results/m2_final_2026-10-30/e1_fit_ip12.json
  results/m2_final_2026-10-30/e1_fit_ipmini6.json
  results/m2_final_2026-10-30/e1_fit_poco.json
  results/m2_final_2026-10-30/e1_fit_tab.json
  results/m2_final_2026-10-30/e1_fit_s6lite.json
  results/m2_final_2026-10-30/e1_fit_a56.json
)
# Idle-gap runs (§4.5)
IDLEGAP=(
  results/ippro3_idlegap_all_0_20261029T194654Z
  results/ip12_idlegap_all_0_20261013T133140Z
  results/ipmini6_idlegap_all_0_20261016T082139Z
  results/poco_idlegap_all_0_20261010T192016Z
  results/tab_idlegap_all_0_20261016T060707Z
  results/s6lite_idlegap_all_0_20261014T171232Z
  results/a56_idlegap_all_0_20261024T103828Z
)

for f in "${FITS[@]}" "${IDLEGAP[@]}"; do
  [ -e "$f" ] || { echo "missing input: $f" >&2; exit 1; }
done

python3 tools/analysis/fig_kappa0_calm.py "${FITS[@]}"        # Figure 1, §4.2
python3 tools/analysis/fig_kappa_class.py "${FITS[@]}"        # Figure 2, §4.3 (reads the runs' frames.csv: ~1 min)
python3 tools/analysis/fig_onset_penalty.py "${FITS[@]}"      # Figure 3, §4.4
python3 tools/analysis/fig_idlegap.py "${IDLEGAP[@]}"         # Figure 4, §4.5
python3 tools/analysis/fig_excel.py                          # paper/figures/fig_*.xlsx from the CSVs above
echo "figures rebuilt; now: python3 paper/build_docx.py"
