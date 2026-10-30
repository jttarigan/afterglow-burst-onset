#pragma once
// Research level generator (docs/02 §3, aglab --mode=levelgen). Searches seeds and tap ticks and
// positions headlessly until each burst starts exactly at its calm length (onset tick) and its peak
// live-blast count lies in its class band. Integer-only and single-threaded, so the output is a
// pure function of the spec (R1).
#include "ag/json.h"
#include "ag/level.h"

#include <string>
#include <vector>

namespace ag::lab {

struct BurstSpec {
    u32 calmTicks = 0;      // from the end of the previous burst (or level start) to the onset
    std::string cls;        // small, medium, large
    u32 bandLo = 0, bandHi = 0;   // peak live blasts, inclusive
};

struct LevelSpec {
    std::string id;
    std::string kind;       // research_single, research_repeat
    u32 orbsLo = 0, orbsHi = 0;
    std::vector<BurstSpec> bursts;
};

struct GenConfig {
    u64 seedBase = 1;
    u32 holdTicks = 7;          // finger-down to finger-up (120 ms, docs/02 §4)
    u32 maxAttempts = 400;      // seeds tried per level
    u32 positionStep = 30;      // tap-position grid, arena units
    u32 maxCascadesPerBurst = 96;
    u32 marginTicks = 600;      // max_ticks = expected ticks + margin
};

/// Reads a levelgen spec (bench/levels/levelgen_spec.json) into level specs and a config.
bool parse_levelgen_spec(const Json& j, GenConfig& cfg, std::vector<LevelSpec>& levels, std::string& err);

/// Generates one level. Returns false (with a reason) if no seed within maxAttempts fits.
bool generate_level(const GenConfig& cfg, const LevelSpec& spec, u32 levelIndex, Level& out, std::string& err);

/// Random orb field (docs/02 §2): n orbs, uniform positions, speeds 30-120 units/s, 4 colours.
std::vector<Orb> make_orbs(u64 seed, u64 stream, u32 n);

/// Exploration: for each orb count, the distribution of peak live blasts from random taps.
/// Prints a table; used once to choose the class bands (D23).
void explore_bursts(u32 nLo, u32 nHi, u32 nStep, u32 samples, u64 seedBase);

} // namespace ag::lab
