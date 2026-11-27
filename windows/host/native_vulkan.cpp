// native_vulkan.cpp -- com.macapk.loader.HostVulkan: the Vulkan version the PC gives an app's
// native code (host/vk/guest_vk.h), for the features PackageManager reports.

#include "host_natives.h"
#include "vk/guest_vk.h"

namespace winapk::host {

namespace {

jint JNICALL Vk_apiVersion(JNIEnv*, jclass) { return static_cast<jint>(guest_vk_api_version()); }

const HostMethod kMethods[] = {
    WINAPK_HOST_NATIVE("com.macapk.loader.HostVulkan", "nativeApiVersion", "()I", Vk_apiVersion),
};

}  // namespace

HostMethods native_vulkan_methods() { return {kMethods, sizeof(kMethods) / sizeof(kMethods[0])}; }

}  // namespace winapk::host
