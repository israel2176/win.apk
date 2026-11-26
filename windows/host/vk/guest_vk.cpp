// guest_vk.cpp -- libvulkan for an app's native code, on the PC's Vulkan.
//
// The PC's Vulkan is the Khronos loader shipped beside the runtime (bin\vulkan-1.dll, ANGLE's)
// and the GPU's driver it finds; where no driver offers a device (no GPU, or a driver without
// Vulkan, as on CI), the loader is pointed at the SwiftShader driver shipped beside it, Vulkan on
// the CPU, before the app's first instance: a phone always has Vulkan. Every command the app
// calls is a System V wrapper (guest_vk.inc, tools/gen_vk_thunks.py) of the driver's, found
// through the table of the instance or device the dispatchable handle belongs to (the loader's
// dispatch pointer, the first word of every dispatchable object, keys the tables).
//
// Written here, where Win.apk answers as an Android device's libvulkan would:
//  - versions and extensions: the instance and devices are reported at Vulkan 1.3 at most
//    (Android 14's loader); the host's extensions that name Windows' window system, displays or
//    Win32 handles are left out, and VK_KHR_surface, VK_KHR_android_surface,
//    VK_KHR_get_surface_capabilities2 and VK_KHR_swapchain are offered, emulated below;
//  - the app's allocation callbacks are not handed to the driver (it would call System V code
//    with its own convention): null, which the API always allows; its debug callbacks are, each
//    through a host trampoline that calls the app's function;
//  - the window system: a surface is the runtime's native window (native_window.h); a swapchain
//    is images of the device's own, and vkQueuePresentKHR copies the presented one to a buffer,
//    waits for it and shows it as the guest layer (guest_frame_present), as an EGL window
//    surface's frames are shown (gl/guest_gl.cpp); FIFO returns at the display's rate
//    (swap_pacing.h), MAILBOX at once. A window resized since the swapchain was made makes
//    present answer VK_ERROR_OUT_OF_DATE_KHR, as a device's does, so the app makes a new one.
// What differs from a device: a present costs a copy to the CPU; no AHardwareBuffer import or
// export (VK_ANDROID_external_memory_android_hardware_buffer is not offered); no shared
// presentable images, display timing or HDR metadata; surfaces report the identity transform.

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_android.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "guest_vk.h"
#include "native_window.h"
#include "swap_pacing.h"
#include "winapk/arm64_guest.h"
#include "winapk/guest_abi.h"

namespace winapk::host {

namespace {

// ---- the loader --------------------------------------------------------------------------------

std::wstring exe_dir() {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return {};
    std::wstring path(exe, n);
    path.resize(path.find_last_of(L"\\/") + 1);
    return path;
}

PFN_vkGetInstanceProcAddr loader_gipa() {
    static const PFN_vkGetInstanceProcAddr gipa = [] {
        std::wstring path = exe_dir() + L"vulkan-1.dll";
        HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m) m = LoadLibraryW(L"vulkan-1.dll");  // the system's, in a build run from its tree
        auto p = m ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(m, "vkGetInstanceProcAddr")) : nullptr;
        if (!p) std::fprintf(stderr, "[vulkan] no Vulkan loader (vulkan-1.dll): an app gets no Vulkan instance\n");
        return p;
    }();
    return gipa;
}

template <typename F>
F global(const char* name) {
    PFN_vkGetInstanceProcAddr gipa = loader_gipa();
    return gipa ? reinterpret_cast<F>(gipa(nullptr, name)) : nullptr;
}

// Whether any driver the loader finds gives a device; if none does, the loader is told of the
// SwiftShader driver beside the runtime (VK_DRIVER_FILES, and VK_ICD_FILENAMES for older loaders),
// unless the environment already names drivers. Once, before the app's first instance.
void choose_driver() {
    static const bool chosen = [] {
        if (GetEnvironmentVariableW(L"VK_DRIVER_FILES", nullptr, 0) || GetEnvironmentVariableW(L"VK_ICD_FILENAMES", nullptr, 0)) {
            return true;
        }
        auto create = global<PFN_vkCreateInstance>("vkCreateInstance");
        if (!create) return true;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        VkInstance probe = VK_NULL_HANDLE;
        uint32_t devices = 0;
        if (create(&info, nullptr, &probe) == VK_SUCCESS) {
            PFN_vkGetInstanceProcAddr gipa = loader_gipa();
            auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(probe, "vkEnumeratePhysicalDevices"));
            auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(probe, "vkDestroyInstance"));
            if (enumerate) enumerate(probe, &devices, nullptr);
            if (destroy) destroy(probe, nullptr);
        }
        if (devices > 0) {
            std::fprintf(stderr, "[vulkan] the PC's Vulkan driver: %u device(s)\n", devices);
            return true;
        }
        const std::wstring json = exe_dir() + L"vk_swiftshader_icd.json";
        if (GetFileAttributesW(json.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::fprintf(stderr, "[vulkan] no Vulkan device and no SwiftShader beside the runtime\n");
            return true;
        }
        SetEnvironmentVariableW(L"VK_DRIVER_FILES", json.c_str());
        SetEnvironmentVariableW(L"VK_ICD_FILENAMES", json.c_str());
        std::fprintf(stderr, "[vulkan] no GPU driver offers Vulkan: SwiftShader (Vulkan on the CPU)\n");
        return true;
    }();
    (void)chosen;
}

constexpr uint32_t kMaxApi = VK_API_VERSION_1_3;  // Android 14's loader

// ---- dispatch tables ----------------------------------------------------------------------------

#include "vk/guest_vk_table.inc"

// The tables by the loader's dispatch pointer, read without a lock on every call (a handful of
// instances and devices in a process's life; entries are never moved or freed while in use).
struct Slot {
    std::atomic<const void*> key{nullptr};
    std::atomic<VkTable*> table{nullptr};
};
constexpr int kSlots = 64;
Slot g_slots[kSlots];
std::mutex g_slots_lock;
const VkTable g_no_table{};

const void* key_of(const void* handle) { return handle ? *static_cast<const void* const*>(handle) : nullptr; }

template <typename H>
const VkTable& table_of(H handle) {
    const void* key = key_of(reinterpret_cast<const void*>(handle));
    for (Slot& s : g_slots) {
        if (s.key.load(std::memory_order_acquire) == key) {
            if (VkTable* t = s.table.load(std::memory_order_acquire)) return *t;
        }
    }
    return g_no_table;
}
void register_key(const void* handle, VkTable* table) {
    const void* key = key_of(handle);
    if (!key) return;
    std::lock_guard<std::mutex> hold(g_slots_lock);
    for (Slot& s : g_slots) {
        if (s.key.load() == key) {
            s.table.store(table, std::memory_order_release);
            return;
        }
    }
    for (Slot& s : g_slots) {
        if (!s.key.load()) {
            s.table.store(table, std::memory_order_release);
            s.key.store(key, std::memory_order_release);
            return;
        }
    }
    std::fprintf(stderr, "[vulkan] more than %d instances and devices at once\n", kSlots);
}
void unregister_table(const VkTable* table) {
    std::lock_guard<std::mutex> hold(g_slots_lock);
    for (Slot& s : g_slots) {
        if (s.table.load() == table) {
            s.key.store(nullptr, std::memory_order_release);
            s.table.store(nullptr, std::memory_order_release);
        }
    }
}

