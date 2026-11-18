// Tests for the controller behind the UI bridge: a UI message goes in as JSON text, and the
// messages the controller sends back to the UI are captured and checked. The model backend is
// the scripted FakeTransport.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/app_controller.h"
#include "fake_transport.h"

using namespace buddy;
using namespace buddy::test;
using json = nlohmann::json;

namespace {

class FakeCredentials final : public ICredentialStore {
public:
    std::map<std::string, std::string> entries;

    bool set(std::string_view name, std::string_view secret) override {
        entries[std::string(name)] = std::string(secret);
        return true;
    }
    std::optional<std::string> get(std::string_view name) override {
        const auto it = entries.find(std::string(name));
        if (it == entries.end()) return std::nullopt;
        return it->second;
    }
    bool erase(std::string_view name) override {
        entries.erase(std::string(name));
        return true;
    }
};

// Stands in for the window: it collects what the controller sends to the UI, and plays the role
// of the UI thread's task queue (the test thread runs the queued tasks in pump()).
struct Harness {
    FakeTransport transport;
    FakeCredentials credentials;
    std::vector<json> sent;
    std::filesystem::path dir;
    std::unique_ptr<AppController> controller;

    std::mutex mutex;
    std::deque<std::function<void()>> tasks;

    // Companion shell: a fake hotkey registrar and records of what the controller asked the windows to do.
    struct Registrar final : hotkey::IRegistrar {
        std::vector<hotkey::Binding> taken_elsewhere;
        std::map<int, hotkey::Binding> ours;
        hotkey::RegisterOutcome register_hotkey(int id, const hotkey::Binding& binding) override {
            for (const auto& other : taken_elsewhere) {
                if (other == binding) return hotkey::RegisterOutcome::AlreadyTaken;
            }
            ours[id] = binding;
            return hotkey::RegisterOutcome::Ok;
        }
        void unregister_hotkey(int id) override { ours.erase(id); }
    } registrar;
    std::vector<ViewId> hidden;
    std::vector<bool> opened_main;
    std::vector<std::string> spoken;     // what the controller asked to have read aloud
    std::vector<std::string> captions;   // overlay captions from finished replies
    int voice_cancels = 0;

    explicit Harness(const std::string& name) {
        dir = std::filesystem::temp_directory_path() / ("buddy-test-controller-" + name);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        make_controller();
    }
    ~Harness() {
        controller.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    void make_controller() {
        controller.reset();
        AppController::Deps deps;
        deps.transport = &transport;
        deps.credentials = &credentials;
        deps.settings_path = dir / "settings.json";
        deps.theme_folders = {std::filesystem::path(BUDDY_SOURCE_DIR) / "themes"};
        deps.app_version = "9.9.9";
        deps.send_to_ui = [this](const std::string& text) { sent.push_back(json::parse(text)); };
        deps.run_on_ui = [this](std::function<void()> task) {
            std::lock_guard lock(mutex);
            tasks.push_back(std::move(task));
        };
        deps.hotkeys = &registrar;
        deps.hide_window = [this](ViewId view) { hidden.push_back(view); };
        deps.open_main = [this](bool settings) { opened_main.push_back(settings); };
        deps.speak = [this](std::string text) { spoken.push_back(std::move(text)); };
        deps.show_caption = [this](std::string text) { captions.push_back(std::move(text)); };
        deps.cancel_voice = [this] { ++voice_cancels; };
        controller = std::make_unique<AppController>(std::move(deps));
    }

    void ui(const std::string& type, const json& payload = json::object(), ViewId from = ViewId::Main) {
        controller->handle_ui_message(json{{"v", 1}, {"type", type}, {"payload", payload}}.dump(), from);
    }

    void pump() {
        std::deque<std::function<void()>> batch;
        {
            std::lock_guard lock(mutex);
            batch.swap(tasks);
        }
        for (auto& task : batch) task();
    }

    // Runs queued tasks until `done` is true or five seconds pass.
    bool pump_until(const std::function<bool()>& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            pump();
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    std::vector<json> of_type(const std::string& type) const {
        std::vector<json> out;
        for (const json& m : sent) {
            if (m["type"] == type) out.push_back(m["payload"]);
        }
        return out;
    }
    bool has(const std::string& type) const { return !of_type(type).empty(); }

    bool wait_for_reply() {
        return pump_until([&] { return has("chat.done") || has("chat.error"); });
    }

    std::string all_text_sent() const {
        std::string all;
        for (const json& m : sent) all += m.dump();
        return all;
    }

    json last_request_body() const { return json::parse(transport.last_request.body); }
};

void script_reply(FakeTransport& transport, const std::vector<std::string>& pieces) {
    transport.chunks.clear();
    transport.chunks.push_back(sse(R"({"type":"message_start","message":{"usage":{"input_tokens":20}}})"));
    for (const std::string& piece : pieces) {
        transport.chunks.push_back(sse(
            R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":)" +
            json(piece).dump() + R"(}})"));
    }

    transport.chunks.push_back(
        sse(R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":5}})"));
    transport.chunks.push_back(sse(R"({"type":"message_stop"})"));
}

void script_openai_reply(FakeTransport& transport, const std::vector<std::string>& pieces) {
    transport.chunks.clear();
    for (const std::string& piece : pieces) transport.chunks.push_back(text_chunk_raw(piece));
    transport.chunks.push_back(finish_chunk("stop"));
    transport.chunks.push_back(usage_chunk(20, 5));
    transport.chunks.push_back(done_marker());
}

}  // namespace

TEST_CASE("controller: app.ready is answered with the full state", "[controller]") {
    Harness h("ready");
    h.ui("app.ready");

    const auto states = h.of_type("app.state");
    REQUIRE(states.size() == 1);
    const json& state = states[0];
    CHECK(state["app_version"] == "9.9.9");
    CHECK(state["theme"] == "dark");
    CHECK(state["provider"] == "anthropic");
    CHECK(state["busy"] == false);

    std::vector<std::string> theme_ids;
    for (const json& t : state["themes"]) theme_ids.push_back(t["id"]);
    CHECK(theme_ids == std::vector<std::string>{"dark", "light"});
    CHECK(state["themes"][0]["tokens"].contains("accent"));

    REQUIRE(state["providers"].size() == 3);
    CHECK(state["providers"][0]["id"] == "anthropic");
    CHECK(state["providers"][0]["base_url"] == "https://api.anthropic.com/v1");
    CHECK(state["providers"][0]["has_key"] == false);
}

TEST_CASE("controller: a malformed UI message gets host.error and changes nothing", "[controller]") {
    Harness h("malformed");
    h.controller->handle_ui_message("{ not json");
    h.controller->handle_ui_message(R"({"v":1,"type":"chat.send","payload":{"request_id":"r1"}})");
    h.controller->handle_ui_message(R"({"v":2,"type":"app.ready","payload":{}})");

    const auto errors = h.of_type("host.error");
    REQUIRE(errors.size() == 3);
    CHECK(errors[0]["code"] == "invalid_json");
    CHECK(errors[1]["code"] == "invalid_payload");
    CHECK(errors[2]["code"] == "unsupported_version");
    CHECK(h.transport.calls == 0);
    CHECK_FALSE(h.controller->chat_in_progress());
}

TEST_CASE("controller: chatting without an API key is refused before any request", "[controller]") {
    Harness h("nokey");
    h.ui("chat.send", {{"request_id", "r1"}, {"text", "hello"}});

    const auto errors = h.of_type("chat.error");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0]["request_id"] == "r1");
    CHECK(errors[0]["code"] == "not_configured");
    CHECK(h.transport.calls == 0);
}

