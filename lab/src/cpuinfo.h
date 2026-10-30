#pragma once
// Which CPU the render thread is on, and that core's current frequency where the OS lets an app
// read it (docs/03 §6: core migration is part of power management; the frequency is a cross-check
// only, never the measure). Android only; elsewhere both are unknown (-1).
#include "ag/types.h"

#if defined(__ANDROID__)
#include <fcntl.h>
#include <sched.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#endif

namespace ag::lab {

class CpuInfo {
public:
    ~CpuInfo() {
#if defined(__ANDROID__)
        for (int& fd : fds_) {
            if (fd >= 0) close(fd);
        }
#endif
    }

    /// The CPU this thread runs on now, or -1.
    static i64 cpu() {
#if defined(__ANDROID__)
        return sched_getcpu();
#else
        return -1;
#endif
    }

    /// scaling_cur_freq of 'cpu' in kHz, or -1 if unreadable (SELinux may deny it).
    i64 freqKhz(i64 cpu) {
#if defined(__ANDROID__)
        if (cpu < 0 || cpu >= kMaxCpus) return -1;
        int& fd = fds_[cpu];
        if (fd == -2) return -1;   // tried before and denied
        if (fd < 0) {
            char path[96];
            std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%lld/cpufreq/scaling_cur_freq",
                          static_cast<long long>(cpu));
            fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                fd = -2;
                return -1;
            }
        }
        char buf[32] = {};
        const ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);
        return n > 0 ? std::strtoll(buf, nullptr, 10) : -1;
#else
        (void)cpu;
        return -1;
#endif
    }

private:
    static constexpr i64 kMaxCpus = 16;
#if defined(__ANDROID__)
    int fds_[kMaxCpus] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
#endif
};

} // namespace ag::lab