struct Instance {
    VkInstance handle = VK_NULL_HANDLE;
    VkTable table;
};
struct Device {
    VkDevice handle = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkTable table;
    std::mutex queues_lock;
    std::map<VkQueue, uint32_t> queue_family;  // every queue the app got, and its family
};

std::mutex g_objects_lock;
std::map<VkInstance, std::unique_ptr<Instance>> g_instances;
std::map<VkDevice, std::unique_ptr<Device>> g_devices;

Device* device_of(VkDevice d) {
    std::lock_guard<std::mutex> hold(g_objects_lock);
    auto it = g_devices.find(d);
    return it == g_devices.end() ? nullptr : it->second.get();
}

// ---- extensions ----------------------------------------------------------------------------------

// Host extensions an Android device does not have: Windows' window system and handles, displays.
bool hidden_extension(const char* name) {
    static const char* const kHidden[] = {
        "VK_KHR_win32_surface", "VK_KHR_display", "VK_KHR_get_display_properties2", "VK_EXT_direct_mode_display",
        "VK_EXT_display_surface_counter", "VK_EXT_headless_surface", "VK_EXT_swapchain_colorspace",
        "VK_EXT_surface_maintenance1", "VK_KHR_surface_protected_capabilities", "VK_LUNARG_direct_driver_loading",
        "VK_KHR_portability_enumeration", "VK_EXT_acquire_drm_display", "VK_NV_acquire_winrt_display",
        "VK_EXT_full_screen_exclusive", "VK_KHR_present_id", "VK_KHR_present_wait", "VK_EXT_swapchain_maintenance1",
        "VK_KHR_incremental_present", "VK_GOOGLE_display_timing", "VK_EXT_hdr_metadata",
        "VK_KHR_shared_presentable_image", "VK_AMD_display_native_hdr", "VK_EXT_display_control",
        "VK_KHR_display_swapchain", "VK_EXT_device_memory_report", "VK_KHR_win32_keyed_mutex",
        "VK_NV_win32_keyed_mutex", "VK_EXT_present_mode_fifo_latest_ready", "VK_NV_low_latency2",
        "VK_KHR_present_id2", "VK_KHR_present_wait2", "VK_KHR_swapchain_mutable_format",
        "VK_EXT_metal_objects", "VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32",
        "VK_KHR_external_fence_win32", "VK_NV_external_memory_win32",
    };
    for (const char* h : kHidden) {
        if (std::strcmp(name, h) == 0) return true;
    }
    return std::strstr(name, "win32") != nullptr;
}

// The window system's extensions this file implements, with Android's spec versions.
const VkExtensionProperties kInstanceWsi[] = {
    {"VK_KHR_surface", 25}, {"VK_KHR_android_surface", 6}, {"VK_KHR_get_surface_capabilities2", 1}};
const VkExtensionProperties kDeviceWsi[] = {{"VK_KHR_swapchain", 70}};

bool is_wsi(const char* name) {
    for (const auto& e : kInstanceWsi) if (std::strcmp(name, e.extensionName) == 0) return true;
    for (const auto& e : kDeviceWsi) if (std::strcmp(name, e.extensionName) == 0) return true;
    return false;
}

