#pragma once
// The gameplay simulation (docs/02 §2, docs/03 §3). Integer-only (R1): positions and velocities are
// Q8 fixed point (1/256 arena unit), radii are integer units, time is integer ticks at 60 Hz.
// A State is plain old data with no pointers, so a snapshot is a copy (docs/03 §3).
#include "ag/types.h"

#include <type_traits>

namespace ag {

// ---------------------------------------------------------------- rules (docs/02 §2)
inline constexpr i32 kArenaW = 1080;          // arena units (1 unit = 1/1080 of the width)
inline constexpr i32 kArenaH = 2340;
inline constexpr u32 kQ = 8;                  // Q8: 256 sub-units per arena unit
inline constexpr i32 kOrbRadius = 18;
inline constexpr i32 kTapRadius = 160;        // player blast
inline constexpr i32 kChainRadius = 120;      // chain blast
inline constexpr u32 kGrowTicks = 18;
inline constexpr u32 kHoldTicks = 36;
inline constexpr u32 kShrinkTicks = 18;
inline constexpr u32 kBlastLife = kGrowTicks + kHoldTicks + kShrinkTicks;  // 72 ticks
inline constexpr u32 kTickHz = 60;
inline constexpr i32 kMinSpeed = 30;          // units per second
inline constexpr i32 kMaxSpeed = 120;

inline constexpr u32 kMaxOrbs = 400;
inline constexpr u32 kMaxTaps = 8;
inline constexpr u32 kMaxBlasts = kMaxOrbs + kMaxTaps;
inline constexpr u32 kMaxTapsPerTick = 2;

// Broad phase: square cells of 256 units, larger than the largest reach (160 + 18), so a 3 x 3
// neighbourhood always holds every blast that can touch an orb.
inline constexpr u32 kCellShift = kQ + 8;     // Q8 coordinate >> 16 = cell index
inline constexpr i32 kGridW = (kArenaW + 255) / 256;   // 5
inline constexpr i32 kGridH = (kArenaH + 255) / 256;   // 10
static_assert(kTapRadius + kOrbRadius < 256, "a 3x3 cell neighbourhood must cover the largest reach");

enum class BlastKind : u8 { Tap = 0, Chain = 1 };

struct Orb {
    i32 x = 0, y = 0;      // Q8 position
    i32 vx = 0, vy = 0;    // Q8 per tick
    u8 colour = 0;
    u8 alive = 0;          // 0 = detonated (or unused slot)
    u16 pad = 0;
};

struct Blast {
    i32 x = 0, y = 0;      // Q8 position (fixed for the blast's life)
    u32 birth = 0;         // first tick the blast is tested; its age is tick - birth
    i32 rmax = 0;          // full radius, units
    i16 parent = -1;       // blast index of the parent (chain graph); -1 for a tap
    u8 gen = 0;            // generations from the tap (combo "xN")
    BlastKind kind = BlastKind::Tap;
};

/// The whole gameplay state. Blasts are append-only in creation order; all share one lifetime,
/// so the live blasts are always the contiguous range [firstLive, blastCount).
struct State {
    u32 tick = 0;              // the next tick to run
    u32 orbCount = 0;
    u32 blastCount = 0;
    u32 firstLive = 0;
    u32 tapsAllowed = 0;
    u32 tapsUsed = 0;
    u32 detonationsTotal = 0;
    u32 pad = 0;
    Orb orbs[kMaxOrbs];
    Blast blasts[kMaxBlasts];
};
static_assert(std::is_trivially_copyable_v<State>, "snapshot/restore is a plain copy");

/// One tap released on this tick (docs/02 §2: the blast appears at the finger-release point).
struct TapEvent {
    i32 x = 0, y = 0;      // arena units; clamped into the arena
};

struct TickInput {
    u32 tapCount = 0;
    TapEvent taps[kMaxTapsPerTick];
};

/// Workload descriptor (docs/03 §4): fixed size, emitted every tick, preset-independent. The
/// renderer derives every GPU work size from the stream of these plus the preset (workload.h).
struct Descriptor {
    u32 tick = 0;
    u16 liveOrbs = 0;      // idle orbs still drifting
    u16 liveBlasts = 0;    // blasts alive on this tick (tap + chain)
    u16 detonations = 0;   // orbs that detonated on this tick
    u16 taps = 0;          // player blasts placed on this tick
    u16 liveArcs = 0;      // live chain blasts; each draws an arc to its parent
    u16 chainDepth = 0;    // highest generation among live blasts
};
inline constexpr usize kDescriptorBytes = 16;   // serialised size (descriptor_bytes)

/// Little-endian serialisation used for the workload-trace hash.
void descriptor_bytes(const Descriptor& d, u8 out[kDescriptorBytes]);
bool operator==(const Descriptor& a, const Descriptor& b);

/// Radius of a blast at a given age, in Q8 units (docs/02 §2 lifecycle). Age >= kBlastLife is dead.
i64 blast_radius_q8(i32 rmaxUnits, u32 age);

/// Initial velocity for a speed in units/s along an integer direction (dx, dy), in Q8 per tick.
/// Integer-only: the direction is normalised with isqrt.
void orb_velocity(i32 speedUnitsPerS, i32 dx, i32 dy, i32& vx, i32& vy);

class Sim {
public:
    /// collisionPasses is the "simulation sub-steps" workload dial (docs/02 §6): the detonation
    /// pass runs this many times per tick on the same state. Every pass must agree, so the cascade
    /// never depends on the dial (D20); disagreement is counted in passMismatches().
    explicit Sim(u32 collisionPasses = 1);

    void tick(State& s, const TickInput& in, Descriptor& out);
    u32 passMismatches() const { return mismatches_; }

private:
    u32 detonationPass(const State& s, u16* orbIdx, i16* parents);

    u32 passes_ = 1;
    u32 mismatches_ = 0;
    // Scratch (not state): grid of live blasts, and the detonation lists of each pass.
    u16 cellStart_[kGridW * kGridH + 1] = {};
    u16 cellItems_[kMaxBlasts] = {};
    u16 detOrb_[kMaxOrbs] = {};
    i16 detParent_[kMaxOrbs] = {};
    u16 chkOrb_[kMaxOrbs] = {};
    i16 chkParent_[kMaxOrbs] = {};
};

/// Level end (docs/02 §2): no taps left and no blast alive.
inline bool finished(const State& s) { return s.tapsUsed >= s.tapsAllowed && s.firstLive == s.blastCount; }

/// Order-independent state hash (NeonTide D7 scheme, hash.h).
u64 state_hash(const State& s);

} // namespace ag
