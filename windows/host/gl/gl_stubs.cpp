// gl_stubs.cpp -- every GLES and EGL function the runtime can call, forwarded
// to ANGLE on first use (gl_loader.h), each call in its display's turn
// (gl_turn.h). Generated part: gl_stubs.inc (tools/gen_gl_stubs.py).

#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>

#include <cstring>

#include "gl/gl_loader.h"
#include "gl/gl_turn.h"

// GLES 1's one type gl2.h lacks (GLES/gl.h is not included: its prototypes
// would clash with GLES 3's for the names both declare).
typedef khronos_int32_t GLclampx;

#include "gl/gl_stubs.inc"

namespace winapk::host {

void* gl_stub(const char* name) {
    struct Symbol {
        const char* name;
        void* fn;
    };
    static const Symbol kSymbols[] = {WINAPK_GL_STUB_SYMBOLS};
    if (!name) return nullptr;
    size_t lo = 0, hi = sizeof(kSymbols) / sizeof(kSymbols[0]);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = std::strcmp(kSymbols[mid].name, name);
        if (c == 0) return kSymbols[mid].fn;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return nullptr;
}

}  // namespace winapk::host
