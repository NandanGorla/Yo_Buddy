#include "webhost/webview_host.h"

#include <cstdio>
#include <cwchar>

#include <wrl.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <WebView2.h>

#include "platform/win_text.h"

using Microsoft::WRL::Callback;

namespace buddy {
namespace {

bool starts_with_ci(const wchar_t* text, const wchar_t* prefix) {
    return text && _wcsnicmp(text, prefix, std::wcslen(prefix)) == 0;
}

// True for URLs inside the bundled UI: exactly the origin, or the origin followed by a path.
bool is_app_url(const wchar_t* uri) {
    if (!starts_with_ci(uri, WebViewHost::kOrigin)) return false;
    const wchar_t next = uri[std::wcslen(WebViewHost::kOrigin)];
    return next == L'\0' || next == L'/';
}

// Schemes that reach the network or the disk. Anything using them outside the app origin is
// blocked. data:, blob: and about: never leave the process and are governed by the page's CSP.
bool is_external_fetch(const wchar_t* uri) {
    return starts_with_ci(uri, L"http:") || starts_with_ci(uri, L"https:") || starts_with_ci(uri, L"ws:") ||
           starts_with_ci(uri, L"wss:") || starts_with_ci(uri, L"ftp:") || starts_with_ci(uri, L"file:");
}

std::string hr_text(const char* what, HRESULT hr) {
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "%s failed (HRESULT 0x%08lX)", what, static_cast<unsigned long>(hr));
    return buffer;
}

}  // namespace

// The state lives in a shared_ptr so that asynchronous WebView2 callbacks which outlive the
// WebViewHost object find a valid (but closed) Impl instead of a dangling pointer.
struct WebViewHost::Impl : std::enable_shared_from_this<WebViewHost::Impl> {
    WebViewConfig config;
    MessageHandler on_message;
    ReadyHandler on_ready;
    LogHandler log;

    wil::com_ptr<ICoreWebView2Environment> environment;
    wil::com_ptr<ICoreWebView2Controller> controller;
    wil::com_ptr<ICoreWebView2> webview;
    bool closed = false;
    bool reported = false;
    RECT pending_bounds{};

    void info(std::string message) {
        if (log) log(false, std::move(message));
    }
    void error(std::string message) {
        if (log) log(true, std::move(message));
    }

    void report(bool ok, std::string message) {
        if (reported) return;
        reported = true;
        if (!ok) error(message);
        if (on_ready) on_ready(ok, std::move(message));
    }

