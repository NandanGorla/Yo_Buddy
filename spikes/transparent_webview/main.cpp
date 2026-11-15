// SPIKE (M1.5, report only; not part of the app).
//
// Question: can a WebView2 surface have a transparent background and rounded corners, and can
// such a small visual be moved around the screen quickly enough to follow the mouse?
//
// Method:
//  1. Create an opaque magenta "backdrop" window, so there is a known colour behind the test.
//  2. Create a borderless, click-through, topmost window with no redirection bitmap, and host
//     WebView2 in it through DirectComposition ("visual hosting") with a fully transparent
//     default background. The page draws one rounded card on a transparent body.
//  3. Read pixels back from the screen inside that window: where the page is transparent the
//     backdrop's magenta must show through; where the card is, it must not.
//  4. Move the window in a circle, once per display refresh, for three seconds, and measure the
//     achieved rate and this process's CPU use.
//
// It never moves the real mouse and only reads pixels inside its own windows.
// Results are written to %TEMP%\buddy-spike-transparent.txt.

#include <cmath>
#include <cstdio>
#include <string>

#include <windows.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <wrl.h>
#include <wil/com.h>
#include <WebView2.h>

using Microsoft::WRL::Callback;

namespace {

constexpr int kSize = 320;         // the transparent window is kSize x kSize pixels
constexpr int kBackdrop = 640;
constexpr COLORREF kMagenta = RGB(255, 0, 255);

HWND g_backdrop = nullptr;
HWND g_window = nullptr;
FILE* g_report = nullptr;
wil::com_ptr<IDCompositionDevice> g_device;
wil::com_ptr<IDCompositionTarget> g_target;
wil::com_ptr<IDCompositionVisual> g_root;
wil::com_ptr<ICoreWebView2Environment> g_environment;
wil::com_ptr<ICoreWebView2CompositionController> g_composition;
wil::com_ptr<ICoreWebView2Controller> g_controller;
wil::com_ptr<ICoreWebView2> g_webview;

void report(const char* format, ...) {
    va_list args;
    va_start(args, format);
    if (g_report) {
        vfprintf(g_report, format, args);
        fputc('\n', g_report);
        fflush(g_report);
    }
    va_end(args);
}

const wchar_t* kPage = LR"(<!doctype html><html><head><style>
  html, body { margin: 0; height: 100%; background: transparent; overflow: hidden; }
  .card { position: absolute; inset: 20px; border-radius: 60px; background: #1c2a4a;
          display: flex; align-items: center; justify-content: center;
          color: white; font: 600 22px 'Segoe UI', sans-serif; }
  .dot { width: 22px; height: 22px; border-radius: 50%; background: #ffb000; margin-right: 10px;
         animation: pulse 1s infinite alternate; }
  @keyframes pulse { from { transform: scale(0.7); } to { transform: scale(1.2); } }
</style></head><body><div class="card"><span class="dot"></span>Buddy</div></body></html>)";

COLORREF screen_pixel(int x, int y) {
    HDC screen = GetDC(nullptr);
    const COLORREF c = GetPixel(screen, x, y);
    ReleaseDC(nullptr, screen);
    return c;
}

void describe_pixel(const char* what, int local_x, int local_y) {
    RECT r{};
    GetWindowRect(g_window, &r);
    const COLORREF c = screen_pixel(r.left + local_x, r.top + local_y);
    const bool magenta = GetRValue(c) > 240 && GetGValue(c) < 15 && GetBValue(c) > 240;
    report("  pixel %-34s at (%3d,%3d): rgb(%3d,%3d,%3d)  %s", what, local_x, local_y, GetRValue(c), GetGValue(c),
           GetBValue(c), magenta ? "= backdrop shows through (transparent)" : "= page content (opaque)");
}

