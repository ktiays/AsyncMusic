//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "net.hpp"
#include "playback.hpp"
#include "protocol.hpp"
#include "ref_counted.hpp"
#include "serial_queue.hpp"
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

/// What the leader is currently playing from a file: its own ring buffer, which the server
/// forwards to followers frame for frame.
struct OutgoingStream {
    uint64_t id = 0;
    uint64_t source = 0;
    Ref<StreamBuffer> buffer;
    /// Where playback is, to start late joiners at the heard position.
    Ref<PlaybackPosition> position;
};

/// Leader side of audio streaming: accepts one TCP connection per follower and forwards the
/// leader's own decoded stream to each. A port of the Apple app's `AudioStreamServer`.
///
/// Followers learn the port through the clock sync channel (`onPortChange` →
/// `ClockSyncLeader::setAudioPort`) and connect to the address they already sync with. Nothing
/// is decoded twice and no extra pacing is needed: the leader's decoder keeps its buffer a few
/// seconds ahead of playback, so each sender forwards whatever it has not sent yet, a burst on
/// start and then a trickle at the playback rate. A follower joining mid-stream gets audio from
/// the position being heard.
///
/// Followers report how far their buffer reaches, which `waitUntilBuffered` uses to start
/// playback as soon as everyone has enough (adaptive start).
///
/// Callbacks run on the server's threads and must not destroy the server.
class AudioStreamServer {
public:
    struct Readiness {
        int ready = 0;
        int total = 0;
        bool timedOut = false;
    };

    explicit AudioStreamServer(AudioStreamCodec codec = AudioStreamCodec::alac);
    ~AudioStreamServer();
    AudioStreamServer(const AudioStreamServer&) = delete;
    AudioStreamServer& operator=(const AudioStreamServer&) = delete;

    /// Set before `start()`. The port is 0 while not listening.
    std::function<void(uint16_t port)> onPortChange;
    std::function<void(int count)> onReceiversChange;
    std::function<void(const std::string& message)> onError;

    void start();
    /// Stops and waits for the server's threads.
    void stop();

    /// Switches every follower to `stream` (none: nothing to send, e.g. metronome or stopped).
    void setStream(const std::optional<OutgoingStream>& stream);

    /// Calls `completion` once every connected follower has buffered `stream` up to `frame`, or
    /// after `timeout` seconds, whichever comes first. With no followers it completes right away.
    void waitUntilBuffered(uint64_t stream, int64_t frame, double timeout, std::function<void(Readiness)> completion);

private:
    class Sender;
    struct Waiter {
        uint64_t stream;
        int64_t frame;
        std::chrono::steady_clock::time_point deadline;
        std::function<void(Readiness)> completion;
    };

    void listen();
    void accept();
    void checkWaiters();
    void removeFinishedSenders();

    const AudioStreamCodec codec_;
    SerialQueue queue_;
    net::TCPListener listener_;
    std::thread acceptor_;
    std::atomic<bool> running_{false};

    std::mutex mutex_;
    std::optional<OutgoingStream> stream_;
    /// Increases with every `setStream`, so senders notice a switch.
    std::atomic<uint64_t> streamGeneration_{0};
    std::vector<std::shared_ptr<Sender>> senders_;
    std::vector<Waiter> waiters_;
};

}  // namespace asyncmusic
