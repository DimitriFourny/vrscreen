// --source test: a synthetic picture (no capture permission or display needed) and beeps.
#include <windows.h>
#include <avrt.h>
#include <d3d11_1.h>
#include <mmsystem.h>

#include <cmath>
#include <vector>

#include "capture.h"
#include "log.h"
#include "util.h"

namespace vr {

void TestPattern::start() {
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void TestPattern::stop() {
    stopping_ = true;
    if (thread_.joinable()) thread_.join();
}

void TestPattern::update(const CaptureSettings& settings) {
    std::lock_guard lock(mutex_);
    settings_ = settings;
}

void TestPattern::run() {
    timeBeginPeriod(1);
    std::string error;
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || factory->EnumAdapters1(0, &adapter) != S_OK) {
        if (onStatus) onStatus({CaptureStatus::Kind::Failed, "no graphics adapter", ""});
        return;
    }
    auto gpu = createGpuDevice(adapter.Get(), error);
    if (!gpu) {
        if (onStatus) onStatus({CaptureStatus::Kind::Failed, error, ""});
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = UINT(width_);
    desc.Height = UINT(height_);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> canvas;
    ComPtr<ID3D11RenderTargetView> target;
    gpu->device->CreateTexture2D(&desc, nullptr, &canvas);
    if (canvas) gpu->device->CreateRenderTargetView(canvas.Get(), nullptr, &target);
    ComPtr<ID3D11DeviceContext1> context1;
    gpu->context.As(&context1);
    if (!target || !context1) {
        if (onStatus) onStatus({CaptureStatus::Kind::Failed, "test pattern textures", ""});
        return;
    }
    VideoConverter converter;
    std::shared_ptr<TexturePool> pool;
    logf("capture: test pattern %dx%d on %s (white flash + 1 kHz beep once per second)", width_, height_,
         gpu->adapterName.c_str());
    if (onStatus) onStatus({CaptureStatus::Kind::Running, "", "test pattern"});

    // Audio: 48 kHz stereo, 10 ms blocks, a 60 ms 1 kHz beep (-10.5 dBFS) starting at each flash.
    constexpr int kRate = 48000, kBlock = 480;
    const uint64_t start = nowMicros();
    uint64_t audioFrame = 0;  // frames synthesized since start
    std::vector<float> audio(kBlock * 2);

    uint64_t frameIndex = 0;
    uint64_t lastSecond = UINT64_MAX;
    while (!stopping_) {
        CaptureSettings s;
        {
            std::lock_guard lock(mutex_);
            s = settings_;
        }
        const uint64_t interval = 1'000'000 / uint64_t(std::max(1, s.fps));
        const uint64_t due = start + frameIndex * interval;
        uint64_t now = nowMicros();
        // Audio up to now.
        while (onAudio && start + (audioFrame + kBlock) * 1'000'000 / kRate <= now) {
            for (int i = 0; i < kBlock; ++i) {
                const uint64_t t = (audioFrame + uint64_t(i)) * 1'000'000 / kRate;  // µs since start
                // The flash is the first frame at or after each whole second.
                const uint64_t flashAt = (t / 1'000'000 * 1'000'000 + interval - 1) / interval * interval;
                const bool beep = t >= flashAt && t - flashAt < 60'000;
                const float v = beep ? 0.2985f * std::sin(2 * 3.14159265f * 1000.0f * float(t - flashAt) / 1e6f) : 0.0f;
                audio[size_t(2 * i)] = audio[size_t(2 * i + 1)] = v;
            }
            onAudio(audio.data(), kBlock, start + audioFrame * 1'000'000 / kRate);
            audioFrame += kBlock;
        }
        if (now < due) {
            Sleep(DWORD(std::min<uint64_t>((due - now) / 1000 + 1, 10)));
            continue;
        }
        // Frame time: the scheduled time (exact, for A/V checks).
        const uint64_t t = due - start;
        const uint64_t second = t / 1'000'000;
        const bool flash = second != lastSecond;
        lastSecond = second;
        int outW, outH;
        fitSize(width_, height_, s.maxWidth, outW, outH);
        if (!pool || pool->width() != outW || pool->height() != outH)
            pool = std::make_shared<TexturePool>(gpu, outW, outH, DXGI_FORMAT_NV12,
                                                 D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER, 4);
        auto nv12 = pool->acquire();
        ++frameIndex;
        if (!nv12) continue;
        {
            GpuDevice::Lock lock(*gpu);
            const float white[4] = {1, 1, 1, 1};
            const float grey[4] = {0.12f, 0.12f, 0.16f, 1};
            const float bar[4] = {0.9f, 0.45f, 0.1f, 1};
            const float stripe[4] = {0.2f, 0.6f, 0.9f, 1};
            context1->ClearView(target.Get(), flash ? white : grey, nullptr, 0);
            if (!flash) {
                // A bar sweeping across once per second and a stripe per frame (motion check).
                const int x = int((t % 1'000'000) * uint64_t(width_) / 1'000'000);
                const D3D11_RECT r{x, 0, std::min(width_, x + width_ / 20), height_};
                context1->ClearView(target.Get(), bar, &r, 1);
                const int y = int(frameIndex % 60) * height_ / 60;
                const D3D11_RECT r2{0, y, width_, std::min(height_, y + height_ / 60)};
                context1->ClearView(target.Get(), stripe, &r2, 1);
            }
            if (!converter.convert(*gpu, canvas.Get(), width_, height_, nv12.get(), outW, outH, error)) {
                log("capture: " + error);
                continue;
            }
            gpu->context->Flush();
        }
        if (onFrame) onFrame(VideoFrame{gpu, std::move(nv12), outW, outH, due});
        // Skip frames we are late for (keep the schedule).
        now = nowMicros();
        if (now > start + frameIndex * interval + interval) frameIndex = (now - start) / interval + 1;
    }
    timeEndPeriod(1);
}

}  // namespace vr
