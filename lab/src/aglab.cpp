// aglab: the headless lab runner on macOS (docs/03 §11). Modes: selftest, sim, levelgen,
// forecast-bench, info. Timing here is only for development (the Mac is not a valid timing
// platform unless declared idle); research timing happens on the devices.
#include "aglab.h"

#include "app.h"
#include "ag/gfx/Clock.h"

#include "ag/build_info.h"
#include "ag/forecast.h"
#include "ag/level.h"
#include "ag/sha256.h"
#include "ag/workload.h"
#include "args.h"
#include "framelog.h"
#include "levelgen.h"
#include "provenance.h"
#include "runio.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <thread>

namespace ag::lab {
namespace {

int fail(const std::string& msg) {
    std::fprintf(stderr, "aglab: %s\n", msg.c_str());
    return 2;
}

/// Level files listed in <dir>/manifest.json, with their recorded SHA-256.
struct ManifestEntry {
    std::string file, sha256, id;
};

bool read_manifest(const std::string& dir, std::vector<ManifestEntry>& out, std::string& err) {
    std::string text;
    if (!read_text(dir + "/manifest.json", text)) {
        err = "cannot read " + dir + "/manifest.json";
        return false;
    }
    Json j;
    if (!Json::parse(text, j, err)) return false;
    const Json* levels = j.get("levels");
    if (levels == nullptr || !levels->isArray()) {
        err = "manifest has no levels";
        return false;
    }
    for (const Json& e : levels->items()) {
        const Json* f = e.get("file");
        const Json* s = e.get("sha256");
        const Json* id = e.get("id");
        if (f == nullptr || s == nullptr || id == nullptr) {
            err = "bad manifest entry";
            return false;
        }
        out.push_back({f->asString(), s->asString(), id->asString()});
    }
    return true;
}

std::string bursts_text(const std::vector<BurstStats>& bursts) {
    std::string s;
    for (const BurstStats& b : bursts) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s[onset=%d end=%u peak=%u det=%u]", s.empty() ? "" : " ",
                      b.onset == kNoOnset ? -1 : static_cast<int>(b.onset), b.end, b.peakLiveBlasts, b.detonations);
        s += buf;
    }
    return s;
}

