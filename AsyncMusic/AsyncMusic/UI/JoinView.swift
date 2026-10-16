//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AsyncMusicCore
import SwiftUI

/// A follower's screen: pick a host nearby, then show what this device plays for it.
struct JoinView: View {
    let session: ClockSyncSession

    var body: some View {
        ScrollView {
            VStack(spacing: 16) {
                if session.isLocalNetworkDenied {
                    LocalNetworkBanner()
                }
                if let host = session.connectedLeader {
                    SpeakerStatusView(session: session, host: host)
                } else {
                    hostList
                }
                Button(session.connectedLeader == nil ? "Cancel" : "Leave", role: .destructive) {
                    session.setRole(.off)
                }
                .buttonStyle(.bordered)
                .padding(.top, 8)
            }
            .readableColumn()
        }
        .navigationTitle("Speaker")
    }

    private var hostList: some View {
        Card(title: "Nearby Hosts") {
            if session.discoveredLeaders.isEmpty {
                HStack(spacing: 10) {
                    ProgressView()
                        .controlSize(.small)
                    Text("Looking for hosts…")
                        .foregroundStyle(.secondary)
                }
                Text("On the device with the music, open AsyncMusic and tap Host Music. Both devices need to be on the same Wi-Fi.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            ForEach(session.discoveredLeaders) { leader in
                Button {
                    session.follow(leader)
                } label: {
                    ChoiceRow(systemImage: "play.circle.fill", title: leader.name, subtitle: "Tap to join", showsChevron: true)
                }
                .buttonStyle(.plain)
            }
        }
    }
}

private struct SpeakerStatusView: View {
    let session: ClockSyncSession
    let host: String

    private var channel: SpeakerChannel { session.player.channel }
    private var isPlaying: Bool { session.player.playback.isPlaying() }

    var body: some View {
        VStack(spacing: 16) {
            VStack(spacing: 14) {
                SpeakerBadge(channel: channel, size: 120)
                    .overlay(alignment: .bottomTrailing) {
                        if isPlaying {
                            Image(systemName: "speaker.wave.3.fill")
                                .font(.title3)
                                .foregroundStyle(.tint)
                                .symbolEffect(.variableColor.iterative)
                                .padding(8)
                                .background(.background, in: Circle())
                                .offset(x: 6, y: 6)
                        }
                    }
                    .animation(.spring, value: channel)
                Text(title)
                    .font(.title.bold())
                Text(explanation)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }
            .padding(.vertical, 12)

            Card {
                LabeledContent("Host", value: host)
                Divider()
                LabeledContent("Sync") {
                    StatusLabel(text: syncText, tone: syncTone)
                }
                Divider()
                LabeledContent("Playback") {
                    StatusLabel(text: playbackText, tone: isPlaying ? (session.player.isWaitingForAudio ? .pending : .good) : .neutral)
                }
            }
        }
    }

    private var title: String {
        switch channel {
        case .left: "Left Speaker"
        case .right: "Right Speaker"
        default: "Speaker"
        }
    }

    private var explanation: String {
        switch channel {
        case .left, .right:
            "This device plays the \(channel.title.lowercased()) channel of a stereo pair with \(host)."
        default:
            session.remoteLayout == .stereoPair
                ? "Plays the full mix until both sides of the stereo pair are connected."
                : "This device plays the full mix in sync with \(host)."
        }
    }

    private var syncText: String {
        switch session.followerState {
        case .syncing?: session.clockMap == nil ? "Syncing…" : "In sync"
        case .reconnecting?: "Reconnecting…"
        case .failed?: "Connection failed"
        default: "Connecting…"
        }
    }

    private var syncTone: StatusLabel.Tone {
        switch session.followerState {
        case .syncing?: session.clockMap == nil ? .pending : .good
        case .failed?: .bad
        default: .pending
        }
    }

    private var playbackText: String {
        guard isPlaying else { return "Waiting for the host" }
        return session.player.isWaitingForAudio ? "Receiving audio…" : "Playing"
    }
}

#Preview {
    NavigationStack {
        JoinView(session: ClockSyncSession())
    }
}
