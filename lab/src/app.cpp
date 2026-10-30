#include "app.h"

#include "ag/KernelBuildInfo.gen.h"
#include "ag/forecast.h"
#include "ag/gfx/Clock.h"
#include "ag/level.h"
#include "ag/pcg32.h"
#include "ag/sha256.h"
#include "loads.h"
#include "provenance.h"
#include "runio.h"
#include "sysinfo.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <thread>

namespace ag::lab {
namespace {

constexpr u32 kMults[4] = {1, 2, 4, 8};   // E0 probe linearity: 4 sizes (docs/04 E0)
constexpr u32 kInitialGpuIters = 4000;    // first warm-up frames, before calibration
constexpr u32 kInitialCpuIters = 2000000;
constexpr u8 kClear[3] = {6, 6, 10};      // near-black (docs/02 §7)

u32 g_sink = 0;   // keeps the CPU probe result alive

std::string compact_utc() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
    return buf;
}

i64 json_int(const Json* v, i64 fallback) {
    if (v == nullptr) return fallback;
    if (v->isInt()) return v->asInt();
    if (v->type() == Json::Type::Bool) return v->asBool() ? 1 : 0;
    return fallback;
}

double median(std::vector<i64> v) {
    if (v.empty()) return -1.0;
    std::sort(v.begin(), v.end());
    const usize n = v.size();
    return n % 2 == 1 ? static_cast<double>(v[n / 2]) : 0.5 * static_cast<double>(v[n / 2 - 1] + v[n / 2]);
}

struct Stats {
    double mean = 0, sd = 0, cv = 0, med = 0, robustCv = 0;
};

Stats stats(const std::vector<i64>& v) {
    Stats s;
    if (v.size() < 2) return s;
    double sum = 0;
    for (i64 x : v) sum += static_cast<double>(x);
    s.mean = sum / static_cast<double>(v.size());
    double ss = 0;
    for (i64 x : v) ss += (static_cast<double>(x) - s.mean) * (static_cast<double>(x) - s.mean);
    s.sd = std::sqrt(ss / static_cast<double>(v.size() - 1));
    s.cv = s.sd / s.mean;
    s.med = median(v);
    std::vector<i64> dev;
    dev.reserve(v.size());
    for (i64 x : v) dev.push_back(static_cast<i64>(std::llabs(x - static_cast<i64>(s.med))));
    s.robustCv = 1.4826 * median(dev) / s.med;
    return s;
}

/// R^2 of the least-squares line through (x, y).
double r_squared(const std::vector<double>& x, const std::vector<double>& y) {
    const usize n = x.size();
    if (n < 2) return 0.0;
    double mx = 0, my = 0;
    for (usize i = 0; i < n; ++i) {
        mx += x[i];
        my += y[i];
    }
    mx /= static_cast<double>(n);
    my /= static_cast<double>(n);
    double sxy = 0, sxx = 0, syy = 0;
    for (usize i = 0; i < n; ++i) {
        sxy += (x[i] - mx) * (y[i] - my);
        sxx += (x[i] - mx) * (x[i] - mx);
        syy += (y[i] - my) * (y[i] - my);
    }
    return sxx > 0 && syy > 0 ? (sxy * sxy) / (sxx * syy) : 0.0;
}

Json num_d(double v, int decimals) { return Json::raw(fmt_double(v, decimals)); }

} // namespace

struct App::E0State {
    bool cpuDone = false;
    u32 frames = 1500;     // probe stage length (25 s at 60 Hz)
    u32 warmSkip = 300;    // first 5 s under load are not used for the warm statistics
    CsvWriter probes;
    std::vector<i64> gpu[4], cpu[4];
    std::vector<i64> gpuStd, cpuStd, cpuStdCore;
    Json levels = Json::object();
    Json forecast = Json::array();
    bool levelsOk = false;
    bool forecastOk = false;
    Json summary = Json::object();
};

App::App() = default;
App::~App() {
    if (gfx_ != nullptr) gfx_->waitIdle();
}

