//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "asyncmusic/share_discovery.hpp"

#include <algorithm>
#include <chrono>
#include <random>

#include "text.hpp"

namespace asyncmusic {

namespace {

constexpr uint8_t kMagic0 = 'A';
constexpr uint8_t kMagic1 = 'D';
constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderLength = 4;
/// Shortest share lifetime a listener accepts, and longest; outside that an announcement is
/// either a mistake or would keep a dead share around too long.
constexpr uint16_t kMinimumLifetime = 1'000;
constexpr uint16_t kMaximumLifetime = 60'000;
/// A jump in sequence numbers larger than this is a restarted sender, not lost datagrams.
constexpr uint32_t kMaximumSequenceGap = 1'000;
/// The worker never sleeps longer than this, so commands and expiry are handled promptly.
constexpr int64_t kMaximumWait = 100'000'000;
/// A loop iteration this much later than planned means the device slept.
constexpr int64_t kSleepGap = 3'000'000'000;
constexpr int64_t kNetworkCheckInterval = 2'000'000'000;
constexpr int64_t kSocketRetryInterval = 1'000'000'000;

int64_t steadyNanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void put(Bytes& bytes, uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void putText(Bytes& bytes, const std::string& text) {
    const std::string cut = utf8Prefix(text, kShareMaximumTextLength);
    bytes.push_back(static_cast<uint8_t>(cut.size()));
    bytes.insert(bytes.end(), cut.begin(), cut.end());
}

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool read(uint64_t& value, size_t width) {
        if (position_ + width > size_) return false;
        value = 0;
        for (size_t i = 0; i < width; ++i) value |= static_cast<uint64_t>(data_[position_ + i]) << (8 * i);
        position_ += width;
        return true;
    }
    template <typename T>
    bool read(T& value) {
        uint64_t bits = 0;
        if (!read(bits, sizeof(T))) return false;
        value = static_cast<T>(bits);
        return true;
    }
    bool readText(std::string& text) {
        uint8_t length = 0;
        if (!read(length) || position_ + length > size_) return false;
        text.assign(reinterpret_cast<const char*>(data_ + position_), length);
        position_ += length;
        return true;
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t position_ = 0;
};

}  // namespace

// MARK: - ShareInfo, ShareMessage

bool ShareInfo::operator==(const ShareInfo& other) const {
    return name == other.name && source == other.source && sourceKind == other.sourceKind && clockPort == other.clockPort && listenerCount == other.listenerCount;
}

ShareMessage ShareMessage::makeAnnounce(uint64_t device, uint64_t share, uint32_t sequence, uint32_t revision, uint16_t lifetimeMilliseconds, const ShareInfo& info) {
    ShareMessage message;
    message.type = Type::announce;
    message.device = device;
    message.share = share;
    message.sequence = sequence;
    message.revision = revision;
    message.lifetimeMilliseconds = lifetimeMilliseconds;
    message.info = info;
    return message;
}

ShareMessage ShareMessage::makeGoodbye(uint64_t device, uint64_t share) {
    ShareMessage message;
    message.type = Type::goodbye;
    message.device = device;
    message.share = share;
    return message;
}

ShareMessage ShareMessage::makeQuery(uint64_t device) {
    ShareMessage message;
    message.type = Type::query;
    message.device = device;
    return message;
}

Bytes ShareMessage::encode() const {
    Bytes bytes;
    bytes.reserve(64 + 2 * kShareMaximumTextLength);
    bytes.push_back(kMagic0);
    bytes.push_back(kMagic1);
    bytes.push_back(kVersion);
    bytes.push_back(static_cast<uint8_t>(type));
    put(bytes, device, 8);
    switch (type) {
    case Type::announce:
        put(bytes, share, 8);
        put(bytes, sequence, 4);
        put(bytes, revision, 4);
        put(bytes, lifetimeMilliseconds, 2);
        put(bytes, info.clockPort, 2);
        put(bytes, info.listenerCount, 2);
        bytes.push_back(static_cast<uint8_t>(info.sourceKind));
        putText(bytes, info.name);
        putText(bytes, info.source);
        break;
    case Type::goodbye:
        put(bytes, share, 8);
        break;
    case Type::query:
        break;
    }
    return bytes;
}

bool ShareMessage::decode(const uint8_t* data, size_t size, ShareMessage& message) {
    if (data == nullptr || size < kHeaderLength || data[0] != kMagic0 || data[1] != kMagic1 || data[2] != kVersion) return false;
    Reader reader(data + kHeaderLength, size - kHeaderLength);
    ShareMessage result;
    result.type = static_cast<Type>(data[3]);
    if (!reader.read(result.device)) return false;
    switch (result.type) {
    case Type::announce: {
        uint8_t kind = 0;
        if (!reader.read(result.share) || !reader.read(result.sequence) || !reader.read(result.revision) || !reader.read(result.lifetimeMilliseconds)
            || !reader.read(result.info.clockPort) || !reader.read(result.info.listenerCount) || !reader.read(kind) || !reader.readText(result.info.name)
            || !reader.readText(result.info.source)) {
            return false;
        }
        // A kind this version does not know is still a share; it just cannot be described.
        result.info.sourceKind = kind <= static_cast<uint8_t>(ShareSourceKind::system) ? static_cast<ShareSourceKind>(kind) : ShareSourceKind::unknown;
        if (result.share == 0 || result.info.clockPort == 0) return false;
        break;
    }
    case Type::goodbye:
        if (!reader.read(result.share) || result.share == 0) return false;
        break;
    case Type::query:
        break;
    default:
        return false;
    }
    message = std::move(result);
    return true;
}

// MARK: - ShareDiscovery

ShareDiscovery* ShareDiscovery::create(uint64_t device) {
    return new ShareDiscovery(device);
}

ShareDiscovery::ShareDiscovery(uint64_t device, Configuration configuration) : ShareDiscovery(device, nullptr, std::move(configuration)) {}

ShareDiscovery::ShareDiscovery(uint64_t device, std::function<void(const ShareEvent&)> onEvent, Configuration configuration)
    : device_(device), configuration_(std::move(configuration)), onEvent_(std::move(onEvent)) {
    std::random_device entropy;
    random_ = (static_cast<uint64_t>(entropy()) << 32) ^ entropy() ^ static_cast<uint64_t>(steadyNanoseconds()) ^ device;
    thread_ = std::thread([this] { run(); });
}

ShareDiscovery::~ShareDiscovery() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        commands_.push_back({Command::Kind::stop, {}});
        exiting_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void ShareDiscovery::listen() {
    std::lock_guard<std::mutex> lock(mutex_);
    commands_.push_back({Command::Kind::listen, {}});
    wake_.notify_all();
}

void ShareDiscovery::share(const ShareInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    commands_.push_back({Command::Kind::share, info});
    wake_.notify_all();
}

void ShareDiscovery::update(const ShareInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    commands_.push_back({Command::Kind::update, info});
    wake_.notify_all();
}

void ShareDiscovery::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    commands_.push_back({Command::Kind::stop, {}});
    wake_.notify_all();
}

void ShareDiscovery::refresh() {
    std::lock_guard<std::mutex> lock(mutex_);
    commands_.push_back({Command::Kind::refresh, {}});
    wake_.notify_all();
}

std::vector<DiscoveredShare> ShareDiscovery::shares() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DiscoveredShare> result;
    result.reserve(shares_.size());
    for (const auto& entry : shares_) result.push_back(entry.second);
    // Newest first: the order a UI lists them in.
    std::sort(result.begin(), result.end(), [](const DiscoveredShare& a, const DiscoveredShare& b) { return a.firstSeen > b.firstSeen; });
    return result;
}

