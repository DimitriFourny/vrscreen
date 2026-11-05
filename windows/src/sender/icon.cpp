// The tray icon, drawn in code (anti-aliased by supersampling) so it is sharp at any DPI:
// a headset seen from the front, white on a dark taskbar and black on a light one.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "tray.h"

namespace vr {
namespace {

bool lightTaskbar() {
    DWORD value = 0, size = sizeof value;
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
           value != 0;
}

float roundedRect(float x, float y, float x0, float y0, float x1, float y1, float r) {
    // Signed distance (negative inside).
    const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    const float hx = (x1 - x0) / 2 - r, hy = (y1 - y0) / 2 - r;
    const float dx = std::fabs(x - cx) - hx, dy = std::fabs(y - cy) - hy;
    const float ox = std::max(dx, 0.0f), oy = std::max(dy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(dx, dy), 0.0f) - r;
}

float ellipse(float x, float y, float cx, float cy, float rx, float ry) {
    const float nx = (x - cx) / rx, ny = (y - cy) / ry;
    return (std::sqrt(nx * nx + ny * ny) - 1) * std::min(rx, ry);
}

// 0..1 coverage of the glyph at unit coordinates.
bool glyph(float x, float y) {
    if (roundedRect(x, y, 0.04f, 0.24f, 0.96f, 0.76f, 0.17f) > 0) return false;
    if (ellipse(x, y, 0.5f, 0.86f, 0.13f, 0.17f) < 0) return false;  // nose
    if (roundedRect(x, y, 0.15f, 0.36f, 0.45f, 0.62f, 0.09f) < 0) return false;  // lenses
    if (roundedRect(x, y, 0.55f, 0.36f, 0.85f, 0.62f, 0.09f) < 0) return false;
    return true;
}

bool badgeCircle(float x, float y) { return ellipse(x, y, 0.76f, 0.76f, 0.24f, 0.24f) < 0; }
bool badgeMark(float x, float y) {
    return (std::fabs(x - 0.76f) < 0.045f && y > 0.59f && y < 0.80f) || ellipse(x, y, 0.76f, 0.88f, 0.05f, 0.05f) < 0;
}

}  // namespace

HICON makeTrayIcon(bool badge, bool active) {
    const int size = GetSystemMetrics(SM_CXSMICON);
    const bool light = lightTaskbar();
    const uint32_t ink = light ? 0x202020 : 0xFFFFFF;
    const float inkAlpha = active ? 1.0f : 0.62f;
    constexpr int kSS = 4;  // 4x4 supersampling

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    auto* px = static_cast<uint32_t*>(bits);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float r = 0, g = 0, b = 0, a = 0;  // premultiplied
            for (int sy = 0; sy < kSS; ++sy) {
                for (int sx = 0; sx < kSS; ++sx) {
                    const float u = (x + (sx + 0.5f) / kSS) / size, v = (y + (sy + 0.5f) / kSS) / size;
                    float cr = 0, cg = 0, cb = 0, ca = 0;
                    if (badge && badgeCircle(u, v)) {
                        if (badgeMark(u, v)) {
                            cr = cg = cb = 1;
                        } else {
                            cr = 0.96f, cg = 0.55f, cb = 0.1f;
                        }
                        ca = 1;
                    } else if (glyph(u, v) && !(badge && ellipse(u, v, 0.76f, 0.76f, 0.30f, 0.30f) < 0)) {
                        cr = ((ink >> 16) & 0xFF) / 255.0f;
                        cg = ((ink >> 8) & 0xFF) / 255.0f;
                        cb = (ink & 0xFF) / 255.0f;
                        ca = inkAlpha;
                    }
                    r += cr * ca;
                    g += cg * ca;
                    b += cb * ca;
                    a += ca;
                }
            }
            const float n = kSS * kSS;
            const auto c = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255 + 0.5f); };
            px[y * size + x] = c(a / n) << 24 | c(r / n) << 16 | c(g / n) << 8 | c(b / n);
        }
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

}  // namespace vr
