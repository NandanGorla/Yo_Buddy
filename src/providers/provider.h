#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace buddy {

enum class Role { System, User, Assistant };

struct ChatMessage {
    Role role = Role::User;
    std::string text;
};

// An image sent along with the newest user message (screenshots, from M4 on).
struct ImageInput {
    std::string media_type;   // "image/png" or "image/jpeg"
    std::string base64;       // the encoded bytes, without a "data:" prefix
};

// A function the model may call. `parameters` is a JSON Schema object.
struct ToolSpec {
    std::string name;
    std::string description;
    nlohmann::json parameters;
};

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments;   // raw JSON text exactly as the model produced it: validate before use
};

struct ChatOptions {
    std::string model;
    int max_output_tokens = 0;   // 0 = let the server decide
};

enum class ChatStatus {
    Ok,
    Cancelled,
    NotConfigured,   // no model chosen
    AuthFailed,      // 401 / 403
    RateLimited,     // 429
    BadRequest,      // 400 / 404 / 422: wrong model name, wrong endpoint, rejected parameters
    ServerError,     // 5xx
    NetworkError,    // could not connect, TLS failure
    Timeout,
    BadResponse,     // the server answered, but not with something we understand
};

// A short machine-readable name for each status, sent to the UI as the error code.
const char* to_code(ChatStatus status);

struct ChatResult {
    ChatStatus status = ChatStatus::BadResponse;
    std::string message;                    // user-facing text when status != Ok
    std::string stop_reason;                // "stop", "length", "tool_calls", ...
    std::vector<ToolCall> tool_calls;
    std::optional<std::int64_t> input_tokens;
    std::optional<std::int64_t> output_tokens;
    int http_status = 0;
};

struct ChatSink {
    // Called on the calling (worker) thread for every piece of reply text, in order.
    std::function<void(std::string_view text)> on_text;
};

// One interface for every model backend (PROJECT.md feature 1). The call blocks until the reply
// is complete, an error occurs or `stop` fires, and streams text through `sink` meanwhile.
// `images` belong to the last user message in `messages`.
class IChatProvider {
public:
    virtual ~IChatProvider() = default;

    virtual ChatResult chat(const std::vector<ChatMessage>& messages,
                            const std::vector<ImageInput>& images,
                            const std::vector<ToolSpec>& tools,
                            const ChatOptions& options,
                            const ChatSink& sink,
                            std::stop_token stop) = 0;
};

}  // namespace buddy