bool App::create(const Args& a, const HostSurface& surface, std::string& err) {
    args_ = a;
    mode_ = a.get("mode");
    if (mode_ != "stepbench" && mode_ != "smoke" && mode_ != "e0" && mode_ != "idlegap") {
        err = "unknown --mode '" + mode_ + "' (stepbench, smoke, e0, idlegap)";
        return false;
    }
    const std::string device = a.get("device-id", device_model());
    runId_ = a.get("run-id", device + "_" + a.get("exp", mode_) + "_" + a.get("cell", "all") + "_" + a.get("rep", "0") +
                                 "_" + compact_utc());
    runDir_ = a.outPath("out-dir", "results") + "/" + runId_;
    if (!make_dirs(runDir_)) {
        err = "cannot create " + runDir_;
        return false;
    }

    // Stepbench design (docs/04 E1); smoke is a short fixed design (docs/03 §13).
    if (mode_ == "smoke") {
        sbCfg_.calmsMs = {250, 1000};
        sbCfg_.loadsPct = {50};
        sbCfg_.targets = {SbTarget::Gpu, SbTarget::Cpu};
        sbCfg_.l0Ambient = {false};
        sbCfg_.repeats = 2;
        sbCfg_.warmupLoadMs = 5000;
        sbCfg_.warmupCalmMs = 3000;
        sbCfg_.primerMs = 1000;
    }
    if (!sbCfg_.parse(a.get("calms"), a.get("loads"), a.get("targets"), a.get("l0"), err)) return false;
    sbCfg_.repeats = a.getU("repeats", sbCfg_.repeats);
    sbCfg_.seed = a.getU("seed", static_cast<u32>(sbCfg_.seed));
    sbCfg_.burstMs = a.getU("burst-ms", sbCfg_.burstMs);
    sbCfg_.warmupLoadMs = a.getU("warmup-ms", sbCfg_.warmupLoadMs);
    sbCfg_.warmupCalmMs = a.getU("warmup-calm-ms", sbCfg_.warmupCalmMs);
    sbCfg_.primerMs = a.getU("primer-ms", sbCfg_.primerMs);
    sbCfg_.probeMultiplier = a.getU("probe-mult", 1);
    // D53/D60: the frame budget follows the requested refresh (60 Hz primary, 120 Hz subset), so a
    // load of L % means L % of that display's frame period; the host sets the display to match.
    refreshHz_ = a.getU("req-display_refresh_hz", 60);
    if (refreshHz_ < 24 || refreshHz_ > 240) {
        err = "--req-display_refresh_hz must be 24..240";
        return false;
    }
    sbCfg_.budgetNs = (1000000000LL + refreshHz_ / 2) / refreshHz_;
    if (a.has("trials")) {
        unsigned from = 0, to = 0;
        if (std::sscanf(a.get("trials").c_str(), "%u:%u", &from, &to) != 2 || to < from) {
            err = "--trials must be from:to";
            return false;
        }
        sbCfg_.trialFrom = from;
        sbCfg_.trialTo = to;
    }
    if (mode_ == "e0") {
        e0_ = std::make_unique<E0State>();
        e0_->frames = a.getU("e0-frames", e0_->frames * refreshHz_ / 60);   // 25 s at any refresh
        e0_->warmSkip = e0_->warmSkip * refreshHz_ / 60;                      // 5 s
        e0_->warmSkip = std::min(e0_->warmSkip, e0_->frames / 4);
        if (!e0_->probes.open(runDir_ + "/e0_probes.csv", "afterglow.e0probes/1", Json::object(),
                              "frame,mult,gpu_probe_ns,cpu_probe_ns")) {
            err = "cannot write e0_probes.csv";
            return false;
        }
    } else if (mode_ == "idlegap") {
        // Gaps in ms (decimals allowed), log-spaced 0.1 ms - 2 s by default (docs/04 E1, S6).
        idleGap_ = true;
        const std::string gaps = a.get("gaps", "0.1,0.3,1,3,10,30,100,300,1000,2000");
        const u32 reps = a.getU("gap-reps", 20);
        Json gl = Json::array();
        std::vector<i64> base;
        usize start = 0;
        while (start <= gaps.size()) {
            const usize comma = gaps.find(',', start);
            const std::string item = gaps.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!item.empty()) {
                const i64 us = static_cast<i64>(std::llround(std::stod(item) * 1000.0));
                base.push_back(us * 1000);
                gl.push(Json::num(us));
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        for (u32 r = 0; r < reps; ++r) gapsNs_.insert(gapsNs_.end(), base.begin(), base.end());
        Pcg32 rng(sbCfg_.seed, 0x16u);
        for (usize i = gapsNs_.size(); i > 1; --i) std::swap(gapsNs_[i - 1], gapsNs_[rng.below(static_cast<u32>(i))]);
        gapCfg_.set("gaps_us", std::move(gl)).set("repeats", Json::num(reps)).set("seed", Json::num(static_cast<i64>(sbCfg_.seed)));
        if (!gapCsv_.open(runDir_ + "/idlegap.csv", "afterglow.idlegap/1", Json::object(),
                          "idx,gap_us,submit_ns,gpu_start_ns,gpu_end_ns,done_ns,submit_to_start_ns,round_trip_ns,gpu_ns")) {
            err = "cannot write idlegap.csv";
            return false;
        }
    } else {
        sched_ = std::make_unique<StepbenchSchedule>(sbCfg_);
    }

    gpuCal_.probeCap = kInitialGpuIters * 64;
    cpuCal_.probeCap = kInitialCpuIters * 64;
    // GPU: every pipeline and buffer is created now, before any measured frame (R5).
    gfx_ = gfx::create_gfx();
    gfx::InitDesc gd;
    gd.kernelDir = a.inPath("kernels", AG_DEFAULT_KERNEL_DIR);
    gd.surface = surface.kind;
    gd.nativeSurface = surface.ptr;
    gd.width = surface.width;
    gd.height = surface.height;
    if (!gfx_->init(gd, err)) return false;
    probePipe_ = gfx_->createPipeline("ag_probe", err);
    if (!probePipe_.valid()) return false;
    loadPipe_ = gfx_->createPipeline("ag_load", err);
    if (!loadPipe_.valid()) return false;
    probeBuf_ = gfx_->createBuffer(kGpuProbeGroups * 64 * 4, gfx::Memory::HostShared, "probeOut");
    probeExtraBuf_ = gfx_->createBuffer(kGpuProbeGroups * 64 * 4, gfx::Memory::HostShared, "probeExtraOut");
    loadBuf_ = gfx_->createBuffer(u64{kGpuLoadGroups} * 64 * 4, gfx::Memory::DeviceLocal, "loadOut");
    wideGroups_ = a.getU("probe-wide-groups", 0);
    wideIters_ = a.getU("probe-wide-iters", kGpuProbeIters);
    if (wideGroups_ > 0) {
        wideBuf_ = gfx_->createBuffer(u64{wideGroups_} * 64 * 4, gfx::Memory::DeviceLocal, "probeWideOut");
        if (!wideCsv_.open(runDir_ + "/probe_wide.csv", "afterglow.probewide/1", Json::object(), "frame,gpu_probe_wide_ns")) {
            err = "cannot write probe_wide.csv";
            return false;
        }
    }
    if (!probeBuf_.valid() || !probeExtraBuf_.valid() || !loadBuf_.valid() || (wideGroups_ > 0 && !wideBuf_.valid())) {
        err = "buffer allocation failed";
        return false;
    }

    Json meta = Json::object();
    meta.set("run_id", Json::str(runId_));
    if (!frames_.open(runDir_ + "/frames.csv", meta) || !device_.open(runDir_ + "/device.csv", meta)) {
        err = "cannot write the frame or device log in " + runDir_;
        return false;
    }

    RunInfo ri;
    ri.mode = mode_;
    ri.runId = runId_;
    ri.deviceId = a.get("device-id");
    ri.args = a.raw;
    ri.workload.set("preset", Json::str("n/a: stepbench runs synthetic loads"));
    ri.workload.set("level", Json::str("n/a: stepbench runs synthetic loads"));
    ri.workload.set("policy", Json::str(a.get("policy", "P0")));
    ri.workload.set("seed", Json::num(static_cast<i64>(sbCfg_.seed)));
    ri.workload.set("expected_trace_sha256", Json::str("n/a: stepbench"));
    ri.workload.set("actual_trace_sha256", Json::str("n/a: stepbench"));
    ri.input.set("path", Json::str("none"));
    ri.osGame.set("app_category", Json::str(a.get("app-category", "game")));
    {   // The host reports the OS game features it can see (Game Mode on Android; n/a on iOS).
        Json host;
        std::string perr;
        if (Json::parse(platform_state_json(), host, perr) && host.get("os_game") != nullptr) {
            for (const auto& kv : host.get("os_game")->members()) ri.osGame.set(kv.first, kv.second);
        }
    }
    for (const char* k : {"display_refresh_hz", "brightness", "charging", "low_power_mode"}) {
        const std::string key = std::string("req-") + k;
        if (a.has(key)) {
            const std::string v = a.get(key);
            if (v == "true" || v == "false") ri.requested.set(k, Json::boolean(v == "true"));
            else ri.requested.set(k, Json::num(std::stoll(v)));
        }
    }
    prov_ = build_provenance(ri);
    // Thermal gate (docs/04 §1): the run starts only once the device reports nominal (iOS) or
    // NONE (Android). Waiting frames are cleared and presented but not logged. 0 disables it.
    gateUntil_ = static_cast<i64>(a.getU("thermal-gate-s", 0)) * 1000000000;
    gating_ = gateUntil_ > 0;
    // The run mode is stepbench-like for the n/a rules (D27): smoke and e0 have no level either.
    if (mode_ != "stepbench") prov_.getMutable("run")->set("mode", Json::str(mode_));
    created_ = true;
    message_ = "running " + mode_ + " -> " + runDir_;
    return true;
}

void App::Calib::add(i64 durationNs, u32 iters) {
    if (iters == 0 || durationNs <= 0) return;
    if (durationNs < 500000) {   // overhead-dominated: grow the next calibration size instead
        const u32 base = std::max(probeIters, iters);
        probeIters = base <= probeCap / 4 ? base * 4 : probeCap;
        return;
    }
    samples.push_back(static_cast<double>(durationNs) / iters);
    maxMeasured = std::max(maxMeasured, iters);
    std::vector<double> s = samples;
    std::sort(s.begin(), s.end());
    nsPerIter = s[(s.size() - 1) / 10];   // 10th percentile: warm, but robust to one bogus sample
    probeIters = 0;
}

u32 App::Calib::itersFor(u32 pct, i64 budgetNs, u32 initial) const {
    if (pct == 0) return 0;
    if (nsPerIter <= 0) {
        const double it = static_cast<double>(probeIters > 0 ? probeIters : initial) * pct / 70.0;
        return static_cast<u32>(std::clamp(it, 1.0, 2000000000.0));
    }
    const double it = (static_cast<double>(pct) / 100.0) * static_cast<double>(budgetNs) / nsPerIter;
    return static_cast<u32>(std::clamp(it, 1.0, 4.0 * static_cast<double>(maxMeasured)));
}

u32 App::gpuItersFor(u32 pct) const { return gpuCal_.itersFor(pct, sbCfg_.budgetNs, kInitialGpuIters); }
u32 App::cpuItersFor(u32 pct) const { return cpuCal_.itersFor(pct, sbCfg_.budgetNs, kInitialCpuIters); }

int App::frame(i64 targetPresentNs, i64 periodNs) {
    if (!created_) return -1;
    if (finished_) return 1;
    const i64 t0 = gfx::now_ns();
    if (periodNs > 0) periodNs_ = periodNs;
    if (gating_) {
        if (gateStart_ < 0) gateStart_ = t0;
        const Json st = device_state();
        const Json* th = st.get("thermal_state");
        const std::string thermal = th != nullptr ? th->asString() : "unknown";
        const bool cool = thermal == "nominal" || thermal == "none";
        if (!cool && t0 - gateStart_ < gateUntil_) {
            message_ = "waiting for a nominal thermal state (now " + thermal + ")";
            gfx_->begin();
            gfx_->clearAndPresent(kClear[0], kClear[1], kClear[2]);
            const gfx::ListId id = gfx_->submit();
            gfx_->wait(id);
            return 0;
        }
        gating_ = false;
        prov_.set("state_start", st);   // the run starts now
        Json g = Json::object();
        g.set("waited_ms", Json::num((t0 - gateStart_) / 1000000)).set("passed", Json::boolean(cool));
        prov_.set("thermal_gate", std::move(g));
        message_ = "running " + mode_ + " -> " + runDir_;
    }
    if (lastDeviceSample_ < 0 || t0 - lastDeviceSample_ >= 1000000000) sampleDevice(t0);
    if (idleGap_) return idleGapFrame();

    Pending p;
    FrameRow& r = p.row;
    auto set = [&](const char* col, i64 v) { r.at(static_cast<usize>(frame_column(col))) = v; };
    set("frame", frame_);
    set("tick", frame_);
    set("ticks_run", 1);
    set("t_frame_start_ns", t0);
    if (targetPresentNs > 0) set("present_target_ns", targetPresentNs);
    set("policy_state", 0);
    set("heater_gpu_us", 0);
    set("heater_cpu_us", 0);
    set("hint_calls", 0);

    // 1. CPU probe (docs/03 §6): fixed size, every frame.
    i64 c0 = gfx::cpu_now_ns();
    g_sink ^= cpu_spin(kCpuProbeIters, static_cast<u32>(frame_));
    set("cpu_probe_ns", gfx::cpu_now_ns() - c0);
    const i64 core = CpuInfo::cpu();   // after the timed probe, so the reads cost the probe nothing
    if (core >= 0) {
        set("cpu_id", core);
        const i64 khz = cpuInfo_.freqKhz(core);
        if (khz >= 0) set("cpu_freq_khz", khz);
    }

    // 2. What the mode asks of this frame.
    u32 gpuPct = 0, cpuPct = 0;
    bool calibrate = false;
    if (sched_ != nullptr) {
        const SbFrame f = sched_->next(t0);
        if (f.phase == SbPhase::Done) {
            finish(false, "");
            return 1;
        }
        set("sb_phase", static_cast<i64>(f.phase));
        if (f.trial >= 0) {
            set("sb_trial", f.trial);
            set("sb_calm_ms", f.cell.calmMs);
            set("sb_load_pct", f.cell.loadPct);
            set("sb_target", static_cast<i64>(f.cell.target));
        }
        gpuPct = f.gpuLoadPct;
        cpuPct = f.cpuLoadPct;
        calibrate = f.calibrate;
    } else if (e0_ != nullptr) {
        if (frame_ >= static_cast<i64>(e0_->frames)) {
            finish(false, "");
            return 1;
        }
        // Saturating load on both processors (D42, D44): a load that fits the frame lets a
        // deadline-driven governor sit below its top level, and the "warm" CV would then measure
        // the governor's hunting instead of the probe.
        gpuPct = sbCfg_.warmupLoadPct;
        cpuPct = sbCfg_.warmupLoadPct;
        calibrate = frame_ < static_cast<i64>(e0_->warmSkip);
        p.probeMult = kMults[frame_ % 4];
    }

    // 3. CPU work (stepbench: the synthetic CPU load in the "sim" slot, D25).
    if (cpuPct > 0) {
        const u32 iters = cpuItersFor(cpuPct);
        c0 = gfx::cpu_now_ns();
        g_sink ^= cpu_spin(iters, static_cast<u32>(frame_) * 7u);
        const i64 dt = gfx::cpu_now_ns() - c0;
        set("cpu_sim_ns", dt);
        if (calibrate) cpuCal_.add(dt, iters);
    }
    if (p.probeMult > 0) {
        c0 = gfx::cpu_now_ns();
        g_sink ^= cpu_spin(kCpuProbeIters * p.probeMult, static_cast<u32>(frame_));
        p.cpuProbeExtraNs = gfx::cpu_now_ns() - c0;
    }

    // 4. Encode: GPU probe in its own list at frame start, then the load, then clear + present.
    const i64 e0 = gfx::cpu_now_ns();
    SpinParams sp{kGpuProbeIters * sbCfg_.probeMultiplier, probeSeed_, 0, 0};
    gfx::Bind pb{"probeOut", probeBuf_};
    gfx_->begin();
    gfx_->dispatch(probePipe_, &pb, 1, &sp, sizeof(sp), kGpuProbeGroups);
    p.probe = gfx_->submit();
    if (wideGroups_ > 0) {
        SpinParams sw{wideIters_, probeSeed_, 0, 0};
        gfx::Bind wb{"probeOut", wideBuf_};
        gfx_->begin();
        gfx_->dispatch(probePipe_, &wb, 1, &sw, sizeof(sw), wideGroups_);
        p.probeWide = gfx_->submit();
    }
    if (p.probeMult > 0) {
        SpinParams sx{kGpuProbeIters * p.probeMult, probeSeed_, 0, 0};
        gfx::Bind xb{"probeOut", probeExtraBuf_};
        gfx_->begin();
        gfx_->dispatch(probePipe_, &xb, 1, &sx, sizeof(sx), kGpuProbeGroups);
        p.probeExtra = gfx_->submit();
    }
    if (gpuPct > 0) {
        p.gpuLoadIters = gpuItersFor(gpuPct);
        p.calibrate = calibrate;
        SpinParams lp{p.gpuLoadIters, static_cast<u32>(frame_), 0, 0};
        gfx::Bind lb{"loadOut", loadBuf_};
        gfx_->begin();
        gfx_->dispatch(loadPipe_, &lb, 1, &lp, sizeof(lp), kGpuLoadGroups);
        p.load = gfx_->submit();
    }
    gfx_->begin();
    gfx_->clearAndPresent(kClear[0], kClear[1], kClear[2]);
    p.present = gfx_->submit();
    set("cpu_encode_ns", gfx::cpu_now_ns() - e0);

    pending_.push_back(p);
    ++frame_;
    drain(false);
    return 0;
}

void App::drain(bool all) {
    while (!pending_.empty()) {
        Pending& p = pending_.front();
        bool ready = true;
        for (gfx::ListId id : {p.probe, p.probeWide, p.probeExtra, p.load, p.present}) {
            if (id != 0 && !gfx_->timing(id).done) ready = false;
        }
        if (!ready) {
            if (!all && pending_.size() <= 4) return;
            for (gfx::ListId id : {p.probe, p.probeWide, p.probeExtra, p.load, p.present}) {
                if (id != 0) gfx_->wait(id);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));   // presented handler, at most 250 ms
            continue;
        }
        complete(p);
        pending_.pop_front();
    }
}

