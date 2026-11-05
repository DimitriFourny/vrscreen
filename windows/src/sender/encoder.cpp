#include "encoder.h"

#include <codecapi.h>
#include <mferror.h>
#include <wrl/implements.h>

#include <algorithm>

#include "log.h"
#include "nal.h"
#include "util.h"

namespace vr {

using Microsoft::WRL::ComPtr;

// Receives an async MFT's events (BeginGetEvent) on a Media Foundation work queue thread and
// hands them to the encoder thread.
class MfEventQueue
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFAsyncCallback> {
public:
    MfEventQueue() { signal_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); }
    ~MfEventQueue() override { CloseHandle(signal_); }

    void start(IMFMediaEventGenerator* generator) {
        generator_ = generator;
        generator_->BeginGetEvent(this, nullptr);
    }
    void stop() {
        std::lock_guard lock(mutex_);
        stopped_ = true;
    }
    // Pops one event; waits at most `timeoutMs`. False on timeout.
    bool pop(DWORD timeoutMs, MediaEventType& type, HRESULT& status) {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        for (;;) {
            {
                std::lock_guard lock(mutex_);
                if (!events_.empty()) {
                    type = events_.front().first;
                    status = events_.front().second;
                    events_.pop_front();
                    return true;
                }
            }
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) return false;
            WaitForSingleObject(signal_, DWORD(deadline - now));
        }
    }

    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult* result) override {
        ComPtr<IMFMediaEvent> event;
        const HRESULT hr = generator_->EndGetEvent(result, &event);
        MediaEventType type = MEError;
        HRESULT status = hr;
        if (SUCCEEDED(hr) && event) {
            event->GetType(&type);
            event->GetStatus(&status);
        }
        bool again;
        {
            std::lock_guard lock(mutex_);
            events_.emplace_back(type, status);
            again = !stopped_ && SUCCEEDED(hr) && type != MEError;
        }
        SetEvent(signal_);
        if (again) generator_->BeginGetEvent(this, nullptr);
        return S_OK;
    }

private:
    IMFMediaEventGenerator* generator_ = nullptr;  // owned by the encoder
    HANDLE signal_;
    std::mutex mutex_;
    bool stopped_ = false;
    std::deque<std::pair<MediaEventType, HRESULT>> events_;
};

namespace {
constexpr DWORD kEventTimeoutMs = 1000;

void setColorAttributes(IMFMediaType* type) {
    type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
}

std::string activateName(IMFActivate* activate) {
    wchar_t* name = nullptr;
    UINT32 length = 0;
    std::string out = "unknown encoder";
    if (SUCCEEDED(activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &length)) && name) {
        out = narrow(std::wstring_view(name, length));
        CoTaskMemFree(name);
    }
    return out;
}
}  // namespace

VideoEncoder::VideoEncoder(EncoderSettings settings) : settings_(std::move(settings)) {}

VideoEncoder::~VideoEncoder() { shutdown(); }

void VideoEncoder::shutdown() {
    if (queue_) queue_->stop();
    if (transform_) {
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        ComPtr<IMFShutdown> shutdown;
        if (SUCCEEDED(transform_.As(&shutdown))) shutdown->Shutdown();
    }
    // A pending BeginGetEvent completes with an error after Shutdown; give it a moment.
    if (queue_) {
        MediaEventType t;
        HRESULT s;
        queue_->pop(50, t, s);
    }
    codecApi_.Reset();
    events_.Reset();
    transform_.Reset();
    queue_.Reset();
    deviceManager_.Reset();
    gpu_.reset();
    width_ = height_ = 0;
    needInput_ = 0;
    inputs_.clear();
    outputs_.clear();
    parameterSets_.clear();
}

bool VideoEncoder::prepare(const VideoFrame& frame, std::string& error) {
    if (transform_ && frame.gpu == gpu_ && frame.width == width_ && frame.height == height_) return true;
    shutdown();
    if (!createSession(frame, error)) {
        shutdown();
        return false;
    }
    return true;
}

void VideoEncoder::setCodecValue(const GUID& key, UINT32 value, const char* name, bool required) {
    if (!codecApi_) return;
    VARIANT v;
    VariantInit(&v);
    if (key == CODECAPI_AVLowLatencyMode) {
        v.vt = VT_BOOL;
        v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    } else {
        v.vt = VT_UI4;
        v.ulVal = value;
    }
    const HRESULT hr = codecApi_->SetValue(&key, &v);
    if (FAILED(hr) && (required || std::string(name) != "ForceKeyFrame"))
        logf("encoder: %s = %u not supported (%s)%s", name, value, hresultText(hr).c_str(), required ? "" : ", ignored");
}

