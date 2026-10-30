#pragma once
// run.json: the provenance block (R7, docs/03 §9) and build-level validity. Adapted from
// NeonTide/lab/src/provenance.h (commit 3dc7266). tools/analysis/validate.py applies the full
// run-validity rules (docs/04 §2) to finished runs; this file catches what is known at run time.
#include "ag/json.h"

#include <string>
#include <vector>

namespace ag::lab {

/// What the caller knows about the run; everything else comes from the device and the build.
struct RunInfo {
    std::string mode;            // selftest, sim, levelgen, forecast-bench, stepbench, research, replay
    std::string runId;           // <device>_<exp>_<cell>_<rep>_<utc> (docs/03 §12)
    std::string deviceId;        // ippro3, ip12, ipmini6, poco, tab, mac, ks, a56
    std::vector<std::string> args;
    Json workload = Json::object();   // preset, policy, level, seed, expected/actual trace hash
    Json input = Json::object();      // path: synthetic, adb_input, sendevent, xcuitest, human
    Json osGame = Json::object();     // app_category, game_mode, vendor_game_service
    Json requested = Json::object();  // conditions the protocol asked for (refresh, brightness, ...)
};

/// Builds run.json at run start. Call finish_provenance() at the end of the run.
Json build_provenance(const RunInfo& info);
/// Adds the end time, state_end and status (done, interrupted, invalid_reasons).
void finish_provenance(Json& prov, bool interrupted);

/// Every required key, as a dotted path. A value must be a non-empty string, a number or a bool.
/// "n/a: <reason>" is accepted only for keys the platform (or the mode) cannot report.
std::vector<std::string> missing_provenance(const Json& prov);

/// Reasons a run is invalid that are known at run time (R7): dirty tree, Debug build, FMA canary,
/// fast-math, incomplete provenance. Empty means no build-level reason.
std::vector<std::string> build_invalid_reasons(const Json& prov);

/// The required keys and the keys a headless macOS run may report as "n/a: ..." (for
/// `aglab --mode=schema`, which tools/analysis checks against its own copy).
std::vector<std::string> required_provenance_keys();
std::vector<std::string> headless_na_keys();
std::vector<std::string> nolevel_na_keys();
std::vector<std::string> nolevel_modes();

/// Dotted-path lookup ("state_start.thermal_state"); nullptr if missing.
const Json* at_path(const Json& root, const std::string& path);

/// ISO 8601 UTC timestamp (metadata only, never on the simulation path).
std::string iso_utc_now();

} // namespace ag::lab