uint64_t ShareDiscovery::generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

ShareDiscovery::Status ShareDiscovery::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Status status = status_;
    status.mode = mode_;
    status.share = mode_ == Mode::sharing ? share_ : 0;
    return status;
}

uint64_t ShareDiscovery::nextRandom() {
    // SplitMix64.
    random_ += 0x9E3779B97F4A7C15ull;
    uint64_t z = random_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

int64_t ShareDiscovery::nextInterval() {
    const int64_t base = fastRemaining_ > 0 ? configuration_.fastInterval : configuration_.steadyInterval;
    const double unit = static_cast<double>(nextRandom() >> 11) / static_cast<double>(1ull << 53);  // [0, 1)
    const double factor = 1.0 + configuration_.jitter * (2.0 * unit - 1.0);
    return static_cast<int64_t>(static_cast<double>(base) * factor);
}

void ShareDiscovery::run() {
    std::vector<uint8_t> buffer(2048);
    std::vector<ShareEvent> events;
    lastLoop_ = steadyNanoseconds();
    for (;;) {
        std::vector<Command> commands;
        bool exiting = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            commands.swap(commands_);
            exiting = exiting_;
        }
        int64_t now = steadyNanoseconds();
        events.clear();
        for (const auto& command : commands) apply(command, now, events);

        Mode mode;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            mode = mode_;
        }
        if (exiting && outgoing_.empty()) break;

        if (now - lastLoop_ > kSleepGap) {
            // Back from sleep: what was heard before may be gone, and others may have missed us.
            if (mode == Mode::listening) sendQueries(now);
            if (mode == Mode::sharing) restartAnnouncing(now);
        }
        lastLoop_ = now;

        if (mode != Mode::off || !outgoing_.empty()) {
            ensureSocket(now);
            checkNetwork(now);
        } else if (socket_.isOpen()) {
            closeSocket();
        }

        // Due datagrams, in order.
        std::sort(outgoing_.begin(), outgoing_.end(), [](const Outgoing& a, const Outgoing& b) { return a.due < b.due; });
        size_t sent = 0;
        while (sent < outgoing_.size() && outgoing_[sent].due <= now) {
            send(outgoing_[sent].message);
            ++sent;
        }
        outgoing_.erase(outgoing_.begin(), outgoing_.begin() + static_cast<std::ptrdiff_t>(sent));

        if (mode == Mode::sharing && now >= nextAnnouncement_) announce(now);
        if (mode == Mode::listening) expire(now, events);

        for (const auto& event : events) {
            if (onEvent_) onEvent_(event);
        }
        events.clear();

        int64_t wait = kMaximumWait;
        if (!outgoing_.empty()) wait = std::min(wait, std::max<int64_t>(0, outgoing_.front().due - now));
        if (mode == Mode::sharing) wait = std::min(wait, std::max<int64_t>(0, nextAnnouncement_ - now));

        if (socket_.isOpen()) {
            // Read everything that arrives until the next thing to do.
            const int64_t until = now + wait;
            int64_t remaining = wait;
            do {
                net::Address from;
                const int received = socket_.receive(buffer.data(), buffer.size(), static_cast<int>(remaining / 1'000'000), &from);
                if (received < 0) {
                    // On some systems an ICMP error lands here; reopen rather than spin on it.
                    closeSocket();
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.error = "Receiving failed: " + net::lastErrorDescription();
                    break;
                }
                if (received == 0) break;
                const int64_t at = steadyNanoseconds();
                receive(buffer.data(), static_cast<size_t>(received), from, at, events);
                for (const auto& event : events) {
                    if (onEvent_) onEvent_(event);
                }
                events.clear();
                remaining = until - steadyNanoseconds();
                std::lock_guard<std::mutex> lock(mutex_);
                if (!commands_.empty()) break;
            } while (remaining > 1'000'000);
        } else {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock, std::chrono::nanoseconds(wait), [this] { return !commands_.empty(); });
        }
    }
    closeSocket();
}

