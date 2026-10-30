#pragma once
// SHA-256 (FIPS 180-4) for workload-trace hashes and the level manifest (R1, R7).
// Copied from NeonTide/core/include/nt/sha256.h (commit 3dc7266).
#include "ag/types.h"

#include <array>
#include <string>
#include <string_view>

namespace ag {

class Sha256 {
public:
    Sha256();
    void update(const void* data, usize len);
    std::array<u8, 32> finish();

    static std::array<u8, 32> of(const void* data, usize len);
    static std::string hex(const std::array<u8, 32>& digest);

private:
    void block(const u8* p);
    std::array<u32, 8> h_{};
    std::array<u8, 64> buf_{};
    usize bufLen_ = 0;
    u64 totalLen_ = 0;
};

/// SHA-256 of a file's bytes as lowercase hex; empty string if the file cannot be read.
std::string sha256_file_hex(const std::string& path);

} // namespace ag
