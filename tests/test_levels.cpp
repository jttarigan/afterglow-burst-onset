// The research level set (bench/levels): determinism, expected hashes, design targets (M0 acceptance).
#include "ag/json.h"
#include "ag/level.h"
#include "ag/sha256.h"

#include "doctest/doctest.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace ag;

namespace {

std::string read_file(const std::string& path) {
    std::string out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return out;
    char buf[1 << 14];
    for (usize n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) out.append(buf, n);
    std::fclose(f);
    return out;
}

struct Entry {
    std::string file, sha256;
};

std::vector<Entry> manifest() {
    const std::string dir = std::string(AG_SOURCE_DIR) + "/bench/levels/";
    Json m;
    std::string err;
    REQUIRE(Json::parse(read_file(dir + "manifest.json"), m, err));
    std::vector<Entry> out;
    for (const Json& e : m.get("levels")->items()) out.push_back({dir + e.get("file")->asString(), e.get("sha256")->asString()});
    return out;
}

} // namespace

TEST_CASE("the research set has 22 levels: 18 single-burst and 4 repeated-burst") {
    const auto m = manifest();
    CHECK(m.size() == 22);
    int single = 0, repeat = 0;
    for (const Entry& e : m) {
        Level lv;
        std::string err;
        REQUIRE_MESSAGE(load_level(e.file, lv, err), err);
        single += lv.kind == "research_single";
        repeat += lv.kind == "research_repeat";
    }
    CHECK(single == 18);
    CHECK(repeat == 4);
}

TEST_CASE("every level: file hash, identical trace over 3 runs, expected hash, targets met") {
    for (const Entry& e : manifest()) {
        CAPTURE(e.file);
        CHECK(sha256_file_hex(e.file) == e.sha256);
        Level lv;
        std::string err;
        REQUIRE(load_level(e.file, lv, err));
        const TraceResult r1 = run_level(lv, 1);
        const TraceResult r2 = run_level(lv, 2);
        const TraceResult r3 = run_level(lv, 4);
        CHECK(r1.sha256 == lv.expectedTraceSha256);
        CHECK(r2.sha256 == r1.sha256);
        CHECK(r3.sha256 == r1.sha256);
        CHECK(r1.finished);
        CHECK(r1.detonations == lv.target);

        // Design targets: each onset exactly at its calm length (D22), each peak in its band (D23).
        const Json* bursts = lv.design.get("bursts");
        REQUIRE(bursts != nullptr);
        REQUIRE(bursts->items().size() == r1.bursts.size());
        u32 prevEnd = 0;
        for (usize k = 0; k < r1.bursts.size(); ++k) {
            const Json& bj = bursts->items()[k];
            const BurstStats& b = r1.bursts[k];
            CHECK(b.onset == prevEnd + static_cast<u32>(bj.get("calm_ticks")->asInt()));
            const Json* band = bj.get("band_peak_live_blasts");
            CHECK(b.peakLiveBlasts >= static_cast<u32>(band->items()[0].asInt()));
            CHECK(b.peakLiveBlasts <= static_cast<u32>(band->items()[1].asInt()));
            prevEnd = b.end;
        }
    }
}

TEST_CASE("level JSON round trip is lossless") {
    const auto m = manifest();
    Level lv;
    std::string err;
    REQUIRE(load_level(m.front().file, lv, err));
    Level back;
    REQUIRE(level_from_json(level_to_json(lv), back, err));
    CHECK(level_to_json(back).dump(1) == level_to_json(lv).dump(1));
    CHECK(run_level(back, 1).sha256 == lv.expectedTraceSha256);
}
