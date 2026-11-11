//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import AppKit
import SwiftUI

/// The menu under the menu bar icon: listen for shares and join one, or share this Mac's audio. AsyncMusic Server does the work; the panel shows its state and sends it what the
/// user does.
struct MenuBarPanel: View {
    let client: ServiceClient
    /// Closes the panel before the Settings window opens.
    var willOpenSettings: () -> Void

    @Environment(\.openWindow) private var openWindow
    @State private var pendingMode: ShareMode?

    var body: some View {
        ZStack {
            VStack(alignment: .leading, spacing: 14) {
                header
                if client.isRunning {
                    modePicker
                    switch client.state.mode {
                    case .listen: ListenSection(client: client)
                    case .share: ShareSection(client: client)
                    }
                } else {
                    ServiceUnavailable(client: client)
                }
            }
            .padding(14)
            .disabled(pendingMode != nil)
            .accessibilityHidden(pendingMode != nil)

            if let pendingMode {
                ModeSwitchConfirmation(client: client, mode: pendingMode) {
                    withAnimation(.snappy) { self.pendingMode = nil }
                }
                .transition(.opacity)
            }
        }
        .frame(width: 340)
        // The sections crossfade on the spring the panel's glass resizes on. Changes the server
        // sends aren't animated by themselves.
        .animation(.spring(MenuPanelItemView.spring), value: layoutKey)
    }

    /// What changes the panel's shape; not things like the volume, which would lag behind a drag.
    private var layoutKey: [AnyHashable] {
        let state = client.state
        return [
            client.isRunning, String(describing: client.status), state.mode, state.isSharing,
            state.joined?.id, state.joinedLayout, state.nearbyOthers.map(\.id), state.listeners.map(\.id),
            state.applications.map(\.id), state.shareSource?.name, state.syncStatus.map { String(describing: $0) },
            state.isLocalNetworkDenied, state.discoveryError, pendingMode,
        ]
    }

    @ContentBuilder
    private var header: some View {
        HStack(spacing: 10) {
            AppGlyph(size: 32)
            VStack(alignment: .leading, spacing: 1) {
                Text("AsyncMusic").font(.headline)
                Text(client.settings.deviceName).font(.subheadline).foregroundStyle(.secondary)
            }
            Spacer()
            Button {
                willOpenSettings()
                openWindow(id: SettingsSectionID.windowID)
            } label: {
                Image(systemName: "gearshape").font(.body.weight(.medium))
            }
            .buttonStyle(.borderless)
            .help("Settings")
            .accessibilityLabel("Settings")
        }
    }

    @ContentBuilder
    private var modePicker: some View {
        Group {
            if #available(macOS 27.0, *) {
                ModeTabs(selection: client.state.mode, select: requestMode)
                    .fixedSize()
            } else {
                Picker("Mode", selection: Binding(get: { client.state.mode }, set: requestMode)) {
                    ForEach(ShareMode.allCases) { mode in
                        Text(mode.rawValue).tag(mode)
                    }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .controlSize(.large)
            }
        }
        .frame(maxWidth: .infinity)
    }

    private func requestMode(_ mode: ShareMode) {
        if client.state.switchInterrupts(to: mode) {
            withAnimation(.snappy) { pendingMode = mode }
        } else {
            client.setMode(mode)
        }
    }
}

// MARK: - Listen

private struct ListenSection: View {
    let client: ServiceClient

    var body: some View {
        let state = client.state
        VStack(alignment: .leading, spacing: 14) {
            NetworkProblems(state: state)
            if let joined = state.joined {
                JoinedCard(client: client, share: joined)
            } else if state.nearbyOthers.isEmpty {
                ListeningPlaceholder()
            }
            if !state.nearbyOthers.isEmpty {
                PanelSection(state.joined == nil ? "Sharing Nearby" : "Also Sharing Nearby") {
                    ForEach(Array(state.nearbyOthers.enumerated()), id: \.element.id) { index, share in
                        if index > 0 { Divider().padding(.leading, 56) }
                        NearbyRow(share: share) { client.join(share) }
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
    let share: NearbyShare
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
    let client: ServiceClient
    let share: NearbyShare

    private var isPlaying: Bool { client.state.isPlaying }
    private var channel: ServerState.Channel { client.state.channel }

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
                    ChannelBadge(channel: channel)
                }
                HStack(spacing: 8) {
                    Image(systemName: "speaker.fill").foregroundStyle(.secondary)
                    Slider(value: Binding(get: { Double(client.volume) }, set: { client.setVolume(Float($0)) }), in: 0 ... 1)
                        .controlSize(.small)
                        .accessibilityLabel("Volume")
                    Image(systemName: "speaker.wave.3.fill").foregroundStyle(.secondary)
                }
            }
            .padding(12)
            .background(.fill.quinary, in: .rect(cornerRadius: 14))

            PanelSection {
                ChannelRow(state: client.state) { client.requestChannel($0) }
                Divider().padding(.leading, 12)
                PanelRow("Sync") { SyncLabel(status: client.state.syncStatus) }
            }

            Button(role: .destructive) {
                client.leave()
            } label: {
                Text("Leave").frame(maxWidth: .infinity)
            }
            .buttonStyle(.glass)
            .controlSize(.large)
        }
    }
}

// MARK: - Share

private struct ShareSection: View {
    let client: ServiceClient