bool same_bursts(const std::vector<BurstStats>& a, const std::vector<BurstStats>& b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (a[i].start != b[i].start || a[i].onset != b[i].onset || a[i].end != b[i].end ||
            a[i].peakLiveBlasts != b[i].peakLiveBlasts || a[i].peakDetonations != b[i].peakDetonations ||
            a[i].detonations != b[i].detonations) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- selftest
struct Checks {
    int failed = 0;
    int passed = 0;
    void check(bool ok, const std::string& what) {
        std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
        (ok ? passed : failed) += 1;
    }
};

/// Forecast exactness on a level: from every 'stride'-th tick, forecasting h ticks with the bot's
/// future inputs must reproduce the real descriptors exactly (docs/03 §5).
bool forecast_exact(const Level& lv, const std::vector<Descriptor>& trace, u32 h, u32 stride, u32& windows) {
    auto s = std::make_unique<State>();
    auto sim = std::make_unique<Sim>(1);
    auto fc = std::make_unique<Forecaster>(1);
    init_state(lv, *s);
    std::vector<Descriptor> out(h);
    std::vector<TickInput> inputs(h);
    windows = 0;
    Descriptor d;
    while (s->tick < trace.size()) {
        const u32 t = s->tick;
        if (t % stride == 0 && t + h <= trace.size()) {
            for (u32 i = 0; i < h; ++i) inputs[i] = bot_input(lv, t + i);
            const u32 n = fc->forecast(*s, h, inputs.data(), out.data());
            for (u32 i = 0; i < n; ++i) {
                if (!(out[i] == trace[t + i])) return false;
            }
            ++windows;
        }
        sim->tick(*s, bot_input(lv, t), d);
    }
    return true;
}

int mode_selftest(const Args& a) {
    Checks c;
    const CoreBuildInfo bi = core_build_info();
    c.check(!bi.fmaContracted, "FMA canary: a*b+c is not contracted (-ffp-contract=off honoured)");
    c.check(!bi.fastMath, "core compiled without fast-math");

    const std::string dir = a.inPath("levels", "bench/levels");
    std::vector<ManifestEntry> manifest;
    std::string err;
    if (!read_manifest(dir, manifest, err)) return fail(err);
    c.check(!manifest.empty(), "manifest lists " + std::to_string(manifest.size()) + " levels");

    bool snapshotDone = false;
    for (const ManifestEntry& m : manifest) {
        const std::string path = dir + "/" + m.file;
        c.check(sha256_file_hex(path) == m.sha256, m.id + ": file SHA-256 matches the manifest");
        Level lv;
        if (!load_level(path, lv, err)) {
            c.check(false, err);
            continue;
        }
        std::vector<Descriptor> trace;
        const TraceResult r1 = run_level(lv, 1, &trace);
        const TraceResult r4 = run_level(lv, 4);
        c.check(r1.sha256 == lv.expectedTraceSha256 && r1.ticks == lv.expectedTicks &&
                    r1.detonations == lv.expectedDetonations && same_bursts(r1.bursts, lv.expectedBursts),
                m.id + ": workload trace matches the expected hash " + lv.expectedTraceSha256.substr(0, 16));
        c.check(r4.sha256 == r1.sha256 && r4.passMismatches == 0,
                m.id + ": collision passes 1 and 4 give the same cascade (D20)");
        u32 windows = 0;
        c.check(forecast_exact(lv, trace, 24, 5, windows) && windows > 0,
                m.id + ": forecast exact over " + std::to_string(windows) + " windows of 24 ticks");

        if (!snapshotDone && !lv.expectedBursts.empty() && lv.expectedBursts[0].onset != kNoOnset) {
            // Snapshot/restore mid-cascade: a copy continues exactly like the original.
            auto s = std::make_unique<State>();
            auto sim = std::make_unique<Sim>(1);
            init_state(lv, *s);
            Descriptor d1, d2;
            const u32 mid = lv.expectedBursts[0].onset + 20;
            while (s->tick < mid) sim->tick(*s, bot_input(lv, s->tick), d1);
            auto snap = std::make_unique<State>(*s);
            auto sim2 = std::make_unique<Sim>(1);
            bool same = state_hash(*snap) == state_hash(*s);
            for (u32 i = 0; i < 200 && same; ++i) {
                const TickInput in = bot_input(lv, s->tick);
                sim->tick(*s, in, d1);
                sim2->tick(*snap, in, d2);
                same = d1 == d2 && state_hash(*s) == state_hash(*snap);
            }
            c.check(same, m.id + ": snapshot taken mid-cascade continues bit-identically for 200 ticks");
            snapshotDone = true;
        }
    }
    std::printf("selftest: %d passed, %d failed\n", c.passed, c.failed);
    return c.failed == 0 ? 0 : 1;
}

// ---------------------------------------------------------------- sim
int mode_sim(const Args& a) {
    const std::string path = a.inPath("level");
    if (path.empty()) return fail("--level=<file> is required");
    Level lv;
    std::string err;
    if (!load_level(path, lv, err)) return fail(err);
    const Preset* preset = find_preset(a.get("preset", "research_mid"));
    if (preset == nullptr) return fail("unknown --preset");
    const u32 repeats = a.getU("repeats", 1);
    std::vector<Descriptor> trace;
    TraceResult first;
    bool allSame = true;
    for (u32 r = 0; r < repeats; ++r) {
        const TraceResult res = run_level(lv, preset->collisionPasses, r == 0 ? &trace : nullptr);
        std::printf("run %u: trace_sha256=%s ticks=%u detonations=%u bursts=%s\n", r + 1, res.sha256.c_str(), res.ticks,
                    res.detonations, bursts_text(res.bursts).c_str());
        if (r == 0) first = res;
        allSame = allSame && res.sha256 == first.sha256;
    }
    const bool matches = lv.expectedTraceSha256.empty() || first.sha256 == lv.expectedTraceSha256;
    std::printf("level %s: %s; %s\n", lv.id.c_str(), allSame ? "identical across runs" : "RUNS DIFFER",
                lv.expectedTraceSha256.empty() ? "no expected hash" : (matches ? "matches expected" : "DOES NOT MATCH EXPECTED"));

    const std::string traceOut = a.outPath("trace-out");
    if (!traceOut.empty()) {
        CsvWriter w;
        Json meta = Json::object();
        meta.set("level", Json::str(lv.id));
        meta.set("preset", Json::str(preset->name));
        meta.set("trace_sha256", Json::str(first.sha256));
        if (!w.open(traceOut, "afterglow.trace/1", meta,
                    "tick,live_orbs,live_blasts,detonations,taps,live_arcs,chain_depth,sparks_spawn,live_sparks,smoke_on,"
                    "smoke_splats,smoke_cells,jacobi_iters,sdf_instances,bloom_mips,internal_w,internal_h")) {
            return fail("cannot write " + traceOut);
        }
        WorkTracker wt(*preset);
        for (const Descriptor& d : trace) {
            const GpuWork g = wt.step(d);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u", d.tick, d.liveOrbs,
                          d.liveBlasts, d.detonations, d.taps, d.liveArcs, d.chainDepth, g.sparksSpawn, g.liveSparks,
                          g.smokeOn, g.smokeSplats, g.smokeCells, g.jacobiIters, g.sdfInstances, g.bloomMips, g.internalW,
                          g.internalH);
            w.row(buf);
        }
        w.close();
    }
    return allSame && matches ? 0 : 1;
}

