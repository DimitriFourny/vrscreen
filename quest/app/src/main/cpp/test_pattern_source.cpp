#include "test_pattern_source.h"

#include "log.h"

#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace qm {
namespace {

constexpr uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) {
    // RGBA_8888 in memory is R,G,B,A bytes, i.e. little-endian 0xAABBGGRR.
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

constexpr uint32_t kBlack = rgb(0, 0, 0);
constexpr uint32_t kWhite = rgb(255, 255, 255);
constexpr uint32_t kBackground = rgb(24, 24, 24);
constexpr uint32_t kGridFine = rgb(72, 72, 72);
constexpr uint32_t kGridCoarse = rgb(150, 150, 150);
constexpr uint32_t kRed = rgb(255, 40, 40);
constexpr uint32_t kYellow = rgb(255, 220, 0);

// 5x7 bitmap font: 7 rows, bit 4 = leftmost column.
struct Glyph {
    char c;
    uint8_t rows[7];
};

constexpr Glyph kFont[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
    {'=', {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}},
    {'/', {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}},
    {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}},
    {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
    {'#', {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}},
    {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
};
constexpr uint8_t kUnknownGlyph[7] = {0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1F};

const uint8_t* glyphRows(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    for (const Glyph& g : kFont) {
        if (g.c == c) return g.rows;
    }
    return kUnknownGlyph;
}

// Clipped drawing into an RGBA_8888 buffer.
struct Canvas {
    uint32_t* px;
    int32_t stride;  // in pixels
    int32_t w;
    int32_t h;

    void fill(int32_t x, int32_t y, int32_t fw, int32_t fh, uint32_t c) const {
        const int32_t x0 = std::max(x, 0), y0 = std::max(y, 0);
        const int32_t x1 = std::min(x + fw, w), y1 = std::min(y + fh, h);
        for (int32_t yy = y0; yy < y1; ++yy) std::fill(px + yy * stride + x0, px + yy * stride + x1, c);
    }

    void set(int32_t x, int32_t y, uint32_t c) const {
        if (x >= 0 && y >= 0 && x < w && y < h) px[y * stride + x] = c;
    }

    // Each glyph cell is 6x8 units (5x7 glyph + 1 unit spacing); a unit is `scale` pixels.
    static int32_t textWidth(const char* s, int32_t scale) {
        return static_cast<int32_t>(std::strlen(s)) * 6 * scale;
    }

    void text(int32_t x, int32_t y, int32_t scale, const char* s, uint32_t fg) const {
        for (; *s; ++s, x += 6 * scale) {
            const uint8_t* rows = glyphRows(*s);
            for (int32_t r = 0; r < 7; ++r) {
                for (int32_t col = 0; col < 5; ++col) {
                    if (rows[r] & (0x10 >> col)) fill(x + col * scale, y + r * scale, scale, scale, fg);
                }
            }
        }
    }

    // Text on an opaque box with a 1-unit margin.
    void label(int32_t x, int32_t y, int32_t scale, const char* s, uint32_t fg, uint32_t bg) const {
        fill(x - scale, y - scale, textWidth(s, scale) + scale, 9 * scale, bg);
        text(x, y, scale, s, fg);
    }
};

constexpr const char* kPangram1 = "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789 +-=/:.,%#()!?";
constexpr const char* kPangram2 = "PACK MY BOX WITH FIVE DOZEN LIQUOR JUGS. SPHINX OF BLACK QUARTZ, JUDGE MY VOW";
constexpr const char* kPangram3 = "ILI1L1 0O0O 8B8B 5S5S 2Z2Z MWMW - FONT 5X7 AT 1PX PER DOT";
constexpr const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789";

void textPanel(const Canvas& cv, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t fg, uint32_t bg) {
    cv.fill(x, y, w, h, bg);
    int32_t ty = y + 12;
    for (int32_t scale = 1; scale <= 3; ++scale) {
        const char* lines[] = {kPangram1, kPangram2, kPangram3, kAlphabet};
        for (const char* line : lines) {
            if (Canvas::textWidth(line, scale) > w - 24) continue;
            cv.text(x + 12, ty, scale, line, fg);
            ty += 10 * scale;
        }
        ty += 6;
    }
    cv.text(x + 12, ty, 4, "QUEST 3 MIRROR", fg);
}

}  // namespace

TestPatternSource::~TestPatternSource() { stopThread(); }

void TestPatternSource::onSurfaceReady(ANativeWindow* window, jobject /*surface*/, int32_t width, int32_t height) {
    stopThread();
    window_ = window;
    width_ = width;
    height_ = height;
    framesPosted_ = 0;
    const int32_t rc = ANativeWindow_setBuffersGeometry(window_, width_, height_, WINDOW_FORMAT_RGBA_8888);
    LOGI("TestPattern: surface ready %dx%d, setBuffersGeometry(RGBA_8888) rc=%d, window reports %dx%d format %d",
         width_, height_, rc, ANativeWindow_getWidth(window_), ANativeWindow_getHeight(window_),
         ANativeWindow_getFormat(window_));
    renderBackground();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = false;
    }
    thread_ = std::thread(&TestPatternSource::run, this);
}

