#pragma once

// Small Win32 helpers shared by Buddy's windows.

#include <deque>
#include <functional>
#include <mutex>
#include <string>

#include <windows.h>

namespace buddy::win {

// Messages private to Buddy's windows.
inline constexpr UINT kMsgRunTasks = WM_APP + 1;        // drain the UI-thread task queue
inline constexpr UINT kMsgTray = WM_APP + 2;            // notification-area icon events
inline constexpr UINT kMsgActivateInstance = WM_APP + 3;   // a second launch asks the first to show itself

// "#rrggbb" (or the #rgb / #rrggbbaa forms the theme validator accepts) to a GDI colour.
COLORREF color_from_hex(const std::string& hex, COLORREF fallback);

// DIPs (96 per inch) to pixels for this window's monitor.
int scale_for(HWND hwnd, int dip);

// Lets worker threads run code on the UI thread: tasks go into a queue and a posted window
// message wakes the UI thread to drain it.
class UiDispatcher {
public:
    void attach(HWND hwnd) { hwnd_ = hwnd; }
    void post(std::function<void()> task);
    void drain();

private:
    HWND hwnd_ = nullptr;
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
};

// Makes `hwnd` the foreground window, the one that receives keyboard input.
//
// Windows only lets a process take the foreground in certain situations (for example right
// after it received a hotkey or a click). This first asks politely with SetForegroundWindow.
// If Windows refuses, it joins this thread's input state to the current foreground thread
// (AttachThreadInput), which makes Windows treat the request as coming from the foreground
// application, and asks again.
//
// Returns true when `hwnd` ended up in the foreground. `method`, when given, says how:
// "already", "direct", "attach" or "refused".
bool bring_to_foreground(HWND hwnd, const char** method = nullptr);

// Buddy's icon, drawn at run time (there is no icon artwork yet): a rounded square with a "B".
HICON create_app_icon(int size_px);

}  // namespace buddy::win
