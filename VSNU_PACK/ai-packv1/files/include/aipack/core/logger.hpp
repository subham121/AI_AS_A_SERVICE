// =============================================================================
// AI Packs System - Logging Framework
// Lightweight logging for embedded devices
// =============================================================================
#pragma once

#include <string>
#include <cstdio>
#include <cstdarg>
#include <chrono>
#include <mutex>
#include <functional>
#include <vector>

namespace aipack {

enum class LogLevel : uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warning = 3,
    Error = 4,
    Fatal = 5,
    Off = 6,
};

inline const char* logLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        default: return "?";
    }
}

using LogSink = std::function<void(LogLevel, const char* tag, const char* msg)>;

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void setLevel(LogLevel level) { level_ = level; }
    LogLevel getLevel() const { return level_; }

    void addSink(LogSink sink) {
        std::lock_guard<std::mutex> lock(mutex_);
        sinks_.push_back(std::move(sink));
    }

    void clearSinks() {
        std::lock_guard<std::mutex> lock(mutex_);
        sinks_.clear();
    }

    void log(LogLevel level, const char* tag, const char* fmt, ...) {
        if (level < level_) return;

        char buffer[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        std::lock_guard<std::mutex> lock(mutex_);
        if (sinks_.empty()) {
            // Default: stderr
            fprintf(stderr, "[%s] [%s] %s\n", logLevelName(level), tag, buffer);
        } else {
            for (auto& sink : sinks_) {
                sink(level, tag, buffer);
            }
        }
    }

private:
    Logger() = default;
    LogLevel level_ = LogLevel::Info;
    std::mutex mutex_;
    std::vector<LogSink> sinks_;
};

#define AIPACK_LOG(level, tag, ...) \
    ::aipack::Logger::instance().log(level, tag, __VA_ARGS__)

#define AIPACK_TRACE(tag, ...) AIPACK_LOG(::aipack::LogLevel::Trace, tag, __VA_ARGS__)
#define AIPACK_DEBUG(tag, ...) AIPACK_LOG(::aipack::LogLevel::Debug, tag, __VA_ARGS__)
#define AIPACK_INFO(tag, ...)  AIPACK_LOG(::aipack::LogLevel::Info, tag, __VA_ARGS__)
#define AIPACK_WARN(tag, ...)  AIPACK_LOG(::aipack::LogLevel::Warning, tag, __VA_ARGS__)
#define AIPACK_ERROR(tag, ...) AIPACK_LOG(::aipack::LogLevel::Error, tag, __VA_ARGS__)
#define AIPACK_FATAL(tag, ...) AIPACK_LOG(::aipack::LogLevel::Fatal, tag, __VA_ARGS__)

} // namespace aipack