TEST_CASE("controller: a chat streams started, deltas and done", "[controller]") {
    Harness h("stream");
    h.credentials.entries["apikey/anthropic"] = "sk-ant-test";
    script_reply(h.transport, {"Hello", " ", "world"});

    h.ui("chat.send", {{"request_id", "r1"}, {"text", "say hello"}});
    CHECK(h.controller->chat_in_progress());
    REQUIRE(h.wait_for_reply());
    CHECK_FALSE(h.controller->chat_in_progress());

    const auto started = h.of_type("chat.started");
    REQUIRE(started.size() == 1);
    CHECK(started[0] == json{{"request_id", "r1"}, {"provider", "anthropic"}, {"model", "claude-opus-5-5"}, {"text", "say hello"}});

    std::string text;
    for (const json& delta : h.of_type("chat.delta")) {
        CHECK(delta["request_id"] == "r1");
        text += delta["text"].get<std::string>();
    }
    CHECK(text == "Hello world");

    const auto done = h.of_type("chat.done");
    REQUIRE(done.size() == 1);
    CHECK(done[0]["stop_reason"] == "end_turn");
    CHECK(done[0]["input_tokens"] == 20);
    CHECK(done[0]["output_tokens"] == 5);
    CHECK_FALSE(h.has("chat.error"));

    // The request went to the configured endpoint with the stored key and the tutor prompt.
    CHECK(h.transport.last_request.url == "https://api.anthropic.com/v1/messages");
    bool has_auth = false;
    for (const auto& [name, value] : h.transport.last_request.headers) {
        if (name == "Authorization") has_auth = value == "Bearer sk-ant-test";
    }
    CHECK_FALSE(has_auth);
    const json body = h.last_request_body();
    CHECK(body["model"] == "claude-opus-5-5");
    CHECK(body["system"].is_string());
    REQUIRE(body["messages"].size() == 1);
    CHECK(body["messages"][0] == json{{"role", "user"}, {"content", "say hello"}});
}

