// Apple system facts (macOS and iOS). Copied from NeonTide/lab/src/sysinfo_apple.cpp (commit 3dc7266);
// its sysctl and NSProcessInfo calls follow Lightbound/src/bench/SysInfo.cpp (commit 5a186a9). On macOS, battery level and charging come
// from IOKit power sources; on iOS (no IOKit), battery, brightness and display refresh rate come
// from the Swift host through the C ABI (M1) and are merged in here.
#include "sysinfo.h"

#include <CoreFoundation/CoreFoundation.h>
#include <TargetConditionals.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <sys/sysctl.h>
#if TARGET_OS_OSX
#include <IOKit/IOKitLib.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#endif

#include <cstring>

namespace ag::lab {
namespace {

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\0')) s.pop_back();
    return s;
}

std::string sysctl_string(const char* name) {
    char buf[256] = {};
    size_t len = sizeof(buf) - 1;
    if (sysctlbyname(name, buf, &len, nullptr, 0) != 0) return {};
    return trim(buf);
}

u64 sysctl_u64(const char* name) {
    u64 v = 0;
    size_t len = sizeof(v);
    if (sysctlbyname(name, &v, &len, nullptr, 0) != 0) return 0;
    if (len == sizeof(u32)) {
        u32 v32 = 0;
        std::memcpy(&v32, &v, sizeof(v32));
        return v32;
    }
    return v;
}

id process_info() {
    Class cls = objc_getClass("NSProcessInfo");
    if (cls == nullptr) return nullptr;
    using Fn = id (*)(Class, SEL);
    return reinterpret_cast<Fn>(objc_msgSend)(cls, sel_registerName("processInfo"));
}

long process_info_long(const char* selector) {
    id pi = process_info();
    if (pi == nullptr) return -1;
    using Fn = long (*)(id, SEL);
    return reinterpret_cast<Fn>(objc_msgSend)(pi, sel_registerName(selector));
}

/// The host's platform-state JSON (mobile), or an empty object.
Json host_json() {
    Json j;
    std::string err;
    const std::string text = platform_state_json();
    if (text.empty() || !Json::parse(text, j, err) || !j.isObject()) {
        return Json::object();
    }
    return j;
}

void merge(Json& into, const Json* from) {
    if (from == nullptr || !from->isObject()) return;
    for (const auto& kv : from->members()) into.set(kv.first, kv.second);
}

#if TARGET_OS_OSX
std::string gpu_cores() {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AGXAccelerator"));
    if (svc == 0) return "unknown";
    std::string out = "unknown";
    CFTypeRef v = IORegistryEntryCreateCFProperty(svc, CFSTR("gpu-core-count"), kCFAllocatorDefault, 0);
    if (v != nullptr) {
        long n = 0;
        if (CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberLongType, &n)) {
            out = std::to_string(n);
        }
        CFRelease(v);
    }
    IOObjectRelease(svc);
    return out;
}

std::string cf_string(CFStringRef s) {
    if (s == nullptr) return {};
    char buf[128] = {};
    if (!CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8)) return {};
    return buf;
}

