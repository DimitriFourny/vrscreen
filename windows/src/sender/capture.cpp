#include "capture.h"

#include <avrt.h>
#include <mmsystem.h>

#include <algorithm>

#include "log.h"
#include "util.h"

namespace vr {
namespace {
constexpr int kPoolSize = 4;

// Picks the display: --display N, else the saved device name, else the primary display.
std::optional<DisplayInfo> pickDisplay(const CaptureSettings& s, std::string& note) {
    auto displays = listDisplays();
    if (displays.empty()) return std::nullopt;
    if (s.displayIndex) {
        if (*s.displayIndex < int(displays.size())) return displays[size_t(*s.displayIndex)];
        note = formatString("--display %d does not exist (%zu display(s)); using the primary display", *s.displayIndex,
                            displays.size());
    } else if (!s.display.empty()) {
        for (auto& d : displays) {
            if (d.deviceName == s.display) return d;
        }
        note = s.display + " is not connected; using the primary display";
    }
    return displays.front();  // primary first
}

uint64_t micros() { return nowMicros(); }
}  // namespace

void ScreenCapture::start() {
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void ScreenCapture::stop() {
    stopping_ = true;
    if (thread_.joinable()) thread_.join();
}

void ScreenCapture::update(const CaptureSettings& settings) {
    std::lock_guard lock(mutex_);
    settings_ = settings;
    settingsChanged_ = true;
}

void ScreenCapture::report(const CaptureStatus& status) {
    const std::string key = (status.kind == CaptureStatus::Kind::Running ? "R:" + status.deviceName : "F:" + status.message);
    if (key == lastReported_) return;
    lastReported_ = key;
    if (onStatus) onStatus(status);
}

void ScreenCapture::close() {
    dup_.Reset();
    desktop_.Reset();
    desktopView_.Reset();
    composite_.Reset();
    compositeTarget_.Reset();
    cursor_.reset();
}

bool ScreenCapture::open(std::string& error) {
    close();
    std::string note;
    auto display = pickDisplay(active_, note);
    if (!display) {
        error = "no display found";
        return false;
    }
    if (!note.empty() && deviceName_ != display->deviceName) log("capture: " + note);
    DXGI_ADAPTER_DESC1 ad{};
    display->adapter->GetDesc1(&ad);
    if (!gpu_ || memcmp(&gpu_->luid, &ad.AdapterLuid, sizeof(LUID)) != 0) {
        gpu_ = createGpuDevice(display->adapter.Get(), error);
        if (!gpu_) return false;
        if (!compositor_.init(*gpu_, error)) {
            gpu_.reset();
            return false;
        }
        converter_ = VideoConverter{};
        pool_.reset();
    }
    HRESULT hr = E_FAIL;
    ComPtr<IDXGIOutput5> output5;
    if (SUCCEEDED(display->output.As(&output5))) {
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        hr = output5->DuplicateOutput1(gpu_->device.Get(), 0, 1, formats, &dup_);
    }
    if (FAILED(hr)) {
        ComPtr<IDXGIOutput1> output1;
        if (SUCCEEDED(display->output.As(&output1))) hr = output1->DuplicateOutput(gpu_->device.Get(), &dup_);
    }
    if (FAILED(hr)) {
        if (hr == E_ACCESSDENIED) error = "the secure desktop is shown (UAC prompt, lock screen); waiting";
        else if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) error = "too many screen capture apps running";
        else if (hr == DXGI_ERROR_UNSUPPORTED) error = "Desktop Duplication is not supported on " + gpu_->adapterName;
        else error = "DuplicateOutput: " + hresultText(hr);
        return false;
    }
    DXGI_OUTDUPL_DESC dd{};
    dup_->GetDesc(&dd);
    rotation_ = dd.Rotation;
    imageW_ = int(dd.ModeDesc.Width);
    imageH_ = int(dd.ModeDesc.Height);
    const bool sideways = rotation_ == DXGI_MODE_ROTATION_ROTATE90 || rotation_ == DXGI_MODE_ROTATION_ROTATE270;
    desktopW_ = sideways ? imageH_ : imageW_;
    desktopH_ = sideways ? imageW_ : imageH_;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(imageW_);
    desc.Height = UINT(imageH_);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = gpu_->device->CreateTexture2D(&desc, nullptr, &desktop_);
    if (SUCCEEDED(hr)) hr = gpu_->device->CreateShaderResourceView(desktop_.Get(), nullptr, &desktopView_);
    desc.Width = UINT(desktopW_);
    desc.Height = UINT(desktopH_);
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (SUCCEEDED(hr)) hr = gpu_->device->CreateTexture2D(&desc, nullptr, &composite_);
    if (SUCCEEDED(hr)) hr = gpu_->device->CreateRenderTargetView(composite_.Get(), nullptr, &compositeTarget_);
    if (FAILED(hr)) {
        error = "capture textures: " + hresultText(hr);
        close();
        return false;
    }
    int outW, outH;
    fitSize(desktopW_, desktopH_, active_.maxWidth, outW, outH);
    if (!pool_ || pool_->width() != outW || pool_->height() != outH) {
        pool_ = std::make_shared<TexturePool>(gpu_, outW, outH, DXGI_FORMAT_NV12,
                                              D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER, kPoolSize);
    }
    outW_ = outW;
    outH_ = outH;
    if (deviceName_ != display->deviceName || !everRunning_) {
        logf("capture: %s on %s, desktop %dx%d%s -> %dx%d, up to %d fps (Desktop Duplication)", display->label().c_str(),
             gpu_->adapterName.c_str(), desktopW_, desktopH_,
             rotation_ > DXGI_MODE_ROTATION_IDENTITY ? formatString(" rotated %d°", (int(rotation_) - 1) * 90).c_str() : "",
             outW_, outH_, active_.fps);
        if (display->hdr) log("capture: the display is in HDR mode; it is captured as SDR (8-bit, BT.709)");
    }
    deviceName_ = display->deviceName;
    everRunning_ = true;
    report({CaptureStatus::Kind::Running, "", deviceName_});
    return true;
}

bool ScreenCapture::deliver(uint64_t captureMicros) {
    auto nv12 = pool_->acquire();
    if (!nv12) return false;  // every texture is still in use: deliver later
    std::string error;
    {
        GpuDevice::Lock lock(*gpu_);
        compositor_.draw(*gpu_, desktopView_.Get(), compositeTarget_.Get(), desktopW_, desktopH_, 0, 0, float(desktopW_),
                         float(desktopH_), rotation_, Compositor::Blend::Opaque);
        cursor_.draw(*gpu_, compositor_, compositeTarget_.Get(), desktopW_, desktopH_);
        if (!converter_.convert(*gpu_, composite_.Get(), desktopW_, desktopH_, nv12.get(), outW_, outH_, error)) {
            log("capture: " + error);
            return true;  // drop this one; the next change retries
        }
        gpu_->context->Flush();
    }
    if (onFrame) onFrame(VideoFrame{gpu_, std::move(nv12), outW_, outH_, captureMicros});
    return true;
}

void ScreenCapture::run() {
    timeBeginPeriod(1);
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    {
        std::lock_guard lock(mutex_);
        active_ = settings_;
        settingsChanged_ = false;
    }
    bool dirty = false;               // a picture newer than the last delivered one
    uint64_t dirtyMicros = 0;         // its capture time
    uint64_t nextDue = 0;             // earliest time for the next delivery (µs)
    uint64_t failingSince = 0;
    std::string lastLoggedError;
    while (!stopping_) {
        {
            std::lock_guard lock(mutex_);
            if (settingsChanged_) {
                settingsChanged_ = false;
                const bool reopen = settings_.display != active_.display || settings_.displayIndex != active_.displayIndex ||
                                    settings_.maxWidth != active_.maxWidth;
                active_ = settings_;
                if (reopen) {
                    close();
                    deviceName_.clear();
                }
            }
        }
        if (!dup_) {
            std::string error;
            if (!open(error)) {
                if (!failingSince) failingSince = micros();
                // Transient states (secure desktop, mode switch) are only logged; a failure that
                // lasts is reported.
                if (micros() - failingSince > 3'000'000) {
                    report({CaptureStatus::Kind::Failed, error, ""});
                }
                if (lastLoggedError != error) {
                    log("capture: " + error);
                    lastLoggedError = error;
                }
                Sleep(250);
                continue;
            }
            failingSince = 0;
            lastLoggedError.clear();
            dirty = false;
        }
        const uint64_t interval = 1'000'000 / uint64_t(std::max(1, active_.fps));
        // AcquireNextFrame holds the device's lock while it waits, which would stall the encoder
        // (same device) for the whole timeout: poll without waiting, every millisecond.
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        HRESULT hr = dup_->AcquireNextFrame(0, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            if (!(dirty && micros() >= nextDue)) {
                Sleep(1);
                continue;
            }
        } else if (FAILED(hr)) {
            if (hr != DXGI_ERROR_ACCESS_LOST) log("capture: AcquireNextFrame: " + hresultText(hr));
            close();  // mode change, secure desktop, fullscreen switch: duplicate again
            continue;
        } else {
            bool changed = false;
            if (info.LastPresentTime.QuadPart != 0 && resource) {
                ComPtr<ID3D11Texture2D> frame;
                if (SUCCEEDED(resource.As(&frame))) {
                    GpuDevice::Lock lock(*gpu_);
                    gpu_->context->CopyResource(desktop_.Get(), frame.Get());
                }
                dirtyMicros = qpcTicksToMicros(info.LastPresentTime.QuadPart);
                changed = true;
            }
            if (info.LastMouseUpdateTime.QuadPart != 0) {
                const bool wasVisible = cursor_.visible();
                if (info.PointerShapeBufferSize > 0) {
                    pointerBuffer_.resize(info.PointerShapeBufferSize);
                    UINT size = 0;
                    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{};
                    if (SUCCEEDED(dup_->GetFramePointerShape(UINT(pointerBuffer_.size()), pointerBuffer_.data(), &size, &shape)))
                        cursor_.updateShape(*gpu_, shape, pointerBuffer_.data());
                }
                cursor_.setPosition(info.PointerPosition.Position.x, info.PointerPosition.Position.y,
                                    info.PointerPosition.Visible != FALSE);
                if (cursor_.visible() || wasVisible) {
                    if (!changed) dirtyMicros = qpcTicksToMicros(info.LastMouseUpdateTime.QuadPart);
                    changed = true;
                }
            }
            dup_->ReleaseFrame();
            if (changed) dirty = true;
        }
        if (dirty && micros() >= nextDue) {
            if (deliver(dirtyMicros)) {
                dirty = false;
                // Keep a steady cadence (a 144 Hz desktop at 60 fps gives 60, not 48).
                const uint64_t t = micros();
                nextDue = std::max(nextDue + interval, t > interval ? t - interval / 2 : 0);
                if (nextDue < t) nextDue = t + interval / 2;
            } else {
                nextDue = micros() + 2000;  // pool exhausted: retry shortly
            }
        }
    }
    close();
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    timeEndPeriod(1);
}

}  // namespace vr
