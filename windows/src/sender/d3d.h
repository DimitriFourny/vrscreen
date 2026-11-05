// Direct3D 11 plumbing shared by the capture sources and the encoder: the device on the captured
// display's adapter, the display list, a small compositor (desktop rotation + mouse cursor) and
// the GPU colour conversion BGRA → NV12 (BT.709 limited range) with scaling.
#pragma once

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vr {

using Microsoft::WRL::ComPtr;

struct GpuDevice {
    ComPtr<IDXGIAdapter1> adapter;
    LUID luid{};
    std::string adapterName;
    UINT vendorId = 0;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D10Multithread> multithread;

    // Holds the device's lock across a sequence of immediate-context calls (the encoder uses the
    // same device from its own thread).
    struct Lock {
        explicit Lock(const GpuDevice& d) : mt(d.multithread.Get()) { if (mt) mt->Enter(); }
        ~Lock() { if (mt) mt->Leave(); }
        ID3D10Multithread* mt;
    };
};

// nullptr with `error` set on failure.
std::shared_ptr<GpuDevice> createGpuDevice(IDXGIAdapter1* adapter, std::string& error);

struct DisplayInfo {
    std::string deviceName;    // "\\.\DISPLAY1"
    std::string friendlyName;  // "DELL U2723QE", or the device name
    int width = 0, height = 0; // desktop size in pixels (rotated)
    int refreshHz = 0;
    bool primary = false;
    bool hdr = false;
    std::string adapterName;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    std::string label() const;  // "DELL U2723QE (3840x2160, primary)"
};

std::vector<DisplayInfo> listDisplays();

// A pool of NV12 textures handed out as shared_ptr leases; a texture returns to the pool when its
// last lease goes away (pipeline "latest", encoder in progress...).
class TexturePool : public std::enable_shared_from_this<TexturePool> {
public:
    TexturePool(std::shared_ptr<GpuDevice> gpu, int width, int height, DXGI_FORMAT format, UINT bind, size_t maxCount);
    // nullptr when all `maxCount` textures are in use.
    std::shared_ptr<ID3D11Texture2D> acquire();
    int width() const { return width_; }
    int height() const { return height_; }

private:
    std::shared_ptr<GpuDevice> gpu_;
    int width_, height_;
    DXGI_FORMAT format_;
    UINT bind_;
    size_t maxCount_;
    std::mutex mutex_;
    std::vector<ComPtr<ID3D11Texture2D>> free_;
    size_t created_ = 0;
};

// A captured picture ready to encode.
struct VideoFrame {
    std::shared_ptr<GpuDevice> gpu;
    std::shared_ptr<ID3D11Texture2D> nv12;
    int width = 0, height = 0;
    uint64_t captureMicros = 0;
};

// Draws textured quads into a BGRA render target: the desktop (with its rotation) and the cursor.
class Compositor {
public:
    enum class Blend { Opaque, Alpha, Invert };
    bool init(GpuDevice& gpu, std::string& error);
    // Draws `source` into `target` covering destination pixels (x, y, w, h) of a target of size
    // (tw, th). `rotation` (DXGI_MODE_ROTATION) rotates the source image.
    void draw(GpuDevice& gpu, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, int tw, int th, float x,
              float y, float w, float h, DXGI_MODE_ROTATION rotation, Blend blend);

private:
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11InputLayout> layout_;
    ComPtr<ID3D11Buffer> vertices_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> blendAlpha_, blendInvert_;
    ComPtr<ID3D11RasterizerState> raster_;
};

// BGRA (full range sRGB) → NV12 (BT.709 limited range) with scaling, on the GPU video processor.
class VideoConverter {
public:
    bool convert(GpuDevice& gpu, ID3D11Texture2D* bgra, int srcW, int srcH, ID3D11Texture2D* nv12, int dstW, int dstH,
                 std::string& error);

private:
    bool ensure(GpuDevice& gpu, int srcW, int srcH, int dstW, int dstH, std::string& error);
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    ComPtr<ID3D11VideoProcessor> processor_;
    int srcW_ = 0, srcH_ = 0, dstW_ = 0, dstH_ = 0;
    ID3D11Device* device_ = nullptr;
};

// Output size: (w, h) scaled down to fit `maxWidth`, aspect kept, both even.
void fitSize(int w, int h, int maxWidth, int& outW, int& outH);

}  // namespace vr
