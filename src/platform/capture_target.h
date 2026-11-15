#pragma once

#include <optional>
#include <string>

#include <windows.h>

#include "core/capture_policy.h"

namespace buddy::capture {

struct WindowIdentityResult {
    std::optional<WindowIdentity> identity;
    std::string error;

    bool ok() const { return identity.has_value(); }
};

struct MonitorTarget {
    HMONITOR monitor = nullptr;
    RECT bounds{};
};

struct TargetResult {
    std::optional<MonitorTarget> monitor;
    HWND window = nullptr;
    std::string error;

    bool ok() const { return monitor.has_value() || window != nullptr; }
};

// Reads only the title and executable name for an existing top-level window.
// It does not capture pixels or change the target window.
WindowIdentityResult identify_window(HWND hwnd);

// Resolves the target at request time. A monitor target follows the foreground window's monitor;
// a window target is the foreground top-level window. The caller must reject Buddy-owned HWNDs.
TargetResult resolve_foreground_target(bool monitor_target);

bool is_buddy_window(HWND hwnd);

}  // namespace buddy::capture
