#include "audio_capture.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmsystem.h>
#include <wrl/client.h>

#include <algorithm>

#include "log.h"
#include "util.h"

namespace vr {

using Microsoft::WRL::ComPtr;

// MARK: AudioSink

void AudioSink::setEnabled(bool on) {
    if (enabled_ == on) return;
    enabled_ = on;
    if (!on) {
        std::lock_guard lock(mutex_);
        if (chunker_) chunker_->reset();
    }
    std::lock_guard lock(mutex_);
    server_.audioConfig = on ? std::optional(config_) : std::nullopt;
}

void AudioSink::push(const float* stereo, int frames, int sampleRate, uint64_t micros) {
    if (!enabled_ || frames <= 0) return;
    std::vector<proto::AudioMessage> chunks;
    proto::AudioConfigMessage config;
    {
        std::lock_guard lock(mutex_);
        if (!chunker_ || chunker_->sampleRate() != sampleRate) {
            chunker_ = std::make_unique<AudioChunker>(sampleRate, 2, sampleRate / 100);
            config_ = proto::AudioConfigMessage{proto::AudioConfigMessage::kPcmS16LE, uint32_t(sampleRate), 2};
        }
        chunks = chunker_->append(frames, micros, [&](int f, int c) { return stereo[2 * f + c]; });
        config = config_;
    }
    if (chunks.empty()) return;
    loop_.post([this, chunks = std::move(chunks), config]() mutable {
        if (!enabled_) return;
        if (server_.audioConfig != config) server_.audioConfig = config;
        for (auto& chunk : chunks) server_.sendAudio(std::move(chunk));
    });
}

void AudioSink::noteDelivery(double ageMs) {
    std::lock_guard lock(mutex_);
    if (ages_.size() < 10000) ages_.push_back(ageMs);
}

std::string AudioSink::takeStats() {
    std::lock_guard lock(mutex_);
    if (ages_.empty()) return {};
    std::sort(ages_.begin(), ages_.end());
    // Negative: WASAPI stamps loopback audio with its playback time, slightly ahead of delivery.
    const std::string s = formatString("loopback delivery p50 %+.1f max %+.1f ms", ages_[ages_.size() / 2], ages_.back());
    ages_.clear();
    return s;
}

// MARK: LoopbackCapture

void LoopbackCapture::start() {
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void LoopbackCapture::stop() {
    stopping_ = true;
    if (thread_.joinable()) thread_.join();
}

void LoopbackCapture::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    std::string lastError;
    while (!stopping_) {
        std::string error;
        if (!runDevice(error) && !stopping_) {
            if (error != lastError) {
                log("audio: " + error + "; retrying");
                if (onStatus) onStatus(error);
                lastError = error;
            }
            Sleep(1000);
        } else {
            lastError.clear();
        }
    }
    timeEndPeriod(1);
    CoUninitialize();
}

namespace {
enum class SampleKind { Float32, Int16, Int32, Int24, Unknown };

SampleKind sampleKind(const WAVEFORMATEX* f) {
    GUID sub{};
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) sub = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat;
    const bool isFloat = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || sub == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    const bool isPcm = f->wFormatTag == WAVE_FORMAT_PCM || sub == KSDATAFORMAT_SUBTYPE_PCM;
    if (isFloat && f->wBitsPerSample == 32) return SampleKind::Float32;
    if (isPcm && f->wBitsPerSample == 16) return SampleKind::Int16;
    if (isPcm && f->wBitsPerSample == 32) return SampleKind::Int32;
    if (isPcm && f->wBitsPerSample == 24) return SampleKind::Int24;
    return SampleKind::Unknown;
}

float readSample(const uint8_t* p, SampleKind kind) {
    switch (kind) {
        case SampleKind::Float32: { float v; memcpy(&v, p, 4); return v; }
        case SampleKind::Int16: { int16_t v; memcpy(&v, p, 2); return v / 32768.0f; }
        case SampleKind::Int32: { int32_t v; memcpy(&v, p, 4); return float(v / 2147483648.0); }
        case SampleKind::Int24: { const int32_t v = int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24); return float(v / 2147483648.0); }
        default: return 0;
    }
}

std::string deviceName(IMMDevice* device) {
    ComPtr<IPropertyStore> props;
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string name = "default output";
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) &&
        v.vt == VT_LPWSTR)
        name = narrow(v.pwszVal);
    PropVariantClear(&v);
    return name;
}

std::wstring deviceId(IMMDevice* device) {
    LPWSTR id = nullptr;
    std::wstring out;
    if (SUCCEEDED(device->GetId(&id)) && id) out = id;
    CoTaskMemFree(id);
    return out;
}
}  // namespace