void App::complete(Pending& p) {
    FrameRow& r = p.row;
    auto set = [&](const char* col, i64 v) { r.at(static_cast<usize>(frame_column(col))) = v; };
    const gfx::ListTiming tp = gfx_->timing(p.probe);
    if (tp.durationNs >= 0 && !tp.error) {
        set("gpu_probe_ns", tp.durationNs);
        if (tp.startNs >= 0) set("gpu_probe_start_ns", tp.startNs);
    } else {
        ++missingGpu_;
    }
    if (p.load != 0) {
        const gfx::ListTiming tl = gfx_->timing(p.load);
        if (tl.durationNs >= 0 && !tl.error) {
            set("gpu_main_ns", tl.durationNs);
            if (p.calibrate) gpuCal_.add(tl.durationNs, p.gpuLoadIters);
        }
    }
    if (p.probeWide != 0) {
        const gfx::ListTiming tw = gfx_->timing(p.probeWide);
        wideCsv_.row(std::to_string(r.v[0]) + "," + (tw.durationNs >= 0 ? std::to_string(tw.durationNs) : std::string()));
    }
    const gfx::ListTiming tr = gfx_->timing(p.present);
    if (tr.presentedNs > 0) {
        set("present_actual_ns", tr.presentedNs);
        if (lastPresented_ > 0) {
            const bool late = static_cast<double>(tr.presentedNs - lastPresented_) > 1.5 * static_cast<double>(periodNs_);
            set("late", late ? 1 : 0);
            lateFrames_ += late ? 1u : 0u;
        }
        lastPresented_ = tr.presentedNs;
    }
    if (e0_ != nullptr && p.probeMult > 0) {
        const gfx::ListTiming tx = gfx_->timing(p.probeExtra);
        const i64 frame = r.v[0];
        if (tx.durationNs >= 0 && frame >= static_cast<i64>(e0_->warmSkip)) {
            usize mi = 0;
            while (kMults[mi] != p.probeMult) ++mi;
            e0_->gpu[mi].push_back(tx.durationNs);
            e0_->cpu[mi].push_back(p.cpuProbeExtraNs);
            if (tp.durationNs >= 0) e0_->gpuStd.push_back(tp.durationNs);
            e0_->cpuStd.push_back(r.at(static_cast<usize>(frame_column("cpu_probe_ns"))));
            e0_->cpuStdCore.push_back(r.at(static_cast<usize>(frame_column("cpu_id"))));
        }
        e0_->probes.row(std::to_string(frame) + "," + std::to_string(p.probeMult) + "," +
                        (tx.durationNs >= 0 ? std::to_string(tx.durationNs) : std::string()) + "," +
                        std::to_string(p.cpuProbeExtraNs));
    }
    frames_.write(r);
    ++rows_;
}