VkResult copy_out(const std::vector<VkExtensionProperties>& list, uint32_t* count, VkExtensionProperties* out) {
    if (!out) {
        *count = static_cast<uint32_t>(list.size());
        return VK_SUCCESS;
    }
    const uint32_t n = std::min<uint32_t>(*count, static_cast<uint32_t>(list.size()));
    std::copy(list.begin(), list.begin() + n, out);
    *count = n;
    return n < list.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

std::vector<VkExtensionProperties> host_instance_extensions() {
    std::vector<VkExtensionProperties> list;
    auto enumerate = global<PFN_vkEnumerateInstanceExtensionProperties>("vkEnumerateInstanceExtensionProperties");
    uint32_t n = 0;
    if (enumerate && enumerate(nullptr, &n, nullptr) == VK_SUCCESS) {
        list.resize(n);
        if (enumerate(nullptr, &n, list.data()) < 0) n = 0;
        list.resize(n);
    }
    return list;
}

// ---- debug callbacks -------------------------------------------------------------------------------

struct GuestMessenger {
    PFN_vkDebugUtilsMessengerCallbackEXT guest = nullptr;  // System V (the app's)
    void* user = nullptr;
};
struct GuestReport {
    PFN_vkDebugReportCallbackEXT guest = nullptr;
    void* user = nullptr;
};
using GuestUtilsFn = VkBool32(WINAPK_GUEST_ABI*)(VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
                                                 const VkDebugUtilsMessengerCallbackDataEXT*, void*);
using GuestReportFn = VkBool32(WINAPK_GUEST_ABI*)(VkDebugReportFlagsEXT, VkDebugReportObjectTypeEXT, uint64_t, size_t,
                                                  int32_t, const char*, const char*, void*);

VKAPI_ATTR VkBool32 VKAPI_CALL utils_trampoline(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                VkDebugUtilsMessageTypeFlagsEXT types,
                                                const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    auto* m = static_cast<GuestMessenger*>(user);
    auto fn = reinterpret_cast<GuestUtilsFn>(arm64::host_callable(reinterpret_cast<void*>(m->guest)));
    return fn ? fn(severity, types, data, m->user) : VK_FALSE;
}
VKAPI_ATTR VkBool32 VKAPI_CALL report_trampoline(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT type,
                                                 uint64_t object, size_t location, int32_t code, const char* prefix,
                                                 const char* message, void* user) {
    auto* r = static_cast<GuestReport*>(user);
    auto fn = reinterpret_cast<GuestReportFn>(arm64::host_callable(reinterpret_cast<void*>(r->guest)));
    return fn ? fn(flags, type, object, location, code, prefix, message, r->user) : VK_FALSE;
}

// The records live as long as the process: a messenger in vkCreateInstance's chain is used until
// the instance goes, and a few bytes per messenger are not worth tracking.
GuestMessenger* keep(const GuestMessenger& m) { return new GuestMessenger(m); }
GuestReport* keep(const GuestReport& r) { return new GuestReport(r); }

std::mutex g_messengers_lock;
std::map<uint64_t, GuestMessenger*> g_messengers;

// ---- surfaces and swapchains -------------------------------------------------------------------

struct Surface {
    void* window = nullptr;
};
Surface* surface_of(VkSurfaceKHR s) { return reinterpret_cast<Surface*>(s); }

// The window's buffer size, as eglCreateWindowSurface gives it (native_window.h).
VkExtent2D window_extent(const Surface* s) {
    int w = 0, h = 0;
    if (!s || !native_window_buffer_size(s->window, &w, &h) || w <= 0 || h <= 0) w = 1280, h = 720;
    return {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
}

struct Swapchain {
    Device* device = nullptr;
    Surface* surface = nullptr;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkExtent2D extent{};
    VkExtent2D window_at_creation{};
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage> images;
    std::vector<VkDeviceMemory> memory;
    std::vector<bool> acquired;
    uint32_t next = 0;
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    // One command buffer per queue family a present came on.
    std::map<uint32_t, std::pair<VkCommandPool, VkCommandBuffer>> commands;
    VkFence fence = VK_NULL_HANDLE;
    std::vector<uint32_t> argb;
    SwapPacer pacer;
    std::mutex lock;
};
Swapchain* swapchain_of(VkSwapchainKHR s) { return reinterpret_cast<Swapchain*>(s); }

int32_t memory_type(const Device* d, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice = 0) {
    VkPhysicalDeviceMemoryProperties props{};
    d->table.vkGetPhysicalDeviceMemoryProperties(d->physical, &props);
    int32_t found = -1;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if (!(bits & (1u << i)) || (props.memoryTypes[i].propertyFlags & want) != want) continue;
        if ((props.memoryTypes[i].propertyFlags & nice) == nice) return static_cast<int32_t>(i);
        if (found < 0) found = static_cast<int32_t>(i);
    }
    return found;
}

void destroy_swapchain(Swapchain* sc) {
    if (!sc) return;
    const VkTable& t = sc->device->table;
    VkDevice dev = sc->device->handle;
    if (sc->fence) {
        t.vkWaitForFences(dev, 1, &sc->fence, VK_TRUE, UINT64_MAX);
        t.vkDestroyFence(dev, sc->fence, nullptr);
    }
    for (auto& [family, c] : sc->commands) t.vkDestroyCommandPool(dev, c.first, nullptr);
    if (sc->mapped) t.vkUnmapMemory(dev, sc->readback_memory);
    if (sc->readback) t.vkDestroyBuffer(dev, sc->readback, nullptr);
    if (sc->readback_memory) t.vkFreeMemory(dev, sc->readback_memory, nullptr);
    for (VkImage i : sc->images) t.vkDestroyImage(dev, i, nullptr);
    for (VkDeviceMemory m : sc->memory) t.vkFreeMemory(dev, m, nullptr);
    delete sc;
}

void log_once(const char* what) {
    static std::mutex lock;
    static std::set<std::string> said;
    std::lock_guard<std::mutex> hold(lock);
    if (said.insert(what).second) {
        std::fprintf(stderr, "[vulkan] %s\n", what);
        std::fflush(stderr);
    }
}

}  // namespace

// ---- the hand-written commands -------------------------------------------------------------------

namespace {

WINAPK_GUEST_ABI VkResult guest_vkEnumerateInstanceVersion(uint32_t* version) {
    if (!version) return VK_ERROR_INITIALIZATION_FAILED;
    *version = VK_API_VERSION_1_0;
    auto host = global<PFN_vkEnumerateInstanceVersion>("vkEnumerateInstanceVersion");
    if (host && host(version) == VK_SUCCESS) *version = std::min(*version, kMaxApi);
    return VK_SUCCESS;
}

WINAPK_GUEST_ABI VkResult guest_vkEnumerateInstanceExtensionProperties(const char* layer, uint32_t* count,
                                                                        VkExtensionProperties* out) {
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
    choose_driver();
    std::vector<VkExtensionProperties> list;
    for (const auto& e : host_instance_extensions()) {
        if (!hidden_extension(e.extensionName) && !is_wsi(e.extensionName)) list.push_back(e);
    }
    if (loader_gipa()) list.insert(list.end(), std::begin(kInstanceWsi), std::end(kInstanceWsi));
    return copy_out(list, count, out);
}

// No layers: a device's libvulkan lists only the layers an app ships (debuggable apps), and the
// PC's (validation, overlays) are not the app's.
WINAPK_GUEST_ABI VkResult guest_vkEnumerateInstanceLayerProperties(uint32_t* count, VkLayerProperties*) {
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    *count = 0;
    return VK_SUCCESS;
}
WINAPK_GUEST_ABI VkResult guest_vkEnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t* count, VkLayerProperties*) {
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    *count = 0;
    return VK_SUCCESS;
}

// The instance chain's structs the loader reads, copied so their callbacks can be replaced; others
// (validation features and flags, layer settings) are passed as they are.
struct ChainCopy {
    std::vector<std::unique_ptr<uint8_t[]>> blocks;
    const void* head = nullptr;
};
ChainCopy copy_instance_chain(const void* next) {
    ChainCopy c;
    VkBaseOutStructure* tail = nullptr;
    for (auto* in = static_cast<const VkBaseInStructure*>(next); in; in = in->pNext) {
        size_t size = 0;
        switch (in->sType) {
            case VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT: size = sizeof(VkDebugUtilsMessengerCreateInfoEXT); break;
            case VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT: size = sizeof(VkDebugReportCallbackCreateInfoEXT); break;
            case VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT: size = sizeof(VkValidationFeaturesEXT); break;
            case VK_STRUCTURE_TYPE_VALIDATION_FLAGS_EXT: size = sizeof(VkValidationFlagsEXT); break;
            case VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT: size = sizeof(VkLayerSettingsCreateInfoEXT); break;
            default: continue;  // nothing else belongs in an instance's chain on Android
        }
        auto block = std::make_unique<uint8_t[]>(size);
        std::memcpy(block.get(), in, size);
        auto* out = reinterpret_cast<VkBaseOutStructure*>(block.get());
        out->pNext = nullptr;
        if (in->sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT) {
            auto* m = reinterpret_cast<VkDebugUtilsMessengerCreateInfoEXT*>(out);
            m->pUserData = keep(GuestMessenger{m->pfnUserCallback, m->pUserData});
            m->pfnUserCallback = utils_trampoline;
        } else if (in->sType == VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT) {
            auto* r = reinterpret_cast<VkDebugReportCallbackCreateInfoEXT*>(out);
            r->pUserData = keep(GuestReport{r->pfnCallback, r->pUserData});
            r->pfnCallback = report_trampoline;
        }
        if (tail) tail->pNext = out;
        else c.head = out;
        tail = out;
        c.blocks.push_back(std::move(block));
    }
    return c;
}

