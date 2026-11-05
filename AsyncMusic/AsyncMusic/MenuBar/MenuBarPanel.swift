//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import AppKit
import AsyncMusicCore
import UniformTypeIdentifiers
import SwiftUI

/// The floating window under the menu bar icon: listen for shares and join one, or share this
/// Mac's audio. Everything the user does happens here.
struct MenuBarPanel: View {
    let center: ShareCenter
    var openDiagnostics: () -> Void

    @State private var showsSettings = false
    @State private var pendingMode: ShareCenter.Mode?

    var body: some View {
        ZStack {
            VStack(alignment: .leading, spacing: 14) {
                if showsSettings {
                    SettingsPanel(center: center, openDiagnostics: openDiagnostics) {
                        withAnimation(.snappy) { showsSettings = false }
                    }
                    .transition(.move(edge: .trailing).combined(with: .opacity))
                } else {
                    header
                    modePicker
                    switch center.mode {
                    case .listen: ListenSection(center: center)
                    case .share: ShareSection(center: center)
                    }
                }
            }
            .padding(14)
            .disabled(pendingMode != nil)
            .accessibilityHidden(pendingMode != nil)

            if let pendingMode {
                ModeSwitchConfirmation(center: center, mode: pendingMode) {
                    withAnimation(.snappy) { self.pendingMode = nil }
                }
                .transition(.opacity)
            }
        }
        .frame(width: 340)
        .glassEffect(.regular, in: .rect(cornerRadius: 22))
    }

    private var header: some View {
        HStack(spacing: 10) {
            AppGlyph(size: 32)
            VStack(alignment: .leading, spacing: 1) {
                Text("AsyncMusic").font(.headline)
                Text(center.deviceName).font(.subheadline).foregroundStyle(.secondary)
            }
            Spacer()
            Button {
                withAnimation(.snappy) { showsSettings = true }
            } label: {
                Image(systemName: "gearshape").font(.body.weight(.medium))
            }
            .buttonStyle(.borderless)
            .help("Settings")
            .accessibilityLabel("Settings")
        }
    }

    private var modePicker: some View {
        Picker("Mode", selection: Binding(get: { center.mode }, set: requestMode)) {
            ForEach(ShareCenter.Mode.allCases) { mode in
                Text(mode.rawValue).tag(mode)
            }
        }
        .pickerStyle(.segmented)
        .labelsHidden()
        .controlSize(.large)
        .frame(maxWidth: .infinity)
    }

    private func requestMode(_ mode: ShareCenter.Mode) {
        if center.switchInterrupts(to: mode) {
            withAnimation(.snappy) { pendingMode = mode }
        } else {
            center.setMode(mode)
        }
    }
}

// MARK: - Listen

private struct ListenSection: View {
    let center: ShareCenter

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            NetworkProblems(center: center)
            if let joined = center.joined {
                JoinedCard(center: center, share: joined)
            } else if center.nearbyOthers.isEmpty {
                ListeningPlaceholder()
            }
            if !center.nearbyOthers.isEmpty {
                PanelSection(center.joined == nil ? "Sharing Nearby" : "Also Sharing Nearby") {
                    ForEach(Array(center.nearbyOthers.enumerated()), id: \.element.id) { index, share in
                        if index > 0 { Divider().padding(.leading, 56) }
                        NearbyRow(share: share) { center.join(share) }
                    }
                }
            }
        }
    }
}

