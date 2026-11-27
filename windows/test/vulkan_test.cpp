// vulkan_test.cpp -- winapk-vulkan-test: libvulkan (host/vk/guest_vk.cpp) as an app's native code
// calls it, System V, through the exported names and vkGet*ProcAddr, on the PC's Vulkan: the
// loader beside the runtime and its driver, or SwiftShader where there is none (CI's runners have
// no GPU). Run from the staged bin\, where vulkan-1.dll and SwiftShader are.
//
// Checks: the version (at most 1.3) and the extensions (the window system's offered, Windows'
// hidden, one of those refused); an instance with a debug messenger in its chain, whose callback
// is the test's own System V function; a physical device at 1.3 at most, VK_KHR_swapchain among
// its extensions, a device and a queue; vkGet*ProcAddr's answers; a surface on the runtime's
// window, its capabilities, formats and present modes; a swapchain of 64x48 images, two frames
// cleared red then blue, acquired, presented and shown on the window with those colours; a
// window resized since: VK_ERROR_OUT_OF_DATE_KHR; the version PackageManager reports.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_android.h>

#include <windows.h>

#include "native_window.h"
#include "vk/guest_vk.h"
#include "winapk/guest_abi.h"

// ---- the stand-ins ---------------------------------------------------------------------------

namespace {
int g_window = 0;
std::atomic<int> g_window_w{64}, g_window_h{48};
std::atomic<int> g_frames{0};
uint32_t g_last_top = 0, g_last_bottom = 0;
int g_last_w = 0, g_last_h = 0;
}  // namespace

namespace winapk {
namespace arm64 {
void* host_callable(void* fn) { return fn; }
}  // namespace arm64
namespace host {
const void* native_window_handle() { return &g_window; }
bool native_window_buffer_size(const void* window, int* w, int* h) {
    if (window != &g_window) return false;
    *w = g_window_w, *h = g_window_h;
    return true;
}
void guest_frame_present(const uint32_t* argb, int w, int h) {
    g_last_w = w, g_last_h = h;
    g_last_top = argb[static_cast<size_t>(w) * 2 + 2];
    g_last_bottom = argb[static_cast<size_t>(w) * (h - 2) + 2];
    ++g_frames;
}
}  // namespace host
}  // namespace winapk

namespace {

int g_failures = 0;
void check(const char* what, bool ok, const std::string& detail = {}) {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
    if (!ok) ++g_failures;
}

#define GUEST WINAPK_GUEST_ABI

template <typename F>
F exported(const char* name) {
    void* p = winapk::host::guest_vk_resolve(name);
    if (!p) {
        std::printf("  [FAIL] %s is not exported\n", name);
        std::exit(1);
    }
    return reinterpret_cast<F>(p);
}

using GuestGipa = void*(GUEST*)(VkInstance, const char*);
using GuestGdpa = void*(GUEST*)(VkDevice, const char*);
GuestGipa g_gipa;
GuestGdpa g_gdpa;
VkInstance g_instance = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;

// A command through vkGetInstanceProcAddr, as an app's loader wrapper gets it; the type is the
// System V one of the same signature.
template <typename R, typename... A>
auto inst(const char* name) {
    using F = R(GUEST*)(A...);
    void* p = g_gipa(g_instance, name);
    if (!p) std::printf("  [FAIL] vkGetInstanceProcAddr(%s) is null\n", name), ++g_failures;
    return reinterpret_cast<F>(p);
}
template <typename R, typename... A>
auto dev(const char* name) {
    using F = R(GUEST*)(A...);
    void* p = g_gdpa(g_device, name);
    if (!p) std::printf("  [FAIL] vkGetDeviceProcAddr(%s) is null\n", name), ++g_failures;
    return reinterpret_cast<F>(p);
}

std::atomic<int> g_messages{0};
GUEST VkBool32 on_message(VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
                          const VkDebugUtilsMessengerCallbackDataEXT*, void* user) {
    if (user == &g_messages) ++g_messages;
    return VK_FALSE;
}

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const auto& e : list) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08x", v);
    return b;
}

}  // namespace

