#pragma once

#include <string>

#include "providers/http.h"

namespace buddy {

// IHttpTransport on top of WinHTTP, the HTTP client built into Windows (STATUS.md D16).
//
//  - Proxy: the session uses automatic proxy detection (WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY).
//    Loopback hosts always go direct.
//  - TLS: certificate validation is left at the WinHTTP default, which is "on". Nothing in this
//    class relaxes it.
//  - Redirects are refused, so an Authorization header can never be replayed to another host.
//  - Streaming: body data is handed to the caller as it arrives.
//  - Cancel: when the stop token fires, the request handle is closed, which makes the blocking
//    WinHTTP call on the worker thread return at once.
class WinHttpTransport final : public IHttpTransport {
public:
    explicit WinHttpTransport(std::wstring user_agent);
    ~WinHttpTransport() override;

    WinHttpTransport(const WinHttpTransport&) = delete;
    WinHttpTransport& operator=(const WinHttpTransport&) = delete;

    HttpResponse send(const HttpRequest& request, const HttpHandlers& handlers,
                      std::stop_token stop) override;

private:
    void* session_ = nullptr;   // HINTERNET; kept opaque so this header needs no Windows includes
};

}  // namespace buddy
