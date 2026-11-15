#include "providers/anthropic.h"

#include <map>

#include "core/text.h"
#include "providers/sse_parser.h"

namespace buddy {
namespace {

using json = nlohmann::json;

std::string error_message(std::string_view body) {
    const json root = json::parse(body, nullptr, false);
    if (!root.is_object()) return {};
    const auto error = root.find("error");
    if (error == root.end() || !error->is_object()) return {};
    const auto message = error->find("message");
    return message != error->end() && message->is_string()
               ? text::truncate_utf8(message->get_ref<const std::string&>(), 600)
               : std::string();
}

ChatStatus status_for(int status) {
    if (status == 401 || status == 403) return ChatStatus::AuthFailed;
    if (status == 429) return ChatStatus::RateLimited;
    if (status == 400 || status == 404 || status == 413 || status == 422) return ChatStatus::BadRequest;
    if (status >= 500) return ChatStatus::ServerError;
    return ChatStatus::BadResponse;
}

class Decoder {
public:
    explicit Decoder(const ChatSink& sink) : sink_(sink) {}

    void event(std::string_view data) {
        const json root = json::parse(data, nullptr, false);
        if (!root.is_object()) {
            malformed_ = true;
            return;
        }
        saw_event_ = true;
        const std::string type = root.value("type", "");
        if (type == "message_start") {
            const auto message = root.find("message");
            if (message != root.end() && message->is_object()) {
                const auto usage = message->find("usage");
                if (usage != message->end()) read_usage(*usage);
            }
        } else if (type == "content_block_start") {
            const auto content_block = root.find("content_block");
            if (content_block != root.end() && content_block->is_object() &&
                content_block->value("type", "") == "tool_use") {
                const auto index = root.value("index", 0);
                ToolCall& call = tools_[index];
                call.id = content_block->value("id", "");
                call.name = content_block->value("name", "");
            }
        } else if (type == "content_block_delta") {
            const auto delta = root.find("delta");
            if (delta == root.end() || !delta->is_object()) return;
            const std::string delta_type = delta->value("type", "");
            if (delta_type == "text_delta") {
                const std::string text = delta->value("text", "");
                if (!text.empty()) {
                    saw_text_ = true;
                    if (sink_.on_text) sink_.on_text(text);
                }
            } else if (delta_type == "input_json_delta") {
                tools_[root.value("index", 0)].arguments += delta->value("partial_json", "");
            }
        } else if (type == "message_delta") {
            const auto delta = root.find("delta");
            if (delta != root.end() && delta->is_object()) stop_reason_ = delta->value("stop_reason", "");
            const auto usage = root.find("usage");
            if (usage != root.end()) read_usage(*usage);
        } else if (type == "error") {
            error_ = error_message(data);
            if (error_.empty()) error_ = "Anthropic reported an error while streaming.";
        }
    }

    void fill(ChatResult& result) const {
        result.stop_reason = stop_reason_;
        result.input_tokens = input_tokens_;
        result.output_tokens = output_tokens_;
        for (const auto& [index, call] : tools_) result.tool_calls.push_back(call);
    }
    bool malformed() const { return malformed_; }
    bool saw_event() const { return saw_event_; }
    bool saw_text() const { return saw_text_; }
    const std::string& error() const { return error_; }

private:
    void read_usage(const json& usage) {
        if (!usage.is_object()) return;
        if (usage.contains("input_tokens") && usage["input_tokens"].is_number_integer()) {
            input_tokens_ = usage["input_tokens"].get<std::int64_t>();
        }
        if (usage.contains("output_tokens") && usage["output_tokens"].is_number_integer()) {
            output_tokens_ = usage["output_tokens"].get<std::int64_t>();
        }
    }

