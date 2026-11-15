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
    for (const std::string& piece : pieces) transport.chunks.push_back(text_chunk(piece));
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

    REQUIRE(state["providers"].size() == 2);
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
    CHECK(done[0]["stop_reason"] == "stop");
    CHECK(done[0]["input_tokens"] == 20);
    CHECK(done[0]["output_tokens"] == 5);
    CHECK_FALSE(h.has("chat.error"));

    // The request went to the configured endpoint with the stored key and the tutor prompt.
    CHECK(h.transport.last_request.url == "https://api.anthropic.com/v1/chat/completions");
    bool has_auth = false;
    for (const auto& [name, value] : h.transport.last_request.headers) {
        if (name == "Authorization") has_auth = value == "Bearer sk-ant-test";
    }
    CHECK(has_auth);
    const json body = h.last_request_body();
    CHECK(body["model"] == "claude-opus-5-5");
    REQUIRE(body["messages"].size() == 2);
    CHECK(body["messages"][0]["role"] == "system");
    CHECK(body["messages"][1] == json{{"role", "user"}, {"content", "say hello"}});
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
    REQUIRE(body["messages"].size() == 4);
    CHECK(body["messages"][1]["content"] == "first question");
    CHECK(body["messages"][2] == json{{"role", "assistant"}, {"content", "first answer"}});
    CHECK(body["messages"][3]["content"] == "second question");

    SECTION("chat.clear forgets it") {
        h.ui("chat.clear");
        CHECK(h.has("chat.cleared"));
        script_reply(h.transport, {"fresh"});
        h.ui("chat.send", {{"request_id", "r3"}, {"text", "new topic"}});
        REQUIRE(h.pump_until([&] { return h.of_type("chat.done").size() == 2; }));
        CHECK(h.last_request_body()["messages"].size() == 2);
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
    CHECK(h.last_request_body()["messages"].size() == 2);
}

TEST_CASE("controller: cancel stops a running reply", "[controller]") {
    Harness h("cancel");
    h.credentials.entries["apikey/anthropic"] = "k";
    h.transport.chunks = {text_chunk("partial"), text_chunk("never")};
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
    h.transport.chunks = {text_chunk("partial")};
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
    script_reply(h.transport, {"local reply"});

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
    h.transport.chunks = {text_chunk("so far"), text_chunk("never")};
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
    CHECK(state["panel"] == json{{"hide_on_click_away", true}, {"pinned", false}});
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
    h.ui("settings.set_panel", {{"hide_on_click_away", false}, {"pinned", true}}, ViewId::Panel);
    CHECK(h.of_type("app.state").back()["panel"] == json{{"hide_on_click_away", false}, {"pinned", true}});

    h.controller->set_muted(true);
    CHECK(h.of_type("app.state").back()["muted"] == true);
    h.controller->set_panel_pinned(false);
    CHECK(h.of_type("app.state").back()["panel"]["pinned"] == false);
    h.controller->set_panel_size(700, 5);   // the height is below the minimum

    h.make_controller();
    const auto& s = h.controller->settings();
    CHECK_FALSE(s.hide_on_click_away);
    CHECK_FALSE(s.panel_pinned);
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
