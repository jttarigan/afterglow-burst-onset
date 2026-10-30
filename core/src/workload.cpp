#include "ag/workload.h"

namespace ag {
namespace {

// Provisional values (D24): research_low/mid/high step every GPU dial together; play_default is
// research_mid with ambient sparks. M3 recalibrates them against device timings.
constexpr Preset kPresets[] = {
    {"research_low", 64, 30, 64, 128, 13, 4, 75, 1, 60, 0},
    {"research_mid", 256, 60, 64, 128, 26, 4, 100, 2, 60, 0},
    {"research_high", 1024, 60, 128, 256, 26, 6, 100, 4, 60, 0},
    {"play_default", 256, 60, 64, 128, 26, 4, 100, 1, 60, 512},
};

constexpr bool str_eq(const char* a, std::string_view b) {
    usize i = 0;
    for (; a[i] != '\0'; ++i) {
        if (i >= b.size() || a[i] != b[i]) return false;
    }
    return i == b.size();
}

} // namespace

const Preset* find_preset(std::string_view name) {
    for (const Preset& p : kPresets) {
        if (str_eq(p.name, name)) return &p;
    }
    return nullptr;
}

GpuWork WorkTracker::step(const Descriptor& d) {
    GpuWork w;
    const u32 life = p_.sparkLifeTicks == 0 ? 1u : (p_.sparkLifeTicks > kMaxSparkLife ? kMaxSparkLife : p_.sparkLifeTicks);
    const u32 newBlasts = u32{d.detonations} + d.taps;
    w.sparksSpawn = newBlasts * p_.sparksPerDetonation;
    // Sparks live exactly 'life' ticks: a ring buffer of spawn counts gives the live count.
    live_ -= spawn_[head_];
    spawn_[head_] = w.sparksSpawn;
    live_ += w.sparksSpawn;
    head_ = (head_ + 1u) % life;
    w.liveSparks = live_ + (d.liveBlasts == 0 ? p_.ambientSparks : 0u);
    w.smokeOn = (d.liveBlasts > 0 && p_.smokeW > 0) ? 1u : 0u;
    w.smokeSplats = w.smokeOn != 0 ? newBlasts : 0u;
    w.smokeCells = w.smokeOn != 0 ? p_.smokeW * p_.smokeH : 0u;
    w.jacobiIters = w.smokeOn != 0 ? p_.jacobiIters : 0u;
    w.sdfInstances = u32{d.liveOrbs} + d.liveBlasts + d.liveArcs;
    w.bloomMips = p_.bloomMips;
    w.internalW = kInternalW * p_.renderScalePct / 100u;
    w.internalH = kInternalH * p_.renderScalePct / 100u;
    return w;
}

} // namespace ag
