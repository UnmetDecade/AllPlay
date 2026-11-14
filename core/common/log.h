/**
 * @file   log.h
 * @brief  Minimal logging
 * @author blt
 */

// ============================================================================
// core/common/log.h — minimal logging
// ============================================================================
// Why no third-party logging library: this project only depends on system libraries; the logging needs are just "module prefix,
// level, one line, thread-safe". The level is controlled by the ALLPLAY_LOG env var (debug/info/warn/error).
#pragma once

#include <cstdarg>

namespace core {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

/// Initialize from the ALLPLAY_LOG env var (call once at process entry; defaults to Info otherwise)
void logInitFromEnv();
void setLogLevel(Level lv);
Level logLevel();

/// Extra log sink (HarmonyOS routes core logs into hilog / UI). Pass nullptr to remove.
/// Only used where the caller assembles its own; desktop/Android leave it unset, behavior unchanged.
using LogSink = void (*)(Level lv, const char* msg);
void setLogSink(LogSink sink);

/// Normally use the macros below; file/line come from the macro (only the file name is printed)
void logf(Level lv, const char* file, int line, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

}  // namespace core

#define LOG_DEBUG(...) ::core::logf(::core::Level::Debug, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  ::core::logf(::core::Level::Info,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  ::core::logf(::core::Level::Warn,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) ::core::logf(::core::Level::Error, __FILE__, __LINE__, __VA_ARGS__)
