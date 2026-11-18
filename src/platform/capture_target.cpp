#include "platform/capture_target.h"

#include <array>

#include "platform/win_text.h"

namespace buddy::capture {
namespace {

constexpr int kMaxTitleChars = 512;
constexpr int kMaxProcessPathChars = 32768;
constexpr wchar_t kMainClass[] = L"BuddyMainWindow";
constexpr wchar_t kPanelClass[] = L"BuddyPanelWindow";
constexpr wchar_t kOverlayClass[] = L"BuddyOverlayWindow";

}  // namespace

WindowIdentityResult identify_window(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return {{}, "The capture target window is no longer available."};

    std::wstring title(static_cast<std::size_t>(kMaxTitleChars), L'\0');
    const int title_length = GetWindowTextW(hwnd, title.data(), static_cast<int>(title.size()));
    if (title_length < 0) return {{}, "Could not read the capture target title."};
    title.resize(static_cast<std::size_t>(title_length));

    DWORD process_id = 0;
    if (GetWindowThreadProcessId(hwnd, &process_id) == 0 || process_id == 0) {
        return {{}, "Could not identify the capture target process."};
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) return {{}, "Could not inspect the capture target process."};

    std::wstring path(static_cast<std::size_t>(kMaxProcessPathChars), L'\0');
    DWORD path_length = static_cast<DWORD>(path.size());
    const BOOL read = QueryFullProcessImageNameW(process, 0, path.data(), &path_length);
    CloseHandle(process);
    if (!read || path_length == 0) return {{}, "Could not read the capture target process name."};
    path.resize(path_length);

    const std::size_t slash = path.find_last_of(L"\\/");
    const std::wstring process_name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (process_name.empty()) return {{}, "The capture target process name is empty."};
    return {WindowIdentity{win::narrow(process_name), win::narrow(title)}, {}};
}

TargetResult resolve_foreground_target(bool monitor_target) {
    return resolve_target(GetForegroundWindow(), monitor_target);
}

TargetResult resolve_target(HWND foreground, bool monitor_target) {
    if (!foreground || !IsWindow(foreground)) return {{}, nullptr, "There is no foreground window to capture."};
    if (is_buddy_window(foreground)) return {{}, nullptr, "Buddy windows cannot be captured."};

    HWND top_level = GetAncestor(foreground, GA_ROOT);
    if (!top_level) top_level = foreground;
    if (is_buddy_window(top_level)) return {{}, nullptr, "Buddy windows cannot be captured."};

    if (!monitor_target) {
        return {{}, top_level, {}};
    }

    const HMONITOR monitor = MonitorFromWindow(top_level, MONITOR_DEFAULTTONEAREST);
    if (!monitor) return {{}, nullptr, "Could not find the monitor containing the foreground window."};
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return {{}, nullptr, "Could not read the target monitor bounds."};
    return {MonitorTarget{monitor, info.rcMonitor}, nullptr, {}};
}

bool is_buddy_window(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t class_name[128] = {};
    const int length = GetClassNameW(hwnd, class_name, static_cast<int>(std::size(class_name)));
    if (length <= 0) return false;
    return std::wstring_view(class_name, static_cast<std::size_t>(length)) == kMainClass ||
           std::wstring_view(class_name, static_cast<std::size_t>(length)) == kPanelClass ||
           std::wstring_view(class_name, static_cast<std::size_t>(length)) == kOverlayClass;
}

}  // namespace buddy::capture