// ---------------------------------------------------------------- levelgen
int mode_levelgen(const Args& a) {
    if (a.has("explore")) {
        const std::string range = a.get("orbs", "40:400:20");
        unsigned lo = 40, hi = 400, step = 20;
        std::sscanf(range.c_str(), "%u:%u:%u", &lo, &hi, &step);
        explore_bursts(lo, hi, step == 0 ? 20 : step, a.getU("samples", 40), a.getU("seed", 1));
        return 0;
    }
    const std::string specPath = a.inPath("spec", "bench/levels/levelgen_spec.json");
    const std::string outDir = a.outPath("out", "bench/levels");
    std::string text, err;
    if (!read_text(specPath, text)) return fail("cannot read " + specPath);
    Json spec;
    if (!Json::parse(text, spec, err)) return fail(specPath + ": " + err);
    GenConfig cfg;
    std::vector<LevelSpec> specs;
    if (!parse_levelgen_spec(spec, cfg, specs, err)) return fail(err);
    if (!make_dirs(outDir)) return fail("cannot create " + outDir);

    Json entries = Json::array();
    int failed = 0;
    for (u32 i = 0; i < specs.size(); ++i) {
        Level lv;
        if (!generate_level(cfg, specs[i], i, lv, err)) {
            std::printf("FAIL  %s: %s\n", specs[i].id.c_str(), err.c_str());
            ++failed;
            continue;
        }
        const std::string file = lv.id + ".json";
        if (!write_text(outDir + "/" + file, level_to_json(lv).dump(1) + "\n")) return fail("cannot write " + file);
        std::printf("ok    %-28s orbs=%3zu attempt=%-3lld ticks=%4u det=%3u %s\n", lv.id.c_str(), lv.orbs.size(),
                    static_cast<long long>(lv.design.get("attempt")->asInt()), lv.expectedTicks, lv.expectedDetonations,
                    bursts_text(lv.expectedBursts).c_str());
        Json e = Json::object();
        e.set("id", Json::str(lv.id));
        e.set("file", Json::str(file));
        e.set("sha256", Json::str(sha256_file_hex(outDir + "/" + file)));
        e.set("kind", Json::str(lv.kind));
        e.set("trace_sha256", Json::str(lv.expectedTraceSha256));
        entries.push(std::move(e));
    }
    Json manifest = Json::object();
    manifest.set("schema", Json::str("afterglow.levelset/1"));
    manifest.set("spec_sha256", Json::str(sha256_file_hex(specPath)));
    manifest.set("levels", std::move(entries));
    if (!write_text(outDir + "/manifest.json", manifest.dump(1) + "\n")) return fail("cannot write manifest");
    std::printf("levelgen: %zu levels written, %d failed\n", specs.size() - static_cast<usize>(failed), failed);
    return failed == 0 ? 0 : 1;
}

