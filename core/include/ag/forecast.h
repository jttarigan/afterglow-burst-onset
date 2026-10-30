#pragma once
// Look-ahead forecaster (docs/03 §5): copy the state, run tick() h times on the copy, return the
// descriptors. Because the simulation is deterministic, the forecast is exact whenever the inputs
// it is given are the inputs that actually arrive (doctest: tests/test_forecast.cpp).
#include "ag/sim.h"

namespace ag {

class Forecaster {
public:
    static constexpr u32 kMaxHorizon = 240;   // ticks (4 s); P4 uses at most 24 (400 ms)

    explicit Forecaster(u32 collisionPasses = 1) : sim_(collisionPasses) {}

    /// Runs min(h, kMaxHorizon) ticks on a copy of 'from'. 'inputs' is null (no input) or holds one
    /// TickInput per forecast tick. Writes one descriptor per tick to 'out'; returns the count.
    /// Allocation-free: the scratch state lives in the forecaster.
    u32 forecast(const State& from, u32 h, const TickInput* inputs, Descriptor* out);

    /// Speculative tap (docs/03 §5 trigger on finger-down): as forecast(), with a tap at (x, y)
    /// released 'releaseIn' ticks from now and no other input.
    u32 forecast_tap(const State& from, u32 h, u32 releaseIn, TapEvent tap, Descriptor* out);

private:
    Sim sim_;
    State scratch_{};
};

} // namespace ag