TEST_CASE("controller: the conversation history is sent on the next turn", "[controller]") {
    Harness h("history");
    h.credentials.entries["apikey/anthropic"] = "k";
    script_reply(h.transport, {"first answer"});
    h.ui("chat.send", {{"request_id", "r1"}, {"text", "first question"}});
    REQUIRE(h.wait_for_reply());

    h.sent.clear();
    script_reply(h.transport, {"second answer"});
    h.ui("chat.send", {{"request_id", "r2"}, {"text", "second question"}});
    REQUIRE(h.wait_for_reply());

    const json body = h.last_request_body();
    REQUIRE(body["messages"].size() == 3);
    CHECK(body["messages"][0]["content"] == "first question");
    CHECK(body["messages"][1] == json{{"role", "assistant"}, {"content", "first answer"}});
    CHECK(body["messages"][2]["content"] == "second question");

    SECTION("chat.clear forgets it") {
        h.ui("chat.clear");
        CHECK(h.has("chat.cleared"));
        script_reply(h.transport, {"fresh"});
        h.ui("chat.send", {{"request_id", "r3"}, {"text", "new topic"}});
        REQUIRE(h.pump_until([&] { return h.of_type("chat.done").size() == 2; }));
        CHECK(h.last_request_body()["messages"].size() == 1);
    }
}

TEST_CASE("controller: a failed request reports the error and is not kept in the history", "[controller]") {
    Harness h("failure");
    h.credentials.entries["apikey/anthropic"] = "wrong";
    h.transport.status = 401;
    h.transport.content_type = "application/json";
    h.transport.chunks = {R"({"error":{"message":"invalid x-api-key"}})"};

    h.ui("chat.send", {{"request_id", "r1"}, {"text", "question"}});
    REQUIRE(h.wait_for_reply());
    const auto errors = h.of_type("chat.error");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0]["code"] == "auth_failed");
    CHECK(errors[0]["message"] == "invalid x-api-key");
    CHECK_FALSE(h.controller->chat_in_progress());

    // Trying again must not send the failed question twice.
    h.sent.clear();
    h.transport.status = 200;
    h.transport.content_type = "text/event-stream";
    script_reply(h.transport, {"ok"});
    h.ui("chat.send", {{"request_id", "r2"}, {"text", "question"}});
    REQUIRE(h.wait_for_reply());
    CHECK(h.last_request_body()["messages"].size() == 1);
}

TEST_CASE("controller: cancel stops a running reply", "[controller]") {
    Harness h("cancel");
    h.credentials.entries["apikey/anthropic"] = "k";
    h.transport.chunks = {
        sse(R"({"type":"message_start","message":{"usage":{"input_tokens":1}}})") +
            sse(R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})") +
            sse(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"partial"}})"),
        sse(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"never"}})")};
    h.transport.block_after_first_chunk = true;

    h.ui("chat.send", {{"request_id", "r1"}, {"text", "long question"}});
    REQUIRE(h.pump_until([&] { return h.has("chat.delta"); }));

    SECTION("a second send while busy is refused") {
        h.ui("chat.send", {{"request_id", "r2"}, {"text", "impatient"}});
        const auto errors = h.of_type("chat.error");
        REQUIRE(errors.size() == 1);
        CHECK(errors[0]["request_id"] == "r2");
        CHECK(errors[0]["code"] == "busy");
    }
    SECTION("cancelling a different request id does nothing") {
        h.ui("chat.cancel", {{"request_id", "other"}});
        h.pump();
        CHECK(h.controller->chat_in_progress());
    }

    h.ui("chat.cancel", {{"request_id", "r1"}});
    REQUIRE(h.pump_until([&] { return h.has("chat.done"); }));
    const auto done = h.of_type("chat.done");
    CHECK(done[0]["request_id"] == "r1");
    CHECK(done[0]["stop_reason"] == "cancelled");
    CHECK_FALSE(h.controller->chat_in_progress());
}

TEST_CASE("controller: destroying the controller mid-reply does not hang or crash", "[controller]") {
    Harness h("teardown");
    h.credentials.entries["apikey/anthropic"] = "k";
    h.transport.chunks = {
        sse(R"({"type":"message_start","message":{"usage":{"input_tokens":1}}})") +
            sse(R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})") +
            sse(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"partial"}})")};
    h.transport.block_after_first_chunk = true;
    h.ui("chat.send", {{"request_id", "r1"}, {"text", "question"}});
    REQUIRE(h.pump_until([&] { return h.has("chat.delta"); }));

    h.controller.reset();   // like closing the window while a reply is streaming
    h.pump();               // tasks queued by the dying worker must be harmless
    SUCCEED();
}

TEST_CASE("controller: switching theme is saved and survives a restart", "[controller]") {
    Harness h("theme");
    h.ui("settings.set_theme", {{"theme", "light"}});
    REQUIRE(h.of_type("app.state").size() == 1);
    CHECK(h.of_type("app.state")[0]["theme"] == "light");
    CHECK(h.controller->active_theme().id == "light");

    h.ui("settings.set_theme", {{"theme", "no-such-theme"}});
    REQUIRE(h.of_type("host.error").size() == 1);
    CHECK(h.of_type("host.error")[0]["code"] == "unknown_theme");
    CHECK(h.controller->active_theme().id == "light");

    h.sent.clear();
    h.make_controller();   // a new controller reads the settings file written by the old one
    h.ui("app.ready");
    CHECK(h.of_type("app.state")[0]["theme"] == "light");
}

