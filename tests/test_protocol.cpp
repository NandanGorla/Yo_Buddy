#include <catch2/catch_test_macros.hpp>

#include <string>

#include "core/protocol.h"

using namespace buddy::protocol;
using json = nlohmann::json;

namespace {

std::string envelope(const std::string& type, const json& payload, int version = kVersion) {
    return json{{"v", version}, {"type", type}, {"payload", payload}}.dump();
}

template <typename T>
T expect(const std::string& text) {
    const ParseResult result = parse_ui_message(text);
    INFO(result.error_code << ": " << result.error_detail);
    REQUIRE(result.ok());
    REQUIRE(std::holds_alternative<T>(*result.message));
    return std::get<T>(*result.message);
}

std::string error_of(const std::string& text) {
    const ParseResult result = parse_ui_message(text);
    REQUIRE_FALSE(result.ok());
    CHECK_FALSE(result.error_detail.empty());
    return result.error_code;
}

}  // namespace

TEST_CASE("protocol: every UI message type parses", "[protocol]") {
    expect<AppReady>(envelope("app.ready", json::object()));
    expect<ChatClear>(envelope("chat.clear", json::object()));

    const auto send = expect<ChatSend>(envelope("chat.send", {{"request_id", "r_1"}, {"text", "hello"}}));
    CHECK(send.request_id == "r_1");
    CHECK(send.text == "hello");

    CHECK(expect<ChatCancel>(envelope("chat.cancel", {{"request_id", "r_1"}})).request_id == "r_1");
    CHECK(expect<SetTheme>(envelope("settings.set_theme", {{"theme", "dark"}})).theme == "dark");

    const auto provider = expect<SetProvider>(envelope(
        "settings.set_provider",
        {{"provider", "anthropic"}, {"base_url", "https://api.anthropic.com/v1"}, {"model", "claude-opus-5-5"}}));
    CHECK(provider.provider == "anthropic");
    CHECK(provider.base_url == "https://api.anthropic.com/v1");
    CHECK(provider.model == "claude-opus-5-5");

    const auto key = expect<SetApiKey>(envelope("settings.set_api_key", {{"provider", "openai"}, {"key", "sk-abc_123"}}));
    CHECK(key.key == "sk-abc_123");
    CHECK(expect<ClearApiKey>(envelope("settings.clear_api_key", {{"provider", "openai"}})).provider == "openai");

    const auto log = expect<UiLog>(envelope("ui.log", {{"level", "warn"}, {"message", "something"}}));
    CHECK(log.level == "warn");
}

TEST_CASE("protocol: an empty model is allowed (not chosen yet)", "[protocol]") {
    const auto provider = expect<SetProvider>(
        envelope("settings.set_provider", {{"provider", "openai"}, {"base_url", "https://api.openai.com/v1"}, {"model", ""}}));
    CHECK(provider.model.empty());
}

TEST_CASE("protocol: chat text keeps Unicode and markup exactly", "[protocol]") {
    const std::string text = "na\xC3\xAFve <b>bold</b> \xF0\x9F\x98\x80\nline two";
    CHECK(expect<ChatSend>(envelope("chat.send", {{"request_id", "r1"}, {"text", text}})).text == text);
}

