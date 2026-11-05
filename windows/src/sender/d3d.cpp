#include "d3d.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <map>

#include "log.h"
#include "util.h"

namespace vr {

std::shared_ptr<GpuDevice> createGpuDevice(IDXGIAdapter1* adapter, std::string& error) {
    auto gpu = std::make_shared<GpuDevice>();
    gpu->adapter = adapter;
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    gpu->luid = desc.AdapterLuid;
    gpu->adapterName = narrow(desc.Description);
    gpu->vendorId = desc.VendorId;
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
                                   &gpu->device, nullptr, &gpu->context);
    if (FAILED(hr)) {
        error = "D3D11CreateDevice on " + gpu->adapterName + ": " + hresultText(hr);
        return nullptr;
    }
    gpu->device.As(&gpu->videoDevice);
    gpu->context.As(&gpu->videoContext);
    if (!gpu->videoDevice || !gpu->videoContext) {
        error = gpu->adapterName + " has no Direct3D 11 video support";
        return nullptr;
    }
    if (SUCCEEDED(gpu->device.As(&gpu->multithread))) gpu->multithread->SetMultithreadProtected(TRUE);
    return gpu;
}

std::string DisplayInfo::label() const {
    std::string s = friendlyName + formatString(" (%dx%d", width, height);
    if (refreshHz > 0) s += formatString(" %d Hz", refreshHz);
    if (hdr) s += ", HDR";
    if (primary) s += ", primary";
    return s + ")";
}

static std::map<std::wstring, std::wstring> monitorFriendlyNames() {
    std::map<std::wstring, std::wstring> names;
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return names;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
        return names;
    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof source;
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof target;
        target.header.adapterId = paths[i].targetInfo.adapterId;
        target.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS &&
            DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS && target.monitorFriendlyDeviceName[0]) {
            names[source.viewGdiDeviceName] = target.monitorFriendlyDeviceName;
        }
    }
    return names;
}

std::vector<DisplayInfo> listDisplays() {
    std::vector<DisplayInfo> displays;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return displays;
    const auto friendly = monitorFriendlyNames();
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC od{};
            output->GetDesc(&od);
            if (!od.AttachedToDesktop) continue;
            DisplayInfo d;
            d.deviceName = narrow(od.DeviceName);
            auto it = friendly.find(od.DeviceName);
            d.friendlyName = it != friendly.end() ? narrow(it->second) : d.deviceName;
            d.width = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
            d.height = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
            MONITORINFO mi{sizeof mi};
            if (GetMonitorInfoW(od.Monitor, &mi)) d.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
            DEVMODEW dm{};
            dm.dmSize = sizeof dm;
            if (EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) d.refreshHz = int(dm.dmDisplayFrequency);
            ComPtr<IDXGIOutput6> o6;
            if (SUCCEEDED(output.As(&o6))) {
                DXGI_OUTPUT_DESC1 d1{};
                if (SUCCEEDED(o6->GetDesc1(&d1))) d.hdr = d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            }
            d.adapterName = narrow(ad.Description);
            d.adapter = adapter;
            d.output = output;
            displays.push_back(std::move(d));
        }
    }
    // Primary first, then by device name.
    std::stable_sort(displays.begin(), displays.end(), [](const DisplayInfo& a, const DisplayInfo& b) {
        if (a.primary != b.primary) return a.primary;
        return a.deviceName < b.deviceName;
    });
    return displays;
}

void fitSize(int w, int h, int maxWidth, int& outW, int& outH) {
    if (w > maxWidth && w > 0) {
        outW = maxWidth;
        outH = int(double(h) * maxWidth / w + 0.5);
    } else {
        outW = w;
        outH = h;
    }
    outW = std::max(2, outW & ~1);
    outH = std::max(2, outH & ~1);
}

// MARK: TexturePool

TexturePool::TexturePool(std::shared_ptr<GpuDevice> gpu, int width, int height, DXGI_FORMAT format, UINT bind,
                         size_t maxCount)
    : gpu_(std::move(gpu)), width_(width), height_(height), format_(format), bind_(bind), maxCount_(maxCount) {}

