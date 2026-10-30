# afterglow-burst-onset

Source code for measuring the **burst-onset penalty** on stock mobile devices: how much slower the first
frames of a sudden burst of CPU or GPU work run after a calm period, while the operating system's power
management (DVFS governors, idle states) brings the clocks back up, and how long the clocks stay up afterwards.

The workload comes from **Afterglow**, a native chain-reaction puzzle game written for this purpose. Its gameplay
simulation is integer-only, so the amount of work per frame is identical on every device. The measurements
themselves use a step benchmark (`stepbench`): a fixed calm period followed by a fixed load, repeated over a
grid of calm lengths, load sizes and targets (CPU, GPU or both), with always-on clock probes on both processors.

No root and no private APIs are used. Everything runs on stock commercial phones and tablets.

The measured data (per-run frame logs, provenance, model fits and tables) is published separately on Zenodo
as `afterglow-burst-onset-data`.

## Layout

| Path | Contents |
|---|---|
| `core/` | Integer simulation: orbs, blasts, chain graph, RNG, levels, hashing, workload descriptor |
| `shaders/` | Slang kernels (clock probe, load), compiled offline to Metal and SPIR-V 1.3 |
| `gfx/` | Thin compute layer: Metal (metal-cpp) and Vulkan 1.1, GPU timestamps |
| `lab/` | `aglab` runner: selftest, sim, levelgen, stepbench, E0 checks; frame log, provenance, CSV writers |
| `platform/ios/`, `platform/android/` | iOS/iPadOS (Swift, UIScene, Metal) and Android (Kotlin, NDK, Vulkan) hosts |
| `bench/levels/` | Generated research level set with a hash manifest |
| `tools/device/` | Device run scripts and the shuffled, resumable campaign runner; campaign specs |
| `tools/analysis/` | Run validation, step-response fits with bootstrap CIs, tables and figures |
| `tests/` | C++ unit tests (doctest) |

## Build

Requirements: CMake ≥ 3.25, Ninja, a C++20 compiler; Xcode for iOS; Android SDK + NDK for Android;
Python 3 for the analysis (`pip install -r tools/analysis/requirements.txt`). Slang, doctest, metal-cpp and the
Vulkan headers are fetched by CMake at pinned versions with SHA-256 checks (`cmake/AgDependencies.cmake`).

```sh
cmake --preset mac-release && cmake --build --preset mac-release     # aglab + tests on macOS
ctest --preset mac-release
cmake --preset ios-release                                           # then build platform/ios in Xcode
(cd platform/android && ./gradlew assembleRelease)                   # Android app (arm64-v8a)
```

The simulation is compiled with `-ffp-contract=off` and without fast-math; `tools/check_core_numeric.py` and
`tools/check_spirv_contraction.py` check this.

For iOS, set your own signing team in Xcode (`DEVELOPMENT_TEAM` is empty here). The device scripts take iOS
device UDIDs from the environment (`AG_UDID_IP12`, `AG_UDID_IPMINI6`, `AG_UDID_IPPRO3`, `AG_UDID_KS`) and Android
devices from `adb devices`.

## Running a measurement

```sh
tools/device/run_matrix.py tools/device/campaigns/m2_e1.json --device <device-id>   # E1 step response, shuffled
python3 tools/analysis/validate.py results/ --out validity.csv                        # run validity rules
python3 tools/analysis/e1_fit.py results/<device>_e1_* --device <device-id> --boot 1000 --out e1_fit_<device>.json
```

Every run writes a provenance block (`run.json`: device, SoC, OS build, display refresh, power source, battery,
thermal state at start and end, brightness, build commit and flags, workload parameters). A run that is
incomplete, was built in Debug, was interrupted or whose requested conditions did not apply is marked invalid.

## Notes

- Comments refer to the project's internal design notes (`docs/…`, decision numbers `Dnn`) and to the
  authors' earlier, unpublished projects from which some build and device code was adapted. Those notes are
  not part of this release; the code and the data README describe everything needed to run and analyse it.
- Each measured run records the development-repository commit it was built from (`build.git_commit` in
  `run.json`). This repository is a snapshot of that code.
- The analysis tests run with `PYTHONPATH=tools/analysis python3 -m unittest discover -s tools/analysis/tests`.

## Authors

Jos Timanta Tarigan (ORCID 0000-0001-9433-2265), Universitas Sumatera Utara
Suherman (ORCID 0000-0002-7375-4626), Universitas Sumatera Utara

Software, measurement orchestration and analysis code were developed with Claude Code (Anthropic) under the
authors' direction.

## Licence

MIT, see `LICENSE`. Third-party code fetched at build time keeps its own licence.
