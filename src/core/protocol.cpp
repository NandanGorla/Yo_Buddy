#include "core/protocol.h"

#include <initializer_list>

#include "core/text.h"
#include "core/theme.h"

namespace buddy::protocol {
namespace {

using json = nlohmann::json;

ParseResult fail(std::string code, std::string detail) {
    ParseResult r;
    r.error_code = std::move(code);
    r.error_detail = std::move(detail);
    return r;
}

ParseResult ok(UiMessage message) {
    ParseResult r;
    r.message = std::move(message);
    return r;
}

// True when `obj` has exactly the listed keys: nothing missing, nothing extra.
bool has_exactly(const json& obj, std::initializer_list<const char*> keys) {
    if (!obj.is_object() || obj.size() != keys.size()) return false;
    for (const char* key : keys) {
        if (!obj.contains(key)) return false;
    }
    return true;
}

// Reads a string field. Returns nullptr when the field is not a string.
const std::string* string_field(const json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return nullptr;
    return it->get_ptr<const std::string*>();
}

bool is_model_char(unsigned char c) {
    return text::is_ascii_alnum(c) || c == '.' || c == '_' || c == ':' || c == '/' || c == '@' ||
           c == '-';
}

ParseResult bad_payload(std::string_view type, const char* what) {
    return fail("invalid_payload", std::string(type) + ": " + what);
}

ParseResult parse_payload(const std::string& type, const json& p) {
    if (type == "app.ready") {
        if (!has_exactly(p, {})) return bad_payload(type, "expected an empty payload");
        return ok(AppReady{});
    }
    if (type == "chat.send") {
        if (!has_exactly(p, {"request_id", "text"})) return bad_payload(type, "expected request_id and text");
        const auto* id = string_field(p, "request_id");
        const auto* text = string_field(p, "text");
        if (!id || !is_valid_id(*id)) return bad_payload(type, "request_id is not a valid id");
        if (!text || text->empty()) return bad_payload(type, "text must be a non-empty string");
        if (text->size() > kMaxChatTextBytes) return bad_payload(type, "text is too long");
        return ok(ChatSend{*id, *text});
    }
    if (type == "chat.cancel") {
        if (!has_exactly(p, {"request_id"})) return bad_payload(type, "expected request_id");
        const auto* id = string_field(p, "request_id");
        if (!id || !is_valid_id(*id)) return bad_payload(type, "request_id is not a valid id");
        return ok(ChatCancel{*id});
    }
    if (type == "chat.clear") {
        if (!has_exactly(p, {})) return bad_payload(type, "expected an empty payload");
        return ok(ChatClear{});
    }
    if (type == "settings.set_theme") {
        if (!has_exactly(p, {"theme"})) return bad_payload(type, "expected theme");
        const auto* theme = string_field(p, "theme");
        if (!theme || !is_valid_id(*theme)) return bad_payload(type, "theme is not a valid id");
        return ok(SetTheme{*theme});
    }
    if (type == "settings.set_overlay") {
        if (!has_exactly(p, {"ring", "dot", "caption"})) return bad_payload(type, "expected ring, dot and caption");
        const auto* ring = string_field(p, "ring");
        const auto* dot = string_field(p, "dot");
        const auto* caption = string_field(p, "caption");
        if (!ring || !dot || !caption || !theme::is_valid_color(*ring) || !theme::is_valid_color(*dot) ||
            !theme::is_valid_color(*caption)) {
            return bad_payload(type, "overlay colors must be valid hex colors");
        }
        return ok(SetOverlayPalette{*ring, *dot, *caption});
    }
    if (type == "settings.set_provider") {
        if (!has_exactly(p, {"provider", "base_url", "model"})) {
            return bad_payload(type, "expected provider, base_url and model");
        }
        const auto* provider = string_field(p, "provider");
        const auto* base_url = string_field(p, "base_url");
        const auto* model = string_field(p, "model");
        if (!provider || !is_valid_id(*provider)) return bad_payload(type, "provider is not a valid id");
        if (!base_url || base_url->empty() || base_url->size() > kMaxBaseUrlLength) {
            return bad_payload(type, "base_url must be a non-empty string of reasonable length");
        }
        if (!model || !is_valid_model(*model)) return bad_payload(type, "model has invalid characters");
        return ok(SetProvider{*provider, *base_url, *model});
    }
    if (type == "settings.set_api_key") {
        if (!has_exactly(p, {"provider", "key"})) return bad_payload(type, "expected provider and key");
        const auto* provider = string_field(p, "provider");
        const auto* key = string_field(p, "key");
        if (!provider || !is_valid_id(*provider)) return bad_payload(type, "provider is not a valid id");
        if (!key || !is_valid_api_key(*key)) {
            return bad_payload(type, "key must be 1 to 512 printable ASCII characters without spaces");
        }
        return ok(SetApiKey{*provider, *key});
    }
    if (type == "settings.clear_api_key") {
        if (!has_exactly(p, {"provider"})) return bad_payload(type, "expected provider");
        const auto* provider = string_field(p, "provider");
        if (!provider || !is_valid_id(*provider)) return bad_payload(type, "provider is not a valid id");
        return ok(ClearApiKey{*provider});
    }
    if (type == "ui.log") {
        if (!has_exactly(p, {"level", "message"})) return bad_payload(type, "expected level and message");
        const auto* level = string_field(p, "level");
        const auto* message = string_field(p, "message");
        if (!level || (*level != "info" && *level != "warn" && *level != "error")) {
            return bad_payload(type, "level must be info, warn or error");
        }
        if (!message || message->size() > kMaxLogLength) return bad_payload(type, "message is too long");
        return ok(UiLog{*level, *message});
    }
    if (type == "settings.set_hotkey") {
        if (!has_exactly(p, {"hotkey"})) return bad_payload(type, "expected hotkey");
        const auto* hotkey = string_field(p, "hotkey");
        if (!hotkey || hotkey->empty() || hotkey->size() > kMaxHotkeyLength) {
            return bad_payload(type, "hotkey must be a short non-empty string");
        }
        return ok(SetHotkey{*hotkey});
    }
    if (type == "settings.set_panel") {
        if (!has_exactly(p, {"hide_on_click_away", "pinned"})) {
            return bad_payload(type, "expected hide_on_click_away and pinned");
        }
        if (!p["hide_on_click_away"].is_boolean() || !p["pinned"].is_boolean()) {
            return bad_payload(type, "hide_on_click_away and pinned must be booleans");
        }
        return ok(SetPanel{p["hide_on_click_away"].get<bool>(), p["pinned"].get<bool>()});
    }
    if (type == "settings.set_capture") {
        if (!has_exactly(p, {"exclusions", "send_preview"})) {
            return bad_payload(type, "expected exclusions and send_preview");
        }
        if (!p["exclusions"].is_array() || p["exclusions"].size() > kMaxCaptureExclusions ||
            !p["send_preview"].is_boolean()) {
            return bad_payload(type, "invalid capture policy");
        }
        std::vector<std::string> exclusions;
        exclusions.reserve(p["exclusions"].size());
        for (const auto& value : p["exclusions"]) {
            if (!value.is_string() || value.get<std::string>().empty() ||
                value.get<std::string>().size() > kMaxCaptureExclusionLength) {
                return bad_payload(type, "capture exclusions must be bounded strings");
            }
            exclusions.push_back(value.get<std::string>());
        }
        return ok(SetCapturePolicy{std::move(exclusions), p["send_preview"].get<bool>()});
    }
    if (type == "overlay.debug") {
        if (!has_exactly(p, {"action", "caption"})) return bad_payload(type, "expected action and caption");
        const auto* action = string_field(p, "action");
        const auto* caption = string_field(p, "caption");
        if (!action || (*action != "demo" && *action != "clear" && *action != "indicator" &&
                        *action != "caption") ||
            !caption || caption->size() > 512) {
            return bad_payload(type, "invalid overlay debug command");
        }
        return ok(OverlayDebug{*action, *caption});
    }
    if (type == "window.hide") {
        if (!has_exactly(p, {})) return bad_payload(type, "expected an empty payload");
        return ok(WindowHide{});
    }
    if (type == "window.open_main") {
        if (!has_exactly(p, {"settings"}) || !p["settings"].is_boolean()) {
            return bad_payload(type, "expected a boolean settings field");
        }
        return ok(OpenMain{p["settings"].get<bool>()});
    }
    if (type == "local.test_model") {
        if (!has_exactly(p, {"endpoint", "model"})) return bad_payload(type, "expected endpoint and model");
        const auto* endpoint = string_field(p, "endpoint");
        const auto* model = string_field(p, "model");
        if (!endpoint || endpoint->empty() || endpoint->size() > kMaxBaseUrlLength) {
            return bad_payload(type, "endpoint must be a non-empty string of reasonable length");
        }
        if (!model || !is_valid_model(*model) || model->empty()) {
            return bad_payload(type, "model is invalid");
        }
        return ok(LocalTestModel{*endpoint, *model});
    }
    if (type == "capture.request") {
        if (!has_exactly(p, {"target", "preview"})) return bad_payload(type, "expected target and preview");
        const auto* target = string_field(p, "target");
        if (!target || (*target != "monitor" && *target != "window")) {
            return bad_payload(type, "target must be monitor or window");
        }
        if (!p["preview"].is_boolean()) return bad_payload(type, "preview must be a boolean");
        return ok(CaptureRequest{*target, p["preview"].get<bool>()});
    }
    return fail("unknown_type", "unknown message type");
}

}  // namespace

bool is_valid_id(std::string_view s) {
    if (s.empty() || s.size() > kMaxIdLength) return false;
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        const bool lower_alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!lower_alnum && c != '_' && c != '-') return false;
    }
    return true;
}

