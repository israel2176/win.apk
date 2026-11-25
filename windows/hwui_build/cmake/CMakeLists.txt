cmake_minimum_required(VERSION 3.20)
project(aosp_hwui C CXX)
# libjpeg-turbo's x86-64 SIMD (NASM, Win64 objects).
set(CMAKE_ASM_NASM_OBJECT_FORMAT win64)
enable_language(ASM_NASM)
set(CMAKE_ASM_NASM_FLAGS "-DWIN64 -D__x86_64__ -DPIC")
# Soong's Windows host flags (build/soong/cc/config/x86_windows_host.go), for clang on MinGW-w64.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
# SK_R32_SHIFT=0: Skia's N32 is RGBA, as on Android (BGRA is its Windows default), the order
# HWUI gives a window's GL framebuffer (GL_RGBA8) and the NDK's bitmaps have.
add_compile_definitions(SK_R32_SHIFT=0 USE_MINGW WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601 WINVER=0x0601 __USE_MINGW_ANSI_STDIO=1
                        _FILE_OFFSET_BITS=64 NOMINMAX)
# -mno-ms-bitfields: Itanium bitfield layout everywhere (HWUI's DisplayListOp packs its header in
# bitfields; MinGW defaults to MSVC's layout), the same in every library that shares a struct.
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-O2;-fno-strict-aliasing;-ffunction-sections;-fdata-sections;-w;-Wno-error;-fmessage-length=0;-fdebug-prefix-map=${AOSP}=.;-mno-ms-bitfields>")
# The cross build links GCC's libstdc++ for MinGW, whose thread_locals are emulated (the toolchain
# file's -femulated-tls); MSYS2's CLANG64 and llvm-mingw link LLVM's libc++ with native TLS.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("#include <cstddef>\n#ifndef _LIBCPP_VERSION\n#error libstdc++\n#endif\nint main() {}"
                          HWUI_LIBCXX)
if(NOT HWUI_LIBCXX)
  add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-femulated-tls>")
  set(HWUI_CXX_RUNTIME -lstdc++ -latomic)
else()
  set(HWUI_CXX_RUNTIME)
endif()
# Soong's libc++ pulls these in transitively; libstdc++ and a newer libc++ (MSYS2's: BlobCache.cpp's
# std::lower_bound, std::terminate) do not.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -include cstring -include cstdint -include cstdlib -include functional -include optional -include algorithm -include exception")
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
foreach(m aosp_libhwui aosp_libEGL_blobCache)
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
set(GLUE "${CMAKE_CURRENT_SOURCE_DIR}/../glue")
target_sources(aosp_libhwui PRIVATE "${GLUE}/hwui_stubs.cpp")
target_include_directories(aosp_libhwui PRIVATE "${GLUE}" "${AOSP}/native_opengl/EGL/include")
# libjpeg-turbo's NASM sources include their neighbours.
target_include_directories(aosp_libjpeg PRIVATE "${AOSP}/libjpeg-turbo/simd/x86_64/" "${AOSP}/libjpeg-turbo/simd/nasm/")

find_program(CMAKE_DLLTOOL NAMES llvm-dlltool x86_64-w64-mingw32-dlltool dlltool REQUIRED)
# ANGLE's import libraries, from its exports (cmake/angle/*.def): the DLL loads libEGL.dll and
# libGLESv2.dll from bin/, beside it.
foreach(lib libEGL libGLESv2)
  set(def "${CMAKE_CURRENT_SOURCE_DIR}/angle/${lib}.def")
  set(imp "${CMAKE_CURRENT_BINARY_DIR}/${lib}.dll.a")
  add_custom_command(OUTPUT "${imp}" COMMAND ${CMAKE_DLLTOOL} -d "${def}" -l "${imp}" -m i386:x86-64 DEPENDS "${def}")
  list(APPEND ANGLE_IMPORTS "${imp}")
endforeach()
add_custom_target(angle_imports DEPENDS ${ANGLE_IMPORTS})

# The graphics DLL: HWUI, Skia and the rest, and the glue (hwui_build/glue): JNI_OnLoad, the window
# layers and their compositor, the choreographer, libutils' Looper on emulated descriptors.
add_library(aosp_hwui SHARED "${GLUE}/jni_onload.cpp" "${GLUE}/layers.cpp" "${GLUE}/choreographer.cpp"
            "${GLUE}/fd_emul.cpp" "${GLUE}/stubs.cpp" "${GLUE}/nio_fd.cpp" "${GLUE}/blob_cache.cpp" "${AOSP}/system_core/libutils/Looper.cpp"
            "${AOSP}/icu4c/source/stubdata/stubdata.cpp")
set_target_properties(aosp_hwui PROPERTIES OUTPUT_NAME "aosp-hwui" PREFIX "" CXX_STANDARD 20)
get_target_property(HWUI_INCLUDES aosp_libhwui INCLUDE_DIRECTORIES)
target_include_directories(aosp_hwui BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/shim/device" "${GLUE}")
target_include_directories(aosp_hwui PRIVATE ${HWUI_INCLUDES} "${AOSP}/hwui" "${AOSP}/skia14/include/gpu"
    "${AOSP}/skia14/include/core" "${AOSP}/icu4c/source/common" "${AOSP}/native_libs/ultrahdr/include" "${AOSP}/native_opengl/EGL")
target_compile_definitions(aosp_hwui PRIVATE HWUI_DEVICE=1 SK_GANESH SK_GL EGL_EGLEXT_PROTOTYPES GL_GLEXT_PROTOTYPES)
target_compile_options(aosp_hwui PRIVATE "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/shim/winapk_device_compat.h")
add_dependencies(aosp_hwui angle_imports)
set(HWUI_ARCHIVES aosp_libhwui aosp_libskia aosp_libminikin aosp_libharfbuzz_ng aosp_libicui18n aosp_libicuuc
    aosp_libft2 aosp_libpng aosp_libjpeg aosp_libwebp_decode aosp_libwebp_encode aosp_libgif aosp_libexpat
    aosp_libwuffs_mirror_release_c aosp_libz_static aosp_libEGL_blobCache aosp_libutils aosp_libcutils
    aosp_libcutils_sockets aosp_libbase aosp_liblog aosp_fmtlib aosp_libnativehelper)
target_link_libraries(aosp_hwui PRIVATE -Wl,--start-group ${HWUI_ARCHIVES} -Wl,--end-group ${ANGLE_IMPORTS}
    -static ${HWUI_CXX_RUNTIME} -lws2_32 -lbcrypt -lwindowscodecs -lole32 -loleaut32 -luuid -lgdi32 -luser32
    -lwinpthread -Wl,--gc-sections -s)
target_link_options(aosp_hwui PRIVATE -fuse-ld=lld "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/aosp-hwui.map")