TEST_CASE("controller: provider settings are validated", "[controller]") {
    Harness h("provider");

    h.ui("settings.set_provider",
         {{"provider", "openai"}, {"base_url", "http://example.com/v1"}, {"model", "some-model"}});
    REQUIRE(h.of_type("host.error").size() == 1);
    CHECK(h.of_type("host.error")[0]["code"] == "invalid_base_url");
    CHECK_FALSE(h.has("app.state"));

    h.ui("settings.set_provider", {{"provider", "made-up"}, {"base_url", "https://x.example/v1"}, {"model", "m"}});
    CHECK(h.of_type("host.error")[1]["code"] == "unknown_provider");

    h.ui("settings.set_provider",
         {{"provider", "openai"}, {"base_url", "https://api.openai.com/v1/"}, {"model", "some-model"}});
    REQUIRE(h.of_type("app.state").size() == 1);
    const json state = h.of_type("app.state")[0];
    CHECK(state["provider"] == "openai");
    CHECK(state["providers"][1]["base_url"] == "https://api.openai.com/v1");   // normalised
    CHECK(state["providers"][1]["model"] == "some-model");
}

TEST_CASE("controller: a server on this PC needs no key", "[controller]") {
    Harness h("loopback");
    h.ui("settings.set_provider",
         {{"provider", "openai"}, {"base_url", "http://127.0.0.1:9931/v1"}, {"model", "local-model"}});
    script_openai_reply(h.transport, {"local reply"});

    h.ui("chat.send", {{"request_id", "r1"}, {"text", "hi"}});
    REQUIRE(h.wait_for_reply());
    CHECK(h.has("chat.done"));
    CHECK(h.transport.last_request.url == "http://127.0.0.1:9931/v1/chat/completions");
    for (const auto& [name, value] : h.transport.last_request.headers) CHECK(name != "Authorization");
}

TEST_CASE("controller: a provider without a model cannot chat", "[controller]") {
    Harness h("nomodel");
    h.credentials.entries["apikey/openai"] = "k";
    h.ui("settings.set_provider", {{"provider", "openai"}, {"base_url", "https://api.openai.com/v1"}, {"model", ""}});
    h.ui("chat.send", {{"request_id", "r1"}, {"text", "hi"}});
    REQUIRE(h.of_type("chat.error").size() == 1);
    CHECK(h.of_type("chat.error")[0]["code"] == "not_configured");
    CHECK(h.transport.calls == 0);
}

TEST_CASE("controller: an API key is stored in the credential store and never sent back", "[controller]") {
    Harness h("key");
    h.ui("settings.set_api_key", {{"provider", "anthropic"}, {"key", "sk-ant-very-secret-123"}});

    CHECK(h.credentials.entries.at("apikey/anthropic") == "sk-ant-very-secret-123");
    REQUIRE(h.of_type("app.state").size() == 1);
    CHECK(h.of_type("app.state")[0]["providers"][0]["has_key"] == true);
    CHECK(h.of_type("app.state")[0]["providers"][1]["has_key"] == false);
    CHECK(h.all_text_sent().find("very-secret") == std::string::npos);

    // The key is not in the settings file either.
    h.ui("settings.set_theme", {{"theme", "light"}});   // forces a save
    std::ifstream file(h.dir / "settings.json");
    const std::string saved((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    CHECK_FALSE(saved.empty());
    CHECK(saved.find("very-secret") == std::string::npos);

    h.ui("settings.clear_api_key", {{"provider", "anthropic"}});
    CHECK(h.credentials.entries.count("apikey/anthropic") == 0);
    CHECK(h.of_type("app.state").back()["providers"][0]["has_key"] == false);

    h.ui("settings.set_api_key", {{"provider", "made-up"}, {"key", "k"}});
    CHECK(h.of_type("host.error").back()["code"] == "unknown_provider");
}

TEST_CASE("controller: UI log lines are accepted quietly", "[controller]") {
    Harness h("uilog");
    h.ui("ui.log", {{"level", "warn"}, {"message", "CSP blocked connect-src: https://example.com"}});
    CHECK(h.sent.empty());
}

// ---- companion shell (M1.5) ----

TEST_CASE("controller: a window that loads gets the state and then the conversation", "[controller][shell]") {
    Harness h("history-sync");
    h.credentials.entries["apikey/anthropic"] = "k";
    script_reply(h.transport, {"the answer"});
    h.ui("chat.send", {{"request_id", "r1"}, {"text", "the question"}});
    REQUIRE(h.wait_for_reply());

    h.sent.clear();
    h.ui("app.ready", json::object(), ViewId::Panel);
    REQUIRE(h.sent.size() == 2);
    CHECK(h.sent[0]["type"] == "app.state");
    CHECK(h.sent[1]["type"] == "chat.history");
    const json history = h.sent[1]["payload"];
    REQUIRE(history["messages"].size() == 2);
    CHECK(history["messages"][0] == json{{"role", "user"}, {"text", "the question"}});
    CHECK(history["messages"][1] == json{{"role", "assistant"}, {"text", "the answer"}});
    CHECK(history["active_request"].is_null());
}

TEST_CASE("controller: history sent mid-reply carries the partial text", "[controller][shell]") {
    Harness h("history-partial");
    h.credentials.entries["apikey/anthropic"] = "k";
    h.transport.chunks = {
        sse(R"({"type":"message_start","message":{"usage":{"input_tokens":1}}})") +
            sse(R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})") +
            sse(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"so far"}})"),
        sse(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"never"}})")};
    h.transport.block_after_first_chunk = true;
    h.ui("chat.send", {{"request_id", "r1"}, {"text", "long one"}});
    REQUIRE(h.pump_until([&] { return h.has("chat.delta"); }));

    h.ui("app.ready");
    const json history = h.of_type("chat.history").back();
    CHECK(history["active_request"] == "r1");
    CHECK(history["active_model"] == "claude-opus-5-5");
    CHECK(history["partial"] == "so far");
    REQUIRE(history["messages"].size() == 1);
    CHECK(history["messages"][0]["text"] == "long one");

    h.ui("chat.cancel", {{"request_id", "r1"}});
    REQUIRE(h.pump_until([&] { return h.has("chat.done"); }));
}

