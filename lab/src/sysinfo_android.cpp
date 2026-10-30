// Android system facts (copied from NeonTide/lab/src/sysinfo_android.cpp, commit 3dc7266): system
// properties from C++, everything the framework owns (thermal status,
// battery, brightness, refresh rate) from the Kotlin host via ag_app_platform_state().
#include "sysinfo.h"

#include <sys/system_properties.h>
#include <sys/sysinfo.h>
#include <unistd.h>

namespace ag::lab {
namespace {

std::string prop(const char* key) {
    char v[PROP_VALUE_MAX] = {};
    __system_property_get(key, v);
    return v;
}

Json host_json() {
    Json j;
    std::string err;
    const std::string text = platform_state_json();
    if (text.empty() || !Json::parse(text, j, err) || !j.isObject()) return Json::object();
    return j;
}

void merge(Json& into, const Json* from) {
    if (from == nullptr || !from->isObject()) return;
    for (const auto& kv : from->members()) into.set(kv.first, kv.second);
}

} // namespace

std::string device_model() { return prop("ro.product.model"); }

std::string device_soc() { return prop("ro.soc.manufacturer") + " " + prop("ro.soc.model"); }

std::string thermal_state() {
    const Json h = host_json();
    const Json* s = h.get("state");
    const Json* t = s != nullptr ? s->get("thermal_state") : nullptr;
    return t != nullptr ? t->asString() : "unknown";
}

Json device_facts() {
    Json d = Json::object();
    d.set("platform", Json::str("Android"));
    d.set("model", Json::str(prop("ro.product.model")));
    d.set("manufacturer", Json::str(prop("ro.product.manufacturer")));
    d.set("os", Json::str("Android " + prop("ro.build.version.release") + " (API " + prop("ro.build.version.sdk") + ")"));
    d.set("os_build", Json::str(prop("ro.build.display.id")));
    d.set("fingerprint", Json::str(prop("ro.build.fingerprint")));
    d.set("soc", Json::str(device_soc()));
    d.set("sdk_int", Json::str(prop("ro.build.version.sdk")));
    d.set("cpu", Json::str(prop("ro.soc.model")));
    d.set("cpu_cores", Json::str(std::to_string(sysconf(_SC_NPROCESSORS_CONF))));
    struct sysinfo si {};
    if (sysinfo(&si) == 0) d.set("ram_bytes", Json::num(static_cast<i64>(si.totalram) * si.mem_unit));
    merge(d, host_json().get("device"));
    return d;
}

Json device_state() {
    Json s = Json::object();
    s.set("thermal_state", Json::str("unknown"));
    merge(s, host_json().get("state"));
    return s;
}

} // namespace ag::lab
