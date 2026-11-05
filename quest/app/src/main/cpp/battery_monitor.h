#pragma once

#include "headset_status.h"

#include <android/native_activity.h>
#include <jni.h>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace qm {

// Reads the headset's battery every 10 s on its own thread (attached to the JVM) from the sticky
// ACTION_BATTERY_CHANGED intent (Context.registerReceiver(null, filter): no receiver, no Java
// code) and hands every reading to `onStatus` (on that thread). Stops in its destructor.
class BatteryMonitor {
public:
    BatteryMonitor(ANativeActivity* activity, std::function<void(const HeadsetStatus&)> onStatus);
    ~BatteryMonitor();
    BatteryMonitor(const BatteryMonitor&) = delete;
    BatteryMonitor& operator=(const BatteryMonitor&) = delete;

private:
    void run();
    // One reading; false (logged once) if the JNI calls failed.
    bool read(JNIEnv* env, HeadsetStatus* out);

    ANativeActivity* activity_;
    std::function<void(const HeadsetStatus&)> onStatus_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace qm
