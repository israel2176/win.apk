// Fences: HWUI's GL path makes none on Windows (no EGL_ANDROID_native_fence_sync on ANGLE: every
// fence descriptor is -1, which sync_wait takes as signalled).
#pragma once
static inline int sync_wait(int fd, int) { return 0; }
static inline int sync_merge(const char*, int, int) { return -1; }