TEST_CASE("controller: first run is detected from the missing settings file", "[controller][shell]") {
    Harness h("first-run");
    CHECK(h.controller->first_run());
    h.ui("settings.set_theme", {{"theme", "light"}});   // writes the settings file
    h.make_controller();
    CHECK_FALSE(h.controller->first_run());
}

TEST_CASE("controller: the stored hotkey is registered and reported in the state", "[controller][shell]") {
    Harness h("hotkey-ok");
    CHECK(h.controller->hotkey_id() == 0);
    CHECK(h.controller->apply_stored_hotkey() == hotkey::Status::Active);
    CHECK(h.controller->hotkey_id() != 0);
    CHECK(h.registrar.ours.size() == 1);

    h.ui("app.ready");
    const json state = h.of_type("app.state")[0];
    CHECK(state["hotkey"] == json{{"requested", "Alt+Shift+Space"}, {"active", "Alt+Shift+Space"}, {"status", "active"}, {"message", ""}});
    CHECK(state["panel"] == json{{"hide_on_click_away", true}, {"pinned", false}, {"captions_first", true}});
    CHECK(state["muted"] == false);
}

TEST_CASE("controller: a hotkey taken at start-up is reported, with no live hotkey", "[controller][shell]") {
    Harness h("hotkey-taken-start");
    h.registrar.taken_elsewhere = {*hotkey::parse("Alt+Shift+Space").binding};
    CHECK(h.controller->apply_stored_hotkey() == hotkey::Status::Taken);
    CHECK(h.controller->hotkey_id() == 0);
    CHECK(h.controller->hotkey_active().empty());
    CHECK(h.controller->hotkey_message().find("already used by another app") != std::string::npos);

    h.ui("app.ready");
    const json hotkey = h.of_type("app.state")[0]["hotkey"];
    CHECK(hotkey["status"] == "taken");
    CHECK(hotkey["active"] == "");
    CHECK(hotkey["requested"] == "Alt+Shift+Space");
}

TEST_CASE("controller: changing the hotkey from settings", "[controller][shell]") {
    Harness h("hotkey-change");
    h.registrar.taken_elsewhere = {*hotkey::parse("Ctrl+Alt+K").binding};
    REQUIRE(h.controller->apply_stored_hotkey() == hotkey::Status::Active);

    SECTION("to a free binding: it becomes active and is saved") {
        h.ui("settings.set_hotkey", {{"hotkey", "ctrl+alt+b"}});
        CHECK_FALSE(h.has("host.error"));
        CHECK(h.of_type("app.state").back()["hotkey"]["active"] == "Ctrl+Alt+B");
        CHECK(h.controller->settings().hotkey == "Ctrl+Alt+B");
        CHECK(h.registrar.ours.size() == 1);

        h.make_controller();   // restart: the saved hotkey is the one that gets registered
        CHECK(h.controller->settings().hotkey == "Ctrl+Alt+B");
    }
    SECTION("to a binding another app owns: an error, and the old hotkey keeps working") {
        h.ui("settings.set_hotkey", {{"hotkey", "Ctrl+Alt+K"}});
        REQUIRE(h.of_type("host.error").size() == 1);
        CHECK(h.of_type("host.error")[0]["code"] == "hotkey_taken");
        CHECK(h.of_type("host.error")[0]["message"].get<std::string>().find("Ctrl+Alt+K is already used") != std::string::npos);
        const json hotkey = h.of_type("app.state").back()["hotkey"];
        CHECK(hotkey["status"] == "taken");
        CHECK(hotkey["requested"] == "Ctrl+Alt+K");
        CHECK(hotkey["active"] == "Alt+Shift+Space");
        CHECK(h.controller->settings().hotkey == "Alt+Shift+Space");   // not saved
        CHECK(h.controller->hotkey_id() != 0);
    }
    SECTION("to something that is not a usable shortcut") {
        h.ui("settings.set_hotkey", {{"hotkey", "Win+Q"}});
        CHECK(h.of_type("host.error")[0]["code"] == "hotkey_invalid");
        CHECK(h.controller->hotkey_active() == "Alt+Shift+Space");
    }
}

