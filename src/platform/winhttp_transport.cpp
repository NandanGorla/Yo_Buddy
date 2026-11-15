#include "platform/winhttp_transport.h"

#include <mutex>

#include <windows.h>
#include <winhttp.h>

#include "core/url.h"
#include "platform/win_text.h"

namespace buddy {
namespace {

// Closes a WinHTTP handle when it goes out of scope.
struct ScopedHandle {
    HINTERNET handle = nullptr;
    ~ScopedHandle() {
        if (handle) WinHttpCloseHandle(handle);
    }
};

// The request handle, shared between the worker thread (which blocks inside WinHTTP calls) and
// whichever thread requests the stop. Closing the handle is WinHTTP's way to abort a pending
// call; the mutex makes sure it is closed exactly once.
struct CancellableRequest {
    std::mutex mutex;
    HINTERNET handle = nullptr;
    bool cancelled = false;

    void cancel() {
        std::lock_guard lock(mutex);
        cancelled = true;
        if (handle) {
            WinHttpCloseHandle(handle);
            handle = nullptr;
        }
    }
    void close() {
        std::lock_guard lock(mutex);
        if (handle) {
            WinHttpCloseHandle(handle);
            handle = nullptr;
        }
    }
    bool is_cancelled() {
        std::lock_guard lock(mutex);
        return cancelled;
    }
};

HttpResponse failure(HttpOutcome outcome, std::string detail) {
    HttpResponse r;
    r.outcome = outcome;
    r.error_detail = std::move(detail);
    return r;
}

HttpResponse failure_from_last_error(DWORD error, const char* step) {
    HttpOutcome outcome = HttpOutcome::Failed;
    switch (error) {
        case ERROR_WINHTTP_TIMEOUT: outcome = HttpOutcome::Timeout; break;
        case ERROR_WINHTTP_CANNOT_CONNECT:
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: outcome = HttpOutcome::ConnectFailed; break;
        case ERROR_WINHTTP_SECURE_FAILURE:
        case ERROR_WINHTTP_SECURE_FAILURE_PROXY: outcome = HttpOutcome::TlsFailed; break;
        case ERROR_WINHTTP_OPERATION_CANCELLED: outcome = HttpOutcome::Cancelled; break;
        default: break;
    }
    return failure(outcome, std::string(step) + " failed with WinHTTP error " + std::to_string(error));
}

bool has_line_break(std::string_view s) { return s.find_first_of("\r\n") != std::string_view::npos; }

std::wstring query_header(HINTERNET request, DWORD info) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &bytes,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &bytes,
                             WINHTTP_NO_HEADER_INDEX)) {
        return {};
    }
    value.resize(bytes / sizeof(wchar_t));
    return value;
}

}  // namespace

