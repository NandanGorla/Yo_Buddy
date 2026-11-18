#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/settings.h"

using namespace buddy::settings;
using json = nlohmann::json;

TEST_CASE("settings: defaults include cloud and explicit local providers", "[settings]") {
    const Settings s = defaults();
    REQUIRE(s.providers.size() == 3);
    CHECK(s.provider == "anthropic");
    CHECK(s.active().id == "anthropic");
    CHECK(s.find("anthropic")->base_url == "https://api.anthropic.com/v1");
    CHECK(s.find("anthropic")->model == "claude-opus-5-5");
    CHECK(s.find("openai")->base_url == "https://api.openai.com/v1");
    CHECK(s.find("local")->base_url == "http://127.0.0.1:9931/v1");
    CHECK(s.find("local")->model.empty());
    CHECK(s.find("nope") == nullptr);
}

TEST_CASE("settings: a round trip keeps user choices", "[settings]") {
    Settings s = defaults();
    s.theme = "light";
    s.provider = "openai";
    s.find("openai")->model = "some-model";
    s.find("anthropic")->base_url = "http://127.0.0.1:9931/v1";

    const Settings back = from_json(to_json(s));
    CHECK(back.theme == "light");
    CHECK(back.provider == "openai");
    CHECK(back.find("openai")->model == "some-model");
    CHECK(back.find("anthropic")->base_url == "http://127.0.0.1:9931/v1");
}

TEST_CASE("settings: stored JSON never contains a key field", "[settings]") {
    const std::string text = to_json(defaults());
    CHECK(text.find("api_key") == std::string::npos);
    CHECK(text.find("apikey") == std::string::npos);
    CHECK(text.find("sk-") == std::string::npos);
    CHECK(text.find("secret") == std::string::npos);
}

TEST_CASE("settings: damaged input falls back field by field", "[settings]") {
    std::vector<std::string> warnings;

    CHECK(from_json("not json", &warnings).provider == "anthropic");
    CHECK(warnings.size() == 1);

    warnings.clear();
    const json damaged = {
        {"theme", "../../etc"},
        {"provider", "made-up"},
        {"providers",
         {{"anthropic", {{"base_url", "http://evil.example/v1"}, {"model", "bad model name"}}},
          {"openai", {{"base_url", 12}, {"model", "fine-model"}}},
          {"unknown", {{"base_url", "https://x.example"}}}}},
    };
    const Settings s = from_json(damaged.dump(), &warnings);
    CHECK(s.theme == "dark");                                              // invalid id ignored
    CHECK(s.provider == "anthropic");                                      // unknown provider ignored
    CHECK(s.find("anthropic")->base_url == "https://api.anthropic.com/v1");   // plain http to a remote host refused
    CHECK(s.find("anthropic")->model == "claude-opus-5-5");
    CHECK(s.find("openai")->base_url == "https://api.openai.com/v1");
    CHECK(s.find("openai")->model == "fine-model");                        // the valid field is still taken
    CHECK(s.providers.size() == 3);                                        // unknown providers are not added
    CHECK(warnings.size() == 5);
}

TEST_CASE("settings: save and load through a file", "[settings]") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "buddy-test-settings";
    std::filesystem::remove_all(dir);
    const std::filesystem::path file = dir / "nested" / "settings.json";

    CHECK(load(file).theme == "dark");   // no file yet: defaults

    Settings s = defaults();
    s.theme = "light";
    REQUIRE(save(file, s));
    CHECK(load(file).theme == "light");
    CHECK_FALSE(std::filesystem::exists(dir / "nested" / "settings.json.tmp"));

    s.theme = "dark";
    REQUIRE(save(file, s));   // overwriting an existing file works too
    CHECK(load(file).theme == "dark");

    std::filesystem::remove_all(dir);
}

TEST_CASE("settings: companion shell options round-trip", "[settings]") {
    const Settings d = defaults();
    CHECK(d.hotkey == "Alt+Shift+Space");
    CHECK(d.panel_width == 440);
    CHECK(d.panel_height == 560);
    CHECK(d.hide_on_click_away);
    CHECK_FALSE(d.panel_pinned);
    CHECK(d.captions_first);
    CHECK_FALSE(d.muted);

    Settings s = defaults();
    s.hotkey = "Ctrl+Alt+B";
    s.panel_width = 600;
    s.panel_height = 700;
    s.hide_on_click_away = false;
    s.panel_pinned = true;
    s.captions_first = false;
    s.muted = true;
    const Settings back = from_json(to_json(s));
    CHECK(back.hotkey == "Ctrl+Alt+B");
    CHECK(back.panel_width == 600);
    CHECK(back.panel_height == 700);
    CHECK_FALSE(back.hide_on_click_away);
    CHECK(back.panel_pinned);
    CHECK_FALSE(back.captions_first);
    CHECK(back.muted);
}

