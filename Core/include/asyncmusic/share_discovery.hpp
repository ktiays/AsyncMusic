//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "net.hpp"
#include "protocol.hpp"
#include "ref_counted.hpp"

AM_ASSUME_NONNULL_BEGIN

namespace asyncmusic {

/// The UDP port every device listens on for share announcements, and broadcasts them to.
constexpr uint16_t kShareDiscoveryPort = 41750;

/// Longest name or source description an announcement carries, in bytes (cut at a character
/// boundary).
constexpr size_t kShareMaximumTextLength = 120;

enum class AM_CLOSED_ENUM ShareSourceKind : uint8_t {
    unknown = 0,
    /// Another app's audio (the source names the app).
    application = 1,
    /// An audio file played by the sharer.
    file = 2,
    /// Everything the sharing device plays.
    system = 3,
};

/// What kind of device shares, for listeners to picture it when `ShareInfo::model` is empty or
/// unknown to them.
enum class AM_CLOSED_ENUM ShareDeviceKind : uint8_t {
    unknown = 0,
    laptop = 1,
    desktop = 2,
    phone = 3,
    tablet = 4,
};

/// What a sharing device tells everyone about its share.
struct ShareInfo {
    /// The sharing device's display name (UTF-8).
    std::string name;
    /// What it plays, e.g. the app's name (UTF-8, may be empty).
    std::string source;
    ShareSourceKind sourceKind = ShareSourceKind::unknown;
    ShareDeviceKind deviceKind = ShareDeviceKind::unknown;
    /// The sharing device's model identifier (ASCII, may be empty), e.g. "Mac16,10" for an Apple
    /// device, which Apple platforms can turn into a picture of the exact model.
    std::string model;
    /// The UDP port of the sharer's clock sync leader on the address the announcement came
    /// from; joining a share means following that leader.
    uint16_t clockPort = 0;
    /// How many devices are listening to the share.
    uint16_t listenerCount = 0;

    bool operator==(const ShareInfo& other) const;
    bool operator!=(const ShareInfo& other) const { return !(*this == other); }
};

/// Datagrams of the share discovery protocol, broadcast on `kShareDiscoveryPort`.
/// Little-endian:
///
///     header:    magic(2) = "AD" version(1) = 1 type(1)
///     announce:  header device(8) share(8) sequence(4) revision(4) lifetime(2, ms) clockPort(2)
///                listeners(2) sourceKind(1) nameLength(1) name sourceLength(1) source
///                [deviceKind(1) modelLength(1) model]
///     goodbye:   header device(8) share(8)
///     query:     header device(8)
///
/// `device` is the sender's stable ID. `share` is chosen at random each time a device starts
/// sharing, so a listener can tell a new share (worth an alert) from the same one heard again.
/// `sequence` counts the sharer's announcements (listeners use it to drop duplicates that came
/// over several interfaces, and to measure loss); `revision` increases whenever the share's
/// details change. A listener forgets a share it has not heard for `lifetime`.
///
/// Sharers announce periodically, a few times quickly at first, and say `goodbye` (repeated)
/// when they stop. Listeners send `query` when they start or come back from sleep, which every
/// sharer answers with an announcement right away, broadcast and also sent to the asker. Bytes after the fields above are ignored, so
/// later versions can append fields; a different version number means an incompatible format.
/// The bracketed device fields were appended that way: an announcement that ends before them is
/// from an older sharer and describes an unknown device.
struct ShareMessage {
    enum class AM_CLOSED_ENUM Type : uint8_t {
        announce = 1,
        goodbye = 2,
        query = 3,
    };

    Type type = Type::announce;
    uint64_t device = 0;
    uint64_t share = 0;
    uint32_t sequence = 0;
    uint32_t revision = 0;
    uint16_t lifetimeMilliseconds = 0;
    ShareInfo info;

    static ShareMessage makeAnnounce(uint64_t device, uint64_t share, uint32_t sequence, uint32_t revision, uint16_t lifetimeMilliseconds, const ShareInfo& info);
    static ShareMessage makeGoodbye(uint64_t device, uint64_t share);
    static ShareMessage makeQuery(uint64_t device);

