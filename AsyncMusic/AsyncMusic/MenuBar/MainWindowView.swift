//
//  Created by ktiays on 2026/11/06.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import AppKit
import SwiftUI

/// The main window's content, for the app without its menu bar icon. It does what the menu bar
/// panel does, laid out for a resizable window: the mode in the toolbar, nearby shares as cards,
/// the sources to share in a sidebar, and standard alerts.
struct MainWindowView: View {
    let client: ServiceClient

    @State private var pendingMode: ShareMode?

    static let minimumSize = CGSize(width: 680, height: 460)

    var body: some View {
        Group {
            if client.isRunning {
                switch client.state.mode {
                case .listen: ListenPage(client: client)
                case .share: SharePage(client: client)
                }
            } else {
                ServiceUnavailablePage(client: client)
            }
        }
        .frame(minWidth: Self.minimumSize.width, maxWidth: .infinity, minHeight: Self.minimumSize.height, maxHeight: .infinity)
        .navigationTitle("AsyncMusic")
        .navigationSubtitle(client.settings.deviceName)
        .toolbar {
            ToolbarItem(placement: .principal) {
                Picker("Mode", selection: Binding(get: { client.state.mode }, set: requestMode)) {
                    ForEach(ShareMode.allCases) { mode in
                        Text(mode.rawValue).tag(mode)
                    }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .disabled(!client.isRunning)
                .help("Listen for shares, or share this Mac’s audio")
            }
        }
        .alert(pendingPrompt?.title ?? "", isPresented: Binding(get: { pendingMode != nil }, set: { if !$0 { pendingMode = nil } }), presenting: pendingPrompt) { prompt in
            Button(prompt.keepTitle, role: .cancel) {}
            Button(prompt.switchTitle) { client.setMode(prompt.mode) }
        } message: { prompt in
            Text(prompt.message)
        }
    }

    private var pendingPrompt: ModeSwitchPrompt? {
        pendingMode.map { ModeSwitchPrompt(mode: $0, state: client.state) }
    }

    private func requestMode(_ mode: ShareMode) {
        if client.state.switchInterrupts(to: mode) {
            pendingMode = mode
        } else {
            client.setMode(mode)
        }
    }
}

// MARK: - Listen

private struct ListenPage: View {
    let client: ServiceClient

    var body: some View {
        let state = client.state
        if state.joined == nil, state.nearbyOthers.isEmpty {
            VStack(spacing: 0) {
                NetworkProblems(state: state).padding([.horizontal, .top], 24)
                ContentUnavailableView {
                    Label {
                        Text("Listening for Shares")
                    } icon: {
                        ShareIcon(emphasis: .listening).foregroundStyle(.tint).frame(width: 56, height: 56)
                    }
                } description: {
                    Text("When a Mac, PC or phone on this network starts sharing audio, it appears here and you’ll get an alert asking whether to join.")
                }
            }
        } else {
            ScrollView {
                VStack(alignment: .leading, spacing: 28) {
                    NetworkProblems(state: state)
                    if let joined = state.joined {
                        NowPlayingCard(client: client, share: joined)
                    }
                    if !state.nearbyOthers.isEmpty {
                        VStack(alignment: .leading, spacing: 12) {
                            Text(state.joined == nil ? "Sharing Nearby" : "Also Sharing Nearby")
                                .font(.title3.weight(.semibold))
                            LazyVGrid(columns: [GridItem(.adaptive(minimum: 200), spacing: 14)], spacing: 14) {
                                ForEach(state.nearbyOthers) { share in
                                    ShareCard(share: share) { client.join(share) }
                                }
                            }
                        }
                    }
                }
                .padding(24)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }
}

private struct NowPlayingCard: View {
    let client: ServiceClient
    let share: NearbyShare

    var body: some View {
        let state = client.state
        VStack(alignment: .leading, spacing: 18) {
            HStack(alignment: .center, spacing: 18) {
                Image(systemName: "waveform")
                    .font(.system(size: 34, weight: .semibold))
                    .foregroundStyle(.white)
                    .symbolEffect(.variableColor.iterative, isActive: state.isPlaying)
                    .frame(width: 84, height: 84)
                    .background(Color.accentColor.gradient, in: .rect(cornerRadius: 20))
                VStack(alignment: .leading, spacing: 4) {
                    Text("Listening to").font(.subheadline.weight(.medium)).foregroundStyle(.tint)
                    Text(share.name).font(.title2.weight(.bold)).lineLimit(1)
                    Text(share.summary).foregroundStyle(.secondary).lineLimit(1)
                }
                Spacer(minLength: 12)
                Button(role: .destructive) {
                    client.leave()
                } label: {
                    Text("Leave").frame(minWidth: 72)
                }
                .buttonStyle(.glass)
                .controlSize(.large)
            }
            Divider()
            HStack(spacing: 24) {
                HStack(spacing: 8) {
                    Image(systemName: "speaker.fill").foregroundStyle(.secondary)
                    Slider(value: Binding(get: { Double(client.volume) }, set: { client.setVolume(Float($0)) }), in: 0 ... 1)
                        .accessibilityLabel("Volume")
                    Image(systemName: "speaker.wave.3.fill").foregroundStyle(.secondary)
                }
                .frame(maxWidth: 280)
                Spacer(minLength: 0)
                if state.joinedLayout == .stereoPair {
                    HStack(spacing: 8) {
                        Text("Channel").foregroundStyle(.secondary)
                        ChannelPicker(state: state) { client.requestChannel($0) }
                    }
                    .fixedSize()
                } else {
                    HStack(spacing: 6) {
                        Text("Plays as").foregroundStyle(.secondary)
                        Text(state.channel.title)
                    }
                    .fixedSize()
                }
                SyncLabel(status: state.syncStatus).fixedSize()
            }
            .font(.callout)
        }
        .padding(20)
        .card(cornerRadius: 20)
    }
}

private struct ShareCard: View {
    let share: NearbyShare
    var join: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            DeviceTile(kind: share.sourceKind, size: 44)
            VStack(alignment: .leading, spacing: 2) {
                Text(share.name).font(.headline).lineLimit(1)
                Text(share.summary).font(.subheadline).foregroundStyle(.secondary).lineLimit(1)
            }
            Button(action: join) {
                Text("Join").frame(maxWidth: .infinity)
            }
            .buttonStyle(.glassProminent)
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .card(cornerRadius: 16)
    }
}

// MARK: - Share

private struct SharePage: View {
    let client: ServiceClient