double process_cpu_seconds() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
    const auto to_seconds = [](const FILETIME& f) {
        return ((static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7;
    };
    return to_seconds(kernel) + to_seconds(user);
}

void run_measurements() {
    report("1. Transparency and rounded corners (visual hosting, DefaultBackgroundColor alpha 0)");
    describe_pixel("outside the card (page margin)", 6, 6);
    describe_pixel("inside the card's corner cut-off", 28, 28);
    describe_pixel("just inside the rounded corner", 62, 62);
    describe_pixel("centre of the card", kSize / 2 + 40, kSize / 2 + 40);
    describe_pixel("bottom-right corner cut-off", kSize - 28, kSize - 28);

    report("");
    report("2. Following: moving the window once per display refresh for 3 seconds");
    LARGE_INTEGER frequency{}, start{}, now{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    const double cpu_before = process_cpu_seconds();
    RECT base{};
    GetWindowRect(g_backdrop, &base);
    const int cx = base.left + kBackdrop / 2 - kSize / 2;
    const int cy = base.top + kBackdrop / 2 - kSize / 2;
    int moves = 0;
    double worst_gap_ms = 0, last = 0;
    while (true) {
        QueryPerformanceCounter(&now);
        const double t = static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);
        if (t >= 3.0) break;
        if (moves > 0 && (t - last) * 1000.0 > worst_gap_ms) worst_gap_ms = (t - last) * 1000.0;
        last = t;
        const int x = cx + static_cast<int>(140 * std::cos(t * 4.0));
        const int y = cy + static_cast<int>(140 * std::sin(t * 4.0));
        SetWindowPos(g_window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        ++moves;
        DwmFlush();   // wait for the next composition pass (one display refresh)
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    const double cpu_used = process_cpu_seconds() - cpu_before;
    report("  moves: %d in 3.0 s = %.0f per second; longest gap between moves: %.1f ms", moves, moves / 3.0, worst_gap_ms);
    report("  CPU used by this process while moving: %.1f %% of one core", cpu_used / 3.0 * 100.0);
    describe_pixel("outside the card, after moving", 6, 6);
    describe_pixel("centre of the card, after moving", kSize / 2 + 40, kSize / 2 + 40);

    const LONG ex = GetWindowLongW(g_window, GWL_EXSTYLE);
    report("");
    report("3. Window styles in use: layered=%d click-through=%d no-activate=%d topmost=%d no-redirection-bitmap=%d",
           (ex & WS_EX_LAYERED) != 0, (ex & WS_EX_TRANSPARENT) != 0, (ex & WS_EX_NOACTIVATE) != 0,
           (ex & WS_EX_TOPMOST) != 0, (ex & WS_EX_NOREDIRECTIONBITMAP) != 0);
    report("done");
    PostQuitMessage(0);
}

void on_controller(HRESULT result, ICoreWebView2CompositionController* composition) {
    if (FAILED(result) || !composition) {
        report("FAILED: composition controller, HRESULT 0x%08lX", static_cast<unsigned long>(result));
        PostQuitMessage(1);
        return;
    }
    g_composition = composition;
    g_controller = g_composition.query<ICoreWebView2Controller>();
    g_controller->get_CoreWebView2(&g_webview);

    // The web view renders into this DirectComposition visual instead of into a child window.
    g_composition->put_RootVisualTarget(g_root.get());
    RECT bounds{0, 0, kSize, kSize};
    g_controller->put_Bounds(bounds);
    if (auto controller2 = g_controller.try_query<ICoreWebView2Controller2>()) {
        const HRESULT hr = controller2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{0, 0, 0, 0});
        report("put_DefaultBackgroundColor(alpha 0): HRESULT 0x%08lX", static_cast<unsigned long>(hr));
    }
    g_controller->put_IsVisible(TRUE);
    g_device->Commit();

    EventRegistrationToken token{};
    g_webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                // Give the first frames time to reach the screen before reading pixels.
                SetTimer(g_window, 1, 1500, nullptr);
                return S_OK;
            })
            .Get(),
        &token);
    g_webview->NavigateToString(kPage);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_TIMER && hwnd == g_window) {
        KillTimer(hwnd, 1);
        run_measurements();
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring report_path = std::wstring(temp) + L"buddy-spike-transparent.txt";
    _wfopen_s(&g_report, report_path.c_str(), L"w");

    // Work in real pixels. Without this Windows would scale the window and the coordinates used
    // for the pixel checks would not line up with what is drawn.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    WNDCLASSEXW backdrop_class{};
    backdrop_class.cbSize = sizeof(backdrop_class);
    backdrop_class.lpfnWndProc = DefWindowProcW;
    backdrop_class.hInstance = instance;
    backdrop_class.hbrBackground = CreateSolidBrush(kMagenta);
    backdrop_class.lpszClassName = L"BuddySpikeBackdrop";
    RegisterClassExW(&backdrop_class);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"BuddySpikeWindow";
    RegisterClassExW(&window_class);

    g_backdrop = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"BuddySpikeBackdrop",
                                 L"Buddy spike backdrop", WS_POPUP, 80, 80, kBackdrop, kBackdrop, nullptr, nullptr,
                                 instance, nullptr);
    ShowWindow(g_backdrop, SW_SHOWNOACTIVATE);

    // WS_EX_NOREDIRECTIONBITMAP: the window has no GDI surface of its own, only what
    //   DirectComposition draws, which is what makes per-pixel transparency possible.
    // WS_EX_LAYERED | WS_EX_TRANSPARENT: mouse clicks pass through to whatever is underneath.
    // WS_EX_NOACTIVATE: it never takes keyboard focus.
    g_window = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                                   WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                               L"BuddySpikeWindow", L"Buddy spike", WS_POPUP, 80 + 160, 80 + 160, kSize, kSize,
                               nullptr, nullptr, instance, nullptr);
    if (!g_window) {
        report("FAILED: CreateWindowEx, error %lu", GetLastError());
        return 1;
    }
    ShowWindow(g_window, SW_SHOWNOACTIVATE);

    HRESULT hr = DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&g_device));
    if (SUCCEEDED(hr)) hr = g_device->CreateTargetForHwnd(g_window, TRUE, &g_target);
    if (SUCCEEDED(hr)) hr = g_device->CreateVisual(&g_root);
    if (SUCCEEDED(hr)) hr = g_target->SetRoot(g_root.get());
    if (FAILED(hr)) {
        report("FAILED: DirectComposition setup, HRESULT 0x%08lX", static_cast<unsigned long>(hr));
        return 1;
    }

    wchar_t user_data[MAX_PATH] = {};
    swprintf_s(user_data, L"%sbuddy-spike-webview2", temp);
    hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, user_data, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(result) || !environment) {
                    report("FAILED: environment, HRESULT 0x%08lX", static_cast<unsigned long>(result));
                    PostQuitMessage(1);
                    return S_OK;
                }
                g_environment = environment;
                auto environment3 = g_environment.try_query<ICoreWebView2Environment3>();
                if (!environment3) {
                    report("FAILED: this WebView2 Runtime has no visual hosting (ICoreWebView2Environment3)");
                    PostQuitMessage(1);
                    return S_OK;
                }
                environment3->CreateCoreWebView2CompositionController(
                    g_window, Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                                  [](HRESULT created, ICoreWebView2CompositionController* composition) -> HRESULT {
                                      on_controller(created, composition);
                                      return S_OK;
                                  })
                                  .Get());
                return S_OK;
            })
            .Get());
    if (FAILED(hr)) {
        report("FAILED: CreateCoreWebView2EnvironmentWithOptions, HRESULT 0x%08lX", static_cast<unsigned long>(hr));
        return 1;
    }

    // Safety net: never leave test windows on screen if something stalls.
    SetTimer(g_backdrop, 2, 25000, [](HWND, UINT, UINT_PTR, DWORD) {
        report("TIMED OUT after 25 s");
        PostQuitMessage(2);
    });

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    g_webview.reset();
    if (g_controller) g_controller->Close();
    g_controller.reset();
    g_composition.reset();
    DestroyWindow(g_window);
    DestroyWindow(g_backdrop);
    if (g_report) fclose(g_report);
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