void ShareDiscovery::apply(const Command& command, int64_t now, std::vector<ShareEvent>& events) {
    Mode mode;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mode = mode_;
    }
    switch (command.kind) {
    case Command::Kind::listen:
        if (mode == Mode::sharing) scheduleRepeated(ShareMessage::makeGoodbye(device_, share_), now);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            mode_ = Mode::listening;
            share_ = 0;
        }
        sendQueries(now);
        break;
    case Command::Kind::share: {
        if (mode == Mode::listening) forgetShares(events);
        if (mode == Mode::sharing) scheduleRepeated(ShareMessage::makeGoodbye(device_, share_), now);
        uint64_t share = 0;
        while (share == 0) share = nextRandom();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            mode_ = Mode::sharing;
            share_ = share;
            info_ = command.info;
            revision_ = 1;
            status_.announcementsSent = 0;
            status_.queriesAnswered = 0;
        }
        restartAnnouncing(now);
        break;
    }
    case Command::Kind::update:
        if (mode == Mode::sharing && command.info != info_) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                info_ = command.info;
                ++revision_;
            }
            nextAnnouncement_ = now;
        }
        break;
    case Command::Kind::stop:
        if (mode == Mode::sharing) scheduleRepeated(ShareMessage::makeGoodbye(device_, share_), now);
        if (mode == Mode::listening) forgetShares(events);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            mode_ = Mode::off;
            share_ = 0;
        }
        break;
    case Command::Kind::refresh:
        if (mode == Mode::listening) sendQueries(now);
        break;
    }
}

