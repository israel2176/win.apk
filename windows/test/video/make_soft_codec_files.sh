#!/usr/bin/env bash
# make_soft_codec_files.sh -- remakes the WebM files test/soft_codecs_test.cpp (and
# test/video_test.cpp's WebM part) read, with ffmpeg (libvpx, libaom and libopus encoders):
#
#   color.webm            64x48, 25 fps, 1 s, red in the top half and blue in the bottom, a key
#                         frame every 10: VP9 profile 0, with a 440 Hz mono Opus tone
#   color_vp8.webm        the same picture, VP8
#   color_vp9_10bit.webm  VP9 profile 2 (10-bit)
#   color_vp9_444.webm    VP9 profile 1 (8-bit 4:4:4)
#   color_vp9_odd.webm    VP9 at 63x47: a picture whose size is not a whole number of chroma
#                         samples nor of blocks (the crop)
#   color_av1.webm        AV1 (8-bit)
#   color_av1_hdr.webm    AV1 10-bit tagged BT.2020 / SMPTE ST 2084 (PQ), in the bitstream and in
#                         the WebM's Colour element
#   surround.webm         1 s of 5.1 Opus (mapping family 1): a different tone in each channel,
#                         300, 500, 700, 60 (LFE), 900 and 1100 Hz in FL FR FC LFE BL BR order
#
#   windows/test/video/make_soft_codec_files.sh [out dir, default: this script's directory]
set -euo pipefail
out=${1:-$(cd "$(dirname "$0")" && pwd)}
ff=(ffmpeg -hide_banner -loglevel error -y)
# Clusters of 200 ms: a file of several, so that seeking (by its Cues) and a cut file have
# whole clusters to work with.
mux=(-cluster_time_limit 200)
# The picture: blue, a red box over its top half.
pic=(-f lavfi -i "color=c=blue:s=64x48:r=25:d=1,drawbox=x=0:y=0:w=64:h=24:color=red:t=fill")
tone=(-f lavfi -i "sine=frequency=440:sample_rate=48000:duration=1")
common=(-g 10 -keyint_min 10 -an)

"${ff[@]}" "${pic[@]}" "${tone[@]}" -map 0:v -map 1:a -c:v libvpx-vp9 -pix_fmt yuv420p -g 10 -keyint_min 10 \
    -b:v 200k -c:a libopus -b:a 32k -ac 1 "${mux[@]}" "$out/color.webm"
"${ff[@]}" "${pic[@]}" -c:v libvpx -pix_fmt yuv420p -b:v 200k "${common[@]}" "${mux[@]}" "$out/color_vp8.webm"
"${ff[@]}" "${pic[@]}" -c:v libvpx-vp9 -pix_fmt yuv420p10le -b:v 200k "${common[@]}" "${mux[@]}" "$out/color_vp9_10bit.webm"
"${ff[@]}" "${pic[@]}" -c:v libvpx-vp9 -pix_fmt yuv444p -b:v 200k "${common[@]}" "${mux[@]}" "$out/color_vp9_444.webm"
"${ff[@]}" -f lavfi -i "color=c=blue:s=63x47:r=25:d=1,format=rgb24,drawbox=x=0:y=0:w=63:h=24:color=red:t=fill" \
    -c:v libvpx-vp9 -pix_fmt yuv420p -b:v 200k "${common[@]}" "${mux[@]}" "$out/color_vp9_odd.webm"
"${ff[@]}" "${pic[@]}" -c:v libaom-av1 -cpu-used 8 -pix_fmt yuv420p -b:v 200k "${common[@]}" "${mux[@]}" "$out/color_av1.webm"
"${ff[@]}" "${pic[@]}" -c:v libaom-av1 -cpu-used 8 -pix_fmt yuv420p10le -b:v 200k -color_primaries bt2020 \
    -color_trc smpte2084 -colorspace bt2020nc -color_range tv "${common[@]}" "${mux[@]}" "$out/color_av1_hdr.webm"
"${ff[@]}" -f lavfi -i "sine=frequency=300:sample_rate=48000:duration=1" \
    -f lavfi -i "sine=frequency=500:sample_rate=48000:duration=1" \
    -f lavfi -i "sine=frequency=700:sample_rate=48000:duration=1" \
    -f lavfi -i "sine=frequency=60:sample_rate=48000:duration=1" \
    -f lavfi -i "sine=frequency=900:sample_rate=48000:duration=1" \
    -f lavfi -i "sine=frequency=1100:sample_rate=48000:duration=1" \
    -filter_complex "[0:a][1:a][2:a][3:a][4:a][5:a]join=inputs=6:channel_layout=5.1:map=0.0-FL|1.0-FR|2.0-FC|3.0-LFE|4.0-BL|5.0-BR[a]" -map "[a]" \
    -c:a libopus -b:a 96k -mapping_family 1 "$out/surround.webm"
ls -l "$out"/color*.webm "$out/surround.webm"