TEST_CASE("protocol: malformed envelopes are rejected", "[protocol]") {
    CHECK(error_of("") == "invalid_json");
    CHECK(error_of("not json") == "invalid_json");
    CHECK(error_of("{\"v\":1,\"type\":\"app.ready\",\"payload\":{}") == "invalid_json");   // truncated
    CHECK(error_of("[]") == "invalid_envelope");
    CHECK(error_of("42") == "invalid_envelope");
    CHECK(error_of(R"({"type":"app.ready","payload":{}})") == "invalid_envelope");            // no version
    CHECK(error_of(R"({"v":1,"type":"app.ready"})") == "invalid_envelope");                   // no payload
    CHECK(error_of(R"({"v":1,"type":"app.ready","payload":{},"extra":1})") == "invalid_envelope");
    CHECK(error_of(R"({"v":"1","type":"app.ready","payload":{}})") == "invalid_envelope");    // version as string
    CHECK(error_of(R"({"v":1.5,"type":"app.ready","payload":{}})") == "invalid_envelope");
    CHECK(error_of(R"({"v":1,"type":7,"payload":{}})") == "invalid_envelope");
    CHECK(error_of(R"({"v":1,"type":"app.ready","payload":[]})") == "invalid_envelope");
    CHECK(error_of(R"({"v":1,"type":"app.ready","payload":null})") == "invalid_envelope");
}

TEST_CASE("protocol: other versions are refused", "[protocol]") {
    CHECK(error_of(envelope("app.ready", json::object(), 0)) == "unsupported_version");
    CHECK(error_of(envelope("app.ready", json::object(), 2)) == "unsupported_version");
}

TEST_CASE("protocol: unknown types are refused", "[protocol]") {
    CHECK(error_of(envelope("chat.delete_everything", json::object())) == "unknown_type");
    CHECK(error_of(envelope("app.state", json::object())) == "unknown_type");   // a host->UI type sent the wrong way
    CHECK(error_of(envelope("", json::object())) == "invalid_envelope");
}

TEST_CASE("protocol: payloads must match exactly", "[protocol]") {
    // missing, extra and wrongly typed fields
    CHECK(error_of(envelope("chat.send", {{"request_id", "r1"}})) == "invalid_payload");
    CHECK(error_of(envelope("chat.send", {{"request_id", "r1"}, {"text", "x"}, {"model", "y"}})) == "invalid_payload");
    CHECK(error_of(envelope("chat.send", {{"request_id", 5}, {"text", "x"}})) == "invalid_payload");
    CHECK(error_of(envelope("chat.send", {{"request_id", "r1"}, {"text", 5}})) == "invalid_payload");
    CHECK(error_of(envelope("chat.send", {{"request_id", "r1"}, {"text", ""}})) == "invalid_payload");
    CHECK(error_of(envelope("app.ready", {{"surprise", true}})) == "invalid_payload");
    CHECK(error_of(envelope("ui.log", {{"level", "fatal"}, {"message", "x"}})) == "invalid_payload");
}

TEST_CASE("protocol: ids are restricted to a safe alphabet", "[protocol]") {
    for (const char* bad : {"", "UPPER", "has space", "semi;colon", "../up", "a\nb", "<x>"}) {
        INFO("id: " << bad);
        CHECK(error_of(envelope("chat.cancel", {{"request_id", bad}})) == "invalid_payload");
        CHECK(error_of(envelope("settings.set_theme", {{"theme", bad}})) == "invalid_payload");
    }
    CHECK(error_of(envelope("chat.cancel", {{"request_id", std::string(65, 'a')}})) == "invalid_payload");
    expect<ChatCancel>(envelope("chat.cancel", {{"request_id", std::string(64, 'a')}}));
}

TEST_CASE("protocol: size limits are enforced", "[protocol]") {
    const std::string at_limit(kMaxChatTextBytes, 'a');
    expect<ChatSend>(envelope("chat.send", {{"request_id", "r1"}, {"text", at_limit}}));
    CHECK(error_of(envelope("chat.send", {{"request_id", "r1"}, {"text", at_limit + "a"}})) == "invalid_payload");

    CHECK(error_of(std::string(kMaxMessageBytes + 1, ' ')) == "too_large");
    CHECK(error_of(envelope("ui.log", {{"level", "info"}, {"message", std::string(kMaxLogLength + 1, 'x')}})) ==
          "invalid_payload");
}

