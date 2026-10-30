// JNI glue for the Android app: the Kotlin host calls the same C ABI as the iOS app
// (lab/include/ag/ag_app.h). stdout/stderr are forwarded to logcat (tag "aglab"), which is how
// tools/device/run_android.sh follows a run. The stdio redirect is copied from
// NeonTide/platform/android/jni/neontide_jni.cpp (commit 3dc7266).
#include "ag/ag_app.h"

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

ANativeWindow* g_window = nullptr;

void* forward_output(void* arg) {
    const int fd = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    std::string line;
    char buf[512];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                __android_log_write(ANDROID_LOG_INFO, "aglab", line.c_str());
                line.clear();
            } else {
                line.push_back(buf[i]);
            }
        }
    }
    return nullptr;
}

void redirect_stdio_once() {
    static bool done = false;
    if (done) return;
    done = true;
    int fds[2];
    if (pipe(fds) != 0) return;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    pthread_t t;
    if (pthread_create(&t, nullptr, forward_output, reinterpret_cast<void*>(static_cast<intptr_t>(fds[0]))) == 0) {
        pthread_detach(t);
    }
}

/// --req-display_refresh_hz from the launch arguments (60 Hz primary; 120 Hz subset, D53/D60).
float requested_hz(const std::string& args) {
    static const std::string key = "--req-display_refresh_hz=";
    const auto p = args.find(key);
    if (p == std::string::npos) return 60.0f;
    const float hz = std::strtof(args.c_str() + p + key.size(), nullptr);
    return hz > 0.0f ? hz : 60.0f;
}

std::string str(JNIEnv* env, jstring s) {
    if (s == nullptr) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out(c);
    env->ReleaseStringUTFChars(s, c);
    return out;
}

} // namespace

extern "C" {

JNIEXPORT jint JNICALL Java_org_afterglow_app_NativeBridge_create(JNIEnv* env, jclass, jstring args, jstring rootIn,
                                                                    jstring rootOut, jobject surface, jint width,
                                                                    jint height) {
    redirect_stdio_once();
    if (g_window != nullptr) ANativeWindow_release(g_window);
    g_window = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    const std::string a = str(env, args), in = str(env, rootIn), out = str(env, rootOut);
    if (g_window != nullptr) {
        // The requested refresh (60 Hz primary, docs/04 §1; 120 Hz subset, D53); the activity also
        // picks the matching display mode. The ALWAYS strategy (API 31) asks for the mode switch
        // even when it is not seamless: with the default, the Tab S7+ stayed at 120 Hz (D46).
        ANativeWindow_setFrameRateWithChangeStrategy(g_window, requested_hz(a), ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                                                     ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS);
    }
    const int rc = ag_app_create(a.c_str(), in.c_str(), out.c_str(),
                                 g_window != nullptr ? AG_SURFACE_ANDROID_WINDOW : AG_SURFACE_NONE, g_window, width, height);
    std::printf("aglab: %s\n", ag_app_message());
    return rc;
}

JNIEXPORT jint JNICALL Java_org_afterglow_app_NativeBridge_frame(JNIEnv*, jclass, jlong targetNs, jlong periodNs) {
    return ag_app_frame(static_cast<int64_t>(targetNs), static_cast<int64_t>(periodNs));
}

JNIEXPORT void JNICALL Java_org_afterglow_app_NativeBridge_platformState(JNIEnv* env, jclass, jstring json) {
    const std::string s = str(env, json);
    ag_app_platform_state(s.c_str());
}

JNIEXPORT void JNICALL Java_org_afterglow_app_NativeBridge_event(JNIEnv* env, jclass, jstring name) {
    const std::string s = str(env, name);
    ag_app_event(s.c_str());
}

JNIEXPORT jint JNICALL Java_org_afterglow_app_NativeBridge_finish(JNIEnv*, jclass) { return ag_app_finish(); }

JNIEXPORT jstring JNICALL Java_org_afterglow_app_NativeBridge_message(JNIEnv* env, jclass) {
    return env->NewStringUTF(ag_app_message());
}

JNIEXPORT jstring JNICALL Java_org_afterglow_app_NativeBridge_runDir(JNIEnv* env, jclass) {
    return env->NewStringUTF(ag_app_run_dir());
}

JNIEXPORT jlong JNICALL Java_org_afterglow_app_NativeBridge_nowNs(JNIEnv*, jclass) { return ag_app_now_ns(); }

JNIEXPORT void JNICALL Java_org_afterglow_app_NativeBridge_destroy(JNIEnv*, jclass) {
    ag_app_destroy();
    std::fflush(stdout);
    if (g_window != nullptr) {
        ANativeWindow_release(g_window);
        g_window = nullptr;
    }
}

} // extern "C"
