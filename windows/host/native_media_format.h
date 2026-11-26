// native_media_format.h -- AMediaFormat's object, shared by native_media.cpp (the
// AMediaFormat_* functions) and ndk_media_codec.cpp (the formats a codec and an
// extractor hand out, which the app reads and deletes with those functions).

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace winapk::host {

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

    // For the host's own use (the app's calls are native_media.cpp's).
    void set_int32(const std::string& key, int32_t v) { put(key, number(FormatValue::kInt32, v)); }
    void set_int64(const std::string& key, int64_t v) { put(key, number(FormatValue::kInt64, v)); }
    void set_float(const std::string& key, float v) {
        FormatValue f;
        f.kind = FormatValue::kFloat;
        f.d = v;
        put(key, f);
    }
    void set_string(const std::string& key, const std::string& v) { put(key, bytes(FormatValue::kString, v)); }
    void set_buffer(const std::string& key, const std::vector<uint8_t>& v) {
        put(key, bytes(FormatValue::kBuffer, std::string(v.begin(), v.end())));
    }
    bool int32(const std::string& key, int32_t* out) {
        std::lock_guard<std::mutex> hold(lock);
        auto it = values.find(key);
        if (it == values.end()) return false;
        // AOSP's findInt32 takes an int32 only; an int64 is read as one too, as MediaFormat.getInteger
        // forgives in Java, since native callers mix them up for sizes.
        if (it->second.kind != FormatValue::kInt32 && it->second.kind != FormatValue::kInt64) return false;
        *out = static_cast<int32_t>(it->second.i);
        return true;
    }
    std::string string(const std::string& key) {
        std::lock_guard<std::mutex> hold(lock);
        auto it = values.find(key);
        return it != values.end() && it->second.kind == FormatValue::kString ? it->second.bytes : std::string();
    }
    std::vector<uint8_t> buffer(const std::string& key) {
        std::lock_guard<std::mutex> hold(lock);
        auto it = values.find(key);
        if (it == values.end() || it->second.kind != FormatValue::kBuffer) return {};
        return std::vector<uint8_t>(it->second.bytes.begin(), it->second.bytes.end());
    }
    void copy_from(MediaFormat& other) {
        std::scoped_lock hold(lock, other.lock);
        values = other.values;
    }

private:
    static FormatValue number(FormatValue::Kind kind, int64_t v) {
        FormatValue f;
        f.kind = kind;
        f.i = v;
        return f;
    }
    static FormatValue bytes(FormatValue::Kind kind, std::string v) {
        FormatValue f;
        f.kind = kind;
        f.bytes = std::move(v);
        return f;
    }
    void put(const std::string& key, FormatValue v) {
        std::lock_guard<std::mutex> hold(lock);
        values[key] = std::move(v);
    }
};

// ndk_media_codec.cpp's AMediaCodec_*, AMediaExtractor_* and AMediaDataSource_* by name; null for others.
void* ndk_media_codec_resolve(const char* name);

}  // namespace winapk::host