bool is_valid_model(std::string_view s) {
    if (s.size() > kMaxModelLength) return false;
    for (const char ch : s) {
        if (!is_model_char(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

bool is_valid_api_key(std::string_view s) {
    return !s.empty() && s.size() <= kMaxApiKeyLength && text::is_printable_ascii_token(s);
}

ParseResult parse_ui_message(std::string_view json_text) {
    if (json_text.size() > kMaxMessageBytes) return fail("too_large", "message exceeds the size limit");

    // The third argument turns exceptions off: malformed input yields a "discarded" value.
    const json root = json::parse(json_text, nullptr, false);
    if (root.is_discarded()) return fail("invalid_json", "message is not valid JSON");

    if (!has_exactly(root, {"v", "type", "payload"})) {
        return fail("invalid_envelope", "expected exactly the keys v, type and payload");
    }
    const json& version = root["v"];
    if (!version.is_number_integer()) return fail("invalid_envelope", "v must be an integer");
    if (version.get<std::int64_t>() != kVersion) {
        return fail("unsupported_version", "unsupported protocol version");
    }
    const auto* type = string_field(root, "type");
    if (!type || type->empty() || type->size() > kMaxIdLength) {
        return fail("invalid_envelope", "type must be a short string");
    }
    const json& payload = root["payload"];
    if (!payload.is_object()) return fail("invalid_envelope", "payload must be an object");

    return parse_payload(*type, payload);
}

std::string make_message(std::string_view type, nlohmann::json payload) {
    json envelope;
    envelope["v"] = kVersion;
    envelope["type"] = type;
    envelope["payload"] = std::move(payload);
    return envelope.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string app_state(nlohmann::json payload) { return make_message("app.state", std::move(payload)); }

std::string chat_started(std::string_view request_id, std::string_view provider, std::string_view model,
                         std::string_view user_text) {
    return make_message("chat.started", {{"request_id", request_id},
                                         {"provider", provider},
                                         {"model", model},
                                         {"text", user_text}});
}

std::string chat_history(nlohmann::json messages, std::string_view active_request,
                         std::string_view active_model, std::string_view partial) {
    json payload;
    payload["messages"] = std::move(messages);
    payload["active_request"] = active_request.empty() ? json(nullptr) : json(active_request);
    payload["active_model"] = active_model;
    payload["partial"] = partial;
    return make_message("chat.history", std::move(payload));
}

std::string focus_input() { return make_message("ui.focus_input", json::object()); }

std::string show_settings() { return make_message("ui.show_settings", json::object()); }

std::string chat_delta(std::string_view request_id, std::string_view text) {
    return make_message("chat.delta", {{"request_id", request_id}, {"text", text}});
}

std::string chat_done(std::string_view request_id, std::string_view stop_reason,
                      std::optional<std::int64_t> input_tokens,
                      std::optional<std::int64_t> output_tokens) {
    json payload = {{"request_id", request_id}, {"stop_reason", stop_reason}};
    payload["input_tokens"] = input_tokens ? json(*input_tokens) : json(nullptr);
    payload["output_tokens"] = output_tokens ? json(*output_tokens) : json(nullptr);
    return make_message("chat.done", std::move(payload));
}

std::string chat_error(std::string_view request_id, std::string_view code, std::string_view message) {
    return make_message("chat.error",
                        {{"request_id", request_id}, {"code", code}, {"message", message}});
}

std::string chat_cleared() { return make_message("chat.cleared", json::object()); }

std::string host_error(std::string_view code, std::string_view message) {
    return make_message("host.error", {{"code", code}, {"message", message}});
}

std::string local_test_result(std::string_view model, bool vision, std::string_view message) {
    return make_message("local.test_result", {{"model", model}, {"vision", vision}, {"message", message}});
}

std::string capture_preview(std::string_view media_type, std::string_view base64, int width, int height) {
    return make_message("capture.preview",
                        {{"media_type", media_type}, {"base64", base64}, {"width", width}, {"height", height}});
}

}  // namespace buddy::protocol