TEST_CASE("protocol: an API key cannot carry spaces, control bytes or line breaks", "[protocol]") {
    for (const std::string bad : {std::string(""), std::string("has space"), std::string("line\r\nbreak"),
                                  std::string("tab\there"), std::string("caf\xC3\xA9"), std::string(513, 'k')}) {
        CHECK(error_of(envelope("settings.set_api_key", {{"provider", "openai"}, {"key", bad}})) == "invalid_payload");
    }
    expect<SetApiKey>(envelope("settings.set_api_key", {{"provider", "openai"}, {"key", std::string(512, 'k')}}));
}

TEST_CASE("protocol: model names are restricted", "[protocol]") {
    CHECK(is_valid_model("claude-opus-5-5"));
    CHECK(is_valid_model("gemma4:12b"));
    CHECK(is_valid_model("org/model@v1.2"));
    CHECK(is_valid_model(""));
    CHECK_FALSE(is_valid_model("has space"));
    CHECK_FALSE(is_valid_model("quote\""));
    CHECK_FALSE(is_valid_model(std::string(129, 'm')));
    CHECK(error_of(envelope("settings.set_provider",
                            {{"provider", "openai"}, {"base_url", "https://x.example"}, {"model", "bad model"}})) ==
          "invalid_payload");
}

TEST_CASE("protocol: error details never echo payload values", "[protocol]") {
    const ParseResult result =
        parse_ui_message(envelope("settings.set_api_key", {{"provider", "openai"}, {"key", "secret key with space"}}));
    REQUIRE_FALSE(result.ok());
    CHECK(result.error_detail.find("secret") == std::string::npos);
}

TEST_CASE("protocol: host messages are well-formed envelopes", "[protocol]") {
    const json started = json::parse(chat_started("r1", "anthropic", "claude-opus-5-5", "hi <b>"));
    CHECK(started["v"] == kVersion);
    CHECK(started["type"] == "chat.started");
    CHECK(started["payload"] == json{{"request_id", "r1"}, {"provider", "anthropic"}, {"model", "claude-opus-5-5"}, {"text", "hi <b>"}});

    const json delta = json::parse(chat_delta("r1", "<script>\"quoted\"</script>\n"));
    CHECK(delta["payload"]["text"] == "<script>\"quoted\"</script>\n");

    const json done = json::parse(chat_done("r1", "stop", 10, std::nullopt));
    CHECK(done["payload"]["input_tokens"] == 10);
    CHECK(done["payload"]["output_tokens"].is_null());

    CHECK(json::parse(chat_error("r1", "auth_failed", "no"))["payload"]["code"] == "auth_failed");
    CHECK(json::parse(chat_cleared())["payload"].empty());
    CHECK(json::parse(host_error("busy", "wait"))["type"] == "host.error");
}

TEST_CASE("protocol: invalid UTF-8 in outgoing text is replaced, not thrown on", "[protocol]") {
    const std::string broken = std::string("ok \xFF\xFE end");
    std::string message;
    REQUIRE_NOTHROW(message = chat_delta("r1", broken));
    const json parsed = json::parse(message);   // must still be valid JSON
    CHECK(parsed["payload"]["text"].get<std::string>().find("ok ") == 0);
}

TEST_CASE("protocol: companion shell messages parse", "[protocol]") {
    CHECK(expect<SetHotkey>(envelope("settings.set_hotkey", {{"hotkey", "Ctrl+Alt+B"}})).hotkey == "Ctrl+Alt+B");

    const auto panel = expect<SetPanel>(envelope("settings.set_panel", {{"hide_on_click_away", false}, {"pinned", true}}));
    CHECK_FALSE(panel.hide_on_click_away);
    CHECK(panel.pinned);
    const auto capture_policy =
        expect<SetCapturePolicy>(envelope("settings.set_capture",
                                           {{"exclusions", {"private", "Vault"}}, {"send_preview", false}}));
    CHECK(capture_policy.exclusions == std::vector<std::string>{"private", "Vault"});
    CHECK_FALSE(capture_policy.send_preview);

    expect<WindowHide>(envelope("window.hide", json::object()));
    CHECK(expect<OpenMain>(envelope("window.open_main", {{"settings", true}})).settings);
    CHECK_FALSE(expect<OpenMain>(envelope("window.open_main", {{"settings", false}})).settings);
    const auto capture = expect<CaptureRequest>(envelope("capture.request", {{"target", "monitor"}, {"preview", true}}));
    CHECK(capture.target == "monitor");
    CHECK(capture.preview);
    CHECK(expect<OverlayDebug>(envelope("overlay.debug", {{"action", "caption"}, {"caption", "Hello"}})).caption ==
          "Hello");
}

