#include "providers/openai_compat.h"

#include <map>

#include "core/text.h"
#include "providers/sse_parser.h"

namespace buddy {
namespace {

using json = nlohmann::json;

constexpr std::size_t kMaxErrorBodyBytes = 64 * 1024;
constexpr std::size_t kMaxErrorMessageBytes = 600;

const char* role_name(Role role) {
    switch (role) {
        case Role::System: return "system";
        case Role::User: return "user";
        case Role::Assistant: return "assistant";
    }
    return "user";
}

// Pulls a readable message out of an error body. Handles the OpenAI shape
// {"error":{"message":"..."}}, Anthropic's native shape {"type":"error","error":{"message":"..."}}
// and a bare {"message":"..."}. Falls back to a generic sentence: the raw body is never shown,
// because a proxy or captive portal can put arbitrary HTML there.
std::string extract_error_message(std::string_view body) {
    const json root = json::parse(body, nullptr, false);
    if (root.is_object()) {
        if (const auto err = root.find("error"); err != root.end()) {
            if (err->is_object()) {
                if (const auto msg = err->find("message"); msg != err->end() && msg->is_string()) {
                    return text::truncate_utf8(msg->get_ref<const std::string&>(), kMaxErrorMessageBytes);
                }
            } else if (err->is_string()) {
                return text::truncate_utf8(err->get_ref<const std::string&>(), kMaxErrorMessageBytes);
            }
        }
        if (const auto msg = root.find("message"); msg != root.end() && msg->is_string()) {
            return text::truncate_utf8(msg->get_ref<const std::string&>(), kMaxErrorMessageBytes);
        }
    }
    return {};
}

ChatStatus status_from_http(int status) {
    if (status == 401 || status == 403) return ChatStatus::AuthFailed;
    if (status == 429) return ChatStatus::RateLimited;
    if (status == 400 || status == 404 || status == 405 || status == 413 || status == 422) {
        return ChatStatus::BadRequest;
    }
    if (status >= 500) return ChatStatus::ServerError;
    return ChatStatus::BadResponse;
}

std::string default_message(ChatStatus status, int http_status) {
    switch (status) {
        case ChatStatus::AuthFailed:
            return "The server rejected the API key (HTTP " + std::to_string(http_status) + ").";
        case ChatStatus::RateLimited: return "The server is rate limiting requests. Try again shortly.";
        case ChatStatus::BadRequest:
            return "The server rejected the request (HTTP " + std::to_string(http_status) +
                   "). Check the model name and the endpoint URL.";
        case ChatStatus::ServerError:
            return "The server reported an error (HTTP " + std::to_string(http_status) + ").";
        default: return "Unexpected response from the server (HTTP " + std::to_string(http_status) + ").";
    }
}

// Decodes the "chat.completion.chunk" objects of a streamed reply.
class StreamDecoder {
public:
    explicit StreamDecoder(const ChatSink& sink) : sink_(sink) {}

    void on_event(std::string_view data) {
        if (data == "[DONE]") {
            saw_done_ = true;
            return;
        }
        const json chunk = json::parse(data, nullptr, false);
        if (!chunk.is_object()) {
            ++malformed_;
            return;
        }
        saw_chunk_ = true;

        // Some servers report a failure mid-stream as an event carrying an "error" member.
        if (const auto err = chunk.find("error"); err != chunk.end() && !err->is_null()) {
            stream_error_ = extract_error_message(data);
            if (stream_error_.empty()) stream_error_ = "The server reported an error while streaming.";
            return;
        }

        if (const auto usage = chunk.find("usage"); usage != chunk.end() && usage->is_object()) {
            if (const auto v = usage->find("prompt_tokens"); v != usage->end() && v->is_number_integer()) {
                input_tokens_ = v->get<std::int64_t>();
            }
            if (const auto v = usage->find("completion_tokens"); v != usage->end() && v->is_number_integer()) {
                output_tokens_ = v->get<std::int64_t>();
            }
        }

        const auto choices = chunk.find("choices");
        if (choices == chunk.end() || !choices->is_array() || choices->empty()) return;
        const json& choice = choices->front();
        if (!choice.is_object()) return;

        if (const auto reason = choice.find("finish_reason"); reason != choice.end() && reason->is_string()) {
            finish_reason_ = reason->get<std::string>();
        }

        const auto delta = choice.find("delta");
        if (delta == choice.end() || !delta->is_object()) return;

        if (const auto content = delta->find("content"); content != delta->end() && content->is_string()) {
            const std::string& piece = content->get_ref<const std::string&>();
            if (!piece.empty() && sink_.on_text) sink_.on_text(piece);
        }

        // Tool calls arrive in fragments keyed by "index": the id and name come first, then the
        // argument JSON text in pieces that must be concatenated.
        if (const auto calls = delta->find("tool_calls"); calls != delta->end() && calls->is_array()) {
            for (const json& call : *calls) {
                if (!call.is_object()) continue;
                std::int64_t index = 0;
                if (const auto i = call.find("index"); i != call.end() && i->is_number_integer()) {
                    index = i->get<std::int64_t>();
                }
                ToolCall& target = tool_calls_[index];
                if (const auto id = call.find("id"); id != call.end() && id->is_string()) {
                    target.id = id->get<std::string>();
                }
                if (const auto fn = call.find("function"); fn != call.end() && fn->is_object()) {
                    if (const auto name = fn->find("name"); name != fn->end() && name->is_string()) {
                        target.name += name->get_ref<const std::string&>();
                    }
                    if (const auto args = fn->find("arguments"); args != fn->end() && args->is_string()) {
                        target.arguments += args->get_ref<const std::string&>();
                    }
                }
            }
        }
    }

