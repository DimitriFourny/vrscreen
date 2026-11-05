// A single-threaded event loop (the Windows counterpart of the Mac sender's serial dispatch
// queue): waits on registered handles (socket events), runs posted tasks and timers, all on its
// own thread. Networking and the pipeline decisions live on it.
#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace vr {

class EventLoop {
public:
    using Task = std::function<void()>;
    using Clock = std::chrono::steady_clock;

    EventLoop();
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void start();
    // Stops after the current task; pending tasks are dropped. Not from the loop thread.
    void stop();

    // Thread-safe.
    void post(Task task);
    void postDelayed(std::chrono::milliseconds delay, Task task);
    // Runs `task` on the loop and waits for it (or runs it inline on the loop thread).
    void runSync(const Task& task);

    // Loop thread only. `handler` runs whenever `handle` is signaled (WSAEventSelect events).
    void addHandle(HANDLE handle, Task handler);
    void removeHandle(HANDLE handle);

    bool onLoopThread() const { return std::this_thread::get_id() == threadId_; }

private:
    void run();

    HANDLE wake_;
    std::thread thread_;
    std::thread::id threadId_;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::vector<Task> tasks_;
    std::multimap<Clock::time_point, Task> timers_;
    std::vector<std::pair<HANDLE, Task>> handles_;  // loop thread only
};

}  // namespace vr
