#pragma once

#include <android/log.h>

#define QM_LOG_TAG "QuestMirror"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, QM_LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, QM_LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, QM_LOG_TAG, __VA_ARGS__)
