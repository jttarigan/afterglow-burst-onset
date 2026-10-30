// lab/: frame-log schema, CSV writing, provenance completeness and build-level validity (R7).
#include "framelog.h"
#include "provenance.h"
#include "runio.h"

#include "doctest/doctest.h"

#include <filesystem>
#include <string>

using namespace ag;
using namespace ag::lab;

namespace {

std::string tmp_dir() {
    const auto d = std::filesystem::temp_directory_path() / "afterglow_test_lab";
    std::filesystem::create_directories(d);
    return d.string();
}

} // namespace

TEST_CASE("frames.csv: schema line, metadata, the docs/03 §9 columns, empty fields for n/a") {
    const std::string path = tmp_dir() + "/frames.csv";
    FrameLogWriter w;
    Json meta = Json::object();
    meta.set("run_id", Json::str("test"));
    REQUIRE(w.open(path, meta));
    FrameRow r;
    r.at(static_cast<usize>(frame_column("frame"))) = 7;
    r.at(static_cast<usize>(frame_column("gpu_probe_ns"))) = 101234;
    w.write(r);
    REQUIRE(w.close());
    std::string text;
    REQUIRE(read_text(path, text));
    CHECK(text.rfind("# schema=afterglow.frames/2\n# run_id=test\nframe,tick,ticks_run,t_frame_start_ns,", 0) == 0);
    const usize nl = text.find('\n', text.find("frame,tick"));
    const std::string row = text.substr(nl + 1);
    CHECK(row.rfind("7,,,", 0) == 0);
    CHECK(row.find(",101234,") != std::string::npos);
    for (const char* col : {"cpu_probe_ns", "gpu_main_ns", "present_actual_ns", "live_sparks", "policy_state",
                            "heater_gpu_us", "hint_calls", "cpu_id", "sb_trial", "sb_calm_ms"}) {
        CHECK(frame_column(col) >= 0);
    }
    CHECK(frame_column("nope") == -1);
}

TEST_CASE("device.csv: thermal state as text, numbers as integers") {
    const std::string path = tmp_dir() + "/device.csv";
    DeviceLogWriter w;
    REQUIRE(w.open(path, Json::object()));
    DeviceRow r;
    r.tNs = 1000000000;
    r.thermalState = "nominal";
    r.batteryPct = 93;
    w.write(r);
    REQUIRE(w.close());
    std::string text;
    REQUIRE(read_text(path, text));
    CHECK(text.find("t_ns,thermal_state,battery_pct,charging,current_ua,voltage_mv,brightness_pct,low_power_mode\n"
                    "1000000000,nominal,93,,,,,\n") != std::string::npos);
}

TEST_CASE("provenance: a headless macOS run is complete; dropping a required key is caught") {
    RunInfo ri;
    ri.mode = "selftest";
    ri.runId = "mac_test";
    Json p = build_provenance(ri);
    finish_provenance(p, false);
    CHECK(missing_provenance(p).empty());
    CHECK(at_path(p, "device.soc") != nullptr);
    CHECK(at_path(p, "state_end.thermal_state") != nullptr);

    Json q = p;
    q.getMutable("build")->set("git_commit", Json::str(""));
    const auto miss = missing_provenance(q);
    REQUIRE(miss.size() == 1);
    CHECK(miss[0] == "build.git_commit");

    // n/a is only accepted where the platform cannot report the value.
    Json r = p;
    r.getMutable("build")->set("build_type", Json::str("n/a: unknown"));
    CHECK(missing_provenance(r).size() == 1);
}

TEST_CASE("build-level validity: dirty tree and Debug builds are invalid (R7)") {
    RunInfo ri;
    ri.mode = "selftest";
    ri.runId = "x";
    Json p = build_provenance(ri);
    finish_provenance(p, false);
    Json clean = p;
    clean.getMutable("build")->set("git_dirty", Json::boolean(false));
    clean.getMutable("build")->set("git_commit", Json::str("abc"));
    clean.getMutable("build")->set("build_type", Json::str("Release"));
    clean.getMutable("build")->set("optimized", Json::boolean(true));
    clean.getMutable("build")->set("ndebug", Json::boolean(true));
    CHECK(build_invalid_reasons(clean).empty());

    Json dirty = clean;
    dirty.getMutable("build")->set("git_dirty", Json::boolean(true));
    CHECK(build_invalid_reasons(dirty).size() == 1);

    Json debug = clean;
    debug.getMutable("build")->set("build_type", Json::str("Debug"));
    CHECK(build_invalid_reasons(debug) == std::vector<std::string>{"not a Release build"});
}

TEST_CASE("provenance on a device: level and preset may be n/a only in no-level modes (D27)") {
    RunInfo ri;
    ri.mode = "e0";
    ri.runId = "ip12_e0";
    ri.deviceId = "ip12";
    Json p = build_provenance(ri);
    finish_provenance(p, false);
    // Pretend this ran on an iPhone: no headless allowances, Game Mode n/a, everything else set.
    p.getMutable("device")->set("platform", Json::str("iOS"));
    for (const char* k : {"battery_percent", "charging", "display_refresh_hz", "brightness"}) {
        p.getMutable("state_start")->set(k, Json::num(1));
        p.getMutable("state_end")->set(k, Json::num(1));
    }
    p.getMutable("workload")->set("preset", Json::str("n/a: no level"));
    p.getMutable("workload")->set("level", Json::str("n/a: no level"));
    p.getMutable("workload")->set("policy", Json::str("P0"));
    p.getMutable("workload")->set("seed", Json::num(1));
    p.getMutable("input")->set("path", Json::str("none"));
    p.getMutable("os_game")->set("app_category", Json::str("game"));
    p.getMutable("os_game")->set("game_mode", Json::str("n/a: no public API"));
    for (const char* mode : {"stepbench", "smoke", "e0", "idlegap"}) {
        p.getMutable("run")->set("mode", Json::str(mode));
        CAPTURE(mode);
        CHECK(missing_provenance(p).empty());
    }
    p.getMutable("run")->set("mode", Json::str("research"));
    const auto miss = missing_provenance(p);
    CHECK(miss.size() == 2);   // preset and level: a research run must name both
}
