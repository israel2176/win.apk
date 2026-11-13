// native_sensor.h -- libandroid's <android/sensor.h> for an app's native code:
// a sensor manager with no sensors (native_sensor.cpp).

#pragma once

namespace winapk::host {

// The ASensor* function `name`; null for other names.
void* native_sensor_resolve(const char* name);

}  // namespace winapk::host