private struct ListeningPlaceholder: View {
    var body: some View {
        VStack(spacing: 10) {
            ShareIcon(emphasis: .listening)
                .foregroundStyle(.tint)
                .frame(width: 48, height: 48)
                .padding(8)
                .background(.tint.opacity(0.12), in: .circle)
            Text("Listening for Shares").font(.headline)
            Text("When a Mac, PC or phone on this network starts sharing audio, you’ll get an alert asking whether to join.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .fixedSize(horizontal: false, vertical: true)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 18)
        .padding(.horizontal, 16)
        .background(.fill.quinary, in: .rect(cornerRadius: 14))
    }
}

private struct NearbyRow: View {
    let share: ShareCenter.NearbyShare
    var join: () -> Void

    var body: some View {
        HStack(spacing: 12) {
            DeviceTile(kind: share.sourceKind, size: 36)
            VStack(alignment: .leading, spacing: 2) {
                Text(share.name).font(.body.weight(.semibold)).lineLimit(1)
                Text(share.summary).font(.subheadline).foregroundStyle(.secondary).lineLimit(1)
            }
            Spacer(minLength: 8)
            Button("Join", action: join)
                .buttonStyle(.glassProminent)
                .controlSize(.small)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
    }
}

private struct JoinedCard: View {
    let center: ShareCenter
    let share: ShareCenter.NearbyShare

    private var session: ClockSyncSession { center.session }
    private var isPlaying: Bool { session.player.playback.isPlaying() }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            VStack(alignment: .leading, spacing: 14) {
                HStack(spacing: 12) {
                    Image(systemName: "waveform")
                        .font(.title2.weight(.semibold))
                        .foregroundStyle(.white)
                        .symbolEffect(.variableColor.iterative, isActive: isPlaying)
                        .frame(width: 48, height: 48)
                        .background(Color.accentColor.gradient, in: .rect(cornerRadius: 12))
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Listening to").font(.caption.weight(.medium)).foregroundStyle(.tint)
                        Text(share.name).font(.headline).lineLimit(1)
                        Text(share.summary).font(.subheadline).foregroundStyle(.secondary).lineLimit(1)
                    }
                    Spacer(minLength: 0)
                    Text(session.player.channel.badge)
                        .font(.caption.weight(.bold).monospaced())
                        .padding(.horizontal, 8)
                        .padding(.vertical, 4)
                        .background(.fill.tertiary, in: .capsule)
                        .help(session.player.channel.title)
                }
                HStack(spacing: 8) {
                    Image(systemName: "speaker.fill").foregroundStyle(.secondary)
                    Slider(value: Binding(get: { Double(session.player.volume) }, set: { session.player.volume = Float($0) }), in: 0 ... 1)
                        .controlSize(.small)
                        .accessibilityLabel("Volume")
                    Image(systemName: "speaker.wave.3.fill").foregroundStyle(.secondary)
                }
            }
            .padding(12)
            .background(.fill.quinary, in: .rect(cornerRadius: 14))

            PanelSection {
                PanelRow("Plays as") { Text(session.player.channel.title).foregroundStyle(.secondary) }
                Divider().padding(.leading, 12)
                PanelRow("Sync") { SyncLabel(state: session.followerState) }
            }

            Button(role: .destructive) {
                center.leave()
            } label: {
                Text("Leave").frame(maxWidth: .infinity)
            }
            .buttonStyle(.glass)
            .controlSize(.large)
        }
    }
}

private struct SyncLabel: View {
    let state: ClockSyncFollower.State?

    var body: some View {
        let (text, color): (String, Color) = switch state {
        case .syncing: ("In sync", .green)
        case .reconnecting(ClockSyncFollower.localNetworkDenied): ("No Local Network access", .red)
        case .reconnecting: ("Reconnecting…", .orange)
        case let .failed(message): (message, .red)
        default: ("Connecting…", .orange)
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

// MARK: - Share

private struct ShareSection: View {
    let center: ShareCenter

    var body: some View {
        if center.isSharing {
            SharingLive(center: center)
        } else {
            ShareSetup(center: center)
        }
    }
}

private struct ShareSetup: View {
    let center: ShareCenter
    @State private var catalog = AudioApplicationCatalog()

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            NetworkProblems(center: center)
            PanelSection("What to Share") {
                ScrollView {
                    VStack(spacing: 2) {
                        ForEach(catalog.applications) { application in
                            SourceRow(isSelected: center.shareSource == .application(application)) {
                                AppIcon(url: application.appURL)
                            } title: {
                                Text(application.name)
                            } subtitle: {
                                Text(application.isPlaying ? "Playing now" : "Quiet")
                            } action: {
                                center.shareSource = .application(application)
                            }
                        }
                        if catalog.applications.isEmpty {
                            Text(catalog.error ?? "Open an app that plays audio.")
                                .font(.callout).foregroundStyle(.secondary)
                                .padding(10)
                        }
                    }
                    .padding(4)
                }
                .scrollBounceBehavior(.basedOnSize)
                .frame(maxHeight: 220)
                .fixedSize(horizontal: false, vertical: true)
                Divider().padding(.horizontal, 10)
                SourceRow(isSelected: { if case .file = center.shareSource { true } else { false } }()) {
                    Image(systemName: "doc.badge.plus").font(.title3).frame(width: 26, height: 26)
                } title: {
                    if case let .file(url) = center.shareSource {
                        Text(url.lastPathComponent).lineLimit(1)
                    } else {
                        Text("Choose Audio File…")
                    }
                } subtitle: {
                    EmptyView()
                } action: {
                    chooseFile()
                }
                .padding(4)
            }

            Button {
                center.startSharing()
            } label: {
                Label("Start Sharing", systemImage: "dot.radiowaves.left.and.right").frame(maxWidth: .infinity)
            }
            .buttonStyle(.glassProminent)
            .tint(.orange)
            .controlSize(.large)
            .disabled(center.shareSource == nil)

            Text("Everyone on this network will get an alert.")
                .font(.footnote)
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity)
        }
        .task {
            while !Task.isCancelled {
                catalog.refresh()
                do { try await Task.sleep(for: .seconds(2)) } catch { return }
            }
        }
    }

