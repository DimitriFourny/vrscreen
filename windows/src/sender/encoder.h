// Hardware video encoder through Media Foundation (NVIDIA NVENC, AMD AMF/VCN, Intel Quick Sync
// all ship a hardware MFT): HEVC Main or H.264 High, no B-frames, low-latency mode, encoding
// straight from the NV12 GPU texture of the capture (no copy to system memory).
// Not thread-safe: use from one thread (the pipeline's encoder thread).
#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>

#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "d3d.h"
#include "protocol.h"

namespace vr {

struct EncoderSettings {
    proto::Codec codec = proto::Codec::HEVC;
    double bitrateMbps = 30;
    int fps = 60;
    double keyframeIntervalSeconds = 5;
    std::string preferredName;  // substring of the MFT's name, e.g. "NVIDIA"
    bool operator==(const EncoderSettings&) const = default;
};

struct EncodedOutput {
    proto::ConfigMessage config;
    proto::FrameMessage frame;
};

class MfEventQueue;

class VideoEncoder {
public:
    explicit VideoEncoder(EncoderSettings settings);
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    const EncoderSettings& settings() const { return settings_; }
    // Creates (or recreates, for another size or device) the session for frames like this one.
    bool prepare(const VideoFrame& frame, std::string& error);
    // Encodes one frame and waits for its output (normally a few ms). `out` stays empty if the
    // encoder produced nothing for it.
    bool encode(const VideoFrame& frame, bool forceKeyframe, std::optional<EncodedOutput>& out, std::string& error);
    void shutdown();

private:
    bool createSession(const VideoFrame& frame, std::string& error);
    bool configure(std::string& error);
    bool waitEvent(DWORD timeoutMs, std::string& error);
    bool processOutput(std::string& error);
    bool handleOutputSample(IMFSample* sample, std::string& error);
    void setCodecValue(const GUID& key, UINT32 value, const char* name, bool required = false);

    EncoderSettings settings_;
    std::shared_ptr<GpuDevice> gpu_;
    int width_ = 0, height_ = 0;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> deviceManager_;
    UINT resetToken_ = 0;
    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    Microsoft::WRL::ComPtr<ICodecAPI> codecApi_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> events_;
    Microsoft::WRL::ComPtr<MfEventQueue> queue_;
    bool async_ = false;
    bool providesSamples_ = true;
    DWORD outputBufferSize_ = 0;
    int needInput_ = 0;
    std::string name_;
    int64_t lastSampleTime_ = -1;
    struct InputInfo {
        uint64_t captureMicros;
    };
    std::deque<InputInfo> inputs_;
    std::vector<EncodedOutput> outputs_;
    proto::Bytes parameterSets_;
    bool loggedMissingParameterSets_ = false;
};

}  // namespace vr
