#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "providers/http.h"
#include "providers/provider.h"

namespace buddy {

struct OpenAiCompatConfig {
    std::string base_url;   // e.g. "https://api.anthropic.com/v1"; "/chat/completions" is appended
    std::string api_key;    // sent as "Authorization: Bearer <key>"; empty = no auth header
};

// A client for the OpenAI "chat completions" wire format with streaming. The same format is
// spoken by OpenAI, by Anthropic's compatibility endpoint, and by local servers such as
// llama-server, Ollama and LM Studio (STATUS.md D10).
//
// The client sends only fields that every one of those accepts: model, messages, stream,
// stream_options, max_completion_tokens and tools. It deliberately sends no sampling
// parameters (temperature, top_p): current Claude models reject non-default values.
class OpenAiCompatProvider final : public IChatProvider {
public:
    OpenAiCompatProvider(IHttpTransport& transport, OpenAiCompatConfig config);

    ChatResult chat(const std::vector<ChatMessage>& messages,
                    const std::vector<ImageInput>& images,
                    const std::vector<ToolSpec>& tools,
                    const ChatOptions& options,
                    const ChatSink& sink,
                    std::stop_token stop) override;

private:
    IHttpTransport& transport_;
    OpenAiCompatConfig config_;
};

// The request body for POST {base_url}/chat/completions. Exposed so tests can check the wire
// format without a network.
nlohmann::json build_chat_request(const std::vector<ChatMessage>& messages,
                                  const std::vector<ImageInput>& images,
                                  const std::vector<ToolSpec>& tools,
                                  const ChatOptions& options);

}  // namespace buddy
