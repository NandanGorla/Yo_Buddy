#include "app/app_controller.h"

#include <mutex>
#include <utility>

#include "app/tutor_prompt.h"
#include "core/log.h"
#include "core/placement.h"
#include "core/url.h"
#include "providers/anthropic.h"
#include "providers/openai_compat.h"

namespace buddy {

using json = nlohmann::json;

// Reply text produced by the worker and not yet forwarded to the UI. Tokens arrive far faster
// than the UI needs them, so they are collected here and sent in one message per UI-thread turn.
struct AppController::DeltaBuffer {
    std::mutex mutex;
    std::string pending;
    bool flush_scheduled = false;
};

AppController::AppController(Deps deps) : deps_(std::move(deps)) {
    std::error_code exists_error;
    first_run_ = !std::filesystem::exists(deps_.settings_path, exists_error);
    if (deps_.hotkeys) hotkey_ = std::make_unique<hotkey::Manager>(*deps_.hotkeys);

    std::vector<std::string> warnings;
    settings_ = settings::load(deps_.settings_path, &warnings);
    for (const std::string& w : warnings) log::warn("settings: " + w);

    themes_ = theme::load_themes(deps_.theme_folders);
    for (const theme::LoadIssue& issue : themes_.issues) {
        log::warn("theme file skipped: " + issue.file + " (" + issue.problem + ")");
    }
    // Keep the stored choice valid if the theme it names has gone.
    settings_.theme = theme::pick_theme(themes_, settings_.theme).id;
}

AppController::~AppController() { shutdown(); }

void AppController::shutdown() {
    release_hotkey();
    alive_->store(false);
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    busy_ = false;
}

const theme::Theme& AppController::active_theme() const { return theme::pick_theme(themes_, settings_.theme); }

std::string AppController::key_name(std::string_view provider_id) {
    return "apikey/" + std::string(provider_id);
}

void AppController::send(const std::string& json_text) const {
    if (deps_.send_to_ui) deps_.send_to_ui(json_text);
}

void AppController::send_state() const {
    json themes = json::array();
    for (const theme::Theme& t : themes_.themes) themes.push_back(theme::to_json(t));

    json providers = json::array();
    for (const settings::ProviderConfig& p : settings_.providers) {
        const bool has_key = deps_.credentials && deps_.credentials->get(key_name(p.id)).has_value();
        providers.push_back({{"id", p.id},
                             {"label", p.label},
                             {"base_url", p.base_url},
                             {"model", p.model},
                             {"default_base_url", p.default_base_url},
                             {"default_model", p.default_model},
                             {"has_key", has_key}});   // only the fact, never the key itself
    }

    send(protocol::app_state({{"app_version", deps_.app_version},
                              {"themes", std::move(themes)},
                              {"theme", settings_.theme},
                              {"provider", settings_.provider},
                              {"providers", std::move(providers)},
                              {"busy", busy_},
                              {"hotkey",
                               {{"requested", hotkey_ ? hotkey_->requested() : settings_.hotkey},
                                {"active", hotkey_active()},
                                {"status", hotkey::to_code(hotkey_status())},
                                {"message", hotkey_message()}}},
                              {"panel",
                               {{"hide_on_click_away", settings_.hide_on_click_away},
                                {"pinned", settings_.panel_pinned}}},
                              {"muted", settings_.muted}}));
}

void AppController::save_settings() const {
    if (!settings::save(deps_.settings_path, settings_)) log::error("could not save settings");
}

void AppController::handle_ui_message(std::string_view json_text, ViewId source) {
    current_source_ = source;
    protocol::ParseResult parsed = protocol::parse_ui_message(json_text);
    if (!parsed.ok()) {
        log::warn("rejected UI message: " + parsed.error_code + " (" + parsed.error_detail + ")");
        send(protocol::host_error(parsed.error_code, parsed.error_detail));
        return;
    }
    std::visit([this](const auto& message) { on_message(message); }, *parsed.message);
}

void AppController::on_message(const protocol::AppReady&) {
    send_state();
    send_history();   // a window that has just loaded needs the conversation so far
}

void AppController::send_history() const {
    json messages = json::array();
    for (const ChatMessage& m : conversation_) {
        messages.push_back({{"role", m.role == Role::Assistant ? "assistant" : "user"}, {"text", m.text}});
    }
    // While a reply is streaming, the newest user message is the one being answered and
    // `partial` is the reply text received so far.
    send(protocol::chat_history(std::move(messages), busy_ ? active_request_ : std::string(), active_model_,
                                busy_ ? reply_so_far_ : std::string()));
}

hotkey::Status AppController::apply_stored_hotkey() {
    if (!hotkey_) return hotkey::Status::None;
    const hotkey::Status status = hotkey_->apply(settings_.hotkey);
    if (status != hotkey::Status::Active) log::warn("hotkey not registered: " + hotkey_->message());
    return status;
}

void AppController::release_hotkey() {
    if (hotkey_) hotkey_->release();
}

int AppController::hotkey_id() const { return hotkey_ ? hotkey_->active_id() : 0; }

hotkey::Status AppController::hotkey_status() const { return hotkey_ ? hotkey_->status() : hotkey::Status::None; }

std::string AppController::hotkey_message() const { return hotkey_ ? hotkey_->message() : std::string(); }

std::string AppController::hotkey_active() const { return hotkey_ ? hotkey_->active() : std::string(); }

void AppController::set_panel_size(int width_dip, int height_dip) {
    const placement::Size clamped = placement::clamp_panel_size({width_dip, height_dip});
    if (clamped.width == settings_.panel_width && clamped.height == settings_.panel_height) return;
    settings_.panel_width = clamped.width;
    settings_.panel_height = clamped.height;
    save_settings();
}

void AppController::set_panel_pinned(bool pinned) {
    if (settings_.panel_pinned == pinned) return;
    settings_.panel_pinned = pinned;
    save_settings();
    send_state();
}

void AppController::set_muted(bool muted) {
    if (settings_.muted == muted) return;
    settings_.muted = muted;
    save_settings();
    send_state();
}

void AppController::on_message(const protocol::SetHotkey& m) {
    if (!hotkey_) {
        send(protocol::host_error("hotkey_unavailable", "Global hotkeys are not available."));
        return;
    }
    const hotkey::Status status = hotkey_->apply(m.hotkey);
    if (status == hotkey::Status::Active) {
        settings_.hotkey = hotkey_->active();
        save_settings();
        log::info("hotkey set to " + settings_.hotkey);
    } else {
        // The previous hotkey, if there was one, is still registered and still saved.
        send(protocol::host_error(std::string("hotkey_") + hotkey::to_code(status), hotkey_->message()));
    }
    send_state();
}

void AppController::on_message(const protocol::SetPanel& m) {
    settings_.hide_on_click_away = m.hide_on_click_away;
    settings_.panel_pinned = m.pinned;
    save_settings();
    send_state();
}

void AppController::on_message(const protocol::WindowHide&) {
    if (deps_.hide_window) deps_.hide_window(current_source_);
}

void AppController::on_message(const protocol::OpenMain& m) {
    if (deps_.open_main) deps_.open_main(m.settings);
}

void AppController::on_message(const protocol::LocalTestModel& m) {
    if (busy_) {
        send(protocol::host_error("busy", "Stop the current reply before testing a local model."));
        return;
    }
    if (!deps_.transport || !deps_.run_on_ui) {
        send(protocol::host_error("local_test_failed", "The app is not fully initialised."));
        return;
    }
    if (worker_.joinable()) worker_.join();
    worker_ = std::jthread([this, alive = alive_, transport = deps_.transport, run_on_ui = deps_.run_on_ui,
                            endpoint = m.endpoint, model = m.model](std::stop_token stop) {
        LocalEndpointConnector connector(*transport);
        ChatResult result = connector.test_model(endpoint, model, stop);
        run_on_ui([this, alive, model, result = std::move(result)] {
            if (!alive->load()) return;
            const bool vision = result.status == ChatStatus::Ok;
            const std::string message = vision
                                            ? "The model accepted an image and replied. Vision input works."
                                            : (result.message.empty() ? "The model did not accept the test image."
                                                                       : result.message);
            send(protocol::local_test_result(model, vision, message));
        });
    });
}

void AppController::on_message(const protocol::UiLog& m) {
    const std::string line = "ui: " + m.message;
    if (m.level == "error") {
        log::error(line);
    } else if (m.level == "warn") {
        log::warn(line);
    } else {
        log::info(line);
    }
}

void AppController::on_message(const protocol::SetTheme& m) {
    const theme::Theme& chosen = theme::pick_theme(themes_, m.theme);
    if (chosen.id != m.theme) {
        send(protocol::host_error("unknown_theme", "That theme is not installed."));
        return;
    }
    settings_.theme = chosen.id;
    save_settings();
    send_state();
}

void AppController::on_message(const protocol::SetProvider& m) {
    settings::ProviderConfig* provider = settings_.find(m.provider);
    if (!provider) {
        send(protocol::host_error("unknown_provider", "That provider is not available."));
        return;
    }
    const url::BaseUrlResult parsed = url::parse_base_url(m.base_url);
    if (!parsed.ok()) {
        send(protocol::host_error("invalid_base_url", parsed.error));
        return;
    }
    provider->base_url = parsed.url->to_string();
    provider->model = m.model;
    settings_.provider = provider->id;
    save_settings();
    send_state();
}

void AppController::on_message(const protocol::SetApiKey& m) {
    if (!settings_.find(m.provider)) {
        send(protocol::host_error("unknown_provider", "That provider is not available."));
        return;
    }
    if (!deps_.credentials || !deps_.credentials->set(key_name(m.provider), m.key)) {
        send(protocol::host_error("key_not_saved", "Windows Credential Manager refused to store the key."));
        return;
    }
    log::info("stored an API key for " + m.provider);
    send_state();
}

void AppController::on_message(const protocol::ClearApiKey& m) {
    if (!settings_.find(m.provider)) {
        send(protocol::host_error("unknown_provider", "That provider is not available."));
        return;
    }
    if (!deps_.credentials || !deps_.credentials->erase(key_name(m.provider))) {
        send(protocol::host_error("key_not_removed", "The key could not be removed from Windows Credential Manager."));
        return;
    }
    log::info("removed the API key for " + m.provider);
    send_state();
}

void AppController::on_message(const protocol::ChatClear&) {
    if (busy_) {
        send(protocol::host_error("busy", "Stop the current reply before clearing the conversation."));
        return;
    }
    conversation_.clear();
    send(protocol::chat_cleared());
}

void AppController::on_message(const protocol::ChatCancel& m) {
    if (busy_ && m.request_id == active_request_) worker_.request_stop();
}

void AppController::on_message(const protocol::ChatSend& m) {
    if (busy_) {
        send(protocol::chat_error(m.request_id, "busy", "A reply is already in progress."));
        return;
    }
    const settings::ProviderConfig provider = settings_.active();
    if (provider.model.empty()) {
        send(protocol::chat_error(m.request_id, "not_configured",
                                  "No model is selected for " + provider.label + ". Choose one in Settings."));
        return;
    }
    const url::BaseUrlResult endpoint = url::parse_base_url(provider.base_url);
    if (!endpoint.ok()) {
        send(protocol::chat_error(m.request_id, "not_configured", endpoint.error));
        return;
    }
    std::string api_key;
    if (deps_.credentials) {
        if (auto stored = deps_.credentials->get(key_name(provider.id))) api_key = std::move(*stored);
    }
    // A server on this PC may need no key. Anything else does.
    if (api_key.empty() && !url::is_loopback_host(endpoint.url->host)) {
        send(protocol::chat_error(m.request_id, "not_configured",
                                  "Add an API key for " + provider.label + " in Settings."));
        return;
    }
    if (!deps_.transport || !deps_.run_on_ui) {
        send(protocol::chat_error(m.request_id, "bad_response", "The app is not fully initialised."));
        return;
    }

    conversation_.push_back({Role::User, m.text});
    busy_ = true;
    active_request_ = m.request_id;
    reply_so_far_.clear();
    active_model_ = provider.model;
    send(protocol::chat_started(m.request_id, provider.id, provider.model, m.text));

    // Everything the worker needs is copied in, so it shares no mutable state with this thread.
    std::vector<ChatMessage> messages;
    messages.reserve(conversation_.size() + 1);
    messages.push_back({Role::System, kTutorSystemPrompt});
    messages.insert(messages.end(), conversation_.begin(), conversation_.end());

    ChatOptions options;
    options.model = provider.model;
    options.max_output_tokens = provider.max_output_tokens;

    if (worker_.joinable()) worker_.join();   // the previous worker has finished; reclaim it
    worker_ = std::jthread(
        [this, alive = alive_, transport = deps_.transport, run_on_ui = deps_.run_on_ui,
         provider_id = provider.id, base_url = endpoint.url->to_string(), api_key = std::move(api_key), messages = std::move(messages),
         options, request_id = m.request_id](std::stop_token stop) {
            auto buffer = std::make_shared<DeltaBuffer>();

            ChatSink sink;
            sink.on_text = [&](std::string_view piece) {
                bool schedule = false;
                {
                    std::lock_guard lock(buffer->mutex);
                    buffer->pending.append(piece);
                    schedule = !buffer->flush_scheduled;
                    buffer->flush_scheduled = true;
                }
                if (schedule) {
                    run_on_ui([this, alive, buffer, request_id] {
                        if (alive->load()) flush_deltas(request_id, *buffer);
                    });
                }
            };

            std::unique_ptr<IChatProvider> provider_client;
            if (provider_id == "anthropic") {
                provider_client = std::make_unique<AnthropicProvider>(
                    *transport, AnthropicConfig{base_url, api_key, "2023-06-01"});
            } else {
                provider_client = std::make_unique<OpenAiCompatProvider>(
                    *transport, OpenAiCompatConfig{base_url, api_key});
            }
            ChatResult result = provider_client->chat(messages, {}, {}, options, sink, stop);

            run_on_ui([this, alive, buffer, request_id, result = std::move(result)] {
                if (!alive->load()) return;
                flush_deltas(request_id, *buffer);
                finish_chat(request_id, result);
            });
        });
}

void AppController::flush_deltas(const std::string& request_id, DeltaBuffer& buffer) {
    std::string text;
    {
        std::lock_guard lock(buffer.mutex);
        text.swap(buffer.pending);
        buffer.flush_scheduled = false;
    }
    if (text.empty() || request_id != active_request_) return;
    reply_so_far_ += text;
    send(protocol::chat_delta(request_id, text));
}

void AppController::finish_chat(const std::string& request_id, const ChatResult& result) {
    if (request_id != active_request_) return;
    busy_ = false;
    active_request_.clear();

    // Keep the turn in the history when there is something to keep. A request that produced no
    // text is dropped again, so the next attempt does not send the same user message twice.
    if (!reply_so_far_.empty()) {
        conversation_.push_back({Role::Assistant, reply_so_far_});
    } else if (!conversation_.empty() && conversation_.back().role == Role::User) {
        conversation_.pop_back();
    }
    reply_so_far_.clear();

    if (result.status == ChatStatus::Ok || result.status == ChatStatus::Cancelled) {
        send(protocol::chat_done(request_id, result.stop_reason, result.input_tokens, result.output_tokens));
    } else {
        log::warn(std::string("chat failed: ") + to_code(result.status) + ", HTTP " +
                  std::to_string(result.http_status));
        send(protocol::chat_error(request_id, to_code(result.status), result.message));
    }
}

}  // namespace buddy