bool VideoEncoder::createSession(const VideoFrame& frame, std::string& error) {
    gpu_ = frame.gpu;
    width_ = frame.width;
    height_ = frame.height;
    const GUID subtype = settings_.codec == proto::Codec::HEVC ? MFVideoFormat_HEVC : MFVideoFormat_H264;
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, subtype};

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = E_FAIL;
    {
        // Hardware encoders of the capture's adapter (a second GPU's encoder could not read its textures).
        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(MFCreateAttributes(&attributes, 1))) {
            attributes->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&gpu_->luid), sizeof(LUID));
            hr = MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &input,
                          &output, attributes.Get(), &activates, &count);
        }
    }
    if (FAILED(hr) || count == 0) {
        if (activates) CoTaskMemFree(activates);
        activates = nullptr;
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &input, &output,
                       &activates, &count);
    }
    if (FAILED(hr) || count == 0) {
        if (activates) CoTaskMemFree(activates);
        error = std::string("no hardware ") + proto::codecName(settings_.codec) + " encoder on " + gpu_->adapterName +
                (settings_.codec == proto::Codec::HEVC ? " (try H.264 in the Video menu)" : "");
        return false;
    }
    UINT32 chosen = 0;
    std::string names;
    for (UINT32 i = 0; i < count; ++i) {
        const std::string n = activateName(activates[i]);
        names += (i ? ", " : "") + n;
        if (!settings_.preferredName.empty() && containsIgnoreCase(n, settings_.preferredName) &&
            !containsIgnoreCase(activateName(activates[chosen]), settings_.preferredName))
            chosen = i;
    }
    name_ = activateName(activates[chosen]);
    hr = activates[chosen]->ActivateObject(IID_PPV_ARGS(&transform_));
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    if (FAILED(hr)) {
        error = "activate " + name_ + ": " + hresultText(hr);
        return false;
    }

    ComPtr<IMFAttributes> attributes;
    transform_->GetAttributes(&attributes);
    UINT32 isAsync = 0;
    if (attributes && SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync)) && isAsync) {
        attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        async_ = true;
    } else {
        async_ = false;
    }
    UINT32 d3d11Aware = 0;
    if (attributes) attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3d11Aware);
    if (attributes) attributes->SetUINT32(MF_LOW_LATENCY, TRUE);

    hr = MFCreateDXGIDeviceManager(&resetToken_, &deviceManager_);
    if (SUCCEEDED(hr)) hr = deviceManager_->ResetDevice(gpu_->device.Get(), resetToken_);
    if (SUCCEEDED(hr)) hr = transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(deviceManager_.Get()));
    if (FAILED(hr)) {
        error = name_ + ": cannot use the Direct3D 11 device (" + hresultText(hr) + ")" + (d3d11Aware ? "" : ", not D3D11 aware");
        return false;
    }
    transform_.As(&codecApi_);
    if (!configure(error)) return false;

    MFT_OUTPUT_STREAM_INFO info{};
    transform_->GetOutputStreamInfo(0, &info);
    providesSamples_ = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    outputBufferSize_ = std::max<DWORD>(info.cbSize, DWORD(width_) * DWORD(height_) * 3 / 2);

    if (async_) {
        hr = transform_.As(&events_);
        if (FAILED(hr)) {
            error = name_ + ": async MFT without an event generator";
            return false;
        }
        queue_ = Microsoft::WRL::Make<MfEventQueue>();
        queue_->start(events_.Get());
    }
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    logf("encoder: %s %dx%d, %s Mbit/s CBR, %d fps, keyframe every %g s max, low-latency, %s MFT (found: %s)",
         proto::codecName(settings_.codec), width_, height_, formatString("%.1f", settings_.bitrateMbps).c_str(),
         settings_.fps, settings_.keyframeIntervalSeconds, async_ ? "async" : "sync", names.c_str());
    log("encoder: using " + name_ + " on " + gpu_->adapterName);
    return true;
}

