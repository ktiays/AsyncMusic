//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace asyncmusic {

/// A thread that runs tasks one at a time, in order, now or after a delay: the portable
/// stand-in for the dispatch queues the Apple app confines each component to.
///
/// Destroying the queue (or `shutdown()`) waits for the running task and drops the rest, so a
/// task must never destroy its own queue.
class SerialQueue {
public:
    using Task = std::function<void()>;

    explicit SerialQueue(std::string name);
    ~SerialQueue();
    SerialQueue(const SerialQueue&) = delete;
    SerialQueue& operator=(const SerialQueue&) = delete;

    void async(Task task);
    /// Runs `task` once `delayNanoseconds` have passed (on the steady clock).
    void asyncAfter(int64_t delayNanoseconds, Task task);
    /// Runs `task` and waits for it. Must not be called from the queue itself.
    void sync(const Task& task);
    bool isCurrent() const;
    /// Stops the thread; later tasks are ignored.
    void shutdown();

    const std::string& name() const { return name_; }

private:
    struct Entry {
        std::chrono::steady_clock::time_point due;
        uint64_t order;
        Task task;
    };
    struct Later {
        bool operator()(const Entry& a, const Entry& b) const { return a.due != b.due ? a.due > b.due : a.order > b.order; }
    };

    void run();

    const std::string name_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::priority_queue<Entry, std::vector<Entry>, Later> entries_;
    uint64_t nextOrder_ = 0;
    bool stopping_ = false;
    std::thread thread_;
};

}  // namespace asyncmusic