bool LoopbackCapture::runDevice(std::string& error) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        error = "MMDeviceEnumerator: " + hresultText(hr);
        return false;
    }
    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        error = "no audio output device";
        return false;
    }
    const std::wstring id = deviceId(device.Get());
    const std::string name = deviceName(device.Get());

    ComPtr<IAudioClient> capture;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &capture);
    WAVEFORMATEX* format = nullptr;
    if (SUCCEEDED(hr)) hr = capture->GetMixFormat(&format);
    if (FAILED(hr)) {
        error = "audio client for \"" + name + "\": " + hresultText(hr);
        return false;
    }
    std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> formatOwner(format, &CoTaskMemFree);
    const SampleKind kind = sampleKind(format);
    const int channels = format->nChannels;
    const int rate = int(format->nSamplesPerSec);
    const int frameBytes = format->nBlockAlign;
    const int sampleBytes = format->wBitsPerSample / 8;
    const uint32_t mask = format->wFormatTag == WAVE_FORMAT_EXTENSIBLE
                              ? reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->dwChannelMask
                              : 0;
    if (kind == SampleKind::Unknown || channels < 1) {
        error = formatString("unsupported mix format of \"%s\" (%d bit, tag 0x%X)", name.c_str(), format->wBitsPerSample,
                             format->wFormatTag);
        return false;
    }
    hr = capture->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 200'000 /* 20 ms */, 0, format, nullptr);
    ComPtr<IAudioCaptureClient> captureClient;
    if (SUCCEEDED(hr)) hr = capture->GetService(IID_PPV_ARGS(&captureClient));
    if (FAILED(hr)) {
        error = "loopback capture of \"" + name + "\": " + hresultText(hr);
        return false;
    }

    // Silence played to the same device keeps loopback packets coming while nothing plays.
    ComPtr<IAudioClient> render;
    ComPtr<IAudioRenderClient> renderClient;
    UINT32 renderFrames = 0;
    if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &render)) &&
        SUCCEEDED(render->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1'000'000 /* 100 ms */, 0, format, nullptr)) &&
        SUCCEEDED(render->GetBufferSize(&renderFrames)) && SUCCEEDED(render->GetService(IID_PPV_ARGS(&renderClient)))) {
        BYTE* data = nullptr;
        if (SUCCEEDED(renderClient->GetBuffer(renderFrames, &data))) renderClient->ReleaseBuffer(renderFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        render->Start();
    } else {
        renderClient.Reset();
    }

    ComPtr<IAudioEndpointVolume> volume;
    device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume);
    bool mutedByUs = false;
    const auto applyMute = [&](bool want) {
        if (!volume) return;
        if (want && !mutedByUs) {
            BOOL muted = FALSE;
            volume->GetMute(&muted);
            if (!muted && SUCCEEDED(volume->SetMute(TRUE, nullptr))) {
                mutedByUs = true;
                log("audio: muted \"" + name + "\" while the headset plays the sound");
            }
        } else if (!want && mutedByUs) {
            volume->SetMute(FALSE, nullptr);
            mutedByUs = false;
            log("audio: unmuted \"" + name + "\"");
        }
    };

    hr = capture->Start();
    if (FAILED(hr)) {
        error = "start loopback capture: " + hresultText(hr);
        return false;
    }
    logf("audio: capturing \"%s\" (WASAPI loopback), %d Hz, %d ch, %d-bit %s -> s16le stereo, %d-frame chunks%s", name.c_str(),
         rate, channels, format->wBitsPerSample, kind == SampleKind::Float32 ? "float" : "PCM", rate / 100,
         offsetMicros_ ? formatString(", timestamps +%llu ms", offsetMicros_ / 1000).c_str() : "");
    if (onStatus) onStatus("");

    const DownmixMatrix downmix = makeDownmix(channels, mask);
    std::vector<float> stereo;
    ULONGLONG lastDeviceCheck = GetTickCount64();
    bool ok = true;
    while (!stopping_) {
        applyMute(wantMute_);
        // Keep the silent stream fed.
        if (renderClient) {
            UINT32 padding = 0;
            if (SUCCEEDED(render->GetCurrentPadding(&padding)) && padding < renderFrames / 2) {
                BYTE* data = nullptr;
                const UINT32 n = renderFrames - padding;
                if (SUCCEEDED(renderClient->GetBuffer(n, &data))) renderClient->ReleaseBuffer(n, AUDCLNT_BUFFERFLAGS_SILENT);
            }
        }
        UINT32 packet = 0;
        hr = captureClient->GetNextPacketSize(&packet);
        if (FAILED(hr)) {
            error = hr == AUDCLNT_E_DEVICE_INVALIDATED ? "output device \"" + name + "\" went away" : "loopback: " + hresultText(hr);
            ok = false;
            break;
        }
        while (packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 qpc = 0;
            hr = captureClient->GetBuffer(&data, &frames, &flags, nullptr, &qpc);
            if (FAILED(hr)) break;
            stereo.resize(size_t(frames) * 2);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::fill(stereo.begin(), stereo.end(), 0.0f);
            } else {
                for (UINT32 f = 0; f < frames; ++f) {
                    const uint8_t* frame = data + size_t(f) * size_t(frameBytes);
                    float l = 0, r = 0;
                    for (int c = 0; c < channels; ++c) {
                        const float v = readSample(frame + c * sampleBytes, kind);
                        l += v * downmix.left[size_t(c)];
                        r += v * downmix.right[size_t(c)];
                    }
                    stereo[2 * f] = l;
                    stereo[2 * f + 1] = r;
                }
            }
            captureClient->ReleaseBuffer(frames);
            const uint64_t micros = hundredNsToMicros(qpc);
            const double endMicros = double(micros) + double(frames) * 1e6 / rate;
            sink_.noteDelivery((double(nowMicros()) - endMicros) / 1000.0);
            sink_.push(stereo.data(), int(frames), rate, micros + offsetMicros_);
            if (FAILED(captureClient->GetNextPacketSize(&packet))) break;
        }
        // Follow default-device changes (checked once per second).
        if (GetTickCount64() - lastDeviceCheck > 1000) {
            lastDeviceCheck = GetTickCount64();
            ComPtr<IMMDevice> current;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &current)) && deviceId(current.Get()) != id) {
                log("audio: default output changed from \"" + name + "\" to \"" + deviceName(current.Get()) + "\"");
                break;
            }
        }
        Sleep(3);
    }
    applyMute(false);
    capture->Stop();
    if (render) render->Stop();
    return ok;
}

}  // namespace vr