bool VideoEncoder::configure(std::string& error) {
    const UINT32 bitrate = UINT32(std::clamp(settings_.bitrateMbps, 0.5, 500.0) * 1e6);
    // Codec properties first: some encoders only take the rate-control mode before the types.
    setCodecValue(CODECAPI_AVLowLatencyMode, 1, "LowLatencyMode");
    setCodecValue(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR, "RateControlMode");
    setCodecValue(CODECAPI_AVEncCommonMeanBitRate, bitrate, "MeanBitRate");
    setCodecValue(CODECAPI_AVEncMPVDefaultBPictureCount, 0, "BPictureCount");
    setCodecValue(CODECAPI_AVEncMPVGOPSize,
                  UINT32(std::max(1.0, settings_.fps * settings_.keyframeIntervalSeconds)), "GOPSize");

    const GUID subtype = settings_.codec == proto::Codec::HEVC ? MFVideoFormat_HEVC : MFVideoFormat_H264;
    ComPtr<IMFMediaType> out;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, subtype);
    out->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, UINT32(width_), UINT32(height_));
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, UINT32(settings_.fps), 1);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, settings_.codec == proto::Codec::HEVC ? UINT32(eAVEncH265VProfile_Main_420_8)
                                                                               : UINT32(eAVEncH264VProfile_High));
    setColorAttributes(out.Get());
    HRESULT hr = transform_->SetOutputType(0, out.Get(), 0);
    if (FAILED(hr)) {
        // Some encoders want an explicit level.
        out->SetUINT32(MF_MT_MPEG2_LEVEL, settings_.codec == proto::Codec::HEVC ? UINT32(eAVEncH265VLevel6_2)
                                                                                 : UINT32(eAVEncH264VLevel5_2));
        hr = transform_->SetOutputType(0, out.Get(), 0);
    }
    if (FAILED(hr)) {
        error = name_ + formatString(": output type %s %dx%d@%d rejected: ", proto::codecName(settings_.codec), width_,
                                     height_, settings_.fps) + hresultText(hr);
        return false;
    }
    // Input: NV12 from the encoder's own list (it carries attributes the encoder expects).
    ComPtr<IMFMediaType> in;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> candidate;
        if (FAILED(transform_->GetInputAvailableType(0, i, &candidate))) break;
        GUID sub{};
        candidate->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_NV12) {
            in = candidate;
            break;
        }
    }
    if (!in) {
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    }
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, UINT32(width_), UINT32(height_));
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, UINT32(settings_.fps), 1);
    setColorAttributes(in.Get());
    hr = transform_->SetInputType(0, in.Get(), 0);
    if (FAILED(hr)) {
        error = name_ + ": NV12 input rejected: " + hresultText(hr);
        return false;
    }
    // Again after the types, for encoders that reset it.
    setCodecValue(CODECAPI_AVEncCommonMeanBitRate, bitrate, "MeanBitRate");
    // Parameter sets, if the encoder publishes them before the first frame.
    ComPtr<IMFMediaType> current;
    if (SUCCEEDED(transform_->GetOutputCurrentType(0, &current))) {
        UINT32 size = 0;
        if (SUCCEEDED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size > 0) {
            proto::Bytes header(size);
            current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, header.data(), size, nullptr);
            // The header has no slice; collect its parameter sets in order.
            proto::Bytes sets;
            for (int wanted : settings_.codec == proto::Codec::HEVC ? std::vector<int>{32, 33, 34} : std::vector<int>{7, 8}) {
                for (const auto& u : nal::split(header.data(), header.size())) {
                    if (nal::type(settings_.codec, u) == wanted) {
                        static constexpr uint8_t kStart[4] = {0, 0, 0, 1};
                        sets.insert(sets.end(), kStart, kStart + 4);
                        sets.insert(sets.end(), u.data, u.data + u.size);
                        break;
                    }
                }
            }
            parameterSets_ = sets;
        }
    }
    return true;
}

bool VideoEncoder::waitEvent(DWORD timeoutMs, std::string& error) {
    MediaEventType type;
    HRESULT status;
    if (!queue_->pop(timeoutMs, type, status)) {
        error = name_ + formatString(": no event from the encoder for %lu ms", timeoutMs);
        return false;
    }
    switch (type) {
        case METransformNeedInput: ++needInput_; return true;
        case METransformHaveOutput: return processOutput(error);
        case METransformDrainComplete:
        case METransformMarker: return true;
        case MEError:
        default:
            if (FAILED(status)) {
                error = name_ + ": encoder error " + hresultText(status);
                return false;
            }
            return true;
    }
}