WINAPK_GUEST_ABI VkResult guest_vkCreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks*,
                                                 VkInstance* out) {
    if (!info || !out) return VK_ERROR_INITIALIZATION_FAILED;
    choose_driver();
    auto create = global<PFN_vkCreateInstance>("vkCreateInstance");
    if (!create) return VK_ERROR_INCOMPATIBLE_DRIVER;
    // The extensions: ours are not the host's (VK_KHR_surface and get_surface_capabilities2 go to
    // the host too when it has them, harmless and what the loader expects of an instance with a
    // surface); one the host does not have fails as it would on a device.
    std::vector<VkExtensionProperties> host = host_instance_extensions();
    auto host_has = [&](const char* n) {
        return std::any_of(host.begin(), host.end(), [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, n) == 0; });
    };
    std::vector<const char*> enabled;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        const char* n = info->ppEnabledExtensionNames[i];
        if (is_wsi(n)) {
            if (host_has(n)) enabled.push_back(n);
            continue;
        }
        if (hidden_extension(n) || !host_has(n)) return VK_ERROR_EXTENSION_NOT_PRESENT;
        enabled.push_back(n);
    }
    ChainCopy chain = copy_instance_chain(info->pNext);
    VkInstanceCreateInfo host_info = *info;
    host_info.pNext = chain.head;
    host_info.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    host_info.ppEnabledExtensionNames = enabled.data();
    host_info.enabledLayerCount = 0;  // none listed, so none asked for
    host_info.ppEnabledLayerNames = nullptr;
    VkApplicationInfo app{};
    if (info->pApplicationInfo) {
        app = *info->pApplicationInfo;
        if (app.apiVersion > kMaxApi) app.apiVersion = kMaxApi;
        host_info.pApplicationInfo = &app;
    }
    VkInstance instance = VK_NULL_HANDLE;
    const VkResult r = create(&host_info, nullptr, &instance);
    if (r != VK_SUCCESS) return r;
    auto rec = std::make_unique<Instance>();
    rec->handle = instance;
    vk_fill_instance(rec->table, instance, loader_gipa());
    register_key(instance, &rec->table);
    {
        std::lock_guard<std::mutex> hold(g_objects_lock);
        g_instances[instance] = std::move(rec);
    }
    *out = instance;
    return VK_SUCCESS;
}

WINAPK_GUEST_ABI void guest_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks*) {
    if (!instance) return;
    std::unique_ptr<Instance> rec;
    {
        std::lock_guard<std::mutex> hold(g_objects_lock);
        auto it = g_instances.find(instance);
        if (it == g_instances.end()) return;
        rec = std::move(it->second);
        g_instances.erase(it);
    }
    if (rec->table.vkDestroyInstance) rec->table.vkDestroyInstance(instance, nullptr);
    unregister_table(&rec->table);
}

WINAPK_GUEST_ABI VkResult guest_vkEnumeratePhysicalDevices(VkInstance instance, uint32_t* count, VkPhysicalDevice* out) {
    const VkTable& t = table_of(instance);
    if (!t.vkEnumeratePhysicalDevices) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult r = t.vkEnumeratePhysicalDevices(instance, count, out);
    if (out && r >= 0) {
        for (uint32_t i = 0; i < *count; ++i) register_key(out[i], const_cast<VkTable*>(&t));
    }
    return r;
}
VkResult physical_groups(PFN_vkEnumeratePhysicalDeviceGroups fn, VkInstance instance, uint32_t* count,
                         VkPhysicalDeviceGroupProperties* out) {
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult r = fn(instance, count, out);
    if (out && r >= 0) {
        for (uint32_t i = 0; i < *count; ++i) {
            for (uint32_t j = 0; j < out[i].physicalDeviceCount; ++j) {
                register_key(out[i].physicalDevices[j], const_cast<VkTable*>(&table_of(instance)));
            }
        }
    }
    return r;
}
WINAPK_GUEST_ABI VkResult guest_vkEnumeratePhysicalDeviceGroups(VkInstance i, uint32_t* n, VkPhysicalDeviceGroupProperties* o) {
    return physical_groups(table_of(i).vkEnumeratePhysicalDeviceGroups, i, n, o);
}
WINAPK_GUEST_ABI VkResult guest_vkEnumeratePhysicalDeviceGroupsKHR(VkInstance i, uint32_t* n, VkPhysicalDeviceGroupProperties* o) {
    return physical_groups(table_of(i).vkEnumeratePhysicalDeviceGroupsKHR, i, n, o);
}

// The device's version, at most 1.3, as Android 14's loader reports it.
WINAPK_GUEST_ABI void guest_vkGetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties* p) {
    if (auto f = table_of(pd).vkGetPhysicalDeviceProperties) f(pd, p);
    if (p && p->apiVersion > kMaxApi) p->apiVersion = kMaxApi;
}
WINAPK_GUEST_ABI void guest_vkGetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2* p) {
    if (auto f = table_of(pd).vkGetPhysicalDeviceProperties2) f(pd, p);
    if (p && p->properties.apiVersion > kMaxApi) p->properties.apiVersion = kMaxApi;
}
WINAPK_GUEST_ABI void guest_vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice pd, VkPhysicalDeviceProperties2* p) {
    if (auto f = table_of(pd).vkGetPhysicalDeviceProperties2KHR) f(pd, p);
    if (p && p->properties.apiVersion > kMaxApi) p->properties.apiVersion = kMaxApi;
}

std::vector<VkExtensionProperties> host_device_extensions(VkPhysicalDevice pd) {
    std::vector<VkExtensionProperties> list;
    auto f = table_of(pd).vkEnumerateDeviceExtensionProperties;
    uint32_t n = 0;
    if (f && f(pd, nullptr, &n, nullptr) == VK_SUCCESS) {
        list.resize(n);
        if (f(pd, nullptr, &n, list.data()) < 0) n = 0;
        list.resize(n);
    }
    return list;
}

WINAPK_GUEST_ABI VkResult guest_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char* layer,
                                                                      uint32_t* count, VkExtensionProperties* out) {
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
    std::vector<VkExtensionProperties> list;
    for (const auto& e : host_device_extensions(pd)) {
        if (!hidden_extension(e.extensionName) && !is_wsi(e.extensionName) &&
            std::strcmp(e.extensionName, "VK_ANDROID_external_memory_android_hardware_buffer") != 0) {
            list.push_back(e);
        }
    }
    list.insert(list.end(), std::begin(kDeviceWsi), std::end(kDeviceWsi));
    return copy_out(list, count, out);
}

