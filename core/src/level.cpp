#include "ag/level.h"

#include "ag/sha256.h"

#include <cstdio>
#include <memory>

namespace ag {

void BurstTracker::feed(const Descriptor& d) {
    if (d.liveBlasts > 0) {
        if (!open_) {
            BurstStats b;
            b.start = d.tick;
            b.onset = kNoOnset;
            bursts_.push_back(b);
            open_ = true;
        }
        BurstStats& b = bursts_.back();
        if (d.detonations > 0 && b.onset == kNoOnset) b.onset = d.tick;
        if (d.liveBlasts > b.peakLiveBlasts) b.peakLiveBlasts = d.liveBlasts;
        if (d.detonations > b.peakDetonations) b.peakDetonations = d.detonations;
        b.detonations += d.detonations;
    } else if (open_) {
        bursts_.back().end = d.tick;
        open_ = false;
    }
}

namespace {

bool get_u32(const Json& j, const char* key, u32& out, std::string& err) {
    const Json* v = j.get(key);
    if (v == nullptr || !v->isInt() || v->asInt() < 0 || v->asInt() > 0xFFFFFFFFll) {
        err = std::string("missing or bad integer '") + key + "'";
        return false;
    }
    out = static_cast<u32>(v->asInt());
    return true;
}

bool get_i32(const Json& j, i32& out) {
    if (!j.isInt() || j.asInt() < -0x7FFFFFFFll || j.asInt() > 0x7FFFFFFFll) return false;
    out = static_cast<i32>(j.asInt());
    return true;
}

Json burst_to_json(const BurstStats& b) {
    Json j = Json::object();
    j.set("start", Json::num(b.start));
    j.set("onset", b.onset == kNoOnset ? Json::null() : Json::num(b.onset));
    j.set("end", Json::num(b.end));
    j.set("peak_live_blasts", Json::num(b.peakLiveBlasts));
    j.set("peak_detonations", Json::num(b.peakDetonations));
    j.set("detonations", Json::num(b.detonations));
    return j;
}

bool burst_from_json(const Json& j, BurstStats& b, std::string& err) {
    if (!get_u32(j, "start", b.start, err) || !get_u32(j, "end", b.end, err) ||
        !get_u32(j, "peak_live_blasts", b.peakLiveBlasts, err) ||
        !get_u32(j, "peak_detonations", b.peakDetonations, err) || !get_u32(j, "detonations", b.detonations, err)) {
        return false;
    }
    const Json* on = j.get("onset");
    if (on == nullptr || on->isNull()) {
        b.onset = kNoOnset;
        return true;
    }
    return get_u32(j, "onset", b.onset, err);
}

} // namespace

bool level_from_json(const Json& j, Level& lv, std::string& err) {
    lv = Level{};
    const Json* schema = j.get("schema");
    if (schema == nullptr || schema->asString() != "afterglow.level/1") {
        err = "schema is not afterglow.level/1";
        return false;
    }
    const Json* id = j.get("id");
    const Json* kind = j.get("kind");
    const Json* seed = j.get("seed");
    if (id == nullptr || !id->isString() || kind == nullptr || !kind->isString() || seed == nullptr ||
        !seed->isInt() || seed->asInt() < 0) {
        err = "missing id, kind or seed";
        return false;
    }
    lv.id = id->asString();
    lv.kind = kind->asString();
    lv.seed = static_cast<u64>(seed->asInt());
    if (!get_u32(j, "taps_allowed", lv.tapsAllowed, err) || !get_u32(j, "target", lv.target, err) ||
        !get_u32(j, "max_ticks", lv.maxTicks, err)) {
        return false;
    }
    if (lv.tapsAllowed > kMaxTaps) {
        err = "taps_allowed exceeds kMaxTaps";
        return false;
    }
    const Json* orbs = j.get("orbs");
    if (orbs == nullptr || !orbs->isArray() || orbs->items().size() > kMaxOrbs) {
        err = "missing orbs, or more than kMaxOrbs";
        return false;
    }
    for (const Json& o : orbs->items()) {
        if (!o.isArray() || o.items().size() != 5) {
            err = "orb entry must be [x_q8, y_q8, vx_q8, vy_q8, colour]";
            return false;
        }
        Orb orb;
        i32 colour = 0;
        if (!get_i32(o.items()[0], orb.x) || !get_i32(o.items()[1], orb.y) || !get_i32(o.items()[2], orb.vx) ||
            !get_i32(o.items()[3], orb.vy) || !get_i32(o.items()[4], colour) || colour < 0 || colour > 255) {
            err = "bad orb entry";
            return false;
        }
        orb.colour = static_cast<u8>(colour);
        orb.alive = 1;
        lv.orbs.push_back(orb);
    }
    const Json* taps = j.get("taps");
    if (taps == nullptr || !taps->isArray()) {
        err = "missing taps";
        return false;
    }
    for (const Json& t : taps->items()) {
        TapSpec ts;
        const Json* x = t.get("x");
        const Json* y = t.get("y");
        if (!get_u32(t, "down", ts.down, err) || !get_u32(t, "up", ts.up, err) || x == nullptr || y == nullptr ||
            !get_i32(*x, ts.x) || !get_i32(*y, ts.y) || ts.down > ts.up) {
            if (err.empty()) err = "bad tap entry";
            return false;
        }
        lv.taps.push_back(ts);
    }
    if (const Json* d = j.get("design"); d != nullptr) lv.design = *d;
    const Json* exp = j.get("expected");
    if (exp != nullptr && exp->isObject()) {
        const Json* sha = exp->get("trace_sha256");
        if (sha != nullptr) lv.expectedTraceSha256 = sha->asString();
        if (!get_u32(*exp, "ticks", lv.expectedTicks, err) || !get_u32(*exp, "detonations", lv.expectedDetonations, err)) {
            return false;
        }
        const Json* bursts = exp->get("bursts");
        if (bursts != nullptr && bursts->isArray()) {
            for (const Json& b : bursts->items()) {
                BurstStats bs;
                if (!burst_from_json(b, bs, err)) return false;
                lv.expectedBursts.push_back(bs);
            }
        }
    }
    return true;
}

Json level_to_json(const Level& lv) {
    Json j = Json::object();
    j.set("schema", Json::str("afterglow.level/1"));
    j.set("id", Json::str(lv.id));
    j.set("kind", Json::str(lv.kind));
    j.set("seed", Json::num(static_cast<i64>(lv.seed)));
    j.set("taps_allowed", Json::num(lv.tapsAllowed));
    j.set("target", Json::num(lv.target));
    j.set("max_ticks", Json::num(lv.maxTicks));
    j.set("design", lv.design);
    Json taps = Json::array();
    for (const TapSpec& t : lv.taps) {
        Json tj = Json::object();
        tj.set("down", Json::num(t.down));
        tj.set("up", Json::num(t.up));
        tj.set("x", Json::num(t.x));
        tj.set("y", Json::num(t.y));
        taps.push(std::move(tj));
    }
    j.set("taps", std::move(taps));
    Json exp = Json::object();
    exp.set("trace_sha256", Json::str(lv.expectedTraceSha256));
    exp.set("ticks", Json::num(lv.expectedTicks));
    exp.set("detonations", Json::num(lv.expectedDetonations));
    Json bursts = Json::array();
    for (const BurstStats& b : lv.expectedBursts) bursts.push(burst_to_json(b));
    exp.set("bursts", std::move(bursts));
    j.set("expected", std::move(exp));
    Json orbs = Json::array();
    for (const Orb& o : lv.orbs) {
        Json oj = Json::array();
        oj.push(Json::num(o.x)).push(Json::num(o.y)).push(Json::num(o.vx)).push(Json::num(o.vy)).push(Json::num(o.colour));
        orbs.push(std::move(oj));
    }
    j.set("orbs", std::move(orbs));
    return j;
}

bool load_level(const std::string& path, Level& out, std::string& err) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        err = "cannot open " + path;
        return false;
    }
    std::string text;
    char buf[1 << 14];
    for (;;) {
        const usize n = std::fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;
        text.append(buf, n);
    }
    std::fclose(f);
    Json j;
    if (!Json::parse(text, j, err)) {
        err = path + ": " + err;
        return false;
    }
    if (!level_from_json(j, out, err)) {
        err = path + ": " + err;
        return false;
    }
    return true;
}

