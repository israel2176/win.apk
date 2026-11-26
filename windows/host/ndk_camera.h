// ndk_camera.h -- libcamera2ndk and libmediandk's AImageReader / AImage for an app's native code,
// on the PC's webcams (ndk_camera.cpp).

#pragma once

namespace winapk::host {

// The ACamera*, ACapture*, AImageReader_* or AImage_* function `name`; null for other names.
void* ndk_camera_resolve(const char* name);

}  // namespace winapk::host