WinHttpTransport::WinHttpTransport(std::wstring user_agent) {
    // Synchronous session (no WINHTTP_FLAG_ASYNC): each request blocks its own worker thread.
    session_ = WinHttpOpen(user_agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
}

WinHttpTransport::~WinHttpTransport() {
    if (session_) WinHttpCloseHandle(static_cast<HINTERNET>(session_));
}

HttpResponse WinHttpTransport::send(const HttpRequest& request, const HttpHandlers& handlers,
                                    std::stop_token stop) {
    if (!session_) return failure(HttpOutcome::Failed, "WinHTTP session could not be created");
    if (stop.stop_requested()) return failure(HttpOutcome::Cancelled, "cancelled before start");

    // --- split the URL ---
    const std::wstring url = win::widen(request.url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (url.empty() || !WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) {
        return failure(HttpOutcome::InvalidRequest, "the URL could not be parsed");
    }
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && parts.nScheme != INTERNET_SCHEME_HTTP) {
        return failure(HttpOutcome::InvalidRequest, "only http and https are supported");
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring object(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo && parts.dwExtraInfoLength > 0) object.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (object.empty()) object = L"/";

    // --- headers, with a guard against header injection ---
    std::wstring header_block;
    for (const auto& [name, value] : request.headers) {
        if (name.empty() || has_line_break(name) || has_line_break(value) ||
            name.find(':') != std::string::npos) {
            return failure(HttpOutcome::InvalidRequest, "a request header contains invalid characters");
        }
        header_block += win::widen(name) + L": " + win::widen(value) + L"\r\n";
    }

    // --- connect and open the request ---
    ScopedHandle connection;
    connection.handle = WinHttpConnect(static_cast<HINTERNET>(session_), host.c_str(), parts.nPort, 0);
    if (!connection.handle) return failure_from_last_error(GetLastError(), "WinHttpConnect");

    const std::wstring method = win::widen(request.method);
    CancellableRequest shared;
    shared.handle = WinHttpOpenRequest(connection.handle, method.c_str(), object.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       secure ? WINHTTP_FLAG_SECURE : 0);
    if (!shared.handle) return failure_from_last_error(GetLastError(), "WinHttpOpenRequest");
    const HINTERNET req = shared.handle;

    // From here on a stop request closes the handle, which aborts whichever call is blocking.
    std::stop_callback on_stop(stop, [&shared] { shared.cancel(); });
    const auto fail_step = [&](const char* step) {
        const DWORD error = GetLastError();
        const bool cancelled = shared.is_cancelled();
        shared.close();
        if (cancelled) return failure(HttpOutcome::Cancelled, "cancelled");
        return failure_from_last_error(error, step);
    };

    // Never follow redirects: the API endpoints do not use them, and following one could hand
    // the Authorization header to a different host.
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy));
    // No cookie jar and no automatic Windows authentication for API calls.
    DWORD disable = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
    disable = WINHTTP_DISABLE_AUTHENTICATION;
    WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));

    // A server on this PC is always reached directly, whatever the system proxy says.
    // WinHttpCrackUrl returns an IPv6 literal without its brackets.
    if (url::is_loopback_host(win::narrow(host)) || host == L"::1") {
        WINHTTP_PROXY_INFO direct{};
        direct.dwAccessType = WINHTTP_ACCESS_TYPE_NO_PROXY;
        WinHttpSetOption(req, WINHTTP_OPTION_PROXY, &direct, sizeof(direct));
    }

    WinHttpSetTimeouts(req, request.connect_timeout_ms, request.connect_timeout_ms, 30'000,
                       request.idle_timeout_ms);

    if (!header_block.empty() &&
        !WinHttpAddRequestHeaders(req, header_block.c_str(), static_cast<DWORD>(header_block.size()),
                                  WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
        return fail_step("WinHttpAddRequestHeaders");
    }

    // --- send ---
    const DWORD body_size = static_cast<DWORD>(request.body.size());
    void* body = body_size > 0 ? const_cast<char*>(request.body.data()) : WINHTTP_NO_REQUEST_DATA;
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, body, body_size, body_size, 0)) {
        return fail_step("WinHttpSendRequest");
    }
    if (!WinHttpReceiveResponse(req, nullptr)) return fail_step("WinHttpReceiveResponse");

    // --- status line and headers ---
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX)) {
        return fail_step("WinHttpQueryHeaders");
    }
    const std::string content_type = win::narrow(query_header(req, WINHTTP_QUERY_CONTENT_TYPE));
    if (handlers.on_head) handlers.on_head(static_cast<int>(status), content_type);

    // --- body, delivered as it arrives ---
    std::string buffer(16 * 1024, '\0');
    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(req, &available)) return fail_step("WinHttpQueryDataAvailable");
        if (available == 0) break;   // end of the response

        const DWORD to_read = available < buffer.size() ? available : static_cast<DWORD>(buffer.size());
        DWORD read = 0;
        if (!WinHttpReadData(req, buffer.data(), to_read, &read)) return fail_step("WinHttpReadData");
        if (read == 0) break;
        if (handlers.on_chunk) handlers.on_chunk(std::string_view(buffer.data(), read));
    }

    const bool cancelled = shared.is_cancelled();
    shared.close();
    HttpResponse response;
    response.outcome = cancelled ? HttpOutcome::Cancelled : HttpOutcome::Completed;
    response.status = static_cast<int>(status);
    return response;
}

}  // namespace buddy
