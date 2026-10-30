// Gameplay rules (docs/02 §2) on hand-built states.
#include "ag/level.h"
#include "ag/sim.h"

#include "doctest/doctest.h"

#include <memory>

using namespace ag;

namespace {

/// A state with still orbs at the given unit positions and 'taps' taps available.
std::unique_ptr<State> still_orbs(std::initializer_list<std::pair<i32, i32>> xy, u32 taps = 1) {
    auto s = std::make_unique<State>();
    for (auto [x, y] : xy) {
        Orb& o = s->orbs[s->orbCount++];
        o.x = x << kQ;
        o.y = y << kQ;
        o.alive = 1;
    }
    s->tapsAllowed = taps;
    return s;
}

TickInput tap_at(i32 x, i32 y) {
    TickInput in;
    in.tapCount = 1;
    in.taps[0] = {x, y};
    return in;
}

/// Runs ticks until the first detonation; returns the tick, or -1.
int first_detonation(State& s, Sim& sim, const TickInput& firstInput, int maxTicks = 100) {
    Descriptor d;
    sim.tick(s, firstInput, d);
    if (d.detonations > 0) return static_cast<int>(d.tick);
    for (int i = 1; i < maxTicks; ++i) {
        sim.tick(s, TickInput{}, d);
        if (d.detonations > 0) return static_cast<int>(d.tick);
    }
    return -1;
}

} // namespace

TEST_CASE("blast lifecycle: grow 18 ticks, hold 36, shrink 18 (docs/02 §2)") {
    const i64 R = i64{kTapRadius} << kQ;
    CHECK(blast_radius_q8(kTapRadius, 0) == 0);
    CHECK(blast_radius_q8(kTapRadius, 9) == R / 2);
    CHECK(blast_radius_q8(kTapRadius, 17) == R * 17 / 18);
    CHECK(blast_radius_q8(kTapRadius, 18) == R);
    CHECK(blast_radius_q8(kTapRadius, 53) == R);
    CHECK(blast_radius_q8(kTapRadius, 54) == R);
    CHECK(blast_radius_q8(kTapRadius, 63) == R / 2);
    CHECK(blast_radius_q8(kTapRadius, 71) == R / 18);
    CHECK(blast_radius_q8(kTapRadius, 72) == -1);
    CHECK(kBlastLife == 72u);
}

TEST_CASE("detonation boundary is exact: within (radius + orb radius) detonates, one Q8 unit more does not") {
    for (u32 age = 0; age < 18; ++age) {
        const i64 reach = blast_radius_q8(kTapRadius, age) + (i64{kOrbRadius} << kQ);
        for (i64 extra : {i64{0}, i64{1}}) {
            auto s = still_orbs({});
            Orb& o = s->orbs[s->orbCount++];
            o.x = (500 << kQ) + static_cast<i32>(reach + extra);
            o.y = 1000 << kQ;
            o.alive = 1;
            Sim sim;
            // The tap is released at tick 0, so on tick 'age' the blast is that old.
            const int t = first_detonation(*s, sim, tap_at(500, 1000));
            if (extra == 0) {
                CHECK(t <= static_cast<int>(age));
            } else {
                CHECK(t > static_cast<int>(age));
            }
        }
    }
}

TEST_CASE("chain: the parent is the lowest-index touching blast; generation counts from the tap") {
    auto s = still_orbs({{500, 1000}}, 2);
    Sim sim;
    TickInput in;
    in.tapCount = 2;
    in.taps[0] = {490, 1000};   // blast 0
    in.taps[1] = {510, 1000};   // blast 1, also touching
    Descriptor d;
    sim.tick(*s, in, d);
    CHECK(d.taps == 2);
    CHECK(d.detonations == 1);
    REQUIRE(s->blastCount == 3);
    CHECK(s->blasts[2].parent == 0);
    CHECK(s->blasts[2].gen == 1);
    CHECK(s->blasts[2].kind == BlastKind::Chain);
    CHECK(s->blasts[2].birth == 1u);   // a chain blast is tested from the next tick
    CHECK(s->orbs[0].alive == 0);
}

TEST_CASE("chain propagates orb to orb and depth grows") {
    // Orbs 130 units apart in a line: each chain blast (120 + 18 reach) catches the next. The tap
    // sits 60 units left of the first orb, so its own reach (160 + 18) catches only that one.
    auto s = still_orbs({{100, 1000}, {230, 1000}, {360, 1000}, {490, 1000}});
    Sim sim;
    Descriptor d;
    sim.tick(*s, tap_at(40, 1000), d);
    u16 maxDepth = 0;
    while (!finished(*s)) {
        sim.tick(*s, TickInput{}, d);
        if (d.chainDepth > maxDepth) maxDepth = d.chainDepth;
    }
    CHECK(s->detonationsTotal == 4);
    CHECK(maxDepth == 4);
    CHECK(d.liveBlasts == 0);
}

