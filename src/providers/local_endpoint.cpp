#include "providers/local_endpoint.h"

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>

#include "core/url.h"
#include "providers/openai_compat.h"

namespace buddy {
namespace {

using json = nlohmann::json;

struct JsonResponse {
    HttpResponse response;
    json body;
};

JsonResponse get_json(IHttpTransport& transport, const std::string& url, std::stop_token stop) {
    std::string body;
    int status = 0;
    HttpRequest request;
    request.method = "GET";
    request.url = url;
    request.headers.emplace_back("Accept", "application/json");
    request.connect_timeout_ms = 1500;
    request.idle_timeout_ms = 5000;
    HttpHandlers handlers;
    handlers.on_head = [&](int code, std::string_view) { status = code; };
    handlers.on_chunk = [&](std::string_view chunk) { body.append(chunk); };
    JsonResponse result;
    result.response = transport.send(request, handlers, stop);
    result.response.status = status != 0 ? status : result.response.status;
    if (result.response.outcome == HttpOutcome::Completed) {
        result.body = json::parse(body, nullptr, false);
    }
    return result;
}

std::string root_url(const url::BaseUrl& endpoint) {
    std::string root = endpoint.to_string();
    if (endpoint.path == "/v1") root.resize(root.size() - 3);
    return root;
}

LocalServerKind infer_kind(const url::BaseUrl& endpoint) {
    if (endpoint.port == 11434) return LocalServerKind::Ollama;
    if (endpoint.port == 1234) return LocalServerKind::LmStudio;
    if (endpoint.port == 9931 || endpoint.port == 8080) return LocalServerKind::LlamaServer;
    return LocalServerKind::OpenAiCompatible;
}

void merge_capability(LocalModel& model, const json& value) {
    if (!value.is_object()) return;
    if (const auto it = value.find("context_length"); it != value.end() && it->is_number_integer()) {
        model.context_length = it->get<std::int64_t>();
    }
    if (const auto it = value.find("max_context_length"); it != value.end() && it->is_number_integer()) {
        model.context_length = it->get<std::int64_t>();
    }
    const auto capabilities = value.find("capabilities");
    if (capabilities != value.end() && capabilities->is_array()) {
        for (const json& capability : *capabilities) {
            if (!capability.is_string()) continue;
            const std::string name = capability.get<std::string>();
            if (name == "vision" || name == "image") model.vision = true;
            if (name == "tools" || name == "tool_use") model.tools = true;
        }
    }
    const auto modalities = value.find("modalities");
    if (modalities != value.end() && modalities->is_array()) {
        for (const json& modality : *modalities) {
            if (modality.is_string() && modality.get<std::string>() == "vision") model.vision = true;
        }
    }
    const auto input_modalities = value.find("input_modalities");
    if (input_modalities != value.end() && input_modalities->is_array()) {
        for (const json& modality : *input_modalities) {
            if (modality.is_string() && (modality.get<std::string>() == "vision" ||
                                         modality.get<std::string>() == "image")) {
                model.vision = true;
            }
        }
    }
}

void parse_models(const json& root, std::vector<LocalModel>& models) {
    const json* list = nullptr;
    if (root.is_object() && root.contains("data") && root["data"].is_array()) list = &root["data"];
    if (root.is_object() && root.contains("models") && root["models"].is_array()) list = &root["models"];
    if (!list) return;
    for (const json& item : *list) {
        if (!item.is_object()) continue;
        std::string id = item.value("id", item.value("name", item.value("model", "")));
        if (id.empty()) continue;
        LocalModel model;
        model.id = std::move(id);
        merge_capability(model, item);
        if (const auto details = item.find("details"); details != item.end()) merge_capability(model, *details);
        if (const auto architecture = item.find("architecture"); architecture != item.end()) {
            merge_capability(model, *architecture);
        }
        models.push_back(std::move(model));
    }
}

LocalModel* find_model(std::vector<LocalModel>& models, const std::string& id) {
    for (LocalModel& model : models) {
        if (model.id == id) return &model;
    }
    return nullptr;
}

void parse_llama_props(const json& root, std::vector<LocalModel>& models) {
    if (!root.is_object()) return;
    std::optional<std::int64_t> context;
    if (const auto it = root.find("n_ctx"); it != root.end() && it->is_number_integer()) {
        context = it->get<std::int64_t>();
    }
    std::optional<bool> vision;
    if (const auto it = root.find("modalities"); it != root.end() && it->is_array()) {
        for (const json& item : *it) {
            if (item.is_string() && item.get<std::string>() == "vision") vision = true;
        }
    }
    for (LocalModel& model : models) {
        if (context) model.context_length = context;
        if (vision) model.vision = vision;
    }
}

std::string discovery_error(const JsonResponse& response) {
    if (response.response.outcome == HttpOutcome::ConnectFailed) {
        return "No local model server is listening at this endpoint. Start Ollama "
               "(https://ollama.com), LM Studio (https://lmstudio.ai), or llama-server, then try again.";
    }
    if (response.response.outcome == HttpOutcome::Timeout) return "The local model server did not respond in time.";
    if (response.response.outcome == HttpOutcome::Cancelled) return "Discovery was cancelled.";
    if (response.response.status == 401 || response.response.status == 403) {
        return "The local server requires authentication; configure its OpenAI-compatible endpoint manually.";
    }
    if (response.response.status == 404) return "This endpoint does not expose a model list at /v1/models.";
    return "The local server returned an invalid model list.";
}

}  // namespace

const char* local_server_code(LocalServerKind kind) {
    switch (kind) {
        case LocalServerKind::Ollama: return "ollama";
        case LocalServerKind::LmStudio: return "lm_studio";
        case LocalServerKind::LlamaServer: return "llama_server";
        case LocalServerKind::OpenAiCompatible: return "openai_compatible";
        case LocalServerKind::Unknown: return "unknown";
    }
    return "unknown";
}

LocalEndpointConnector::LocalEndpointConnector(IHttpTransport& transport) : transport_(transport) {}

LocalDiscovery LocalEndpointConnector::discover(std::string endpoint, std::stop_token stop) {
    LocalDiscovery result;
    const std::array<std::string, 5> candidates = {
        endpoint, "http://127.0.0.1:9931/v1", "http://127.0.0.1:8080/v1",
        "http://127.0.0.1:11434/v1", "http://127.0.0.1:1234/v1"};
    std::string last_error;
    for (const std::string& candidate : candidates) {
        if (candidate.empty()) continue;
        const url::BaseUrlResult parsed = url::parse_base_url(candidate);
        if (!parsed.ok()) {
            if (endpoint == candidate) {
                result.error = parsed.error;
                return result;
            }
            continue;
        }
        if (!url::is_loopback_host(parsed.url->host)) {
            if (endpoint == candidate) {
                result.error = "Local discovery only accepts a loopback endpoint (localhost, 127.0.0.1, or ::1).";
                return result;
            }
            continue;
        }

        const std::string candidate_endpoint = parsed.url->to_string();
        const JsonResponse models = get_json(transport_, candidate_endpoint + "/models", stop);
        if (models.response.outcome != HttpOutcome::Completed || models.response.status != 200) {
            last_error = discovery_error(models);
            if (!endpoint.empty()) {
                result.error = last_error;
                return result;
            }
            continue;
        }
        result.endpoint = candidate_endpoint;
        result.server = infer_kind(*parsed.url);
        const std::string root = root_url(*parsed.url);
        parse_models(models.body, result.models);
        if (result.models.empty()) {
            last_error = "The local server answered, but it reported no available models.";
            if (!endpoint.empty()) {
                result.error = last_error;
                return result;
            }
            continue;
        }

        if (result.server == LocalServerKind::Ollama) {
            const JsonResponse tags = get_json(transport_, root + "/api/tags", stop);
            if (tags.response.status == 200) parse_models(tags.body, result.models);
        } else if (result.server == LocalServerKind::LlamaServer) {
            const JsonResponse props = get_json(transport_, root + "/props", stop);
            if (props.response.status == 200) parse_llama_props(props.body, result.models);
        } else if (result.server == LocalServerKind::LmStudio) {
            const JsonResponse rich = get_json(transport_, root + "/api/v1/models", stop);
            if (rich.response.status == 200) {
                for (const json& item : rich.body.value("models", json::array())) {
                    if (!item.is_object()) continue;
                    const std::string id = item.value("id", item.value("name", ""));
                    if (LocalModel* model = find_model(result.models, id)) merge_capability(*model, item);
                }
            }
        }

        // Rich server APIs may repeat a model already returned by /v1/models.
        std::sort(result.models.begin(), result.models.end(),
                  [](const LocalModel& a, const LocalModel& b) { return a.id < b.id; });
        result.models.erase(std::unique(result.models.begin(), result.models.end(),
                                        [](const LocalModel& a, const LocalModel& b) { return a.id == b.id; }),
                            result.models.end());
        result.ok = true;
        return result;
    }
    result.error = last_error.empty()
                       ? "No supported local model server was found on the default loopback ports (9931, 8080, 11434, 1234)."
                       : last_error;
    return result;
}

ChatResult LocalEndpointConnector::test_model(std::string endpoint, std::string model, std::stop_token stop) {
    const url::BaseUrlResult parsed = url::parse_base_url(endpoint);
    if (!parsed.ok()) return {ChatStatus::BadRequest, parsed.error};
    if (!url::is_loopback_host(parsed.url->host)) {
        return {ChatStatus::BadRequest, "Model tests are restricted to loopback endpoints."};
    }
    OpenAiCompatProvider provider(transport_, {parsed.url->to_string(), {}});
    const std::vector<ChatMessage> messages{{Role::User, "Reply with the word OK."}};
    const ImageInput image{"image/png",
                           "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="};
    ChatOptions options;
    options.model = std::move(model);
    options.max_output_tokens = 16;
    return provider.chat(messages, {image}, {}, options, {}, stop);
}

}  // namespace buddy
