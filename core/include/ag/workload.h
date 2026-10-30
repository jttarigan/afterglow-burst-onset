#pragma once
// Workload presets (docs/02 §6) and the derivation of GPU work sizes from the descriptor stream
// (docs/03 §4). Everything here is integer and deterministic, so two devices running the same
// level and preset issue identical work, dispatch for dispatch (R1). The renderer (M3) reads
// GpuWork and nothing else.
#include "ag/sim.h"

#include <string_view>

namespace ag {

struct Preset {
    const char* name = "";
    u32 sparksPerDetonation = 0;   // 64 / 256 / 1024
    u32 sparkLifeTicks = 0;        // 30 / 60 (0.5 / 1.0 s)
    u32 smokeW = 0, smokeH = 0;    // fluid grid; 0 = off; 64x128 or 128x256
    u32 jacobiIters = 0;           // 13 / 26
    u32 bloomMips = 0;             // 0 = off, 4, 6
    u32 renderScalePct = 100;      // 50 / 75 / 100 of 720 x 1560
    u32 collisionPasses = 1;       // 1 / 2 / 4 (CPU dial, D20)
    u32 frameCap = 60;             // 60 / 30
    u32 ambientSparks = 0;         // constant drifting sparks in calm stretches (0 = probes only)
};

inline constexpr u32 kInternalW = 720;    // fixed internal resolution at scale 1.0 (D7)
inline constexpr u32 kInternalH = 1560;
inline constexpr u32 kMaxSparkLife = 60;

/// Named presets: research_low, research_mid, research_high, play_default. Values are provisional
/// until M3 calibrates them on devices, then frozen with the level set (D24). nullptr if unknown.
const Preset* find_preset(std::string_view name);

/// GPU work for one tick. A pure function of the descriptor stream so far and the preset.
struct GpuWork {
    u32 sparksSpawn = 0;
    u32 liveSparks = 0;
    u32 smokeOn = 0;               // smoke runs only while blasts are alive (docs/02 §6)
    u32 smokeSplats = 0;           // one per new blast (tap or chain)
    u32 smokeCells = 0;
    u32 jacobiIters = 0;
    u32 sdfInstances = 0;          // idle orbs + live blasts + arcs, one instanced draw
    u32 bloomMips = 0;
    u32 internalW = 0, internalH = 0;
};

class WorkTracker {
public:
    explicit WorkTracker(const Preset& p) : p_(p) {}
    GpuWork step(const Descriptor& d);

private:
    Preset p_;
    u32 spawn_[kMaxSparkLife] = {};
    u32 head_ = 0;
    u32 live_ = 0;
};

} // namespace ag
