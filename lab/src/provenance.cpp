#include "provenance.h"

#include "ag/BuildFlags.gen.h"
#include "ag/GitInfo.h"
#include "ag/build_info.h"
#include "sysinfo.h"

#include <ctime>
#include <iterator>

namespace ag::lab {
namespace {

std::string g_platformState;   // set by the mobile host (M1)

bool is_na(const Json& v) { return v.isString() && v.asString().rfind("n/a:", 0) == 0; }

// R7: device, SoC, OS build, display refresh, power source, battery level, thermal state at start
// and end, Low Power Mode, brightness, git SHA and dirty flag, build type, preset, policy, level,
// seed. Plus the S2/S9 additions (input path, OS game features).
const char* const kRequired[] = {
    "schema",
    "run.mode",
    "run.run_id",
    "run.started_utc",
    "run.ended_utc",
    "device.id",
    "device.platform",
    "device.model",
    "device.soc",
    "device.os",
    "device.os_build",
    "build.git_commit",
    "build.git_dirty",
    "build.build_type",
    "build.compiler",
    "build.cxx_flags",
    "build.fma_canary_contracted",
    "build.fast_math",
    "state_start.thermal_state",
    "state_start.battery_percent",
    "state_start.charging",
    "state_start.power_source",
    "state_start.low_power_mode",
    "state_start.display_refresh_hz",
    "state_start.brightness",
    "state_end.thermal_state",
    "state_end.battery_percent",
    "state_end.charging",
    "state_end.low_power_mode",
    "workload.preset",
    "workload.policy",
    "workload.level",
    "workload.seed",
    "input.path",
    "os_game.app_category",
    "os_game.game_mode",
    "status.done",
    "status.interrupted",
};

// Keys the headless macOS runner cannot report. On a device (M1) only the platform's own gaps
// are allowed; a game-mode run must report everything in workload, input and os_game.
const char* const kHeadlessNa[] = {
    "state_start.battery_percent", "state_start.charging", "state_start.display_refresh_hz",
    "state_start.brightness",      "state_end.battery_percent", "state_end.charging",
    "workload.preset",             "workload.policy",      "workload.level",
    "workload.seed",               "input.path",           "os_game.app_category",
    "os_game.game_mode",
};

// Keys a mode has no value for, on any platform: these modes run synthetic loads or self-checks,
// not a level (D27).
const char* const kNoLevelModes[] = {"stepbench", "smoke", "e0", "idlegap"};
const char* const kNoLevelNa[] = {"workload.preset", "workload.level"};

bool headless(const Json& prov) {
    const Json* p = at_path(prov, "device.platform");
    return p != nullptr && p->asString() == "macOS";
}

void fill_na(Json& obj, const char* key, const char* reason) {
    if (obj.get(key) == nullptr) obj.set(key, Json::str(std::string("n/a: ") + reason));
}

} // namespace

std::vector<std::string> required_provenance_keys() { return {std::begin(kRequired), std::end(kRequired)}; }
std::vector<std::string> headless_na_keys() { return {std::begin(kHeadlessNa), std::end(kHeadlessNa)}; }
std::vector<std::string> nolevel_na_keys() { return {std::begin(kNoLevelNa), std::end(kNoLevelNa)}; }
std::vector<std::string> nolevel_modes() { return {std::begin(kNoLevelModes), std::end(kNoLevelModes)}; }

std::string platform_state_json() { return g_platformState; }
void set_platform_state_json(std::string json) { g_platformState = std::move(json); }

const Json* at_path(const Json& root, const std::string& path) {
    const Json* cur = &root;
    usize start = 0;
    while (cur != nullptr) {
        const usize dot = path.find('.', start);
        const std::string key = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        cur = cur->get(key);
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return cur;
}

std::string iso_utc_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

Json build_provenance(const RunInfo& info) {
    Json p = Json::object();
    p.set("schema", Json::str("afterglow.run/1"));

    Json run = Json::object();
    run.set("mode", Json::str(info.mode));
    run.set("run_id", Json::str(info.runId));
    run.set("started_utc", Json::str(iso_utc_now()));
    Json a = Json::array();
    for (const auto& s : info.args) a.push(Json::str(s));
    run.set("args", std::move(a));
    p.set("run", std::move(run));

    Json device = device_facts();
    device.set("id", Json::str(info.deviceId.empty() ? std::string("n/a: no --device-id given") : info.deviceId));
    p.set("device", std::move(device));

    const CoreBuildInfo cb = core_build_info();
    Json b = Json::object();
    b.set("git_commit", Json::str(AG_GIT_COMMIT));
    b.set("git_dirty", Json::boolean(AG_GIT_DIRTY != 0));
    b.set("build_type", Json::str(AG_BUILD_TYPE));
    b.set("compiler", Json::str(cb.compiler));
    b.set("cxx_flags", Json::str(AG_CXX_FLAGS));
    b.set("fma_canary_contracted", Json::boolean(cb.fmaContracted));
    b.set("fast_math", Json::boolean(cb.fastMath));
    b.set("optimized", Json::boolean(cb.optimized));
    b.set("ndebug", Json::boolean(cb.ndebug));
    p.set("build", std::move(b));

    p.set("state_start", device_state());
    p.set("requested", info.requested);

    Json w = info.workload;
    Json in = info.input;
    Json og = info.osGame;
    if (p.get("device")->get("platform")->asString() == "macOS") {
        for (const char* k : {"preset", "policy", "level", "seed"}) fill_na(w, k, "headless lab mode without a level run");
        fill_na(in, "path", "headless lab runner, no input");
        fill_na(og, "app_category", "headless lab runner");
        fill_na(og, "game_mode", "headless lab runner");
    }
    p.set("workload", std::move(w));
    p.set("input", std::move(in));
    p.set("os_game", std::move(og));
    return p;
}

void finish_provenance(Json& prov, bool interrupted) {
    Json* run = prov.getMutable("run");
    if (run != nullptr) run->set("ended_utc", Json::str(iso_utc_now()));
    prov.set("state_end", device_state());
    Json st = Json::object();
    st.set("done", Json::boolean(true));
    st.set("interrupted", Json::boolean(interrupted));
    prov.set("status", std::move(st));
    Json reasons = Json::array();
    for (const auto& r : build_invalid_reasons(prov)) reasons.push(Json::str(r));
    prov.getMutable("status")->set("invalid_reasons", std::move(reasons));
}

std::vector<std::string> missing_provenance(const Json& prov) {
    std::vector<std::string> missing;
    const bool hl = headless(prov);
    for (const char* key : kRequired) {
        const Json* v = at_path(prov, key);
        bool ok = v != nullptr;
        if (ok) {
            switch (v->type()) {
                case Json::Type::String: ok = !v->asString().empty(); break;
                case Json::Type::Bool:
                case Json::Type::Int:
                case Json::Type::Raw: break;
                default: ok = false;
            }
        }
        if (ok && is_na(*v)) {
            bool allowed = false;
            if (hl) {
                for (const char* na : kHeadlessNa) allowed = allowed || std::string(na) == key;
            }
            // iOS has no public API that reports whether Game Mode is active.
            const Json* plat = at_path(prov, "device.platform");
            if (plat != nullptr && plat->asString() == "iOS" && std::string(key) == "os_game.game_mode") allowed = true;
            // A device id is only optional for local headless runs.
            if (hl && std::string(key) == "device.id") allowed = true;
            const Json* mode = at_path(prov, "run.mode");
            bool noLevel = false;
            for (const char* m : kNoLevelModes) noLevel = noLevel || (mode != nullptr && mode->asString() == m);
            if (noLevel) {
                for (const char* na : kNoLevelNa) allowed = allowed || std::string(na) == key;
            }
            ok = allowed;
        }
        if (!ok) missing.emplace_back(key);
    }
    return missing;
}

std::vector<std::string> build_invalid_reasons(const Json& prov) {
    std::vector<std::string> r;
    const Json* dirty = at_path(prov, "build.git_dirty");
    const Json* commit = at_path(prov, "build.git_commit");
    if (dirty == nullptr || dirty->asBool(true)) r.emplace_back("built from a dirty git tree");
    if (commit == nullptr || commit->asString() == "nogit") r.emplace_back("no git commit");
    const Json* bt = at_path(prov, "build.build_type");
    const Json* opt = at_path(prov, "build.optimized");
    const Json* nd = at_path(prov, "build.ndebug");
    if (bt == nullptr || bt->asString() != "Release" || opt == nullptr || !opt->asBool() || nd == nullptr ||
        !nd->asBool()) {
        r.emplace_back("not a Release build");
    }
    const Json* fma = at_path(prov, "build.fma_canary_contracted");
    if (fma == nullptr || fma->asBool(true)) r.emplace_back("FMA canary: CPU core contracts a*b+c");
    const Json* fm = at_path(prov, "build.fast_math");
    if (fm == nullptr || fm->asBool(true)) r.emplace_back("core compiled with fast-math");
    const auto missing = missing_provenance(prov);
    if (!missing.empty()) {
        std::string s = "provenance incomplete:";
        for (const auto& m : missing) s += " " + m;
        r.push_back(s);
    }
    return r;
}

} // namespace ag::lab
