#include "ag/forecast.h"

namespace ag {

u32 Forecaster::forecast(const State& from, u32 h, const TickInput* inputs, Descriptor* out) {
    const u32 n = h < kMaxHorizon ? h : kMaxHorizon;
    scratch_ = from;
    const TickInput none{};
    for (u32 i = 0; i < n; ++i) sim_.tick(scratch_, inputs != nullptr ? inputs[i] : none, out[i]);
    return n;
}

u32 Forecaster::forecast_tap(const State& from, u32 h, u32 releaseIn, TapEvent tap, Descriptor* out) {
    const u32 n = h < kMaxHorizon ? h : kMaxHorizon;
    scratch_ = from;
    const TickInput none{};
    TickInput withTap{};
    withTap.tapCount = 1;
    withTap.taps[0] = tap;
    for (u32 i = 0; i < n; ++i) sim_.tick(scratch_, i == releaseIn ? withTap : none, out[i]);
    return n;
}

} // namespace ag
