// guest_vk.h -- libvulkan for an app's native code, on the PC's Vulkan (guest_vk.cpp).

#pragma once

#include <cstdint>

namespace winapk::host {

// The Vulkan command `name` as an app's native code calls it (System V); null for other names.
void* guest_vk_resolve(const char* name);

// The Vulkan version a device with this PC's Vulkan would report as android.hardware.vulkan.version
// (major << 22 | minor << 12, at most 1.3), 0 when there is none. Probed once, by making an instance.
uint32_t guest_vk_api_version();

}  // namespace winapk::host
