#pragma once

#include <string>
#include <string_view>

#include <windows.h>

// UTF-8 <-> UTF-16 conversion. Buddy keeps text as UTF-8 internally and converts only at the
// Win32 boundary, where the "W" APIs want UTF-16.
namespace buddy::win {

inline std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
    return out;
}

inline std::string narrow(std::wstring_view utf16) {
    if (utf16.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), out.data(), size, nullptr,
                        nullptr);
    return out;
}

}  // namespace buddy::win
