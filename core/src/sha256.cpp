#include "ag/sha256.h"

#include <cstdio>
#include <cstring>

namespace ag {
namespace {

constexpr std::array<u32, 64> kK = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

constexpr u32 rotr(u32 x, u32 n) { return (x >> n) | (x << (32u - n)); }

} // namespace

Sha256::Sha256() {
    h_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
}

void Sha256::block(const u8* p) {
    std::array<u32, 64> w{};
    for (usize i = 0; i < 16; ++i) {
        w[i] = (u32{p[4 * i]} << 24u) | (u32{p[4 * i + 1]} << 16u) | (u32{p[4 * i + 2]} << 8u) | u32{p[4 * i + 3]};
    }
    for (usize i = 16; i < 64; ++i) {
        const u32 s0 = rotr(w[i - 15], 7u) ^ rotr(w[i - 15], 18u) ^ (w[i - 15] >> 3u);
        const u32 s1 = rotr(w[i - 2], 17u) ^ rotr(w[i - 2], 19u) ^ (w[i - 2] >> 10u);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (usize i = 0; i < 64; ++i) {
        const u32 s1 = rotr(e, 6u) ^ rotr(e, 11u) ^ rotr(e, 25u);
        const u32 ch = (e & f) ^ (~e & g);
        const u32 t1 = h + s1 + ch + kK[i] + w[i];
        const u32 s0 = rotr(a, 2u) ^ rotr(a, 13u) ^ rotr(a, 22u);
        const u32 maj = (a & b) ^ (a & c) ^ (b & c);
        const u32 t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha256::update(const void* data, usize len) {
    const u8* p = static_cast<const u8*>(data);
    totalLen_ += len;
    while (len > 0) {
        const usize take = (64 - bufLen_) < len ? (64 - bufLen_) : len;
        std::memcpy(buf_.data() + bufLen_, p, take);
        bufLen_ += take;
        p += take;
        len -= take;
        if (bufLen_ == 64) {
            block(buf_.data());
            bufLen_ = 0;
        }
    }
}

std::array<u8, 32> Sha256::finish() {
    const u64 bits = totalLen_ * 8u;
    const u8 pad = 0x80;
    update(&pad, 1);
    const u8 zero = 0;
    while (bufLen_ != 56) update(&zero, 1);
    u8 len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<u8>(bits >> (56 - 8 * i));
    update(len, 8);
    std::array<u8, 32> out{};
    for (usize i = 0; i < 8; ++i) {
        out[4 * i] = static_cast<u8>(h_[i] >> 24u);
        out[4 * i + 1] = static_cast<u8>(h_[i] >> 16u);
        out[4 * i + 2] = static_cast<u8>(h_[i] >> 8u);
        out[4 * i + 3] = static_cast<u8>(h_[i]);
    }
    return out;
}

std::array<u8, 32> Sha256::of(const void* data, usize len) {
    Sha256 s;
    s.update(data, len);
    return s.finish();
}

std::string Sha256::hex(const std::array<u8, 32>& digest) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (u8 b : digest) {
        out.push_back(kHex[b >> 4u]);
        out.push_back(kHex[b & 15u]);
    }
    return out;
}

std::string sha256_file_hex(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return {};
    Sha256 s;
    u8 buf[1 << 16];
    for (;;) {
        const usize n = std::fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;
        s.update(buf, n);
    }
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    if (!ok) return {};
    return Sha256::hex(s.finish());
}

} // namespace ag