void init_state(const Level& lv, State& s) {
    s = State{};
    s.orbCount = static_cast<u32>(lv.orbs.size());
    for (u32 i = 0; i < s.orbCount; ++i) {
        s.orbs[i] = lv.orbs[i];
        s.orbs[i].alive = 1;
    }
    s.tapsAllowed = lv.tapsAllowed;
}

TickInput bot_input(const Level& lv, u32 tick) {
    TickInput in;
    for (const TapSpec& t : lv.taps) {
        if (t.up == tick && in.tapCount < kMaxTapsPerTick) in.taps[in.tapCount++] = TapEvent{t.x, t.y};
    }
    return in;
}

TraceResult run_level(const Level& lv, u32 collisionPasses, std::vector<Descriptor>* trace, u32 stateHashEvery,
                      std::vector<u64>* stateHashes) {
    auto s = std::make_unique<State>();
    auto sim = std::make_unique<Sim>(collisionPasses);
    init_state(lv, *s);
    if (trace != nullptr) {
        trace->clear();
        trace->reserve(lv.maxTicks);
    }
    Sha256 sha;
    BurstTracker bursts;
    TraceResult r;
    Descriptor d;
    u8 bytes[kDescriptorBytes];
    while (!finished(*s) && s->tick < lv.maxTicks) {
        sim->tick(*s, bot_input(lv, s->tick), d);
        descriptor_bytes(d, bytes);
        sha.update(bytes, sizeof(bytes));
        bursts.feed(d);
        if (trace != nullptr) trace->push_back(d);
        if (stateHashEvery > 0 && stateHashes != nullptr && s->tick % stateHashEvery == 0) {
            stateHashes->push_back(state_hash(*s));
        }
    }
    r.sha256 = Sha256::hex(sha.finish());
    r.ticks = s->tick;
    r.detonations = s->detonationsTotal;
    r.bursts = bursts.bursts();
    r.passMismatches = sim->passMismatches();
    r.finished = finished(*s);
    return r;
}

} // namespace ag
