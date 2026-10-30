// Forecaster exactness (docs/03 §5; M0 acceptance: "the forecaster is exact").
#include "ag/forecast.h"
#include "ag/level.h"

#include "doctest/doctest.h"

#include <memory>
#include <string>
#include <vector>

using namespace ag;

namespace {

Level load(const std::string& id) {
    Level lv;
    std::string err;
    REQUIRE_MESSAGE(load_level(std::string(AG_SOURCE_DIR) + "/bench/levels/" + id + ".json", lv, err), err);
    return lv;
}

} // namespace

TEST_CASE("forecast(state, h) equals the next h real descriptors, from every tick of a level") {
    for (const char* id : {"single_00250ms_large", "single_01000ms_medium", "repeat_3x_0250_1000ms"}) {
        const Level lv = load(id);
        std::vector<Descriptor> trace;
        run_level(lv, 1, &trace);
        auto s = std::make_unique<State>();
        init_state(lv, *s);
        auto sim = std::make_unique<Sim>(1);
        auto fc = std::make_unique<Forecaster>(2);   // the dial must not matter (D20)
        constexpr u32 h = 24;
        std::vector<Descriptor> out(h);
        std::vector<TickInput> inputs(h);
        u32 windows = 0, noInputWindows = 0;
        Descriptor d;
        while (s->tick + h <= trace.size()) {
            const u32 t = s->tick;
            bool anyInput = false;
            for (u32 i = 0; i < h; ++i) {
                inputs[i] = bot_input(lv, t + i);
                anyInput = anyInput || inputs[i].tapCount > 0;
            }
            const u64 before = state_hash(*s);
            REQUIRE(fc->forecast(*s, h, inputs.data(), out.data()) == h);
            CHECK(state_hash(*s) == before);   // the source state is untouched
            for (u32 i = 0; i < h; ++i) REQUIRE(out[i] == trace[t + i]);
            if (!anyInput) {
                // With no input in the window, the no-input forecast is exact too.
                fc->forecast(*s, h, nullptr, out.data());
                for (u32 i = 0; i < h; ++i) REQUIRE(out[i] == trace[t + i]);
                ++noInputWindows;
            }
            ++windows;
            sim->tick(*s, bot_input(lv, t), d);
        }
        CHECK(windows > 100);
        CHECK(noInputWindows > 50);
    }
}

TEST_CASE("forecast_tap from finger-down predicts the real burst exactly") {
    const Level lv = load("single_02000ms_large");
    REQUIRE(lv.taps.size() == 1);
    const TapSpec tap = lv.taps[0];
    std::vector<Descriptor> trace;
    run_level(lv, 1, &trace);
    auto s = std::make_unique<State>();
    init_state(lv, *s);
    Sim sim(1);
    Descriptor d;
    while (s->tick < tap.down) sim.tick(*s, bot_input(lv, s->tick), d);
    // At finger-down the policy knows the position; the release comes holdTicks later.
    Forecaster fc(1);
    std::vector<Descriptor> out(120);
    const u32 n = fc.forecast_tap(*s, 120, tap.up - tap.down, TapEvent{tap.x, tap.y}, out.data());
    for (u32 i = 0; i < n; ++i) REQUIRE(out[i] == trace[tap.down + i]);
    bool sawOnset = false;
    for (u32 i = 0; i < n; ++i) sawOnset = sawOnset || out[i].detonations > 0;
    CHECK(sawOnset);
}

TEST_CASE("forecast horizon is capped at kMaxHorizon") {
    const Level lv = load("single_00250ms_small");
    auto s = std::make_unique<State>();
    init_state(lv, *s);
    Forecaster fc;
    std::vector<Descriptor> out(Forecaster::kMaxHorizon);
    CHECK(fc.forecast(*s, Forecaster::kMaxHorizon + 50, nullptr, out.data()) == Forecaster::kMaxHorizon);
}
