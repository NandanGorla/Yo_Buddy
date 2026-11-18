#include "app/app_controller.h"

#include <mutex>
#include <utility>

#include <windows.h>

#include "app/tutor_prompt.h"
#include "core/caption.h"
#include "core/log.h"
#include "core/placement.h"
#include "core/text.h"
#include "core/url.h"
#include "providers/anthropic.h"
#include "providers/openai_compat.h"
#include "platform/capture_target.h"
#include "platform/screen_capture.h"

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
                              {"capture",
                               {{"exclusions", settings_.capture_exclusions},
                                {"send_preview", settings_.capture_send_preview}}},
                              {"muted", settings_.muted},
                              {"voice",
                               {{"base_url", settings_.voice_base_url},
                                {"model", settings_.voice_model}}},
                              {"speech",
                               {{"voice", settings_.speech_voice},
                                {"base_url", settings_.speech_base_url},
                                {"model", settings_.speech_model},
                                {"rate", settings_.speech_rate},
                                {"pitch", settings_.speech_pitch}}},
                              {"overlay",
                               {{"ring", settings_.overlay_ring_color},
                                {"dot", settings_.overlay_dot_color},
                                {"caption", settings_.overlay_caption_color}}}}));
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

void AppController::on_message(const protocol::VoiceCancel&) {
    if (deps_.cancel_voice) deps_.cancel_voice();
}

