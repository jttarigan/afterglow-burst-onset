/* C ABI between the platform shells (platform/ios Swift, platform/android JNI) and the lab runner
 * (lab/src/app.h). One App per process. All calls on the render thread. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { AG_SURFACE_NONE = 0, AG_SURFACE_METAL_LAYER = 1, AG_SURFACE_ANDROID_WINDOW = 2 };

/* args: launch arguments joined by spaces (--key=value ...). root_in: read-only payload (kernels/,
 * levels/); root_out: writable directory (results/<run-id>/ is created below it). Returns 0 on
 * success; ag_app_message() explains a failure. */
int ag_app_create(const char* args, const char* root_in, const char* root_out, int surface_kind, void* surface,
                  int width, int height);
/* One display-link tick. target_present_ns on the lab clock (0 if unknown), period_ns the refresh
 * period. Returns 0 to continue, 1 when the run is finished (files written), -1 on error. */
int ag_app_frame(int64_t target_present_ns, int64_t period_ns);
/* Host state as JSON {"device": {...}, "state": {...}}; call about once a second. */
void ag_app_platform_state(const char* json);
/* "background" or "interrupt": ends the run, marked interrupted. */
void ag_app_event(const char* name);
/* Ends the run early (writes the files). */
int ag_app_finish(void);
const char* ag_app_message(void);
const char* ag_app_run_dir(void);
/* The lab clock in ns (mach_absolute_time on Apple, CLOCK_MONOTONIC on Android). */
int64_t ag_app_now_ns(void);
void ag_app_destroy(void);

#ifdef __cplusplus
}
#endif
