#include "common/log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace core {

namespace {
std::atomic<int> g_level{static_cast<int>(Level::Info)};
std::mutex       g_mu;
std::atomic<LogSink> g_sink{nullptr};

const char* levelName(Level lv)
{
    switch (lv) {
        case Level::Debug: return "DBG";
        case Level::Info:  return "INF";
        case Level::Warn:  return "WRN";
        case Level::Error: return "ERR";
    }
    return "???";
}

const char* baseName(const char* path)
{
    const char* p = std::strrchr(path, '/');
    return p ? p + 1 : path;
}
}  // namespace

void logInitFromEnv()
{
    const char* e = std::getenv("ALLPLAY_LOG");
    if (!e || !*e) return;
    if (!std::strcmp(e, "debug")) setLogLevel(Level::Debug);
    else if (!std::strcmp(e, "info")) setLogLevel(Level::Info);
    else if (!std::strcmp(e, "warn")) setLogLevel(Level::Warn);
    else if (!std::strcmp(e, "error")) setLogLevel(Level::Error);
}

void setLogLevel(Level lv) { g_level.store(static_cast<int>(lv)); }

void setLogSink(LogSink sink) { g_sink.store(sink, std::memory_order_relaxed); }
Level logLevel() { return static_cast<Level>(g_level.load()); }

void logf(Level lv, const char* file, int line, const char* fmt, ...)
{
    if (static_cast<int>(lv) < g_level.load(std::memory_order_relaxed)) return;

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    // 墙钟时间（秒精度够用；日志不打点、别引入更多依赖）
    char ts[24] = "";
    {
        std::time_t t = std::time(nullptr);
        std::tm     tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::strftime(ts, sizeof ts, "%H:%M:%S", &tm);
    }

    {
        std::lock_guard<std::mutex> lk(g_mu);
#if !defined(__ANDROID__)
        // 桌面：打印到 stderr。Android（API<23）不导出 stderr 符号，且日志走 logcat，这里跳过。
        std::fprintf(stderr, "[%s] [%s] %s:%d  %s\n", ts, levelName(lv), baseName(file), line, msg);
        std::fflush(stderr);
#endif
#ifdef __ANDROID__
        int prio = ANDROID_LOG_INFO;
        switch (lv) {
            case Level::Debug: prio = ANDROID_LOG_DEBUG; break;
            case Level::Info:  prio = ANDROID_LOG_INFO;  break;
            case Level::Warn:  prio = ANDROID_LOG_WARN;  break;
            case Level::Error: prio = ANDROID_LOG_ERROR; break;
        }
        __android_log_print(prio, "AllPlay", "%s:%d  %s", baseName(file), line, msg);
#endif
    }

    // 额外出口（鸿蒙：hilog / 界面）。放在锁外 —— sink 里再打日志也不会自锁。
    if (LogSink sink = g_sink.load(std::memory_order_relaxed)) sink(lv, msg);
}

}  // namespace core
