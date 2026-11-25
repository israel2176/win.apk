// guest_gl_extensions.h -- the GL and EGL extensions an app's native code is told of.
//
// A driver on Android lists only the extensions it implements, and an app that
// finds one listed asks eglGetProcAddress for its functions and counts on them:
// Skia's GrGLInterface (Flutter's renderer) refuses the whole context when one
// is missing ("Failed to set up Skia Gr context"), WebRender (Firefox) panics.
// ANGLE reports its own and Chromium's extensions besides the Khronos ones,
// more of them on a real GPU than under Wine, and its build need not have
// every function its header declares. So a guest is told an extension only
// when the Khronos headers or ANGLE's gl2ext_angle.h declare it, the guest has
// a wrapper for each of its functions (tools/gen_gl_stubs.py writes the table,
// guest_gl_extensions.inc), and the ANGLE loaded resolves each of those
// (gl_loader.h): exactly what it can call. GL_ANGLE_base_vertex_base_instance
// (Skia's base-instance draws) and GL_CHROMIUM_bind_uniform_location (Skia's
// glBindUniformLocationCHROMIUM) pass when this ANGLE has their functions.
//
// No window or JNI here, so test/gl_test.cpp checks it against the real ANGLE.

#pragma once

#include <string>

namespace winapk::host {

// Whether a guest may be told of extension `name`. Worked out once per name:
// the DLLs stay loaded, so the answer cannot change within a process.
bool guest_extension_allowed(const std::string& name);

// The functions ANGLE must resolve for `name` (space-separated; Win.apk's own
// functions are not listed), or nullptr when no header declares it.
const char* guest_extension_functions(const std::string& name);

// A driver's space-separated extension list without the ones a guest may not
// see, in the driver's order; kept for the life of the process, as the
// driver's own string is (a guest holds the pointer). nullptr for nullptr.
const char* guest_extensions_filtered(const char* list);

}  // namespace winapk::host
