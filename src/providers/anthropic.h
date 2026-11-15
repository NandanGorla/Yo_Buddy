#pragma once

#include <string>

#include "providers/http.h"
#include "providers/provider.h"

namespace buddy {

struct AnthropicConfig {
    std::string base_url;
    std::string api_key;
    std::string api_version = "2023-06-01";
};

class AnthropicProvider final : public IChatProvider {
public:
    AnthropicProvider(IHttpTransport& transport, AnthropicConfig config);

    ChatResult chat(const std::vector<ChatMessage>& messages,
                    const std::vector<ImageInput>& images,
                    const std::vector<ToolSpec>& tools,
                    const ChatOptions& options,
                    const ChatSink& sink,
                    std::stop_token stop) override;

private:
    IHttpTransport& transport_;
    AnthropicConfig config_;
};

nlohmann::json build_anthropic_request(const std::vector<ChatMessage>& messages,
                                       const std::vector<ImageInput>& images,
                                       const std::vector<ToolSpec>& tools,
                                       const ChatOptions& options);

}  // namespace buddy