int App::idleGapFrame() {
    SpinParams sp{kGpuProbeIters, probeSeed_, 0, 0};
    gfx::Bind pb{"probeOut", probeBuf_};
    if (gapIdx_ == 0 && frame_ == 0) {
        for (int i = 0; i < 3; ++i) {   // every kernel runs once before measurement (R5)
            gfx_->begin();
            gfx_->dispatch(probePipe_, &pb, 1, &sp, sizeof(sp), kGpuProbeGroups);
            gfx_->wait(gfx_->submit());
        }
    }
    ++frame_;
    if (gapIdx_ >= gapsNs_.size()) {
        finish(false, "");
        return 1;
    }
    const i64 gap = gapsNs_[gapIdx_];
    std::this_thread::sleep_for(std::chrono::nanoseconds(gap));   // no GPU work, no present
    gfx_->begin();
    gfx_->dispatch(probePipe_, &pb, 1, &sp, sizeof(sp), kGpuProbeGroups);
    const i64 ts = gfx::now_ns();
    const gfx::ListId id = gfx_->submit();
    gfx_->wait(id);
    const i64 td = gfx::now_ns();
    gfx::ListTiming t = gfx_->timing(id);
    for (int i = 0; i < 1000 && !t.done; ++i) {   // the completion handler can trail the wait
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        t = gfx_->timing(id);
    }
    auto num = [](i64 v) { return v >= 0 ? std::to_string(v) : std::string(); };
    const i64 s2s = t.startNs >= 0 ? t.startNs - ts : -1;
    gapCsv_.row(std::to_string(gapIdx_) + "," + std::to_string(gap / 1000) + "," + std::to_string(ts) + "," +
                num(t.startNs) + "," + num(t.endNs) + "," + std::to_string(td) + "," +
                (t.startNs >= 0 ? std::to_string(s2s) : std::string()) + "," + std::to_string(td - ts) + "," +
                num(t.durationNs));
    if (t.durationNs < 0) ++missingGpu_;
    ++rows_;
    ++gapIdx_;
    return 0;
}

