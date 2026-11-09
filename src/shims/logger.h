#pragma once
// DragonkinTheBanished Bridge shim for Trinity's core/logger.h: every Trinity log line goes to bridge.log
// (with a [trinity] tag) instead of a console window / Trinity.log.
#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <string>
#include <mutex>
#include <unordered_set>

namespace wb { void log(const char* fmt, ...); }

namespace trinity
{
    class Logger
    {
    public:
        enum Level { Debug, Info, Good, Warn, Error };
        static void InitFileLogging(HMODULE) {}
        static void EnableConsole(bool, bool) {}
        static void DisableConsole() {}
        static void Shutdown() {}
        static void Log(Level lvl, const char* fmt, ...)
        {
            if (lvl == Debug) return;
            char msg[2048]; va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
            Emit(lvl, msg);
        }
        static void LogThrottled(uint32_t intervalMs, Level lvl, const char* fmt, ...)
        {
            static std::mutex mtx; static ULONGLONG last = 0;
            { std::lock_guard<std::mutex> lk(mtx); ULONGLONG now = GetTickCount64(); if (now - last < intervalMs) return; last = now; }
            char msg[2048]; va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
            Emit(lvl, msg);
        }
        static void LogOnce(Level lvl, const char* fmt, ...)
        {
            char msg[2048]; va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
            static std::mutex mtx; static std::unordered_set<std::string> seen;
            { std::lock_guard<std::mutex> lk(mtx); if (!seen.insert(msg).second) return; }
            Emit(lvl, msg);
        }
    private:
        static void Emit(Level lvl, const char* msg)
        {
            const char* tag = lvl == Warn ? "WARN " : lvl == Error ? "ERR  " : lvl == Good ? "OK   " : "";
            wb::log("[trinity] %s%s", tag, msg);
        }
    };
}

#define LOG(...)           ::trinity::Logger::Log(::trinity::Logger::Info,  __VA_ARGS__)
#define LOG_INFO(...)      ::trinity::Logger::Log(::trinity::Logger::Info,  __VA_ARGS__)
#define LOG_DEBUG(...)     ((void)0)
#define LOG_OK(...)        ::trinity::Logger::Log(::trinity::Logger::Good,  __VA_ARGS__)
#define LOG_WARN(...)      ::trinity::Logger::Log(::trinity::Logger::Warn,  __VA_ARGS__)
#define LOG_ERR(...)       ::trinity::Logger::Log(::trinity::Logger::Error, __VA_ARGS__)
#define LOG_ONCE(...)      ::trinity::Logger::LogOnce(::trinity::Logger::Info, __VA_ARGS__)
#define LOG_WARN_ONCE(...) ::trinity::Logger::LogOnce(::trinity::Logger::Warn, __VA_ARGS__)
#define LOG_THROTTLE(intervalMs, ...) ::trinity::Logger::LogThrottled(intervalMs, ::trinity::Logger::Info, __VA_ARGS__)
