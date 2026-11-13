// native_media.h -- libmediandk for an app's native code: AMediaFormat, the
// AMEDIAFORMAT_KEY_* data symbols, and an AMediaCodec / AMediaExtractor that
// answer "no codec" (native_media.cpp).

#pragma once

namespace winapk::host {

// The function or the AMEDIAFORMAT_KEY_* variable `name`; null for other names.
void* native_media_resolve(const char* name);

}  // namespace winapk::host
