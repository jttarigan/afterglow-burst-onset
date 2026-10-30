#pragma once
// Run-directory writers (docs/03 §9): CSV with '#' metadata lines and a schema version,
// locale-independent number formatting, JSON files. Copied from NeonTide/lab/src/runio.h
// (commit 3dc7266), without the .ntd field dumps.
#include "ag/json.h"

#include <cstdio>
#include <string>

namespace ag::lab {

/// Locale-independent number formatting (SnakeBuild CSVs once broke on comma-decimal locales).
std::string fmt_double(double v, int decimals);
std::string hex32(u32 v);
std::string hex64(u64 v);

bool make_dirs(const std::string& path);
bool write_text(const std::string& path, const std::string& text);
bool read_text(const std::string& path, std::string& out);

class CsvWriter {
public:
    ~CsvWriter();
    bool open(const std::string& path, const std::string& schema, const Json& meta, const std::string& header);
    void row(const std::string& line);
    bool close();

private:
    std::FILE* f_ = nullptr;
};


} // namespace ag::lab
