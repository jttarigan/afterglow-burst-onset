// Presets and GPU-work derivation (docs/02 §6, docs/03 §4): a pure function of descriptors + preset.
#include "ag/level.h"
#include "ag/workload.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

using namespace ag;

TEST_CASE("the four named presets exist and match the dial values of docs/02 §6") {
    for (const char* n : {"research_low", "research_mid", "research_high", "play_default"}) {
        const Preset* p = find_preset(n);
        REQUIRE(p != nullptr);
        CHECK((p->sparksPerDetonation == 64 || p->sparksPerDetonation == 256 || p->sparksPerDetonation == 1024));
        CHECK((p->sparkLifeTicks == 30 || p->sparkLifeTicks == 60));
        CHECK((p->smokeW == 0 || (p->smokeW == 64 && p->smokeH == 128) || (p->smokeW == 128 && p->smokeH == 256)));
        CHECK((p->jacobiIters == 13 || p->jacobiIters == 26));
        CHECK((p->bloomMips == 0 || p->bloomMips == 4 || p->bloomMips == 6));
        CHECK((p->renderScalePct == 50 || p->renderScalePct == 75 || p->renderScalePct == 100));
        CHECK((p->collisionPasses == 1 || p->collisionPasses == 2 || p->collisionPasses == 4));
        CHECK((p->frameCap == 60 || p->frameCap == 30));
    }
    CHECK(find_preset("nope") == nullptr);
    CHECK(find_preset("research_mi") == nullptr);
}

TEST_CASE("live sparks are the spawns of the last 'life' ticks; smoke runs only while blasts live") {
    Preset p = *find_preset("research_mid");
    p.sparkLifeTicks = 3;
    p.ambientSparks = 0;
    WorkTracker wt(p);
    const u16 dets[] = {1, 2, 0, 0, 5, 0, 0, 0};
    const u32 want[] = {1, 3, 3, 2, 5, 5, 5, 0};
    for (usize i = 0; i < 8; ++i) {
        Descriptor d;
        d.tick = static_cast<u32>(i);
        d.detonations = dets[i];
        d.liveBlasts = i < 7 ? 1 : 0;
        const GpuWork g = wt.step(d);
        CHECK(g.sparksSpawn == dets[i] * p.sparksPerDetonation);
        CHECK(g.liveSparks == want[i] * p.sparksPerDetonation);
        CHECK(g.smokeOn == (i < 7 ? 1u : 0u));
    }
}

TEST_CASE("derived GPU work depends only on the descriptor stream and the preset") {
    Level lv;
    std::string err;
    REQUIRE(load_level(std::string(AG_SOURCE_DIR) + "/bench/levels/single_01000ms_large.json", lv, err));
    std::vector<Descriptor> a, b;
    run_level(lv, 1, &a);
    run_level(lv, 4, &b);
    REQUIRE(a.size() == b.size());
    WorkTracker w1(*find_preset("research_high")), w2(*find_preset("research_high"));
    u32 peakSparks = 0;
    for (usize i = 0; i < a.size(); ++i) {
        const GpuWork g1 = w1.step(a[i]);
        const GpuWork g2 = w2.step(b[i]);
        CHECK(g1.liveSparks == g2.liveSparks);
        CHECK(g1.sdfInstances == g2.sdfInstances);
        CHECK(g1.smokeOn == g2.smokeOn);
        if (g1.liveSparks > peakSparks) peakSparks = g1.liveSparks;
    }
    CHECK(peakSparks > 0);
}
