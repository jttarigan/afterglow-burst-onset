// Metal backend of ag::gfx (metal-cpp). Adapted from NeonTide/gfx/metal/MetalGfx.cpp (commit 3dc7266):
// device setup, library loading and binding by name through the generated table. Added: per-list
// timing from the command buffer's GPUStartTime/GPUEndTime (host time, the lab clock), and a
// cleared CAMetalLayer drawable with its presentedTime.
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <objc/runtime.h>

#include "ag/KernelBindings.gen.h"
#include "ag/gfx/Clock.h"
#include "ag/gfx/Gfx.h"
#include "ag/sha256.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>

namespace ag::gfx {
namespace {

std::string ns_str(const NS::String* s) { return s != nullptr ? std::string(s->utf8String()) : std::string(); }

const gen::KernelInfo* find_kernel(const char* name) {
    for (const auto& k : gen::kKernels) {
        if (std::strcmp(k.name, name) == 0) return &k;
    }
    return nullptr;
}

/// Written by Metal's completion and presented handlers (other threads), read by timing().
struct ListState {
    std::atomic<bool> done{false};
    std::atomic<bool> presentDone{false};
    bool presents = false;
    bool error = false;
    bool errorCounted = false;   // touched only by the thread that calls timing()
    double gpuStart = 0.0, gpuEnd = 0.0, presented = 0.0;
};

class MetalGfx final : public Gfx {
public:
    ~MetalGfx() override {
        waitIdle();
        for (auto& l : lists_) {
            if (l.cb != nullptr) l.cb->release();
        }
        for (auto* b : buffers_) b->release();
        for (auto& p : pipes_) p.pso->release();
        if (order_ != nullptr) order_->release();
        if (queue_ != nullptr) queue_->release();
        if (device_ != nullptr) device_->release();
    }

    bool init(const InitDesc& desc, std::string& err) override {
        kernelDir_ = desc.kernelDir;
        device_ = MTL::CreateSystemDefaultDevice();
        if (device_ == nullptr) {
            err = "no Metal device";
            return false;
        }
        queue_ = device_->newCommandQueue();
        // Lists run strictly one after another on the GPU (D41): Metal may otherwise overlap
        // command buffers that share no resource, and a probe overlapping a load measures
        // contention instead of the clock (E0 on the iPhone 12, 2026-10-06).
        order_ = device_->newEvent();
        info_.api = "Metal";
        info_.deviceName = ns_str(device_->name());
        info_.driverVersion = "n/a: the Metal driver ships with the OS (see device.os_build)";
        auto& p = info_.props;
        if (auto* arch = device_->architecture()) p.emplace_back("architecture", ns_str(arch->name()));
        std::string fams;
        for (int f = 1; f <= 10; ++f) {
            if (device_->supportsFamily(static_cast<MTL::GPUFamily>(1000 + f))) fams += "Apple" + std::to_string(f) + " ";
        }
        if (device_->supportsFamily(MTL::GPUFamilyMetal3)) fams += "Metal3 ";
        if (!fams.empty()) fams.pop_back();
        p.emplace_back("gpu_families", fams);
        p.emplace_back("unified_memory", device_->hasUnifiedMemory() ? "true" : "false");
        p.emplace_back("registry_id", std::to_string(device_->registryID()));
        p.emplace_back("gpu_time_domain", "host time (mach_absolute_time), correlated");

        if (desc.surface == SurfaceKind::MetalLayer && desc.nativeSurface != nullptr) {
            layer_ = static_cast<CA::MetalLayer*>(desc.nativeSurface);
            layer_->setDevice(device_);
            layer_->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
            layer_->setFramebufferOnly(true);
            layer_->setMaximumDrawableCount(3);
            if (desc.width > 0 && desc.height > 0) layer_->setDrawableSize(CGSizeMake(desc.width, desc.height));
            p.emplace_back("surface", "CAMetalLayer " + std::to_string(desc.width) + "x" + std::to_string(desc.height));
        } else if (desc.surface != SurfaceKind::None) {
            err = "the Metal backend needs a CAMetalLayer surface";
            return false;
        }
        return true;
    }

    const DeviceInfo& deviceInfo() const override { return info_; }

    void resize(u32 width, u32 height) override {
        if (layer_ != nullptr && width > 0 && height > 0) layer_->setDrawableSize(CGSizeMake(width, height));
    }

    BufferHandle createBuffer(u64 bytes, Memory memory, const char* name) override {
        const auto opts = memory == Memory::HostShared ? MTL::ResourceStorageModeShared : MTL::ResourceStorageModePrivate;
        MTL::Buffer* b = device_->newBuffer(bytes, opts);
        if (b == nullptr) {
            error(std::string("buffer allocation failed: ") + name);
            return {};
        }
        b->setLabel(NS::String::string(name, NS::UTF8StringEncoding));
        buffers_.push_back(b);
        return {static_cast<u32>(buffers_.size())};
    }

