//
//  Created by ktiays on 2026/11/11.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

// Pieces the Mac's menu bar panel and window share with the iPhone, iPad and Vision Pro app.

struct SyncLabel: View {
    let status: SyncStatus?

    var body: some View {
        let (text, color): (String, Color) = switch status {
        case .syncing: ("In sync", .green)
        case .localNetworkDenied: ("No Local Network access", .red)
        case .reconnecting: ("Reconnecting…", .orange)
        case let .failed(message): (message, .red)
        case .connecting, nil: ("Connecting…", .orange)
        }
        Label {
            Text(text)
        } icon: {
            Circle().fill(color).frame(width: 7, height: 7)
        }
        .foregroundStyle(.secondary)
        .lineLimit(1)
    }
}

/// What to ask before a mode switch that ends a share being listened to or given.
struct ModeSwitchPrompt {
    let mode: ShareMode
    let state: ServerState

    var title: String {
        mode == .share ? "Start sharing from this \(DeviceWord.current)?" : "Stop sharing?"
    }

    var message: String {
        if mode == .share {
            return "You’ll stop listening to \(state.joined?.name ?? "the share"). \(DeviceWord.indefinite) either listens or shares, not both at once."
        }
        let count = state.listeners.count
        return count == 0 ? "Listening ends this share." : "\(count) \(count == 1 ? "device" : "devices") listening to you will be disconnected."
    }

    var keepTitle: String { mode == .share ? "Keep Listening" : "Keep Sharing" }
    var switchTitle: String { mode == .share ? "Leave and Share" : "Stop and Listen" }
}

/// What to call this kind of device in sentences.
enum DeviceWord {
    @MainActor
    static var current: String {
        #if os(macOS)
        "Mac"
        #elseif os(visionOS)
        "Apple Vision Pro"
        #else
        UIDevice.current.userInterfaceIdiom == .pad ? "iPad" : "iPhone"
        #endif
    }

    /// "A Mac", "An iPhone".
    @MainActor
    static var indefinite: String {
        let word = current
        return ("AEIOU".contains(word.first ?? "x") || word.hasPrefix("i") ? "An " : "A ") + word
    }
}

// MARK: - Problems

struct NetworkProblems: View {
    let state: ServerState

    var body: some View {
        if state.isLocalNetworkDenied {
            #if os(macOS)
            Notice(systemImage: "network.slash", text: "Local Network access is off for AsyncMusic Server. Turn it on in System Settings › Privacy & Security › Local Network.")
            #else
            Notice(systemImage: "network.slash", text: "Local Network access is off, so other devices can’t hear this one. Turn it on in Settings › Privacy & Security › Local Network.")
            #endif
        } else if let error = state.discoveryError {
            Notice(systemImage: "exclamationmark.triangle", text: error)
        }
    }
}

struct Notice: View {
    let systemImage: String
    let text: String

    var body: some View {
        Label {
            Text(text).fixedSize(horizontal: false, vertical: true)
        } icon: {
            Image(systemName: systemImage).foregroundStyle(.orange)
        }
        .font(.callout)
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.orange.opacity(0.12), in: .rect(cornerRadius: 12))
    }
}

/// A titled group of rows, like a grouped form section.
struct PanelSection<Content: View>: View {
    var title: String?
    @ViewBuilder var content: Content

    init(_ title: String? = nil, @ViewBuilder content: () -> Content) {
        self.title = title
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            if let title {
                Text(title).font(.subheadline.weight(.semibold)).foregroundStyle(.secondary).padding(.horizontal, 6)
            }
            VStack(spacing: 0) { content }
                .background(.fill.quinary, in: .rect(cornerRadius: 14))
        }
    }
}

/// A label on the left and a control or value on the right.
struct PanelRow<Leading: View, Trailing: View>: View {
    @ViewBuilder var leading: Leading
    @ViewBuilder var trailing: Trailing

    init(@ViewBuilder leading: () -> Leading, @ViewBuilder trailing: () -> Trailing) {
        self.leading = leading()
        self.trailing = trailing()
    }

    var body: some View {
        HStack(spacing: 12) {
            leading
            Spacer(minLength: 8)
            trailing
        }
        .frame(minHeight: 22)
        .padding(.horizontal, 12)
        .padding(.vertical, 9)
    }
}

extension PanelRow where Leading == Text {
    init(_ title: String, @ViewBuilder trailing: () -> Trailing) {
        self.init(leading: { Text(title) }, trailing: trailing)
    }
}

// MARK: - Channel

/// The listener's channel row: in a stereo pair, Left or Right to choose from (none chosen
/// while it waits for the pair and plays both); otherwise what it plays.
struct ChannelRow: View {
    let state: ServerState
    var request: (ServerState.Channel) -> Void

    var body: some View {
        if state.joinedLayout == .stereoPair {
            PanelRow("Channel") {
                ChannelPicker(state: state, request: request).fixedSize()
            }
        } else {
            PanelRow("Plays as") { Text(state.channel.title).foregroundStyle(.secondary) }
        }
    }
}

/// Left | Right for a stereo pair. The sharer sends every listener the same stereo stream, so
/// choosing a side changes only how this device plays it.
struct ChannelPicker: View {
    let state: ServerState
    var request: (ServerState.Channel) -> Void

    var body: some View {
        Picker("Channel", selection: Binding(get: { state.channel }, set: { if $0 != state.channel { request($0) } })) {
            Text("Left").tag(ServerState.Channel.left)
            Text("Right").tag(ServerState.Channel.right)
        }
        .pickerStyle(.segmented)
        .labelsHidden()
        .help("Stereo pair: each device plays one side. Choosing a side swaps with the device on it.")
    }
}

/// "L" or "R" while this device plays one side of a stereo pair; nothing while it plays both.
struct ChannelBadge: View {
    let channel: ServerState.Channel

    var body: some View {
        if channel != .full {
            Text(channel.badge)
                .font(.caption.weight(.bold).monospaced())
                .padding(.horizontal, 8)
                .padding(.vertical, 4)
                .background(.fill.tertiary, in: .capsule)
                .accessibilityLabel("Plays \(channel.title)")
        }
    }
}