    var body: some View {
        if client.state.isSharing {
            SharingLive(client: client)
        } else {
            ShareSetup(client: client)
        }
    }
}

private struct ShareSetup: View {
    let client: ServiceClient
    @Environment(\.dismissMenuBarPanel) private var dismissPanel

    var body: some View {
        let state = client.state
        VStack(alignment: .leading, spacing: 14) {
            NetworkProblems(state: state)
            PanelSection("What to Share") {
                ScrollView {
                    VStack(spacing: 2) {
                        ForEach(state.applications) { application in
                            SourceRow(isSelected: state.shareSource == .application(application)) {
                                AppIcon(url: application.appURL)
                            } title: {
                                Text(application.name)
                            } subtitle: {
                                Text(application.isPlaying ? "Playing now" : "Quiet")
                            } action: {
                                client.select(.application(application))
                            }
                        }
                        if state.applications.isEmpty {
                            Text(state.applicationsError ?? "Open an app that plays audio.")
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
                SourceRow(isSelected: state.shareSource?.isFile == true) {
                    Image(systemName: "doc.badge.plus").font(.title3).frame(width: 26, height: 26)
                } title: {
                    if case let .file(url) = state.shareSource {
                        Text(url.lastPathComponent).lineLimit(1)
                    } else {
                        Text("Choose Audio File…")
                    }
                } subtitle: {
                    EmptyView()
                } action: {
                    // The server's open panel can't come up over an open menu.
                    dismissPanel()
                    client.chooseFile()
                }
                .padding(4)
            }

            Button {
                client.startSharing()
            } label: {
                Label("Start Sharing", systemImage: "dot.radiowaves.left.and.right").frame(maxWidth: .infinity)
            }
            .buttonStyle(.glassProminent)
            .tint(.orange)
            .controlSize(.large)
            .disabled(state.shareSource == nil)

            Text("Everyone on this network will get an alert.")
                .font(.footnote)
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity)
        }
        .refreshesApplications(client)
    }
}

extension View {
    /// Keeps the server's list of apps playing audio fresh while the view is shown. The server
    /// lists them, from its side of the sandbox.
    func refreshesApplications(_ client: ServiceClient) -> some View {
        task {
            while !Task.isCancelled {
                client.refreshApplications()
                do { try await Task.sleep(for: .seconds(2)) } catch { return }
            }
        }
    }
}

private struct SharingLive: View {
    let client: ServiceClient

    var body: some View {
        let state = client.state
        VStack(alignment: .leading, spacing: 14) {
            NetworkProblems(state: state)
            HStack(spacing: 12) {
                Group {
                    switch state.shareSource {
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
                    Text(state.shareSource?.name ?? "Audio").font(.headline).lineLimit(1)
                    Text(state.shareProblem ?? "Seen as “\(state.deviceName)”").font(.subheadline).foregroundStyle(.secondary).lineLimit(1)
                }
                Spacer(minLength: 0)
            }
            .padding(12)
            .background(.fill.quinary, in: .rect(cornerRadius: 14))

            PanelSection("Sound") {
                Picker("Sound", selection: Binding(get: { state.layout }, set: { client.setLayout($0) })) {
                    ForEach(ServerState.Layout.allCases) { layout in
                        Text(layout.title).tag(layout)
                    }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .frame(maxWidth: .infinity)
                .padding(8)
            }

            PanelSection(state.listeners.isEmpty ? "Listening" : "Listening · \(state.listeners.count)") {
                if state.listeners.isEmpty {
                    Text("No one has joined yet.")
                        .font(.callout).foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(12)
                }
                ForEach(Array(state.listeners.enumerated()), id: \.element.id) { index, member in
                    if index > 0 { Divider().padding(.leading, 40) }
                    PanelRow {
                        Label(member.name, systemImage: "hifispeaker")
                    } trailing: {
                        if let channel = member.channel {
                            Text(channel.title).foregroundStyle(.secondary)
                        }
                    }
                }
            }

            Button(role: .destructive) {
                client.stopSharing()
            } label: {
                Text("Stop Sharing").frame(maxWidth: .infinity)
            }
            .buttonStyle(.glass)
            .controlSize(.large)
        }
    }
}

// MARK: - Mode switch

private struct ModeSwitchConfirmation: View {
    let client: ServiceClient
    let mode: ShareMode
    var dismiss: () -> Void

    private var prompt: ModeSwitchPrompt { ModeSwitchPrompt(mode: mode, state: client.state) }

    var body: some View {
        let prompt = prompt
        ZStack {
            Rectangle().fill(.black.opacity(0.18)).onTapGesture(perform: dismiss)
            VStack(spacing: 12) {
                Image(systemName: mode == .share ? "dot.radiowaves.left.and.right" : "ear")
                    .font(.title2)
                    .foregroundStyle(mode == .share ? .orange : .accentColor)
                Text(prompt.title).font(.headline).multilineTextAlignment(.center)
                Text(prompt.message).font(.callout).foregroundStyle(.secondary).multilineTextAlignment(.center)
                    .fixedSize(horizontal: false, vertical: true)
                HStack {
                    Button(prompt.keepTitle, action: dismiss)
                        .keyboardShortcut(.cancelAction)
                        .frame(maxWidth: .infinity)
                    Button(prompt.switchTitle) {
                        client.setMode(mode)
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
        .clipShape(.rect(cornerRadius: 12))
    }

}

// MARK: - Service

/// In place of everything else while AsyncMusic Server isn't running.
private struct ServiceUnavailable: View {
    let client: ServiceClient

    var body: some View {
        VStack(spacing: 10) {
            Group {
                if client.status == .starting {
                    ProgressView().controlSize(.large)
                } else {
                    Image(systemName: "pause.circle").font(.system(size: 40)).foregroundStyle(.secondary)
                }
            }
            .frame(height: 48)
            Text(title).font(.headline)
            Text(message)
                .font(.callout)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .fixedSize(horizontal: false, vertical: true)
            switch client.status {
            case .off, .failed:
                Button("Turn On") { client.setEnabled(true) }
                    .buttonStyle(.glassProminent)
                    .controlSize(.large)
                    .padding(.top, 4)
            case .starting, .running:
                EmptyView()
            }
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 18)
        .padding(.horizontal, 16)
        .background(.fill.quinary, in: .rect(cornerRadius: 14))
    }

    private var title: String {
        switch client.status {
        case .starting, .running: "Starting AsyncMusic Server…"
        case .off: "AsyncMusic Server Is Off"
        case .failed: "AsyncMusic Server Couldn’t Start"
        }
    }

    private var message: String {
        switch client.status {
        case .starting, .running: "It listens for shares and plays them, even after you quit this app."
        case .off: "This Mac doesn’t listen for shares or share its audio until you turn it on."
        case let .failed(message): message
        }
    }
}

// MARK: - Building blocks

extension EnvironmentValues {
    /// Closes the menu the panel is in, before something else needs the screen.
    @Entry var dismissMenuBarPanel: () -> Void = {}
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

/// The mode tabs, as AppKit's own tabs segmented control.
///
/// SwiftUI's tabs picker wraps the same control, but inside the menu its press animation (the
/// glass lifting and settling back) is cut short when the selection changes on release. This
/// control is only told the selection when it differs from what it shows.
@available(macOS 27.0, *)
private struct ModeTabs: NSViewRepresentable {
    var selection: ShareMode
    var select: (ShareMode) -> Void

    private static let modes = ShareMode.allCases

    func makeNSView(context: Context) -> NSSegmentedControl {
        let control = NSSegmentedControl(
            labels: Self.modes.map(\.rawValue), trackingMode: .selectOne,
            target: context.coordinator, action: #selector(Coordinator.didSelect(_:)))
        control.role = .tabs
        control.controlSize = .large
        control.borderShape = .capsule
        control.setAccessibilityLabel("Mode")
        return control
    }

    func updateNSView(_ control: NSSegmentedControl, context: Context) {
        context.coordinator.select = select
        let index = Self.modes.firstIndex(of: selection) ?? -1
        if control.selectedSegment != index { control.selectedSegment = index }
    }

    func makeCoordinator() -> Coordinator { Coordinator(select: select) }

    final class Coordinator: NSObject {
        var select: (ShareMode) -> Void

        init(select: @escaping (ShareMode) -> Void) { self.select = select }

        @objc func didSelect(_ control: NSSegmentedControl) {
            guard ModeTabs.modes.indices.contains(control.selectedSegment) else { return }
            select(ModeTabs.modes[control.selectedSegment])
        }
    }
}
#endif