void App::sampleDevice(i64 now) {
    const Json st = device_state();
    DeviceRow d;
    d.tNs = now;
    const Json* th = st.get("thermal_state");
    d.thermalState = th != nullptr ? th->asString() : "unknown";
    d.batteryPct = json_int(st.get("battery_percent"), kNA);
    d.charging = json_int(st.get("charging"), kNA);
    d.currentUa = json_int(st.get("current_ua"), kNA);
    d.voltageMv = json_int(st.get("voltage_mv"), kNA);
    d.brightnessPct = json_int(st.get("brightness"), kNA);
    d.lowPowerMode = json_int(st.get("low_power_mode"), kNA);
    device_.write(d);
    lastDeviceSample_ = now;
}

void App::platformState(const std::string& json) { set_platform_state_json(json); }

void App::event(const std::string& name) {
    if (name == "background" || name == "interrupt") finish(true, "app " + name);
}

void App::runE0CpuChecks() {
    // Level determinism on this device (docs/04 E0): every research level's trace hash must equal
    // the one aglab recorded (R1).
    const std::string dir = args_.inPath("levels", "bench/levels");
    std::string text, err;
    Json manifest;
    CsvWriter lv;
    lv.open(runDir_ + "/e0_levels.csv", "afterglow.e0levels/1", Json::object(),
            "id,file_sha_ok,trace_ok,expected_trace_sha256,actual_trace_sha256,run_us");
    int n = 0, ok = 0;
    std::vector<State> states;
    if (read_text(dir + "/manifest.json", text) && Json::parse(text, manifest, err) && manifest.get("levels") != nullptr) {
        for (const Json& e : manifest.get("levels")->items()) {
            const std::string path = dir + "/" + e.get("file")->asString();
            const bool fileOk = sha256_file_hex(path) == e.get("sha256")->asString();
            Level level;
            bool traceOk = false;
            std::string actual;
            i64 us = -1;
            if (load_level(path, level, err)) {
                const i64 c0 = gfx::cpu_now_ns();
                const TraceResult res = run_level(level, 1);
                us = (gfx::cpu_now_ns() - c0) / 1000;
                actual = res.sha256;
                traceOk = res.sha256 == level.expectedTraceSha256 && res.passMismatches == 0;
                // Cascade states for the forecaster cost (every 10th tick with live blasts).
                auto s = std::make_unique<State>();
                Sim sim(1);
                init_state(level, *s);
                Descriptor d;
                while (!ag::finished(*s) && s->tick < level.maxTicks && states.size() < 600) {
                    sim.tick(*s, bot_input(level, s->tick), d);
                    if (d.liveBlasts > 0 && s->tick % 10 == 0) states.push_back(*s);
                }
            }
            ++n;
            ok += (fileOk && traceOk) ? 1 : 0;
            lv.row(e.get("id")->asString() + "," + (fileOk ? "1" : "0") + "," + (traceOk ? "1" : "0") + "," +
                   level.expectedTraceSha256 + "," + actual + "," + std::to_string(us));
        }
    }
    lv.close();
    e0_->levelsOk = n > 0 && ok == n;
    e0_->levels.set("levels", Json::num(n)).set("matching", Json::num(ok)).set("pass", Json::boolean(e0_->levelsOk));
    if (n == 0) e0_->levels.set("error", Json::str("no levels found under " + dir));

    // Forecaster cost (docs/03 §5): must stay below 0.5 ms warm at the P4 horizon (24 ticks).
    Forecaster fc(1);
    std::vector<Descriptor> out(Forecaster::kMaxHorizon);
    CsvWriter fcsv;
    fcsv.open(runDir_ + "/e0_forecast.csv", "afterglow.e0forecast/1", Json::object(),
              "horizon_ticks,samples,median_us,p95_us,max_us");
    for (u32 h : {6u, 12u, 24u}) {
        std::vector<i64> ns;
        for (int rep = 0; rep < 3; ++rep) {
            for (const State& st : states) {
                const i64 c0 = gfx::cpu_now_ns();
                fc.forecast(st, h, nullptr, out.data());
                ns.push_back(gfx::cpu_now_ns() - c0);
            }
        }
        if (ns.empty()) continue;
        std::sort(ns.begin(), ns.end());
        const double med = static_cast<double>(ns[ns.size() / 2]) / 1000.0;
        const double p95 = static_cast<double>(ns[ns.size() * 95 / 100]) / 1000.0;
        const double mx = static_cast<double>(ns.back()) / 1000.0;
        fcsv.row(std::to_string(h) + "," + std::to_string(ns.size()) + "," + fmt_double(med, 2) + "," + fmt_double(p95, 2) +
                 "," + fmt_double(mx, 2));
        Json fj = Json::object();
        fj.set("horizon_ticks", Json::num(h)).set("median_us", num_d(med, 2)).set("p95_us", num_d(p95, 2));
        e0_->forecast.push(std::move(fj));
        if (h == 24) e0_->forecastOk = med < 500.0;
    }
    fcsv.close();
    e0_->cpuDone = true;
}