TEST_CASE("controller: shutdown releases the hotkey", "[controller][shell]") {
    Harness h("hotkey-release");
    REQUIRE(h.controller->apply_stored_hotkey() == hotkey::Status::Active);
    h.controller->shutdown();
    CHECK(h.registrar.ours.empty());
}

TEST_CASE("controller: panel options, pin, mute and size are saved", "[controller][shell]") {
    Harness h("panel-options");
    h.ui("settings.set_panel", {{"hide_on_click_away", false}, {"pinned", true}, {"captions_first", false}}, ViewId::Panel);
    CHECK(h.of_type("app.state").back()["panel"] ==
          json{{"hide_on_click_away", false}, {"pinned", true}, {"captions_first", false}});

    h.controller->set_muted(true);
    CHECK(h.of_type("app.state").back()["muted"] == true);
    h.controller->set_panel_pinned(false);
    CHECK(h.of_type("app.state").back()["panel"]["pinned"] == false);
    h.controller->set_panel_size(700, 5);   // the height is below the minimum

    h.make_controller();
    const auto& s = h.controller->settings();
    CHECK_FALSE(s.hide_on_click_away);
    CHECK_FALSE(s.panel_pinned);
    CHECK_FALSE(s.captions_first);
    CHECK(s.muted);
    CHECK(s.panel_width == 700);
    CHECK(s.panel_height == 240);
}

TEST_CASE("controller: window requests name the window they came from", "[controller][shell]") {
    Harness h("window-requests");
    h.ui("window.hide", json::object(), ViewId::Panel);
    h.ui("window.hide", json::object(), ViewId::Main);
    REQUIRE(h.hidden.size() == 2);
    CHECK(h.hidden[0] == ViewId::Panel);
    CHECK(h.hidden[1] == ViewId::Main);

    h.ui("window.open_main", {{"settings", true}}, ViewId::Panel);
    h.ui("window.open_main", {{"settings", false}}, ViewId::Panel);
    CHECK(h.opened_main == std::vector<bool>{true, false});
    CHECK(h.sent.empty());   // these only act on windows; nothing is sent back
}

// ---- voice (M3) ----

TEST_CASE("controller: the state carries the voice and speech settings", "[controller][voice]") {
    Harness h("voice-state");
    h.ui("app.ready");
    const json state = h.of_type("app.state").back();
    CHECK(state["voice"] == json{{"base_url", "http://127.0.0.1:8000/v1"}, {"model", "whisper-1"}});
    // No neural speech server by default: replies are read by a Windows voice, on this PC.
    CHECK(state["speech"] == json{{"voice", ""}, {"base_url", ""}, {"model", ""}, {"rate", 0}, {"pitch", 0}});
}

TEST_CASE("controller: the transcription server must be on this PC", "[controller][voice]") {
    Harness h("voice-settings");
    h.ui("settings.set_voice", {{"base_url", "https://example.com/v1"}, {"model", "whisper-1"}});
    REQUIRE(h.has("host.error"));
    CHECK(h.of_type("host.error").back()["code"] == "invalid_voice_endpoint");
    CHECK(h.controller->settings().voice_base_url == "http://127.0.0.1:8000/v1");

    h.ui("settings.set_voice", {{"base_url", "not a url"}, {"model", "whisper-1"}});
    CHECK(h.of_type("host.error").size() == 2);
    CHECK(h.controller->settings().voice_base_url == "http://127.0.0.1:8000/v1");

    h.sent.clear();
    h.ui("settings.set_voice", {{"base_url", "http://localhost:9000/v1"}, {"model", "small.en"}});
    CHECK_FALSE(h.has("host.error"));
    CHECK(h.of_type("app.state").back()["voice"] == json{{"base_url", "http://localhost:9000/v1"}, {"model", "small.en"}});

    h.make_controller();   // a restart: the choice was saved
    CHECK(h.controller->settings().voice_base_url == "http://localhost:9000/v1");
    CHECK(h.controller->settings().voice_model == "small.en");
}

