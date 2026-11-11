//
//  Created by ktiays on 2026/11/11.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "net.hpp"
#include "ref_counted.hpp"
#include "serial_queue.hpp"
#include "session.hpp"
#include "share_discovery.hpp"
#include "speakers.hpp"

namespace asyncmusic {

/// The Listen/Share model the apps show, on top of a `GroupSession` and a `ShareDiscovery`: the
/// C++ counterpart of the Mac server's `ShareCenter`, for platforms without Swift.
///
/// A device either listens or shares. Listening (the default) hears shares announced on the
/// network, raises an alert for each new one, and joins one by following the sharer's clock
/// leader at the address it announced; a joined share that ends is left by itself. Sharing
/// leads the group, plays the source the platform loaded into the session, and announces the
/// leader's port, the source and the listener count once the leader is listening.
///
/// All work happens on the center's own thread. `state()` can be read from any thread; changes
/// are reported through `onChange`, called on that thread (hop to the UI thread from it). The
/// session must outlive the center.
class ShareCenter {
public:
    enum class Mode : uint8_t { listen, share };

    /// How a joined share's clock sync is going.
    enum class SyncStatus : uint8_t { connecting, syncing, reconnecting, failed };

    /// What this device tells listeners about itself, so they can picture it.
    struct Device {
        ShareDeviceKind kind = ShareDeviceKind::unknown;
        /// A model identifier, e.g. "Pixel 10 Pro" or "Mac16,10"; may be empty.
        std::string model;
    };

    /// A share heard on the network.
    struct Share {
        /// A new ID each time the device starts sharing.
        uint64_t id = 0;
        uint64_t device = 0;
        ShareInfo info;
        /// Where to follow its clock leader (the sharer's address and clock port).
        net::Address address;
        /// Steady-clock nanoseconds.
        int64_t firstSeen = 0;

        /// "Music · 2 listening": what it plays and how many listen.
        std::string summary() const;
    };

    /// A device listening to this device's share.
    struct Listener {
        uint64_t id = 0;
        std::string name;
        /// The channel it plays, when the layout gives it one of its own.
        std::optional<SpeakerChannel> channel;
    };

    /// What this device shares; the platform loads it into the session (`GroupSession::addTrack`).
    struct Source {
        std::string name;
        ShareSourceKind kind = ShareSourceKind::file;

        bool operator==(const Source& other) const { return name == other.name && kind == other.kind; }
        bool operator!=(const Source& other) const { return !(*this == other); }
    };

    struct State {
        Mode mode = Mode::listen;
        std::string deviceName;
        /// Every share heard, including the one joined.
        std::vector<Share> nearby;
        /// New shares waiting for an answer, newest first.
        std::vector<Share> alerts;
        /// Set when a share arrived unanswered (alerts off or dismissed); cleared by `refresh()`.
        bool hasUnseenShares = false;
        std::string discoveryError;

        // Listening
        std::optional<Share> joined;
        bool isPlaying = false;
        SpeakerChannel channel = SpeakerChannel::full;
        /// The joined share's layout: in a stereo pair, the listener picks Left or Right.
        SpeakerLayout joinedLayout = SpeakerLayout::everywhere;
        SyncStatus syncStatus = SyncStatus::connecting;

        // Sharing
        std::optional<Source> source;
        bool isSharing = false;
        /// Why sharing isn't playing yet ("Loading…"), or empty when it is.
        std::string shareProblem;
        SpeakerLayout layout = SpeakerLayout::everywhere;
        std::vector<Listener> listeners;

        /// Increases with every change.
        uint64_t revision = 0;

        /// The shares nearby other than the one joined.
        std::vector<Share> nearbyOthers() const;
        /// Whether switching to `mode` would cut a connection, so the user should confirm.
        bool switchInterrupts(Mode to) const { return to != mode && (joined.has_value() || isSharing); }
    };

    /// What the platform does for the center.
    struct Hooks {
        /// Changes the session's role, and starts or stops what goes with it (the audio output,
        /// keeping the device awake). Defaults to `GroupSession::setRole` alone.
        std::function<void(GroupSession::Role role)> setRole;
        /// Whether a source is still being loaded into the session.
        std::function<bool()> isLoadingSource;
        /// Why the source could not be loaded, or empty.
        std::function<std::string()> sourceError;
    };

    ShareCenter(GroupSession& session, Device device, Hooks hooks = {}, ShareDiscoveryConfiguration configuration = {});
    ~ShareCenter();
    ShareCenter(const ShareCenter&) = delete;
    ShareCenter& operator=(const ShareCenter&) = delete;

    /// Called on the center's thread after every change. Set before `start()` (or the first
    /// command, which starts the center too); nothing calls it earlier.
    std::function<void()> onChange;
    /// A new share arrived and alerts are on (also reflected in `State::alerts`).
    std::function<void(const Share& share)> onAlert;

    /// Starts listening and reporting changes. Commands start the center too.
    void start();

    void setMode(Mode mode);
    /// "Show Alert" or "Only List": whether new shares raise alerts.
    void setAlertsEnabled(bool enabled);

    void join(uint64_t share);
    /// Follows a leader at a known address, without discovery (e.g. an emulator behind NAT).
    void joinAddress(const std::string& name, const net::Address& address);
    void leave();
    /// Listening to a stereo pair: asks to play `channel` here; the device that had it takes
    /// this one's place.
    void requestChannel(SpeakerChannel channel);
    void dismissAlerts();
    /// The UI listing shares opened: clears alerts and the unseen mark, and asks sharers to
    /// announce themselves now.
    void refresh();

    /// Picks what to share (not while sharing).
    void selectSource(Source source);
    /// Leads and plays the session's selected track, once it has loaded, and announces the share.
    void startSharing();
    void stopSharing();
    void setLayout(SpeakerLayout layout);

    State state() const;

private:
    void apply(Mode mode);
    void joinOnQueue(const Share& share);
    void leaveOnQueue();
    void stopSharingOnQueue();
    void setRole(GroupSession::Role role);
    void poll();
    void readShares();
    void updateAnnouncement(const GroupSession::Snapshot& snapshot);
    /// Rebuilds the published state from the center's and the session's, and notifies.
    void publish();
    /// Runs `task` on the center's thread, starting the center first.
    void enqueue(SerialQueue::Task task);

    GroupSession& session_;
    const Device device_;
    const Hooks hooks_;
    Ref<ShareDiscovery> discovery_;

    // Confined to `queue_`.
    Mode mode_ = Mode::listen;
    bool alertsEnabled_ = true;
    std::vector<Share> nearby_;
    std::vector<Share> alerts_;
    bool hasUnseenShares_ = false;
    std::optional<Share> joined_;
    std::optional<Source> source_;
    bool isSharing_ = false;
    bool playsWhenLoaded_ = false;
    std::optional<ShareInfo> announced_;
    uint64_t generation_ = 0;
    std::set<uint64_t> seenShares_;
    std::string discoveryError_;
    uint64_t lastSessionRevision_ = 0;

    std::once_flag started_;
    mutable std::mutex mutex_;
    State state_;

    // Last, so it stops before the members its tasks use go away.
    SerialQueue queue_{"asyncmusic.share-center"};
};

}  // namespace asyncmusic