void ShareDiscovery::ensureSocket(int64_t now) {
    if (socket_.isOpen() || now < nextSocketAttempt_) return;
    nextSocketAttempt_ = now + kSocketRetryInterval;
    net::UDPSocket socket;
    std::string error;
    if (!socket.open(net::Address::fromString("0.0.0.0", 0)->family())) {
        error = "Could not open a socket: " + net::lastErrorDescription();
    } else if (!socket.allowPortSharing() || !socket.enableBroadcast()) {
        error = "Could not configure the socket: " + net::lastErrorDescription();
    } else if (!socket.bind(configuration_.port)) {
        error = "Could not use port " + std::to_string(configuration_.port) + ": " + net::lastErrorDescription();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (error.empty()) {
        socket_ = std::move(socket);
        status_.isReady = true;
        status_.error.clear();
    } else {
        status_.isReady = false;
        status_.error = error;
    }
}

void ShareDiscovery::closeSocket() {
    socket_.close();
    std::lock_guard<std::mutex> lock(mutex_);
    status_.isReady = false;
}

void ShareDiscovery::checkNetwork(int64_t now) {
    if (!configuration_.targets.empty() || now < nextNetworkCheck_) return;
    nextNetworkCheck_ = now + kNetworkCheckInterval;
    std::vector<net::Address> targets = net::ipv4BroadcastAddresses();
    if (targets == lastTargets_) return;
    const bool wasConnected = !lastTargets_.empty();
    lastTargets_ = std::move(targets);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.targets.clear();
        for (const auto& target : lastTargets_) status_.targets.push_back(target.host());
    }
    // A network came or changed (not just the first look): tell it right away.
    if (wasConnected || !lastTargets_.empty()) {
        Mode mode;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            mode = mode_;
        }
        if (mode == Mode::listening) sendQueries(now);
        if (mode == Mode::sharing) restartAnnouncing(now);
    }
}

void ShareDiscovery::send(const ShareMessage& message) {
    if (configuration_.dropOutgoing && configuration_.dropOutgoing(message)) return;
    if (!socket_.isOpen()) return;
    const Bytes datagram = message.encode();
    if (!configuration_.targets.empty()) {
        for (const auto& target : configuration_.targets) socket_.sendTo(datagram.data(), datagram.size(), target);
        return;
    }
    if (lastTargets_.empty()) {
        // No interface found to broadcast on; the limited broadcast may still go out.
        static const auto everyone = net::Address::fromString("255.255.255.255", 0);
        if (everyone) socket_.sendTo(datagram.data(), datagram.size(), everyone->withPort(configuration_.port));
        return;
    }
    for (const auto& target : lastTargets_) socket_.sendTo(datagram.data(), datagram.size(), target.withPort(configuration_.port));
}

void ShareDiscovery::schedule(const ShareMessage& message, int64_t due) {
    outgoing_.push_back({due, message});
}

void ShareDiscovery::scheduleRepeated(const ShareMessage& message, int64_t first) {
    for (int i = 0; i < std::max(1, configuration_.repeats); ++i) schedule(message, first + i * configuration_.repeatSpacing);
}

void ShareDiscovery::sendQueries(int64_t now) {
    // Queries already waiting cover this one.
    for (const auto& item : outgoing_) {
        if (item.message.type == ShareMessage::Type::query) return;
    }
    scheduleRepeated(ShareMessage::makeQuery(device_), now);
}

void ShareDiscovery::restartAnnouncing(int64_t now) {
    fastRemaining_ = std::max(1, configuration_.fastCount);
    nextAnnouncement_ = now;
}

void ShareDiscovery::announce(int64_t now) {
    ShareMessage message;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto lifetime = static_cast<uint16_t>(std::clamp<int64_t>(configuration_.lifetime / 1'000'000, kMinimumLifetime, kMaximumLifetime));
        message = ShareMessage::makeAnnounce(device_, share_, ++sequence_, revision_, lifetime, info_);
        ++status_.announcementsSent;
    }
    send(message);
    if (!queriers_.empty() && socket_.isOpen() && !(configuration_.dropOutgoing && configuration_.dropOutgoing(message))) {
        const Bytes datagram = message.encode();
        for (const auto& querier : queriers_) socket_.sendTo(datagram.data(), datagram.size(), querier);
    }
    queriers_.clear();
    if (fastRemaining_ > 0) --fastRemaining_;
    nextAnnouncement_ = now + nextInterval();
}

