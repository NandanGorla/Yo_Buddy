#pragma once

#include <filesystem>
#include <string>

namespace buddy::paths {

// The folder that holds buddy.exe (and the ui/ and themes/ folders shipped with it).
std::filesystem::path exe_dir();

// %LOCALAPPDATA%\Buddy, created on first use. Holds settings, the log and the WebView2 profile.
std::filesystem::path data_dir();

// Switches data_dir() to %LOCALAPPDATA%\Buddy-<name>. Used by debug builds to keep test runs
// away from the user's real settings. Call it before the first data_dir().
void use_profile(const std::wstring& name);

}  // namespace buddy::paths
