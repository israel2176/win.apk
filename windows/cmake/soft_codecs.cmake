# soft_codecs.cmake -- the libraries behind the decoders MediaCodec falls back on where Media
# Foundation has none (host/soft_codecs.h) and behind MediaExtractor's WebM reader
# (host/webm_demux.h):
#
#   winapk_vpx       libvpx: the VP8 and VP9 decoders (profiles 0 to 3, so 10 and 12-bit too)
#   winapk_dav1d     dav1d: AV1, 8 and 10/12-bit
#   winapk_opus      libopus: Opus, decoded in float
#   winapk_mkvparser libwebm's mkvparser: WebM and Matroska files
#
# Each comes from its project's own release, checked against a pinned SHA-256 (libwebm, which
# publishes no tarball, from its git tag, checked against the tag's commit), and is built the way
# its own build files describe, decoders only:
#  - libvpx and dav1d with their x86-64 SIMD (SSE2 to AVX2, and dav1d's AVX-512), chosen at run
#    time by the CPU, so nasm is needed. Their configure steps (libvpx's configure script, dav1d's
#    meson) were run once by scripts/soft_codecs_config.sh, whose output is in
#    third_party/codecs/: the configuration headers and libvpx's source list for it;
#  - libopus from the source lists of its own makefiles (opus_sources.mk, celt_sources.mk,
#    silk_sources.mk), float, without the deep-learning extensions; the encoder's files are
#    compiled but nothing references them, so the static link leaves them out;
#  - mkvparser.cc and mkvreader.cc alone.
# The targets carry their public include directories; WINAPK_SOFT_CODEC_INCLUDES lists them too.
# Works for a Windows build (MinGW clang) and for a Linux one (test/soft_codecs/CMakeLists.txt).

set(WINAPK_VPX_VERSION 1.17.0)
set(WINAPK_VPX_SHA256 7f98aae5c52c46e85b7b26feabcab9b1554605e771bd1df1d6e6a00ee22fa8ed)
set(WINAPK_DAV1D_VERSION 1.5.5)
set(WINAPK_DAV1D_SHA256 ca96b1d0ba3f01f55de7231d775892b64994ba061aaeeaaa4f662a390b3abf9e)
set(WINAPK_OPUS_VERSION 1.6.1)
set(WINAPK_OPUS_SHA256 6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1)
set(WINAPK_WEBM_VERSION 1.0.0.32)
set(WINAPK_WEBM_COMMIT f2a982d748b80586ae53b89a2e6ebbc305848b8c)

enable_language(C)
enable_language(ASM_NASM)
if(NOT CMAKE_ASM_NASM_COMPILER)
  message(FATAL_ERROR "nasm is needed for libvpx's and dav1d's x86-64 assembly (MSYS2: mingw-w64-clang-x86_64-nasm)")
endif()

set(WINAPK_CODECS_SRC ${CMAKE_CURRENT_LIST_DIR}/../third_party/codecs)
set(WINAPK_CODECS_DIR ${CMAKE_BINARY_DIR}/codecs)

# Downloads a release archive once, checks it, unpacks it into WINAPK_CODECS_DIR/<dir>.
function(winapk_codec_release dir url sha256)
  set(where ${WINAPK_CODECS_DIR}/${dir})
  if(EXISTS ${where}/.winapk-unpacked)
    return()
  endif()
  get_filename_component(archive ${url} NAME)
  file(DOWNLOAD ${url} ${WINAPK_CODECS_DIR}/${archive} EXPECTED_HASH SHA256=${sha256} TLS_VERIFY ON)
  file(ARCHIVE_EXTRACT INPUT ${WINAPK_CODECS_DIR}/${archive} DESTINATION ${WINAPK_CODECS_DIR})
  if(NOT EXISTS ${where})
    message(FATAL_ERROR "${archive} did not unpack into ${dir}")
  endif()
  file(TOUCH ${where}/.winapk-unpacked)
endfunction()

# The files a variable of one of libopus's makefiles lists (`NAME = a.c \` lines).
function(winapk_make_list out mk name)
  file(READ ${mk} text)
  string(REGEX REPLACE "\\\\\r?\n" " " text "${text}")  # continuation lines joined
  if(NOT text MATCHES "(^|\n)${name} *=([^\n]*)")
    message(FATAL_ERROR "${mk} has no ${name}")
  endif()
  string(STRIP "${CMAKE_MATCH_2}" files)
  string(REGEX REPLACE "[ \t]+" ";" files "${files}")
  set(${out} ${files} PARENT_SCOPE)
endfunction()

# Third-party code is built optimized whatever the build type (a debug build of the runtime
# still decodes in real time) and without its warnings.
set(winapk_codec_cflags -O2 -w -fno-strict-aliasing)

# ---- libvpx -------------------------------------------------------------------------------
winapk_codec_release(libvpx-${WINAPK_VPX_VERSION}
  https://storage.googleapis.com/downloads.webmproject.org/releases/webm/libvpx-${WINAPK_VPX_VERSION}.tar.gz
  ${WINAPK_VPX_SHA256})
