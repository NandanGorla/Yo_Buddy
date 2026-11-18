#include "platform/sapi_voice.h"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <utility>

#include <windows.h>

#include <mmsystem.h>
#include <sapi.h>

#include <nlohmann/json.hpp>

#include "core/log.h"
#include "core/text.h"
#include "core/url.h"
#include "core/wav.h"
#include "platform/win_text.h"

namespace buddy {
namespace {

// A neural server that sends more than this is misbehaving; a spoken caption is far smaller.
constexpr std::size_t kMaxNeuralAudioBytes = 32u * 1024u * 1024u;

std::wstring xml_escape(std::wstring_view text) {
    std::wstring result;
    result.reserve(text.size());
    for (const wchar_t c : text) {
        switch (c) {
        case L'&': result += L"&amp;"; break;
        case L'<': result += L"&lt;"; break;
        case L'>': result += L"&gt;"; break;
        case L'"': result += L"&quot;"; break;
        case L'\'': result += L"&apos;"; break;
        default: result += c; break;
        }
    }
    return result;
}

bool same_name(const wchar_t* a, const std::wstring& b) {
    return a && CompareStringOrdinal(a, -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// True when `token` is the voice the user named. Both the short name ("Microsoft Zira Desktop",
// stored under the token's Attributes key) and the full description shown in Windows settings
// ("Microsoft Zira Desktop - English (United States)") are accepted.
bool token_matches(ISpObjectToken* token, const std::wstring& wanted) {
    bool matches = false;
    WCHAR* description = nullptr;
    if (SUCCEEDED(token->GetStringValue(nullptr, &description))) matches = same_name(description, wanted);
    if (description) CoTaskMemFree(description);
    if (matches) return true;

    ISpDataKey* attributes = nullptr;
    if (SUCCEEDED(token->OpenKey(L"Attributes", &attributes)) && attributes) {
        WCHAR* name = nullptr;
        if (SUCCEEDED(attributes->GetStringValue(L"Name", &name))) matches = same_name(name, wanted);
        if (name) CoTaskMemFree(name);
        attributes->Release();
    }
    return matches;
}

// Switches `voice` to the installed voice called `name`. False when there is no such voice.
bool select_voice(ISpVoice* voice, const std::string& name) {
    const std::wstring wanted = win::widen(name);
    // The classic desktop voices, then the newer "OneCore" ones that Windows settings installs.
    const wchar_t* categories[] = {SPCAT_VOICES, L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices"};
    for (const wchar_t* category_id : categories) {
        ISpObjectTokenCategory* category = nullptr;
        IEnumSpObjectTokens* tokens = nullptr;
        bool selected = false;
        if (SUCCEEDED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&category))) &&
            SUCCEEDED(category->SetId(category_id, FALSE)) &&
            SUCCEEDED(category->EnumTokens(nullptr, nullptr, &tokens)) && tokens) {
            ISpObjectToken* token = nullptr;
            while (!selected && tokens->Next(1, &token, nullptr) == S_OK && token) {
                selected = token_matches(token, wanted) && SUCCEEDED(voice->SetVoice(token));
                token->Release();
                token = nullptr;
            }
        }
        if (tokens) tokens->Release();
        if (category) category->Release();
        if (selected) return true;
    }
    return false;
}

void speak_with_windows_voice(const std::string& text, const SapiVoice::Config& config, std::stop_token stop) {
    // The voice object is created, used and released on this thread only. stop() never touches
    // it: this loop notices the stop request itself.
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        log::warn("speech: COM could not be initialised");
        return;
    }
    ISpVoice* voice = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&voice))) && voice) {
        if (!config.voice.empty() && !select_voice(voice, config.voice)) {
            log::warn("speech: the chosen Windows voice is not installed; using the default voice");
        }
        voice->SetRate(std::clamp(config.rate, -10, 10));

        // SPF_IS_NOT_XML matters: without it SAPI treats text that starts with '<' as markup,
        // and a reply that merely mentions a tag would be swallowed or refused.
        std::wstring spoken = win::widen(text);
        DWORD flags = SPF_ASYNC | SPF_IS_NOT_XML;
        const int pitch = std::clamp(config.pitch, -10, 10);
        if (pitch != 0) {
            // SAPI's own markup: <pitch absmiddle> takes the same -10..10 scale as the setting.
            spoken = L"<pitch absmiddle=\"" + std::to_wstring(pitch) + L"\">" + xml_escape(spoken) + L"</pitch>";
            flags = SPF_ASYNC | SPF_IS_XML;
        }
        if (SUCCEEDED(voice->Speak(spoken.c_str(), flags, nullptr))) {
            while (voice->WaitUntilDone(40) == S_FALSE) {   // S_FALSE: still speaking
                if (stop.stop_requested()) {
                    voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
                    break;
                }
            }
        } else {
            log::warn("speech: the Windows voice could not speak");
        }
        voice->Release();
    } else {
        log::warn("speech: no Windows voice is available");
    }
    CoUninitialize();
}

}  // namespace

