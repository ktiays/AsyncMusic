//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "asyncmusic/serial_queue.hpp"

#include <future>

namespace asyncmusic {

SerialQueue::SerialQueue(std::string name) : name_(std::move(name)), thread_([this] { run(); }) {}

SerialQueue::~SerialQueue() {
    shutdown();
}

void SerialQueue::async(Task task) {
    asyncAfter(0, std::move(task));
}

void SerialQueue::asyncAfter(int64_t delayNanoseconds, Task task) {
    const auto due = std::chrono::steady_clock::now() + std::chrono::nanoseconds(delayNanoseconds > 0 ? delayNanoseconds : 0);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        entries_.push(Entry{due, nextOrder_++, std::move(task)});
    }
    changed_.notify_one();
}

void SerialQueue::sync(const Task& task) {
    std::promise<void> done;
    auto finished = done.get_future();
    async([&] {
        task();
        done.set_value();
    });
    // A queue that is shutting down drops the task; do not wait forever for it.
    while (finished.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
    }
}

bool SerialQueue::isCurrent() const {
    return std::this_thread::get_id() == thread_.get_id();
}

void SerialQueue::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_one();
    if (thread_.joinable() && !isCurrent()) {
        thread_.join();
    }
}

void SerialQueue::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        if (entries_.empty()) {
            changed_.wait(lock);
            continue;
        }
        const auto due = entries_.top().due;
        if (std::chrono::steady_clock::now() < due) {
            changed_.wait_until(lock, due);
            continue;
        }
        // priority_queue only hands out const references; the entry is popped right after.
        Task task = std::move(const_cast<Entry&>(entries_.top()).task);
        entries_.pop();
        lock.unlock();
        task();
        task = nullptr;
        lock.lock();
    }
    // Destroy dropped tasks outside the lock: what they capture may use the queue on its way out.
    auto dropped = std::move(entries_);
    lock.unlock();
}

}  // namespace asyncmusic
