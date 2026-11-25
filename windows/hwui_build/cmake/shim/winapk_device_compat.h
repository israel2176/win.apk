// winapk_device_compat.h: force-included into the AOSP modules built with their android (device)
// sources on Windows (HWUI's render thread and GPU pipeline): what Bionic's headers give those
// sources on a device and MinGW's do not, and Windows' macros that collide with their names.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// windows.h's: GL's projection matrices call their arguments near and far; libbase and minikin
// have their own ARRAYSIZE.
#undef near
#undef far
#undef ARRAYSIZE
#undef ERROR
#undef NO_ERROR
#undef OPAQUE
#undef TRANSPARENT
#undef DELETE

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <io.h>

// Bionic's sys/cdefs.h.
#ifndef __BEGIN_DECLS
#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif
#endif
#define __INTRODUCED_IN(x)
#define __DEPRECATED_IN(x)
#define __REMOVED_IN(x)
#define __INTRODUCED_IN_64(x)
#ifndef __ANDROID_API__
#define __ANDROID_API__ 34
#endif
#ifndef __ANDROID_API_FUTURE__
#define __ANDROID_API_FUTURE__ 10000
#endif

#ifdef __cplusplus
extern "C" {
#endif
// The GNU strerror_r is what Bionic gives C++ (it returns the message).
static inline char* winapk_strerror_r(int errnum, char* buf, size_t len) {
    if (strerror_s(buf, len, errnum) != 0 && len) buf[0] = '\0';
    return buf;
}
static inline int winapk_dprintf(int fd, const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) {
        HANDLE h = (HANDLE)_get_osfhandle(fd);
        DWORD w = 0;
        if (h != INVALID_HANDLE_VALUE) WriteFile(h, buf, (DWORD)(n < (int)sizeof buf ? n : (int)sizeof buf - 1), &w, NULL);
    }
    return n;
}
#ifdef __cplusplus
}
#endif
#define strerror_r winapk_strerror_r
#define dprintf winapk_dprintf

// Bionic's: the kernel thread id of a pthread. HWUI asks it only of the calling thread (its render
// thread, its pool's workers, the UI thread) to name them to the performance-hint session.
#include <pthread.h>
static inline int pthread_gettid_np(pthread_t t) {
    return pthread_equal(t, pthread_self()) ? (int)GetCurrentThreadId() : -1;
}
static inline int gettid(void) { return (int)GetCurrentThreadId(); }
#include <sys/mman.h>

#ifdef __cplusplus
#include <limits>
#include <memory>
#include <atomic>
#include <span>
#endif