    const ChatSink& sink_;
    std::map<int, ToolCall> tools_;
    std::string stop_reason_;
    std::string error_;
    std::optional<std::int64_t> input_tokens_;
    std::optional<std::int64_t> output_tokens_;
    bool malformed_ = false;
    bool saw_event_ = false;
    bool saw_text_ = false;
};

}  // namespace

nlohmann::json build_anthropic_request(const std::vector<ChatMessage>& messages,
                                       const std::vector<ImageInput>& images,
                                       const std::vector<ToolSpec>& tools,
                                       const ChatOptions& options) {
    json body;
    body["model"] = options.model;
    body["max_tokens"] = options.max_output_tokens > 0 ? options.max_output_tokens : 4096;
    body["stream"] = true;
    json out_messages = json::array();
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const ChatMessage& message = messages[i];
        if (message.role == Role::System) {
            if (!body.contains("system")) body["system"] = message.text;
            else body["system"] = body["system"].get<std::string>() + "\n\n" + message.text;
            continue;
        }
        json content = message.text;
        if (i + 1 == messages.size() && message.role == Role::User && !images.empty()) {
            content = json::array({{{"type", "text"}, {"text", message.text}}});
            for (const ImageInput& image : images) {
                content.push_back({{"type", "image"},
                                   {"source", {{"type", "base64"},
                                               {"media_type", image.media_type},
                                               {"data", image.base64}}}});
            }
        }
        out_messages.push_back({{"role", message.role == Role::User ? "user" : "assistant"},
                                {"content", std::move(content)}});
    }
    body["messages"] = std::move(out_messages);
    if (!tools.empty()) {
        json out_tools = json::array();
        for (const ToolSpec& tool : tools) {
            out_tools.push_back({{"name", tool.name},
                                 {"description", tool.description},
                                 {"input_schema", tool.parameters}});
        }
        body["tools"] = std::move(out_tools);
    }
    return body;
}

AnthropicProvider::AnthropicProvider(IHttpTransport& transport, AnthropicConfig config)
    : transport_(transport), config_(std::move(config)) {}

ChatResult AnthropicProvider::chat(const std::vector<ChatMessage>& messages,
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
    HttpRequest request;
    request.method = "POST";
    request.url = config_.base_url + "/messages";
    request.headers = {{"Content-Type", "application/json"},
                       {"Accept", "text/event-stream"},
                       {"x-api-key", config_.api_key},
                       {"anthropic-version", config_.api_version}};
    request.body = build_anthropic_request(messages, images, tools, options)
                       .dump(-1, ' ', false, json::error_handler_t::replace);

    int status = 0;
    std::string content_type;
    std::string error_body;
    SseParser sse;
    Decoder decoder(sink);
    HttpHandlers handlers;
    handlers.on_head = [&](int code, std::string_view type) {
        status = code;
        content_type = text::to_lower_ascii(type);
    };
    handlers.on_chunk = [&](std::string_view chunk) {
        if (status >= 200 && status < 300 && content_type.find("text/event-stream") != std::string::npos) {
            sse.feed(chunk, [&](std::string_view, std::string_view data) { decoder.event(data); });
        } else if (error_body.size() < 64 * 1024) {
            error_body.append(chunk.substr(0, 64 * 1024 - error_body.size()));
        }
    };
    const HttpResponse response = transport_.send(request, handlers, stop);
    result.http_status = response.status;
    if (response.outcome == HttpOutcome::Cancelled) {
        result.status = ChatStatus::Cancelled;
        decoder.fill(result);
        result.stop_reason = "cancelled";
        return result;
    }
    if (response.outcome == HttpOutcome::Timeout) {
        result.status = ChatStatus::Timeout;
        result.message = "The server took too long to respond.";
        return result;
    }
    if (response.outcome != HttpOutcome::Completed) {
        result.status = response.outcome == HttpOutcome::TlsFailed || response.outcome == HttpOutcome::ConnectFailed
                            ? ChatStatus::NetworkError
                            : ChatStatus::BadRequest;
        result.message = "The connection to Anthropic failed.";
        return result;
    }
    if (status < 200 || status >= 300) {
        result.status = status_for(status);
        result.message = error_message(error_body);
        if (result.message.empty()) result.message = "Anthropic rejected the request (HTTP " + std::to_string(status) + ").";
        return result;
    }
    decoder.fill(result);
    if (sse.overflowed() || decoder.malformed()) {
        result.status = ChatStatus::BadResponse;
        result.message = "Anthropic sent an invalid stream.";
    } else if (!decoder.error().empty()) {
        result.status = ChatStatus::ServerError;
        result.message = decoder.error();
    } else if (!decoder.saw_event() || (!decoder.saw_text() && result.tool_calls.empty())) {
        result.status = ChatStatus::BadResponse;
        result.message = "Anthropic returned an empty reply.";
    } else {
        result.status = ChatStatus::Ok;
        if (result.stop_reason.empty()) result.stop_reason = "stop";
    }
    return result;
}

}  // namespace buddy