void TestPatternSource::onVisible(bool visible) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        visible_ = visible;
        cv_.notify_all();
        // Pausing is synchronous: return only once no frame is being written to the Surface.
        if (!visible) cv_.wait(lock, [this] { return !producing_; });
    }
    LOGI("TestPattern: producer %s", visible ? "resumed (visible)" : "paused (not visible)");
}

void TestPatternSource::onSurfaceDestroyed() {
    stopThread();
    window_ = nullptr;
    LOGI("TestPattern: surface released after %" PRIu64 " frames", framesPosted_);
}

void TestPatternSource::stopThread() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void TestPatternSource::renderBackground() {
    background_.assign(static_cast<size_t>(width_) * height_, kBackground);
    const Canvas cv{background_.data(), width_, width_, height_};

    // 1-px grid: fine every 32 px, bright every 128 px, white 1-px border.
    for (int32_t y = 0; y < height_; ++y) {
        for (int32_t x = 0; x < width_; ++x) {
            if (x % 128 == 0 || y % 128 == 0) {
                cv.set(x, y, kGridCoarse);
            } else if (x % 32 == 0 || y % 32 == 0) {
                cv.set(x, y, kGridFine);
            }
        }
    }
    cv.fill(0, 0, width_, 1, kWhite);
    cv.fill(0, height_ - 1, width_, 1, kWhite);
    cv.fill(0, 0, 1, height_, kWhite);
    cv.fill(width_ - 1, 0, 1, height_, kWhite);

    char title[96];
    std::snprintf(title, sizeof(title), "QUEST MIRROR TEST PATTERN %dX%d", width_, height_);
    cv.label(40, 40, 3, title, kWhite, kBlack);
    cv.label(40, 80, 2, "1PX GRID: FINE EVERY 32PX, BRIGHT EVERY 128PX", kWhite, kBlack);

    // Resolution patches, 128x128 each.
    struct Patch {
        const char* name;
        uint32_t (*color)(int32_t x, int32_t y);
    };
    const Patch patches[] = {
        {"1PX CHECKER", [](int32_t x, int32_t y) { return ((x + y) & 1) ? kWhite : kBlack; }},
        {"1PX VERTICAL", [](int32_t x, int32_t) { return (x & 1) ? kWhite : kBlack; }},
        {"1PX HORIZONTAL", [](int32_t, int32_t y) { return (y & 1) ? kWhite : kBlack; }},
        {"2PX VERTICAL", [](int32_t x, int32_t) { return (x & 2) ? kWhite : kBlack; }},
        {"2PX HORIZONTAL", [](int32_t, int32_t y) { return (y & 2) ? kWhite : kBlack; }},
        {"4PX CHECKER", [](int32_t x, int32_t y) { return (((x >> 2) + (y >> 2)) & 1) ? kWhite : kBlack; }},
    };
    int32_t px = 40;
    for (const Patch& p : patches) {
        for (int32_t y = 0; y < 128; ++y) {
            for (int32_t x = 0; x < 128; ++x) cv.set(px + x, 128 + y, p.color(x, y));
        }
        cv.label(px, 264, 1, p.name, kWhite, kBlack);
        px += 160;
    }

    // Colour bars and a 16-step grey ramp.
    const uint32_t bars[] = {kWhite, rgb(255, 255, 0), rgb(0, 255, 255), rgb(0, 255, 0),
                             rgb(255, 0, 255), rgb(255, 0, 0), rgb(0, 0, 255)};
    for (int32_t i = 0; i < 7; ++i) cv.fill(40 + i * 128, 296, 128, 56, bars[i]);
    for (int32_t i = 0; i < 16; ++i) {
        const uint32_t v = static_cast<uint32_t>(i * 17);
        cv.fill(40 + i * 56, 352, 56, 40, rgb(v, v, v));
    }

    // Text sharpness panels: black on white and white on black.
    textPanel(cv, 1000, 128, 880, 300, kBlack, kWhite);
    textPanel(cv, 1000, 448, 880, 300, kWhite, kBlack);

    // Marker track with 64-px ticks.
    const int32_t trackY = 900;
    cv.fill(40, trackY, width_ - 80, 1, kWhite);
    for (int32_t x = 40; x <= width_ - 40; x += 64) cv.fill(x, trackY - 8, 1, 17, kWhite);

    // Centre cross.
    cv.fill(width_ / 2 - 20, height_ / 2, 41, 1, kRed);
    cv.fill(width_ / 2, height_ / 2 - 20, 1, 41, kRed);

    cv.label(40, height_ - 48, 2, "RECENTER: A OR X BUTTON    LAYER FILTER: B OR Y BUTTON", kWhite, kBlack);
}

