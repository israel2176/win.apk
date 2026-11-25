cmake_minimum_required(VERSION 3.20)
project(aosp_hwui C CXX)
# Soong's Windows host flags (build/soong/cc/config/x86_windows_host.go), for clang on MinGW-w64.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
add_compile_definitions(USE_MINGW WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601 WINVER=0x0601 __USE_MINGW_ANSI_STDIO=1
                        _FILE_OFFSET_BITS=64 NOMINMAX)
# -mno-ms-bitfields: Itanium bitfield layout everywhere (HWUI's DisplayListOp packs its header in
# bitfields; MinGW defaults to MSVC's layout), the same in every library that shares a struct.
add_compile_options(-O2 -fno-strict-aliasing -ffunction-sections -fdata-sections -w -Wno-error
                    -fmessage-length=0 -fdebug-prefix-map=${AOSP}=. -femulated-tls -mno-ms-bitfields)
# libc++ (Soong's) pulls these in transitively; libstdc++ does not.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -include cstring -include cstdint -include cstdlib -include functional -include optional")
set(KHRONOS "${WINAPK}/windows/third_party/khronos")
include_directories(SYSTEM "${CMAKE_CURRENT_SOURCE_DIR}/shim" "${KHRONOS}")
include(${CMAKE_CURRENT_SOURCE_DIR}/modules.cmake)
# What Soong's android variant gets from elsewhere for Skia's GPU parts.
target_compile_definitions(aosp_libskia PRIVATE SK_GANESH SK_GL)
# libicu is the NDK's ICU, unbuilt here: its headers.
target_include_directories(aosp_libharfbuzz_ng PRIVATE "${AOSP}/icu4c/source/common")
# HWUI and libandroidfw are built with their android (device) parts: Soong's global include
# directories for a device module (cc/config/global.go) after their own, C++20 (std::span), the
# device compat header first; patch_src.py: HWUI_DEVICE is HWUI's render thread and GPU pipeline.
foreach(m aosp_libhwui)
  target_include_directories(${m} AFTER PRIVATE "${AOSP}/native_include" "${AOSP}/native_include/private"
      "${AOSP}/libhardware_include" "${AOSP}/system_core/include" "${AOSP}/vulkan-headers/include"
      "${AOSP}/native_libs/nativewindow/include" "${AOSP}/native_libs/nativebase/include"
      "${AOSP}/native_libs/arect/include" "${AOSP}/native_libs/ui/include" "${AOSP}/native_libs/gui/include"
      "${AOSP}/native_libs/nativedisplay/include" "${AOSP}/native_libs/nativedisplay/include-private"
      "${AOSP}/incremental_delivery/incfs/util/include" "${AOSP}/native_libs/nativewindow/include-private"
      "${AOSP}/skia14/include/private" "${AOSP}/skia14" "${AOSP}/androidfw/include")
  set_target_properties(${m} PROPERTIES CXX_STANDARD 20)
  target_include_directories(${m} BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/shim/device")
  target_compile_options(${m} PRIVATE "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/shim/winapk_device_compat.h")
endforeach()
target_compile_definitions(aosp_libhwui PRIVATE HWUI_DEVICE=1 SK_GANESH SK_GL)
