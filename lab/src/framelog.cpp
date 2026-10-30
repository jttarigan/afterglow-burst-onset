#include "framelog.h"

#include <charconv>
#include <cstring>

namespace ag::lab {
namespace {

std::string join(const char* const* cols, usize n) {
    std::string h;
    for (usize i = 0; i < n; ++i) {
        if (i > 0) h += ',';
        h += cols[i];
    }
    return h;
}

void append_i64(std::string& out, i64 v) {
    if (v == kNA) return;
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, r.ptr);
}

} // namespace

int frame_column(const char* name) {
    for (usize i = 0; i < kFrameColumns.size(); ++i) {
        if (std::strcmp(kFrameColumns[i], name) == 0) return static_cast<int>(i);
    }
    return -1;
}

void format_row(const i64* values, usize n, std::string& out) {
    out.clear();
    for (usize i = 0; i < n; ++i) {
        if (i > 0) out += ',';
        append_i64(out, values[i]);
    }
}

bool FrameLogWriter::open(const std::string& path, const Json& meta) {
    line_.reserve(512);
    return csv_.open(path, kFrameSchema, meta, join(kFrameColumns.data(), kFrameColumns.size()));
}

void FrameLogWriter::write(const FrameRow& r) {
    format_row(r.v.data(), r.v.size(), line_);
    csv_.row(line_);
}

bool DeviceLogWriter::open(const std::string& path, const Json& meta) {
    line_.reserve(128);
    return csv_.open(path, kDeviceSchema, meta, join(kDeviceColumns.data(), kDeviceColumns.size()));
}

void DeviceLogWriter::write(const DeviceRow& r) {
    line_.clear();
    append_i64(line_, r.tNs);
    line_ += ',';
    line_ += r.thermalState;
    for (i64 v : {r.batteryPct, r.charging, r.currentUa, r.voltageMv, r.brightnessPct, r.lowPowerMode}) {
        line_ += ',';
        append_i64(line_, v);
    }
    csv_.row(line_);
}

} // namespace ag::lab