    void start() {
        const std::wstring user_data = config.user_data_dir.wstring();
        std::weak_ptr<Impl> weak = weak_from_this();
        const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, user_data.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [weak](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                    if (auto self = weak.lock(); self && !self->closed) self->on_environment(result, env);
                    return S_OK;
                })
                .Get());
        if (FAILED(hr)) {
            if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
                report(false, "The Microsoft Edge WebView2 Runtime is not installed.");
            } else {
                report(false, hr_text("Starting WebView2", hr));
            }
        }
    }

    void on_environment(HRESULT result, ICoreWebView2Environment* env) {
        if (FAILED(result) || !env) {
            report(false, hr_text("Creating the WebView2 environment", result));
            return;
        }
        environment = env;
        std::weak_ptr<Impl> weak = weak_from_this();
        const HRESULT hr = environment->CreateCoreWebView2Controller(
            config.parent,
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [weak](HRESULT created, ICoreWebView2Controller* ctl) -> HRESULT {
                    if (auto self = weak.lock(); self && !self->closed) self->on_controller(created, ctl);
                    return S_OK;
                })
                .Get());
        if (FAILED(hr)) report(false, hr_text("Creating the WebView2 controller", hr));
    }

    void on_controller(HRESULT result, ICoreWebView2Controller* ctl) {
        if (FAILED(result) || !ctl) {
            report(false, hr_text("Creating the WebView2 controller", result));
            return;
        }
        controller = ctl;
        if (FAILED(controller->get_CoreWebView2(&webview)) || !webview) {
            report(false, "WebView2 did not provide a web view.");
            return;
        }

        apply_settings();
        if (!map_ui_folder()) return;
        register_lockdown();
        register_messaging();
        register_input();

        if (auto controller2 = controller.try_query<ICoreWebView2Controller2>()) {
            COREWEBVIEW2_COLOR color{255, GetRValue(config.background), GetGValue(config.background),
                                     GetBValue(config.background)};
            controller2->put_DefaultBackgroundColor(color);
        }
        controller->put_Bounds(pending_bounds);
        controller->put_IsVisible(config.start_visible ? TRUE : FALSE);

        const std::wstring start_url = std::wstring(kOrigin) + config.start_path;
        const HRESULT hr = webview->Navigate(start_url.c_str());
        if (FAILED(hr)) {
            report(false, hr_text("Loading the UI", hr));
            return;
        }
        report(true, {});
    }

    void apply_settings() {
        wil::com_ptr<ICoreWebView2Settings> settings;
        if (FAILED(webview->get_Settings(&settings)) || !settings) return;
        const BOOL dev = config.developer_mode ? TRUE : FALSE;

        settings->put_IsScriptEnabled(TRUE);
        settings->put_IsWebMessageEnabled(TRUE);
        settings->put_AreHostObjectsAllowed(FALSE);          // the bridge is messages only
        settings->put_AreDefaultScriptDialogsEnabled(FALSE);  // no alert()/confirm()/prompt() windows
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_IsBuiltInErrorPageEnabled(FALSE);
        settings->put_AreDevToolsEnabled(dev);
        settings->put_AreDefaultContextMenusEnabled(dev);

        if (auto s3 = settings.try_query<ICoreWebView2Settings3>()) {
            // Browser shortcuts (F5, Ctrl+P, Ctrl+F, F12, ...). Editing keys such as Ctrl+C keep working.
            s3->put_AreBrowserAcceleratorKeysEnabled(dev);
        }
        if (auto s4 = settings.try_query<ICoreWebView2Settings4>()) {
            // The API key field must never be offered to Edge's password or autofill storage.
            s4->put_IsPasswordAutosaveEnabled(FALSE);
            s4->put_IsGeneralAutofillEnabled(FALSE);
        }
        if (auto s5 = settings.try_query<ICoreWebView2Settings5>()) s5->put_IsPinchZoomEnabled(FALSE);
        if (auto s6 = settings.try_query<ICoreWebView2Settings6>()) s6->put_IsSwipeNavigationEnabled(FALSE);
        if (config.drag_regions) {
            // Lets the page mark an area as a window caption with CSS "app-region: drag", so a
            // frameless window can be moved by its header.
            if (auto s9 = settings.try_query<ICoreWebView2Settings9>()) s9->put_IsNonClientRegionSupportEnabled(TRUE);
        }
    }

    bool map_ui_folder() {
        auto webview3 = webview.try_query<ICoreWebView2_3>();
        if (!webview3) {
            report(false, "The installed WebView2 Runtime is too old (virtual host mapping is missing).");
            return false;
        }
        // DENY: other origins cannot read these files. The UI itself is same-origin, so it can.
        const HRESULT hr = webview3->SetVirtualHostNameToFolderMapping(
            kHostName, config.ui_dir.wstring().c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY);
        if (FAILED(hr)) {
            report(false, hr_text("Mapping the UI folder", hr));
            return false;
        }
        return true;
    }

    void register_lockdown() {
        std::weak_ptr<Impl> weak = weak_from_this();
        EventRegistrationToken token{};

        // 1. Sub-resource and fetch requests: anything outside the bundled origin gets a 403.
        //    Requests for the bundled origin are left alone, so virtual host mapping serves them.
        if (auto webview22 = webview.try_query<ICoreWebView2_22>()) {
            // Covers documents, iframes and workers alike.
            webview22->AddWebResourceRequestedFilterWithRequestSourceKinds(
                L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL, COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_ALL);
        } else {
            webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        }
        webview->add_WebResourceRequested(
            Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [weak](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                    auto self = weak.lock();
                    if (!self || !args) return S_OK;
                    wil::com_ptr<ICoreWebView2WebResourceRequest> request;
                    wil::unique_cotaskmem_string uri;
                    if (FAILED(args->get_Request(&request)) || !request || FAILED(request->get_Uri(&uri))) {
                        return S_OK;
                    }
                    if (is_app_url(uri.get()) || !is_external_fetch(uri.get())) return S_OK;

                    wil::com_ptr<ICoreWebView2WebResourceResponse> response;
                    if (SUCCEEDED(self->environment->CreateWebResourceResponse(nullptr, 403, L"Blocked", L"",
                                                                              &response))) {
                        args->put_Response(response.get());
                    }
                    self->info("blocked a request to an external URL");
                    return S_OK;
                })
                .Get(),
            &token);

        // 2. Navigation: the top-level document and every frame stay inside the bundled origin.
        const auto navigation_guard = Callback<ICoreWebView2NavigationStartingEventHandler>(
            [weak](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                auto self = weak.lock();
                if (!args) return S_OK;
                wil::unique_cotaskmem_string uri;
                if (FAILED(args->get_Uri(&uri)) || !is_app_url(uri.get())) {
                    args->put_Cancel(TRUE);
                    if (self) self->info("cancelled a navigation away from the UI");
                }
                return S_OK;
            });
        webview->add_NavigationStarting(navigation_guard.Get(), &token);
        // Frames may also hold inline documents (srcdoc / about:blank): the visuals sandbox (M6)
        // is built that way. Those never touch the network; anything they fetch is still
        // filtered by rule 1 above.
        webview->add_FrameNavigationStarting(
            Callback<ICoreWebView2NavigationStartingEventHandler>(
                [weak](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                    if (!args) return S_OK;
                    wil::unique_cotaskmem_string uri;
                    const bool ok = SUCCEEDED(args->get_Uri(&uri)) &&
                                    (is_app_url(uri.get()) || _wcsicmp(uri.get(), L"about:srcdoc") == 0 ||
                                     _wcsicmp(uri.get(), L"about:blank") == 0);
                    if (!ok) {
                        args->put_Cancel(TRUE);
                        if (auto self = weak.lock()) self->info("cancelled a frame navigation away from the UI");
                    }
                    return S_OK;
                })
                .Get(),
            &token);

        // 3. No new windows (window.open, target=_blank, middle click).
        webview->add_NewWindowRequested(
            Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [weak](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                    if (args) args->put_Handled(TRUE);
                    if (auto self = weak.lock()) self->info("blocked a new-window request");
                    return S_OK;
                })
                .Get(),
            &token);

        // 4. Deny every permission request.
        webview->add_PermissionRequested(
            Callback<ICoreWebView2PermissionRequestedEventHandler>(
                [weak](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                    if (args) args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                    if (auto self = weak.lock()) self->info("denied a permission request");
                    return S_OK;
                })
                .Get(),
            &token);

        // 5. No downloads.
        if (auto webview4 = webview.try_query<ICoreWebView2_4>()) {
            webview4->add_DownloadStarting(
                Callback<ICoreWebView2DownloadStartingEventHandler>(
                    [weak](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
                        if (args) {
                            args->put_Cancel(TRUE);
                            args->put_Handled(TRUE);
                        }
                        if (auto self = weak.lock()) self->info("cancelled a download");
                        return S_OK;
                    })
                    .Get(),
                &token);
        }

        // 6. No hand-off to other apps through URI schemes (mailto:, ms-settings:, ...).
        if (auto webview18 = webview.try_query<ICoreWebView2_18>()) {
            webview18->add_LaunchingExternalUriScheme(
                Callback<ICoreWebView2LaunchingExternalUriSchemeEventHandler>(
                    [weak](ICoreWebView2*, ICoreWebView2LaunchingExternalUriSchemeEventArgs* args) -> HRESULT {
                        if (args) args->put_Cancel(TRUE);
                        if (auto self = weak.lock()) self->info("blocked an external URI scheme launch");
                        return S_OK;
                    })
                    .Get(),
                &token);
        }

        // Diagnostics: a failed page load and a crashed renderer are worth a log line.
        webview->add_NavigationCompleted(
            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [weak](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                    auto self = weak.lock();
                    if (!self || !args) return S_OK;
                    BOOL success = FALSE;
                    args->get_IsSuccess(&success);
                    if (!success) {
                        COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                        args->get_WebErrorStatus(&status);
                        self->error("UI navigation failed, web error status " + std::to_string(static_cast<int>(status)));
                    }
                    return S_OK;
                })
                .Get(),
            &token);
        webview->add_ProcessFailed(
            Callback<ICoreWebView2ProcessFailedEventHandler>(
                [weak](ICoreWebView2* sender, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                    auto self = weak.lock();
                    if (!self || !args) return S_OK;
                    COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                    args->get_ProcessFailedKind(&kind);
                    self->error("a WebView2 process failed, kind " + std::to_string(static_cast<int>(kind)));
                    // A crashed renderer leaves a blank view: reload the UI so the app stays usable.
                    if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED && sender) sender->Reload();
                    return S_OK;
                })
                .Get(),
            &token);
    }

    void register_messaging() {
        std::weak_ptr<Impl> weak = weak_from_this();
        EventRegistrationToken token{};
        // This event fires only for the top-level document. Frames need a separate registration
        // (ICoreWebView2Frame2), which is deliberately never made, so frame content cannot reach
        // the host. The source check below is a second guard on top of that.
        webview->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [weak](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                    auto self = weak.lock();
                    if (!self || !args) return S_OK;
                    wil::unique_cotaskmem_string source;
                    if (FAILED(args->get_Source(&source)) || !is_app_url(source.get())) {
                        self->error("dropped a web message from an unexpected origin");
                        return S_OK;
                    }
                    wil::unique_cotaskmem_string json;
                    if (FAILED(args->get_WebMessageAsJson(&json)) || !json) return S_OK;
                    if (self->on_message) self->on_message(win::narrow(json.get()));
                    return S_OK;
                })
                .Get(),
            &token);
    }

    void register_input() {
        std::weak_ptr<Impl> weak = weak_from_this();
        EventRegistrationToken token{};

        // Keys that Windows normally handles for the window (Alt+F4, Alt+Space) arrive here while
        // focus is inside the web content. Pass them on to the host window.
        controller->add_AcceleratorKeyPressed(
            Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
                [weak](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                    auto self = weak.lock();
                    if (!self || !args) return S_OK;
                    COREWEBVIEW2_KEY_EVENT_KIND kind = COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN;
                    UINT key = 0;
                    args->get_KeyEventKind(&kind);
                    args->get_VirtualKey(&key);
                    if (kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) return S_OK;
                    if (key == VK_F4) {
                        args->put_Handled(TRUE);
                        PostMessageW(self->config.parent, WM_CLOSE, 0, 0);
                    } else if (key == VK_SPACE) {
                        args->put_Handled(TRUE);
                        PostMessageW(self->config.parent, WM_SYSCOMMAND, SC_KEYMENU, ' ');
                    }
                    return S_OK;
                })
                .Get(),
            &token);

        // Tab at the last (or Shift+Tab at the first) focusable element asks the host to take
        // focus. The web view is the only control in the window, so focus wraps back into it.
        controller->add_MoveFocusRequested(
            Callback<ICoreWebView2MoveFocusRequestedEventHandler>(
                [weak](ICoreWebView2Controller* sender, ICoreWebView2MoveFocusRequestedEventArgs* args) -> HRESULT {
                    if (!sender || !args) return S_OK;
                    COREWEBVIEW2_MOVE_FOCUS_REASON reason = COREWEBVIEW2_MOVE_FOCUS_REASON_NEXT;
                    args->get_Reason(&reason);
                    args->put_Handled(TRUE);
                    sender->MoveFocus(reason);
                    return S_OK;
                })
                .Get(),
            &token);
    }
};

