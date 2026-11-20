#!/usr/bin/env bash
# redirect_decoders.sh -- points AOSP's BitmapFactory and ImageDecoder (class files under <classes dir>)
# at windows/java/aosp_gfx's com.winapk.aospgfx.HostImageDecode for their decoding natives
# (AospCallRedirect.java): the native runtime has no HEIF codec (a device's comes from its media stack),
# and HostImageDecode calls AOSP's native and, when that fails on a HEIF or AVIF file, decodes it with
# the host's WIC (NativeImageCodec), as Java2D's graphics did. build_aosp_gfx.sh runs it on the jar's
# classes, windows/test/heif/run.sh on android-all's for its test.
#
#   windows/scripts/redirect_decoders.sh <java> <classes dir>
#
# <java> and <classes dir> as the platform's java reads them (on MSYS2 a Windows path for the dir).
set -euo pipefail
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
JAVA=${1:?usage: redirect_decoders.sh <java> <classes dir>}
CLASSES=${2:?usage: redirect_decoders.sh <java> <classes dir>}
TOOL="$HERE/AospCallRedirect.java"
command -v cygpath > /dev/null 2>&1 && TOOL=$(cygpath -m "$TOOL")
TO=com/winapk/aospgfx/HostImageDecode
BF=android/graphics/BitmapFactory
ID=android/graphics/ImageDecoder
OPTS='Landroid/graphics/BitmapFactory$Options;'
BM=Landroid/graphics/Bitmap\;
SRC='Landroid/graphics/ImageDecoder$Source;'

"$JAVA" "$TOOL" "$CLASSES" $BF $BF \
    nativeDecodeByteArray "([BII${OPTS}JJ)$BM" $TO \
    nativeDecodeStream "(Ljava/io/InputStream;[BLandroid/graphics/Rect;${OPTS}JJ)$BM" \
    nativeDecodeFileDescriptor "(Ljava/io/FileDescriptor;Landroid/graphics/Rect;${OPTS}JJ)$BM"
# ImageDecoder's asset nCreate is left: Win.apk's AssetInputStream has no native Asset to hand it.
"$JAVA" "$TOOL" "$CLASSES" $ID $ID \
    nCreate "(Ljava/io/InputStream;[BZ${SRC})L$ID;" $TO \
    nCreate "(Ljava/io/FileDescriptor;JZ${SRC})L$ID;" \
    nDecodeBitmap "(JL$ID;ZIILandroid/graphics/Rect;ZIZZZJZ)$BM" \
    nGetSampledSize '(JI)Landroid/util/Size;' \
    nGetPadding '(JLandroid/graphics/Rect;)V' \
    nClose '(J)V' \
    nGetMimeType '(J)Ljava/lang/String;' \
    nGetColorSpace '(J)Landroid/graphics/ColorSpace;'
"$JAVA" "$TOOL" "$CLASSES" "$ID\$ByteArraySource" $ID nCreate "([BIIZ${SRC})L$ID;" $TO
"$JAVA" "$TOOL" "$CLASSES" "$ID\$ByteBufferSource" $ID nCreate "([BIIZ${SRC})L$ID;" $TO \
    nCreate "(Ljava/nio/ByteBuffer;IIZ${SRC})L$ID;"
