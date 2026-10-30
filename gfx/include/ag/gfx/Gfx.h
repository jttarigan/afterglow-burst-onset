#pragma once
// Thin GPU layer (docs/03 §1, tech stack): compute dispatches, a cleared and presented frame, and
// GPU timing. All GPU work is issued from C++ through this interface, so every platform runs the
// identical sequence (R1). Modelled on NeonTide/gfx (commit 3dc7266) and Lightbound/src/gfx.
//
// A command list is one GPU command buffer, timed as a whole: the probe gets its own list at frame
// start (docs/03 §6), loads get theirs, and the frame's clear + present is the last (D30).
//
// Rules shared by every backend:
//  - a binding error, a missing kernel, or a failed command buffer is counted in errorCount() and
//    never silently worked around (R7);
//  - lists execute in submission order on one queue, strictly one after another: a list's GPU
//    work never overlaps the previous list's (D41), so a probe never shares the GPU with a load;
//  - GPU times are in ns; start/end are on the lab clock (lab/src/clock.h) where the backend can
//    correlate GPU and CPU time, otherwise only the duration is known.
#include "ag/types.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ag::gfx {

enum class Memory : u8 {
    DeviceLocal,   // GPU only
    HostShared,    // CPU-mapped, coherent
};

struct BufferHandle { u32 id = 0; bool valid() const { return id != 0; } };
struct PipelineHandle { u32 id = 0; bool valid() const { return id != 0; } };
using ListId = u64;

/// A buffer bound to a kernel parameter by its Slang name (KernelBindings.gen.h).
struct Bind {
    const char* name = "";
    BufferHandle buffer{};
};

enum class SurfaceKind : u8 { None, MetalLayer, AndroidWindow };

struct InitDesc {
    std::string kernelDir;           // build kernel dir, or the app bundle / assets copy
    SurfaceKind surface = SurfaceKind::None;
    void* nativeSurface = nullptr;   // CAMetalLayer* or ANativeWindow*
    u32 width = 0, height = 0;       // drawable size in pixels (fixed internal resolution: D7)
};

/// Facts for provenance (R7).
struct DeviceInfo {
    std::string api;
    std::string deviceName;
    std::string driverVersion;
    std::vector<std::pair<std::string, std::string>> props;
};

struct PipelineInfo {
    std::string kernel;
    std::string binaryPath;
    std::string binarySha256;
};

/// Timing of one submitted list. Fields are -1 when unknown.
struct ListTiming {
    bool done = false;
    bool error = false;
    i64 durationNs = -1;
    i64 startNs = -1;     // GPU start on the lab clock (-1 if the clocks cannot be correlated)
    i64 endNs = -1;
    i64 presentedNs = -1; // when the frame reached the display (lists that presented)
};

class Gfx {
public:
    virtual ~Gfx() = default;

    virtual bool init(const InitDesc& desc, std::string& err) = 0;
    virtual const DeviceInfo& deviceInfo() const = 0;
    /// Surface size changed (Android); the swapchain is rebuilt before the next present.
    virtual void resize(u32 width, u32 height) = 0;

    // ------------------------------------------------------------------ resources (setup time, R5)
    virtual BufferHandle createBuffer(u64 bytes, Memory memory, const char* name) = 0;
    /// CPU pointer of a HostShared buffer; nullptr otherwise.
    virtual void* map(BufferHandle b) = 0;
    virtual PipelineHandle createPipeline(const char* kernel, std::string& err) = 0;
    virtual const std::vector<PipelineInfo>& pipelines() const = 0;

    // ------------------------------------------------------------------ recording
    virtual void begin() = 0;
    virtual void dispatch(PipelineHandle p, const Bind* binds, u32 bindCount, const void* push, u32 pushBytes,
                          u32 groupsX) = 0;
    /// Acquires the next drawable, clears it to (r, g, b) and presents it when the list completes.
    /// Returns false if there is no surface or no drawable; the list still runs.
    virtual bool clearAndPresent(u8 r, u8 g, u8 b) = 0;
    virtual ListId submit() = 0;

    // ------------------------------------------------------------------ completion
    /// Non-blocking. Timing of a list that is no longer tracked comes back with error = true.
    virtual ListTiming timing(ListId id) = 0;
    virtual void wait(ListId id) = 0;
    virtual void waitIdle() = 0;

    // ------------------------------------------------------------------ audit
    virtual u64 errorCount() const = 0;
    virtual const std::string& firstError() const = 0;
};

/// Creates the compiled-in backend (Metal on Apple, Vulkan elsewhere).
std::unique_ptr<Gfx> create_gfx();

} // namespace ag::gfx