void ShareDiscovery::receive(const uint8_t* data, size_t size, const net::Address& from, int64_t now, std::vector<ShareEvent>& events) {
    ShareMessage message;
    if (!ShareMessage::decode(data, size, message) || message.device == device_) return;

    std::lock_guard<std::mutex> lock(mutex_);
    switch (message.type) {
    case ShareMessage::Type::query:
        if (mode_ != Mode::sharing) break;
        // The answer also goes straight to whoever asked: Wi-Fi acknowledges and retransmits
        // unicast frames, but not broadcasts, so this is the copy most likely to arrive.
        if (std::find(queriers_.begin(), queriers_.end(), from) == queriers_.end() && queriers_.size() < 16) queriers_.push_back(from);
        if (now - lastQueryAnswer_ >= configuration_.minimumQueryAnswerSpacing) {
            lastQueryAnswer_ = now;
            ++status_.queriesAnswered;
            // Answer soon, a little apart from other sharers answering the same query.
            const int64_t delay = static_cast<int64_t>(nextRandom() % 50'000'000);
            nextAnnouncement_ = std::min(nextAnnouncement_, now + delay);
        }
        break;

    case ShareMessage::Type::goodbye: {
        if (mode_ != Mode::listening) break;
        ended_[message.share] = now + configuration_.endedMemory;
        const auto found = shares_.find(message.device);
        if (found != shares_.end() && found->second.share == message.share) {
            events.push_back({ShareEvent::Kind::disappeared, ShareEvent::Reason::ended, found->second});
            shares_.erase(found);
            ++generation_;
        }
        break;
    }

    case ShareMessage::Type::announce: {
        if (mode_ != Mode::listening) break;
        const auto ended = ended_.find(message.share);
        if (ended != ended_.end() && ended->second > now) break;
        const int64_t lifetime = static_cast<int64_t>(std::clamp(message.lifetimeMilliseconds, kMinimumLifetime, kMaximumLifetime)) * 1'000'000;
        const net::Address address = from.withPort(message.info.clockPort);

        auto found = shares_.find(message.device);
        if (found != shares_.end() && found->second.share != message.share) {
            events.push_back({ShareEvent::Kind::disappeared, ShareEvent::Reason::replaced, found->second});
            shares_.erase(found);
            found = shares_.end();
        }
        if (found == shares_.end()) {
            DiscoveredShare share;
            share.device = message.device;
            share.share = message.share;
            share.revision = message.revision;
            share.info = message.info;
            share.address = address;
            share.firstSeen = now;
            share.lastSeen = now;
            share.expires = now + lifetime;
            share.lastSequence = message.sequence;
            share.received = 1;
            shares_[message.device] = share;
            events.push_back({ShareEvent::Kind::appeared, ShareEvent::Reason::none, share});
            ++generation_;
            break;
        }

        DiscoveredShare& share = found->second;
        share.lastSeen = now;
        share.expires = now + lifetime;
        if (message.sequence > share.lastSequence) {
            const uint32_t gap = message.sequence - share.lastSequence - 1;
            if (gap <= kMaximumSequenceGap) share.missed += gap;
            share.lastSequence = message.sequence;
            ++share.received;
        }
        // Older revisions arriving late are ignored; the address follows the sharer.
        const bool changed = message.revision > share.revision || share.address != address;
        if (message.revision > share.revision) {
            share.revision = message.revision;
            share.info = message.info;
        }
        share.address = address;
        if (changed) {
            events.push_back({ShareEvent::Kind::updated, ShareEvent::Reason::none, share});
            ++generation_;
        }
        break;
    }
    }
}

void ShareDiscovery::expire(int64_t now, std::vector<ShareEvent>& events) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = shares_.begin(); it != shares_.end();) {
        if (it->second.expires <= now) {
            events.push_back({ShareEvent::Kind::disappeared, ShareEvent::Reason::timedOut, it->second});
            it = shares_.erase(it);
            ++generation_;
        } else {
            ++it;
        }
    }
    for (auto it = ended_.begin(); it != ended_.end();) {
        it = it->second <= now ? ended_.erase(it) : std::next(it);
    }
}

void ShareDiscovery::forgetShares(std::vector<ShareEvent>& events) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : shares_) events.push_back({ShareEvent::Kind::disappeared, ShareEvent::Reason::none, entry.second});
    if (!shares_.empty()) ++generation_;
    shares_.clear();
    ended_.clear();
}

}  // namespace asyncmusic
