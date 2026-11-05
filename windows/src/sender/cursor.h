// The mouse pointer, which Desktop Duplication delivers separately from the desktop image:
// converted to two BGRA textures (normal pixels alpha-blended, inverting pixels drawn with an
// XOR-like blend) and drawn onto the composed frame.
#pragma once

#include <cstdint>
#include <vector>

#include "d3d.h"

namespace vr {

// CPU conversion of a DXGI pointer shape (testable): `normal` is straight-alpha BGRA, `invert`
// is white (alpha 255) where the screen is inverted, transparent black elsewhere.
struct CursorImages {
    int width = 0, height = 0;
    std::vector<uint32_t> normal, invert;
    bool hasInvert = false;
};
CursorImages convertPointerShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info, const uint8_t* buffer);

class CursorRenderer {
public:
    void updateShape(GpuDevice& gpu, const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info, const uint8_t* buffer);
    void setPosition(int x, int y, bool visible) {
        x_ = x;
        y_ = y;
        visible_ = visible;
    }
    bool visible() const { return visible_ && normal_; }
    // Draws at the current position (desktop pixels) into `target` of size (tw, th).
    void draw(GpuDevice& gpu, Compositor& compositor, ID3D11RenderTargetView* target, int tw, int th);
    void reset() {
        normal_.Reset();
        invert_.Reset();
        visible_ = false;
    }

private:
    ComPtr<ID3D11ShaderResourceView> normal_, invert_;
    int width_ = 0, height_ = 0;
    int x_ = 0, y_ = 0;
    bool visible_ = false;
};

}  // namespace vr
