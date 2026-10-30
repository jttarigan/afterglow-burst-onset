#include "ag/sim.h"

#include "ag/fixed.h"
#include "ag/hash.h"

#include <cstring>

namespace ag {

void descriptor_bytes(const Descriptor& d, u8 out[kDescriptorBytes]) {
    auto put16 = [&](usize at, u16 v) {
        out[at] = static_cast<u8>(v);
        out[at + 1] = static_cast<u8>(v >> 8u);
    };
    out[0] = static_cast<u8>(d.tick);
    out[1] = static_cast<u8>(d.tick >> 8u);
    out[2] = static_cast<u8>(d.tick >> 16u);
    out[3] = static_cast<u8>(d.tick >> 24u);
    put16(4, d.liveOrbs);
    put16(6, d.liveBlasts);
    put16(8, d.detonations);
    put16(10, d.taps);
    put16(12, d.liveArcs);
    put16(14, d.chainDepth);
}

bool operator==(const Descriptor& a, const Descriptor& b) {
    return a.tick == b.tick && a.liveOrbs == b.liveOrbs && a.liveBlasts == b.liveBlasts &&
           a.detonations == b.detonations && a.taps == b.taps && a.liveArcs == b.liveArcs &&
           a.chainDepth == b.chainDepth;
}

i64 blast_radius_q8(i32 rmaxUnits, u32 age) {
    const i64 r = i64{rmaxUnits} << kQ;
    if (age < kGrowTicks) return r * age / kGrowTicks;
    if (age < kGrowTicks + kHoldTicks) return r;
    if (age < kBlastLife) return r * (kBlastLife - age) / kShrinkTicks;
    return -1;
}

void orb_velocity(i32 speedUnitsPerS, i32 dx, i32 dy, i32& vx, i32& vy) {
    const i64 len = static_cast<i64>(isqrt64(static_cast<u64>(i64{dx} * dx + i64{dy} * dy)));
    if (len == 0) {
        vx = 0;
        vy = 0;
        return;
    }
    // Q8 per tick = speed * 256 / 60, along (dx, dy) / len. Integer division truncates toward zero.
    const i64 den = i64{kTickHz} * len;
    vx = static_cast<i32>((i64{speedUnitsPerS} << kQ) * dx / den);
    vy = static_cast<i32>((i64{speedUnitsPerS} << kQ) * dy / den);
}

Sim::Sim(u32 collisionPasses) : passes_(collisionPasses == 0 ? 1u : collisionPasses) {}

namespace {

inline u32 cell_of(i32 xq, i32 yq) {
    const i32 cx = clamp_i32(xq >> kCellShift, 0, kGridW - 1);
    const i32 cy = clamp_i32(yq >> kCellShift, 0, kGridH - 1);
    return static_cast<u32>(cy * kGridW + cx);
}

inline void bounce(i32& p, i32& v, i32 lo, i32 hi) {
    if (p < lo) {
        p = 2 * lo - p;
        v = -v;
    } else if (p > hi) {
        p = 2 * hi - p;
        v = -v;
    }
}

} // namespace

u32 Sim::detonationPass(const State& s, u16* orbIdx, i16* parents) {
    constexpr u32 kCells = static_cast<u32>(kGridW * kGridH);
    // Grid of live blasts by counting sort; within a cell, blasts stay in index order.
    std::memset(cellStart_, 0, sizeof(cellStart_));
    for (u32 b = s.firstLive; b < s.blastCount; ++b) ++cellStart_[cell_of(s.blasts[b].x, s.blasts[b].y) + 1];
    for (u32 c = 0; c < kCells; ++c) cellStart_[c + 1] = static_cast<u16>(cellStart_[c + 1] + cellStart_[c]);
    u16 cursor[kCells];
    std::memcpy(cursor, cellStart_, sizeof(cursor));
    for (u32 b = s.firstLive; b < s.blastCount; ++b) {
        cellItems_[cursor[cell_of(s.blasts[b].x, s.blasts[b].y)]++] = static_cast<u16>(b);
    }

    const i64 orbR = i64{kOrbRadius} << kQ;
    u32 n = 0;
    for (u32 i = 0; i < s.orbCount; ++i) {
        const Orb& o = s.orbs[i];
        if (o.alive == 0) continue;
        const i32 cx = o.x >> kCellShift;
        const i32 cy = o.y >> kCellShift;
        i32 best = -1;   // lowest-index touching blast: the parent (docs/02 §2)
        for (i32 gy = max_i32(cy - 1, 0); gy <= min_i32(cy + 1, kGridH - 1); ++gy) {
            for (i32 gx = max_i32(cx - 1, 0); gx <= min_i32(cx + 1, kGridW - 1); ++gx) {
                const u32 c = static_cast<u32>(gy * kGridW + gx);
                for (u32 k = cellStart_[c]; k < cellStart_[c + 1]; ++k) {
                    const u16 bi = cellItems_[k];
                    if (best >= 0 && bi >= best) break;   // items are index-sorted within a cell
                    const Blast& b = s.blasts[bi];
                    const i64 reach = blast_radius_q8(b.rmax, s.tick - b.birth) + orbR;
                    const i64 dx = i64{o.x} - b.x;
                    const i64 dy = i64{o.y} - b.y;
                    if (dx * dx + dy * dy <= reach * reach) best = bi;
                }
            }
        }
        if (best >= 0) {
            orbIdx[n] = static_cast<u16>(i);
            parents[n] = static_cast<i16>(best);
            ++n;
        }
    }
    return n;
}

void Sim::tick(State& s, const TickInput& in, Descriptor& out) {
    const u32 t = s.tick;

    // 1. Taps released on this tick become blasts, tested from this tick on.
    u16 taps = 0;
    for (u32 i = 0; i < in.tapCount && i < kMaxTapsPerTick; ++i) {
        if (s.tapsUsed >= s.tapsAllowed || s.blastCount >= kMaxBlasts) break;
        Blast& b = s.blasts[s.blastCount++];
        b = Blast{};
        b.x = clamp_i32(in.taps[i].x, 0, kArenaW) << kQ;
        b.y = clamp_i32(in.taps[i].y, 0, kArenaH) << kQ;
        b.birth = t;
        b.rmax = kTapRadius;
        b.parent = -1;
        b.gen = 0;
        b.kind = BlastKind::Tap;
        ++s.tapsUsed;
        ++taps;
    }

    // 2. Move the orbs; they bounce off the arena walls.
    constexpr i32 lo = kOrbRadius << kQ;
    constexpr i32 hiX = (kArenaW - kOrbRadius) << kQ;
    constexpr i32 hiY = (kArenaH - kOrbRadius) << kQ;
    u32 alive = 0;
    for (u32 i = 0; i < s.orbCount; ++i) {
        Orb& o = s.orbs[i];
        if (o.alive == 0) continue;
        ++alive;
        o.x += o.vx;
        o.y += o.vy;
        bounce(o.x, o.vx, lo, hiX);
        bounce(o.y, o.vy, lo, hiY);
    }

    // 3. Advance the blast lifecycles: retire blasts that reached the end of their life.
    while (s.firstLive < s.blastCount && t - s.blasts[s.firstLive].birth >= kBlastLife) ++s.firstLive;

    // 4. Detonation test, repeated per the collision-pass dial; every pass must agree.
    const u32 n = detonationPass(s, detOrb_, detParent_);
    for (u32 p = 1; p < passes_; ++p) {
        const u32 m = detonationPass(s, chkOrb_, chkParent_);
        if (m != n || std::memcmp(chkOrb_, detOrb_, n * sizeof(u16)) != 0 ||
            std::memcmp(chkParent_, detParent_, n * sizeof(i16)) != 0) {
            ++mismatches_;
        }
    }

    // Descriptor fields that describe the blasts alive on this tick.
    const u32 liveEnd = s.blastCount;
    u16 arcs = 0;
    u16 depth = 0;
    for (u32 b = s.firstLive; b < liveEnd; ++b) {
        if (s.blasts[b].kind == BlastKind::Chain) ++arcs;
        if (s.blasts[b].gen > depth) depth = s.blasts[b].gen;
    }

    // 5. Detonations, in orb-index order, become chain blasts tested from the next tick.
    for (u32 k = 0; k < n; ++k) {
        Orb& o = s.orbs[detOrb_[k]];
        o.alive = 0;
        const Blast& parent = s.blasts[detParent_[k]];
        Blast& b = s.blasts[s.blastCount++];
        b = Blast{};
        b.x = o.x;
        b.y = o.y;
        b.birth = t + 1;
        b.rmax = kChainRadius;
        b.parent = detParent_[k];
        b.gen = static_cast<u8>(parent.gen < 255 ? parent.gen + 1 : 255);
        b.kind = BlastKind::Chain;
    }
    s.detonationsTotal += n;

    out.tick = t;
    out.liveOrbs = static_cast<u16>(alive - n);
    out.liveBlasts = static_cast<u16>(liveEnd - s.firstLive);
    out.detonations = static_cast<u16>(n);
    out.taps = taps;
    out.liveArcs = arcs;
    out.chainDepth = depth;
    s.tick = t + 1;
}

u64 state_hash(const State& s) {
    Hash64 h;
    const u32 hdr = 0xFFFF0000u;
    h.add(s.tick, hdr, 0);
    h.add(s.orbCount, hdr, 1);
    h.add(s.blastCount, hdr, 2);
    h.add(s.firstLive, hdr, 3);
    h.add(s.tapsAllowed, hdr, 4);
    h.add(s.tapsUsed, hdr, 5);
    h.add(s.detonationsTotal, hdr, 6);
    for (u32 i = 0; i < s.orbCount; ++i) {
        const Orb& o = s.orbs[i];
        h.add(static_cast<u32>(o.x), i, 0);
        h.add(static_cast<u32>(o.y), i, 1);
        h.add(static_cast<u32>(o.vx), i, 2);
        h.add(static_cast<u32>(o.vy), i, 3);
        h.add((u32{o.colour} << 8u) | o.alive, i, 4);
    }
    for (u32 i = 0; i < s.blastCount; ++i) {
        const Blast& b = s.blasts[i];
        const u32 cell = 0x00010000u + i;
        h.add(static_cast<u32>(b.x), cell, 0);
        h.add(static_cast<u32>(b.y), cell, 1);
        h.add(b.birth, cell, 2);
        h.add(static_cast<u32>(b.rmax), cell, 3);
        h.add((static_cast<u32>(static_cast<u16>(b.parent)) << 16u) | (u32{b.gen} << 8u) | static_cast<u32>(b.kind),
              cell, 4);
    }
    return h.value();
}

} // namespace ag