void mac_power(Json& s) {
    std::string source = "unknown";
    i64 percent = -1;
    bool charging = false;
    bool haveBattery = false;
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (info != nullptr) {
        source = cf_string(IOPSGetProvidingPowerSourceType(info));
        CFArrayRef list = IOPSCopyPowerSourcesList(info);
        if (list != nullptr) {
            for (CFIndex i = 0; i < CFArrayGetCount(list); ++i) {
                CFDictionaryRef d = IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(list, i));
                if (d == nullptr) continue;
                int cur = 0, max = 0;
                auto num = [&](CFStringRef key, int& out) {
                    auto v = static_cast<CFNumberRef>(CFDictionaryGetValue(d, key));
                    return v != nullptr && CFNumberGetValue(v, kCFNumberIntType, &out);
                };
                if (num(CFSTR(kIOPSCurrentCapacityKey), cur) && num(CFSTR(kIOPSMaxCapacityKey), max) && max > 0) {
                    percent = i64{cur} * 100 / max;
                    haveBattery = true;
                }
                auto ch = static_cast<CFBooleanRef>(CFDictionaryGetValue(d, CFSTR(kIOPSIsChargingKey)));
                if (ch != nullptr) charging = CFBooleanGetValue(ch);
            }
            CFRelease(list);
        }
        CFRelease(info);
    }
    s.set("power_source", Json::str(source.empty() ? "unknown" : source));
    if (haveBattery) {
        s.set("battery_percent", Json::num(percent));
        s.set("charging", Json::boolean(charging));
    } else {
        s.set("battery_percent", Json::str("n/a: no battery"));
        s.set("charging", Json::str("n/a: no battery"));
    }
    s.set("display_refresh_hz", Json::str("n/a: headless lab runner (no display used)"));
    s.set("brightness", Json::str("n/a: headless lab runner (no display used)"));
}
#endif

} // namespace

std::string device_model() {
#if TARGET_OS_OSX
    return sysctl_string("hw.model");      // e.g. Mac17,4
#else
    return sysctl_string("hw.machine");    // e.g. iPhone13,2 (hw.model is the board id on iOS)
#endif
}

std::string device_soc() {
#if TARGET_OS_OSX
    return sysctl_string("machdep.cpu.brand_string");   // e.g. "Apple M5"
#else
    // The lab's iOS devices; anything else is reported, not guessed.
    static const struct { const char* model; const char* soc; } kTable[] = {
        {"iPad8,5", "A12X"}, {"iPad8,6", "A12X"}, {"iPad8,7", "A12X"}, {"iPad8,8", "A12X"},
        {"iPhone13,2", "A14"}, {"iPad14,1", "A15"}, {"iPad14,2", "A15"}, {"iPhone18,2", "A19 Pro"},
    };
    const std::string m = device_model();
    for (const auto& e : kTable) {
        if (m == e.model) return e.soc;
    }
    return "unknown: " + m;
#endif
}

std::string thermal_state() {
    switch (process_info_long("thermalState")) {
        case 0: return "nominal";
        case 1: return "fair";
        case 2: return "serious";
        case 3: return "critical";
        default: return "unknown";
    }
}

Json device_facts() {
    Json d = Json::object();
#if TARGET_OS_OSX
    d.set("platform", Json::str("macOS"));
    d.set("os", Json::str("macOS " + sysctl_string("kern.osproductversion")));
    d.set("cpu", Json::str(sysctl_string("machdep.cpu.brand_string")));
    d.set("gpu_cores", Json::str(gpu_cores()));
#else
    d.set("platform", Json::str("iOS"));
    d.set("os", Json::str("iOS " + sysctl_string("kern.osproductversion")));
    d.set("cpu", Json::str("n/a: not exposed on iOS (see model)"));
#endif
    d.set("model", Json::str(device_model()));
    d.set("soc", Json::str(device_soc()));
    d.set("os_build", Json::str(sysctl_string("kern.osversion")));
    d.set("cpu_cores", Json::str(std::to_string(sysctl_u64("hw.perflevel0.physicalcpu")) + "P+" +
                                 std::to_string(sysctl_u64("hw.perflevel1.physicalcpu")) + "E"));
    d.set("ram_bytes", Json::num(static_cast<i64>(sysctl_u64("hw.memsize"))));
    merge(d, host_json().get("device"));
    return d;
}

Json device_state() {
    Json s = Json::object();
    s.set("thermal_state", Json::str(thermal_state()));
    s.set("low_power_mode", Json::boolean(process_info_long("isLowPowerModeEnabled") == 1));
#if TARGET_OS_OSX
    mac_power(s);
#endif
    merge(s, host_json().get("state"));
    return s;
}

} // namespace ag::lab
