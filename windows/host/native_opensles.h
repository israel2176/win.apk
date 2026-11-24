// native_opensles.h -- libOpenSLES for an app's native code: the engine,
// output mix, buffer-queue and file players, and buffer-queue recorders on
// the host's audio (native_opensles.cpp).

#pragma once

namespace winapk::host {

// The guest-callable OpenSL ES entry points and SL_IID_x variables, by name;
// null for other names.
void* native_opensles_resolve(const char* name);

}  // namespace winapk::host
