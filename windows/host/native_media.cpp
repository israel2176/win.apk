// native_media.cpp -- libmediandk for an app's native code: AMediaFormat, the
// AMEDIAFORMAT_KEY_* variables, and an AMediaCodec and AMediaExtractor that say
// there is no codec.
//
// AMediaFormat is a map of typed values and is whole. The codecs behind
// AMediaCodec are not ported to native callers (Java's MediaCodec has them,
// native_media_codec.cpp): every AMediaCodec_create* answers null, which is
// what a device answers for a type it has no codec for, and callers fall back
// to their own software decoder or report the format unsupported. An
// AMediaExtractor can be made and refuses every source. Ten of the 56 most
// installed apps with native code import these; all of it went to the loader's
// missing-function stub, where AMediaFormat_new was null and setInt32 on it
// "worked".
//
// The AMEDIAFORMAT_KEY_* variables are here so that a
// library that imports them loads with the real key strings, as on a device:
// each is a `const char*` variable holding a key name (NdkMediaFormat.h),
// pointers on both sides. Bound to zeroed memory instead, every key was a
// null pointer. The names and strings are AOSP's
// (frameworks/av/media/ndk/NdkMediaFormat.cpp).

#include "native_media.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "winapk/guest_abi.h"

namespace winapk::host {

namespace {

// media_status_t (NdkMediaError.h).
constexpr int kMediaOk = 0, kErrUnsupported = -10002, kErrInvalidObject = -10003, kErrInvalidParameter = -10004;

// ---- AMediaFormat ---------------------------------------------------------------------
struct FormatValue {
    enum Kind { kInt32, kInt64, kFloat, kDouble, kSize, kString, kBuffer, kRect } kind = kInt32;
    int64_t i = 0;       // int32, int64, size
    double d = 0;        // float, double
    std::string bytes;   // string, buffer
    int32_t rect[4] = {};
};
struct MediaFormat {
    std::mutex lock;
    std::map<std::string, FormatValue> values;
    std::string text;  // what toString last returned: the caller keeps the pointer
};
FormatValue* find(MediaFormat* f, const char* name, FormatValue::Kind kind) {
    if (!f || !name) return nullptr;
    auto it = f->values.find(name);
    return it != f->values.end() && it->second.kind == kind ? &it->second : nullptr;
}
FormatValue* slot(MediaFormat* f, const char* name, FormatValue::Kind kind) {
    if (!f || !name) return nullptr;
    FormatValue& v = f->values[name];
    v = FormatValue{};
    v.kind = kind;
    return &v;
}

WINAPK_GUEST_ABI MediaFormat* AMediaFormat_new() { return new MediaFormat; }
WINAPK_GUEST_ABI int AMediaFormat_delete(MediaFormat* f) {
    delete f;
    return kMediaOk;
}
WINAPK_GUEST_ABI void AMediaFormat_clear(MediaFormat* f) {
    if (!f) return;
    std::lock_guard<std::mutex> hold(f->lock);
    f->values.clear();
}
WINAPK_GUEST_ABI int AMediaFormat_copy(MediaFormat* to, MediaFormat* from) {
    if (!to || !from) return kErrInvalidParameter;
    if (to == from) return kMediaOk;
    std::scoped_lock hold(to->lock, from->lock);
    to->values = from->values;
    return kMediaOk;
}
// AOSP's AMessage::debugString, near enough for a log line: "mime: string(video/avc), width: int32(1280)".
WINAPK_GUEST_ABI const char* AMediaFormat_toString(MediaFormat* f) {
    if (!f) return "";
    std::lock_guard<std::mutex> hold(f->lock);
    std::string out;
    char num[64];
    for (const auto& [name, v] : f->values) {
        if (!out.empty()) out += ", ";
        out += name + ": ";
        switch (v.kind) {
            case FormatValue::kInt32: std::snprintf(num, sizeof num, "int32(%d)", static_cast<int>(v.i)); out += num; break;
            case FormatValue::kInt64: std::snprintf(num, sizeof num, "int64(%" PRId64 ")", v.i); out += num; break;
            case FormatValue::kSize: std::snprintf(num, sizeof num, "size_t(%" PRId64 ")", v.i); out += num; break;
            case FormatValue::kFloat: std::snprintf(num, sizeof num, "float(%f)", v.d); out += num; break;
            case FormatValue::kDouble: std::snprintf(num, sizeof num, "double(%f)", v.d); out += num; break;
            case FormatValue::kString: out += "string(" + v.bytes + ")"; break;
            case FormatValue::kBuffer: std::snprintf(num, sizeof num, "data(%zu bytes)", v.bytes.size()); out += num; break;
            case FormatValue::kRect:
                std::snprintf(num, sizeof num, "Rect(%d, %d, %d, %d)", v.rect[0], v.rect[1], v.rect[2], v.rect[3]);
                out += num;
                break;
        }
    }
    f->text = out;
    return f->text.c_str();
}
#define WINAPK_FORMAT_NUMBER(Name, type, Kind, member)                                             \
    WINAPK_GUEST_ABI bool AMediaFormat_get##Name(MediaFormat* f, const char* name, type* out) {     \
        if (!f) return false;                                                                      \
        std::lock_guard<std::mutex> hold(f->lock);                                                 \
        const FormatValue* v = find(f, name, FormatValue::Kind);                                   \
        if (!v || !out) return false;                                                              \
        *out = static_cast<type>(v->member);                                                       \
        return true;                                                                               \
    }                                                                                              \
    WINAPK_GUEST_ABI void AMediaFormat_set##Name(MediaFormat* f, const char* name, type value) {    \
        if (!f) return;                                                                            \
        std::lock_guard<std::mutex> hold(f->lock);                                                 \
        if (FormatValue* v = slot(f, name, FormatValue::Kind)) v->member = value;                  \
    }