SapiVoice::SapiVoice(IHttpTransport* transport) : transport_(transport) {}

SapiVoice::~SapiVoice() {
    stop();
}

void SapiVoice::configure(Config config) {
    config_ = std::move(config);
}

void SapiVoice::set_muted(bool muted) {
    muted_ = muted;
    if (muted) stop();
}

void SapiVoice::speak(std::string text) {
    stop();
    text = text::truncate_utf8(text, kMaxSpokenBytes);
    if (muted_ || text.empty()) return;

    stop_source_ = std::stop_source();
    thread_ = std::thread([this, text = std::move(text), config = config_, stop = stop_source_.get_token()] {
        run(text, config, stop);
    });
}

void SapiVoice::stop() {
    if (!thread_.joinable()) return;
    // The worker watches the stop token in every phase: the download, a playing clip, and a
    // Windows voice speaking. It silences whatever it started and returns.
    stop_source_.request_stop();
    thread_.join();
}

void SapiVoice::run(const std::string& text, const Config& config, std::stop_token stop) {
    if (transport_ && !config.neural_base_url.empty() && !config.neural_model.empty()) {
        if (speak_neural(text, config, stop)) return;
    }
    if (stop.stop_requested()) return;
    speak_with_windows_voice(text, config, stop);
}

// True when there is nothing left to say: the clip was played, or speaking was stopped.
// False when the neural server did not deliver and a Windows voice should take over.
bool SapiVoice::speak_neural(const std::string& text, const Config& config, std::stop_token stop) {
    const url::BaseUrlResult parsed = url::parse_base_url(config.neural_base_url);
    if (!parsed.ok() || !url::is_loopback_host(parsed.url->host)) {
        log::warn("speech: the neural speech URL is not a valid address on this PC; using a Windows voice");
        return false;
    }

    HttpRequest request;
    request.url = parsed.url->to_string() + "/audio/speech";
    request.headers = {{"Content-Type", "application/json"}, {"Accept", "audio/wav"}};
    request.body = nlohmann::json{{"model", config.neural_model}, {"input", text}, {"response_format", "wav"}}.dump();

    std::string audio;
    bool too_large = false;
    int status = 0;
    const HttpResponse response = transport_->send(
        request,
        {[&](int code, std::string_view) { status = code; },
         [&](std::string_view chunk) {
             if (audio.size() + chunk.size() > kMaxNeuralAudioBytes) too_large = true;
             else audio.append(chunk);
         }},
        stop);
    if (stop.stop_requested() || response.outcome == HttpOutcome::Cancelled) return true;
    const auto length_ms = wav::duration_ms(
        {reinterpret_cast<const std::uint8_t*>(audio.data()), audio.size()});
    if (response.outcome != HttpOutcome::Completed || status < 200 || status >= 300 || too_large || !length_ms) {
        log::warn("speech: the neural speech server did not return WAV audio (HTTP " + std::to_string(status) +
                  "); using a Windows voice");
        return false;
    }

    // Played asynchronously, with this thread waiting out the clip. A synchronous PlaySound
    // cannot be interrupted from another thread (measured: the stop call simply waits for the
    // clip to end), which would make stop() block the UI thread.
    if (!PlaySoundA(audio.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT)) {
        log::warn("speech: Windows could not play the neural speech clip; using a Windows voice");
        return false;
    }
    const ULONGLONG ends_at = GetTickCount64() + *length_ms + 150;   // a little room for the device to start
    while (GetTickCount64() < ends_at && !stop.stop_requested()) Sleep(20);
    // Always, even after a full play: `audio` must not be released while Windows may still read it.
    PlaySoundA(nullptr, nullptr, 0);
    return true;
}

}  // namespace buddy