void App::finishE0() {
    runE0CpuChecks();   // after the loaded probe stage, so the CPU is warm
    e0_->probes.close();
    Json s = Json::object();
    auto probe_block = [&](std::vector<i64>* per, const std::vector<i64>& std1, const char* what) {
        Json b = Json::object();
        std::vector<double> xm, ym, xs, ys;
        Json meds = Json::array();
        for (usize i = 0; i < 4; ++i) {
            const double m = median(per[i]);
            meds.push(num_d(m, 0));
            if (m > 0) {
                xm.push_back(kMults[i]);
                ym.push_back(m);
            }
            for (i64 v : per[i]) {
                xs.push_back(kMults[i]);
                ys.push_back(static_cast<double>(v));
            }
        }
        const Stats st = stats(std1);
        const double r2 = r_squared(xm, ym);
        b.set("what", Json::str(what));
        b.set("median_ns_by_mult_1_2_4_8", std::move(meds));
        b.set("r2_medians", num_d(r2, 5)).set("r2_samples", num_d(r_squared(xs, ys), 5));
        b.set("warm_samples", Json::num(static_cast<i64>(std1.size())));
        b.set("warm_median_ns", num_d(st.med, 0)).set("warm_cv", num_d(st.cv, 4)).set("warm_cv_robust", num_d(st.robustCv, 4));
        b.set("pct_of_frame", num_d(100.0 * st.med / static_cast<double>(periodNs_), 3));
        b.set("pass_linear", Json::boolean(r2 >= 0.99));
        // D45 (user decision 2026-10-07): robust CV (1.4826 MAD / median) < 5 %. The plain CV is
        // reported too, but rare large outliers (core moves, interrupts) are power management, not
        // probe noise.
        b.set("pass_cv_robust_5pct", Json::boolean(st.robustCv < 0.05));
        return b;
    };
    Json g = probe_block(e0_->gpu, e0_->gpuStd, "GPU probe (ag_probe, 4 x 64 threads)");
    // The probe's result is checked against the CPU reference, so the work cannot be removed (docs/03 §6).
    const auto* out = static_cast<const u32*>(gfx_->map(probeBuf_));
    bool resultOk = out != nullptr;
    for (u32 t = 0; resultOk && t < kGpuProbeGroups * 64; ++t) {
        resultOk = out[t] == spin_reference(t, probeSeed_, kGpuProbeIters * sbCfg_.probeMultiplier);
    }
    g.set("result_matches_cpu_reference", Json::boolean(resultOk));
    g.set("pass_budget_2pct", Json::boolean(g.get("pct_of_frame") != nullptr &&
                                            median(e0_->gpuStd) < 0.02 * static_cast<double>(periodNs_)));
    s.set("gpu_probe", std::move(g));
    Json cpuBlock = probe_block(e0_->cpu, e0_->cpuStd, "CPU probe (dependent integer loop, render thread)");
    {   // Per core (D45): judge stability within the core the render thread ran on most; report hops.
        std::map<i64, std::vector<i64>> byCore;
        for (usize i = 0; i < e0_->cpuStd.size() && i < e0_->cpuStdCore.size(); ++i) {
            byCore[e0_->cpuStdCore[i]].push_back(e0_->cpuStd[i]);
        }
        Json cores = Json::array();
        i64 mainCore = kNA;
        usize mainN = 0;
        for (const auto& kv : byCore) {
            const Stats cs = stats(kv.second);
            Json cj = Json::object();
            cj.set("cpu_id", kv.first == kNA ? Json::str("unknown") : Json::num(kv.first));
            cj.set("frames", Json::num(static_cast<i64>(kv.second.size())));
            cj.set("median_ns", num_d(cs.med, 0)).set("robust_cv", num_d(cs.robustCv, 4));
            cores.push(std::move(cj));
            if (kv.second.size() > mainN) {
                mainN = kv.second.size();
                mainCore = kv.first;
            }
        }
        cpuBlock.set("by_core", std::move(cores));
        if (mainN > 0) {
            const Stats ms = stats(byCore[mainCore]);
            cpuBlock.set("main_core", mainCore == kNA ? Json::str("unknown (not reported on this platform)") : Json::num(mainCore));
            cpuBlock.set("main_core_share", num_d(static_cast<double>(mainN) / static_cast<double>(e0_->cpuStd.size()), 3));
            cpuBlock.set("main_core_robust_cv", num_d(ms.robustCv, 4));
            cpuBlock.set("pass_cv_robust_5pct", Json::boolean(ms.robustCv < 0.05));
        }
    }
    s.set("cpu_probe", std::move(cpuBlock));
    s.set("levels", e0_->levels);
    s.set("forecast", e0_->forecast);
    s.set("forecast_pass_median_below_500us_at_h24", Json::boolean(e0_->forecastOk));
    s.set("not_measured", Json::str("probe isolation from a concurrent queue (docs/03 §6): not implemented yet"));
    e0_->summary = s;
    write_text(runDir_ + "/e0_summary.json", s.dump(1) + "\n");
}