// ---------------------------------------------------------------- forecast-bench
int mode_forecast_bench(const Args& a) {
    const std::string dir = a.inPath("levels", "bench/levels");
    std::vector<ManifestEntry> manifest;
    std::string err;
    if (!read_manifest(dir, manifest, err)) return fail(err);
    const u32 reps = a.getU("reps", 50);
    const std::vector<u32> horizons = {1, 3, 6, 12, 24, 48, 96};

    // States to forecast from: every 10th tick of every level that has a live cascade (the case
    // the forecaster must handle within budget, docs/03 §5).
    std::vector<State> states;
    for (const ManifestEntry& m : manifest) {
        Level lv;
        if (!load_level(dir + "/" + m.file, lv, err)) return fail(err);
        auto s = std::make_unique<State>();
        auto sim = std::make_unique<Sim>(1);
        init_state(lv, *s);
        Descriptor d;
        while (!finished(*s) && s->tick < lv.maxTicks) {
            sim->tick(*s, bot_input(lv, s->tick), d);
            if (d.liveBlasts > 0 && s->tick % 10 == 0) states.push_back(*s);
        }
    }
    if (states.empty()) return fail("no cascade states to benchmark");

    auto fc = std::make_unique<Forecaster>(1);
    std::vector<Descriptor> out(Forecaster::kMaxHorizon);
    CsvWriter w;
    const std::string csv = a.outPath("out");
    if (!csv.empty()) {
        Json meta = Json::object();
        meta.set("note", Json::str("development timing on the Mac; device costs come from E0"));
        if (!w.open(csv, "afterglow.forecastbench/1", meta, "horizon_ticks,samples,median_us,p95_us,max_us")) {
            return fail("cannot write " + csv);
        }
    }
    std::printf("forecast-bench: %zu cascade states x %u reps\n horizon  median_us  p95_us  max_us\n", states.size(), reps);
    for (u32 h : horizons) {
        std::vector<i64> ns;
        ns.reserve(states.size() * reps);
        for (u32 r = 0; r < reps; ++r) {
            for (const State& st : states) {
                const auto t0 = std::chrono::steady_clock::now();
                fc->forecast(st, h, nullptr, out.data());
                const auto t1 = std::chrono::steady_clock::now();
                ns.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            }
        }
        std::sort(ns.begin(), ns.end());
        const i64 med = ns[ns.size() / 2];
        const i64 p95 = ns[ns.size() * 95 / 100];
        const i64 mx = ns.back();
        std::printf(" %7u  %9.1f  %6.1f  %6.1f\n", h, static_cast<double>(med) / 1000.0, static_cast<double>(p95) / 1000.0,
                    static_cast<double>(mx) / 1000.0);
        if (!csv.empty()) {
            w.row(std::to_string(h) + "," + std::to_string(ns.size()) + "," + fmt_double(static_cast<double>(med) / 1000.0, 2) +
                  "," + fmt_double(static_cast<double>(p95) / 1000.0, 2) + "," + fmt_double(static_cast<double>(mx) / 1000.0, 2));
        }
    }
    return 0;
}

