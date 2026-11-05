#include "event_loop.h"

#include <algorithm>
#include <condition_variable>

namespace vr {

EventLoop::EventLoop() { wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); }

EventLoop::~EventLoop() {
    stop();
    CloseHandle(wake_);
}

void EventLoop::start() {
    thread_ = std::thread([this] { run(); });
}

void EventLoop::stop() {
    if (!thread_.joinable()) return;
    stopping_ = true;
    SetEvent(wake_);
    thread_.join();
}

void EventLoop::post(Task task) {
    {
        std::lock_guard lock(mutex_);
        tasks_.push_back(std::move(task));
    }
    SetEvent(wake_);
}

void EventLoop::postDelayed(std::chrono::milliseconds delay, Task task) {
    {
        std::lock_guard lock(mutex_);
        timers_.emplace(Clock::now() + delay, std::move(task));
    }
    SetEvent(wake_);
}

void EventLoop::runSync(const Task& task) {
    if (onLoopThread() || !thread_.joinable()) {
        task();
        return;
    }
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    post([&] {
        task();
        std::lock_guard lock(m);
        done = true;
        cv.notify_one();
    });
    std::unique_lock lock(m);
    cv.wait(lock, [&] { return done || stopping_.load(); });
}

void EventLoop::addHandle(HANDLE handle, Task handler) { handles_.emplace_back(handle, std::move(handler)); }

void EventLoop::removeHandle(HANDLE handle) {
    handles_.erase(std::remove_if(handles_.begin(), handles_.end(), [&](const auto& h) { return h.first == handle; }),
                   handles_.end());
}

void EventLoop::run() {
    threadId_ = std::this_thread::get_id();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    while (!stopping_) {
        // Due timers and posted tasks.
        std::vector<Task> due;
        DWORD timeout = INFINITE;
        {
            std::lock_guard lock(mutex_);
            due.swap(tasks_);
            const auto now = Clock::now();
            while (!timers_.empty() && timers_.begin()->first <= now) {
                due.push_back(std::move(timers_.begin()->second));
                timers_.erase(timers_.begin());
            }
            if (!timers_.empty()) {
                const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(timers_.begin()->first - now).count();
                timeout = DWORD(std::max<long long>(1, wait));
            }
        }
        for (Task& t : due) {
            if (stopping_) return;
            t();
        }
        if (!due.empty()) continue;  // tasks may have queued more work or changed handles

        std::vector<HANDLE> waitHandles{wake_};
        for (const auto& h : handles_) waitHandles.push_back(h.first);
        const DWORD n = DWORD(std::min<size_t>(waitHandles.size(), MAXIMUM_WAIT_OBJECTS));
        WaitForMultipleObjects(n, waitHandles.data(), FALSE, timeout);
        // Serve every signaled handle (the events are manual-reset WSA events, so checking them
        // does not consume them), so a busy task queue cannot starve the sockets.
        for (DWORD i = 1; i < n && !stopping_; ++i) {
            if (WaitForSingleObject(waitHandles[i], 0) != WAIT_OBJECT_0) continue;
            Task handler;  // a copy: the handler may remove itself
            for (const auto& h : handles_) {
                if (h.first == waitHandles[i]) handler = h.second;
            }
            if (handler) handler();
        }
    }
}

}  // namespace vr
