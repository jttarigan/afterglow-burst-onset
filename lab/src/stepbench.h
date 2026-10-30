#pragma once
// Stepbench (docs/04 E1): synthetic step loads for RQ1. Each trial holds load L0 for T_calm, steps
// to L1 for 2 s, and returns to L0. Loads are sized in warm milliseconds, calibrated during the
// warm-up. The schedule is a pure function of the configuration and the frame start times, so it
// is unit-tested with a fake clock (tests/test_stepbench.cpp).
#include "ag/json.h"

#include <string>
#include <vector>

namespace ag::lab {

enum class SbTarget : u8 { Gpu = 1, Cpu = 2, Both = 3 };
enum class SbPhase : u8 { Calm = 0, Burst = 1, Warmup = 2, Done = 3 };

struct SbCell {
    u32 calmMs = 0;
    u32 loadPct = 0;
    SbTarget target = SbTarget::Gpu;
    bool ambient = false;   // L0: probes only, or probes + 10 % ambient
};

struct StepbenchConfig {
    std::vector<u32> calmsMs{50, 100, 250, 500, 1000, 2000, 4000, 8000};
    std::vector<u32> loadsPct{25, 50, 80};
    std::vector<SbTarget> targets{SbTarget::Gpu, SbTarget::Cpu, SbTarget::Both};
    std::vector<bool> l0Ambient{false, true};
    u32 repeats = 15;
    u64 seed = 1;                 // shuffle seed (logged, docs/04 §1)
    u32 trialFrom = 0;            // this run's slice of the shuffled trial list
    u32 trialTo = 0xFFFFFFFFu;
    u32 burstMs = 2000;
    u32 warmupLoadMs = 10000;     // warm-up at high load: calibration (R5)
    u32 warmupCalmMs = 10000;
    u32 primerMs = 2000;          // ends the warm-up with a burst, so trial 1's calm follows one (D22)
    u32 warmupLoadPct = 120;      // saturating: overruns the frame until the governor is at its top level (D42)
    u32 primerLoadPct = 50;
    u32 ambientPct = 10;
    i64 budgetNs = 16666667;      // 60 Hz frame budget
    u32 probeMultiplier = 1;      // E1 probe-size ablation: x1 or x4

    /// Parses --calms=50,100 --loads=25,50,80 --targets=gpu,cpu,both --l0=probes,ambient
    /// --repeats --seed --trials=a:b --burst-ms --warmup-ms --warmup-calm-ms --primer-ms --probe-mult.
    bool parse(const std::string& calms, const std::string& loads, const std::string& targets, const std::string& l0,
               std::string& err);
    Json toJson() const;
};

/// The full shuffled trial list of a configuration (all runs of a device session share it).
std::vector<SbCell> stepbench_trials(const StepbenchConfig& cfg);

/// What one frame must do.
struct SbFrame {
    SbPhase phase = SbPhase::Warmup;
    i64 trial = -1;               // index into the full trial list; -1 in warm-up
    SbCell cell;
    u32 gpuLoadPct = 0;           // of the warm frame budget
    u32 cpuLoadPct = 0;
    bool calibrate = false;       // warm-up load frames: update the ns-per-iteration estimate
};

class StepbenchSchedule {
public:
    explicit StepbenchSchedule(const StepbenchConfig& cfg);
    /// The plan for a frame that starts at tNs (lab clock, nondecreasing).
    SbFrame next(i64 tNs);
    bool done() const { return done_; }
    usize trialCount() const { return trials_.size(); }
    u32 firstTrial() const { return from_; }
    u32 lastTrial() const { return to_; }

private:
    StepbenchConfig cfg_;
    std::vector<SbCell> trials_;
    u32 from_ = 0, to_ = 0;
    int stage_ = 0;               // 0 warm-up load, 1 warm-up calm, 2 primer, 3 trials
    i64 stageStart_ = -1;
    u32 cur_ = 0;                 // current trial index
    bool inBurst_ = false;
    bool done_ = false;
};

} // namespace ag::lab