WINAPK_GUEST_ABI VkResult guest_vkCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* info,
                                               const VkAllocationCallbacks*, VkDevice* out) {
    const VkTable& it = table_of(pd);
    if (!info || !out || !it.vkCreateDevice) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<VkExtensionProperties> host = host_device_extensions(pd);
    auto host_has = [&](const char* n) {
        return std::any_of(host.begin(), host.end(), [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, n) == 0; });
    };
    std::vector<const char*> enabled;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        const char* n = info->ppEnabledExtensionNames[i];
        if (is_wsi(n)) {
            // VK_KHR_swapchain goes to the host too: PRESENT_SRC_KHR, the layout the app hands its
            // images over in, is that extension's.
            if (host_has(n)) enabled.push_back(n);
            continue;
        }
        if (hidden_extension(n) || !host_has(n)) return VK_ERROR_EXTENSION_NOT_PRESENT;
        enabled.push_back(n);
    }
    VkDeviceCreateInfo host_info = *info;
    host_info.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    host_info.ppEnabledExtensionNames = enabled.data();
    host_info.enabledLayerCount = 0;
    host_info.ppEnabledLayerNames = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    const VkResult r = it.vkCreateDevice(pd, &host_info, nullptr, &device);
    if (r != VK_SUCCESS) return r;
    auto rec = std::make_unique<Device>();
    rec->handle = device;
    rec->physical = pd;
    rec->table = it;
    vk_fill_device(rec->table, device, it.vkGetDeviceProcAddr);
    register_key(device, &rec->table);
    {
        std::lock_guard<std::mutex> hold(g_objects_lock);
        g_devices[device] = std::move(rec);
    }
    *out = device;
    return VK_SUCCESS;
}

WINAPK_GUEST_ABI void guest_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks*) {
    if (!device) return;
    std::unique_ptr<Device> rec;
    {
        std::lock_guard<std::mutex> hold(g_objects_lock);
        auto it = g_devices.find(device);
        if (it == g_devices.end()) return;
        rec = std::move(it->second);
        g_devices.erase(it);
    }
    if (rec->table.vkDestroyDevice) rec->table.vkDestroyDevice(device, nullptr);
    unregister_table(&rec->table);
}

void note_queue(VkDevice device, VkQueue queue, uint32_t family) {
    Device* d = device_of(device);
    if (!d || !queue) return;
    if (key_of(queue) != key_of(device)) register_key(queue, &d->table);
    std::lock_guard<std::mutex> hold(d->queues_lock);
    d->queue_family[queue] = family;
}
WINAPK_GUEST_ABI void guest_vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    if (auto f = table_of(device).vkGetDeviceQueue) f(device, family, index, queue);
    if (queue) note_queue(device, *queue, family);
}
WINAPK_GUEST_ABI void guest_vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    if (auto f = table_of(device).vkGetDeviceQueue2) f(device, info, queue);
    if (queue && info) note_queue(device, *queue, info->queueFamilyIndex);
}
WINAPK_GUEST_ABI VkResult guest_vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* info,
                                                         VkCommandBuffer* out) {
    auto f = table_of(device).vkAllocateCommandBuffers;
    if (!f) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult r = f(device, info, out);
    if (r == VK_SUCCESS && info && info->commandBufferCount && key_of(out[0]) != key_of(device)) {
        if (Device* d = device_of(device)) {
            for (uint32_t i = 0; i < info->commandBufferCount; ++i) register_key(out[i], &d->table);
        }
    }
    return r;
}

WINAPK_GUEST_ABI VkResult guest_vkCreateDebugUtilsMessengerEXT(VkInstance instance,
                                                               const VkDebugUtilsMessengerCreateInfoEXT* info,
                                                               const VkAllocationCallbacks*,
                                                               VkDebugUtilsMessengerEXT* out) {
    auto f = table_of(instance).vkCreateDebugUtilsMessengerEXT;
    if (!f || !info) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkDebugUtilsMessengerCreateInfoEXT copy = *info;
    GuestMessenger* m = keep(GuestMessenger{info->pfnUserCallback, info->pUserData});
    copy.pfnUserCallback = utils_trampoline;
    copy.pUserData = m;
    return f(instance, &copy, nullptr, out);
}
WINAPK_GUEST_ABI void guest_vkDestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT m,
                                                            const VkAllocationCallbacks*) {
    if (auto f = table_of(instance).vkDestroyDebugUtilsMessengerEXT) f(instance, m, nullptr);
}
WINAPK_GUEST_ABI VkResult guest_vkCreateDebugReportCallbackEXT(VkInstance instance,
                                                               const VkDebugReportCallbackCreateInfoEXT* info,
                                                               const VkAllocationCallbacks*,
                                                               VkDebugReportCallbackEXT* out) {
    auto f = table_of(instance).vkCreateDebugReportCallbackEXT;
    if (!f || !info) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkDebugReportCallbackCreateInfoEXT copy = *info;
    copy.pUserData = keep(GuestReport{info->pfnCallback, info->pUserData});
    copy.pfnCallback = report_trampoline;
    return f(instance, &copy, nullptr, out);
}
WINAPK_GUEST_ABI void guest_vkDestroyDebugReportCallbackEXT(VkInstance instance, VkDebugReportCallbackEXT c,
                                                            const VkAllocationCallbacks*) {
    if (auto f = table_of(instance).vkDestroyDebugReportCallbackEXT) f(instance, c, nullptr);
}

// ---- VK_KHR_android_surface, VK_KHR_surface, VK_KHR_get_surface_capabilities2 ----------------------

WINAPK_GUEST_ABI VkResult guest_vkCreateAndroidSurfaceKHR(VkInstance, const VkAndroidSurfaceCreateInfoKHR* info,
                                                          const VkAllocationCallbacks*, VkSurfaceKHR* out) {
    if (!info || !out) return VK_ERROR_INITIALIZATION_FAILED;
    if (info->window != native_window_handle()) {
        log_once("vkCreateAndroidSurfaceKHR on a window the runtime did not make (an ASurfaceTexture's?)");
        return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
    }
    *out = reinterpret_cast<VkSurfaceKHR>(new Surface{info->window});
    std::fprintf(stderr, "[vulkan] the app made a surface on its window\n");
    return VK_SUCCESS;
}
WINAPK_GUEST_ABI void guest_vkDestroySurfaceKHR(VkInstance, VkSurfaceKHR surface, const VkAllocationCallbacks*) {
    delete surface_of(surface);
}
WINAPK_GUEST_ABI VkResult guest_vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice pd, uint32_t family, VkSurfaceKHR,
                                                                     VkBool32* supported) {
    if (!supported) return VK_ERROR_INITIALIZATION_FAILED;
    // Any queue that can copy can present (the present is a copy); every family can.
    uint32_t n = 0;
    table_of(pd).vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, nullptr);
    *supported = family < n ? VK_TRUE : VK_FALSE;
    return VK_SUCCESS;
}

