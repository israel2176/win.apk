// native_media.h -- libmediandk for an app's native code: AMediaFormat, the
// AMEDIAFORMAT_KEY_* data symbols and the crypto answers (native_media.cpp),
// AMediaCodec, AMediaExtractor and AMediaDataSource (ndk_media_codec.cpp),
// AMediaMuxer (ndk_media_muxer.cpp).

#pragma once

namespace winapk::host {

// The function or the AMEDIAFORMAT_KEY_* variable `name`; null for other names.
void* native_media_resolve(const char* name);

}  // namespace winapk::host
