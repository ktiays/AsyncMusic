//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include <algorithm>
#include <cmath>
#include <cstring>

#include "asyncmusic/alac.hpp"
#include "asyncmusic/audio_stream.hpp"
#include "asyncmusic/host_clock.hpp"

namespace asyncmusic {

/// Forwards the current stream to one follower, on its own thread.
class AudioStreamServer::Sender {
public:
    Sender(AudioStreamServer& server, net::TCPSocket socket) : server_(server), socket_(std::move(socket)), scratch_(2 * kALACFramesPerPacket) {}
    ~Sender() { stop(); }

    void start() {
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        stopping_ = true;
        if (thread_.joinable()) thread_.join();
    }

    bool isFinished() const { return finished_.load(); }

    bool hasBuffered(uint64_t stream, int64_t frame) {
        std::lock_guard<std::mutex> lock(reportMutex_);
        return hasReport_ && reportedStream_ == stream && reportedEnd_ >= frame;
    }

private:
    /// Sends handed to the network before checking for reports again.
    static constexpr int kBurst = 8;

    void run() {
        uint64_t seen = UINT64_MAX;
        while (!stopping_ && !failed_) {
            const uint64_t generation = server_.streamGeneration_.load();
            if (generation != seen) {
                seen = generation;
                std::optional<OutgoingStream> stream;
                {
                    std::lock_guard<std::mutex> lock(server_.mutex_);
                    stream = server_.stream_;
                }
                begin(stream);
            }
            if (!readReports()) break;
            int sent = 0;
            while (sent < kBurst && !failed_ && pump()) ++sent;
            // Nothing to send: wait for the decoder to append more (or for a report).
            if (sent == 0 && !failed_) socket_.waitReadable(20);
        }
        finished_ = true;
        AudioStreamServer& server = server_;
        server.queue_.async([&server] {
            server.removeFinishedSenders();
            server.checkWaiters();
        });
    }

    /// Announces `stream` and starts sending it from the frame being heard now (or from its
    /// first frame, if playback has not reached it yet).
    void begin(const std::optional<OutgoingStream>& stream) {
        stream_ = stream;
        if (!stream_) return;
        StreamBuffer& buffer = *stream_->buffer;
        const FrameRange range = buffer.validRange();
        const double rate = buffer.sampleRate();
        int64_t heard = 0;
        const int64_t heardFrame = stream_->position->heardMediaTime(hostClockNanoseconds(), heard)
            ? static_cast<int64_t>(std::floor(static_cast<double>(heard) * rate / 1e9))
            : range.lowerBound;
        // A little earlier than what is heard, to cover the follower's output latency.
        nextFrame_ = std::min(std::max(range.lowerBound, heardFrame - static_cast<int64_t>(0.1 * rate)), range.upperBound);

        Bytes cookie;
        if (server_.codec_ == AudioStreamCodec::alac) {
            if (!encoder_ || encoder_->sampleRate() != rate) encoder_ = std::make_unique<ALACEncoder>(rate);
            cookie = encoder_->magicCookie();
        } else {
            encoder_.reset();
        }
        send(AudioStreamMessage::makeStreamStart(stream_->id, stream_->source, rate, nextFrame_, encoder_ ? AudioStreamCodec::alac : AudioStreamCodec::pcm16, cookie));
    }

    /// Sends the next chunk; false when there is nothing to send yet.
    bool pump() {
        if (!stream_) return false;
        StreamBuffer& buffer = *stream_->buffer;
        const FrameRange range = buffer.validRange();
        if (nextFrame_ < range.lowerBound) {
            // This follower fell so far behind that the frames it needs were overwritten.
            begin(stream_);
            return false;
        }
        const int count = static_cast<int>(std::min<int64_t>(kALACFramesPerPacket, range.upperBound - nextFrame_));
        if (count <= 0) return false;
        // Fails only if the decoder overwrote these frames mid-copy; the next call then finds
        // `nextFrame_` lapped and restarts.
        if (!buffer.readInterleavedPCM16(nextFrame_, count, scratch_.data())) return true;

        Bytes payload;
        if (encoder_) {
            if (!encoder_->encode(scratch_.data(), count, payload)) {
                nextFrame_ += count;
                return true;
            }
        } else {
            payload.resize(static_cast<size_t>(count) * 4);
            for (int i = 0; i < 2 * count; ++i) {
                const auto sample = static_cast<uint16_t>(scratch_[static_cast<size_t>(i)]);
                payload[static_cast<size_t>(2 * i)] = static_cast<uint8_t>(sample & 0xFF);
                payload[static_cast<size_t>(2 * i + 1)] = static_cast<uint8_t>(sample >> 8);
            }
        }
        send(AudioStreamMessage::makeAudio(stream_->id, nextFrame_, count, payload));
        nextFrame_ += count;
        return true;
    }