void fill_capabilities(const Surface* s, VkSurfaceCapabilitiesKHR* c) {
    *c = {};
    c->minImageCount = 2;
    c->maxImageCount = 8;
    c->currentExtent = window_extent(s);
    c->minImageExtent = {1, 1};
    c->maxImageExtent = {4096, 4096};
    c->maxImageArrayLayers = 1;
    c->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
}
WINAPK_GUEST_ABI VkResult guest_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice, VkSurfaceKHR surface,
                                                                          VkSurfaceCapabilitiesKHR* c) {
    if (!c || !surface) return VK_ERROR_SURFACE_LOST_KHR;
    fill_capabilities(surface_of(surface), c);
    return VK_SUCCESS;
}
WINAPK_GUEST_ABI VkResult guest_vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice,
                                                                           const VkPhysicalDeviceSurfaceInfo2KHR* info,
                                                                           VkSurfaceCapabilities2KHR* c) {
    if (!info || !c || !info->surface) return VK_ERROR_SURFACE_LOST_KHR;
    fill_capabilities(surface_of(info->surface), &c->surfaceCapabilities);
    return VK_SUCCESS;
}

// What the readback turns into the window's pixels: RGBA, 8 bits a channel (an Android
// surface's usual formats).
const VkSurfaceFormatKHR kFormats[] = {{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                                       {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
WINAPK_GUEST_ABI VkResult guest_vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t* count,
                                                                     VkSurfaceFormatKHR* out) {
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    const uint32_t total = static_cast<uint32_t>(std::size(kFormats));
    if (!out) {
        *count = total;
        return VK_SUCCESS;
    }
    const uint32_t n = std::min(*count, total);
    std::copy(kFormats, kFormats + n, out);
    *count = n;
    return n < total ? VK_INCOMPLETE : VK_SUCCESS;
}
WINAPK_GUEST_ABI VkResult guest_vkGetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR*,
                                                                      uint32_t* count, VkSurfaceFormat2KHR* out) {
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    const uint32_t total = static_cast<uint32_t>(std::size(kFormats));
    if (!out) {
        *count = total;
        return VK_SUCCESS;
    }
    const uint32_t n = std::min(*count, total);
    for (uint32_t i = 0; i < n; ++i) out[i].surfaceFormat = kFormats[i];
    *count = n;
    return n < total ? VK_INCOMPLETE : VK_SUCCESS;
}
WINAPK_GUEST_ABI VkResult guest_vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t* count,
                                                                          VkPresentModeKHR* out) {
    static const VkPresentModeKHR kModes[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR};
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    if (!out) {
        *count = 2;
        return VK_SUCCESS;
    }
    const uint32_t n = std::min<uint32_t>(*count, 2);
    std::copy(kModes, kModes + n, out);
    *count = n;
    return n < 2 ? VK_INCOMPLETE : VK_SUCCESS;
}
WINAPK_GUEST_ABI VkResult guest_vkGetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice, VkSurfaceKHR surface,
                                                                        uint32_t* count, VkRect2D* rects) {
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    if (!rects) {
        *count = 1;
        return VK_SUCCESS;
    }
    if (*count < 1) return VK_INCOMPLETE;
    rects[0] = {{0, 0}, window_extent(surface_of(surface))};
    *count = 1;
    return VK_SUCCESS;
}
WINAPK_GUEST_ABI VkResult guest_vkGetDeviceGroupPresentCapabilitiesKHR(VkDevice, VkDeviceGroupPresentCapabilitiesKHR* c) {
    if (!c) return VK_ERROR_INITIALIZATION_FAILED;
    std::memset(c->presentMask, 0, sizeof c->presentMask);
    c->presentMask[0] = 1;
    c->modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}
WINAPK_GUEST_ABI VkResult guest_vkGetDeviceGroupSurfacePresentModesKHR(VkDevice, VkSurfaceKHR,
                                                                      VkDeviceGroupPresentModeFlagsKHR* modes) {
    if (!modes) return VK_ERROR_INITIALIZATION_FAILED;
    *modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}

// ---- VK_KHR_swapchain --------------------------------------------------------------------------

