#pragma once
// aglab command-line arguments: --key=value pairs (the same flags on desktop and as launch
// arguments in the mobile apps, docs/03 §10). Copied from NeonTide/lab/src/args.h (commit 3dc7266).
#include "ag/types.h"

#include <map>
#include <string>
#include <vector>

namespace ag::lab {

struct Args {
    std::map<std::string, std::string> kv;
    std::vector<std::string> raw;
    bool has(const std::string& k) const { return kv.count(k) != 0; }
    std::string get(const std::string& k, const std::string& def = "") const {
        auto it = kv.find(k);
        return it == kv.end() ? def : it->second;
    }
    u32 getU(const std::string& k, u32 def) const {
        auto it = kv.find(k);
        if (it == kv.end()) return def;
        return static_cast<u32>(std::stoul(it->second));
    }
    /// Input path: relative paths resolve against --root-in (the app bundle on mobile).
    std::string inPath(const std::string& k, const std::string& def = "") const {
        return resolve(get(k, def), get("root-in"));
    }
    /// Output path: relative paths resolve against --root-out (Documents / external files dir).
    std::string outPath(const std::string& k, const std::string& def = "") const {
        return resolve(get(k, def), get("root-out"));
    }
    static std::string resolve(const std::string& p, const std::string& root) {
        if (p.empty() || root.empty() || p[0] == '/') return p;
        return root + "/" + p;
    }
};

inline Args parse_args(int argc, const char* const* argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        a.raw.push_back(s);
        if (s.rfind("--", 0) != 0) continue;
        const auto eq = s.find('=');
        if (eq == std::string::npos) a.kv[s.substr(2)] = "1";
        else a.kv[s.substr(2, eq - 2)] = s.substr(eq + 1);
    }
    return a;
}

} // namespace ag::lab
