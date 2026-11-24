# soft_codecs.cmake -- the software codecs MediaCodec and MediaExtractor fall back on when
# Media Foundation lacks a format (host/soft_codecs.h): VP8 and VP9 (libvpx, decode only),
# AV1 (dav1d), Opus (libopus, decode only) and WebM/Matroska demuxing (libwebm's mkvparser).
# After the other runtime's cmake/aosp/media_video_codecs.cmake and media_codecs.cmake (Apache-2.0),
# which build the same libraries from AOSP's external/ trees the same way: C only, no
# assembly, the encoders left out.
#
# Each release tarball is downloaded once into the build tree and checked against its
# SHA-256 (as libwebp and stb_vorbis above). The configure/meson outputs the libraries
# would generate are kept in third_party/codecs/ instead, so the build needs neither:
#   libvpx/  `configure --target=generic-gnu --disable-vp8-encoder --disable-vp9-encoder
#            --enable-vp9-highbitdepth --disable-postproc --disable-vp9-postproc
#            --disable-webm-io --disable-libyuv --enable-multithread
#            --size-limit=16384x16384` (vpx_config.h, vpx_config.c, the rtcd headers,
#            vpx_version.h), then HAVE_PTHREAD_H set to 0 for Win32 threads;
#   dav1d/   config.h and vcs_version.h, as meson writes them with -Denable_asm=false.
# The libraries' licenses are in package/THIRD_PARTY_NOTICES.txt.
#
# Defines the static libraries winapk_vpx, winapk_dav1d, winapk_opus and winapk_mkvparser,
# and WINAPK_SOFT_CODEC_INCLUDES (their public headers).

enable_language(C)

set(_codecs_cfg ${CMAKE_CURRENT_LIST_DIR}/../third_party/codecs)

# Downloads <url> once into the build tree, checks it, unpacks it; <dir> is the folder the
# tarball holds.
function(winapk_fetch_codec name url sha256 dir)
  if(NOT EXISTS ${CMAKE_BINARY_DIR}/${dir})
    get_filename_component(_file ${url} NAME)
    set(_tgz ${CMAKE_BINARY_DIR}/${_file})
    file(DOWNLOAD ${url} ${_tgz} EXPECTED_HASH SHA256=${sha256} TLS_VERIFY ON)
    file(ARCHIVE_EXTRACT INPUT ${_tgz} DESTINATION ${CMAKE_BINARY_DIR})
  endif()
  if(NOT EXISTS ${CMAKE_BINARY_DIR}/${dir})
    message(FATAL_ERROR "${name}: ${url} did not unpack into ${dir}")
  endif()
endfunction()

# Third-party code: their warnings are not ours.
set(_codec_flags -w -O2)

# --- libvpx 1.15.2: VP8 and VP9 decoding -----------------------------------------------------
# Debian's copy of the release tarball (the same bytes as the tag's archive on GitHub).
winapk_fetch_codec(libvpx https://deb.debian.org/debian/pool/main/libv/libvpx/libvpx_1.15.2.orig.tar.gz
                   26fcd3db88045dee380e581862a6ef106f49b74b6396ee95c2993a260b4636aa libvpx-1.15.2)
