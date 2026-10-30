#include "stepbench.h"

#include "ag/pcg32.h"

#include <sstream>

namespace ag::lab {
namespace {

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    for (std::string item; std::getline(ss, item, ',');) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

const char* target_name(SbTarget t) {
    switch (t) {
        case SbTarget::Gpu: return "gpu";
        case SbTarget::Cpu: return "cpu";
        case SbTarget::Both: return "both";
    }
    return "?";
}

} // namespace

bool StepbenchConfig::parse(const std::string& calms, const std::string& loads, const std::string& targetsArg,
                            const std::string& l0, std::string& err) {
    try {
        if (!calms.empty()) {
            calmsMs.clear();
            for (const auto& v : split(calms)) calmsMs.push_back(static_cast<u32>(std::stoul(v)));
        }
        if (!loads.empty()) {
            loadsPct.clear();
            for (const auto& v : split(loads)) loadsPct.push_back(static_cast<u32>(std::stoul(v)));
        }
    } catch (...) {
        err = "bad number in --calms or --loads";
        return false;
    }
    if (!targetsArg.empty()) {
        targets.clear();
        for (const auto& v : split(targetsArg)) {
            if (v == "gpu") targets.push_back(SbTarget::Gpu);
            else if (v == "cpu") targets.push_back(SbTarget::Cpu);
            else if (v == "both") targets.push_back(SbTarget::Both);
            else {
                err = "unknown target '" + v + "' (gpu, cpu, both)";
                return false;
            }
        }
    }
    if (!l0.empty()) {
        l0Ambient.clear();
        for (const auto& v : split(l0)) {
            if (v == "probes") l0Ambient.push_back(false);
            else if (v == "ambient") l0Ambient.push_back(true);
            else {
                err = "unknown --l0 '" + v + "' (probes, ambient)";
                return false;
            }
        }
    }
    for (u32 p : loadsPct) {
        if (p == 0 || p > 100) {
            err = "loads must be 1..100 % of the budget";
            return false;
        }
    }
    if (calmsMs.empty() || loadsPct.empty() || targets.empty() || l0Ambient.empty() || repeats == 0) {
        err = "empty stepbench design";
        return false;
    }
    return true;
}

Json StepbenchConfig::toJson() const {
    Json j = Json::object();
    Json c = Json::array(), l = Json::array(), t = Json::array(), a = Json::array();
    for (u32 v : calmsMs) c.push(Json::num(v));
    for (u32 v : loadsPct) l.push(Json::num(v));
    for (SbTarget v : targets) t.push(Json::str(target_name(v)));
    for (bool v : l0Ambient) a.push(Json::str(v ? "ambient" : "probes"));
    j.set("calms_ms", std::move(c)).set("loads_pct", std::move(l)).set("targets", std::move(t)).set("l0", std::move(a));
    j.set("repeats", Json::num(repeats)).set("seed", Json::num(static_cast<i64>(seed)));
    j.set("trial_from", Json::num(trialFrom)).set("trial_to", Json::num(trialTo == 0xFFFFFFFFu ? -1 : i64{trialTo}));
    j.set("burst_ms", Json::num(burstMs)).set("warmup_load_ms", Json::num(warmupLoadMs));
    j.set("warmup_calm_ms", Json::num(warmupCalmMs)).set("primer_ms", Json::num(primerMs));
    j.set("warmup_load_pct", Json::num(warmupLoadPct)).set("primer_load_pct", Json::num(primerLoadPct));
    j.set("ambient_pct", Json::num(ambientPct)).set("budget_ns", Json::num(budgetNs));
    j.set("probe_multiplier", Json::num(probeMultiplier));
    return j;
}

std::vector<SbCell> stepbench_trials(const StepbenchConfig& cfg) {
    std::vector<SbCell> cells;
    for (u32 c : cfg.calmsMs) {
        for (u32 l : cfg.loadsPct) {
            for (SbTarget t : cfg.targets) {
                for (bool a : cfg.l0Ambient) cells.push_back({c, l, t, a});
            }
        }
    }
    std::vector<SbCell> trials;
    for (u32 r = 0; r < cfg.repeats; ++r) trials.insert(trials.end(), cells.begin(), cells.end());
    Pcg32 rng(cfg.seed, 0xE1u);   // seeded shuffle (docs/04 §1), logged in run.json
    for (usize i = trials.size(); i > 1; --i) std::swap(trials[i - 1], trials[rng.below(static_cast<u32>(i))]);
    return trials;
}

StepbenchSchedule::StepbenchSchedule(const StepbenchConfig& cfg) : cfg_(cfg), trials_(stepbench_trials(cfg)) {
    const u32 n = static_cast<u32>(trials_.size());
    from_ = cfg.trialFrom < n ? cfg.trialFrom : n;
    to_ = cfg.trialTo < n ? cfg.trialTo : n;
    if (to_ < from_) to_ = from_;
    cur_ = from_;
}

SbFrame StepbenchSchedule::next(i64 t) {
    SbFrame f;
    if (stageStart_ < 0) stageStart_ = t;
    const i64 ms = 1000000;
    const i64 half = cfg_.budgetNs / 2;   // phase edges snap to the nearest frame
    auto elapsed = [&] { return t - stageStart_; };
    auto advance = [&](int stage) {
        stage_ = stage;
        stageStart_ = t;
    };

    if (stage_ == 0 && elapsed() + half >= i64{cfg_.warmupLoadMs} * ms) advance(1);
    if (stage_ == 1 && elapsed() + half >= i64{cfg_.warmupCalmMs} * ms) advance(2);
    if (stage_ == 2 && elapsed() + half >= i64{cfg_.primerMs} * ms) {
        advance(3);
        inBurst_ = false;
    }
    if (stage_ == 3) {
        while (cur_ < to_) {
            const SbCell& c = trials_[cur_];
            if (!inBurst_ && elapsed() + half >= i64{c.calmMs} * ms) {
                inBurst_ = true;
                stageStart_ = t;
            }
            if (inBurst_ && elapsed() + half >= i64{cfg_.burstMs} * ms) {
                inBurst_ = false;
                stageStart_ = t;
                ++cur_;
                continue;
            }
            break;
        }
        if (cur_ >= to_) {
            done_ = true;
            f.phase = SbPhase::Done;
            return f;
        }
        const SbCell& c = trials_[cur_];
        f.trial = cur_;
        f.cell = c;
        f.phase = inBurst_ ? SbPhase::Burst : SbPhase::Calm;
        const u32 pct = inBurst_ ? c.loadPct : (c.ambient ? cfg_.ambientPct : 0u);
        if (c.target == SbTarget::Gpu || c.target == SbTarget::Both) f.gpuLoadPct = pct;
        if (c.target == SbTarget::Cpu || c.target == SbTarget::Both) f.cpuLoadPct = pct;
        return f;
    }
    f.phase = SbPhase::Warmup;
    if (stage_ == 0) {
        f.gpuLoadPct = cfg_.warmupLoadPct;
        f.cpuLoadPct = cfg_.warmupLoadPct;
        f.calibrate = true;
    } else if (stage_ == 2) {
        f.gpuLoadPct = cfg_.primerLoadPct;
        f.cpuLoadPct = cfg_.primerLoadPct;
    }
    return f;
}

} // namespace ag::lab
