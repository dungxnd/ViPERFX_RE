// Shim: redirect to NDK <android/log.h>
#pragma once
#include <android/log.h>
#ifndef ALOG
#  define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "fmq", __VA_ARGS__)
#  define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, "fmq", __VA_ARGS__)
#  define ALOG(prio, tag, ...) __android_log_print(prio, tag, __VA_ARGS__)
#endif