    Bytes encode() const;
    /// Returns false for anything that is not a well-formed message of a known type.
    static bool decode(const uint8_t* AM_NULLABLE data, size_t size, ShareMessage& message);
};

/// A share heard on the network.
struct DiscoveredShare {
    uint64_t device = 0;
    uint64_t share = 0;
    uint32_t revision = 0;
    ShareInfo info;
    /// The sharer's address, with `info.clockPort` as its port: what to follow to join.
    net::Address address;
    /// Steady-clock nanoseconds.
    int64_t firstSeen = 0;
    int64_t lastSeen = 0;
    /// When it is forgotten unless heard again.
    int64_t expires = 0;
    uint32_t lastSequence = 0;
    /// Announcements received, and the ones that never arrived (from gaps in `sequence`).
    uint32_t received = 0;
    uint32_t missed = 0;
};

struct ShareEvent {
    enum class AM_CLOSED_ENUM Kind : uint8_t {
        /// A share started (or was first heard): the moment to ask whether to join.
        appeared,
        /// Its details changed (listeners, source, name).
        updated,
        disappeared,
    };
    enum class AM_CLOSED_ENUM Reason : uint8_t {
        none,
        /// The sharer said goodbye.
        ended,
        /// Nothing heard from it for its lifetime.
        timedOut,
        /// The same device started a new share.
        replaced,
    };

    Kind kind = Kind::appeared;
    Reason reason = Reason::none;
    DiscoveredShare share;
};

/// How a `ShareDiscovery` paces and addresses its datagrams. The defaults suit a real network.
struct ShareDiscoveryConfiguration {
    uint16_t port = kShareDiscoveryPort;
    /// Announcements are sent `fastCount` times `fastInterval` apart when sharing starts (or
    /// the network changes, or the device wakes), then every `steadyInterval`. Wi-Fi drops
    /// broadcasts far more often than unicast, so a share must survive several in a row.
    int64_t fastInterval = 500'000'000;
    int fastCount = 6;
    int64_t steadyInterval = 2'000'000'000;
    /// Each interval is stretched or shrunk at random by up to this fraction, so devices that
    /// started together do not keep colliding.
    double jitter = 0.15;
    /// How long listeners keep a share without hearing it (announced to them).
    int64_t lifetime = 7'000'000'000;
    /// Goodbyes and queries are repeated this many times, `repeatSpacing` apart.
    int repeats = 3;
    int64_t repeatSpacing = 150'000'000;
    /// A sharer answers queries at most this often.
    int64_t minimumQueryAnswerSpacing = 200'000'000;
    /// After a goodbye, announcements of that share that arrive late are ignored this long.
    int64_t endedMemory = 10'000'000'000;
    /// Where to send instead of broadcasting (with ports), e.g. loopback for tests.
    std::vector<net::Address> targets;
    /// For tests: return true to drop an outgoing datagram (simulated loss).
    std::function<bool(const ShareMessage&)> dropOutgoing;
};

/// Announces this device's share, or listens for other devices' shares; never both, as a device
/// either shares its audio or plays someone else's. Switching from sharing to listening says
/// goodbye first; switching from listening to sharing forgets the shares heard.
///
/// Runs its own thread. Announcements go to the directed broadcast address of every IPv4
/// interface (re-read every few seconds, so they follow networks coming and going), from a
/// socket that shares `kShareDiscoveryPort` with other apps on the device. Answers to queries
/// also go to the asker directly.
///
/// The event handler runs on that thread, without locks held; it must not destroy the discovery.
/// Swift owns it as a reference (`create`) and polls `generation()` instead of taking events.
class ShareDiscovery : public RefCounted {
public:
    using Configuration = ShareDiscoveryConfiguration;

    enum class AM_CLOSED_ENUM Mode : uint8_t { off, listening, sharing };

