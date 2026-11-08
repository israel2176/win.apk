// java_io_open.h -- java.io's file opens with FILE_SHARE_DELETE (java_io_open.cpp).
#pragma once

#include <jni.h>

namespace winapk::host {

// Binds FileInputStream, FileOutputStream and RandomAccessFile's open0 to
// opens that let the file be renamed or deleted while open, as on Android.
// False (and a log line) when the JDK's own opens stay.
bool java_io_open_init(JNIEnv* env);

}  // namespace winapk::host