TEST_CASE("protocol: companion shell messages are validated", "[protocol]") {
    CHECK(error_of(envelope("settings.set_hotkey", {{"hotkey", ""}})) == "invalid_payload");
    CHECK(error_of(envelope("settings.set_hotkey", {{"hotkey", std::string(65, 'a')}})) == "invalid_payload");
    CHECK(error_of(envelope("settings.set_hotkey", {{"hotkey", 5}})) == "invalid_payload");
    CHECK(error_of(envelope("settings.set_hotkey", json::object())) == "invalid_payload");

    CHECK(error_of(envelope("settings.set_panel", {{"hide_on_click_away", "yes"}, {"pinned", true}})) == "invalid_payload");
    CHECK(error_of(envelope("settings.set_panel", {{"hide_on_click_away", true}, {"pinned", 1}})) == "invalid_payload");
    CHECK(error_of(envelope("settings.set_panel", {{"pinned", true}})) == "invalid_payload");
    CHECK(error_of(envelope("settings.set_capture", {{"exclusions", {"ok"}}, {"send_preview", "yes"}})) ==
          "invalid_payload");
    CHECK(error_of(envelope("settings.set_capture", {{"exclusions", {std::string(129, 'x')}},
                                                       {"send_preview", true}})) == "invalid_payload");

    CHECK(error_of(envelope("window.hide", {{"which", "all"}})) == "invalid_payload");
    CHECK(error_of(envelope("window.open_main", json::object())) == "invalid_payload");
    CHECK(error_of(envelope("window.open_main", {{"settings", "true"}})) == "invalid_payload");
    CHECK(error_of(envelope("capture.request", {{"target", "desktop"}, {"preview", true}})) == "invalid_payload");
    CHECK(error_of(envelope("capture.request", {{"target", "monitor"}, {"preview", 1}})) == "invalid_payload");
    CHECK(error_of(envelope("capture.request", {{"target", "monitor"}})) == "invalid_payload");
    CHECK(error_of(envelope("overlay.debug", {{"action", "unknown"}, {"caption", ""}})) == "invalid_payload");
}

TEST_CASE("protocol: history and UI command messages", "[protocol]") {
    const json idle = json::parse(chat_history(json::array({{{"role", "user"}, {"text", "q"}}, {{"role", "assistant"}, {"text", "a"}}}), "", "", ""));
    CHECK(idle["type"] == "chat.history");
    CHECK(idle["payload"]["messages"].size() == 2);
    CHECK(idle["payload"]["active_request"].is_null());
    CHECK(idle["payload"]["partial"] == "");

    const json streaming = json::parse(chat_history(json::array(), "r9", "some-model", "half an ans"));
    CHECK(streaming["payload"]["active_request"] == "r9");
    CHECK(streaming["payload"]["active_model"] == "some-model");
    CHECK(streaming["payload"]["partial"] == "half an ans");

    CHECK(json::parse(focus_input())["type"] == "ui.focus_input");
    CHECK(json::parse(show_settings())["type"] == "ui.show_settings");
}