WINAPK_GUEST_ABI VkResult guest_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* info,
                                                     const VkAllocationCallbacks*, VkSwapchainKHR* out) {
    Device* d = device_of(device);
    if (!d || !info || !out || !info->surface) return VK_ERROR_INITIALIZATION_FAILED;
    if (info->imageFormat != VK_FORMAT_R8G8B8A8_UNORM && info->imageFormat != VK_FORMAT_R8G8B8A8_SRGB) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    const VkTable& t = d->table;
    auto* sc = new Swapchain;
    sc->device = d;
    sc->surface = surface_of(info->surface);
    sc->format = info->imageFormat;
    sc->extent = info->imageExtent;
    sc->window_at_creation = window_extent(sc->surface);
    sc->mode = info->presentMode;
    const uint32_t count = std::clamp<uint32_t>(info->minImageCount, 2, 8);
    VkResult r = VK_SUCCESS;
    for (uint32_t i = 0; i < count && r == VK_SUCCESS; ++i) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = info->imageFormat;
        ii.extent = {info->imageExtent.width, info->imageExtent.height, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = std::max(1u, info->imageArrayLayers);
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = info->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ii.sharingMode = info->imageSharingMode;
        ii.queueFamilyIndexCount = info->queueFamilyIndexCount;
        ii.pQueueFamilyIndices = info->pQueueFamilyIndices;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage image = VK_NULL_HANDLE;
        r = t.vkCreateImage(device, &ii, nullptr, &image);
        if (r != VK_SUCCESS) break;
        sc->images.push_back(image);
        VkMemoryRequirements req{};
        t.vkGetImageMemoryRequirements(device, image, &req);
        const int32_t type = memory_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = static_cast<uint32_t>(type < 0 ? memory_type(d, req.memoryTypeBits, 0) : type);
        VkDeviceMemory mem = VK_NULL_HANDLE;
        r = t.vkAllocateMemory(device, &ai, nullptr, &mem);
        if (r != VK_SUCCESS) break;
        sc->memory.push_back(mem);
        r = t.vkBindImageMemory(device, image, mem, 0);
    }
    // The buffer a present copies into, mapped for good.
    if (r == VK_SUCCESS) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = VkDeviceSize{info->imageExtent.width} * info->imageExtent.height * 4;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        r = t.vkCreateBuffer(device, &bi, nullptr, &sc->readback);
    }
    if (r == VK_SUCCESS) {
        VkMemoryRequirements req{};
        t.vkGetBufferMemoryRequirements(device, sc->readback, &req);
        const int32_t type = memory_type(d, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                         VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (type < 0) {
            r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        } else {
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = static_cast<uint32_t>(type);
            r = t.vkAllocateMemory(device, &ai, nullptr, &sc->readback_memory);
            if (r == VK_SUCCESS) r = t.vkBindBufferMemory(device, sc->readback, sc->readback_memory, 0);
            if (r == VK_SUCCESS) r = t.vkMapMemory(device, sc->readback_memory, 0, VK_WHOLE_SIZE, 0, &sc->mapped);
        }
    }
    if (r == VK_SUCCESS) {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        r = t.vkCreateFence(device, &fi, nullptr, &sc->fence);
    }
    if (r != VK_SUCCESS) {
        destroy_swapchain(sc);
        return r;
    }
    sc->acquired.assign(sc->images.size(), false);
    sc->argb.resize(static_cast<size_t>(sc->extent.width) * sc->extent.height);
    std::fprintf(stderr, "[vulkan] swapchain: %u images of %ux%u, %s\n", static_cast<unsigned>(sc->images.size()),
                 sc->extent.width, sc->extent.height, sc->mode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO" : "MAILBOX");
    *out = reinterpret_cast<VkSwapchainKHR>(sc);
    return VK_SUCCESS;
}

WINAPK_GUEST_ABI void guest_vkDestroySwapchainKHR(VkDevice, VkSwapchainKHR swapchain, const VkAllocationCallbacks*) {
    destroy_swapchain(swapchain_of(swapchain));
}

WINAPK_GUEST_ABI VkResult guest_vkGetSwapchainImagesKHR(VkDevice, VkSwapchainKHR swapchain, uint32_t* count,
                                                        VkImage* images) {
    Swapchain* sc = swapchain_of(swapchain);
    if (!sc || !count) return VK_ERROR_INITIALIZATION_FAILED;
    const uint32_t total = static_cast<uint32_t>(sc->images.size());
    if (!images) {
        *count = total;
        return VK_SUCCESS;
    }
    const uint32_t n = std::min(*count, total);
    std::copy(sc->images.begin(), sc->images.begin() + n, images);
    *count = n;
    return n < total ? VK_INCOMPLETE : VK_SUCCESS;
}

// The queue the app got first, for signalling an acquire's semaphore and fence.
VkQueue any_queue(Device* d) {
    std::lock_guard<std::mutex> hold(d->queues_lock);
    return d->queue_family.empty() ? VK_NULL_HANDLE : d->queue_family.begin()->first;
}

VkResult acquire(VkDevice device, Swapchain* sc, VkSemaphore semaphore, VkFence fence, uint32_t* index) {
    if (!sc || !index) return VK_ERROR_INITIALIZATION_FAILED;
    {
        std::lock_guard<std::mutex> hold(sc->lock);
        const uint32_t n = static_cast<uint32_t>(sc->images.size());
        uint32_t found = n;
        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t i = (sc->next + k) % n;
            if (!sc->acquired[i]) {
                found = i;
                break;
            }
        }
        // Every image is the app's: on a device the acquire would wait for a present that only
        // the app can make, so it fails at once (the app asked for more than it may hold).
        if (found == n) return VK_NOT_READY;
        sc->acquired[found] = true;
        sc->next = (found + 1) % n;
        *index = found;
    }
    // The image is ready at once (its last present has been copied out): signal what the app waits
    // on with an empty submission.
    if (semaphore || fence) {
        VkQueue q = any_queue(sc->device);
        if (!q) return VK_ERROR_DEVICE_LOST;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.signalSemaphoreCount = semaphore ? 1 : 0;
        si.pSignalSemaphores = &semaphore;
        const VkResult r = sc->device->table.vkQueueSubmit(q, 1, &si, fence);
        if (r != VK_SUCCESS) return r;
    }
    (void)device;
    return VK_SUCCESS;
}
WINAPK_GUEST_ABI VkResult guest_vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t,
                                                      VkSemaphore semaphore, VkFence fence, uint32_t* index) {
    return acquire(device, swapchain_of(swapchain), semaphore, fence, index);
}
WINAPK_GUEST_ABI VkResult guest_vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* info,
                                                       uint32_t* index) {
    if (!info) return VK_ERROR_INITIALIZATION_FAILED;
    return acquire(device, swapchain_of(info->swapchain), info->semaphore, info->fence, index);
}

// Copies image `index` out, waiting first for the app's semaphores (once, for the first of a
// present's swapchains), and shows it.
VkResult present_one(VkQueue queue, Swapchain* sc, uint32_t index, const VkSemaphore* waits, uint32_t wait_count) {
    Device* d = sc->device;
    const VkTable& t = d->table;
    uint32_t family = 0;
    {
        std::lock_guard<std::mutex> hold(d->queues_lock);
        auto it = d->queue_family.find(queue);
        if (it != d->queue_family.end()) family = it->second;
    }
    std::lock_guard<std::mutex> hold(sc->lock);
    if (index >= sc->images.size()) return VK_ERROR_OUT_OF_DATE_KHR;
    auto& cmd = sc->commands[family];
    if (!cmd.first) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = family;
        VkResult r = t.vkCreateCommandPool(d->handle, &pi, nullptr, &cmd.first);
        if (r != VK_SUCCESS) return r;
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = cmd.first;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        r = t.vkAllocateCommandBuffers(d->handle, &ai, &cmd.second);
        if (r != VK_SUCCESS) return r;
        if (key_of(cmd.second) != key_of(d->handle)) register_key(cmd.second, &d->table);
    }
    VkCommandBuffer cb = cmd.second;
    VkImage image = sc->images[index];
    t.vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    t.vkBeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier to_copy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_copy.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    to_copy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_copy.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_copy.srcQueueFamilyIndex = to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_copy.image = image;
    to_copy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    t.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &to_copy);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {sc->extent.width, sc->extent.height, 1};
    t.vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc->readback, 1, &region);
    VkImageMemoryBarrier back = to_copy;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = 0;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkBufferMemoryBarrier to_host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    to_host.srcQueueFamilyIndex = to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.buffer = sc->readback;
    to_host.size = VK_WHOLE_SIZE;
    t.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 1,
                           &to_host, 1, &back);
    t.vkEndCommandBuffer(cb);
    std::vector<VkPipelineStageFlags> stages(wait_count, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = wait_count;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VkResult r = t.vkQueueSubmit(queue, 1, &si, sc->fence);
    if (r != VK_SUCCESS) return r;
    r = t.vkWaitForFences(d->handle, 1, &sc->fence, VK_TRUE, UINT64_MAX);
    t.vkResetFences(d->handle, 1, &sc->fence);
    sc->acquired[index] = false;
    if (r != VK_SUCCESS) return r;
    // RGBA bytes to the window's 0xAARRGGBB, opaque (the surface's alpha is OPAQUE or INHERIT).
    const auto* px = static_cast<const uint8_t*>(sc->mapped);
    const size_t n = sc->argb.size();
    for (size_t i = 0; i < n; ++i, px += 4) {
        sc->argb[i] = 0xff000000u | (uint32_t(px[0]) << 16) | (uint32_t(px[1]) << 8) | px[2];
    }
    guest_frame_present(sc->argb.data(), static_cast<int>(sc->extent.width), static_cast<int>(sc->extent.height));
    // FIFO waits for the display as a device's queue makes it wait; MAILBOX does not.
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t wait = sc->pacer.wait_ns(now, sc->mode == VK_PRESENT_MODE_FIFO_KHR ? 1 : 0);
    if (wait > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(wait));
    const VkExtent2D now_window = window_extent(sc->surface);
    if (now_window.width != sc->window_at_creation.width || now_window.height != sc->window_at_creation.height) {
        return VK_ERROR_OUT_OF_DATE_KHR;
    }
    return VK_SUCCESS;
}