set(_vpx ${CMAKE_BINARY_DIR}/libvpx-1.15.2)
set(_vpx_srcs
  vp8/common/alloccommon.c vp8/common/blockd.c vp8/common/dequantize.c vp8/common/entropy.c
  vp8/common/entropymode.c vp8/common/entropymv.c vp8/common/extend.c vp8/common/filter.c
  vp8/common/findnearmv.c vp8/common/generic/systemdependent.c vp8/common/idct_blk.c
  vp8/common/idctllm.c vp8/common/loopfilter_filters.c vp8/common/mbpitch.c vp8/common/modecont.c
  vp8/common/quant_common.c vp8/common/reconinter.c vp8/common/reconintra.c
  vp8/common/reconintra4x4.c vp8/common/rtcd.c vp8/common/setupintrarecon.c
  vp8/common/swapyv12buffer.c vp8/common/treecoder.c vp8/common/vp8_loopfilter.c
  vp8/decoder/dboolhuff.c vp8/decoder/decodeframe.c vp8/decoder/decodemv.c vp8/decoder/detokenize.c
  vp8/decoder/onyxd_if.c vp8/decoder/threading.c vp8/vp8_dx_iface.c
  vp9/common/vp9_alloccommon.c vp9/common/vp9_blockd.c vp9/common/vp9_common_data.c
  vp9/common/vp9_entropy.c vp9/common/vp9_entropymode.c vp9/common/vp9_entropymv.c
  vp9/common/vp9_filter.c vp9/common/vp9_frame_buffers.c vp9/common/vp9_idct.c
  vp9/common/vp9_loopfilter.c vp9/common/vp9_mvref_common.c vp9/common/vp9_pred_common.c
  vp9/common/vp9_quant_common.c vp9/common/vp9_reconinter.c vp9/common/vp9_reconintra.c
  vp9/common/vp9_rtcd.c vp9/common/vp9_scale.c vp9/common/vp9_scan.c vp9/common/vp9_seg_common.c
  vp9/common/vp9_thread_common.c vp9/common/vp9_tile_common.c
  vp9/decoder/vp9_decodeframe.c vp9/decoder/vp9_decodemv.c vp9/decoder/vp9_decoder.c
  vp9/decoder/vp9_detokenize.c vp9/decoder/vp9_dsubexp.c vp9/decoder/vp9_job_queue.c
  vp9/vp9_dx_iface.c vp9/vp9_iface_common.c
  vpx/src/vpx_codec.c vpx/src/vpx_decoder.c vpx/src/vpx_image.c
  vpx_dsp/bitreader.c vpx_dsp/bitreader_buffer.c vpx_dsp/intrapred.c vpx_dsp/inv_txfm.c
  vpx_dsp/loopfilter.c vpx_dsp/prob.c vpx_dsp/vpx_convolve.c vpx_dsp/vpx_dsp_rtcd.c
  vpx_mem/vpx_mem.c
  vpx_scale/generic/gen_scalers.c vpx_scale/generic/vpx_scale.c vpx_scale/generic/yv12config.c
  vpx_scale/generic/yv12extend.c vpx_scale/vpx_scale_rtcd.c
  vpx_util/vpx_thread.c)
list(TRANSFORM _vpx_srcs PREPEND ${_vpx}/)
add_library(winapk_vpx STATIC ${_vpx_srcs} ${_codecs_cfg}/libvpx/vpx_config.c)
target_include_directories(winapk_vpx PRIVATE ${_codecs_cfg}/libvpx ${_vpx})
target_compile_options(winapk_vpx PRIVATE ${_codec_flags})

# --- dav1d 1.5.1: AV1 decoding --------------------------------------------------------------
# C only, as Android builds it for x86-64; 8-bit and 10/12-bit pictures (the templates twice).
winapk_fetch_codec(dav1d https://code.videolan.org/videolan/dav1d/-/archive/1.5.1/dav1d-1.5.1.tar.gz
                   fa635e2bdb25147b1384007c83e15de44c589582bb3b9a53fc1579cb9d74b695 dav1d-1.5.1)
set(_dav1d ${CMAKE_BINARY_DIR}/dav1d-1.5.1)
set(_dav1d_includes ${_codecs_cfg}/dav1d ${_dav1d} ${_dav1d}/include)
set(_dav1d_tmpl cdef_apply_tmpl.c cdef_tmpl.c fg_apply_tmpl.c filmgrain_tmpl.c ipred_prepare_tmpl.c
  ipred_tmpl.c itx_tmpl.c lf_apply_tmpl.c loopfilter_tmpl.c looprestoration_tmpl.c lr_apply_tmpl.c
  mc_tmpl.c recon_tmpl.c)