TEST_CASE("protocol: voice messages parse", "[protocol][voice]") {
    expect<VoiceCancel>(envelope("voice.cancel", json::object()));

    const auto voice = expect<SetVoice>(
        envelope("settings.set_voice", {{"base_url", "http://127.0.0.1:8000/v1"}, {"model", "whisper-1"}}));
    CHECK(voice.base_url == "http://127.0.0.1:8000/v1");
    CHECK(voice.model == "whisper-1");

    const auto speech = expect<SetSpeech>(envelope("settings.set_speech", {{"voice", "Microsoft Zira Desktop"},
                                                                            {"base_url", "http://localhost:8880/v1"},
                                                                            {"model", "kokoro"},
                                                                            {"rate", -3},
                                                                            {"pitch", 10}}));
    CHECK(speech.voice == "Microsoft Zira Desktop");
    CHECK(speech.base_url == "http://localhost:8880/v1");
    CHECK(speech.model == "kokoro");
    CHECK(speech.rate == -3);
    CHECK(speech.pitch == 10);

    // The neural server is optional: an empty URL and model mean Windows voices only.
    const auto windows_only = expect<SetSpeech>(envelope(
        "settings.set_speech", {{"voice", ""}, {"base_url", ""}, {"model", ""}, {"rate", 0}, {"pitch", 0}}));
    CHECK(windows_only.base_url.empty());
    CHECK(windows_only.model.empty());
}

TEST_CASE("protocol: voice messages are validated", "[protocol][voice]") {
    CHECK(error_of(envelope("voice.cancel", {{"extra", 1}})) == "invalid_payload");

    // Transcription needs both a server and a model.
    CHECK(error_of(envelope("settings.set_voice", {{"base_url", ""}, {"model", "whisper-1"}})) == "invalid_payload");
    CHECK(error_of(envelope("settings.set_voice", {{"base_url", "http://127.0.0.1:8000/v1"}, {"model", ""}})) ==
          "invalid_payload");
    CHECK(error_of(envelope("settings.set_voice", {{"base_url", "http://127.0.0.1:8000/v1"}, {"model", "bad model"}})) ==
          "invalid_payload");
    CHECK(error_of(envelope("settings.set_voice", {{"base_url", "http://127.0.0.1:8000/v1"}})) == "invalid_payload");

    const auto speech = [](const json& changes) {
        json payload = {{"voice", ""}, {"base_url", ""}, {"model", ""}, {"rate", 0}, {"pitch", 0}};
        payload.update(changes);
        return envelope("settings.set_speech", payload);
    };
    expect<SetSpeech>(speech(json::object()));
    CHECK(error_of(speech({{"rate", 11}})) == "invalid_payload");
    CHECK(error_of(speech({{"rate", -11}})) == "invalid_payload");
    CHECK(error_of(speech({{"pitch", 11}})) == "invalid_payload");
    CHECK(error_of(speech({{"rate", 1.5}})) == "invalid_payload");
    CHECK(error_of(speech({{"pitch", "high"}})) == "invalid_payload");
    CHECK(error_of(speech({{"model", "bad model"}})) == "invalid_payload");
    CHECK(error_of(speech({{"voice", std::string(kMaxSpeechVoiceLength + 1, 'v')}})) == "invalid_payload");
    CHECK(error_of(speech({{"base_url", std::string(kMaxBaseUrlLength + 1, 'u')}})) == "invalid_payload");
}

TEST_CASE("protocol: voice.state is a well-formed envelope", "[protocol][voice]") {
    const json listening = json::parse(voice_state("listening", ""));
    CHECK(listening["type"] == "voice.state");
    CHECK(listening["payload"] == json{{"state", "listening"}, {"error", ""}});

    const json toggle = json::parse(voice_state("listening_toggle", ""));
    CHECK(toggle["payload"]["state"] == "listening_toggle");

    const json failed = json::parse(voice_state("error", "No microphone was found."));
    CHECK(failed["payload"]["state"] == "error");
    CHECK(failed["payload"]["error"] == "No microphone was found.");
}
