// C ABI of the lab runner (lab/include/ag/ag_app.h) for the platform shells.
#include "ag/ag_app.h"

#include "ag/gfx/Clock.h"
#include "app.h"
#include "sysinfo.h"

#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::unique_ptr<ag::lab::App> g_app;
std::string g_message;
std::string g_runDir;
} // namespace

extern "C" int ag_app_create(const char* args, const char* root_in, const char* root_out, int surface_kind,
                             void* surface, int width, int height) {
    std::vector<std::string> words{"aglab-app"};
    std::istringstream ss(args != nullptr ? args : "");
    for (std::string w; ss >> w;) words.push_back(w);
    if (root_in != nullptr) words.push_back(std::string("--root-in=") + root_in);
    if (root_out != nullptr) words.push_back(std::string("--root-out=") + root_out);
    std::vector<const char*> argv;
    for (const auto& w : words) argv.push_back(w.c_str());
    const ag::lab::Args a = ag::lab::parse_args(static_cast<int>(argv.size()), argv.data());
    ag::lab::HostSurface hs;
    hs.kind = static_cast<ag::gfx::SurfaceKind>(surface_kind);
    hs.ptr = surface;
    hs.width = width > 0 ? static_cast<ag::u32>(width) : 0u;
    hs.height = height > 0 ? static_cast<ag::u32>(height) : 0u;
    g_app = std::make_unique<ag::lab::App>();
    std::string err;
    if (!g_app->create(a, hs, err)) {
        g_message = "create failed: " + err;
        g_app.reset();
        return -1;
    }
    g_message = g_app->message();
    g_runDir = g_app->runDir();
    return 0;
}

extern "C" int ag_app_frame(int64_t target_present_ns, int64_t period_ns) {
    if (g_app == nullptr) return -1;
    const int r = g_app->frame(target_present_ns, period_ns);
    g_message = g_app->message();
    return r;
}

extern "C" void ag_app_platform_state(const char* json) {
    // Stored even before ag_app_create: the hosts send it first, so state_start has the host facts.
    if (json != nullptr) ag::lab::set_platform_state_json(json);
}

extern "C" void ag_app_event(const char* name) {
    if (g_app != nullptr && name != nullptr) {
        g_app->event(name);
        g_message = g_app->message();
    }
}

extern "C" int ag_app_finish(void) {
    if (g_app == nullptr) return -1;
    const int r = g_app->finish(false, "finished early by the host");
    g_message = g_app->message();
    return r;
}

extern "C" const char* ag_app_message(void) { return g_message.c_str(); }
extern "C" const char* ag_app_run_dir(void) { return g_runDir.c_str(); }
extern "C" int64_t ag_app_now_ns(void) { return ag::gfx::now_ns(); }
extern "C" void ag_app_destroy(void) { g_app.reset(); }
