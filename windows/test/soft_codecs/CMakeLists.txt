# winapk-soft-codecs-test and winapk-ndk-muxer-test on their own, for a machine without the
# Windows build: the software decoders, the picture conversions, the WebM reader and the muxers
# need no Windows call, so they build on Linux (the codecs' test with gcc or clang, the muxer's
# with clang; optionally with -DWINAPK_SANITIZE=ON) and with a MinGW cross compiler (run under
# Wine). The Windows build makes the same tests from windows/CMakeLists.txt.
#
#   CC=clang CXX=clang++ cmake -S windows/test/soft_codecs -B build-codecs -G Ninja && ninja -C build-codecs
#   build-codecs/winapk-soft-codecs-test windows/test/video
#   build-codecs/winapk-ndk-muxer-test windows/test/video
cmake_minimum_required(VERSION 3.20)
project(winapk_soft_codecs_test LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(WINAPK_SANITIZE "AddressSanitizer and UndefinedBehaviorSanitizer on Win.apk's own code" OFF)

set(win ${CMAKE_CURRENT_LIST_DIR}/../..)
include(${win}/cmake/soft_codecs.cmake)

function(winapk_test_options target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -O1 -g)
  if(WINAPK_SANITIZE)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endif()
  if(MINGW)
    target_link_options(${target} PRIVATE -static)
  endif()
endfunction()

add_executable(winapk-soft-codecs-test ${win}/test/soft_codecs_test.cpp ${win}/host/soft_codecs.cpp
               ${win}/host/webm_demux.cpp ${win}/host/yuv_convert.cpp)
target_link_libraries(winapk-soft-codecs-test PRIVATE winapk_vpx winapk_dav1d winapk_opus winapk_mkvparser)
winapk_test_options(winapk-soft-codecs-test)

# libmediandk's AMediaMuxer, AMediaFormat and crypto answers as an app's native code calls them.
# Their functions are System V ones (winapk/guest_abi.h), which takes clang.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_executable(winapk-ndk-muxer-test ${win}/test/ndk_muxer_test.cpp ${win}/host/ndk_media_muxer.cpp
                 ${win}/host/native_media.cpp ${win}/host/webm_writer.cpp ${win}/host/mp4_writer.cpp
                 ${win}/host/webm_demux.cpp)
  target_include_directories(winapk-ndk-muxer-test PRIVATE ${win}/host ${win}/include)
  target_link_libraries(winapk-ndk-muxer-test PRIVATE winapk_mkvparser winapk_mkvmuxer)
  winapk_test_options(winapk-ndk-muxer-test)
  # The test calls the functions as guest code does, by name through pointers of the C types
  # (void* for AMediaFormat*): UBSan's function check would call each such call a mismatch.
  if(WINAPK_SANITIZE)
    target_compile_options(winapk-ndk-muxer-test PRIVATE -fno-sanitize=function)
  endif()
else()
  message(STATUS "winapk-ndk-muxer-test takes clang (System V functions); not built with ${CMAKE_CXX_COMPILER_ID}")
endif()