TEST_CASE("controller: speech settings, with an optional neural server on this PC", "[controller][voice]") {
    Harness h("speech-settings");
    const auto speech = [](const json& changes) {
        json payload = {{"voice", ""}, {"base_url", ""}, {"model", ""}, {"rate", 0}, {"pitch", 0}};
        payload.update(changes);
        return payload;
    };

    SECTION("Windows voices only") {
        h.ui("settings.set_speech", speech({{"voice", "  Microsoft Zira Desktop "}, {"rate", 2}, {"pitch", -1}}));
        CHECK_FALSE(h.has("host.error"));
        const auto& s = h.controller->settings();
        CHECK(s.speech_voice == "Microsoft Zira Desktop");
        CHECK(s.speech_base_url.empty());
        CHECK(s.speech_rate == 2);
        CHECK(s.speech_pitch == -1);
    }
    SECTION("a neural server on this PC") {
        h.ui("settings.set_speech", speech({{"base_url", "http://127.0.0.1:8880/v1"}, {"model", "kokoro"}}));
        CHECK_FALSE(h.has("host.error"));
        h.make_controller();
        CHECK(h.controller->settings().speech_base_url == "http://127.0.0.1:8880/v1");
        CHECK(h.controller->settings().speech_model == "kokoro");

        // And switching it off again.
        h.ui("settings.set_speech", speech(json::object()));
        CHECK(h.controller->settings().speech_base_url.empty());
    }
    SECTION("a remote server is refused") {
        h.ui("settings.set_speech", speech({{"base_url", "https://example.com/v1"}, {"model", "tts-1"}, {"rate", 5}}));
        REQUIRE(h.has("host.error"));
        CHECK(h.of_type("host.error").back()["code"] == "invalid_speech_endpoint");
        CHECK(h.controller->settings().speech_base_url.empty());
        CHECK(h.controller->settings().speech_rate == 0);   // nothing from a refused message is kept
    }
    SECTION("a server without a model is refused") {
        h.ui("settings.set_speech", speech({{"base_url", "http://127.0.0.1:8880/v1"}}));
        REQUIRE(h.has("host.error"));
        CHECK(h.of_type("host.error").back()["code"] == "invalid_speech_endpoint");
        CHECK(h.controller->settings().speech_base_url.empty());
    }
}

TEST_CASE("controller: a spoken question is sent like a typed one", "[controller][voice]") {
    Harness h("voice-submit");
    h.credentials.entries["apikey/anthropic"] = "sk-ant-test";
    script_reply(h.transport, {"Hi"});

    h.controller->submit_voice_text("  what is a mutex \n");
    REQUIRE(h.wait_for_reply());
    const auto started = h.of_type("chat.started");
    REQUIRE(started.size() == 1);
    CHECK(started[0]["request_id"] == "voice_1");
    CHECK(started[0]["text"] == "what is a mutex");
    CHECK(h.last_request_body()["messages"][0] == json{{"role", "user"}, {"content", "what is a mutex"}});
    CHECK(h.has("chat.done"));
    CHECK_FALSE(h.has("voice.state"));

    // Each spoken question gets its own request id.
    h.sent.clear();
    script_reply(h.transport, {"Again"});
    h.controller->submit_voice_text("and a semaphore");
    REQUIRE(h.wait_for_reply());
    CHECK(h.of_type("chat.started")[0]["request_id"] == "voice_2");

    // Silence transcribed as blank text is not a question.
    h.sent.clear();
    h.controller->submit_voice_text(" \n ");
    CHECK(h.sent.empty());
}

TEST_CASE("controller: a spoken question that cannot be sent says why", "[controller][voice]") {
    SECTION("no API key") {
        Harness h("voice-refused");
        h.controller->submit_voice_text("hello");
        // No window sent this request, so the reason must not go out as a chat.error that every
        // window would ignore.
        CHECK_FALSE(h.has("chat.started"));
        CHECK_FALSE(h.has("chat.error"));
        const auto states = h.of_type("voice.state");
        REQUIRE(states.size() == 1);
        CHECK(states[0]["state"] == "error");
        CHECK(states[0]["error"].get<std::string>().find("API key") != std::string::npos);
        CHECK(h.transport.calls == 0);
    }
    SECTION("a reply is still running") {
        Harness h("voice-busy");
        h.credentials.entries["apikey/anthropic"] = "k";
        h.transport.chunks = {
            sse(R"({"type":"message_start","message":{"usage":{"input_tokens":1}}})") +
                sse(R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})") +
                sse(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"partial"}})"),
            sse(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"never"}})")};
        h.transport.block_after_first_chunk = true;
        h.ui("chat.send", {{"request_id", "r1"}, {"text", "long question"}});
        REQUIRE(h.pump_until([&] { return h.has("chat.delta"); }));

        h.controller->submit_voice_text("impatient");
        const auto states = h.of_type("voice.state");
        REQUIRE(states.size() == 1);
        CHECK(states[0]["state"] == "error");
        CHECK(states[0]["error"].get<std::string>().find("already in progress") != std::string::npos);
        CHECK(h.of_type("chat.started").size() == 1);   // only the typed one

        h.ui("chat.cancel", {{"request_id", "r1"}});
        REQUIRE(h.pump_until([&] { return h.has("chat.done"); }));
    }
}

