#pragma once

#include <cstdarg>

namespace logging {

enum class Level { Trace, Debug, Info, Warning, Error, Fatal };

void write(Level level, const char* channel, const char* fmt, ...);
void set_level(Level level);
bool enabled(Level level);
double uptime();
unsigned long long uptime_ns();

}

#define LOG_TRACE(ch, ...) do { if (logging::enabled(logging::Level::Trace)) logging::write(logging::Level::Trace, ch, __VA_ARGS__); } while (0)
#define LOG_DEBUG(ch, ...) do { if (logging::enabled(logging::Level::Debug)) logging::write(logging::Level::Debug, ch, __VA_ARGS__); } while (0)
#define LOG_INFO(ch, ...) logging::write(logging::Level::Info, ch, __VA_ARGS__)
#define LOG_WARN(ch, ...) logging::write(logging::Level::Warning, ch, __VA_ARGS__)
#define LOG_ERROR(ch, ...) logging::write(logging::Level::Error, ch, __VA_ARGS__)
#define LOG_FATAL(ch, ...) logging::write(logging::Level::Fatal, ch, __VA_ARGS__)