WINAPK_FORMAT_NUMBER(Int32, int32_t, kInt32, i)
WINAPK_FORMAT_NUMBER(Int64, int64_t, kInt64, i)
WINAPK_FORMAT_NUMBER(Size, size_t, kSize, i)
WINAPK_FORMAT_NUMBER(Float, float, kFloat, d)
WINAPK_FORMAT_NUMBER(Double, double, kDouble, d)
#undef WINAPK_FORMAT_NUMBER
// The string and the buffer stay the format's: valid until the key is set again or the format goes.
WINAPK_GUEST_ABI bool AMediaFormat_getString(MediaFormat* f, const char* name, const char** out) {
    if (!f) return false;
    std::lock_guard<std::mutex> hold(f->lock);
    const FormatValue* v = find(f, name, FormatValue::kString);
    if (!v || !out) return false;
    *out = v->bytes.c_str();
    return true;
}
WINAPK_GUEST_ABI void AMediaFormat_setString(MediaFormat* f, const char* name, const char* value) {
    if (!f) return;
    std::lock_guard<std::mutex> hold(f->lock);
    if (FormatValue* v = slot(f, name, FormatValue::kString)) v->bytes = value ? value : "";
}
WINAPK_GUEST_ABI bool AMediaFormat_getBuffer(MediaFormat* f, const char* name, void** data, size_t* size) {
    if (!f) return false;
    std::lock_guard<std::mutex> hold(f->lock);
    FormatValue* v = find(f, name, FormatValue::kBuffer);
    if (!v || !data || !size) return false;
    *data = v->bytes.data();
    *size = v->bytes.size();
    return true;
}
WINAPK_GUEST_ABI void AMediaFormat_setBuffer(MediaFormat* f, const char* name, const void* data, size_t size) {
    if (!f) return;
    std::lock_guard<std::mutex> hold(f->lock);
    if (FormatValue* v = slot(f, name, FormatValue::kBuffer)) v->bytes.assign(static_cast<const char*>(data), data ? size : 0);
}
WINAPK_GUEST_ABI bool AMediaFormat_getRect(MediaFormat* f, const char* name, int32_t* l, int32_t* t, int32_t* r,
                                           int32_t* b) {
    if (!f) return false;
    std::lock_guard<std::mutex> hold(f->lock);
    const FormatValue* v = find(f, name, FormatValue::kRect);
    if (!v || !l || !t || !r || !b) return false;
    *l = v->rect[0], *t = v->rect[1], *r = v->rect[2], *b = v->rect[3];
    return true;
}
WINAPK_GUEST_ABI void AMediaFormat_setRect(MediaFormat* f, const char* name, int32_t l, int32_t t, int32_t r,
                                           int32_t b) {
    if (!f) return;
    std::lock_guard<std::mutex> hold(f->lock);
    if (FormatValue* v = slot(f, name, FormatValue::kRect)) v->rect[0] = l, v->rect[1] = t, v->rect[2] = r, v->rect[3] = b;
}