    bool saw_done() const { return saw_done_; }
    bool saw_chunk() const { return saw_chunk_; }
    int malformed() const { return malformed_; }
    const std::string& stream_error() const { return stream_error_; }
    const std::string& finish_reason() const { return finish_reason_; }

    void fill(ChatResult& result) const {
        result.stop_reason = finish_reason_;
        result.input_tokens = input_tokens_;
        result.output_tokens = output_tokens_;
        for (const auto& [index, call] : tool_calls_) result.tool_calls.push_back(call);
    }

private:
    const ChatSink& sink_;
    std::map<std::int64_t, ToolCall> tool_calls_;
    std::string finish_reason_;
    std::string stream_error_;
    std::optional<std::int64_t> input_tokens_;
    std::optional<std::int64_t> output_tokens_;
    bool saw_done_ = false;
    bool saw_chunk_ = false;
    int malformed_ = 0;
};

// Handles a server that ignored "stream": true and answered with one plain JSON object.
bool decode_whole_response(std::string_view body, const ChatSink& sink, ChatResult& result) {
    const json root = json::parse(body, nullptr, false);
    if (!root.is_object()) return false;
    const auto choices = root.find("choices");
    if (choices == root.end() || !choices->is_array() || choices->empty()) return false;
    const json& choice = choices->front();
    if (!choice.is_object()) return false;
    const auto message = choice.find("message");
    if (message == choice.end() || !message->is_object()) return false;
    if (const auto content = message->find("content"); content != message->end() && content->is_string()) {
        if (sink.on_text) sink.on_text(content->get_ref<const std::string&>());
    }
    if (const auto reason = choice.find("finish_reason"); reason != choice.end() && reason->is_string()) {
        result.stop_reason = reason->get<std::string>();
    }
    return true;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

}  // namespace

const char* to_code(ChatStatus status) {
    switch (status) {
        case ChatStatus::Ok: return "ok";
        case ChatStatus::Cancelled: return "cancelled";
        case ChatStatus::NotConfigured: return "not_configured";
        case ChatStatus::AuthFailed: return "auth_failed";
        case ChatStatus::RateLimited: return "rate_limited";
        case ChatStatus::BadRequest: return "bad_request";
        case ChatStatus::ServerError: return "server_error";
        case ChatStatus::NetworkError: return "network_error";
        case ChatStatus::Timeout: return "timeout";
        case ChatStatus::BadResponse: return "bad_response";
    }
    return "bad_response";
}

nlohmann::json build_chat_request(const std::vector<ChatMessage>& messages,
                                  const std::vector<ImageInput>& images,
                                  const std::vector<ToolSpec>& tools,
                                  const ChatOptions& options) {
    json body;
    body["model"] = options.model;
    body["stream"] = true;
    body["stream_options"] = {{"include_usage", true}};
    if (options.max_output_tokens > 0) body["max_completion_tokens"] = options.max_output_tokens;

    // Images attach to the newest user message.
    std::size_t last_user = messages.size();
    for (std::size_t i = messages.size(); i-- > 0;) {
        if (messages[i].role == Role::User) {
            last_user = i;
            break;
        }
    }

    json out_messages = json::array();
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const ChatMessage& m = messages[i];
        json entry;
        entry["role"] = role_name(m.role);
        if (i == last_user && !images.empty()) {
            json parts = json::array();
            parts.push_back({{"type", "text"}, {"text", m.text}});
            for (const ImageInput& image : images) {
                const std::string data_url = "data:" + image.media_type + ";base64," + image.base64;
                parts.push_back({{"type", "image_url"}, {"image_url", {{"url", data_url}}}});
            }
            entry["content"] = std::move(parts);
        } else {
            entry["content"] = m.text;
        }
        out_messages.push_back(std::move(entry));
    }
    body["messages"] = std::move(out_messages);