WebViewHost::WebViewHost() : impl_(std::make_shared<Impl>()) {}

WebViewHost::~WebViewHost() { close(); }

void WebViewHost::create(const WebViewConfig& config, MessageHandler on_message, ReadyHandler on_ready,
                         LogHandler log) {
    impl_->config = config;
    impl_->on_message = std::move(on_message);
    impl_->on_ready = std::move(on_ready);
    impl_->log = std::move(log);
    GetClientRect(config.parent, &impl_->pending_bounds);
    impl_->start();
}

void WebViewHost::resize(const RECT& bounds_in_parent) {
    impl_->pending_bounds = bounds_in_parent;
    if (impl_->controller) impl_->controller->put_Bounds(bounds_in_parent);
}

void WebViewHost::set_visible(bool visible) {
    if (impl_->controller) impl_->controller->put_IsVisible(visible ? TRUE : FALSE);
}

void WebViewHost::focus() {
    if (impl_->controller) impl_->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void WebViewHost::notify_parent_moved() {
    if (impl_->controller) impl_->controller->NotifyParentWindowPositionChanged();
}

void WebViewHost::post_json(std::string_view json_utf8) {
    if (!impl_->webview || impl_->closed) return;
    const std::wstring wide = win::widen(json_utf8);
    impl_->webview->PostWebMessageAsJson(wide.c_str());
}

void WebViewHost::close() {
    if (impl_->closed) return;
    impl_->closed = true;
    if (impl_->controller) impl_->controller->Close();
    impl_->webview.reset();
    impl_->controller.reset();
    impl_->environment.reset();
    impl_->on_message = nullptr;
    impl_->on_ready = nullptr;
}

bool WebViewHost::ready() const { return impl_->webview != nullptr && !impl_->closed; }

}  // namespace buddy