TEST_CASE("settings: capture policy defaults and round-trip", "[settings]") {
    const Settings d = defaults();
    CHECK(d.capture_send_preview);
    CHECK(d.capture_exclusions.size() == 5);

    Settings s = d;
    s.capture_send_preview = false;
    s.capture_exclusions = {"My Password App", "Private Window"};
    const Settings back = from_json(to_json(s));
    CHECK_FALSE(back.capture_send_preview);
    CHECK(back.capture_exclusions == s.capture_exclusions);
}

TEST_CASE("settings: unsafe capture exclusions are rejected", "[settings]") {
    std::vector<std::string> warnings;
    const json damaged = {
        {"capture", {{"exclusions", {"ok", 7}}, {"send_preview", "no"}}},
    };
    const Settings s = from_json(damaged.dump(), &warnings);
    CHECK(s.capture_exclusions == defaults().capture_exclusions);
    CHECK(s.capture_send_preview);
    CHECK(warnings.size() == 2);
}

TEST_CASE("settings: bad shell options fall back or are clamped", "[settings]") {
    std::vector<std::string> warnings;
    const json damaged = {
        {"hotkey", "Win+Q"},
        {"panel", {{"width", 5}, {"height", 999999}, {"hide_on_click_away", "yes"}, {"pinned", 1},
                   {"captions_first", "yes"}}},
        {"muted", "no"},
    };
    const Settings s = from_json(damaged.dump(), &warnings);
    CHECK(s.hotkey == "Alt+Shift+Space");   // an unusable stored hotkey is replaced by the default
    CHECK(s.panel_width == 320);            // clamped to the minimum
    CHECK(s.panel_height == 1400);          // clamped to the maximum
    CHECK(s.hide_on_click_away);            // wrong type: default kept
    CHECK_FALSE(s.panel_pinned);
    CHECK(s.captions_first);
    CHECK_FALSE(s.muted);
    CHECK(warnings.size() == 1);

    // Stored spellings are normalised.
    CHECK(from_json(R"({"hotkey":"shift + alt + space"})").hotkey == "Alt+Shift+Space");
}

TEST_CASE("settings: voice defaults keep reply text on this PC", "[settings][voice]") {
    const Settings s = defaults();
    // Windows voices are the default. No neural speech server is configured, so nothing is
    // posted anywhere when a reply is read aloud.
    CHECK(s.speech_base_url.empty());
    CHECK(s.speech_model.empty());
    CHECK(s.speech_voice.empty());
    CHECK(s.speech_rate == 0);
    CHECK(s.speech_pitch == 0);
    CHECK(s.voice_base_url == "http://127.0.0.1:8000/v1");
    CHECK(s.voice_model == "whisper-1");
}

TEST_CASE("settings: voice and speech options round-trip", "[settings][voice]") {
    Settings s = defaults();
    s.voice_base_url = "http://localhost:9000/v1";
    s.voice_model = "small.en";
    s.speech_voice = "Microsoft Zira Desktop";
    s.speech_base_url = "http://127.0.0.1:8880/v1";
    s.speech_model = "kokoro";
    s.speech_rate = -4;
    s.speech_pitch = 7;

    const Settings back = from_json(to_json(s));
    CHECK(back.voice_base_url == "http://localhost:9000/v1");
    CHECK(back.voice_model == "small.en");
    CHECK(back.speech_voice == "Microsoft Zira Desktop");
    CHECK(back.speech_base_url == "http://127.0.0.1:8880/v1");
    CHECK(back.speech_model == "kokoro");
    CHECK(back.speech_rate == -4);
    CHECK(back.speech_pitch == 7);

    // Switching the neural server off again is kept too.
    s.speech_base_url.clear();
    s.speech_model.clear();
    const Settings off = from_json(to_json(s));
    CHECK(off.speech_base_url.empty());
    CHECK(off.speech_model.empty());
}

TEST_CASE("settings: bad voice and speech options fall back or are clamped", "[settings][voice]") {
    std::vector<std::string> warnings;
    const Settings s = from_json(R"({
        "voice": {"base_url": "https://example.com/v1", "model": ""},
        "speech": {"voice": 7, "base_url": "https://example.com/v1", "model": "bad model",
                   "rate": 99, "pitch": -99}
    })",
                                 &warnings);
    // Recorded speech and reply text may only go to this PC: remote servers are dropped.
    CHECK(s.voice_base_url == "http://127.0.0.1:8000/v1");
    CHECK(s.voice_model == "whisper-1");   // an empty model is not a model
    CHECK(s.speech_voice.empty());
    CHECK(s.speech_base_url.empty());
    CHECK(s.speech_model.empty());
    CHECK(s.speech_rate == 10);
    CHECK(s.speech_pitch == -10);
    CHECK(warnings.size() == 5);
}