// ---- AMediaCodec, AMediaExtractor: none ---------------------------------------------------
// No codec of any type or name, said once in the log so that "no video" has a line to find.
void* no_codec(const char* what, const char* name) {
    static std::mutex lock;
    static std::map<std::string, bool> said;
    std::lock_guard<std::mutex> hold(lock);
    const std::string key = std::string(what) + (name ? name : "");
    if (!said[key]) {
        said[key] = true;
        std::fprintf(stderr, "[win.apk] %s(\"%s\"): native code asked the NDK for a codec; there is none here "
                             "(Java's MediaCodec has them), so it gets null as for an unsupported type\n",
                     what, name ? name : "");
        std::fflush(stderr);
    }
    return nullptr;
}
WINAPK_GUEST_ABI void* AMediaCodec_createDecoderByType(const char* mime) {
    return no_codec("AMediaCodec_createDecoderByType", mime);
}
WINAPK_GUEST_ABI void* AMediaCodec_createEncoderByType(const char* mime) {
    return no_codec("AMediaCodec_createEncoderByType", mime);
}
WINAPK_GUEST_ABI void* AMediaCodec_createCodecByName(const char* name) {
    return no_codec("AMediaCodec_createCodecByName", name);
}
// On the null codec a careless caller goes on with: an error, where the stub's 0 was AMEDIA_OK
// (and "buffer 0 is ready" from the dequeue calls).
WINAPK_GUEST_ABI int codec_invalid() { return kErrInvalidObject; }
WINAPK_GUEST_ABI int64_t codec_no_buffer() { return -1; }  // AMEDIACODEC_INFO_TRY_AGAIN_LATER
WINAPK_GUEST_ABI void* codec_null() { return nullptr; }

struct MediaExtractor {
    int unused = 0;
};
WINAPK_GUEST_ABI MediaExtractor* AMediaExtractor_new() { return new MediaExtractor; }
WINAPK_GUEST_ABI int AMediaExtractor_delete(MediaExtractor* e) {
    delete e;
    return kMediaOk;
}
WINAPK_GUEST_ABI int extractor_unsupported() { return kErrUnsupported; }
WINAPK_GUEST_ABI size_t AMediaExtractor_getTrackCount(MediaExtractor*) { return 0; }
WINAPK_GUEST_ABI bool AMediaExtractor_advance(MediaExtractor*) { return false; }
WINAPK_GUEST_ABI int64_t extractor_none() { return -1; }  // no sample: size, time, track index

