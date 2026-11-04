#pragma once

#include <android/log.h>

#define VRS_LOG_TAG "VRScreen"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, VRS_LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, VRS_LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, VRS_LOG_TAG, __VA_ARGS__)
