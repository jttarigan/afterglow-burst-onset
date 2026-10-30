// Stepbench schedule (docs/04 E1) with a fake 60 Hz clock.
#include "stepbench.h"

#include "doctest/doctest.h"

#include <cmath>
#include <map>
#include <tuple>

using namespace ag;
using namespace ag::lab;

namespace {

constexpr i64 kFrame = 16666667;

StepbenchConfig small() {
    StepbenchConfig c;
    c.calmsMs = {50, 1000};
    c.loadsPct = {50};
    c.targets = {SbTarget::Gpu, SbTarget::Both};
    c.l0Ambient = {false, true};
    c.repeats = 3;
    c.warmupLoadMs = 1000;
    c.warmupCalmMs = 500;
    c.primerMs = 200;
    c.burstMs = 2000;
    return c;
}

} // namespace

TEST_CASE("trial list: every cell 'repeats' times, shuffled deterministically by the seed") {
    const StepbenchConfig c = small();
    const auto a = stepbench_trials(c);
    const auto b = stepbench_trials(c);
    REQUIRE(a.size() == 2 * 1 * 2 * 2 * 3);
    std::map<std::tuple<u32, u32, int, bool>, int> count;
    for (usize i = 0; i < a.size(); ++i) {
        CHECK(a[i].calmMs == b[i].calmMs);
        CHECK(a[i].target == b[i].target);
        ++count[{a[i].calmMs, a[i].loadPct, static_cast<int>(a[i].target), a[i].ambient}];
    }
    CHECK(count.size() == 8);
    for (const auto& kv : count) CHECK(kv.second == 3);
    StepbenchConfig other = c;
    other.seed = 2;
    const auto d = stepbench_trials(other);
    bool differs = false;
    for (usize i = 0; i < a.size(); ++i) differs = differs || a[i].calmMs != d[i].calmMs || a[i].target != d[i].target;
    CHECK(differs);
}

TEST_CASE("schedule: warm-up load, warm-up calm, primer, then calm and 2 s burst per trial") {
    const StepbenchConfig c = small();
    StepbenchSchedule s(c);
    const auto trials = stepbench_trials(c);
    i64 t = 1000000000;
    int warmLoad = 0, warmCalm = 0, primer = 0;
    std::map<i64, int> calmFrames, burstFrames;
    SbFrame f;
    for (int i = 0; i < 100000; ++i, t += kFrame) {
        f = s.next(t);
        if (f.phase == SbPhase::Done) break;
        if (f.phase == SbPhase::Warmup) {
            if (f.calibrate) {
                ++warmLoad;
                CHECK(f.gpuLoadPct == c.warmupLoadPct);
            } else if (f.gpuLoadPct == 0) {
                ++warmCalm;
            } else {
                ++primer;
                CHECK(f.gpuLoadPct == c.primerLoadPct);
                CHECK(f.cpuLoadPct == c.primerLoadPct);
            }
            continue;
        }
        const SbCell& cell = trials[static_cast<usize>(f.trial)];
        if (f.phase == SbPhase::Calm) {
            ++calmFrames[f.trial];
            CHECK(f.gpuLoadPct == (cell.ambient ? c.ambientPct : 0u));
        } else {
            ++burstFrames[f.trial];
            CHECK(f.gpuLoadPct == cell.loadPct);
            CHECK(f.cpuLoadPct == (cell.target == SbTarget::Both ? cell.loadPct : 0u));
        }
    }
    CHECK(f.phase == SbPhase::Done);
    CHECK(s.done());
    CHECK(warmLoad == 60);   // 1 s
    CHECK(warmCalm == 30);   // 0.5 s
    CHECK(primer == 12);     // 0.2 s
    REQUIRE(calmFrames.size() == trials.size());
    for (const auto& kv : calmFrames) {
        // Calm length within half a frame of the nominal value (phase edges snap to frames).
        const double ms = kv.second * 1000.0 / 60.0;
        CHECK(std::abs(ms - trials[static_cast<usize>(kv.first)].calmMs) <= 1000.0 / 120.0 + 1e-9);
    }
    for (const auto& kv : burstFrames) CHECK(kv.second == 120);
}

TEST_CASE("a run executes only its slice of the trial list") {
    StepbenchConfig c = small();
    c.trialFrom = 5;
    c.trialTo = 7;
    StepbenchSchedule s(c);
    i64 t = 0;
    std::map<i64, int> seen;
    for (int i = 0; i < 100000 && !s.done(); ++i, t += kFrame) {
        const SbFrame f = s.next(t);
        if (f.trial >= 0) ++seen[f.trial];
    }
    REQUIRE(seen.size() == 2);
    CHECK(seen.begin()->first == 5);
    CHECK(seen.rbegin()->first == 6);
}

TEST_CASE("config parsing") {
    StepbenchConfig c;
    std::string err;
    CHECK(c.parse("100,1000,4000", "50", "gpu,cpu", "probes", err));
    CHECK(c.calmsMs.size() == 3);
    CHECK(c.targets.size() == 2);
    CHECK(c.l0Ambient.size() == 1);
    CHECK_FALSE(c.parse("", "", "gpu,xpu", "", err));
    CHECK_FALSE(c.parse("", "0", "", "", err));
}