struct Function {
    const char* name;
    void* fn;
};
#define WINAPK_MEDIA(fn) {#fn, reinterpret_cast<void*>(&fn)}
#define WINAPK_MEDIA_AS(name, fn) {name, reinterpret_cast<void*>(&fn)}
const Function kFunctions[] = {
    WINAPK_MEDIA(AMediaFormat_new), WINAPK_MEDIA(AMediaFormat_delete), WINAPK_MEDIA(AMediaFormat_clear),
    WINAPK_MEDIA(AMediaFormat_copy), WINAPK_MEDIA(AMediaFormat_toString),
    WINAPK_MEDIA(AMediaFormat_getInt32), WINAPK_MEDIA(AMediaFormat_setInt32),
    WINAPK_MEDIA(AMediaFormat_getInt64), WINAPK_MEDIA(AMediaFormat_setInt64),
    WINAPK_MEDIA(AMediaFormat_getSize), WINAPK_MEDIA(AMediaFormat_setSize),
    WINAPK_MEDIA(AMediaFormat_getFloat), WINAPK_MEDIA(AMediaFormat_setFloat),
    WINAPK_MEDIA(AMediaFormat_getDouble), WINAPK_MEDIA(AMediaFormat_setDouble),
    WINAPK_MEDIA(AMediaFormat_getString), WINAPK_MEDIA(AMediaFormat_setString),
    WINAPK_MEDIA(AMediaFormat_getBuffer), WINAPK_MEDIA(AMediaFormat_setBuffer),
    WINAPK_MEDIA(AMediaFormat_getRect), WINAPK_MEDIA(AMediaFormat_setRect),
    WINAPK_MEDIA(AMediaCodec_createDecoderByType), WINAPK_MEDIA(AMediaCodec_createEncoderByType),
    WINAPK_MEDIA(AMediaCodec_createCodecByName),
    WINAPK_MEDIA_AS("AMediaCodec_delete", codec_invalid), WINAPK_MEDIA_AS("AMediaCodec_configure", codec_invalid),
    WINAPK_MEDIA_AS("AMediaCodec_start", codec_invalid), WINAPK_MEDIA_AS("AMediaCodec_stop", codec_invalid),
    WINAPK_MEDIA_AS("AMediaCodec_flush", codec_invalid),
    WINAPK_MEDIA_AS("AMediaCodec_queueInputBuffer", codec_invalid),
    WINAPK_MEDIA_AS("AMediaCodec_queueSecureInputBuffer", codec_invalid),
    WINAPK_MEDIA_AS("AMediaCodec_releaseOutputBuffer", codec_invalid),
    WINAPK_MEDIA_AS("AMediaCodec_releaseOutputBufferAtTime", codec_invalid),
    WINAPK_MEDIA_AS("AMediaCodec_setOutputSurface", codec_invalid),
    WINAPK_MEDIA_AS("AMediaCodec_setParameters", codec_invalid),
    WINAPK_MEDIA_AS("AMediaCodec_setAsyncNotifyCallback", codec_invalid),
    WINAPK_MEDIA_AS("AMediaCodec_signalEndOfInputStream", codec_invalid),
    WINAPK_MEDIA_AS("AMediaCodec_createInputSurface", codec_invalid),
    WINAPK_MEDIA_AS("AMediaCodec_setInputSurface", codec_invalid),
    WINAPK_MEDIA_AS("AMediaCodec_dequeueInputBuffer", codec_no_buffer),
    WINAPK_MEDIA_AS("AMediaCodec_dequeueOutputBuffer", codec_no_buffer),
    WINAPK_MEDIA_AS("AMediaCodec_getInputBuffer", codec_null),
    WINAPK_MEDIA_AS("AMediaCodec_getOutputBuffer", codec_null),
    WINAPK_MEDIA_AS("AMediaCodec_getOutputFormat", codec_null),
    WINAPK_MEDIA_AS("AMediaCodec_getInputFormat", codec_null),
    WINAPK_MEDIA_AS("AMediaCodec_getBufferFormat", codec_null),
    WINAPK_MEDIA(AMediaExtractor_new), WINAPK_MEDIA(AMediaExtractor_delete),
    WINAPK_MEDIA_AS("AMediaExtractor_setDataSource", extractor_unsupported),
    WINAPK_MEDIA_AS("AMediaExtractor_setDataSourceFd", extractor_unsupported),
    WINAPK_MEDIA_AS("AMediaExtractor_setDataSourceCustom", extractor_unsupported),
    WINAPK_MEDIA_AS("AMediaExtractor_selectTrack", extractor_unsupported),
    WINAPK_MEDIA_AS("AMediaExtractor_unselectTrack", extractor_unsupported),
    WINAPK_MEDIA_AS("AMediaExtractor_seekTo", extractor_unsupported),
    WINAPK_MEDIA(AMediaExtractor_getTrackCount), WINAPK_MEDIA(AMediaExtractor_advance),
    WINAPK_MEDIA_AS("AMediaExtractor_getTrackFormat", codec_null),
    WINAPK_MEDIA_AS("AMediaExtractor_getFileFormat", codec_null),
    WINAPK_MEDIA_AS("AMediaExtractor_readSampleData", extractor_none),
    WINAPK_MEDIA_AS("AMediaExtractor_getSampleSize", extractor_none),
    WINAPK_MEDIA_AS("AMediaExtractor_getSampleTime", extractor_none),
    WINAPK_MEDIA_AS("AMediaExtractor_getSampleTrackIndex", extractor_none),
};

