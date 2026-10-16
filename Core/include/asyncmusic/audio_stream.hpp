//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "net.hpp"
#include "ref_counted.hpp"
#include "stream_buffer.hpp"

namespace asyncmusic {

/// Follower side of audio streaming: receives the leader's stream over TCP into a
/// `StreamBuffer` that the renderer plays from exactly like a local file. A port of the Apple
/// app's `AudioStreamClient`.
///
/// The client's thread is the buffer's only writer. Each stream start creates a fresh buffer
/// (handed over through `onStream`), since streams are append-only and a new one usually starts
/// somewhere else in the file. After every chunk the client reports how far its buffer
/// reaches, which the leader uses to decide when everyone is ready to start.
///
/// Callbacks run on the client's thread and must not destroy the client.
class AudioStreamClient {
public:
    enum class State : uint8_t {
        connecting,
        connected,
        reconnecting,
        stopped,
    };

    /// Seconds of audio a buffer holds: the leader's read-ahead plus room behind playback.
    static constexpr double kBufferDuration = 12.0;

    explicit AudioStreamClient(net::Address endpoint);
    ~AudioStreamClient();
    AudioStreamClient(const AudioStreamClient&) = delete;
    AudioStreamClient& operator=(const AudioStreamClient&) = delete;

    /// Set before `start()`.
    std::function<void(State state, const std::string& reason)> onStateChange;
    /// A new stream began; play `source` from this buffer from now on.
    std::function<void(uint64_t source, const Ref<StreamBuffer>& buffer)> onStream;
    std::function<void(const std::string& text)> onLog;

    const net::Address& endpoint() const { return endpoint_; }

    void start();
    /// Stops and waits for the client's thread.
    void stop();

private:
    void run();
    void receive(net::TCPSocket& socket);
    /// Sleeps up to `milliseconds`, returning false if stopped meanwhile.
    bool pause(int milliseconds);
    void log(const std::string& text);

    const net::Address endpoint_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::condition_variable stopped_;
};

}  // namespace asyncmusic