    struct Status {
        Mode mode = Mode::off;
        /// Whether the socket is bound; while not, it is retried every second.
        bool isReady = false;
        std::string error;
        /// While sharing.
        uint64_t share = 0;
        uint32_t announcementsSent = 0;
        uint32_t queriesAnswered = 0;
        /// Where the last announcement went.
        std::vector<std::string> targets;
    };

    /// A discovery with the default configuration and no event handler, holding one reference.
    static ShareDiscovery* create(uint64_t device) SWIFT_RETURNS_RETAINED;

    explicit ShareDiscovery(uint64_t device, Configuration configuration = {});
    ShareDiscovery(uint64_t device, std::function<void(const ShareEvent&)> onEvent, Configuration configuration = {});
    ~ShareDiscovery() override;
    ShareDiscovery(const ShareDiscovery&) = delete;
    ShareDiscovery& operator=(const ShareDiscovery&) = delete;

    /// Starts (or keeps) listening, and asks sharers to announce themselves now.
    void listen();
    /// Starts sharing under a new share ID; while already sharing, starts a new share.
    void share(const ShareInfo& info);
    /// Changes the current share's details (same share ID, next revision). Ignored unless sharing.
    void update(const ShareInfo& info);
    /// Stops; a sharer says goodbye first.
    void stop();
    /// While listening: ask sharers to announce now (e.g. when the UI opens).
    void refresh();

    std::vector<DiscoveredShare> shares() const;
    /// Increases whenever `shares()` changes, for UIs that poll.
    uint64_t generation() const;
    Status status() const;
    uint64_t device() const { return device_; }

private:
    struct Command {
        enum class Kind : uint8_t { listen, share, update, stop, refresh };
        Kind kind;
        ShareInfo info;
    };
    struct Outgoing {
        int64_t due = 0;
        ShareMessage message;
    };

    void run();
    void apply(const Command& command, int64_t now, std::vector<ShareEvent>& events);
    void ensureSocket(int64_t now);
    void closeSocket();
    void send(const ShareMessage& message);
    void schedule(const ShareMessage& message, int64_t due);
    void scheduleRepeated(const ShareMessage& message, int64_t first);
    void announce(int64_t now);
    int64_t nextInterval();
    void receive(const uint8_t* data, size_t size, const net::Address& from, int64_t now, std::vector<ShareEvent>& events);
    void expire(int64_t now, std::vector<ShareEvent>& events);
    void forgetShares(std::vector<ShareEvent>& events);
    void checkNetwork(int64_t now);
    void restartAnnouncing(int64_t now);
    void sendQueries(int64_t now);
    uint64_t nextRandom();

    const uint64_t device_;
    const Configuration configuration_;
    std::function<void(const ShareEvent&)> onEvent_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool exiting_ = false;
    std::vector<Command> commands_;
    Mode mode_ = Mode::off;
    ShareInfo info_;
    uint64_t share_ = 0;
    uint32_t revision_ = 0;
    uint32_t sequence_ = 0;
    std::map<uint64_t, DiscoveredShare> shares_;
    std::map<uint64_t, int64_t> ended_;  // share ID -> until
    uint64_t generation_ = 0;
    Status status_;

    // Worker thread only.
    net::UDPSocket socket_;
    int64_t nextSocketAttempt_ = 0;
    std::vector<Outgoing> outgoing_;
    int64_t nextAnnouncement_ = 0;
    int fastRemaining_ = 0;
    int64_t lastQueryAnswer_ = 0;
    /// Where queries came from since the last announcement, which also goes to each of them.
    std::vector<net::Address> queriers_;
    int64_t lastLoop_ = 0;
    std::vector<net::Address> lastTargets_;
    int64_t nextNetworkCheck_ = 0;
    uint64_t random_ = 0;

    std::thread thread_;
} SWIFT_SHARED_REFERENCE(asyncmusic_share_discovery_retain, asyncmusic_share_discovery_release);

}  // namespace asyncmusic

inline void asyncmusic_share_discovery_retain(asyncmusic::ShareDiscovery* object) { object->retain(); }
inline void asyncmusic_share_discovery_release(asyncmusic::ShareDiscovery* object) { object->release(); }

AM_ASSUME_NONNULL_END
