#include "platform/wasapi_recorder.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <windows.h>

namespace buddy {
namespace {

template <typename T>
void release(T*& value) {
    if (value) value->Release();
    value = nullptr;
}

bool is_subformat(const WAVEFORMATEX* format, const GUID& guid) {
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return guid == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    if (format->wFormatTag == WAVE_FORMAT_PCM) return guid == KSDATAFORMAT_SUBTYPE_PCM;
    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE || format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return false;
    }
    return static_cast<const WAVEFORMATEXTENSIBLE*>(static_cast<const void*>(format))->SubFormat == guid;
}

std::int16_t to_pcm16(float value) {
    value = std::clamp(value, -1.0f, 1.0f);
    return static_cast<std::int16_t>(std::lround(value * 32767.0f));
}

}  // namespace

WasapiRecorder::WasapiRecorder(Options options) : options_(options) {}

WasapiRecorder::~WasapiRecorder() {
    stop();
}

bool WasapiRecorder::start() {
    std::lock_guard lock(mutex_);
    if (recording_ || thread_.joinable()) return false;
    if (options_.max_duration_ms < 100 || options_.max_duration_ms > 300'000) {
        error_ = "The microphone recording length is outside the allowed range.";
        return false;
    }
    samples_.clear();
    error_.clear();
    stop_requested_ = false;
    recording_ = true;
    thread_ = std::thread(&WasapiRecorder::capture, this);
    return true;
}

std::vector<std::int16_t> WasapiRecorder::stop() {
    {
        std::lock_guard lock(mutex_);
        stop_requested_ = true;
    }
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(mutex_);
    recording_ = false;
    return std::exchange(samples_, {});
}

bool WasapiRecorder::recording() const {
    std::lock_guard lock(mutex_);
    return recording_;
}

int WasapiRecorder::sample_rate() const {
    std::lock_guard lock(mutex_);
    return sample_rate_;
}

std::string WasapiRecorder::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

void WasapiRecorder::capture() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::lock_guard lock(mutex_);
        error_ = "The microphone could not be opened (COM did not start).";
        recording_ = false;
        return;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX* format = nullptr;
    HANDLE event = nullptr;
    auto fail = [&](const char* message) {
        std::lock_guard lock(mutex_);
        if (error_.empty()) error_ = message;
    };

    do {
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) { fail("Windows audio devices could not be listed."); break; }
        hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
        if (FAILED(hr)) {
            fail("No microphone was found. Connect one, or choose a default input device in Windows sound settings.");
            break;
        }
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
        if (FAILED(hr)) { fail("The microphone could not be opened."); break; }
        hr = client->GetMixFormat(&format);
        if (FAILED(hr) || !format) { fail("The microphone audio format could not be read."); break; }
        // The loop below mixes every channel down to one. Laptop microphone arrays often expose
        // four channels, so more than two is normal.
        if (format->nChannels < 1 || format->nChannels > 16 || format->nSamplesPerSec < 8'000 ||
            format->nSamplesPerSec > 192'000) {
            fail("The microphone audio format is not supported.");
            break;
        }
        {
            std::lock_guard lock(mutex_);
            sample_rate_ = static_cast<int>(format->nSamplesPerSec);
        }
        const bool float_format = is_subformat(format, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        const bool pcm_format = is_subformat(format, KSDATAFORMAT_SUBTYPE_PCM);
        if ((!float_format && !pcm_format) || (pcm_format && format->wBitsPerSample != 16) ||
            (float_format && format->wBitsPerSample != 32)) {
            fail("The microphone audio format is not supported (16-bit PCM or 32-bit float is needed).");
            break;
        }
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) { fail("The microphone could not be opened."); break; }
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, format, nullptr);
        if (hr == E_ACCESSDENIED) {
            fail("Windows blocked the microphone. Allow desktop apps to use it under Settings > Privacy & security > "
                 "Microphone.");
            break;
        }
        if (FAILED(hr)) { fail("The microphone could not be opened."); break; }
        hr = client->SetEventHandle(event);
        if (FAILED(hr)) { fail("The microphone could not be opened."); break; }
        hr = client->GetService(IID_PPV_ARGS(&capture));
        if (FAILED(hr)) { fail("The microphone could not be opened."); break; }
        hr = client->Start();
        if (FAILED(hr)) { fail("The microphone could not be started."); break; }

        const auto deadline = GetTickCount64() + static_cast<ULONGLONG>(options_.max_duration_ms);
        const int channels = format->nChannels;
        std::vector<std::int16_t> packet;   // one packet, converted outside the lock
        bool failed = false;
        while (!failed && GetTickCount64() < deadline) {
            {
                std::lock_guard lock(mutex_);
                if (stop_requested_) break;
            }
            const DWORD wait = WaitForSingleObject(event, 100);
            if (wait == WAIT_FAILED) { fail("The microphone stopped working."); break; }
            if (wait == WAIT_TIMEOUT) continue;

            // Take everything that is ready. A failure here ends the recording: the usual cause
            // is the microphone being unplugged or disabled, and that does not recover.
            for (;;) {
                UINT32 packet_frames = 0;
                hr = capture->GetNextPacketSize(&packet_frames);
                if (FAILED(hr)) { fail("The microphone was disconnected."); failed = true; break; }
                if (packet_frames == 0) break;

                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) { fail("The microphone was disconnected."); failed = true; break; }

                packet.assign(frames, 0);   // a packet flagged silent stays all zeroes
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0) {
                    for (UINT32 frame = 0; frame < frames; ++frame) {
                        float mixed = 0.0f;
                        for (int channel = 0; channel < channels; ++channel) {
                            const std::size_t index = static_cast<std::size_t>(frame) * channels + channel;
                            if (float_format) {
                                mixed += reinterpret_cast<const float*>(data)[index];
                            } else {
                                mixed += reinterpret_cast<const std::int16_t*>(data)[index] / 32768.0f;
                            }
                        }
                        packet[frame] = to_pcm16(mixed / static_cast<float>(channels));
                    }
                }
                capture->ReleaseBuffer(frames);

                std::lock_guard lock(mutex_);
                samples_.insert(samples_.end(), packet.begin(), packet.end());
            }
        }
        client->Stop();
    } while (false);

    if (event) CloseHandle(event);
    if (format) CoTaskMemFree(format);
    release(capture);
    release(client);
    release(device);
    release(enumerator);
    CoUninitialize();
    std::lock_guard lock(mutex_);
    recording_ = false;
}

}  // namespace buddy