    if (!tools.empty()) {
        json out_tools = json::array();
        for (const ToolSpec& tool : tools) {
            json fn = {{"name", tool.name}, {"description", tool.description}};
            fn["parameters"] = tool.parameters.is_object() ? tool.parameters
                                                           : json{{"type", "object"}, {"properties", json::object()}};
            out_tools.push_back({{"type", "function"}, {"function", std::move(fn)}});
        }
        body["tools"] = std::move(out_tools);
    }
    return body;
}

OpenAiCompatProvider::OpenAiCompatProvider(IHttpTransport& transport, OpenAiCompatConfig config)
    : transport_(transport), config_(std::move(config)) {}

ChatResult OpenAiCompatProvider::chat(const std::vector<ChatMessage>& messages,
                                      const std::vector<ImageInput>& images,
                                      const std::vector<ToolSpec>& tools,
                                      const ChatOptions& options,
                                      const ChatSink& sink,
                                      std::stop_token stop) {
    ChatResult result;
    if (options.model.empty()) {
        result.status = ChatStatus::NotConfigured;
        result.message = "No model is selected. Choose one in Settings.";
        return result;
    }
    if (!images.empty()) {
        bool has_user = false;
        for (const ChatMessage& m : messages) has_user = has_user || m.role == Role::User;
        if (!has_user) {
            result.status = ChatStatus::BadRequest;
            result.message = "Images need a user message to attach to.";
            return result;
        }
    }

    HttpRequest request;
    request.method = "POST";
    request.url = config_.base_url + "/chat/completions";
    request.headers.emplace_back("Content-Type", "application/json");
    request.headers.emplace_back("Accept", "text/event-stream");
    if (!config_.api_key.empty()) {
        request.headers.emplace_back("Authorization", "Bearer " + config_.api_key);
    }
    request.body = build_chat_request(messages, images, tools, options)
                       .dump(-1, ' ', false, json::error_handler_t::replace);

    int status = 0;
    bool is_event_stream = false;
    std::string plain_body;   // error bodies, and replies from servers that did not stream
    SseParser sse;
    StreamDecoder decoder(sink);

    HttpHandlers handlers;
    handlers.on_head = [&](int http_status, std::string_view content_type) {
        status = http_status;
        is_event_stream = starts_with(text::to_lower_ascii(content_type), "text/event-stream");
    };
    handlers.on_chunk = [&](std::string_view chunk) {
        if (status == 200 && is_event_stream) {
            sse.feed(chunk, [&](std::string_view, std::string_view data) { decoder.on_event(data); });
        } else if (plain_body.size() < kMaxErrorBodyBytes) {
            plain_body.append(chunk.substr(0, kMaxErrorBodyBytes - plain_body.size()));
        }
    };

    const HttpResponse response = transport_.send(request, handlers, stop);
    result.http_status = response.status;

    switch (response.outcome) {
        case HttpOutcome::Completed: break;
        case HttpOutcome::Cancelled:
            result.status = ChatStatus::Cancelled;
            decoder.fill(result);
            result.stop_reason = "cancelled";
            return result;
        case HttpOutcome::Timeout:
            result.status = ChatStatus::Timeout;
            result.message = "The server took too long to respond.";
            return result;
        case HttpOutcome::ConnectFailed:
            result.status = ChatStatus::NetworkError;
            result.message = "Could not connect to the server. Check the endpoint URL and your connection.";
            return result;
        case HttpOutcome::TlsFailed:
            result.status = ChatStatus::NetworkError;
            result.message = "The secure connection could not be verified. The request was not sent.";
            return result;
        case HttpOutcome::InvalidRequest:
            result.status = ChatStatus::BadRequest;
            result.message = "The endpoint URL or a request header is not valid.";
            return result;
        case HttpOutcome::Failed:
            result.status = ChatStatus::NetworkError;
            result.message = "The connection failed while waiting for the reply.";
            return result;
    }

    if (response.status != 200) {
        result.status = status_from_http(response.status);
        result.message = extract_error_message(plain_body);
        if (result.message.empty()) result.message = default_message(result.status, response.status);
        return result;
    }

    if (!is_event_stream) {
        if (decode_whole_response(plain_body, sink, result)) {
            result.status = ChatStatus::Ok;
            if (result.stop_reason.empty()) result.stop_reason = "stop";
            return result;
        }
        result.status = ChatStatus::BadResponse;
        result.message = "The server did not answer in the chat completions format. "
                         "Check that the endpoint URL points at an OpenAI-compatible API.";
        return result;
    }

    decoder.fill(result);
    if (sse.overflowed()) {
        result.status = ChatStatus::BadResponse;
        result.message = "The server sent an oversized stream event.";
        return result;
    }
    if (!decoder.stream_error().empty()) {
        result.status = ChatStatus::ServerError;
        result.message = decoder.stream_error();
        return result;
    }
    if (!decoder.saw_chunk()) {
        result.status = ChatStatus::BadResponse;
        result.message = "The server closed the stream without sending a reply.";
        return result;
    }
    if (!decoder.saw_done() && decoder.finish_reason().empty()) {
        // The connection ended mid-reply: neither the "[DONE]" marker nor a finish reason arrived.
        result.status = ChatStatus::BadResponse;
        result.message = "The reply was cut off before the server finished sending it.";
        return result;
    }

    result.status = ChatStatus::Ok;
    if (result.stop_reason.empty()) result.stop_reason = "stop";
    return result;
}

}  // namespace buddy