void TestPatternSource::drawFrame(uint32_t* pixels, int32_t stride, double seconds, double fps) {
    for (int32_t y = 0; y < height_; ++y) {
        std::memcpy(pixels + static_cast<size_t>(y) * stride, background_.data() + static_cast<size_t>(y) * width_,
                    static_cast<size_t>(width_) * sizeof(uint32_t));
    }
    const Canvas cv{pixels, stride, width_, height_};

    char line[64];
    std::snprintf(line, sizeof(line), "FRAME %06" PRIu64, framesPosted_);
    cv.label(48, 480, 10, line, kYellow, kBlack);
    std::snprintf(line, sizeof(line), "T %8.1fS  %4.1f FPS", seconds, fps);
    cv.label(48, 580, 4, line, kWhite, kBlack);

    // Marker bounces along the track, 8 px per posted frame: a stalled producer is obvious.
    const int32_t travel = std::max(1, width_ - 80 - 48);
    const int32_t phase = static_cast<int32_t>((framesPosted_ * 8) % (2 * static_cast<uint64_t>(travel)));
    const int32_t mx = 40 + (phase < travel ? phase : 2 * travel - phase);
    const int32_t trackY = 900;
    cv.fill(mx, trackY - 24, 48, 48, kRed);
    cv.fill(mx + 4, trackY - 20, 40, 40, kYellow);
    cv.fill(mx + 24, trackY - 40, 1, 81, kWhite);
}

void TestPatternSource::run() {
    pthread_setname_np(pthread_self(), "qm-testpattern");
    using Clock = std::chrono::steady_clock;
    constexpr auto kPeriod = std::chrono::microseconds(33333);  // ~30 fps
    const auto start = Clock::now();
    auto next = start;
    auto lastPost = start;
    auto lastLog = start;
    uint64_t framesAtLastLog = framesPosted_;
    double fps = 0.0;
    int32_t lockFailures = 0;

    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        if (!visible_ && !stop_) {
            cv_.wait(lock, [this] { return stop_ || visible_; });
            // Restart pacing and stats after a pause.
            next = lastLog = Clock::now();
            framesAtLastLog = framesPosted_;
            fps = 0.0;
        }
        if (stop_) break;
        producing_ = true;  // onVisible(false) waits for this to clear
        lock.unlock();

        ANativeWindow_Buffer buffer;
        const int32_t rc = ANativeWindow_lock(window_, &buffer, nullptr);
        const auto now = Clock::now();
        if (rc == 0) {
            if (buffer.width < width_ || buffer.height < height_ || buffer.format != WINDOW_FORMAT_RGBA_8888) {
                LOGE("TestPattern: unexpected buffer %dx%d format %d stride %d", buffer.width, buffer.height,
                     buffer.format, buffer.stride);
                ANativeWindow_unlockAndPost(window_);
            } else {
                if (framesPosted_ > 0) {
                    const double dt = std::chrono::duration<double>(now - lastPost).count();
                    if (dt > 0.0) fps = fps == 0.0 ? 1.0 / dt : 0.9 * fps + 0.1 / dt;
                }
                lastPost = now;
                drawFrame(static_cast<uint32_t*>(buffer.bits), buffer.stride,
                          std::chrono::duration<double>(now - start).count(), fps);
                ANativeWindow_unlockAndPost(window_);
                if (framesPosted_ == 0) {
                    LOGI("TestPattern: first frame posted (buffer %dx%d stride %d)", buffer.width, buffer.height,
                         buffer.stride);
                }
                ++framesPosted_;
            }
        } else if (lockFailures++ % 30 == 0) {
            LOGW("TestPattern: ANativeWindow_lock failed rc=%d (%d failures)", rc, lockFailures);
        }

        if (now - lastLog >= std::chrono::seconds(5)) {
            const double secs = std::chrono::duration<double>(now - lastLog).count();
            LOGI("TestPattern: posted frame %" PRIu64 " (%.1f fps over last %.1fs)", framesPosted_,
                 static_cast<double>(framesPosted_ - framesAtLastLog) / secs, secs);
            lastLog = now;
            framesAtLastLog = framesPosted_;
        }

        next += kPeriod;
        if (next < now) next = now + kPeriod;
        lock.lock();
        producing_ = false;
        cv_.notify_all();
        cv_.wait_until(lock, next, [this] { return stop_ || !visible_; });
    }
}

}  // namespace qm