    var body: some View {
        if client.state.isSharing {
            SharingPage(client: client)
        } else {
            HStack(spacing: 0) {
                SourceList(client: client)
                    .frame(width: 240)
                Divider()
                SourceDetail(client: client)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
            .refreshesApplications(client)
        }
    }
}

/// The apps playing audio, in a sidebar, and a button to choose a file under them; choosing one
/// selects it on the server.
private struct SourceList: View {
    let client: ServiceClient

    var body: some View {
        let state = client.state
        List(selection: Binding(get: { selectedTag(state.shareSource) }, set: select)) {
            Section("Apps") {
                ForEach(state.applications) { application in
                    Label {
                        VStack(alignment: .leading, spacing: 1) {
                            Text(application.name).lineLimit(1)
                            Text(application.isPlaying ? "Playing now" : "Quiet").font(.caption).foregroundStyle(.secondary)
                        }
                    } icon: {
                        AppIcon(url: application.appURL, size: 24)
                    }
                    .tag(application.id)
                }
                if state.applications.isEmpty {
                    Text(state.applicationsError ?? "Open an app that plays audio.")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .selectionDisabled()
                }
            }
        }
        .listStyle(.sidebar)
        .safeAreaInset(edge: .bottom, spacing: 0) {
            VStack(spacing: 0) {
                Divider()
                Button {
                    client.chooseFile()
                } label: {
                    Label("Choose Audio File…", systemImage: "doc.badge.plus")
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .contentShape(.rect)
                }
                .buttonStyle(.borderless)
                .padding(.horizontal, 16)
                .padding(.vertical, 12)
            }
            .background(.bar)
        }
    }

    private func selectedTag(_ source: ShareSource?) -> String? {
        if case let .application(application) = source { application.id } else { nil }
    }

    private func select(_ tag: String?) {
        guard let tag, tag != selectedTag(client.state.shareSource),
              let application = client.state.applications.first(where: { $0.id == tag }) else { return }
        client.select(.application(application))
    }
}

private struct SourceDetail: View {
    let client: ServiceClient

    var body: some View {
        let state = client.state
        VStack(spacing: 0) {
            NetworkProblems(state: state).padding([.horizontal, .top], 24)
            if let source = state.shareSource {
                VStack(spacing: 20) {
                    Spacer(minLength: 0)
                    SourceArtwork(source: source, size: 96)
                    VStack(spacing: 4) {
                        Text(source.name).font(.title.weight(.bold)).lineLimit(1)
                        Text(subtitle(source)).foregroundStyle(.secondary)
                    }
                    LayoutPicker(client: client).fixedSize()
                    VStack(spacing: 8) {
                        Button {
                            client.startSharing()
                        } label: {
                            Label("Start Sharing", systemImage: "dot.radiowaves.left.and.right").frame(minWidth: 200)
                        }
                        .buttonStyle(.glassProminent)
                        .tint(.orange)
                        .controlSize(.extraLarge)
                        .keyboardShortcut(.defaultAction)
                        Text("Everyone on this network will get an alert.")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                    if source.isFile {
                        Button("Choose Another File…") { client.chooseFile() }
                            .buttonStyle(.link)
                    }
                    Spacer(minLength: 0)
                }
                .padding(24)
            } else {
                ContentUnavailableView {
                    Label("Choose What to Share", systemImage: "dot.radiowaves.left.and.right")
                } description: {
                    Text("Pick an app playing audio, or an audio file. Everyone on this network can then listen along.")
                }
            }
        }
    }

    private func subtitle(_ source: ShareSource) -> String {
        switch source {
        case let .application(application): application.isPlaying ? "Playing now" : "Quiet"
        case .file: "Audio file"
        }
    }
}

private struct SharingPage: View {
    let client: ServiceClient