    private func chooseFile() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.audio]
        panel.allowsMultipleSelection = false
        NSApp.activate()
        if panel.runModal() == .OK, let url = panel.url {
            center.shareSource = .file(url)
        }
    }
}

private struct SharingLive: View {
    let center: ShareCenter
    private var session: ClockSyncSession { center.session }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            NetworkProblems(center: center)
            HStack(spacing: 12) {
                Group {
                    switch center.shareSource {
                    case let .application(application): AppIcon(url: application.appURL, size: 44)
                    default: Image(systemName: "music.note").font(.title2).frame(width: 44, height: 44).background(.fill.tertiary, in: .rect(cornerRadius: 10))
                    }
                }
                VStack(alignment: .leading, spacing: 2) {
                    Label {
                        Text("Sharing")
                    } icon: {
                        Circle().fill(.orange).frame(width: 7, height: 7)
                    }
                    .font(.caption.weight(.medium))
                    .foregroundStyle(.orange)
                    Text(center.shareSource?.name ?? "Audio").font(.headline).lineLimit(1)
                    Text(statusText).font(.subheadline).foregroundStyle(.secondary).lineLimit(1)
                }
                Spacer(minLength: 0)
            }
            .padding(12)
            .background(.fill.quinary, in: .rect(cornerRadius: 14))

            PanelSection("Sound") {
                Picker("Sound", selection: Binding(get: { session.speakerLayout }, set: { session.setSpeakerLayout($0) })) {
                    ForEach(SpeakerLayout.allCases) { layout in
                        Text(layout.title).tag(layout)
                    }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .frame(maxWidth: .infinity)
                .padding(8)
            }

            PanelSection(center.listeners.isEmpty ? "Listening" : "Listening · \(center.listeners.count)") {
                if center.listeners.isEmpty {
                    Text("No one has joined yet.")
                        .font(.callout).foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(12)
                }
                ForEach(Array(center.listeners.enumerated()), id: \.element.id) { index, member in
                    if index > 0 { Divider().padding(.leading, 40) }
                    PanelRow {
                        Label(member.name, systemImage: "hifispeaker")
                    } trailing: {
                        if let channel = session.activeChannels[member.id], channel != .full {
                            Text(channel.title).foregroundStyle(.secondary)
                        }
                    }
                }
            }

            Button(role: .destructive) {
                center.stopSharing()
            } label: {
                Text("Stop Sharing").frame(maxWidth: .infinity)
            }
            .buttonStyle(.glass)
            .controlSize(.large)
        }
    }

    private var statusText: String {
        if let error = session.applicationAudio.error { return error }
        if session.isLoadingTrack { return "Loading…" }
        if session.applicationAudio.isWaitingForAudio { return "Waiting for audio…" }
        return "Seen as “\(center.deviceName)”"
    }
}

// MARK: - Mode switch

private struct ModeSwitchConfirmation: View {
    let center: ShareCenter
    let mode: ShareCenter.Mode
    var dismiss: () -> Void