    void* map(BufferHandle b) override {
        if (!b.valid() || b.id > buffers_.size()) return nullptr;
        MTL::Buffer* buf = buffers_[b.id - 1];
        return buf->storageMode() == MTL::StorageModeShared ? buf->contents() : nullptr;
    }

    PipelineHandle createPipeline(const char* kernel, std::string& err) override {
        const gen::KernelInfo* ki = find_kernel(kernel);
        if (ki == nullptr) {
            err = std::string("kernel not in the binding table: ") + kernel;
            return {};
        }
        const std::string path = kernelDir_ + "/metal/" + kernel + ".metallib";
        NS::Error* nserr = nullptr;
        MTL::Library* lib = device_->newLibrary(NS::String::string(path.c_str(), NS::UTF8StringEncoding), &nserr);
        if (lib == nullptr) {
            err = "cannot load " + path + (nserr ? ": " + ns_str(nserr->localizedDescription()) : std::string());
            return {};
        }
        MTL::Function* fn = lib->newFunction(NS::String::string(kernel, NS::UTF8StringEncoding));
        if (fn == nullptr) {
            lib->release();
            err = path + ": no function " + kernel;
            return {};
        }
        MTL::ComputePipelineState* pso = device_->newComputePipelineState(fn, &nserr);
        fn->release();
        lib->release();
        if (pso == nullptr) {
            err = std::string("pipeline creation failed for ") + kernel +
                  (nserr ? ": " + ns_str(nserr->localizedDescription()) : std::string());
            return {};
        }
        if (pso->maxTotalThreadsPerThreadgroup() < u64{ki->groupX} * ki->groupY * ki->groupZ) {
            pso->release();
            err = std::string(kernel) + ": thread group larger than the device allows";
            return {};
        }
        pipes_.push_back({pso, ki});
        pipeInfos_.push_back({kernel, path, sha256_file_hex(path)});
        return {static_cast<u32>(pipes_.size())};
    }

    const std::vector<PipelineInfo>& pipelines() const override { return pipeInfos_; }

    void begin() override {
        if (cb_ != nullptr) {
            error("begin() while a command list is open");
            return;
        }
        pool_ = NS::AutoreleasePool::alloc()->init();
        cb_ = queue_->commandBuffer();
        cb_->retain();
        if (orderValue_ > 0) cb_->encodeWait(order_, orderValue_);
        cur_ = std::make_shared<ListState>();
    }

    void dispatch(PipelineHandle p, const Bind* binds, u32 bindCount, const void* push, u32 pushBytes,
                  u32 groupsX) override {
        if (cb_ == nullptr || !p.valid() || p.id > pipes_.size()) {
            error("dispatch outside a list or with an invalid pipeline");
            return;
        }
        if (compute_ == nullptr) compute_ = cb_->computeCommandEncoder();
        const Pipe& pipe = pipes_[p.id - 1];
        const gen::KernelInfo& ki = *pipe.info;
        compute_->setComputePipelineState(pipe.pso);
        for (u32 i = 0; i < ki.bindingCount; ++i) {
            const gen::KernelBinding& kb = ki.bindings[i];
            if (kb.kind == gen::BindKind::PushConstants) {
                if (push == nullptr || pushBytes != ki.pushBytes) {
                    error(std::string(ki.name) + ": push constants missing or wrong size");
                    return;
                }
                compute_->setBytes(push, pushBytes, kb.mtlIndex);
                continue;
            }
            const Bind* bnd = nullptr;
            for (u32 j = 0; j < bindCount; ++j) {
                if (std::strcmp(binds[j].name, kb.name) == 0) bnd = &binds[j];
            }
            MTL::Buffer* b = bnd != nullptr && bnd->buffer.valid() && bnd->buffer.id <= buffers_.size()
                                 ? buffers_[bnd->buffer.id - 1]
                                 : nullptr;
            if (b == nullptr || (kb.kind != gen::BindKind::Buffer && kb.kind != gen::BindKind::RWBuffer)) {
                error(std::string(ki.name) + ": parameter '" + kb.name + "' not bound to a buffer");
                return;
            }
            compute_->setBuffer(b, 0, kb.mtlIndex);
        }
        compute_->dispatchThreadgroups(MTL::Size::Make(groupsX, 1, 1), MTL::Size::Make(ki.groupX, ki.groupY, ki.groupZ));
    }

