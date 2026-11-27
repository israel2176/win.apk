// gl_loader.h -- ANGLE (libEGL.dll, libGLESv2.dll) for the runtime's GL.
//
// The runtime does not link ANGLE: it starts without it, and every GL or EGL
// call (gl_stubs.inc) looks its function up here on first use. ANGLE's DLLs
// sit next to winapk-runtime.exe (stage_runtime.sh copies them there).

#pragma once

namespace winapk::host {

enum class GlLib { Egl, Gles };

// The ANGLE function `name`, or nullptr when ANGLE is not installed or does
// not have it. Names the DLL does not export (extensions) are asked of
// eglGetProcAddress.
void* gl_proc(const char* name, GlLib lib);

// The runtime's own forwarder of the GL or EGL function `name` (gl_stubs.inc), which takes the
// display's turn (gl_turn.h) before calling ANGLE's, or nullptr when it has none. Code that looks GL
// up by name gets this, not gl_proc's.
void* gl_stub(const char* name);

// True when both ANGLE DLLs loaded.
bool gl_available();

}  // namespace winapk::host
