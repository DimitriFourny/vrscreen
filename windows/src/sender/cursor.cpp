#include "cursor.h"

namespace vr {

CursorImages convertPointerShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info, const uint8_t* buffer) {
    CursorImages img;
    const int w = int(info.Width);
    const int pitch = int(info.Pitch);
    constexpr uint32_t kBlack = 0xFF000000, kWhite = 0xFFFFFFFF, kClear = 0;
    if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
        // AND mask on top, XOR mask below, 1 bit per pixel.
        const int h = int(info.Height) / 2;
        img.width = w;
        img.height = h;
        img.normal.assign(size_t(w) * h, kClear);
        img.invert.assign(size_t(w) * h, kClear);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int byte = x / 8, bit = 7 - x % 8;
                const bool andBit = (buffer[y * pitch + byte] >> bit) & 1;
                const bool xorBit = (buffer[(y + h) * pitch + byte] >> bit) & 1;
                const size_t i = size_t(y) * w + x;
                if (!andBit) {
                    img.normal[i] = xorBit ? kWhite : kBlack;
                } else if (xorBit) {
                    img.invert[i] = kWhite;
                    img.hasInvert = true;
                }
            }
        }
        return img;
    }
    const int h = int(info.Height);
    img.width = w;
    img.height = h;
    img.normal.assign(size_t(w) * h, kClear);
    img.invert.assign(size_t(w) * h, kClear);
    for (int y = 0; y < h; ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(buffer + size_t(y) * pitch);
        for (int x = 0; x < w; ++x) {
            const uint32_t p = row[x];
            const size_t i = size_t(y) * w + x;
            if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
                img.normal[i] = p;  // BGRA, straight alpha
            } else {
                // Masked color: mask 0 = replace with the colour, mask 0xFF = XOR the colour.
                if ((p >> 24) == 0) {
                    img.normal[i] = p | 0xFF000000;
                } else if (p & 0x00FFFFFF) {
                    img.invert[i] = (p & 0x00FFFFFF) | 0xFF000000;
                    img.hasInvert = true;
                }
            }
        }
    }
    return img;
}

static ComPtr<ID3D11ShaderResourceView> makeTexture(GpuDevice& gpu, int w, int h, const std::vector<uint32_t>& pixels) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(w);
    desc.Height = UINT(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{pixels.data(), UINT(w * 4), 0};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    if (SUCCEEDED(gpu.device->CreateTexture2D(&desc, &data, &texture)))
        gpu.device->CreateShaderResourceView(texture.Get(), nullptr, &view);
    return view;
}

void CursorRenderer::updateShape(GpuDevice& gpu, const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info, const uint8_t* buffer) {
    const CursorImages img = convertPointerShape(info, buffer);
    normal_.Reset();
    invert_.Reset();
    if (img.width <= 0 || img.height <= 0) return;
    width_ = img.width;
    height_ = img.height;
    normal_ = makeTexture(gpu, img.width, img.height, img.normal);
    if (img.hasInvert) invert_ = makeTexture(gpu, img.width, img.height, img.invert);
}

void CursorRenderer::draw(GpuDevice& gpu, Compositor& compositor, ID3D11RenderTargetView* target, int tw, int th) {
    if (!visible()) return;
    compositor.draw(gpu, normal_.Get(), target, tw, th, float(x_), float(y_), float(width_), float(height_),
                    DXGI_MODE_ROTATION_IDENTITY, Compositor::Blend::Alpha);
    if (invert_)
        compositor.draw(gpu, invert_.Get(), target, tw, th, float(x_), float(y_), float(width_), float(height_),
                        DXGI_MODE_ROTATION_IDENTITY, Compositor::Blend::Invert);
}

}  // namespace vr