std::shared_ptr<ID3D11Texture2D> TexturePool::acquire() {
    ComPtr<ID3D11Texture2D> texture;
    {
        std::lock_guard lock(mutex_);
        if (!free_.empty()) {
            texture = free_.back();
            free_.pop_back();
        } else if (created_ < maxCount_) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = UINT(width_);
            desc.Height = UINT(height_);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = format_;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = bind_;
            HRESULT hr = gpu_->device->CreateTexture2D(&desc, nullptr, &texture);
            if (FAILED(hr) && (bind_ & D3D11_BIND_VIDEO_ENCODER)) {
                bind_ &= ~UINT(D3D11_BIND_VIDEO_ENCODER);
                desc.BindFlags = bind_;
                hr = gpu_->device->CreateTexture2D(&desc, nullptr, &texture);
            }
            if (FAILED(hr)) {
                log("capture: CreateTexture2D failed: " + hresultText(hr));
                return nullptr;
            }
            ++created_;
        } else {
            return nullptr;
        }
    }
    std::weak_ptr<TexturePool> weak = weak_from_this();
    ID3D11Texture2D* raw = texture.Detach();
    return std::shared_ptr<ID3D11Texture2D>(raw, [weak](ID3D11Texture2D* t) {
        if (auto pool = weak.lock()) {
            std::lock_guard lock(pool->mutex_);
            pool->free_.emplace_back(t);  // ComPtr takes another reference
        }
        t->Release();
    });
}

// MARK: Compositor

namespace {
const char kShader[] = R"(
struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut vs(VSIn i) { VSOut o; o.pos = float4(i.pos, 0, 1); o.uv = i.uv; return o; }
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 ps(VSOut i) : SV_TARGET { return tex.Sample(smp, i.uv); }
)";

struct Vertex {
    float x, y, u, v;
};

ComPtr<ID3DBlob> compile(const char* entry, const char* target, std::string& error) {
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(kShader, sizeof kShader - 1, "vrscreen", nullptr, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        error = std::string("shader compile failed: ") +
                (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                        : hresultText(hr));
        return nullptr;
    }
    return code;
}
}  // namespace

bool Compositor::init(GpuDevice& gpu, std::string& error) {
    auto vsCode = compile("vs", "vs_4_0", error);
    auto psCode = compile("ps", "ps_4_0", error);
    if (!vsCode || !psCode) return false;
    ID3D11Device* d = gpu.device.Get();
    d->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs_);
    d->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps_);
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    d->CreateInputLayout(layout, 2, vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &layout_);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Vertex) * 4;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    d->CreateBuffer(&bd, nullptr, &vertices_);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    d->CreateSamplerState(&sd, &sampler_);
    D3D11_BLEND_DESC blend{};
    auto& rt = blend.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
    rt.DestBlendAlpha = D3D11_BLEND_ONE;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    d->CreateBlendState(&blend, &blendAlpha_);
    // XOR-like: white source inverts the destination, black leaves it.
    rt.SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
    rt.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
    d->CreateBlendState(&blend, &blendInvert_);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    d->CreateRasterizerState(&rd, &raster_);
    if (!vs_ || !ps_ || !layout_ || !vertices_ || !sampler_ || !blendAlpha_ || !blendInvert_ || !raster_) {
        error = "could not create the compositor's Direct3D objects";
        return false;
    }
    return true;
}

