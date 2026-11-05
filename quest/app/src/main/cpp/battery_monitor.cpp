#include "battery_monitor.h"

#include "log.h"

#include <chrono>

namespace qm {
namespace {

constexpr auto kPollInterval = std::chrono::seconds(10);

bool clearException(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

}  // namespace

BatteryMonitor::BatteryMonitor(ANativeActivity* activity, std::function<void(const HeadsetStatus&)> onStatus)
    : activity_(activity), onStatus_(std::move(onStatus)), thread_([this] { run(); }) {}

BatteryMonitor::~BatteryMonitor() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void BatteryMonitor::run() {
    JNIEnv* env = nullptr;
    if (activity_->vm->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr) {
        LOGW("Battery: cannot attach to the JVM; headset status not reported");
        return;
    }
    bool loggedFailure = false;
    HeadsetStatus last;
    bool haveLast = false;
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
        lock.unlock();
        HeadsetStatus status;
        if (read(env, &status)) {
            if (!haveLast || status != last) {
                LOGI("Battery: %u %%%s%s", status.batteryPercent, status.flags & HeadsetStatus::kCharging ? ", charging" : "",
                     status.flags & HeadsetStatus::kPluggedIn ? ", plugged in" : "");
            }
            last = status;
            haveLast = true;
            onStatus_(status);
        } else if (!loggedFailure) {
            loggedFailure = true;
            LOGW("Battery: reading ACTION_BATTERY_CHANGED failed; headset status not reported");
        }
        lock.lock();
        cv_.wait_for(lock, kPollInterval, [this] { return stop_; });
    }
    lock.unlock();
    activity_->vm->DetachCurrentThread();
}

bool BatteryMonitor::read(JNIEnv* env, HeadsetStatus* out) {
    if (env->PushLocalFrame(16) != JNI_OK) {
        clearException(env);
        return false;
    }
    bool ok = false;
    // registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED)) returns the sticky
    // battery intent without registering anything.
    jclass filterClass = env->FindClass("android/content/IntentFilter");
    jmethodID filterInit = filterClass ? env->GetMethodID(filterClass, "<init>", "(Ljava/lang/String;)V") : nullptr;
    jclass contextClass = env->GetObjectClass(activity_->clazz);
    jmethodID registerReceiver =
        env->GetMethodID(contextClass, "registerReceiver",
                         "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;");
    jclass intentClass = env->FindClass("android/content/Intent");
    jmethodID getIntExtra = intentClass ? env->GetMethodID(intentClass, "getIntExtra", "(Ljava/lang/String;I)I") : nullptr;
    if (!clearException(env) && filterInit != nullptr && registerReceiver != nullptr && getIntExtra != nullptr) {
        jobject filter = env->NewObject(filterClass, filterInit, env->NewStringUTF("android.intent.action.BATTERY_CHANGED"));
        jobject intent = filter ? env->CallObjectMethod(activity_->clazz, registerReceiver, nullptr, filter) : nullptr;
        if (!clearException(env) && intent != nullptr) {
            auto extra = [&](const char* name, int fallback) {
                const int v = env->CallIntMethod(intent, getIntExtra, env->NewStringUTF(name), fallback);
                return clearException(env) ? fallback : v;
            };
            *out = headsetStatusFromBattery(extra("level", -1), extra("scale", -1), extra("status", -1),
                                            extra("plugged", 0));
            ok = true;
        }
    }
    clearException(env);
    env->PopLocalFrame(nullptr);
    return ok;
}

}  // namespace qm