list(TRANSFORM _dav1d_tmpl PREPEND ${_dav1d}/src/)
foreach(_bits 8 16)
  add_library(winapk_dav1d_${_bits}bit OBJECT ${_dav1d_tmpl})
  target_include_directories(winapk_dav1d_${_bits}bit PRIVATE ${_dav1d_includes})
  target_compile_definitions(winapk_dav1d_${_bits}bit PRIVATE BITDEPTH=${_bits})
  target_compile_options(winapk_dav1d_${_bits}bit PRIVATE ${_codec_flags} -std=gnu11)
endforeach()
set(_dav1d_srcs cdf.c cpu.c ctx.c data.c decode.c dequant_tables.c getbits.c intra_edge.c itx_1d.c
  lf_mask.c lib.c log.c mem.c msac.c obu.c pal.c picture.c qm.c ref.c refmvs.c scan.c tables.c
  thread_task.c warpmv.c wedge.c win32/thread.c)
list(TRANSFORM _dav1d_srcs PREPEND ${_dav1d}/src/)
add_library(winapk_dav1d STATIC ${_dav1d_srcs} $<TARGET_OBJECTS:winapk_dav1d_8bit>
            $<TARGET_OBJECTS:winapk_dav1d_16bit>)
target_include_directories(winapk_dav1d PRIVATE ${_dav1d_includes})
target_compile_options(winapk_dav1d PRIVATE ${_codec_flags} -std=gnu11)

# --- libopus 1.5.2: Opus decoding -----------------------------------------------------------
# Floating point, no intrinsics (x86-64's SSE2 is the compiler's baseline); the source lists
# are read from the release's own .mk files. Only the decoder's objects end up linked.
winapk_fetch_codec(opus https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz
                   65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1 opus-1.5.2)
set(_opus ${CMAKE_BINARY_DIR}/opus-1.5.2)
function(_winapk_opus_list mk var out)
  file(READ ${_opus}/${mk} _text)
  # "VAR = \" and its continued lines.
  string(REGEX MATCH "${var} =([^\n\\\\]|\\\\\n)*" _block "${_text}")
  string(REGEX MATCHALL "[A-Za-z_0-9/]+\\.c" _files "${_block}")
  list(TRANSFORM _files PREPEND ${_opus}/)
  set(${out} ${_files} PARENT_SCOPE)
endfunction()
_winapk_opus_list(opus_sources.mk OPUS_SOURCES _o1)
_winapk_opus_list(opus_sources.mk OPUS_SOURCES_FLOAT _o2)
_winapk_opus_list(celt_sources.mk CELT_SOURCES _o3)
_winapk_opus_list(silk_sources.mk SILK_SOURCES _o4)
_winapk_opus_list(silk_sources.mk SILK_SOURCES_FLOAT _o5)
add_library(winapk_opus STATIC ${_o1} ${_o2} ${_o3} ${_o4} ${_o5})
target_include_directories(winapk_opus PRIVATE ${_opus} ${_opus}/include ${_opus}/celt ${_opus}/silk
                           ${_opus}/silk/float ${_opus}/dnn)
target_compile_definitions(winapk_opus PRIVATE OPUS_BUILD USE_ALLOCA HAVE_LRINT HAVE_LRINTF
                           "PACKAGE_VERSION=\"1.5.2\"")
target_compile_options(winapk_opus PRIVATE ${_codec_flags})

# --- libwebm 1.0.0.31: mkvparser, the WebM/Matroska demuxer ----------------------------------
winapk_fetch_codec(libwebm https://deb.debian.org/debian/pool/main/libw/libwebm/libwebm_1.0.0.31.orig.tar.xz
                   0a2597bb7c6ee10bd53f40ac16de10368b97cb7bb50acf9543c9d6a84aa9babe libwebm-1.0.0.31)
set(_webm ${CMAKE_BINARY_DIR}/libwebm-1.0.0.31)
add_library(winapk_mkvparser STATIC ${_webm}/mkvparser/mkvparser.cc)
target_include_directories(winapk_mkvparser PUBLIC ${_webm})
target_compile_options(winapk_mkvparser PRIVATE ${_codec_flags})

set(WINAPK_SOFT_CODEC_INCLUDES ${_vpx} ${_codecs_cfg}/libvpx ${_dav1d}/include ${_opus}/include ${_webm})