void Compositor::draw(GpuDevice& gpu, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, int tw, int th,
                      float x, float y, float w, float h, DXGI_MODE_ROTATION rotation, Blend blend) {
    ID3D11DeviceContext* c = gpu.context.Get();
    const float l = x / tw * 2 - 1, r = (x + w) / tw * 2 - 1;
    const float t = 1 - y / th * 2, b = 1 - (y + h) / th * 2;
    // Corner order: TL, TR, BL, BR (triangle strip); UVs per rotation as in Microsoft's
    // Desktop Duplication sample.
    float uv[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    switch (rotation) {
        case DXGI_MODE_ROTATION_ROTATE90: {
            const float r90[4][2] = {{0, 1}, {0, 0}, {1, 1}, {1, 0}};
            std::copy(&r90[0][0], &r90[0][0] + 8, &uv[0][0]);
            break;
        }
        case DXGI_MODE_ROTATION_ROTATE180: {
            const float r180[4][2] = {{1, 1}, {0, 1}, {1, 0}, {0, 0}};
            std::copy(&r180[0][0], &r180[0][0] + 8, &uv[0][0]);
            break;
        }
        case DXGI_MODE_ROTATION_ROTATE270: {
            const float r270[4][2] = {{1, 0}, {1, 1}, {0, 0}, {0, 1}};
            std::copy(&r270[0][0], &r270[0][0] + 8, &uv[0][0]);
            break;
        }
        default: break;
    }
    const Vertex v[4] = {{l, t, uv[0][0], uv[0][1]}, {r, t, uv[1][0], uv[1][1]}, {l, b, uv[2][0], uv[2][1]},
                         {r, b, uv[3][0], uv[3][1]}};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(c->Map(vertices_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    memcpy(mapped.pData, v, sizeof v);
    c->Unmap(vertices_.Get(), 0);

    const UINT stride = sizeof(Vertex), offset = 0;
    ID3D11Buffer* vb = vertices_.Get();
    c->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    c->IASetInputLayout(layout_.Get());
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    c->VSSetShader(vs_.Get(), nullptr, 0);
    c->PSSetShader(ps_.Get(), nullptr, 0);
    c->PSSetShaderResources(0, 1, &source);
    ID3D11SamplerState* s = sampler_.Get();
    c->PSSetSamplers(0, 1, &s);
    c->RSSetState(raster_.Get());
    D3D11_VIEWPORT vp{0, 0, float(tw), float(th), 0, 1};
    c->RSSetViewports(1, &vp);
    c->OMSetRenderTargets(1, &target, nullptr);
    const float factor[4] = {0, 0, 0, 0};
    c->OMSetBlendState(blend == Blend::Opaque ? nullptr : blend == Blend::Alpha ? blendAlpha_.Get() : blendInvert_.Get(),
                       factor, 0xFFFFFFFF);
    c->Draw(4, 0);
    ID3D11ShaderResourceView* none = nullptr;
    c->PSSetShaderResources(0, 1, &none);
    ID3D11RenderTargetView* noTarget = nullptr;
    c->OMSetRenderTargets(1, &noTarget, nullptr);
}

// MARK: VideoConverter

bool VideoConverter::ensure(GpuDevice& gpu, int srcW, int srcH, int dstW, int dstH, std::string& error) {
    if (processor_ && device_ == gpu.device.Get() && srcW == srcW_ && srcH == srcH_ && dstW == dstW_ && dstH == dstH_)
        return true;
    processor_.Reset();
    enumerator_.Reset();
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate = {60, 1};
    desc.InputWidth = UINT(srcW);
    desc.InputHeight = UINT(srcH);
    desc.OutputFrameRate = {60, 1};
    desc.OutputWidth = UINT(dstW);
    desc.OutputHeight = UINT(dstH);
    desc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
    HRESULT hr = gpu.videoDevice->CreateVideoProcessorEnumerator(&desc, &enumerator_);
    if (SUCCEEDED(hr)) hr = gpu.videoDevice->CreateVideoProcessor(enumerator_.Get(), 0, &processor_);
    if (FAILED(hr)) {
        error = "video processor " + std::to_string(srcW) + "x" + std::to_string(srcH) + " -> " + std::to_string(dstW) +
                "x" + std::to_string(dstH) + ": " + hresultText(hr);
        return false;
    }
    ID3D11VideoContext* vc = gpu.videoContext.Get();
    ComPtr<ID3D11VideoContext1> vc1;
    if (SUCCEEDED(gpu.videoContext.As(&vc1))) {
        vc1->VideoProcessorSetStreamColorSpace1(processor_.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        vc1->VideoProcessorSetOutputColorSpace1(processor_.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
        in.RGB_Range = 0;  // full
        vc->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &in);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
        out.YCbCr_Matrix = 1;  // BT.709
        out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        vc->VideoProcessorSetOutputColorSpace(processor_.Get(), &out);
    }
    vc->VideoProcessorSetStreamFrameFormat(processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vc->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0, FALSE);
    const RECT src{0, 0, srcW, srcH}, dst{0, 0, dstW, dstH};
    vc->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &src);
    vc->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &dst);
    vc->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &dst);
    device_ = gpu.device.Get();
    srcW_ = srcW;
    srcH_ = srcH;
    dstW_ = dstW;
    dstH_ = dstH;
    return true;
}

bool VideoConverter::convert(GpuDevice& gpu, ID3D11Texture2D* bgra, int srcW, int srcH, ID3D11Texture2D* nv12, int dstW,
                             int dstH, std::string& error) {
    if (!ensure(gpu, srcW, srcH, dstW, dstH, error)) return false;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inDesc{};
    inDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorInputView> input;
    HRESULT hr = gpu.videoDevice->CreateVideoProcessorInputView(bgra, enumerator_.Get(), &inDesc, &input);
    if (FAILED(hr)) {
        error = "CreateVideoProcessorInputView: " + hresultText(hr);
        return false;
    }
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outDesc{};
    outDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11VideoProcessorOutputView> output;
    hr = gpu.videoDevice->CreateVideoProcessorOutputView(nv12, enumerator_.Get(), &outDesc, &output);
    if (FAILED(hr)) {
        error = "CreateVideoProcessorOutputView: " + hresultText(hr);
        return false;
    }
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input.Get();
    hr = gpu.videoContext->VideoProcessorBlt(processor_.Get(), output.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        error = "VideoProcessorBlt: " + hresultText(hr);
        return false;
    }
    return true;
}

}  // namespace vr
