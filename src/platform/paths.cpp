#include "platform/paths.h"

#include <string>
#include <system_error>

#include <windows.h>
#include <shlobj.h>

namespace buddy::paths {
namespace {
std::wstring g_folder_name = L"Buddy";
}

void use_profile(const std::wstring& name) { g_folder_name = L"Buddy-" + name; }

std::filesystem::path exe_dir() {
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return std::filesystem::current_path();
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(buffer.size() * 2);   // path was truncated: retry with a larger buffer
    }
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path data_dir() {
    std::filesystem::path base;
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw)) && raw) {
        base = raw;
    } else {
        base = std::filesystem::temp_directory_path();
    }
    CoTaskMemFree(raw);

    const std::filesystem::path dir = base / g_folder_name;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

}  // namespace buddy::paths