    var body: some View {
        let state = client.state
        ScrollView {
            VStack(alignment: .leading, spacing: 28) {
                NetworkProblems(state: state)
                HStack(spacing: 18) {
                    if let source = state.shareSource {
                        SourceArtwork(source: source, size: 84)
                    }
                    VStack(alignment: .leading, spacing: 4) {
                        Label {
                            Text("Sharing")
                        } icon: {
                            Circle().fill(.orange).frame(width: 8, height: 8)
                        }
                        .font(.subheadline.weight(.medium))
                        .foregroundStyle(.orange)
                        Text(state.shareSource?.name ?? "Audio").font(.title2.weight(.bold)).lineLimit(1)
                        Text(state.shareProblem ?? "Seen as “\(state.deviceName)”").foregroundStyle(.secondary).lineLimit(1)
                    }
                    Spacer(minLength: 12)
                    Button(role: .destructive) {
                        client.stopSharing()
                    } label: {
                        Text("Stop Sharing").frame(minWidth: 96)
                    }
                    .buttonStyle(.glass)
                    .controlSize(.large)
                }
                .padding(20)
                .card(cornerRadius: 20)

                HStack {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Sound").font(.headline)
                        Text("Play the full mix on every device, or split left and right across a pair.")
                            .font(.callout).foregroundStyle(.secondary)
                    }
                    Spacer(minLength: 16)
                    LayoutPicker(client: client).fixedSize()
                }

                VStack(alignment: .leading, spacing: 12) {
                    Text(state.listeners.isEmpty ? "Listening" : "Listening · \(state.listeners.count)")
                        .font(.title3.weight(.semibold))
                    VStack(spacing: 0) {
                        if state.listeners.isEmpty {
                            Text("No one has joined yet.")
                                .foregroundStyle(.secondary)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .padding(14)
                        }
                        ForEach(Array(state.listeners.enumerated()), id: \.element.id) { index, member in
                            if index > 0 { Divider().padding(.leading, 44) }
                            PanelRow {
                                Label(member.name, systemImage: "hifispeaker")
                            } trailing: {
                                if let channel = member.channel {
                                    Text(channel.title).foregroundStyle(.secondary)
                                }
                            }
                        }
                    }
                    .card(cornerRadius: 14)
                }
            }
            .padding(24)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

private struct LayoutPicker: View {
    let client: ServiceClient

    var body: some View {
        Picker("Sound", selection: Binding(get: { client.state.layout }, set: { client.setLayout($0) })) {
            ForEach(ServerState.Layout.allCases) { layout in
                Text(layout.title).tag(layout)
            }
        }
        .pickerStyle(.segmented)
        .labelsHidden()
    }
}

private struct SourceArtwork: View {
    let source: ShareSource
    var size: CGFloat

    var body: some View {
        switch source {
        case let .application(application):
            AppIcon(url: application.appURL, size: size)
        case .file:
            Image(systemName: "music.note")
                .font(.system(size: size * 0.4, weight: .medium))
                .foregroundStyle(.secondary)
                .frame(width: size, height: size)
                .background(.fill.tertiary, in: .rect(cornerRadius: size * 0.22))
        }
    }
}

private extension View {
    /// A rounded panel set off from the window background.
    func card(cornerRadius: CGFloat) -> some View {
        background(.fill.quaternary, in: .rect(cornerRadius: cornerRadius))
    }
}

// MARK: - Service

private struct ServiceUnavailablePage: View {
    let client: ServiceClient

    var body: some View {
        ContentUnavailableView {
            if client.status == .starting || client.status == .running {
                VStack(spacing: 12) {
                    ProgressView().controlSize(.large)
                    Text("Starting AsyncMusic Server…")
                }
            } else {
                Label(client.status == .off ? "AsyncMusic Server Is Off" : "AsyncMusic Server Couldn’t Start",
                      systemImage: "pause.circle")
            }
        } description: {
            switch client.status {
            case .starting, .running: Text("It listens for shares and plays them, even after you quit this app.")
            case .off: Text("This Mac doesn’t listen for shares or share its audio until you turn it on.")
            case let .failed(message): Text(message)
            }
        } actions: {
            switch client.status {
            case .off, .failed:
                Button("Turn On") { client.setEnabled(true) }
                    .buttonStyle(.glassProminent)
                    .controlSize(.large)
            case .starting, .running:
                EmptyView()
            }
        }
    }
}
#endif
