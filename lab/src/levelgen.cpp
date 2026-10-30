#include "levelgen.h"

#include "ag/pcg32.h"

#include <algorithm>
#include <cstdio>
#include <memory>

namespace ag::lab {
namespace {

u64 mix64(u64 x) {   // splitmix64 finaliser
    x ^= x >> 30u;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27u;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31u;
    return x;
}

bool u32_at(const Json& arr, usize i, u32& out) {
    if (!arr.isArray() || arr.items().size() <= i || !arr.items()[i].isInt() || arr.items()[i].asInt() < 0) return false;
    out = static_cast<u32>(arr.items()[i].asInt());
    return true;
}

bool pair_of(const Json* j, u32& lo, u32& hi) {
    return j != nullptr && u32_at(*j, 0, lo) && u32_at(*j, 1, hi) && lo <= hi;
}

i64 reach_q8(u32 age) { return blast_radius_q8(kTapRadius, age) + (i64{kOrbRadius} << kQ); }

/// Runs one burst from 'c' (state at the start of the tap tick) until it closes.
BurstStats run_burst(Sim& sim, State& c, TapEvent tap, u32 guardTicks) {
    BurstTracker bt;
    Descriptor d;
    TickInput in{};
    in.tapCount = 1;
    in.taps[0] = tap;
    sim.tick(c, in, d);
    bt.feed(d);
    const TickInput none{};
    for (u32 i = 0; i < guardTicks && (bt.bursts().empty() || bt.bursts().back().end == 0); ++i) {
        sim.tick(c, none, d);
        bt.feed(d);
    }
    return bt.bursts().empty() ? BurstStats{} : bt.bursts().front();
}

/// Places one burst: from calm state 's', finds a tap whose first detonation is exactly at tick T
/// and whose peak live-blast count is in the band. On success 's' becomes the state after the
/// burst's end tick.
bool place_burst(const GenConfig& cfg, State& s, u32 T, const BurstSpec& b, Pcg32& rng, TapSpec& tap,
                 BurstStats& stats) {
    if (s.firstLive != s.blastCount) return false;   // the previous burst must be over
    const u32 uLo = std::max({s.tick, T >= kGrowTicks - 1 ? T - (kGrowTicks - 1) : 0u, cfg.holdTicks});
    const u32 uHi = T;
    if (uLo > uHi) return false;
    Sim sim(1);
    const TickInput none{};
    Descriptor d;

    // States at the start of ticks uLo .. T+1. With no live blast nothing detonates, so orb
    // positions after the move step of tick t are those at the start of tick t+1.
    std::vector<State> snaps(uHi - uLo + 2);
    State cur = s;
    while (cur.tick < uLo) sim.tick(cur, none, d);
    for (usize k = 0; k < snaps.size(); ++k) {
        snaps[k] = cur;
        if (k + 1 < snaps.size()) sim.tick(cur, none, d);
    }

    const i32 step = static_cast<i32>(cfg.positionStep);
    std::vector<std::pair<i32, i32>> pos;
    for (i32 y = step / 2; y <= kArenaH - step / 2; y += step) {
        for (i32 x = step / 2; x <= kArenaW - step / 2; x += step) pos.emplace_back(x, y);
    }
    const usize nt = uHi - uLo + 1;
    // d2[p * nt + k]: squared distance (Q8) from position p to the nearest live orb at tick uLo + k.
    std::vector<i64> d2(pos.size() * nt, INT64_MAX);
    for (usize k = 0; k < nt; ++k) {
        const State& at = snaps[k + 1];
        for (u32 i = 0; i < at.orbCount; ++i) {
            const Orb& o = at.orbs[i];
            if (o.alive == 0) continue;
            for (usize p = 0; p < pos.size(); ++p) {
                const i64 dx = i64{o.x} - (i64{pos[p].first} << kQ);
                const i64 dy = i64{o.y} - (i64{pos[p].second} << kQ);
                const i64 v = dx * dx + dy * dy;
                if (v < d2[p * nt + k]) d2[p * nt + k] = v;
            }
        }
    }

    // Candidates whose first detonation is exactly at T (released on tick u, blast age t - u).
    struct Cand {
        u32 u;
        usize p;
    };
    std::vector<Cand> cands;
    for (u32 u = uLo; u <= uHi; ++u) {
        for (usize p = 0; p < pos.size(); ++p) {
            bool early = false;
            for (u32 t = u; t < T && !early; ++t) {
                const i64 r = reach_q8(t - u);
                early = d2[p * nt + (t - uLo)] <= r * r;
            }
            if (early) continue;
            const i64 r = reach_q8(T - u);
            if (d2[p * nt + (T - uLo)] <= r * r) cands.push_back({u, p});
        }
    }
    // Seeded shuffle, then try cascades until one lands in the band.
    for (usize i = cands.size(); i > 1; --i) {
        const usize j = rng.below(static_cast<u32>(i));
        std::swap(cands[i - 1], cands[j]);
    }
    const usize tries = std::min<usize>(cands.size(), cfg.maxCascadesPerBurst);
    for (usize i = 0; i < tries; ++i) {
        State c = snaps[cands[i].u - uLo];
        const TapEvent ev{pos[cands[i].p].first, pos[cands[i].p].second};
        const BurstStats r = run_burst(sim, c, ev, 20000);
        if (r.onset != T || r.end == 0 || r.peakLiveBlasts < b.bandLo || r.peakLiveBlasts > b.bandHi) continue;
        tap.up = cands[i].u;
        tap.down = cands[i].u - cfg.holdTicks;
        tap.x = ev.x;
        tap.y = ev.y;
        stats = r;
        s = c;
        return true;
    }
    return false;
}

} // namespace

std::vector<Orb> make_orbs(u64 seed, u64 stream, u32 n) {
    Pcg32 rng(seed, stream);
    std::vector<Orb> v(n);
    const u32 spanX = static_cast<u32>((kArenaW - 2 * kOrbRadius) << kQ);
    const u32 spanY = static_cast<u32>((kArenaH - 2 * kOrbRadius) << kQ);
    for (Orb& o : v) {
        o.x = (kOrbRadius << kQ) + static_cast<i32>(rng.below(spanX + 1));
        o.y = (kOrbRadius << kQ) + static_cast<i32>(rng.below(spanY + 1));
        const i32 speed = kMinSpeed + static_cast<i32>(rng.below(static_cast<u32>(kMaxSpeed - kMinSpeed + 1)));
        i32 dx = 0, dy = 0;
        i64 m = 0;
        do {   // uniform direction by rejection in an annulus (no trigonometry, R1)
            dx = static_cast<i32>(rng.below(2049)) - 1024;
            dy = static_cast<i32>(rng.below(2049)) - 1024;
            m = i64{dx} * dx + i64{dy} * dy;
        } while (m < 256 * 256 || m > 1024 * 1024);
        orb_velocity(speed, dx, dy, o.vx, o.vy);
        o.colour = static_cast<u8>(rng.below(4));
        o.alive = 1;
    }
    return v;
}

bool parse_levelgen_spec(const Json& j, GenConfig& cfg, std::vector<LevelSpec>& levels, std::string& err) {
    const Json* schema = j.get("schema");
    if (schema == nullptr || schema->asString() != "afterglow.levelgen/1") {
        err = "spec schema is not afterglow.levelgen/1";
        return false;
    }
    auto opt = [&](const char* k, u32& out) {
        const Json* v = j.get(k);
        if (v != nullptr && v->isInt() && v->asInt() >= 0) out = static_cast<u32>(v->asInt());
    };
    if (const Json* v = j.get("seed_base"); v != nullptr && v->isInt()) cfg.seedBase = static_cast<u64>(v->asInt());
    opt("hold_ticks", cfg.holdTicks);
    opt("max_attempts", cfg.maxAttempts);
    opt("position_step", cfg.positionStep);
    opt("max_cascades_per_burst", cfg.maxCascadesPerBurst);
    opt("margin_ticks", cfg.marginTicks);

    const Json* classes = j.get("classes");
    if (classes == nullptr || !classes->isObject()) {
        err = "spec needs 'classes'";
        return false;
    }
    auto cls = [&](const std::string& name, BurstSpec& b, u32& orbsLo, u32& orbsHi) {
        const Json* c = classes->get(name);
        if (c == nullptr || !pair_of(c->get("band"), b.bandLo, b.bandHi) || !pair_of(c->get("orbs"), orbsLo, orbsHi)) {
            err = "class '" + name + "' needs band [lo, hi] and orbs [lo, hi]";
            return false;
        }
        b.cls = name;
        return true;
    };

    if (const Json* single = j.get("single"); single != nullptr) {
        const Json* calms = single->get("calm_ms");
        const Json* names = single->get("classes");
        if (calms == nullptr || !calms->isArray() || names == nullptr || !names->isArray()) {
            err = "'single' needs calm_ms and classes arrays";
            return false;
        }
        for (const Json& cm : calms->items()) {
            for (const Json& nm : names->items()) {
                LevelSpec ls;
                BurstSpec b;
                if (!cm.isInt() || cm.asInt() <= 0 || !cls(nm.asString(), b, ls.orbsLo, ls.orbsHi)) {
                    if (err.empty()) err = "bad single entry";
                    return false;
                }
                b.calmTicks = static_cast<u32>(cm.asInt()) * kTickHz / 1000u;
                char id[64];
                std::snprintf(id, sizeof(id), "single_%05lldms_%s", static_cast<long long>(cm.asInt()), b.cls.c_str());
                ls.id = id;
                ls.kind = "research_single";
                ls.bursts.push_back(b);
                levels.push_back(ls);
            }
        }
    }
    if (const Json* rep = j.get("repeated"); rep != nullptr && rep->isArray()) {
        for (const Json& r : rep->items()) {
            LevelSpec ls;
            const Json* id = r.get("id");
            const Json* calms = r.get("calm_ms");
            const Json* cname = r.get("class");
            if (id == nullptr || calms == nullptr || !calms->isArray() || calms->items().size() < 2 || cname == nullptr) {
                err = "repeated entry needs id, class and calm_ms (one per burst, >= 2)";
                return false;
            }
            ls.id = id->asString();
            ls.kind = "research_repeat";
            for (const Json& cm : calms->items()) {
                BurstSpec b;
                if (!cm.isInt() || cm.asInt() <= 0 || !cls(cname->asString(), b, ls.orbsLo, ls.orbsHi)) {
                    if (err.empty()) err = "bad repeated entry " + ls.id;
                    return false;
                }
                b.calmTicks = static_cast<u32>(cm.asInt()) * kTickHz / 1000u;
                ls.bursts.push_back(b);
            }
            pair_of(r.get("orbs"), ls.orbsLo, ls.orbsHi);   // optional override
            levels.push_back(ls);
        }
    }
    if (levels.empty()) {
        err = "spec defines no levels";
        return false;
    }
    return true;
}

bool generate_level(const GenConfig& cfg, const LevelSpec& spec, u32 levelIndex, Level& out, std::string& err) {
    if (spec.orbsHi > kMaxOrbs || spec.orbsLo == 0 || spec.bursts.size() > kMaxTaps) {
        err = spec.id + ": orb range or burst count out of bounds";
        return false;
    }
    for (u32 attempt = 0; attempt < cfg.maxAttempts; ++attempt) {
        const u64 seed = mix64(cfg.seedBase * 1000003ull + u64{levelIndex} * 7919ull + attempt) & 0x7FFFFFFFFFFFull;
        Pcg32 rng(seed, 0x5EEDull + levelIndex);
        const u32 n = spec.orbsLo + rng.below(spec.orbsHi - spec.orbsLo + 1);
        Level lv;
        lv.id = spec.id;
        lv.kind = spec.kind;
        lv.seed = seed;
        lv.orbs = make_orbs(seed, 1, n);
        lv.tapsAllowed = static_cast<u32>(spec.bursts.size());
        auto s = std::make_unique<State>();
        init_state(lv, *s);
        u32 prevEnd = 0;
        bool ok = true;
        for (const BurstSpec& b : spec.bursts) {
            TapSpec tap;
            BurstStats st;
            if (!place_burst(cfg, *s, prevEnd + b.calmTicks, b, rng, tap, st)) {
                ok = false;
                break;
            }
            lv.taps.push_back(tap);
            prevEnd = st.end;
        }
        if (!ok) continue;

        // Replay the whole level the way every device will, and record what it must produce.
        lv.maxTicks = prevEnd + cfg.marginTicks + 1;
        const TraceResult r = run_level(lv, 1);
        bool good = r.finished && r.passMismatches == 0 && r.bursts.size() == spec.bursts.size();
        u32 end = 0;
        for (usize k = 0; good && k < r.bursts.size(); ++k) {
            const BurstStats& b = r.bursts[k];
            good = b.onset == end + spec.bursts[k].calmTicks && b.peakLiveBlasts >= spec.bursts[k].bandLo &&
                   b.peakLiveBlasts <= spec.bursts[k].bandHi;
            end = b.end;
        }
        if (!good) continue;   // cannot happen if place_burst is right; never write a bad level

        lv.target = r.detonations;
        lv.expectedTraceSha256 = r.sha256;
        lv.expectedTicks = r.ticks;
        lv.expectedDetonations = r.detonations;
        lv.expectedBursts = r.bursts;
        Json design = Json::object();
        design.set("generator", Json::str("aglab --mode=levelgen (afterglow.levelgen/1)"));
        design.set("attempt", Json::num(attempt));
        design.set("orbs", Json::num(n));
        design.set("hold_ticks", Json::num(cfg.holdTicks));
        design.set("t_calm_definition",
                   Json::str("ticks from the end of the previous burst (level start for the first) to the first "
                             "detonation (D22)"));
        Json bursts = Json::array();
        for (const BurstSpec& b : spec.bursts) {
            Json bj = Json::object();
            bj.set("calm_ms", Json::num(b.calmTicks * 1000u / kTickHz));
            bj.set("calm_ticks", Json::num(b.calmTicks));
            bj.set("class", Json::str(b.cls));
            Json band = Json::array();
            band.push(Json::num(b.bandLo)).push(Json::num(b.bandHi));
            bj.set("band_peak_live_blasts", std::move(band));
            bursts.push(std::move(bj));
        }
        design.set("bursts", std::move(bursts));
        lv.design = std::move(design);
        out = std::move(lv);
        return true;
    }
    err = spec.id + ": no seed fits within " + std::to_string(cfg.maxAttempts) + " attempts";
    return false;
}

void explore_bursts(u32 nLo, u32 nHi, u32 nStep, u32 samples, u64 seedBase) {
    std::printf("orbs  no_onset  peak_live_blasts p10/p25/p50/p75/p90   detonations p10/p50/p90   duration_ticks p50\n");
    for (u32 n = nLo; n <= nHi; n += nStep) {
        std::vector<u32> peaks, dets, durs;
        u32 noOnset = 0;
        for (u32 k = 0; k < samples; ++k) {
            const u64 seed = mix64(seedBase + u64{n} * 100003ull + k);
            Pcg32 rng(seed, 7);
            Level lv;
            lv.orbs = make_orbs(seed, 1, n);
            lv.tapsAllowed = 1;
            auto s = std::make_unique<State>();
            init_state(lv, *s);
            Sim sim(1);
            Descriptor d;
            const TickInput none{};
            for (int i = 0; i < 60; ++i) sim.tick(*s, none, d);
            const TapEvent tap{static_cast<i32>(rng.below(static_cast<u32>(kArenaW))),
                               static_cast<i32>(rng.below(static_cast<u32>(kArenaH)))};
            const BurstStats b = run_burst(sim, *s, tap, 20000);
            if (b.onset == kNoOnset) {
                ++noOnset;
                continue;
            }
            peaks.push_back(b.peakLiveBlasts);
            dets.push_back(b.detonations);
            durs.push_back(b.end - b.onset);
        }
        auto q = [](std::vector<u32> v, u32 pct) -> u32 {
            if (v.empty()) return 0;
            std::sort(v.begin(), v.end());
            return v[(v.size() - 1) * pct / 100u];
        };
        std::printf("%4u  %8u  %5u %5u %5u %5u %5u   %5u %5u %5u   %5u\n", n, noOnset, q(peaks, 10), q(peaks, 25),
                    q(peaks, 50), q(peaks, 75), q(peaks, 90), q(dets, 10), q(dets, 50), q(dets, 90), q(durs, 50));
    }
}

} // namespace ag::lab
