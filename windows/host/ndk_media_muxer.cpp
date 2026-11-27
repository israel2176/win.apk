// ndk_media_muxer.cpp -- libmediandk's AMediaMuxer for an app's native code, on the writers
// MediaMuxer stands on: mp4_writer.h for MUXER_OUTPUT_MPEG_4 and MUXER_OUTPUT_3GPP (H.264 and
// AAC), webm_writer.h for MUXER_OUTPUT_WEBM (VP8, VP9, Opus, Vorbis).
//
// NdkMediaMuxer.h's contract, as AOSP's MediaMuxer keeps it:
//  - AMediaMuxer_new takes the app's descriptor, open for writing: the muxer writes through a
//    duplicate of it, from the file's start, and the app may close its own at once; null for a
//    format it cannot write or a descriptor that is not open;
//  - addTrack, setOrientationHint and setLocation only before start, start only with a track,
//    writeSampleData only between start and stop; a call out of order is
//    AMEDIA_ERROR_INVALID_OPERATION, a bad argument AMEDIA_ERROR_INVALID_PARAMETER, a track the
//    format cannot hold AMEDIA_ERROR_UNSUPPORTED (addTrack returns these negative values);
//  - writeSampleData takes AMediaCodecBufferInfo's offset, size, time and flags as MediaCodec
//    gives them: BUFFER_FLAG_KEY_FRAME marks a sync sample, BUFFER_FLAG_CODEC_CONFIG codec data
//    (for MP4, the SPS and PPS a track's format lacked; never written as a sample);
//  - stop writes the index; a muxer deleted while started is stopped first, as MediaMuxer's
//    destructor does.
// What differs from a device: MP4 and 3GPP hold H.264 and AAC only (no HEVC, AV1, MPEG-4 part 2,
// H.263 or AMR), there is no HEIF or Ogg output, and AMediaMuxer_append (API 31) answers null,
// as for a file it cannot append to.
//
// No Windows call: the CRT's descriptor functions on Windows, POSIX's elsewhere, so that the
// muxer is checked on Linux too (test/ndk_muxer_test.cpp).

#include "native_media_format.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <sys/types.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "mp4_writer.h"
#include "webm_writer.h"
#include "winapk/guest_abi.h"