set(vpx ${WINAPK_CODECS_DIR}/libvpx-${WINAPK_VPX_VERSION})
include(${WINAPK_CODECS_SRC}/libvpx/sources.cmake)
set(vpx_sources ${WINAPK_CODECS_SRC}/libvpx/vpx_config.c)
foreach(f IN LISTS WINAPK_VPX_C_SOURCES WINAPK_VPX_ASM_SOURCES)
  list(APPEND vpx_sources ${vpx}/${f})
  # The flags libvpx's own makefile gives a file by its name (build/make/Makefile): the rest of
  # the library is plain x86-64, and these files run only on a CPU that has the extension.
  foreach(pair "_sse2\\.c$;-msse2" "_sse3\\.c$;-msse3" "_ssse3\\.c$;-mssse3" "_sse4\\.c$;-msse4.1"
               "_avx\\.c$;-mavx" "_avx2\\.c$;-mavx2")
    list(GET pair 0 pattern)
    list(GET pair 1 flag)
    if(f MATCHES "${pattern}")
      set_source_files_properties(${vpx}/${f} PROPERTIES COMPILE_OPTIONS ${flag})
    endif()
  endforeach()
endforeach()
add_library(winapk_vpx STATIC ${vpx_sources})
target_include_directories(winapk_vpx PRIVATE ${WINAPK_CODECS_SRC}/libvpx ${vpx})
target_include_directories(winapk_vpx SYSTEM INTERFACE ${vpx})
target_compile_options(winapk_vpx PRIVATE $<$<COMPILE_LANGUAGE:C>:${winapk_codec_cflags}>)
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(winapk_vpx PUBLIC Threads::Threads)
endif()

# ---- dav1d --------------------------------------------------------------------------------
winapk_codec_release(dav1d-${WINAPK_DAV1D_VERSION}
  https://downloads.videolan.org/pub/videolan/dav1d/${WINAPK_DAV1D_VERSION}/dav1d-${WINAPK_DAV1D_VERSION}.tar.xz
  ${WINAPK_DAV1D_SHA256})
set(dav1d ${WINAPK_CODECS_DIR}/dav1d-${WINAPK_DAV1D_VERSION})
file(WRITE ${WINAPK_CODECS_DIR}/dav1d-generated/vcs_version.h
     "/* written by cmake/soft_codecs.cmake */\n#define DAV1D_VERSION \"${WINAPK_DAV1D_VERSION}\"\n")
# dav1d's src/meson.build, for x86-64 with its assembly.
set(dav1d_c cdf cpu ctx data decode dequant_tables getbits intra_edge itx_1d lf_mask lib log mem msac obu pal
            picture qm ref refmvs scan tables thread_task warpmv wedge x86/cpu)
set(dav1d_tmpl cdef_apply cdef fg_apply filmgrain ipred_prepare ipred itx lf_apply loopfilter looprestoration
               lr_apply mc recon)
set(dav1d_asm cpuid lf_mask msac pal refmvs itx_avx512 cdef_avx2 cdef_avx512 itx_avx2 cdef_sse itx_sse
              filmgrain_avx512 ipred_avx512 loopfilter_avx512 looprestoration_avx512 mc_avx512 filmgrain_avx2
              ipred_avx2 loopfilter_avx2 looprestoration_avx2 mc_avx2 filmgrain_sse ipred_sse loopfilter_sse
              looprestoration_sse mc_sse cdef16_avx512 filmgrain16_avx512 ipred16_avx512 itx16_avx512
              loopfilter16_avx512 looprestoration16_avx512 mc16_avx512 cdef16_avx2 filmgrain16_avx2 ipred16_avx2
              itx16_avx2 loopfilter16_avx2 looprestoration16_avx2 mc16_avx2 cdef16_sse filmgrain16_sse ipred16_sse
              itx16_sse loopfilter16_sse looprestoration16_sse mc16_sse)
set(dav1d_includes ${WINAPK_CODECS_SRC}/dav1d ${WINAPK_CODECS_DIR}/dav1d-generated ${dav1d} ${dav1d}/include/dav1d
                   ${dav1d}/include ${dav1d}/src)
set(dav1d_cflags ${winapk_codec_cflags} -fomit-frame-pointer -ffast-math)
foreach(depth 8 16)
  set(files "")
  foreach(f IN LISTS dav1d_tmpl)
    list(APPEND files ${dav1d}/src/${f}_tmpl.c)
  endforeach()
  add_library(winapk_dav1d_${depth} OBJECT ${files})
  target_include_directories(winapk_dav1d_${depth} PRIVATE ${dav1d_includes})
  target_compile_definitions(winapk_dav1d_${depth} PRIVATE BITDEPTH=${depth} $<$<NOT:$<BOOL:${WIN32}>>:_GNU_SOURCE>)
  target_compile_options(winapk_dav1d_${depth} PRIVATE ${dav1d_cflags})
