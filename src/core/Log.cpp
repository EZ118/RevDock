#include "core/Log.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace revdock {
namespace {

std::mutex g_mutex;
std::ofstream g_file;
bool g_verbose = false;
bool g_initialized = false;

const char *levelName(LogLevel level) {
    switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO ";
    case LogLevel::Warn: return "WARN ";
    case LogLevel::Error: return "ERROR";
    }
    return "INFO ";
}

} // namespace

void logInit(const std::filesystem::path &logFile, bool verbose) {
    std::lock_guard lock(g_mutex);
    g_verbose = verbose;
    g_initialized = true;
    std::error_code ec;
    std::filesystem::create_directories(logFile.parent_path(), ec);
    if (std::filesystem::exists(logFile) &&
        std::filesystem::file_size(logFile, ec) > 4 * 1024 * 1024) {
        std::filesystem::rename(logFile, logFile.string() + ".1", ec);
    }
    g_file.open(logFile, std::ios::out | std::ios::app);
}

void logShutdown() {
    std::lock_guard lock(g_mutex);
    if (g_file.is_open()) {
        g_file.flush();
        g_file.close();
    }
    g_initialized = false;
}

void logFlush() {
    std::lock_guard lock(g_mutex);
    if (g_file.is_open()) {
        g_file.flush();
    }
}

void logWrite(LogLevel level, const char *fmt, ...) {
    if (!g_initialized) {
        return;
    }
    if (level == LogLevel::Debug && !g_verbose) {
        return;
    }

    char message[4096];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    std::lock_guard lock(g_mutex);
    if (g_file.is_open()) {
        g_file << levelName(level) << " " << message << '\n';
        g_file.flush();
#ifdef _WIN32
        if (g_verbose) {
            OutputDebugStringA(message);
            OutputDebugStringA("\n");
        }
#endif
    }
    if (g_verbose) {
        std::fprintf(stderr, "[%s] %s\n", levelName(level), message);
    }
}

} // namespace revdock