bool VideoEncoder::processOutput(std::string& error) {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER buffer{};
        ComPtr<IMFSample> allocated;
        if (!providesSamples_) {
            ComPtr<IMFMediaBuffer> mem;
            MFCreateMemoryBuffer(outputBufferSize_, &mem);
            MFCreateSample(&allocated);
            allocated->AddBuffer(mem.Get());
            buffer.pSample = allocated.Get();
        }
        DWORD status = 0;
        const HRESULT hr = transform_->ProcessOutput(0, 1, &buffer, &status);
        if (buffer.pEvents) buffer.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> type;
            if (SUCCEEDED(transform_->GetOutputAvailableType(0, 0, &type))) transform_->SetOutputType(0, type.Get(), 0);
            if (buffer.pSample && providesSamples_) buffer.pSample->Release();
            continue;
        }
        if (FAILED(hr)) {
            if (buffer.pSample && providesSamples_) buffer.pSample->Release();
            error = name_ + ": ProcessOutput " + hresultText(hr);
            return false;
        }
        bool ok = true;
        if (buffer.pSample) {
            ok = handleOutputSample(buffer.pSample, error);
            if (providesSamples_) buffer.pSample->Release();
        }
        // Async MFTs produce one output per METransformHaveOutput.
        if (async_ || !ok) return ok;
    }
}

bool VideoEncoder::handleOutputSample(IMFSample* sample, std::string& error) {
    ComPtr<IMFMediaBuffer> contiguous;
    if (FAILED(sample->ConvertToContiguousBuffer(&contiguous))) {
        error = name_ + ": output without data";
        return false;
    }
    BYTE* data = nullptr;
    DWORD length = 0;
    if (FAILED(contiguous->Lock(&data, nullptr, &length))) {
        error = name_ + ": cannot read the output";
        return false;
    }
    nal::AccessUnit au;
    std::string rebuildError;
    const bool ok = nal::rebuild(settings_.codec, data, length, parameterSets_, au, rebuildError);
    contiguous->Unlock();
    const uint64_t captureMicros = inputs_.empty() ? nowMicros() : inputs_.front().captureMicros;
    if (!inputs_.empty()) inputs_.pop_front();
    if (!ok) {
        if (!loggedMissingParameterSets_) log("encoder: " + rebuildError + "; output skipped");
        loggedMissingParameterSets_ = true;
        if (rebuildError.find("parameter sets") != std::string::npos) {
            error = name_ + ": " + rebuildError;
            return false;
        }
        return true;
    }
    if (!au.parameterSets.empty()) parameterSets_ = au.parameterSets;
    EncodedOutput out;
    out.config.codec = settings_.codec;
    out.config.width = uint32_t(width_);
    out.config.height = uint32_t(height_);
    out.config.parameterSets = parameterSets_;
    out.frame.keyframe = au.keyframe;
    out.frame.captureMicros = captureMicros;
    out.frame.accessUnit = std::move(au.data);
    outputs_.push_back(std::move(out));
    return true;
}

bool VideoEncoder::encode(const VideoFrame& frame, bool forceKeyframe, std::optional<EncodedOutput>& result,
                          std::string& error) {
    result.reset();
    if (!prepare(frame, error)) return false;

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), frame.nv12.get(), 0, FALSE, &buffer);
    if (FAILED(hr)) {
        error = "MFCreateDXGISurfaceBuffer: " + hresultText(hr);
        return false;
    }
    ComPtr<IMF2DBuffer> buffer2d;
    DWORD length = 0;
    if (SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->GetContiguousLength(&length))) buffer->SetCurrentLength(length);
    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buffer.Get());
    // Timestamps must increase; a re-encode of the same capture (keyframe for a new client)
    // reuses its capture time.
    const int64_t time = std::max<int64_t>(int64_t(frame.captureMicros) * 10, lastSampleTime_ + 1);
    lastSampleTime_ = time;
    sample->SetSampleTime(time);
    sample->SetSampleDuration(10'000'000 / std::max(1, settings_.fps));

    if (async_) {
        while (needInput_ == 0) {
            if (!waitEvent(kEventTimeoutMs, error)) return false;
        }
    }
    if (forceKeyframe) setCodecValue(CODECAPI_AVEncVideoForceKeyFrame, 1, "ForceKeyFrame");
    hr = transform_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING && !async_) {
        if (!processOutput(error)) return false;
        hr = transform_->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) {
        error = name_ + ": ProcessInput " + hresultText(hr);
        return false;
    }
    if (async_) --needInput_;
    inputs_.push_back({frame.captureMicros});

    if (async_) {
        while (outputs_.empty()) {
            if (!waitEvent(kEventTimeoutMs, error)) return false;
        }
    } else if (!processOutput(error)) {
        return false;
    }
    if (!outputs_.empty()) {
        // Normally exactly one; with several, the newest carries the latest picture and the
        // older ones are sent first by the caller asking again (not expected in low-latency mode).
        result = std::move(outputs_.front());
        outputs_.erase(outputs_.begin());
    }
    return true;
}

}  // namespace vr
