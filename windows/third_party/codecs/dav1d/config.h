/* dav1d's config.h for Win.apk: what dav1d's meson.build writes for x86-64
 * Windows with clang/MinGW and -Denable_asm=false -Dlogging=false, both bit
 * depths. Written by hand, as Android's external/libdav1d keeps its own, so
 * that the build needs no meson. test/soft_codecs_test.cpp also builds on
 * Linux with it. */
#pragma once

#define ARCH_AARCH64 0
#define ARCH_ARM 0
#define ARCH_LOONGARCH 0
#define ARCH_LOONGARCH32 0
#define ARCH_LOONGARCH64 0
#define ARCH_PPC64LE 0
#define ARCH_RISCV 0
#define ARCH_RV32 0
#define ARCH_RV64 0
#define ARCH_X86 1
#define ARCH_X86_32 0
#define ARCH_X86_64 1
#define CONFIG_16BPC 1
#define CONFIG_8BPC 1
#define CONFIG_LOG 0
#define CONFIG_MACOS_KPERF 0
#define ENDIANNESS_BIG 0
#define HAVE_ALIGNED_ALLOC 0
#define HAVE_ASM 0
#define HAVE_C11_GENERIC 1
#define HAVE_CLOCK_GETTIME 0
#define HAVE_DLSYM 0
#define HAVE_ELF_AUX_INFO 0
#define HAVE_GETAUXVAL 0
#define HAVE_IO_H 1
#define HAVE_MEMALIGN 0
/* Windows allocates with _aligned_malloc; the Linux build of the test, with this. */
#ifdef _WIN32
#define HAVE_POSIX_MEMALIGN 0
#else
#define HAVE_POSIX_MEMALIGN 1
#endif
#define HAVE_PTHREAD_GETAFFINITY_NP 0
#define HAVE_PTHREAD_NP_H 0
#define HAVE_PTHREAD_SETAFFINITY_NP 0
#define HAVE_PTHREAD_SETNAME_NP 0
#define HAVE_PTHREAD_SET_NAME_NP 0
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define TRIM_DSP_FUNCTIONS 1
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef UNICODE
#define UNICODE 1
#endif
#ifndef _UNICODE
#define _UNICODE 1
#endif
#define __USE_MINGW_ANSI_STDIO 1
#define _FILE_OFFSET_BITS 64