namespace winapk::host {

namespace {

// media_status_t (NdkMediaError.h).
constexpr int kOk = 0, kErrUnsupported = -10002, kErrInvalidObject = -10003, kErrInvalidParameter = -10004,
              kErrInvalidOperation = -10005, kErrIo = -10007;
// NdkMediaMuxer.h's OutputFormat.
constexpr int kMpeg4 = 0, kWebm = 1, kThreeGpp = 2, kHeif = 3, kOgg = 4;
// NdkMediaCodec.h's buffer flags.
constexpr uint32_t kFlagKeyFrame = 1, kFlagCodecConfig = 2;

struct BufferInfo {  // AMediaCodecBufferInfo
    int32_t offset;
    int32_t size;
    int64_t presentationTimeUs;
    uint32_t flags;
};

struct Muxer {
    std::mutex lock;
    int format = kMpeg4;
    std::unique_ptr<mp4::Writer> mp4;
    std::unique_ptr<webm::Writer> webm;
    enum State { kInitialized, kStarted, kStopped } state = kInitialized;
    std::vector<std::shared_ptr<MediaFormat>> tracks;
    int video_tracks = 0, audio_tracks = 0;
};

void log_once(const char* what, const std::string& detail) {
    static std::mutex lock;
    static std::vector<std::string> said;
    const std::string key = std::string(what) + detail;
    std::lock_guard<std::mutex> hold(lock);
    for (const auto& s : said)
        if (s == key) return;
    said.push_back(key);
    std::fprintf(stderr, "[win.apk] %s(\"%s\")\n", what, detail.c_str());
    std::fflush(stderr);
}

// A stdio file over a duplicate of the app's descriptor, at the file's start.
std::FILE* file_of(int fd) {
    if (fd < 0) return nullptr;
#ifdef _WIN32
    const int copy = _dup(fd);
    std::FILE* f = copy >= 0 ? _fdopen(copy, "wb") : nullptr;
    if (!f && copy >= 0) _close(copy);
    if (f && _fseeki64(f, 0, SEEK_SET) != 0) {
#else
    const int copy = dup(fd);
    std::FILE* f = copy >= 0 ? fdopen(copy, "wb") : nullptr;
    if (!f && copy >= 0) close(copy);
    if (f && fseeko(f, 0, SEEK_SET) != 0) {
#endif
        std::fclose(f);
        return nullptr;
    }
    return f;
}

int32_t int_of(MediaFormat* f, const char* key) {
    int32_t v = 0;
    return f->int32(key, &v) ? v : 0;
}

WINAPK_GUEST_ABI Muxer* AMediaMuxer_new(int fd, int format) {
    if (format != kMpeg4 && format != kWebm && format != kThreeGpp) {
        log_once("AMediaMuxer_new: no writer for the output format",
                 format == kHeif ? "HEIF" : format == kOgg ? "Ogg" : std::to_string(format));
        return nullptr;
    }
    std::FILE* file = file_of(fd);
    if (!file) return nullptr;
    auto m = std::make_unique<Muxer>();
    m->format = format;
    if (format == kWebm) m->webm = webm::Writer::adopt(file);
    else m->mp4 = mp4::Writer::adopt(file, format == kThreeGpp);
    if (!m->webm && !m->mp4) return nullptr;
    return m.release();
}

int stop_locked(Muxer* m) {
    if (m->state != Muxer::kStarted) return kErrInvalidOperation;
    m->state = Muxer::kStopped;
    const bool ok = m->webm ? m->webm->finish() : m->mp4->finish();
    return ok ? kOk : kErrIo;
}

WINAPK_GUEST_ABI int AMediaMuxer_delete(Muxer* m) {
    if (!m) return kErrInvalidObject;
    {
        std::lock_guard<std::mutex> hold(m->lock);
        if (m->state == Muxer::kStarted) stop_locked(m);
    }
    delete m;
    return kOk;
}

// The same file appended to (API 31): not something these writers do.
WINAPK_GUEST_ABI Muxer* AMediaMuxer_append(int, int mode) {
    log_once("AMediaMuxer_append: no appending to a written file; null", std::to_string(mode));
    return nullptr;
}

WINAPK_GUEST_ABI int AMediaMuxer_setLocation(Muxer* m, float latitude, float longitude) {
    if (!m) return kErrInvalidObject;
    if (latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180) return kErrInvalidParameter;
    std::lock_guard<std::mutex> hold(m->lock);
    if (m->state != Muxer::kInitialized) return kErrInvalidOperation;
    if (m->mp4) m->mp4->set_location(latitude, longitude);  // WebM has no place for it
    return kOk;
}

WINAPK_GUEST_ABI int AMediaMuxer_setOrientationHint(Muxer* m, int degrees) {
    if (!m) return kErrInvalidObject;
    if (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270) return kErrInvalidParameter;
    std::lock_guard<std::mutex> hold(m->lock);
    if (m->state != Muxer::kInitialized) return kErrInvalidOperation;
    if (m->mp4) m->mp4->set_orientation(degrees);  // nor for this
    return kOk;
}

WINAPK_GUEST_ABI ssize_t AMediaMuxer_addTrack(Muxer* m, MediaFormat* format) {
    if (!m) return kErrInvalidObject;
    if (!format) return kErrInvalidParameter;
    std::lock_guard<std::mutex> hold(m->lock);
    if (m->state != Muxer::kInitialized) return kErrInvalidOperation;
    const std::string mime = format->string("mime");
    const bool video = mime.rfind("video/", 0) == 0;
    int index = -1;
    if (m->webm) {
        // A WebM file has one video track and one audio track at most (MediaMuxer's rule).
        if (!webm::Writer::takes(mime) || (video ? m->video_tracks : m->audio_tracks) > 0) {
            log_once("AMediaMuxer_addTrack: not a track this WebM file takes", mime);
            return kErrUnsupported;
        }
        webm::TrackFormat t;
        t.mime = mime;
        t.width = int_of(format, "width");
        t.height = int_of(format, "height");
        t.sample_rate = int_of(format, "sample-rate");
        t.channels = int_of(format, "channel-count");
        t.csd0 = format->buffer("csd-0");
        t.csd1 = format->buffer("csd-1");
        t.csd2 = format->buffer("csd-2");
        index = m->webm->add_track(t);
    } else {
        if (mime != "video/avc" && mime != "audio/mp4a-latm") {
            log_once("AMediaMuxer_addTrack: MP4 holds H.264 and AAC here, not", mime);
            return kErrUnsupported;
        }
        mp4::TrackFormat t;
        t.video = video;
        t.width = int_of(format, "width");
        t.height = int_of(format, "height");
        t.sample_rate = int_of(format, "sample-rate");
        t.channels = int_of(format, "channel-count");
        t.bitrate = int_of(format, "bitrate");
        t.csd0 = format->buffer("csd-0");
        t.csd1 = format->buffer("csd-1");
        index = m->mp4->add_track(t);
    }
    if (index < 0) return kErrInvalidParameter;  // a size, a rate or codec data the track needs
    auto copy = std::make_shared<MediaFormat>();
    copy->copy_from(*format);
    m->tracks.push_back(copy);
    ++(video ? m->video_tracks : m->audio_tracks);
    return index;
}

WINAPK_GUEST_ABI int AMediaMuxer_start(Muxer* m) {
    if (!m) return kErrInvalidObject;
    std::lock_guard<std::mutex> hold(m->lock);
    if (m->state != Muxer::kInitialized || m->tracks.empty()) return kErrInvalidOperation;
    if (!(m->webm ? m->webm->start() : m->mp4->start())) return kErrIo;
    m->state = Muxer::kStarted;
    return kOk;
}

WINAPK_GUEST_ABI int AMediaMuxer_writeSampleData(Muxer* m, size_t track, const uint8_t* data,
                                                 const BufferInfo* info) {
    if (!m) return kErrInvalidObject;
    if (!data || !info || info->offset < 0 || info->size < 0) return kErrInvalidParameter;
    std::lock_guard<std::mutex> hold(m->lock);
    if (m->state != Muxer::kStarted) return kErrInvalidOperation;
    if (track >= m->tracks.size()) return kErrInvalidParameter;
    const uint8_t* p = data + info->offset;
    const size_t n = static_cast<size_t>(info->size);
    const bool key = (info->flags & kFlagKeyFrame) != 0, config = (info->flags & kFlagCodecConfig) != 0;
    const int t = static_cast<int>(track);
    const bool ok = m->webm ? m->webm->write(t, p, n, info->presentationTimeUs, key, config)
                            : m->mp4->write(t, p, n, info->presentationTimeUs, key, config);
    return ok ? kOk : kErrIo;
}

WINAPK_GUEST_ABI int AMediaMuxer_stop(Muxer* m) {
    if (!m) return kErrInvalidObject;
    std::lock_guard<std::mutex> hold(m->lock);
    return stop_locked(m);
}

WINAPK_GUEST_ABI ssize_t AMediaMuxer_getTrackCount(Muxer* m) {
    if (!m) return -1;
    std::lock_guard<std::mutex> hold(m->lock);
    return static_cast<ssize_t>(m->tracks.size());
}

// A new format the caller deletes, as AOSP's.
WINAPK_GUEST_ABI MediaFormat* AMediaMuxer_getTrackFormat(Muxer* m, size_t index) {
    if (!m) return nullptr;
    std::lock_guard<std::mutex> hold(m->lock);
    if (index >= m->tracks.size()) return nullptr;
    auto* f = new MediaFormat;
    f->copy_from(*m->tracks[index]);
    return f;
}

struct Function {
    const char* name;
    void* fn;
};
#define WINAPK_NDK_MUXER(fn) {#fn, reinterpret_cast<void*>(&fn)}
const Function kFunctions[] = {
    WINAPK_NDK_MUXER(AMediaMuxer_new),           WINAPK_NDK_MUXER(AMediaMuxer_delete),
    WINAPK_NDK_MUXER(AMediaMuxer_append),        WINAPK_NDK_MUXER(AMediaMuxer_setLocation),
    WINAPK_NDK_MUXER(AMediaMuxer_setOrientationHint), WINAPK_NDK_MUXER(AMediaMuxer_addTrack),
    WINAPK_NDK_MUXER(AMediaMuxer_start),         WINAPK_NDK_MUXER(AMediaMuxer_writeSampleData),
    WINAPK_NDK_MUXER(AMediaMuxer_stop),          WINAPK_NDK_MUXER(AMediaMuxer_getTrackCount),
    WINAPK_NDK_MUXER(AMediaMuxer_getTrackFormat),
};
#undef WINAPK_NDK_MUXER

}  // namespace

void* ndk_media_muxer_resolve(const char* name) {
    for (const Function& f : kFunctions) {
        if (std::strcmp(name, f.name) == 0) return f.fn;
    }
    return nullptr;
}

}  // namespace winapk::host
