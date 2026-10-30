#include "runio.h"

#include <charconv>
#include <filesystem>

namespace ag::lab {

std::string fmt_double(double v, int decimals) {
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, decimals);
    return std::string(buf, r.ptr);
}

std::string hex32(u32 v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", v);
    return buf;
}

std::string hex64(u64 v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

bool make_dirs(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec && std::filesystem::is_directory(path);
}

bool write_text(const std::string& path, const std::string& text) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

bool read_text(const std::string& path, std::string& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    char buf[1 << 14];
    for (;;) {
        const usize n = std::fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;
        out.append(buf, n);
    }
    std::fclose(f);
    return true;
}

CsvWriter::~CsvWriter() { close(); }

bool CsvWriter::open(const std::string& path, const std::string& schema, const Json& meta, const std::string& header) {
    f_ = std::fopen(path.c_str(), "wb");
    if (f_ == nullptr) return false;
    std::fprintf(f_, "# schema=%s\n", schema.c_str());
    for (const auto& kv : meta.members()) {
        const std::string v = kv.second.isString() ? kv.second.asString() : kv.second.dump(-1);
        std::fprintf(f_, "# %s=%s\n", kv.first.c_str(), v.c_str());
    }
    std::fprintf(f_, "%s\n", header.c_str());
    return true;
}

void CsvWriter::row(const std::string& line) {
    if (f_ == nullptr) return;
    std::fwrite(line.data(), 1, line.size(), f_);
    std::fputc('\n', f_);
}

bool CsvWriter::close() {
    if (f_ == nullptr) return true;
    const bool ok = std::fclose(f_) == 0;
    f_ = nullptr;
    return ok;
}


} // namespace ag::lab