TEST_CASE("orbs bounce off the walls exactly; there are no orb-orb collisions") {
    auto s = still_orbs({{19, 1000}, {600, 1000}, {600, 1000}}, 0);
    s->orbs[0].vx = -2 << kQ;   // 2 units per tick toward the left wall at x = 18
    s->orbs[1].vx = 512;
    s->orbs[2].vx = 512;        // overlapping twin: passes through, no interaction
    Sim sim;
    Descriptor d;
    sim.tick(*s, TickInput{}, d);
    CHECK(s->orbs[0].x == (18 << kQ) + (1 << kQ));   // 19 - 2 = 17, reflected about 18 -> 19
    CHECK(s->orbs[0].vx == 2 << kQ);
    CHECK(s->orbs[1].x == s->orbs[2].x);
    CHECK(d.detonations == 0);
}

TEST_CASE("a tap with no taps left is ignored; the level ends when no blast is alive") {
    auto s = still_orbs({{100, 100}}, 1);
    Sim sim;
    Descriptor d;
    sim.tick(*s, tap_at(900, 2000), d);   // misses the orb
    CHECK(d.taps == 1);
    sim.tick(*s, tap_at(100, 100), d);    // no taps left
    CHECK(d.taps == 0);
    int guard = 0;
    while (!finished(*s) && guard++ < 200) sim.tick(*s, TickInput{}, d);
    CHECK(finished(*s));
    CHECK(s->detonationsTotal == 0);
    CHECK(s->orbs[0].alive == 1);
}

TEST_CASE("collision passes do not change the cascade (D20)") {
    auto a = still_orbs({{100, 1000}, {200, 1000}, {300, 1060}, {420, 1000}, {300, 940}});
    auto b = std::make_unique<State>(*a);
    Sim s1(1), s4(4);
    Descriptor d1, d4;
    s1.tick(*a, tap_at(100, 1000), d1);
    s4.tick(*b, tap_at(100, 1000), d4);
    while (!finished(*a)) {
        s1.tick(*a, TickInput{}, d1);
        s4.tick(*b, TickInput{}, d4);
        CHECK(d1 == d4);
    }
    CHECK(state_hash(*a) == state_hash(*b));
    CHECK(s4.passMismatches() == 0);
}

TEST_CASE("state hash follows the state; snapshot is a copy") {
    auto a = still_orbs({{100, 1000}, {200, 1000}});
    a->orbs[0].vx = 100;
    const u64 h0 = state_hash(*a);
    auto snap = std::make_unique<State>(*a);
    CHECK(state_hash(*snap) == h0);
    Sim sim;
    Descriptor d;
    sim.tick(*a, TickInput{}, d);
    CHECK(state_hash(*a) != h0);
    Sim sim2;
    sim2.tick(*snap, TickInput{}, d);
    CHECK(state_hash(*snap) == state_hash(*a));
}

TEST_CASE("descriptor serialisation is little-endian and 16 bytes") {
    Descriptor d;
    d.tick = 0x01020304u;
    d.liveOrbs = 0x0506;
    d.chainDepth = 0x0708;
    u8 b[kDescriptorBytes];
    descriptor_bytes(d, b);
    CHECK(b[0] == 0x04);
    CHECK(b[3] == 0x01);
    CHECK(b[4] == 0x06);
    CHECK(b[5] == 0x05);
    CHECK(b[14] == 0x08);
    CHECK(b[15] == 0x07);
}

TEST_CASE("BurstTracker: onset is the first detonation, end the first tick with no live blast") {
    BurstTracker bt;
    auto feed = [&](u32 t, u16 live, u16 det) {
        Descriptor d;
        d.tick = t;
        d.liveBlasts = live;
        d.detonations = det;
        bt.feed(d);
    };
    feed(0, 0, 0);
    feed(1, 1, 0);
    feed(2, 1, 0);
    feed(3, 2, 1);
    feed(4, 3, 2);
    feed(5, 0, 0);
    REQUIRE(bt.bursts().size() == 1);
    CHECK(bt.bursts()[0].start == 1);
    CHECK(bt.bursts()[0].onset == 3);
    CHECK(bt.bursts()[0].end == 5);
    CHECK(bt.bursts()[0].peakLiveBlasts == 3);
    CHECK(bt.bursts()[0].detonations == 3);
}