struct FormatKey {
    const char* name;
    const char* value;  // the exported variable itself
};

FormatKey kFormatKeys[] = {
    {"AMEDIAFORMAT_KEY_AAC_DRC_ATTENUATION_FACTOR", "aac-drc-cut-level"},
    {"AMEDIAFORMAT_KEY_AAC_DRC_BOOST_FACTOR", "aac-drc-boost-level"},
    {"AMEDIAFORMAT_KEY_AAC_DRC_HEAVY_COMPRESSION", "aac-drc-heavy-compression"},
    {"AMEDIAFORMAT_KEY_AAC_DRC_TARGET_REFERENCE_LEVEL", "aac-target-ref-level"},
    {"AMEDIAFORMAT_KEY_AAC_ENCODED_TARGET_LEVEL", "aac-encoded-target-level"},
    {"AMEDIAFORMAT_KEY_AAC_MAX_OUTPUT_CHANNEL_COUNT", "aac-max-output-channel_count"},
    {"AMEDIAFORMAT_KEY_AAC_PROFILE", "aac-profile"},
    {"AMEDIAFORMAT_KEY_AAC_SBR_MODE", "aac-sbr-mode"},
    {"AMEDIAFORMAT_KEY_ALBUM", "album"},
    {"AMEDIAFORMAT_KEY_ALBUMART", "albumart"},
    {"AMEDIAFORMAT_KEY_ALBUMARTIST", "albumartist"},
    {"AMEDIAFORMAT_KEY_ALLOW_FRAME_DROP", "allow-frame-drop"},
    {"AMEDIAFORMAT_KEY_ARTIST", "artist"},
    {"AMEDIAFORMAT_KEY_AUDIO_PRESENTATION_INFO", "audio-presentation-info"},
    {"AMEDIAFORMAT_KEY_AUDIO_PRESENTATION_PRESENTATION_ID", "audio-presentation-presentation-id"},
    {"AMEDIAFORMAT_KEY_AUDIO_PRESENTATION_PROGRAM_ID", "audio-presentation-program-id"},
    {"AMEDIAFORMAT_KEY_AUDIO_SESSION_ID", "audio-session-id"},
    {"AMEDIAFORMAT_KEY_AUTHOR", "author"},
    {"AMEDIAFORMAT_KEY_BITRATE_MODE", "bitrate-mode"},
    {"AMEDIAFORMAT_KEY_BIT_RATE", "bitrate"},
    {"AMEDIAFORMAT_KEY_BITS_PER_SAMPLE", "bits-per-sample"},
    {"AMEDIAFORMAT_KEY_CAPTURE_RATE", "capture-rate"},
    {"AMEDIAFORMAT_KEY_CDTRACKNUMBER", "cdtracknum"},
    {"AMEDIAFORMAT_KEY_CHANNEL_COUNT", "channel-count"},
    {"AMEDIAFORMAT_KEY_CHANNEL_MASK", "channel-mask"},
    {"AMEDIAFORMAT_KEY_COLOR_FORMAT", "color-format"},
    {"AMEDIAFORMAT_KEY_COLOR_RANGE", "color-range"},
    {"AMEDIAFORMAT_KEY_COLOR_STANDARD", "color-standard"},
    {"AMEDIAFORMAT_KEY_COLOR_TRANSFER", "color-transfer"},
    {"AMEDIAFORMAT_KEY_COMPILATION", "compilation"},
    {"AMEDIAFORMAT_KEY_COMPLEXITY", "complexity"},
    {"AMEDIAFORMAT_KEY_COMPOSER", "composer"},
    {"AMEDIAFORMAT_KEY_CREATE_INPUT_SURFACE_SUSPENDED", "create-input-buffers-suspended"},
    {"AMEDIAFORMAT_KEY_CRYPTO_DEFAULT_IV_SIZE", "crypto-default-iv-size"},
    {"AMEDIAFORMAT_KEY_CRYPTO_ENCRYPTED_BYTE_BLOCK", "crypto-encrypted-byte-block"},
    {"AMEDIAFORMAT_KEY_CRYPTO_ENCRYPTED_SIZES", "crypto-encrypted-sizes"},
    {"AMEDIAFORMAT_KEY_CRYPTO_IV", "crypto-iv"},
    {"AMEDIAFORMAT_KEY_CRYPTO_KEY", "crypto-key"},
    {"AMEDIAFORMAT_KEY_CRYPTO_MODE", "crypto-mode"},
    {"AMEDIAFORMAT_KEY_CRYPTO_PLAIN_SIZES", "crypto-plain-sizes"},
    {"AMEDIAFORMAT_KEY_CRYPTO_SKIP_BYTE_BLOCK", "crypto-skip-byte-block"},
    {"AMEDIAFORMAT_KEY_CSD", "csd"},
    {"AMEDIAFORMAT_KEY_CSD_0", "csd-0"},
    {"AMEDIAFORMAT_KEY_CSD_1", "csd-1"},
    {"AMEDIAFORMAT_KEY_CSD_2", "csd-2"},
    {"AMEDIAFORMAT_KEY_CSD_AVC", "csd-avc"},
    {"AMEDIAFORMAT_KEY_CSD_HEVC", "csd-hevc"},
    {"AMEDIAFORMAT_KEY_D263", "d263"},
    {"AMEDIAFORMAT_KEY_DATE", "date"},
    {"AMEDIAFORMAT_KEY_DISCNUMBER", "discnum"},
    {"AMEDIAFORMAT_KEY_DISPLAY_CROP", "crop"},
    {"AMEDIAFORMAT_KEY_DISPLAY_HEIGHT", "display-height"},
    {"AMEDIAFORMAT_KEY_DISPLAY_WIDTH", "display-width"},
    {"AMEDIAFORMAT_KEY_DURATION", "durationUs"},
    {"AMEDIAFORMAT_KEY_ENCODER_DELAY", "encoder-delay"},
    {"AMEDIAFORMAT_KEY_ENCODER_PADDING", "encoder-padding"},
    {"AMEDIAFORMAT_KEY_ESDS", "esds"},
    {"AMEDIAFORMAT_KEY_EXIF_OFFSET", "exif-offset"},
    {"AMEDIAFORMAT_KEY_EXIF_SIZE", "exif-size"},
    {"AMEDIAFORMAT_KEY_FLAC_COMPRESSION_LEVEL", "flac-compression-level"},
    {"AMEDIAFORMAT_KEY_FRAME_COUNT", "frame-count"},
    {"AMEDIAFORMAT_KEY_FRAME_RATE", "frame-rate"},
    {"AMEDIAFORMAT_KEY_GENRE", "genre"},
    {"AMEDIAFORMAT_KEY_GRID_COLUMNS", "grid-cols"},
    {"AMEDIAFORMAT_KEY_GRID_ROWS", "grid-rows"},
    {"AMEDIAFORMAT_KEY_HAPTIC_CHANNEL_COUNT", "haptic-channel-count"},
    {"AMEDIAFORMAT_KEY_HDR_STATIC_INFO", "hdr-static-info"},
    {"AMEDIAFORMAT_KEY_HDR10_PLUS_INFO", "hdr10-plus-info"},
    {"AMEDIAFORMAT_KEY_HEIGHT", "height"},
    {"AMEDIAFORMAT_KEY_ICC_PROFILE", "icc-profile"},
    {"AMEDIAFORMAT_KEY_IMPORTANCE", "importance"},
    {"AMEDIAFORMAT_KEY_INTRA_REFRESH_PERIOD", "intra-refresh-period"},
    {"AMEDIAFORMAT_KEY_IS_ADTS", "is-adts"},
    {"AMEDIAFORMAT_KEY_IS_AUTOSELECT", "is-autoselect"},
    {"AMEDIAFORMAT_KEY_IS_DEFAULT", "is-default"},
    {"AMEDIAFORMAT_KEY_IS_FORCED_SUBTITLE", "is-forced-subtitle"},
    {"AMEDIAFORMAT_KEY_IS_SYNC_FRAME", "is-sync-frame"},
    {"AMEDIAFORMAT_KEY_I_FRAME_INTERVAL", "i-frame-interval"},
    {"AMEDIAFORMAT_KEY_LANGUAGE", "language"},
    {"AMEDIAFORMAT_KEY_LAST_SAMPLE_INDEX_IN_CHUNK", "last-sample-index-in-chunk"},
    {"AMEDIAFORMAT_KEY_LATENCY", "latency"},
    {"AMEDIAFORMAT_KEY_LEVEL", "level"},
    {"AMEDIAFORMAT_KEY_LOCATION", "location"},
    {"AMEDIAFORMAT_KEY_LOOP", "loop"},
    {"AMEDIAFORMAT_KEY_LOW_LATENCY", "low-latency"},
    {"AMEDIAFORMAT_KEY_LYRICIST", "lyricist"},
    {"AMEDIAFORMAT_KEY_MANUFACTURER", "manufacturer"},
    {"AMEDIAFORMAT_KEY_MAX_BIT_RATE", "max-bitrate"},
    {"AMEDIAFORMAT_KEY_MAX_B_FRAMES", "max-bframes"},
    {"AMEDIAFORMAT_KEY_MAX_FPS_TO_ENCODER", "max-fps-to-encoder"},
    {"AMEDIAFORMAT_KEY_MAX_HEIGHT", "max-height"},
    {"AMEDIAFORMAT_KEY_MAX_INPUT_SIZE", "max-input-size"},
    {"AMEDIAFORMAT_KEY_BUFFER_BATCH_MAX_OUTPUT_SIZE", "buffer-batch-max-output-size"},
    {"AMEDIAFORMAT_KEY_BUFFER_BATCH_THRESHOLD_OUTPUT_SIZE", "buffer-batch-threshold-output-size"},
    {"AMEDIAFORMAT_KEY_MAX_PTS_GAP_TO_ENCODER", "max-pts-gap-to-encoder"},
    {"AMEDIAFORMAT_KEY_MAX_WIDTH", "max-width"},
    {"AMEDIAFORMAT_KEY_MIME", "mime"},
    {"AMEDIAFORMAT_KEY_MPEG_USER_DATA", "mpeg-user-data"},
    {"AMEDIAFORMAT_KEY_MPEG2_STREAM_HEADER", "mpeg2-stream-header"},
    {"AMEDIAFORMAT_KEY_MPEGH_COMPATIBLE_SETS", "mpegh-compatible-sets"},
    {"AMEDIAFORMAT_KEY_MPEGH_PROFILE_LEVEL_INDICATION", "mpegh-profile-level-indication"},
    {"AMEDIAFORMAT_KEY_MPEGH_REFERENCE_CHANNEL_LAYOUT", "mpegh-reference-channel-layout"},
    {"AMEDIAFORMAT_KEY_OPERATING_RATE", "operating-rate"},
    {"AMEDIAFORMAT_KEY_PCM_ENCODING", "pcm-encoding"},
    {"AMEDIAFORMAT_KEY_PICTURE_TYPE", "picture-type"},
    {"AMEDIAFORMAT_KEY_PRIORITY", "priority"},
    {"AMEDIAFORMAT_KEY_PROFILE", "profile"},
    {"AMEDIAFORMAT_KEY_PCM_BIG_ENDIAN", "pcm-big-endian"},
    {"AMEDIAFORMAT_KEY_PSSH", "pssh"},
    {"AMEDIAFORMAT_KEY_PUSH_BLANK_BUFFERS_ON_STOP", "push-blank-buffers-on-shutdown"},
    {"AMEDIAFORMAT_KEY_REPEAT_PREVIOUS_FRAME_AFTER", "repeat-previous-frame-after"},
    {"AMEDIAFORMAT_KEY_ROTATION", "rotation-degrees"},
    {"AMEDIAFORMAT_KEY_SAMPLE_FILE_OFFSET", "sample-file-offset"},
    {"AMEDIAFORMAT_KEY_SAMPLE_RATE", "sample-rate"},
    {"AMEDIAFORMAT_KEY_SAMPLE_TIME_BEFORE_APPEND", "sample-time-before-append"},
    {"AMEDIAFORMAT_KEY_SAR_HEIGHT", "sar-height"},
    {"AMEDIAFORMAT_KEY_SAR_WIDTH", "sar-width"},
    {"AMEDIAFORMAT_KEY_SEI", "sei"},
    {"AMEDIAFORMAT_KEY_SLICE_HEIGHT", "slice-height"},
    {"AMEDIAFORMAT_KEY_SLOW_MOTION_MARKERS", "slow-motion-markers"},
    {"AMEDIAFORMAT_KEY_STRIDE", "stride"},
    {"AMEDIAFORMAT_KEY_TARGET_TIME", "target-time"},
    {"AMEDIAFORMAT_KEY_TEMPORAL_LAYER_COUNT", "temporal-layer-count"},
    {"AMEDIAFORMAT_KEY_TEMPORAL_LAYER_ID", "temporal-layer-id"},
    {"AMEDIAFORMAT_KEY_TEMPORAL_LAYERING", "ts-schema"},
    {"AMEDIAFORMAT_KEY_TEXT_FORMAT_DATA", "text-format-data"},
    {"AMEDIAFORMAT_KEY_THUMBNAIL_CSD_AV1C", "thumbnail-csd-av1c"},
    {"AMEDIAFORMAT_KEY_THUMBNAIL_CSD_HEVC", "thumbnail-csd-hevc"},
    {"AMEDIAFORMAT_KEY_THUMBNAIL_HEIGHT", "thumbnail-height"},
    {"AMEDIAFORMAT_KEY_THUMBNAIL_TIME", "thumbnail-time"},
    {"AMEDIAFORMAT_KEY_THUMBNAIL_WIDTH", "thumbnail-width"},
    {"AMEDIAFORMAT_KEY_TILE_HEIGHT", "tile-height"},
    {"AMEDIAFORMAT_KEY_TILE_WIDTH", "tile-width"},
    {"AMEDIAFORMAT_KEY_TIME_US", "timeUs"},
    {"AMEDIAFORMAT_KEY_TITLE", "title"},
    {"AMEDIAFORMAT_KEY_TRACK_ID", "track-id"},
    {"AMEDIAFORMAT_KEY_TRACK_INDEX", "track-index"},
    {"AMEDIAFORMAT_KEY_VALID_SAMPLES", "valid-samples"},
    {"AMEDIAFORMAT_KEY_VIDEO_ENCODING_STATISTICS_LEVEL", "video-encoding-statistics-level"},
    {"AMEDIAFORMAT_KEY_VIDEO_QP_AVERAGE", "video-qp-average"},
    {"AMEDIAFORMAT_KEY_WIDTH", "width"},
    {"AMEDIAFORMAT_KEY_XMP_OFFSET", "xmp-offset"},
    {"AMEDIAFORMAT_KEY_XMP_SIZE", "xmp-size"},
    {"AMEDIAFORMAT_KEY_YEAR", "year"},
};

}  // namespace

void* native_media_resolve(const char* name) {
    if (std::strncmp(name, "AMedia", 6) == 0) {
        for (const Function& f : kFunctions) {
            if (std::strcmp(name, f.name) == 0) return f.fn;
        }
        return nullptr;
    }
    if (std::strncmp(name, "AMEDIAFORMAT_KEY_", 17) != 0) return nullptr;
    for (FormatKey& k : kFormatKeys) {
        if (std::strcmp(name, k.name) == 0) return &k.value;
    }
    return nullptr;
}

}  // namespace winapk::host
