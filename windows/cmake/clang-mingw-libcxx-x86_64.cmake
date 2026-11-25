# Cross-compile for Windows x64 from Linux with an llvm-mingw toolchain
# (github.com/mstorsjo/llvm-mingw, a ucrt build): clang, lld and LLVM's libc++,
# as MSYS2's CLANG64 has them, so a Linux host can reproduce CI's native
# Windows build. Point LLVM_MINGW (environment or -D) at its root:
#
#   LLVM_MINGW=/opt/llvm-mingw \
#   HWUI_TOOLCHAIN_FILE=windows/cmake/clang-mingw-libcxx-x86_64.cmake \
#       windows/hwui_build/build.sh <work dir>

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT LLVM_MINGW)
  set(LLVM_MINGW "$ENV{LLVM_MINGW}")
endif()
if(NOT LLVM_MINGW)
  message(FATAL_ERROR "set LLVM_MINGW to the llvm-mingw toolchain's root")
endif()
set(LLVM_MINGW "${LLVM_MINGW}" CACHE PATH "llvm-mingw's root")

set(CMAKE_C_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32-windres)
set(CMAKE_AR ${LLVM_MINGW}/bin/llvm-ar CACHE FILEPATH "")
set(CMAKE_RANLIB ${LLVM_MINGW}/bin/llvm-ranlib CACHE FILEPATH "")

set(CMAKE_FIND_ROOT_PATH ${LLVM_MINGW}/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