TEST_CASE("controller: a finished reply is read aloud, unless muted", "[controller][voice]") {
    Harness h("speak");
    h.credentials.entries["apikey/anthropic"] = "sk-ant-test";

    script_reply(h.transport, {"Hello", " ", "world"});
    h.ui("chat.send", {{"request_id", "r1"}, {"text", "say hello"}});
    REQUIRE(h.wait_for_reply());
    CHECK(h.spoken == std::vector<std::string>{"Hello world"});
    CHECK(h.captions == std::vector<std::string>{"Hello world"});

    h.controller->set_muted(true);
    h.sent.clear();
    script_reply(h.transport, {"Quiet"});
    h.ui("chat.send", {{"request_id", "r2"}, {"text", "again"}});
    REQUIRE(h.wait_for_reply());
    CHECK(h.spoken.size() == 1);   // nothing new
    CHECK(h.captions == std::vector<std::string>{"Hello world", "Quiet"});
}

TEST_CASE("controller: markdown replies are spoken as plain text", "[controller][voice]") {
    Harness h("speak-markdown");
    h.credentials.entries["apikey/anthropic"] = "sk-ant-test";
    script_reply(h.transport, {"Click **File**, then New."});
    h.ui("chat.send", {{"request_id", "r1"}, {"text", "where"}});
    REQUIRE(h.wait_for_reply());
    CHECK(h.spoken == std::vector<std::string>{"Click File, then New."});
    CHECK(h.captions == std::vector<std::string>{"Click File, then New."});
}

TEST_CASE("controller: a code reply is spoken but not captioned", "[controller][voice]") {
    Harness h("speak-code");
    h.credentials.entries["apikey/anthropic"] = "sk-ant-test";
    script_reply(h.transport, {"Sure.\n```cpp\nint x = 1;\n```\n"});
    h.ui("chat.send", {{"request_id", "r1"}, {"text", "code"}});
    REQUIRE(h.wait_for_reply());
    CHECK(h.spoken == std::vector<std::string>{"Sure."});
    CHECK(h.captions.empty());
}

TEST_CASE("controller: captions-first hides short panel replies and offers expansion for complex ones",
          "[controller][m4]") {
    SECTION("short plain text becomes a caption and hides the panel") {
        Harness h("captions-first-short");
        h.credentials.entries["apikey/anthropic"] = "k";
        script_reply(h.transport, {"Click File, then New."});
        h.ui("chat.send", {{"request_id", "r1"}, {"text", "how do I start?"}}, ViewId::Panel);
        REQUIRE(h.wait_for_reply());
        CHECK(h.captions == std::vector<std::string>{"Click File, then New."});
        CHECK(h.hidden == std::vector<ViewId>{ViewId::Panel});
        CHECK(h.of_type("chat.done").back()["expand_available"] == false);
    }

    SECTION("code stays in the panel and has an expand action") {
        Harness h("captions-first-code");
        h.credentials.entries["apikey/anthropic"] = "k";
        script_reply(h.transport, {"```cpp\nint answer = 42;\n```"});
        h.ui("chat.send", {{"request_id", "r1"}, {"text", "show code"}}, ViewId::Panel);
        REQUIRE(h.wait_for_reply());
        CHECK(h.captions.empty());
        CHECK(h.hidden.empty());
        CHECK(h.of_type("chat.done").back()["expand_available"] == true);
    }
}

TEST_CASE("controller: always-in-panel disables short reply captions and auto-hide", "[controller][m4]") {
    Harness h("always-panel");
    h.credentials.entries["apikey/anthropic"] = "k";
    h.ui("settings.set_panel", {{"hide_on_click_away", true}, {"pinned", false}, {"captions_first", false}});
    script_reply(h.transport, {"A short answer."});
    h.ui("chat.send", {{"request_id", "r1"}, {"text", "question"}}, ViewId::Panel);
    REQUIRE(h.wait_for_reply());
    CHECK(h.captions.empty());
    CHECK(h.hidden.empty());
    CHECK(h.of_type("chat.done").back()["expand_available"] == false);
}

TEST_CASE("controller: overlay debug captions are read aloud unless muted", "[controller][voice]") {
    Harness h("debug-caption");
    h.ui("overlay.debug", {{"action", "caption"}, {"caption", "Look here"}});
    CHECK(h.spoken == std::vector<std::string>{"Look here"});

    h.controller->set_muted(true);
    h.ui("overlay.debug", {{"action", "caption"}, {"caption", "Silent"}});
    CHECK(h.spoken.size() == 1);
}

TEST_CASE("controller: a failed reply is not read aloud", "[controller][voice]") {
    Harness h("speak-failure");
    h.credentials.entries["apikey/anthropic"] = "wrong";
    h.transport.status = 401;
    h.transport.content_type = "application/json";
    h.transport.chunks = {R"({"error":{"message":"invalid x-api-key"}})"};
    h.ui("chat.send", {{"request_id", "r1"}, {"text", "question"}});
    REQUIRE(h.wait_for_reply());
    CHECK(h.spoken.empty());
}

TEST_CASE("controller: voice.cancel reaches the shell", "[controller][voice]") {
    Harness h("voice-cancel");
    h.ui("voice.cancel", json::object(), ViewId::Panel);
    CHECK(h.voice_cancels == 1);
    CHECK(h.sent.empty());
}
