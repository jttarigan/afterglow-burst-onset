#pragma once
// Level files (docs/02 §3, docs/03 §12), the open-loop bot (docs/02 §4), burst detection, and the
// headless level runner that produces the workload trace and its hash (R1).
#include "ag/json.h"
#include "ag/sim.h"

#include <string>
#include <vector>

namespace ag {

/// One scheduled tap: finger down, then finger up (the release places the blast).
struct TapSpec {
    u32 down = 0;          // tick of finger-down (the touch-down lead available to policies)
    u32 up = 0;            // tick of finger-up
    i32 x = 0, y = 0;      // arena units
};

/// One burst: an interval of ticks with live blasts. The onset is its first tick with a
/// detonation (docs/03 §4); the end is the first following tick with no live blast.
struct BurstStats {
    u32 start = 0;         // first tick with a live blast
    u32 onset = 0;         // first tick with detonations > 0 (== kNoOnset if the tap hit nothing)
    u32 end = 0;           // first tick after the burst with liveBlasts == 0
    u32 peakLiveBlasts = 0;
    u32 peakDetonations = 0;
    u32 detonations = 0;
};
inline constexpr u32 kNoOnset = 0xFFFFFFFFu;

/// Splits a descriptor stream into bursts. Feed every descriptor in tick order.
class BurstTracker {
public:
    void feed(const Descriptor& d);
    const std::vector<BurstStats>& bursts() const { return bursts_; }

private:
    std::vector<BurstStats> bursts_;
    bool open_ = false;
};

struct Level {
    std::string id;
    std::string kind;                 // "research_single", "research_repeat", "play"
    u64 seed = 0;
    u32 tapsAllowed = 1;
    u32 target = 0;                   // detonations needed ("CHAIN 47 / 60")
    u32 maxTicks = 0;                 // safety cap for the runner
    std::vector<Orb> orbs;            // initial layout (x, y, vx, vy, colour; all alive)
    std::vector<TapSpec> taps;        // the bot's schedule
    Json design = Json::object();     // generator inputs, copied through (calm lengths, class, band)
    // Expected results, written by levelgen and checked by every run (R1).
    std::string expectedTraceSha256;
    u32 expectedTicks = 0;
    u32 expectedDetonations = 0;
    std::vector<BurstStats> expectedBursts;
};

bool level_from_json(const Json& j, Level& out, std::string& err);
Json level_to_json(const Level& lv);
bool load_level(const std::string& path, Level& out, std::string& err);

/// Fresh state for a level: its orbs, its tap count, tick 0.
void init_state(const Level& lv, State& s);

/// The bot's input for one tick: every tap whose finger-up is on this tick.
TickInput bot_input(const Level& lv, u32 tick);

struct TraceResult {
    std::string sha256;               // SHA-256 of the serialised descriptor stream
    u32 ticks = 0;
    u32 detonations = 0;
    std::vector<BurstStats> bursts;
    u32 passMismatches = 0;           // must be 0 (D20)
    bool finished = false;            // false if maxTicks was reached first
};

/// Runs a level headless from tick 0 with the bot until it ends (or maxTicks). If 'trace' is not
/// null it receives every descriptor. 'stateHashEvery' > 0 records state_hash every N ticks.
TraceResult run_level(const Level& lv, u32 collisionPasses, std::vector<Descriptor>* trace = nullptr,
                      u32 stateHashEvery = 0, std::vector<u64>* stateHashes = nullptr);

} // namespace ag
