/**
 * @file   log.h
 * @brief  极简日志
 * @author blt
 */

// ============================================================================
// core/common/log.h —— 极简日志
// ============================================================================
// 为什么不用第三方日志库：本项目依赖只用系统库；日志需求就是"带模块前缀、
// 分级别、线程安全地打一行"。级别用环境变量 ALLPLAY_LOG 控（debug/info/warn/error）。
#pragma once

#include <cstdarg>

namespace core {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

/// 从环境变量 ALLPLAY_LOG 初始化（进程入口调一次即可；不调默认 Info）
void logInitFromEnv();
void setLogLevel(Level lv);
Level logLevel();

/// 额外的日志出口（鸿蒙把 core 的日志转进 hilog / 界面）。传 nullptr 取消。
/// 只在调用方自己装配的地方用；桌面/Android 不设，行为不变。
using LogSink = void (*)(Level lv, const char* msg);
void setLogSink(LogSink sink);

/// 一般用下面的宏；file/line 由宏传入（打印时只取文件名）
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