endforeach()
set(files "")
foreach(f IN LISTS dav1d_c)
  list(APPEND files ${dav1d}/src/${f}.c)
endforeach()
foreach(f IN LISTS dav1d_asm)
  list(APPEND files ${dav1d}/src/x86/${f}.asm)
endforeach()
if(WIN32)
  list(APPEND files ${dav1d}/src/win32/thread.c)
endif()
add_library(winapk_dav1d STATIC ${files} $<TARGET_OBJECTS:winapk_dav1d_8> $<TARGET_OBJECTS:winapk_dav1d_16>)
# The assembly finds config.asm and src/ext/x86/x86inc.asm through these too.
target_include_directories(winapk_dav1d PRIVATE ${dav1d_includes})
target_include_directories(winapk_dav1d SYSTEM INTERFACE ${dav1d}/include)
target_compile_definitions(winapk_dav1d PRIVATE $<$<AND:$<COMPILE_LANGUAGE:C>,$<NOT:$<BOOL:${WIN32}>>>:_GNU_SOURCE>)
target_compile_options(winapk_dav1d PRIVATE $<$<COMPILE_LANGUAGE:C>:${dav1d_cflags}>)
if(NOT WIN32)
  target_link_libraries(winapk_dav1d PUBLIC Threads::Threads)
endif()

# ---- libopus ------------------------------------------------------------------------------
winapk_codec_release(opus-${WINAPK_OPUS_VERSION}
  https://downloads.xiph.org/releases/opus/opus-${WINAPK_OPUS_VERSION}.tar.gz ${WINAPK_OPUS_SHA256})
set(opus ${WINAPK_CODECS_DIR}/opus-${WINAPK_OPUS_VERSION})
winapk_make_list(opus_src ${opus}/opus_sources.mk OPUS_SOURCES)
winapk_make_list(opus_float ${opus}/opus_sources.mk OPUS_SOURCES_FLOAT)
winapk_make_list(celt_src ${opus}/celt_sources.mk CELT_SOURCES)
winapk_make_list(silk_src ${opus}/silk_sources.mk SILK_SOURCES)
winapk_make_list(silk_float ${opus}/silk_sources.mk SILK_SOURCES_FLOAT)
set(files "")
foreach(f IN LISTS opus_src opus_float celt_src silk_src silk_float)
  list(APPEND files ${opus}/${f})
endforeach()
if(NOT files MATCHES "opus_multistream_decoder\\.c" OR NOT files MATCHES "celt_decoder\\.c")
  message(FATAL_ERROR "libopus ${WINAPK_OPUS_VERSION}: its makefiles' source lists were not read")
endif()
add_library(winapk_opus STATIC ${files})
target_include_directories(winapk_opus PRIVATE ${opus} ${opus}/include ${opus}/celt ${opus}/silk ${opus}/silk/float)
target_include_directories(winapk_opus SYSTEM INTERFACE ${opus}/include)
# What its configure script defines for a float build on a C99 compiler (config.h).
target_compile_definitions(winapk_opus PRIVATE OPUS_BUILD VAR_ARRAYS HAVE_LRINTF HAVE_LRINT FLOAT_APPROX
                           "PACKAGE_VERSION=\"${WINAPK_OPUS_VERSION}\"")
target_compile_options(winapk_opus PRIVATE ${winapk_codec_cflags})
if(NOT WIN32)
  target_link_libraries(winapk_opus PUBLIC m)
endif()

# ---- libwebm's mkvparser ------------------------------------------------------------------
set(webm ${WINAPK_CODECS_DIR}/libwebm-${WINAPK_WEBM_VERSION})
if(NOT EXISTS ${webm}/.winapk-unpacked)
  find_package(Git REQUIRED)
  file(REMOVE_RECURSE ${webm})
  execute_process(COMMAND ${GIT_EXECUTABLE} -c advice.detachedHead=false clone --quiet --depth 1 --branch
                          libwebm-${WINAPK_WEBM_VERSION} https://github.com/webmproject/libwebm ${webm}
                  RESULT_VARIABLE cloned)
  execute_process(COMMAND ${GIT_EXECUTABLE} -C ${webm} rev-parse HEAD OUTPUT_VARIABLE head
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT cloned EQUAL 0 OR NOT head STREQUAL WINAPK_WEBM_COMMIT)
    message(FATAL_ERROR "libwebm-${WINAPK_WEBM_VERSION}: expected commit ${WINAPK_WEBM_COMMIT}, got '${head}'")
  endif()
  file(TOUCH ${webm}/.winapk-unpacked)
endif()
add_library(winapk_mkvparser STATIC ${webm}/mkvparser/mkvparser.cc ${webm}/mkvparser/mkvreader.cc)
target_include_directories(winapk_mkvparser SYSTEM PUBLIC ${webm})
target_compile_options(winapk_mkvparser PRIVATE -O2 -w)

set(WINAPK_SOFT_CODEC_INCLUDES ${vpx} ${dav1d}/include ${opus}/include ${webm})
