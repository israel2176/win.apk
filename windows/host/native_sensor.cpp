// native_sensor.cpp -- libandroid's <android/sensor.h>: a sensor manager with no sensors.
//
// A PC has no accelerometer or gyroscope the runtime reads, which is a device
// whose sensor list is empty: the manager exists, its list has no entry,
// there is no default sensor of any type, and an event queue can be made and
// never has an event. Java's SensorManager says the same. Ten of the 56 most
// installed apps with native code import these (game engines, ads and
// anti-fraud SDKs); they went to the loader's missing-function stub, where
// ASensorManager_getInstance was null and the list call left its out-pointer
// unwritten, with a count nobody set.

#include "native_sensor.h"

#include <cstdint>
#include <cstring>

#include "winapk/guest_abi.h"

namespace winapk::host {

namespace {

struct SensorManager {
    int unused = 0;
};
struct SensorEventQueue {
    int unused = 0;
};
SensorManager g_manager;
void* g_no_sensors[1] = {nullptr};  // ASensorList: an array of ASensor const*

WINAPK_GUEST_ABI SensorManager* ASensorManager_getInstance() { return &g_manager; }
WINAPK_GUEST_ABI SensorManager* ASensorManager_getInstanceForPackage(const char*) { return &g_manager; }
WINAPK_GUEST_ABI int ASensorManager_getSensorList(SensorManager*, void*** list) {
    if (list) *list = g_no_sensors;
    return 0;
}
WINAPK_GUEST_ABI int64_t ASensorManager_getDynamicSensorList(SensorManager*, void*** list) {
    if (list) *list = g_no_sensors;
    return 0;
}
WINAPK_GUEST_ABI const void* ASensorManager_getDefaultSensor(SensorManager*, int) { return nullptr; }
WINAPK_GUEST_ABI const void* ASensorManager_getDefaultSensorEx(SensorManager*, int, bool) { return nullptr; }
// The looper is not told of the queue: nothing will ever be written to it.
WINAPK_GUEST_ABI SensorEventQueue* ASensorManager_createEventQueue(SensorManager*, void*, int, void*, void*) {
    return new SensorEventQueue;
}
WINAPK_GUEST_ABI int ASensorManager_destroyEventQueue(SensorManager*, SensorEventQueue* queue) {
    delete queue;
    return 0;
}
// Enabling a sensor that does not exist: -EINVAL, as the sensor service answers a bad handle.
WINAPK_GUEST_ABI int sensor_einval() { return -22; }
WINAPK_GUEST_ABI int sensor_zero() { return 0; }
WINAPK_GUEST_ABI int64_t ASensorEventQueue_getEvents(SensorEventQueue*, void*, size_t) { return 0; }
// What is asked of a sensor, for a caller that asks without one.
WINAPK_GUEST_ABI const char* sensor_no_text() { return nullptr; }
WINAPK_GUEST_ABI float sensor_no_number() { return 0.0f; }

struct Function {
    const char* name;
    void* fn;
};
#define WINAPK_SENSOR(fn) {#fn, reinterpret_cast<void*>(&fn)}
#define WINAPK_SENSOR_AS(name, fn) {name, reinterpret_cast<void*>(&fn)}
const Function kFunctions[] = {
    WINAPK_SENSOR(ASensorManager_getInstance),
    WINAPK_SENSOR(ASensorManager_getInstanceForPackage),
    WINAPK_SENSOR(ASensorManager_getSensorList),
    WINAPK_SENSOR(ASensorManager_getDynamicSensorList),
    WINAPK_SENSOR(ASensorManager_getDefaultSensor),
    WINAPK_SENSOR(ASensorManager_getDefaultSensorEx),
    WINAPK_SENSOR(ASensorManager_createEventQueue),
    WINAPK_SENSOR(ASensorManager_destroyEventQueue),
    WINAPK_SENSOR_AS("ASensorEventQueue_enableSensor", sensor_einval),
    WINAPK_SENSOR_AS("ASensorEventQueue_registerSensor", sensor_einval),
    WINAPK_SENSOR_AS("ASensorEventQueue_setEventRate", sensor_einval),
    WINAPK_SENSOR_AS("ASensorEventQueue_disableSensor", sensor_zero),
    WINAPK_SENSOR_AS("ASensorEventQueue_hasEvents", sensor_zero),
    WINAPK_SENSOR_AS("ASensorEventQueue_requestAdditionalInfoEvents", sensor_einval),
    WINAPK_SENSOR(ASensorEventQueue_getEvents),
    WINAPK_SENSOR_AS("ASensor_getName", sensor_no_text),
    WINAPK_SENSOR_AS("ASensor_getVendor", sensor_no_text),
    WINAPK_SENSOR_AS("ASensor_getStringType", sensor_no_text),
    WINAPK_SENSOR_AS("ASensor_getType", sensor_zero),
    WINAPK_SENSOR_AS("ASensor_getMinDelay", sensor_zero),
    WINAPK_SENSOR_AS("ASensor_getFifoMaxEventCount", sensor_zero),
    WINAPK_SENSOR_AS("ASensor_getFifoReservedEventCount", sensor_zero),
    WINAPK_SENSOR_AS("ASensor_getReportingMode", sensor_zero),
    WINAPK_SENSOR_AS("ASensor_isWakeUpSensor", sensor_zero),
    WINAPK_SENSOR_AS("ASensor_isDirectChannelTypeSupported", sensor_zero),
    WINAPK_SENSOR_AS("ASensor_getHighestDirectReportRateLevel", sensor_zero),
    WINAPK_SENSOR_AS("ASensor_getHandle", sensor_zero),
    WINAPK_SENSOR_AS("ASensor_getResolution", sensor_no_number),
};

}  // namespace

void* native_sensor_resolve(const char* name) {
    if (std::strncmp(name, "ASensor", 7) != 0) return nullptr;
    for (const Function& f : kFunctions) {
        if (std::strcmp(name, f.name) == 0) return f.fn;
    }
    return nullptr;
}

}  // namespace winapk::host