void AppController::on_message(const protocol::CaptureRequest& request) {
    const settings::ProviderConfig* provider = settings_.find(settings_.provider);
    if (!provider) {
        send(protocol::host_error("capture_failed", "The active provider is not configured."));
        return;
    }
    const url::BaseUrlResult endpoint = url::parse_base_url(provider->base_url);
    if (!endpoint.ok()) {
        send(protocol::host_error("capture_failed", endpoint.error));
        return;
    }
    if (capture::requires_preview(url::is_loopback_host(endpoint.url->host),
                                  settings_.capture_send_preview) &&
        !request.preview) {
        send(protocol::host_error("capture_preview_required",
                                  "Preview is required before sending a capture to a remote endpoint."));
        return;
    }

    const capture::TargetResult target =
        capture::resolve_foreground_target(request.target == "monitor");
    if (!target.ok()) {
        send(protocol::host_error("capture_failed", target.error));
        return;
    }
    const HWND identity_window = target.window ? target.window : GetForegroundWindow();
    const capture::WindowIdentityResult identity = capture::identify_window(identity_window);
    if (!identity.ok()) {
        send(protocol::host_error("capture_failed", identity.error));
        return;
    }
    if (capture::is_excluded(*identity.identity, settings_.capture_exclusions)) {
        send(protocol::host_error("capture_excluded", "The selected window is excluded from capture."));
        return;
    }

    const capture::FrameResult frame = capture::capture_one(target);
    if (!frame.ok()) {
        send(protocol::host_error("capture_failed", frame.error));
        return;
    }
    const capture::EncodedFrameResult encoded = capture::encode_png(frame.frame);
    if (!encoded.ok()) {
        send(protocol::host_error("capture_failed", encoded.error));
        return;
    }
    pending_capture_ = ImageInput{"image/png", encoded.base64};
    send(protocol::capture_preview("image/png", encoded.base64, frame.frame.width, frame.frame.height));
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

std::optional<hotkey::Binding> AppController::hotkey_binding() const {
    return hotkey_ ? hotkey_->active_binding() : std::nullopt;
}

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

void AppController::submit_voice_text(std::string_view spoken) {
    const std::string text = text::truncate_utf8(text::trim_ascii(spoken), protocol::kMaxChatTextBytes);
    if (text.empty()) return;
    const std::string request_id = "voice_" + std::to_string(next_voice_request_++);
    if (const auto refusal = begin_chat(request_id, text)) {
        // No window sent this request, so none of them would show a chat.error for it. Every
        // window shows the voice state, so the reason goes there.
        send(protocol::voice_state("error", refusal->message + " Your spoken question was not sent."));
    }
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

void AppController::on_message(const protocol::SetCapturePolicy& m) {
    settings_.capture_exclusions = m.exclusions;
    settings_.capture_send_preview = m.send_preview;
    save_settings();
    send_state();
}

void AppController::on_message(const protocol::OverlayDebug& m) {
    log::info("overlay.debug received action=" + m.action);
    if (m.action == "caption" && !settings_.muted && deps_.speak && !m.caption.empty()) {
        deps_.speak(caption::for_speech(m.caption));
    }
    if (!deps_.overlay_debug) {
        log::warn("overlay.debug ignored: overlay callback is not installed");
        return;
    }
    deps_.overlay_debug(m);
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

void AppController::on_message(const protocol::SetOverlayPalette& m) {
    settings_.overlay_ring_color = m.ring;
    settings_.overlay_dot_color = m.dot;
    settings_.overlay_caption_color = m.caption;
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

void AppController::on_message(const protocol::SetVoice& m) {
    const url::BaseUrlResult parsed = url::parse_base_url(m.base_url);
    if (!parsed.ok()) {
        send(protocol::host_error("invalid_voice_endpoint", parsed.error));
        return;
    }
    // Recorded speech never leaves this PC.
    if (!url::is_loopback_host(parsed.url->host)) {
        send(protocol::host_error("invalid_voice_endpoint",
                                  "The transcription server must be on this PC (a localhost URL)."));
        return;
    }
    settings_.voice_base_url = parsed.url->to_string();
    settings_.voice_model = m.model;
    save_settings();
    send_state();
}

void AppController::on_message(const protocol::SetSpeech& m) {
    // The neural server is optional. An empty URL means Windows voices only.
    std::string base_url;
    if (!m.base_url.empty()) {
        const url::BaseUrlResult parsed = url::parse_base_url(m.base_url);
        if (!parsed.ok()) {
            send(protocol::host_error("invalid_speech_endpoint", parsed.error));
            return;
        }
        if (!url::is_loopback_host(parsed.url->host)) {
            send(protocol::host_error("invalid_speech_endpoint",
                                      "The neural speech server must be on this PC (a localhost URL)."));
            return;
        }
        if (m.model.empty()) {
            send(protocol::host_error("invalid_speech_endpoint",
                                      "Enter the neural speech model, or clear the URL to use Windows voices only."));
            return;
        }
        base_url = parsed.url->to_string();
    }
    settings_.speech_voice = std::string(text::trim_ascii(m.voice));
    settings_.speech_base_url = std::move(base_url);
    settings_.speech_model = m.model;
    settings_.speech_rate = m.rate;
    settings_.speech_pitch = m.pitch;
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
    if (const auto refusal = begin_chat(m.request_id, m.text)) {
        send(protocol::chat_error(m.request_id, refusal->code, refusal->message));
    }
}

// Starts a reply to `text`. Returns why not when the request cannot be started; nothing has been
// sent to the UI in that case, so the caller decides where the reason is shown.
std::optional<AppController::ChatRefusal> AppController::begin_chat(const std::string& request_id,
                                                                    const std::string& text) {
    if (busy_) return ChatRefusal{"busy", "A reply is already in progress."};
    const settings::ProviderConfig provider = settings_.active();
    if (provider.model.empty()) {
        return ChatRefusal{"not_configured", "No model is selected for " + provider.label + ". Choose one in Settings."};
    }
    const url::BaseUrlResult endpoint = url::parse_base_url(provider.base_url);
    if (!endpoint.ok()) return ChatRefusal{"not_configured", endpoint.error};
    std::string api_key;
    if (deps_.credentials) {
        if (auto stored = deps_.credentials->get(key_name(provider.id))) api_key = std::move(*stored);
    }
    // A server on this PC may need no key. Anything else does.
    if (api_key.empty() && !url::is_loopback_host(endpoint.url->host)) {
        return ChatRefusal{"not_configured", "Add an API key for " + provider.label + " in Settings."};
    }
    if (!deps_.transport || !deps_.run_on_ui) {
        return ChatRefusal{"bad_response", "The app is not fully initialised."};
    }

    conversation_.push_back({Role::User, text});
    busy_ = true;
    active_request_ = request_id;
    reply_so_far_.clear();
    active_model_ = provider.model;
    send(protocol::chat_started(request_id, provider.id, provider.model, text));

    // Everything the worker needs is copied in, so it shares no mutable state with this thread.
    std::vector<ChatMessage> messages;
    messages.reserve(conversation_.size() + 1);
    messages.push_back({Role::System, kTutorSystemPrompt});
    messages.insert(messages.end(), conversation_.begin(), conversation_.end());

    ChatOptions options;
    options.model = provider.model;
    options.max_output_tokens = provider.max_output_tokens;
    std::vector<ImageInput> images;
    if (pending_capture_) {
        images.push_back(std::move(*pending_capture_));
        pending_capture_.reset();
    }

    if (worker_.joinable()) worker_.join();   // the previous worker has finished; reclaim it
    worker_ = std::jthread(
        [this, alive = alive_, transport = deps_.transport, run_on_ui = deps_.run_on_ui,
         provider_id = provider.id, base_url = endpoint.url->to_string(), api_key = std::move(api_key), messages = std::move(messages),
         images = std::move(images),
         options, request_id](std::stop_token stop) {
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
            ChatResult result = provider_client->chat(messages, images, {}, options, sink, stop);

            run_on_ui([this, alive, buffer, request_id, result = std::move(result)] {
                if (!alive->load()) return;
                flush_deltas(request_id, *buffer);
                finish_chat(request_id, result);
            });
        });
    return std::nullopt;
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
    const std::string spoken_reply = reply_so_far_;
    if (!reply_so_far_.empty()) {
        conversation_.push_back({Role::Assistant, reply_so_far_});
    } else if (!conversation_.empty() && conversation_.back().role == Role::User) {
        conversation_.pop_back();
    }
    reply_so_far_.clear();

    if (result.status == ChatStatus::Ok || result.status == ChatStatus::Cancelled) {
        if (result.status == ChatStatus::Ok && !spoken_reply.empty()) {
            if (deps_.show_caption) {
                const std::string overlay = caption::for_overlay(spoken_reply);
                if (!overlay.empty()) deps_.show_caption(overlay);
            }
            if (!settings_.muted && deps_.speak) {
                const std::string spoken = caption::for_speech(spoken_reply);
                if (!spoken.empty()) deps_.speak(spoken);
            }
        }
        send(protocol::chat_done(request_id, result.stop_reason, result.input_tokens, result.output_tokens));
    } else {
        log::warn(std::string("chat failed: ") + to_code(result.status) + ", HTTP " +
                  std::to_string(result.http_status));
        send(protocol::chat_error(request_id, to_code(result.status), result.message));
    }
}

}  // namespace buddy
