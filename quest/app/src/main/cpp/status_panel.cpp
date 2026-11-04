#include "status_panel.h"

#include "bitmap_font.h"
#include "log.h"

#include <sstream>

namespace qm {

void StatusPanel::attach(ANativeWindow* window) {
    window_ = window;
    const int32_t rc = ANativeWindow_setBuffersGeometry(window_, kWidth, kHeight, WINDOW_FORMAT_RGBA_8888);
    if (rc != 0) LOGE("StatusPanel: setBuffersGeometry failed rc=%d", rc);
}

bool StatusPanel::draw(const std::string& text) {
    if (window_ == nullptr) return false;
    ANativeWindow_Buffer buffer;
    const int32_t rc = ANativeWindow_lock(window_, &buffer, nullptr);
    if (rc != 0) {
        LOGW("StatusPanel: ANativeWindow_lock failed rc=%d", rc);
        return false;
    }
    const Canvas cv{static_cast<uint32_t*>(buffer.bits), buffer.stride, buffer.width, buffer.height};
    const uint32_t border = rgb(90, 140, 255);
    cv.fill(0, 0, cv.w, cv.h, rgb(18, 20, 28));
    cv.fill(0, 0, cv.w, 4, border);
    cv.fill(0, cv.h - 4, cv.w, 4, border);
    cv.fill(0, 0, 4, cv.h, border);
    cv.fill(cv.w - 4, 0, 4, cv.h, border);

    std::istringstream lines(text);
    std::string line;
    int32_t y = 36;
    bool heading = true;
    while (std::getline(lines, line)) {
        const int32_t scale = heading ? 6 : 4;
        cv.text(40, y, scale, line.c_str(), heading ? rgb(255, 220, 0) : rgb(235, 235, 235));
        y += heading ? 72 : 44;
        heading = false;
    }
    ANativeWindow_unlockAndPost(window_);
    return true;
}

}  // namespace qm
