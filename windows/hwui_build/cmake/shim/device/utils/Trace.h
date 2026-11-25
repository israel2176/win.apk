// utils/Trace.h: libutils' says nothing traces on _WIN32 and stops at ATRACE_NAME and ATRACE_CALL;
// HWUI's device sources use the rest of cutils' macros too. No systrace on Windows: all no-ops.
#pragma once
#include <stdint.h>
#define ATRACE_NAME(...)
#define ATRACE_CALL()
#define ATRACE_BEGIN(name) ((void)0)
#define ATRACE_END() ((void)0)
#define ATRACE_INT(name, value) ((void)0)
#define ATRACE_INT64(name, value) ((void)0)
#define ATRACE_ENABLED() 0
#define ATRACE_FORMAT(...)
#define ATRACE_FORMAT_BEGIN(...)
#define ATRACE_ASYNC_BEGIN(name, cookie) ((void)0)
#define ATRACE_ASYNC_END(name, cookie) ((void)0)
