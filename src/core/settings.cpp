#include "core/settings.h"

#include <fstream>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

#include "core/hotkey.h"
#include "core/placement.h"
#include "core/protocol.h"
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
    // Both entries use the same OpenAI-compatible client (STATUS.md D10).
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

    s.providers = {anthropic, openai};
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
        const placement::Size clamped = placement::clamp_panel_size({s.panel_width, s.panel_height});
        s.panel_width = clamped.width;
        s.panel_height = clamped.height;
    }
    if (const auto it = root.find("muted"); it != root.end() && it->is_boolean()) s.muted = it->get<bool>();

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
                         {"pinned", settings.panel_pinned}}},
                       {"muted", settings.muted}};
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