int main() {
    // Unbuffered: a hang or a crash in the driver must not keep the lines before it back.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_gipa = exported<GuestGipa>("vkGetInstanceProcAddr");
    g_gdpa = exported<GuestGdpa>("vkGetDeviceProcAddr");

    std::printf("instance\n");
    auto version_fn = reinterpret_cast<VkResult(GUEST*)(uint32_t*)>(g_gipa(nullptr, "vkEnumerateInstanceVersion"));
    uint32_t version = 0;
    check("vkEnumerateInstanceVersion: 1.1 to 1.3", version_fn && version_fn(&version) == VK_SUCCESS &&
                                                        version >= VK_API_VERSION_1_1 && version <= VK_API_VERSION_1_3,
          hex(version));
    auto ext_fn = reinterpret_cast<VkResult(GUEST*)(const char*, uint32_t*, VkExtensionProperties*)>(
        g_gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
    uint32_t n = 0;
    ext_fn(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    ext_fn(nullptr, &n, exts.data());
    exts.resize(n);
    check("VK_KHR_surface and VK_KHR_android_surface offered",
          has_extension(exts, "VK_KHR_surface") && has_extension(exts, "VK_KHR_android_surface"));
    check("VK_KHR_win32_surface hidden", !has_extension(exts, "VK_KHR_win32_surface"));
    const bool debug_utils = has_extension(exts, "VK_EXT_debug_utils");

    auto create_instance = reinterpret_cast<VkResult(GUEST*)(const VkInstanceCreateInfo*, const void*, VkInstance*)>(
        g_gipa(nullptr, "vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    const char* win32[] = {"VK_KHR_win32_surface"};
    VkInstanceCreateInfo bad{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    bad.pApplicationInfo = &app;
    bad.enabledExtensionCount = 1;
    bad.ppEnabledExtensionNames = win32;
    VkInstance unused = VK_NULL_HANDLE;
    check("asking for VK_KHR_win32_surface: EXTENSION_NOT_PRESENT",
          create_instance(&bad, nullptr, &unused) == VK_ERROR_EXTENSION_NOT_PRESENT);

    std::vector<const char*> want = {"VK_KHR_surface", "VK_KHR_android_surface"};
    if (debug_utils) want.push_back("VK_EXT_debug_utils");
    VkDebugUtilsMessengerCreateInfoEXT messenger{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messenger.pfnUserCallback = reinterpret_cast<PFN_vkDebugUtilsMessengerCallbackEXT>(&on_message);
    messenger.pUserData = &g_messages;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pNext = debug_utils ? &messenger : nullptr;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<uint32_t>(want.size());
    info.ppEnabledExtensionNames = want.data();
    const VkResult made = create_instance(&info, nullptr, &g_instance);
    check("an instance with the window system's extensions", made == VK_SUCCESS, std::to_string(made));
    if (made != VK_SUCCESS) {
        std::printf("%d FAILED\n", g_failures);
        return 1;
    }
    check("no Win32 surface command", g_gipa(g_instance, "vkCreateWin32SurfaceKHR") == nullptr);
    if (debug_utils) {
        // A messenger of the app's: a message submitted reaches its System V callback.
        VkDebugUtilsMessengerEXT m = VK_NULL_HANDLE;
        inst<VkResult, VkInstance, const VkDebugUtilsMessengerCreateInfoEXT*, const void*, VkDebugUtilsMessengerEXT*>(
            "vkCreateDebugUtilsMessengerEXT")(g_instance, &messenger, nullptr, &m);
        const int before = g_messages;
        VkDebugUtilsMessengerCallbackDataEXT data{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
        data.pMessage = "from the test";
        inst<void, VkInstance, VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
             const VkDebugUtilsMessengerCallbackDataEXT*>("vkSubmitDebugUtilsMessageEXT")(
            g_instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, &data);
        check("a debug messenger's callback is called", g_messages > before, std::to_string(g_messages - before));
        inst<void, VkInstance, VkDebugUtilsMessengerEXT, const void*>("vkDestroyDebugUtilsMessengerEXT")(g_instance, m,
                                                                                                         nullptr);
    } else {
        std::printf("  [SKIP] no VK_EXT_debug_utils\n");
    }

    std::printf("device\n");
    auto enumerate = inst<VkResult, VkInstance, uint32_t*, VkPhysicalDevice*>("vkEnumeratePhysicalDevices");
    uint32_t count = 0;
    enumerate(g_instance, &count, nullptr);
    std::vector<VkPhysicalDevice> pds(count);
    enumerate(g_instance, &count, pds.data());
    check("a physical device", count >= 1, std::to_string(count));
    if (!count) return 1;
    VkPhysicalDevice pd = pds[0];
    VkPhysicalDeviceProperties props{};
    inst<void, VkPhysicalDevice, VkPhysicalDeviceProperties*>("vkGetPhysicalDeviceProperties")(pd, &props);
    std::printf("  device: %s, Vulkan %u.%u\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
                VK_API_VERSION_MINOR(props.apiVersion));
    check("its version is at most 1.3", props.apiVersion <= VK_API_VERSION_1_3 + 0xfff, hex(props.apiVersion));
    auto dev_exts = inst<VkResult, VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*>(
        "vkEnumerateDeviceExtensionProperties");
    dev_exts(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> dexts(n);
    dev_exts(pd, nullptr, &n, dexts.data());
    dexts.resize(n);
    check("VK_KHR_swapchain offered", has_extension(dexts, "VK_KHR_swapchain"));
    check("VK_KHR_external_memory_win32 hidden", !has_extension(dexts, "VK_KHR_external_memory_win32"));

    auto families_fn = inst<void, VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*>(
        "vkGetPhysicalDeviceQueueFamilyProperties");
    families_fn(pd, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    families_fn(pd, &n, families.data());
    uint32_t family = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            family = i;
            break;
        }
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    const char* swapchain_ext[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = swapchain_ext;
    auto create_device = inst<VkResult, VkPhysicalDevice, const VkDeviceCreateInfo*, const void*, VkDevice*>("vkCreateDevice");
    check("a device with VK_KHR_swapchain", create_device(pd, &di, nullptr, &g_device) == VK_SUCCESS);
    VkQueue queue = VK_NULL_HANDLE;
    dev<void, VkDevice, uint32_t, uint32_t, VkQueue*>("vkGetDeviceQueue")(g_device, family, 0, &queue);
    check("a queue", queue != VK_NULL_HANDLE);
    check("vkGetDeviceProcAddr: device commands only", g_gdpa(g_device, "vkCmdDraw") != nullptr &&
                                                           g_gdpa(g_device, "vkCreateInstance") == nullptr &&
                                                           g_gdpa(g_device, "vkEnumeratePhysicalDevices") == nullptr);

    std::printf("surface and swapchain\n");
    VkAndroidSurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    si.window = reinterpret_cast<ANativeWindow*>(const_cast<void*>(winapk::host::native_window_handle()));
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    check("a surface on the window",
          inst<VkResult, VkInstance, const VkAndroidSurfaceCreateInfoKHR*, const void*, VkSurfaceKHR*>(
              "vkCreateAndroidSurfaceKHR")(g_instance, &si, nullptr, &surface) == VK_SUCCESS);
    VkBool32 supported = VK_FALSE;
    inst<VkResult, VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32*>("vkGetPhysicalDeviceSurfaceSupportKHR")(
        pd, family, surface, &supported);
    check("the queue family presents", supported == VK_TRUE);
    VkSurfaceCapabilitiesKHR caps{};
    inst<VkResult, VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR*>(
        "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")(pd, surface, &caps);
    check("the window's size", caps.currentExtent.width == 64 && caps.currentExtent.height == 48,
          std::to_string(caps.currentExtent.width) + "x" + std::to_string(caps.currentExtent.height));
    uint32_t formats = 0;
    VkSurfaceFormatKHR format[4]{};
    formats = 4;
    inst<VkResult, VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkSurfaceFormatKHR*>("vkGetPhysicalDeviceSurfaceFormatsKHR")(
        pd, surface, &formats, format);
    check("RGBA8 formats", formats >= 1 && format[0].format == VK_FORMAT_R8G8B8A8_UNORM);
    uint32_t modes = 4;
    VkPresentModeKHR mode[4]{};
    inst<VkResult, VkPhysicalDevice, VkSurfaceKHR, uint32_t*, VkPresentModeKHR*>(
        "vkGetPhysicalDeviceSurfacePresentModesKHR")(pd, surface, &modes, mode);
    check("FIFO first", modes >= 1 && mode[0] == VK_PRESENT_MODE_FIFO_KHR);

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface;
    sci.minImageCount = 3;
    sci.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
    sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sci.imageExtent = caps.currentExtent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    check("a swapchain", dev<VkResult, VkDevice, const VkSwapchainCreateInfoKHR*, const void*, VkSwapchainKHR*>(
                             "vkCreateSwapchainKHR")(g_device, &sci, nullptr, &swapchain) == VK_SUCCESS);
    uint32_t image_count = 0;
    auto get_images = dev<VkResult, VkDevice, VkSwapchainKHR, uint32_t*, VkImage*>("vkGetSwapchainImagesKHR");
    get_images(g_device, swapchain, &image_count, nullptr);
    std::vector<VkImage> images(image_count);
    get_images(g_device, swapchain, &image_count, images.data());
    check("its images", image_count >= 3, std::to_string(image_count));

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    dev<VkResult, VkDevice, const VkCommandPoolCreateInfo*, const void*, VkCommandPool*>("vkCreateCommandPool")(
        g_device, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    dev<VkResult, VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*>("vkAllocateCommandBuffers")(
        g_device, &cai, &cb);
    VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
    auto create_sem = dev<VkResult, VkDevice, const VkSemaphoreCreateInfo*, const void*, VkSemaphore*>("vkCreateSemaphore");
    create_sem(g_device, &semi, nullptr, &acquired);
    create_sem(g_device, &semi, nullptr, &rendered);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    dev<VkResult, VkDevice, const VkFenceCreateInfo*, const void*, VkFence*>("vkCreateFence")(g_device, &fci, nullptr, &fence);

    auto acquire = dev<VkResult, VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*>("vkAcquireNextImageKHR");
    auto begin = dev<VkResult, VkCommandBuffer, const VkCommandBufferBeginInfo*>("vkBeginCommandBuffer");
    auto end = dev<VkResult, VkCommandBuffer>("vkEndCommandBuffer");
    auto barrier = dev<void, VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t,
                       const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t,
                       const VkImageMemoryBarrier*>("vkCmdPipelineBarrier");
    auto clear = dev<void, VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue*, uint32_t,
                     const VkImageSubresourceRange*>("vkCmdClearColorImage");
    auto submit = dev<VkResult, VkQueue, uint32_t, const VkSubmitInfo*, VkFence>("vkQueueSubmit");
    auto present = dev<VkResult, VkQueue, const VkPresentInfoKHR*>("vkQueuePresentKHR");
    auto wait = dev<VkResult, VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t>("vkWaitForFences");
    auto reset = dev<VkResult, VkDevice, uint32_t, const VkFence*>("vkResetFences");
    auto reset_cb = dev<VkResult, VkCommandBuffer, VkCommandBufferResetFlags>("vkResetCommandBuffer");

    // One frame: clear the acquired image to `color`, hand it over in PRESENT_SRC, present it.
    auto frame = [&](float r, float g, float b) -> VkResult {
        uint32_t index = 0;
        VkResult res = acquire(g_device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
        if (res != VK_SUCCESS) return res;
        reset_cb(cb, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin(cb, &bi);
        VkImageMemoryBarrier to_clear{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        to_clear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_clear.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_clear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_clear.srcQueueFamilyIndex = to_clear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_clear.image = images[index];
        to_clear.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                &to_clear);
        VkClearColorValue color{};
        color.float32[0] = r, color.float32[1] = g, color.float32[2] = b, color.float32[3] = 1.0f;
        clear(cb, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &to_clear.subresourceRange);
        VkImageMemoryBarrier to_present = to_clear;
        to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_present.dstAccessMask = 0;
        to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                &to_present);
        end(cb);
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        sub.waitSemaphoreCount = 1;
        sub.pWaitSemaphores = &acquired;
        sub.pWaitDstStageMask = &stage;
        sub.commandBufferCount = 1;
        sub.pCommandBuffers = &cb;
        sub.signalSemaphoreCount = 1;
        sub.pSignalSemaphores = &rendered;
        submit(queue, 1, &sub, fence);
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &rendered;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &index;
        res = present(queue, &pi);
        wait(g_device, 1, &fence, VK_TRUE, UINT64_MAX);
        reset(g_device, 1, &fence);
        return res;
    };
    const int before = g_frames;
    check("a red frame presented", frame(1, 0, 0) == VK_SUCCESS);
    check("shown red, 64x48", g_frames == before + 1 && g_last_w == 64 && g_last_h == 48 && g_last_top == 0xffff0000u &&
                                  g_last_bottom == 0xffff0000u,
          hex(g_last_top));
    check("a blue frame presented", frame(0, 0, 1) == VK_SUCCESS);
    check("shown blue", g_frames == before + 2 && g_last_top == 0xff0000ffu, hex(g_last_top));
    g_window_w = 80, g_window_h = 60;
    check("after the window was resized: OUT_OF_DATE", frame(0, 1, 0) == VK_ERROR_OUT_OF_DATE_KHR);

    std::printf("cleanup\n");
    dev<VkResult, VkDevice>("vkDeviceWaitIdle")(g_device);
    dev<void, VkDevice, VkSwapchainKHR, const void*>("vkDestroySwapchainKHR")(g_device, swapchain, nullptr);
    auto destroy_sem = dev<void, VkDevice, VkSemaphore, const void*>("vkDestroySemaphore");
    destroy_sem(g_device, acquired, nullptr);
    destroy_sem(g_device, rendered, nullptr);
    dev<void, VkDevice, VkFence, const void*>("vkDestroyFence")(g_device, fence, nullptr);
    dev<void, VkDevice, VkCommandPool, const void*>("vkDestroyCommandPool")(g_device, pool, nullptr);
    dev<void, VkDevice, const void*>("vkDestroyDevice")(g_device, nullptr);
    inst<void, VkInstance, VkSurfaceKHR, const void*>("vkDestroySurfaceKHR")(g_instance, surface, nullptr);
    inst<void, VkInstance, const void*>("vkDestroyInstance")(g_instance, nullptr);

    const uint32_t reported = winapk::host::guest_vk_api_version();
    check("android.hardware.vulkan.version: 1.1 to 1.3", reported >= (1u << 22 | 1u << 12) && reported <= (1u << 22 | 3u << 12),
          hex(reported));

    std::printf(g_failures ? "%d FAILED\n" : "ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