Json App::buildRunJson(bool interrupted, const std::string& why) {
    Json p = prov_;
    p.getMutable("device")->set("gpu_name", Json::str(gfx_->deviceInfo().deviceName));
    Json g = Json::object();
    g.set("api", Json::str(gfx_->deviceInfo().api));
    g.set("driver_version", Json::str(gfx_->deviceInfo().driverVersion));
    for (const auto& kv : gfx_->deviceInfo().props) g.set(kv.first, Json::str(kv.second));
    Json pipes = Json::array();
    for (const auto& pi : gfx_->pipelines()) {
        Json pj = Json::object();
        pj.set("kernel", Json::str(pi.kernel)).set("binary_sha256", Json::str(pi.binarySha256));
        pipes.push(std::move(pj));
    }
    g.set("pipelines", std::move(pipes));
    g.set("errors", Json::num(static_cast<i64>(gfx_->errorCount())));
    if (gfx_->errorCount() > 0) g.set("first_error", Json::str(gfx_->firstError()));
    p.set("gfx", std::move(g));
    Json k = Json::object();
    k.set("slang_version", Json::str(AG_SLANG_VERSION_REPORTED)).set("slang_pinned", Json::str(AG_SLANG_VERSION_PINNED));
    k.set("spirv_profile", Json::str(AG_SLANG_SPIRV_PROFILE)).set("slang_flags", Json::str(AG_SLANG_FLAGS));
    k.set("metal_compiler", Json::str(AG_METAL_COMPILER_VERSION)).set("metal_flags", Json::str(AG_METAL_FLAGS));
    p.set("kernels", std::move(k));
    Json pr = Json::object();
    pr.set("gpu_probe_iters", Json::num(kGpuProbeIters)).set("gpu_probe_groups", Json::num(kGpuProbeGroups));
    pr.set("gpu_probe_multiplier", Json::num(sbCfg_.probeMultiplier)).set("cpu_probe_iters", Json::num(kCpuProbeIters));
    pr.set("gpu_load_groups", Json::num(kGpuLoadGroups));
    if (wideGroups_ > 0) {
        pr.set("exploratory_wide_probe_groups", Json::num(wideGroups_));
        pr.set("exploratory_wide_probe_iters", Json::num(wideIters_));
    }
    p.set("probes", std::move(pr));
    if (sched_ != nullptr) {
        Json sb = sbCfg_.toJson();
        sb.set("trials_total", Json::num(static_cast<i64>(sched_->trialCount())));
        sb.set("trials_run_from", Json::num(sched_->firstTrial())).set("trials_run_to", Json::num(sched_->lastTrial()));
        sb.set("gpu_ns_per_iter_warm", num_d(gpuCal_.nsPerIter, 4)).set("cpu_ns_per_iter_warm", num_d(cpuCal_.nsPerIter, 5));
        sb.set("gpu_calibration_samples", Json::num(static_cast<i64>(gpuCal_.samples.size())));
        sb.set("cpu_calibration_samples", Json::num(static_cast<i64>(cpuCal_.samples.size())));
        sb.set("gpu_iters_at_50pct", Json::num(gpuItersFor(50))).set("cpu_iters_at_50pct", Json::num(cpuItersFor(50)));
        p.set("stepbench", std::move(sb));
    }
    if (e0_ != nullptr) p.set("e0", e0_->summary);
    if (idleGap_) {
        Json ig = gapCfg_;
        ig.set("measurements", Json::num(static_cast<i64>(gapIdx_)));
        p.set("idlegap", std::move(ig));
    }
    Json c = Json::object();
    c.set("frames", Json::num(frame_)).set("rows", Json::num(static_cast<i64>(rows_)));
    c.set("missing_gpu_probe", Json::num(static_cast<i64>(missingGpu_))).set("late", Json::num(static_cast<i64>(lateFrames_)));
    c.set("period_ns", Json::num(periodNs_));
    p.set("counts", std::move(c));
    finish_provenance(p, interrupted);
    Json* status = p.getMutable("status");
    if (!why.empty()) status->set("reason", Json::str(why));
    if (gfx_->errorCount() > 0) {
        Json* reasons = status->getMutable("invalid_reasons");
        reasons->push(Json::str("GPU errors: " + std::to_string(gfx_->errorCount()) + " (" + gfx_->firstError() + ")"));
    }
    return p;
}

int App::finish(bool interrupted, const std::string& why) {
    if (finished_ || !created_) return 0;
    drain(true);
    gfx_->waitIdle();
    if (e0_ != nullptr && !interrupted) finishE0();
    if (idleGap_) gapCsv_.close();
    if (wideGroups_ > 0) wideCsv_.close();
    frames_.close();
    sampleDevice(gfx::now_ns());
    device_.close();
    const Json run = buildRunJson(interrupted, why);
    write_text(runDir_ + "/run.json", run.dump(1) + "\n");
    const std::string status = interrupted ? "status=interrupted: " + why : (why.empty() ? "status=ok" : "status=error: " + why);
    write_text(runDir_ + "/DONE", status + "\n");
    finished_ = true;
    message_ = "finished " + runId_ + " (" + status + ", " + std::to_string(rows_) + " frames)";
    return 0;
}

} // namespace ag::lab
