# winapk-soft-codecs-test on its own, for a machine without the Windows build: the software
# decoders, the picture conversions and the WebM reader need no Windows call, so they build on
# Linux (gcc or clang, optionally with -DWINAPK_SANITIZE=ON) and with a MinGW cross compiler
# (run under Wine). The Windows build makes the same test from windows/CMakeLists.txt.
#
#   cmake -S windows/test/soft_codecs -B build-codecs -G Ninja && ninja -C build-codecs
#   build-codecs/winapk-soft-codecs-test windows/test/video
cmake_minimum_required(VERSION 3.20)
project(winapk_soft_codecs_test LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(WINAPK_SANITIZE "AddressSanitizer and UndefinedBehaviorSanitizer on Win.apk's own code" OFF)

set(win ${CMAKE_CURRENT_LIST_DIR}/../..)
include(${win}/cmake/soft_codecs.cmake)
add_executable(winapk-soft-codecs-test ${win}/test/soft_codecs_test.cpp ${win}/host/soft_codecs.cpp
               ${win}/host/webm_demux.cpp ${win}/host/yuv_convert.cpp)
target_link_libraries(winapk-soft-codecs-test PRIVATE winapk_vpx winapk_dav1d winapk_opus winapk_mkvparser)
target_compile_options(winapk-soft-codecs-test PRIVATE -Wall -Wextra -O1 -g)
if(WINAPK_SANITIZE)
  target_compile_options(winapk-soft-codecs-test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(winapk-soft-codecs-test PRIVATE -fsanitize=address,undefined)
endif()
if(MINGW)
  target_link_options(winapk-soft-codecs-test PRIVATE -static)
endif()