    /// Reads whatever reports have arrived; false once the connection is gone.
    bool readReports() {
        while (true) {
            const int ready = socket_.waitReadable(0);
            if (ready < 0) return false;
            if (ready == 0) return true;
            uint8_t header[AudioStreamMessage::kHeaderLength];
            if (socket_.readExactly(header, sizeof(header), stopping_) != net::TCPSocket::ReadResult::ok) return false;
            const int64_t length = AudioStreamMessage::bodyLength(header, sizeof(header));
            if (length <= 0) return false;
            Bytes body(static_cast<size_t>(length));
            if (socket_.readExactly(body.data(), body.size(), stopping_) != net::TCPSocket::ReadResult::ok) return false;
            AudioStreamMessage message;
            if (!AudioStreamMessage::decodeBody(body.data(), body.size(), message) || message.type != AudioStreamMessage::Type::bufferReport) continue;
            {
                std::lock_guard<std::mutex> lock(reportMutex_);
                hasReport_ = true;
                reportedStream_ = message.streamID;
                reportedEnd_ = message.frame;
            }
            AudioStreamServer& server = server_;
            server.queue_.async([&server] { server.checkWaiters(); });
        }
    }

    void send(const AudioStreamMessage& message) {
        const Bytes bytes = message.framed();
        if (!socket_.sendAll(bytes.data(), bytes.size())) failed_ = true;
    }

    AudioStreamServer& server_;
    net::TCPSocket socket_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> finished_{false};
    bool failed_ = false;

    std::optional<OutgoingStream> stream_;
    int64_t nextFrame_ = 0;
    std::unique_ptr<ALACEncoder> encoder_;
    std::vector<int16_t> scratch_;

    std::mutex reportMutex_;
    bool hasReport_ = false;
    uint64_t reportedStream_ = 0;
    int64_t reportedEnd_ = 0;
};

AudioStreamServer::AudioStreamServer(AudioStreamCodec codec) : codec_(codec), queue_("asyncmusic.audio-stream.server") {}

AudioStreamServer::~AudioStreamServer() {
    stop();
    queue_.shutdown();
}

void AudioStreamServer::start() {
    if (running_.exchange(true)) return;
    net::startup();
    const bool listening = listener_.listen(net::Address::fromString("::", 0)->family(), 0)
        || listener_.listen(net::Address::fromString("0.0.0.0", 0)->family(), 0);
    if (!listening) {
        if (onError) onError(net::lastErrorDescription());
        if (onPortChange) onPortChange(0);
        return;
    }
    if (onPortChange) onPortChange(listener_.localAddress().port());
    acceptor_ = std::thread([this] { accept(); });
}

void AudioStreamServer::stop() {
    if (!running_.exchange(false)) return;
    if (acceptor_.joinable()) acceptor_.join();
    listener_.close();
    if (onPortChange) onPortChange(0);
    std::vector<std::shared_ptr<Sender>> senders;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        senders.swap(senders_);
    }
    for (auto& sender : senders) sender->stop();
    if (onReceiversChange) onReceiversChange(0);
    queue_.sync([this] { checkWaiters(); });
}

void AudioStreamServer::accept() {
    while (running_) {
        net::TCPSocket socket = listener_.accept(200);
        if (!socket.isOpen()) continue;
        auto sender = std::make_shared<Sender>(*this, std::move(socket));
        size_t count = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            senders_.push_back(sender);
            count = senders_.size();
        }
        sender->start();
        if (onReceiversChange) onReceiversChange(static_cast<int>(count));
    }
}

void AudioStreamServer::setStream(const std::optional<OutgoingStream>& stream) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stream_ = stream;
    }
    ++streamGeneration_;
    queue_.async([this] { checkWaiters(); });
}

void AudioStreamServer::waitUntilBuffered(uint64_t stream, int64_t frame, double timeout, std::function<void(Readiness)> completion) {
    queue_.async([this, stream, frame, timeout, completion = std::move(completion)]() mutable {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(timeout));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            waiters_.push_back(Waiter{stream, frame, deadline, std::move(completion)});
        }
        checkWaiters();
        queue_.asyncAfter(static_cast<int64_t>(timeout * 1e9) + 1'000'000, [this] { checkWaiters(); });
    });
}

/// Runs on `queue_`.
void AudioStreamServer::removeFinishedSenders() {
    std::vector<std::shared_ptr<Sender>> finished;
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto sender = senders_.begin(); sender != senders_.end();) {
            if ((*sender)->isFinished()) {
                finished.push_back(*sender);
                sender = senders_.erase(sender);
            } else {
                ++sender;
            }
        }
        count = senders_.size();
    }
    if (finished.empty()) return;
    for (auto& sender : finished) sender->stop();
    if (onReceiversChange) onReceiversChange(static_cast<int>(count));
}

/// Runs on `queue_`.
void AudioStreamServer::checkWaiters() {
    std::vector<std::pair<std::function<void(Readiness)>, Readiness>> done;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (waiters_.empty()) return;
        const auto now = std::chrono::steady_clock::now();
        for (auto waiter = waiters_.begin(); waiter != waiters_.end();) {
            // Waiting for a stream that is no longer current is pointless.
            const bool current = stream_ && stream_->id == waiter->stream;
            int total = 0;
            int ready = 0;
            for (const auto& sender : senders_) {
                if (sender->isFinished()) continue;
                ++total;
                if (sender->hasBuffered(waiter->stream, waiter->frame)) ++ready;
            }
            const bool timedOut = now >= waiter->deadline;
            if (current && ready < total && !timedOut) {
                ++waiter;
                continue;
            }
            done.emplace_back(std::move(waiter->completion), Readiness{ready, total, timedOut && ready < total});
            waiter = waiters_.erase(waiter);
        }
    }
    for (auto& [completion, readiness] : done) completion(readiness);
}

}  // namespace asyncmusic
