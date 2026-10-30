#pragma once
// The in-app runner (docs/03 §2, §9, §10): one App per launch, driven by the platform's display
// link. Modes:
//   stepbench  E1 step loads (docs/04 E1);
//   smoke      a 30 s stepbench whose output schema is validated (docs/03 §13);
//   e0         device self-checks (docs/04 E0): level determinism, forecaster cost, probe
//              linearity, warm CV, probe result check;
//   idlegap    the GPU idle-gap ladder (docs/04 E1, S6): sleep g with no GPU work, submit one probe,
//              block until it completes; submit-to-start and round trip versus g.
// Frame structure (docs/03 §2): CPU probe -> CPU work -> encode GPU probe (own list) -> load
// list -> clear + present list -> frame-log row when the GPU results are back.
#include "ag/gfx/Gfx.h"
#include "args.h"
#include "framelog.h"
#include "cpuinfo.h"
#include "stepbench.h"

#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace ag::lab {

struct HostSurface {
    gfx::SurfaceKind kind = gfx::SurfaceKind::None;
    void* ptr = nullptr;
    u32 width = 0, height = 0;
};

class App {
public:
    App();
    ~App();
    bool create(const Args& a, const HostSurface& surface, std::string& err);
    /// One display-link tick. targetPresentNs: the display's target time for this frame on the lab
    /// clock (0 if unknown); periodNs: the refresh period. Returns 0 to continue, 1 when the run is
    /// finished and its files are written, -1 on a fatal error.
    int frame(i64 targetPresentNs, i64 periodNs);
    /// Host-side state as JSON {"device": {...}, "state": {...}} (battery, brightness, refresh rate,
    /// thermal status on Android). Also samples device.csv (at most once a second).
    void platformState(const std::string& json);
    /// "background", "interrupt": the run is ended and marked interrupted (docs/04 §2 rule 2).
    void event(const std::string& name);
    int finish(bool interrupted, const std::string& why);
    const std::string& message() const { return message_; }
    const std::string& runDir() const { return runDir_; }
    bool finished() const { return finished_; }

private:
    struct Pending {
        FrameRow row;
        gfx::ListId probe = 0, load = 0, present = 0, probeExtra = 0, probeWide = 0;
        u32 gpuLoadIters = 0;
        u32 probeMult = 0;
        i64 cpuProbeExtraNs = -1;
        bool calibrate = false;
    };
    struct E0State;

    void sampleDevice(i64 now);
    void drain(bool all);
    void complete(Pending& p);
    u32 gpuItersFor(u32 pct) const;
    u32 cpuItersFor(u32 pct) const;
    int idleGapFrame();
    void runE0CpuChecks();
    void finishE0();
    Json buildRunJson(bool interrupted, const std::string& why);

    Args args_;
    std::string mode_;
    std::string runId_, runDir_, message_;
    std::unique_ptr<gfx::Gfx> gfx_;
    gfx::PipelineHandle probePipe_, loadPipe_;
    gfx::BufferHandle probeBuf_, probeExtraBuf_, loadBuf_;
    FrameLogWriter frames_;
    DeviceLogWriter device_;
    Json prov_;
    StepbenchConfig sbCfg_;
    u32 refreshHz_ = 60;          // --req-display_refresh_hz (D53/D60): sets the frame budget
    std::unique_ptr<StepbenchSchedule> sched_;
    std::unique_ptr<E0State> e0_;
    // Exploratory wide GPU probe (off by default; M1 probe-design question, STATUS): a second
    // probe dispatch over many groups, logged to probe_wide.csv. Never part of the standard probe.
    u32 wideGroups_ = 0, wideIters_ = 0;
    gfx::BufferHandle wideBuf_;
    CpuInfo cpuInfo_;
    CsvWriter wideCsv_;
    bool idleGap_ = false;
    std::vector<i64> gapsNs_;     // shuffled ladder (idlegap mode)
    usize gapIdx_ = 0;
    CsvWriter gapCsv_;
    Json gapCfg_ = Json::object();
    std::deque<Pending> pending_;
    i64 frame_ = 0;
    i64 lastDeviceSample_ = -1;
    i64 lastPresented_ = -1;
    i64 periodNs_ = 16666667;
    // Warm calibration (D31, D38): accepted samples (>= 0.5 ms of work) of ns per iteration; the
    // estimate is their 10th percentile, and loads never exceed 4x the largest measured size.
    struct Calib {
        std::vector<double> samples;
        double nsPerIter = 0.0;
        u32 probeIters = 0;      // next size to try while no sample is accepted
        u32 maxMeasured = 0;
        u32 probeCap = 0;        // growth stops here: real GPUs pass 0.5 ms far below it
        void add(i64 durationNs, u32 iters);
        u32 itersFor(u32 pct, i64 budgetNs, u32 initial) const;
    };
    Calib gpuCal_, cpuCal_;
    u64 rows_ = 0, missingGpu_ = 0, lateFrames_ = 0;
    u32 probeSeed_ = 0x5EEDu;
    i64 gateUntil_ = 0;           // thermal gate (docs/04 §1): wait for nominal before starting
    i64 gateStart_ = -1;
    bool gating_ = false;
    bool finished_ = false;
    bool created_ = false;
};

} // namespace ag::lab
