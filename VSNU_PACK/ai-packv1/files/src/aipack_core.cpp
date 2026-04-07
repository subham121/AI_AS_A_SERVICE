// =============================================================================
// AI Packs System - Core Library Implementation
// Compiled utilities and system initialization
// =============================================================================

#include "aipack/aipack.hpp"
#include <iostream>

namespace aipack {

// System version info
static const Version SYSTEM_VERSION = {
    AIPACK_VERSION_MAJOR,
    AIPACK_VERSION_MINOR,
    AIPACK_VERSION_PATCH
};

const Version& getSystemVersion() {
    return SYSTEM_VERSION;
}

// Default console log sink
static void defaultLogSink(LogLevel level, const char* tag, const char* msg) {
    const char* color = "";
    const char* reset = "\033[0m";

    switch (level) {
        case LogLevel::Trace:   color = "\033[90m"; break;  // Gray
        case LogLevel::Debug:   color = "\033[36m"; break;  // Cyan
        case LogLevel::Info:    color = "\033[32m"; break;  // Green
        case LogLevel::Warning: color = "\033[33m"; break;  // Yellow
        case LogLevel::Error:   color = "\033[31m"; break;  // Red
        case LogLevel::Fatal:   color = "\033[35m"; break;  // Magenta
        default: break;
    }

    fprintf(stderr, "%s[%s] [%s] %s%s\n",
        color, logLevelName(level), tag, msg, reset);
}

void initializeSystem(LogLevel logLevel) {
    Logger::instance().setLevel(logLevel);
    Logger::instance().clearSinks();
    Logger::instance().addSink(defaultLogSink);

    AIPACK_INFO("System",
        "AI Packs System v%s initialized",
        SYSTEM_VERSION.toString().c_str());
}

} // namespace aipack
