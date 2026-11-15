#pragma once

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "providers/http.h"
#include "providers/provider.h"

namespace buddy {

enum class LocalServerKind { Unknown, Ollama, LmStudio, LlamaServer, OpenAiCompatible };

struct LocalModel {
    std::string id;
    std::optional<bool> vision;
    std::optional<bool> tools;
    std::optional<std::int64_t> context_length;
};

struct LocalDiscovery {
    bool ok = false;
    LocalServerKind server = LocalServerKind::Unknown;
    std::string endpoint;
    std::vector<LocalModel> models;
    std::string error;
};

const char* local_server_code(LocalServerKind kind);

// Discovers models and server-reported capabilities without sending credentials. All endpoints
// accepted by this class must resolve to loopback; chat itself remains OpenAI-compatible.
class LocalEndpointConnector final {
public:
    explicit LocalEndpointConnector(IHttpTransport& transport);

    LocalDiscovery discover(std::string endpoint, std::stop_token stop);
    ChatResult test_model(std::string endpoint, std::string model, std::stop_token stop);

private:
    IHttpTransport& transport_;
};

}  // namespace buddy
