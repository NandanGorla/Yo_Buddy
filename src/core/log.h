#pragma once

#include <filesystem>
#include <string_view>

// A small local log file. Nothing here ever leaves the machine (PROJECT.md: no telemetry).
// Callers must not log API keys, message text or screen content: log metadata only.
namespace buddy::log {

enum class Level { Info, Warn, Error };

// Opens (and truncates when large) the log file. Safe to skip: write() is then a no-op.
void init(const std::filesystem::path& file);

void write(Level level, std::string_view message);

inline void info(std::string_view message) { write(Level::Info, message); }
inline void warn(std::string_view message) { write(Level::Warn, message); }
inline void error(std::string_view message) { write(Level::Error, message); }

}  // namespace buddy::log
