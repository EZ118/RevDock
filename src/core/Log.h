#pragma once

#include <filesystem>
#include <string>

namespace revdock {

enum class LogLevel { Debug, Info, Warn, Error };

void logInit(const std::filesystem::path &logFile, bool verbose);
void logShutdown();
void logWrite(LogLevel level, const char *fmt, ...);
void logFlush();

#define RD_LOG_DEBUG(...) ::revdock::logWrite(::revdock::LogLevel::Debug, __VA_ARGS__)
#define RD_LOG_INFO(...) ::revdock::logWrite(::revdock::LogLevel::Info, __VA_ARGS__)
#define RD_LOG_WARN(...) ::revdock::logWrite(::revdock::LogLevel::Warn, __VA_ARGS__)
#define RD_LOG_ERROR(...) ::revdock::logWrite(::revdock::LogLevel::Error, __VA_ARGS__)

} // namespace revdock
