// AVM 로깅.
// - Android 빌드: logcat(__android_log_print)으로 출력.
// - 호스트 빌드: stderr로 출력.
// 성능 원칙: 로그 레벨이 꺼져 있으면 포맷 비용조차 없어야 하므로,
// 핫패스(인터프리터/JIT 디스패치)에서는 반드시 AVM_LOG_* 매크로를 사용한다.
#pragma once

#include <cstdarg>
#include <cstdio>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace avm {

enum class LogLevel : int {
    kError = 0,
    kWarn  = 1,
    kInfo  = 2,
    kDebug = 3,
    kTrace = 4,
};

// 전역 로그 레벨. 릴리스 실행에서는 kWarn 이하로 낮춰 핫패스 로그를 완전히 제거한다.
extern LogLevel g_log_level;

inline bool LogEnabled(LogLevel level) {
    return static_cast<int>(level) <= static_cast<int>(g_log_level);
}

inline void LogRaw(LogLevel level, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
#if defined(__ANDROID__)
    int prio = ANDROID_LOG_INFO;
    switch (level) {
        case LogLevel::kError: prio = ANDROID_LOG_ERROR; break;
        case LogLevel::kWarn:  prio = ANDROID_LOG_WARN;  break;
        case LogLevel::kInfo:  prio = ANDROID_LOG_INFO;  break;
        case LogLevel::kDebug: prio = ANDROID_LOG_DEBUG; break;
        case LogLevel::kTrace: prio = ANDROID_LOG_VERBOSE; break;
    }
    __android_log_vprint(prio, tag, fmt, ap);
#else
    static const char* kNames[] = {"E", "W", "I", "D", "T"};
    std::fprintf(stderr, "[%s/%s] ", kNames[static_cast<int>(level)], tag);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
#endif
    va_end(ap);
}

} // namespace avm

#define AVM_LOG(level, tag, ...)                                  \
    do {                                                          \
        if (::avm::LogEnabled(level)) {                           \
            ::avm::LogRaw(level, tag, __VA_ARGS__);               \
        }                                                         \
    } while (0)

#define AVM_LOGE(tag, ...) AVM_LOG(::avm::LogLevel::kError, tag, __VA_ARGS__)
#define AVM_LOGW(tag, ...) AVM_LOG(::avm::LogLevel::kWarn,  tag, __VA_ARGS__)
#define AVM_LOGI(tag, ...) AVM_LOG(::avm::LogLevel::kInfo,  tag, __VA_ARGS__)
#define AVM_LOGD(tag, ...) AVM_LOG(::avm::LogLevel::kDebug, tag, __VA_ARGS__)
#define AVM_LOGT(tag, ...) AVM_LOG(::avm::LogLevel::kTrace, tag, __VA_ARGS__)
