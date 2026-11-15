#include "core/log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <system_error>

namespace buddy::log {
namespace {

constexpr std::uintmax_t kMaxLogBytes = 1024 * 1024;

std::mutex g_mutex;
std::ofstream g_file;

const char* level_name(Level level) {
    switch (level) {
        case Level::Info: return "INFO ";
        case Level::Warn: return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "?    ";
}

}  // namespace

void init(const std::filesystem::path& file) {
    std::lock_guard lock(g_mutex);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const auto size = std::filesystem::file_size(file, ec);
    const bool truncate = !ec && size > kMaxLogBytes;
    g_file.open(file, truncate ? std::ios::out | std::ios::trunc : std::ios::out | std::ios::app);
}

void write(Level level, std::string_view message) {
    std::lock_guard lock(g_mutex);
    if (!g_file.is_open()) return;

    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    localtime_s(&local, &now);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);

    g_file << stamp << ' ' << level_name(level) << ' ';
    // Keep each entry on one line so a hostile string cannot forge extra log entries.
    for (const char c : message) {
        g_file << ((c == '\n' || c == '\r') ? ' ' : c);
    }
    g_file << '\n';
    g_file.flush();
}

}  // namespace buddy::log