    bool clearAndPresent(u8 r, u8 g, u8 b) override {
        if (cb_ == nullptr || layer_ == nullptr) return false;
        endCompute();
        CA::MetalDrawable* drawable = layer_->nextDrawable();
        if (drawable == nullptr) return false;
        auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
        auto* ca = pass->colorAttachments()->object(0);
        ca->setTexture(drawable->texture());
        ca->setLoadAction(MTL::LoadActionClear);
        ca->setStoreAction(MTL::StoreActionStore);
        ca->setClearColor(MTL::ClearColor::Make(r / 255.0, g / 255.0, b / 255.0, 1.0));
        MTL::RenderCommandEncoder* enc = cb_->renderCommandEncoder(pass);
        enc->endEncoding();
        // presentedTime needs addPresentedHandler: (iOS 10.3+ devices, macOS 10.15.4+); the iOS
        // simulator's drawable lacks it, so the present time is then unknown rather than a crash.
        if (class_respondsToSelector(object_getClass(reinterpret_cast<id>(drawable)), sel_registerName("addPresentedHandler:"))) {
            std::shared_ptr<ListState> st = cur_;
            st->presents = true;
            drawable->addPresentedHandler([st](MTL::Drawable* d) {
                st->presented = d->presentedTime();
                st->presentDone.store(true, std::memory_order_release);
            });
        }
        cb_->presentDrawable(drawable);
        return true;
    }

    ListId submit() override {
        if (cb_ == nullptr) {
            error("submit() without begin()");
            return 0;
        }
        endCompute();
        cb_->encodeSignalEvent(order_, ++orderValue_);
        std::shared_ptr<ListState> st = cur_;
        cb_->addCompletedHandler([st](MTL::CommandBuffer* c) {
            st->gpuStart = c->GPUStartTime();
            st->gpuEnd = c->GPUEndTime();
            st->error = c->status() == MTL::CommandBufferStatusError;
            st->done.store(true, std::memory_order_release);
        });
        cb_->commit();
        const ListId id = ++lastId_;
        lists_.push_back({id, cb_, st});
        cb_ = nullptr;
        cur_.reset();
        pool_->release();
        pool_ = nullptr;
        while (lists_.size() > 512) {
            List& l = lists_.front();
            l.cb->waitUntilCompleted();
            l.cb->release();
            lists_.pop_front();
        }
        return id;
    }

    ListTiming timing(ListId id) override {
        ListTiming t;
        const List* l = find(id);
        if (l == nullptr) {
            t.done = true;
            t.error = true;
            return t;
        }
        ListState& s = *l->state;
        if (!s.done.load(std::memory_order_acquire)) return t;
        // A drawable that is never shown (app backgrounded) has no presented time: give up on it
        // 250 ms after the GPU finished, and report the presentation as unknown.
        const bool presented = !s.presents || s.presentDone.load(std::memory_order_acquire);
        if (!presented && now_ns() - host_seconds_to_ns(s.gpuEnd) < 250000000) return t;
        t.done = true;
        t.error = s.error;
        if (s.error && !s.errorCounted) {
            s.errorCounted = true;
            countError("command buffer failed");
        }
        if (s.gpuEnd > 0.0 && s.gpuStart > 0.0) {
            t.startNs = host_seconds_to_ns(s.gpuStart);
            t.endNs = host_seconds_to_ns(s.gpuEnd);
            t.durationNs = t.endNs - t.startNs;
        }
        if (s.presents && presented && s.presented > 0.0) t.presentedNs = host_seconds_to_ns(s.presented);
        return t;
    }

    void wait(ListId id) override {
        const List* l = find(id);
        if (l != nullptr) l->cb->waitUntilCompleted();
    }

    void waitIdle() override {
        for (auto& l : lists_) l.cb->waitUntilCompleted();
    }

    u64 errorCount() const override { return errors_; }
    const std::string& firstError() const override { return firstError_; }

private:
    struct Pipe {
        MTL::ComputePipelineState* pso;
        const gen::KernelInfo* info;
    };
    struct List {
        ListId id;
        MTL::CommandBuffer* cb;
        std::shared_ptr<ListState> state;
    };

    void endCompute() {
        if (compute_ != nullptr) {
            compute_->endEncoding();
            compute_ = nullptr;
        }
    }
    const List* find(ListId id) const {
        for (auto it = lists_.rbegin(); it != lists_.rend(); ++it) {
            if (it->id == id) return &*it;
        }
        return nullptr;
    }
    void error(const std::string& msg) { countError(msg); }
    void countError(const std::string& msg) {
        std::lock_guard<std::mutex> lk(errMu_);
        if (errors_++ == 0) firstError_ = msg;
    }

    MTL::Device* device_ = nullptr;
    MTL::CommandQueue* queue_ = nullptr;
    MTL::Event* order_ = nullptr;
    u64 orderValue_ = 0;
    CA::MetalLayer* layer_ = nullptr;
    std::string kernelDir_;
    DeviceInfo info_;
    std::vector<MTL::Buffer*> buffers_;
    std::vector<Pipe> pipes_;
    std::vector<PipelineInfo> pipeInfos_;
    std::deque<List> lists_;
    ListId lastId_ = 0;
    NS::AutoreleasePool* pool_ = nullptr;
    MTL::CommandBuffer* cb_ = nullptr;
    MTL::ComputeCommandEncoder* compute_ = nullptr;
    std::shared_ptr<ListState> cur_;
    std::mutex errMu_;
    u64 errors_ = 0;
    std::string firstError_;
};

} // namespace

std::unique_ptr<Gfx> create_gfx() { return std::make_unique<MetalGfx>(); }

} // namespace ag::gfx