    var body: some View {
        ZStack {
            Rectangle().fill(.black.opacity(0.18)).onTapGesture(perform: dismiss)
            VStack(spacing: 12) {
                Image(systemName: mode == .share ? "dot.radiowaves.left.and.right" : "ear")
                    .font(.title2)
                    .foregroundStyle(mode == .share ? .orange : .accentColor)
                Text(title).font(.headline).multilineTextAlignment(.center)
                Text(message).font(.callout).foregroundStyle(.secondary).multilineTextAlignment(.center)
                    .fixedSize(horizontal: false, vertical: true)
                HStack {
                    Button(keepTitle, action: dismiss)
                        .keyboardShortcut(.cancelAction)
                        .frame(maxWidth: .infinity)
                    Button(switchTitle) {
                        center.setMode(mode)
                        dismiss()
                    }
                    .keyboardShortcut(.defaultAction)
                    .buttonStyle(.glassProminent)
                    .tint(mode == .share ? .orange : .accentColor)
                    .frame(maxWidth: .infinity)
                }
                .controlSize(.large)
                .padding(.top, 4)
            }
            .padding(18)
            .glassEffect(.regular, in: .rect(cornerRadius: 18))
            .padding(18)
        }
        .clipShape(.rect(cornerRadius: 22))
    }

    private var title: String {
        mode == .share ? "Start sharing from this Mac?" : "Stop sharing?"
    }

    private var message: String {
        if mode == .share {
            return "You’ll stop listening to \(center.joined?.name ?? "the share"). A Mac either listens or shares, not both at once."
        }
        let count = center.listeners.count
        return count == 0 ? "Listening ends this share." : "\(count) \(count == 1 ? "device" : "devices") listening to you will be disconnected."
    }

    private var keepTitle: String { mode == .share ? "Keep Listening" : "Keep Sharing" }
    private var switchTitle: String { mode == .share ? "Leave and Share" : "Stop and Listen" }
}

// MARK: - Problems

private struct NetworkProblems: View {
    let center: ShareCenter

    var body: some View {
        if center.session.isLocalNetworkDenied {
            Notice(systemImage: "network.slash", text: "Local Network access is off for AsyncMusic. Turn it on in System Settings › Privacy & Security › Local Network.")
        } else if let error = center.discoveryError {
            Notice(systemImage: "exclamationmark.triangle", text: error)
        }
    }
}

private struct Notice: View {
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

// MARK: - Building blocks

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

/// A selectable row in a source list.
private struct SourceRow<Icon: View, Title: View, Subtitle: View>: View {
    let isSelected: Bool
    @ViewBuilder var icon: Icon
    @ViewBuilder var title: Title
    @ViewBuilder var subtitle: Subtitle
    var action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 10) {
                icon
                VStack(alignment: .leading, spacing: 1) {
                    title.font(.body.weight(isSelected ? .semibold : .regular))
                    subtitle.font(.caption).foregroundStyle(isSelected ? AnyShapeStyle(.white.opacity(0.85)) : AnyShapeStyle(.secondary))
                }
                Spacer(minLength: 0)
            }
            .foregroundStyle(isSelected ? AnyShapeStyle(.white) : AnyShapeStyle(.primary))
            .padding(.horizontal, 8)
            .padding(.vertical, 6)
            .contentShape(.rect)
            .background(isSelected ? AnyShapeStyle(Color.accentColor) : AnyShapeStyle(.clear), in: .rect(cornerRadius: 8))
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(isSelected ? .isSelected : [])
    }
}

struct AppIcon: View {
    let url: URL?
    var size: CGFloat = 26

    var body: some View {
        if let url {
            Image(nsImage: NSWorkspace.shared.icon(forFile: url.path)).resizable().frame(width: size, height: size)
        } else {
            Image(systemName: "waveform").frame(width: size, height: size)
        }
    }
}

/// The app's mark on a dark tile.
struct AppGlyph: View {
    var size: CGFloat

    var body: some View {
        ShareIcon(emphasis: .playing)
            .foregroundStyle(.white)
            .padding(size * 0.14)
            .frame(width: size, height: size)
            .background(Color(white: 0.12), in: .rect(cornerRadius: size * 0.25))
            .accessibilityHidden(true)
    }
}

/// What kind of source a share plays, on a tinted tile.
struct DeviceTile: View {
    let kind: ShareSourceKind
    var size: CGFloat

    var body: some View {
        Image(systemName: symbol)
            .font(.system(size: size * 0.45, weight: .medium))
            .foregroundStyle(.orange)
            .frame(width: size, height: size)
            .background(.orange.opacity(0.15), in: .circle)
            .accessibilityHidden(true)
    }

    private var symbol: String {
        switch kind {
        case .file: "music.note"
        case .system: "desktopcomputer"
        default: "dot.radiowaves.left.and.right"
        }
    }
}
#endif