// ---------------------------------------------------------------- stepbench / smoke / e0 (headless)
/// Runs the in-app runner without a display, paced by a 60 Hz timer: for development and smoke tests
/// only. The Mac is not a valid timing platform unless declared idle.
int mode_app_headless(const Args& a) {
    App app;
    std::string err;
    if (!app.create(a, HostSurface{}, err)) return fail(err);
    std::printf("%s\n", app.message().c_str());
    const i64 period = 16666667;
    i64 next = gfx::now_ns();
    int r = 0;
    while ((r = app.frame(next, period)) == 0) {
        next += period;
        const i64 now = gfx::now_ns();
        if (next > now) std::this_thread::sleep_for(std::chrono::nanoseconds(next - now));
        else next = now;
    }
    std::printf("%s\n", app.message().c_str());
    return r == 1 ? 0 : 1;
}

// ---------------------------------------------------------------- schema
/// Prints the frames.csv and device.csv headers and the provenance key lists, so tools/analysis
/// can check that it uses the same ones.
int mode_schema() {
    std::string f, d, req, na, sb, sm;
    for (usize i = 0; i < kFrameColumns.size(); ++i) f += std::string(i ? "," : "") + kFrameColumns[i];
    for (usize i = 0; i < kDeviceColumns.size(); ++i) d += std::string(i ? "," : "") + kDeviceColumns[i];
    for (const auto& k : required_provenance_keys()) req += (req.empty() ? "" : ",") + k;
    for (const auto& k : headless_na_keys()) na += (na.empty() ? "" : ",") + k;
    for (const auto& k : nolevel_na_keys()) sb += (sb.empty() ? "" : ",") + k;
    for (const auto& k : nolevel_modes()) sm += (sm.empty() ? "" : ",") + k;
    std::printf("%s %s\n%s %s\nprovenance.required %s\nprovenance.headless_na %s\nprovenance.nolevel_na %s\n"
                "provenance.nolevel_modes %s\n",
                kFrameSchema, f.c_str(), kDeviceSchema, d.c_str(), req.c_str(), na.c_str(), sb.c_str(), sm.c_str());
    return 0;
}

// ---------------------------------------------------------------- info
int mode_info(const Args& a) {
    RunInfo ri;
    ri.mode = "info";
    ri.runId = "info";
    ri.args = a.raw;
    ri.deviceId = a.get("device-id");
    Json p = build_provenance(ri);
    finish_provenance(p, false);
    std::printf("%s\n", p.dump(2).c_str());
    return 0;
}

} // namespace

int aglab_main(int argc, const char* const* argv) {
    const Args a = parse_args(argc, argv);
    const std::string mode = a.get("mode");
    if (mode == "selftest") return mode_selftest(a);
    if (mode == "sim") return mode_sim(a);
    if (mode == "levelgen") return mode_levelgen(a);
    if (mode == "forecast-bench") return mode_forecast_bench(a);
    if (mode == "info") return mode_info(a);
    if (mode == "schema") return mode_schema();
    if (mode == "stepbench" || mode == "smoke" || mode == "e0" || mode == "idlegap") return mode_app_headless(a);
    std::fprintf(stderr,
                 "usage: aglab --mode=<selftest|sim|levelgen|forecast-bench|info|schema|stepbench|smoke|e0|idlegap> [options]\n"
                 "  selftest        [--levels=bench/levels]\n"
                 "  sim             --level=<file> [--repeats=N] [--preset=research_mid] [--trace-out=<csv>]\n"
                 "  levelgen        [--spec=bench/levels/levelgen_spec.json] [--out=bench/levels]\n"
                 "                  | --explore [--orbs=lo:hi:step] [--samples=N]\n"
                 "  forecast-bench  [--levels=bench/levels] [--reps=N] [--out=<csv>]\n"
                 "  info            [--device-id=<id>]\n"
                 "  stepbench|smoke|e0|idlegap  headless on this Mac (development only): see lab/src/app.h\n");
    return 2;
}

} // namespace ag::lab
