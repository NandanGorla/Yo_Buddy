#include "core/settings.h"

#include <fstream>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

#include "core/hotkey.h"
#include "core/placement.h"
#include "core/protocol.h"
#include "core/theme.h"
#include "core/url.h"

namespace buddy::settings {
namespace {

using json = nlohmann::json;

void warn(std::vector<std::string>* warnings, std::string message) {
    if (warnings) warnings->push_back(std::move(message));
}

}  // namespace

const ProviderConfig* Settings::find(std::string_view id) const {
    for (const auto& p : providers) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

ProviderConfig* Settings::find(std::string_view id) {
    for (auto& p : providers) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

const ProviderConfig& Settings::active() const {
    if (const ProviderConfig* p = find(provider)) return *p;
    return providers.front();
}

Settings defaults() {
    Settings s;
    s.capture_exclusions = {"1Password", "Bitwarden", "KeePass", "LastPass", "Windows Security"};
    // Cloud entries use the same OpenAI-compatible client (STATUS.md D10). Local is an explicit
    // loopback provider: the user enters the URL and model instead of running discovery.
    // Anthropic's endpoint and model id were checked against its "OpenAI SDK compatibility" page
    // on 2026-11-15. OpenAI's model list was not checked, so no default model is assumed there.
    ProviderConfig anthropic;
    anthropic.id = "anthropic";
    anthropic.label = "Anthropic";
    anthropic.default_base_url = "https://api.anthropic.com/v1";
    anthropic.default_model = "claude-opus-5-5";
    anthropic.max_output_tokens = 64000;
    anthropic.base_url = anthropic.default_base_url;
    anthropic.model = anthropic.default_model;

    ProviderConfig openai;
    openai.id = "openai";
    openai.label = "OpenAI";
    openai.default_base_url = "https://api.openai.com/v1";
    openai.default_model = "";
    openai.max_output_tokens = 0;
    openai.base_url = openai.default_base_url;
    openai.model = openai.default_model;

    ProviderConfig local;
    local.id = "local";
    local.label = "Local server";
    local.default_base_url = "http://127.0.0.1:9931/v1";
    local.base_url = local.default_base_url;
    local.default_model = "";
    local.model = "";

    s.providers = {anthropic, openai, local};
    return s;
}

Settings from_json(std::string_view text, std::vector<std::string>* warnings) {
    Settings s = defaults();

    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        warn(warnings, "settings file is not a JSON object; using defaults");
        return s;
    }

    if (const auto it = root.find("theme"); it != root.end()) {
        if (it->is_string() && protocol::is_valid_id(it->get_ref<const std::string&>())) {
            s.theme = it->get<std::string>();
        } else {
            warn(warnings, "ignored invalid \"theme\"");
        }
    }

    if (const auto it = root.find("providers"); it != root.end() && it->is_object()) {
        for (ProviderConfig& p : s.providers) {
            const auto entry = it->find(p.id);
            if (entry == it->end() || !entry->is_object()) continue;

            if (const auto u = entry->find("base_url"); u != entry->end()) {
                const url::BaseUrlResult parsed =
                    u->is_string() ? url::parse_base_url(u->get_ref<const std::string&>())
                                   : url::BaseUrlResult{};
                if (parsed.ok()) {
                    p.base_url = parsed.url->to_string();
                } else {
                    warn(warnings, "ignored invalid base_url for " + p.id);
                }
            }
            if (const auto m = entry->find("model"); m != entry->end()) {
                if (m->is_string() && protocol::is_valid_model(m->get_ref<const std::string&>())) {
                    p.model = m->get<std::string>();
                } else {
                    warn(warnings, "ignored invalid model for " + p.id);
                }
            }
        }
    }

    if (const auto it = root.find("hotkey"); it != root.end()) {
        if (it->is_string() && hotkey::parse(it->get_ref<const std::string&>()).ok()) {
            s.hotkey = hotkey::format(*hotkey::parse(it->get_ref<const std::string&>()).binding);
        } else {
            warn(warnings, "ignored invalid \"hotkey\"");
        }
    }
    if (const auto it = root.find("panel"); it != root.end() && it->is_object()) {
        const auto read_int = [&](const char* key, int& out) {
            if (const auto v = it->find(key); v != it->end() && v->is_number_integer()) out = static_cast<int>(v->get<std::int64_t>());
        };
        const auto read_bool = [&](const char* key, bool& out) {
            if (const auto v = it->find(key); v != it->end() && v->is_boolean()) out = v->get<bool>();
        };
        read_int("width", s.panel_width);
        read_int("height", s.panel_height);
        read_bool("hide_on_click_away", s.hide_on_click_away);
        read_bool("pinned", s.panel_pinned);
        read_bool("captions_first", s.captions_first);
        const placement::Size clamped = placement::clamp_panel_size({s.panel_width, s.panel_height});
        s.panel_width = clamped.width;
        s.panel_height = clamped.height;
    }
    if (const auto it = root.find("muted"); it != root.end() && it->is_boolean()) s.muted = it->get<bool>();
    if (const auto it = root.find("voice"); it != root.end() && it->is_object()) {
        if (const auto base = it->find("base_url"); base != it->end() && base->is_string()) {
            const url::BaseUrlResult parsed = url::parse_base_url(base->get_ref<const std::string&>());
            if (parsed.ok() && url::is_loopback_host(parsed.url->host)) s.voice_base_url = parsed.url->to_string();
            else warn(warnings, "ignored invalid voice base_url");
        }
        if (const auto model = it->find("model"); model != it->end()) {
            // json::empty() is false for every string, so the value itself has to be checked.
            const std::string* value = model->get_ptr<const std::string*>();
            if (value && !value->empty() && protocol::is_valid_model(*value)) {
                s.voice_model = *value;
            } else {
                warn(warnings, "ignored invalid voice model");
            }
        }
    }
    if (const auto it = root.find("speech"); it != root.end() && it->is_object()) {
        if (const auto voice = it->find("voice"); voice != it->end()) {
            const std::string* value = voice->get_ptr<const std::string*>();
            if (value && value->size() <= protocol::kMaxSpeechVoiceLength) s.speech_voice = *value;
            else warn(warnings, "ignored invalid speech voice");
        }
        // An empty base_url is a real choice: no neural server, Windows voices only.
        if (const auto base = it->find("base_url"); base != it->end()) {
            const std::string* value = base->get_ptr<const std::string*>();
            if (value && value->empty()) {
                s.speech_base_url.clear();
            } else if (value) {
                const url::BaseUrlResult parsed = url::parse_base_url(*value);
                if (parsed.ok() && url::is_loopback_host(parsed.url->host)) s.speech_base_url = parsed.url->to_string();
                else warn(warnings, "ignored invalid speech base_url");
            } else {
                warn(warnings, "ignored invalid speech base_url");
            }
        }
        if (const auto model = it->find("model"); model != it->end()) {
            const std::string* value = model->get_ptr<const std::string*>();
            if (value && protocol::is_valid_model(*value)) s.speech_model = *value;
            else warn(warnings, "ignored invalid speech model");
        }
        if (const auto rate = it->find("rate"); rate != it->end() && rate->is_number_integer()) {
            s.speech_rate = static_cast<int>(std::clamp<std::int64_t>(rate->get<std::int64_t>(), -10, 10));
        }
        if (const auto pitch = it->find("pitch"); pitch != it->end() && pitch->is_number_integer()) {
            s.speech_pitch = static_cast<int>(std::clamp<std::int64_t>(pitch->get<std::int64_t>(), -10, 10));
        }
    }
    if (const auto it = root.find("overlay"); it != root.end() && it->is_object()) {
        const auto read_color = [&](const char* key, std::string& out) {
            const auto value = it->find(key);
            if (value != it->end() && value->is_string() && theme::is_valid_color(value->get_ref<const std::string&>())) {
                out = value->get<std::string>();
            } else if (value != it->end()) {
                warn(warnings, std::string("ignored invalid overlay ") + key);
            }
        };
        read_color("ring", s.overlay_ring_color);
        read_color("dot", s.overlay_dot_color);
        read_color("caption", s.overlay_caption_color);
    }
    if (const auto it = root.find("capture"); it != root.end() && it->is_object()) {
        if (const auto exclusions = it->find("exclusions"); exclusions != it->end()) {
            if (!exclusions->is_array() || exclusions->size() > 64) {
                warn(warnings, "ignored invalid capture exclusions");
            } else {
                std::vector<std::string> parsed;
                bool valid = true;
                for (const auto& value : *exclusions) {
                    if (!value.is_string() || value.get<std::string>().empty() ||
                        value.get<std::string>().size() > 128) {
                        valid = false;
                        break;
                    }
                    parsed.push_back(value.get<std::string>());
                }
                if (valid) s.capture_exclusions = std::move(parsed);
                else warn(warnings, "ignored invalid capture exclusions");
            }
        }
        if (const auto preview = it->find("send_preview"); preview != it->end()) {
            if (preview->is_boolean()) s.capture_send_preview = preview->get<bool>();
            else warn(warnings, "ignored invalid capture send_preview");
        }
    }

    if (const auto it = root.find("provider"); it != root.end()) {
        if (it->is_string() && s.find(it->get_ref<const std::string&>()) != nullptr) {
            s.provider = it->get<std::string>();
        } else {
            warn(warnings, "ignored unknown \"provider\"");
        }
    }
    return s;
}

std::string to_json(const Settings& settings) {
    json providers = json::object();
    for (const ProviderConfig& p : settings.providers) {
        providers[p.id] = {{"base_url", p.base_url}, {"model", p.model}};
    }
    const json root = {{"version", 1},
                       {"theme", settings.theme},
                       {"provider", settings.provider},
                       {"providers", std::move(providers)},
                       {"hotkey", settings.hotkey},
                       {"panel",
                        {{"width", settings.panel_width},
                         {"height", settings.panel_height},
                         {"hide_on_click_away", settings.hide_on_click_away},
                         {"pinned", settings.panel_pinned},
                         {"captions_first", settings.captions_first}}},
                       {"muted", settings.muted},
                       {"voice", {{"base_url", settings.voice_base_url}, {"model", settings.voice_model}}},
                       {"speech",
                        {{"voice", settings.speech_voice},
                         {"base_url", settings.speech_base_url},
                         {"model", settings.speech_model},
                         {"rate", settings.speech_rate},
                         {"pitch", settings.speech_pitch}}},
                       {"overlay",
                        {{"ring", settings.overlay_ring_color},
                         {"dot", settings.overlay_dot_color},
                         {"caption", settings.overlay_caption_color}}},
                       {"capture",
                        {{"exclusions", settings.capture_exclusions},
                         {"send_preview", settings.capture_send_preview}}}};
    return root.dump(2);
}

Settings load(const std::filesystem::path& file, std::vector<std::string>* warnings) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return defaults();   // first run: no file yet
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return from_json(buffer.str(), warnings);
}

bool save(const std::filesystem::path& file, const Settings& settings) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);

    // Write to a temporary file and rename it over the real one, so a crash mid-write
    // cannot leave a half-written settings file behind.
    std::filesystem::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << to_json(settings);
        if (!out) return false;
    }
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

}  // namespace buddy::settings
