// Utilities copied from NeonTide: PCG32, SHA-256, JSON, fixed-point helpers, the FMA canary.
#include "ag/build_info.h"
#include "ag/fixed.h"
#include "ag/hash.h"
#include "ag/json.h"
#include "ag/pcg32.h"
#include "ag/sha256.h"
#include "ag/sim.h"

#include "doctest/doctest.h"

#include <cstring>

using namespace ag;

TEST_CASE("PCG32 matches the reference pcg32_random_r (seed 42, stream 54)") {
    Pcg32 rng(42u, 54u);
    const u32 expected[] = {0xa15c02b7u, 0x7b47f409u, 0xba1d3330u, 0x83d2f293u, 0xbfa4784bu, 0xcbed606eu};
    for (u32 e : expected) CHECK(rng.next() == e);
}

TEST_CASE("PCG32 below() stays in range and is deterministic") {
    Pcg32 a(7u, 3u), b(7u, 3u);
    for (int i = 0; i < 1000; ++i) {
        const u32 x = a.below(37u);
        CHECK(x < 37u);
        CHECK(x == b.below(37u));
    }
}

TEST_CASE("SHA-256 test vectors (FIPS 180-4)") {
    CHECK(Sha256::hex(Sha256::of("abc", 3)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256::hex(Sha256::of("", 0)) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(Sha256::hex(Sha256::of(m, std::strlen(m))) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("Hash64 is order-independent and sensitive to values") {
    Hash64 a, b, c;
    a.add(1, 0, 0);
    a.add(2, 1, 0);
    b.add(2, 1, 0);
    b.add(1, 0, 0);
    c.add(2, 0, 0);
    c.add(1, 1, 0);
    CHECK(a.value() == b.value());
    CHECK(a.value() != c.value());
}

TEST_CASE("JSON round trip keeps integers, strings and order") {
    Json j = Json::object();
    j.set("b", Json::num(-123456789012ll)).set("a", Json::str("x\"y")).set("arr", Json::array());
    j.getMutable("arr")->push(Json::num(1)).push(Json::boolean(true)).push(Json::null());
    Json back;
    std::string err;
    REQUIRE(Json::parse(j.dump(2), back, err));
    CHECK(back.get("b")->asInt() == -123456789012ll);
    CHECK(back.get("a")->asString() == "x\"y");
    CHECK(back.members()[0].first == "b");
    CHECK(back.get("arr")->items().size() == 3);
    CHECK(back.dump(-1) == j.dump(-1));
}

TEST_CASE("isqrt64 is the floor square root") {
    for (u64 x : {0ull, 1ull, 2ull, 3ull, 4ull, 15ull, 16ull, 17ull, 1000000ull, 999999999999ull}) {
        const u64 r = isqrt64(x);
        CHECK(r * r <= x);
        CHECK((r + 1) * (r + 1) > x);
    }
}

TEST_CASE("orb_velocity gives the requested speed within one Q8 unit per axis") {
    for (i32 speed : {kMinSpeed, 75, kMaxSpeed}) {
        for (auto [dx, dy] : {std::pair{1000, 0}, std::pair{0, -700}, std::pair{300, 400}, std::pair{-512, 512}}) {
            i32 vx = 0, vy = 0;
            orb_velocity(speed, dx, dy, vx, vy);
            const i64 want = (i64{speed} << kQ) / kTickHz;   // Q8 per tick
            const i64 got = static_cast<i64>(isqrt64(static_cast<u64>(i64{vx} * vx + i64{vy} * vy)));
            CHECK(got <= want);
            CHECK(got >= want - 2);
        }
    }
}

TEST_CASE("FMA canary: this build does not contract a*b+c") {
    const CoreBuildInfo bi = core_build_info();
    CHECK_FALSE(bi.fmaContracted);
    CHECK_FALSE(bi.fastMath);
}