WINAPK_GUEST_ABI VkResult guest_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    if (!info) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult worst = VK_SUCCESS;
    for (uint32_t i = 0; i < info->swapchainCount; ++i) {
        Swapchain* sc = swapchain_of(info->pSwapchains[i]);
        VkResult r = sc ? present_one(queue, sc, info->pImageIndices[i], i == 0 ? info->pWaitSemaphores : nullptr,
                                      i == 0 ? info->waitSemaphoreCount : 0)
                        : VK_ERROR_SURFACE_LOST_KHR;
        if (info->pResults) info->pResults[i] = r;
        if (r < 0 && worst >= 0) worst = r;
        else if (r == VK_SUBOPTIMAL_KHR && worst == VK_SUCCESS) worst = r;
    }
    return worst;
}

}  // namespace

// ---- vkGet*ProcAddr and the linker --------------------------------------------------------------

WINAPK_GUEST_ABI PFN_vkVoidFunction guest_vkGetInstanceProcAddr(VkInstance instance, const char* name);
WINAPK_GUEST_ABI PFN_vkVoidFunction guest_vkGetDeviceProcAddr(VkDevice device, const char* name);

namespace {

#include "vk/guest_vk.inc"

void* ours(const char* name) {
    if (std::strcmp(name, "vkCreateAndroidSurfaceKHR") == 0) return reinterpret_cast<void*>(&guest_vkCreateAndroidSurfaceKHR);
    for (const GuestVkCommand& c : kGuestVk) {
        if (std::strcmp(name, c.name) == 0) return c.fn;
    }
    return nullptr;
}

bool global_command(const char* name) {
    return std::strcmp(name, "vkCreateInstance") == 0 || std::strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0 ||
           std::strcmp(name, "vkEnumerateInstanceLayerProperties") == 0 ||
           std::strcmp(name, "vkEnumerateInstanceVersion") == 0 || std::strcmp(name, "vkGetInstanceProcAddr") == 0;
}

// The table member a command name stands for, by asking the loader again: whether the instance
// or device has it (an extension not enabled answers null).
bool host_has(VkInstance instance, VkDevice device, const char* name) {
    if (device) {
        const VkTable& t = table_of(device);
        return t.vkGetDeviceProcAddr && t.vkGetDeviceProcAddr(device, name) != nullptr;
    }
    PFN_vkGetInstanceProcAddr gipa = loader_gipa();
    return gipa && gipa(instance, name) != nullptr;
}

bool wsi_command(const char* name) {
    return std::strstr(name, "Surface") || std::strstr(name, "Swapchain") || std::strstr(name, "AcquireNextImage") ||
           std::strcmp(name, "vkQueuePresentKHR") == 0 || std::strstr(name, "DeviceGroupPresent") ||
           std::strstr(name, "PresentRectangles");
}

}  // namespace

WINAPK_GUEST_ABI PFN_vkVoidFunction guest_vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!name) return nullptr;
    if (global_command(name)) return reinterpret_cast<PFN_vkVoidFunction>(ours(name));
    if (!instance) return nullptr;
    void* fn = ours(name);
    if (!fn) return nullptr;
    if (wsi_command(name) || host_has(instance, VK_NULL_HANDLE, name)) return reinterpret_cast<PFN_vkVoidFunction>(fn);
    return nullptr;
}

WINAPK_GUEST_ABI PFN_vkVoidFunction guest_vkGetDeviceProcAddr(VkDevice device, const char* name) {
    if (!device || !name) return nullptr;
    for (const GuestVkCommand& c : kGuestVk) {
        if (std::strcmp(name, c.name) != 0) continue;
        if (!c.device) return nullptr;  // instance-level: a device answers none
        if (wsi_command(name) || host_has(VK_NULL_HANDLE, device, name)) return reinterpret_cast<PFN_vkVoidFunction>(c.fn);
        return nullptr;
    }
    return nullptr;
}

void* guest_vk_resolve(const char* name) {
    if (std::strncmp(name, "vk", 2) != 0) return nullptr;
    return ours(name);
}

// The Vulkan version a device would report (android.hardware.vulkan.version), 0 when there is no
// Vulkan: what an instance made now finds, at most 1.3.
uint32_t guest_vk_api_version() {
    static const uint32_t version = [] {
        choose_driver();
        auto create = global<PFN_vkCreateInstance>("vkCreateInstance");
        if (!create) return 0u;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = kMaxApi;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        VkInstance instance = VK_NULL_HANDLE;
        if (create(&info, nullptr, &instance) != VK_SUCCESS) return 0u;
        PFN_vkGetInstanceProcAddr gipa = loader_gipa();
        auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
        auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
        auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
        uint32_t best = 0, n = 0;
        if (enumerate && props && enumerate(instance, &n, nullptr) == VK_SUCCESS && n) {
            std::vector<VkPhysicalDevice> pds(n);
            if (enumerate(instance, &n, pds.data()) >= 0) {
                for (uint32_t i = 0; i < n; ++i) {
                    VkPhysicalDeviceProperties p{};
                    props(pds[i], &p);
                    best = std::max(best, std::min(p.apiVersion, kMaxApi));
                }
            }
        }
        if (destroy) destroy(instance, nullptr);
        // AOSP's encoding drops the patch number: major << 22 | minor << 12.
        return best ? (VK_API_VERSION_MAJOR(best) << 22) | (VK_API_VERSION_MINOR(best) << 12) : 0u;
    }();
    return version;
}

}  // namespace winapk::host
