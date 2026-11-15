#pragma once

#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace buddy {

struct HttpRequest {
    std::string method = "POST";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int connect_timeout_ms = 15'000;
    // Longest silence allowed between two pieces of the response. Generous on purpose: a local
    // model can take a long time to load before the first token arrives (PROJECT.md feature 2).
    int idle_timeout_ms = 180'000;
};

enum class HttpOutcome {
    Completed,       // a full response was received (check `status`)
    Cancelled,       // the stop token fired
    ConnectFailed,   // DNS failure or nothing listening
    Timeout,
    TlsFailed,       // certificate or handshake problem
    InvalidRequest,  // bad URL or header
    Failed,          // anything else; see error_detail
};

struct HttpResponse {
    HttpOutcome outcome = HttpOutcome::Failed;
    int status = 0;
    std::string error_detail;
};

struct HttpHandlers {
    // Called once, before any body data, with the status code and the Content-Type header.
    std::function<void(int status, std::string_view content_type)> on_head;
    // Called for each piece of the body as it arrives. This is what makes streaming work.
    std::function<void(std::string_view chunk)> on_chunk;
};

// A blocking HTTP call that streams the response body. Run it on a worker thread.
class IHttpTransport {
public:
    virtual ~IHttpTransport() = default;
    virtual HttpResponse send(const HttpRequest& request, const HttpHandlers& handlers,
                              std::stop_token stop) = 0;
};

}  // namespace buddy
