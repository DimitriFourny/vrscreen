#include "decoder.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>

#include "util.h"

namespace vr {

using Microsoft::WRL::ComPtr;

struct Decoder::Impl {
    ComPtr<IMFTransform> mft;
    std::string name;
    proto::ConfigMessage config;
    int width = 0, height = 0, stride = 0;
    bool configured = false;

    bool setOutputType() {
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> type;
            if (FAILED(mft->GetOutputAvailableType(0, i, &type))) return false;
            GUID sub{};
            type->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub != MFVideoFormat_NV12) continue;
            if (FAILED(mft->SetOutputType(0, type.Get(), 0))) return false;
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
            width = int(w);
            height = int(h);
            UINT32 s = 0;
            stride = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)) ? int(s) : width;
            return true;
        }
    }
};

Decoder::Decoder() : impl_(std::make_unique<Impl>()) {}
Decoder::~Decoder() = default;
std::string Decoder::name() const { return impl_->name; }

bool Decoder::configure(const proto::ConfigMessage& config, std::string& error) {
    auto& d = *impl_;
    d.mft.Reset();
    d.configured = false;
    d.config = config;
    const GUID subtype = config.codec == proto::Codec::HEVC ? MFVideoFormat_HEVC : MFVideoFormat_H264;
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, subtype};
    MFT_REGISTER_TYPE_INFO out{MFMediaType_Video, MFVideoFormat_NV12};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out,
                           &activates, &count);
    if (FAILED(hr) || count == 0) {
        error = std::string("no software ") + proto::codecName(config.codec) + " decoder on this PC" +
                (config.codec == proto::Codec::HEVC ? " (install \"HEVC Video Extensions\" or stream H.264)" : "");
        if (activates) CoTaskMemFree(activates);
        return false;
    }
    wchar_t* name = nullptr;
    UINT32 len = 0;
    if (SUCCEEDED(activates[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len))) {
        d.name = narrow(std::wstring_view(name, len));
        CoTaskMemFree(name);
    }
    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&d.mft));
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    if (FAILED(hr)) {
        error = "activate decoder: " + hresultText(hr);
        return false;
    }
    ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(d.mft->GetAttributes(&attributes))) attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    ComPtr<IMFMediaType> type;
    MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, config.width, config.height);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = d.mft->SetInputType(0, type.Get(), 0);
    if (FAILED(hr)) {
        error = "decoder input type: " + hresultText(hr);
        return false;
    }
    if (!d.setOutputType()) {
        error = "decoder has no NV12 output";
        return false;
    }
    d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    d.configured = true;
    return true;
}

bool Decoder::decode(const proto::Bytes& au, uint64_t micros, std::vector<DecodedPicture>& out, std::string& error) {
    auto& d = *impl_;
    if (!d.configured) return false;
    ComPtr<IMFMediaBuffer> buffer;
    MFCreateMemoryBuffer(DWORD(au.size()), &buffer);
    BYTE* p = nullptr;
    buffer->Lock(&p, nullptr, nullptr);
    memcpy(p, au.data(), au.size());
    buffer->Unlock();
    buffer->SetCurrentLength(DWORD(au.size()));
    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(LONGLONG(micros) * 10);
    HRESULT hr = d.mft->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) {
        error = "decoder ProcessInput: " + hresultText(hr);
        return false;
    }
    for (;;) {
        MFT_OUTPUT_STREAM_INFO info{};
        d.mft->GetOutputStreamInfo(0, &info);
        MFT_OUTPUT_DATA_BUFFER ob{};
        ComPtr<IMFSample> outSample;
        const bool provides = info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
        if (!provides) {
            ComPtr<IMFMediaBuffer> mem;
            MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, DWORD(d.width * d.height * 3 / 2)), &mem);
            MFCreateSample(&outSample);
            outSample->AddBuffer(mem.Get());
            ob.pSample = outSample.Get();
        }
        DWORD status = 0;
        hr = d.mft->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (!d.setOutputType()) {
                error = "decoder format change failed";
                return false;
            }
            continue;
        }
        if (FAILED(hr)) {
            error = "decoder ProcessOutput: " + hresultText(hr);
            return false;
        }
        ComPtr<IMFMediaBuffer> contiguous;
        if (ob.pSample && SUCCEEDED(ob.pSample->ConvertToContiguousBuffer(&contiguous))) {
            DecodedPicture pic;
            pic.width = int(d.config.width);
            pic.height = int(d.config.height);
            BYTE* data = nullptr;
            DWORD length = 0;
            contiguous->Lock(&data, nullptr, &length);
            const int stride = std::max(d.stride, d.width);
            // The decoder's buffer may be padded (aligned height): copy the visible part.
            const int alignedH = stride > 0 ? int(length) * 2 / 3 / stride : d.height;
            pic.nv12.resize(size_t(pic.width) * pic.height * 3 / 2);
            double sum = 0;
            for (int y = 0; y < pic.height && size_t(y) * stride + pic.width <= length; ++y) {
                memcpy(&pic.nv12[size_t(y) * pic.width], data + size_t(y) * stride, size_t(pic.width));
                for (int x = 0; x < pic.width; x += 8) sum += data[size_t(y) * stride + x];
            }
            for (int y = 0; y < pic.height / 2; ++y) {
                const size_t src = size_t(alignedH + y) * stride;
                if (src + pic.width > length) break;
                memcpy(&pic.nv12[size_t(pic.width) * pic.height + size_t(y) * pic.width], data + src, size_t(pic.width));
            }
            contiguous->Unlock();
            pic.meanLuma = sum / (double(pic.height) * ((pic.width + 7) / 8));
            LONGLONG time = 0;
            pic.micros = SUCCEEDED(ob.pSample->GetSampleTime(&time)) ? uint64_t(time / 10) : micros;
            out.push_back(std::move(pic));
        }
        if (provides && ob.pSample) ob.pSample->Release();
    }
}

bool writePng(const DecodedPicture& pic, const std::wstring& path, std::string& error) {
    std::vector<uint8_t> bgr(size_t(pic.width) * pic.height * 3);
    const uint8_t* yPlane = pic.nv12.data();
    const uint8_t* uv = pic.nv12.data() + size_t(pic.width) * pic.height;
    for (int y = 0; y < pic.height; ++y) {
        for (int x = 0; x < pic.width; ++x) {
            const float Y = (yPlane[size_t(y) * pic.width + x] - 16) * (255.0f / 219.0f);
            const size_t c = size_t(y / 2) * pic.width + size_t(x / 2) * 2;
            const float U = (uv[c] - 128) * (255.0f / 224.0f), V = (uv[c + 1] - 128) * (255.0f / 224.0f);
            const float r = Y + 1.5748f * V, g = Y - 0.1873f * U - 0.4681f * V, b = Y + 1.8556f * U;
            uint8_t* o = &bgr[(size_t(y) * pic.width + x) * 3];
            o[0] = uint8_t(std::clamp(b, 0.0f, 255.0f));
            o[1] = uint8_t(std::clamp(g, 0.0f, 255.0f));
            o[2] = uint8_t(std::clamp(r, 0.0f, 255.0f));
        }
    }
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapFrameEncode> frame;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(UINT(pic.width), UINT(pic.height));
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr)) hr = frame->WritePixels(UINT(pic.height), UINT(pic.width * 3), UINT(bgr.size()), bgr.data());
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    if (FAILED(hr)) {
        error = "PNG: " + hresultText(hr);
        return false;
    }
    return true;
}

}  // namespace vr
