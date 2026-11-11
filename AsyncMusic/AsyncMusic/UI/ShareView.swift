//
//  Created by ktiays on 2026/11/11.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import SwiftUI
import UniformTypeIdentifiers

/// Sharing: pick an audio file and share it, then control it for everyone, choose how the sound
/// spreads and see who listens.
struct ShareSection: View {
    let center: ShareCenter

    var body: some View {
        if center.state.isSharing {
            SharingLive(center: center)
        } else {
            ShareSetup(center: center)
        }
    }
}

private struct ShareSetup: View {
    let center: ShareCenter

    @State private var choosesFile = false
    @State private var importError: String?

    var body: some View {
        let state = center.state
        VStack(alignment: .leading, spacing: 22) {
            NetworkProblems(state: state)
            if case let .file(url) = state.shareSource {
                HStack(spacing: 14) {
                    FileArtwork(url: url, size: 64)
                    VStack(alignment: .leading, spacing: 3) {
                        Text(url.deletingPathExtension().lastPathComponent).font(.headline).lineLimit(2)
                        Text("Audio file").font(.subheadline).foregroundStyle(.secondary)
                    }
                    Spacer(minLength: 8)
                    Button("Change") { choosesFile = true }
                        .buttonStyle(.bordered)
                        .buttonBorderShape(.capsule)
                }
                .padding(16)
                .card()
            } else {
                Button {
                    choosesFile = true
                } label: {
                    VStack(spacing: 10) {
                        Image(systemName: "music.note.list")
                            .font(.system(size: 30, weight: .medium))
                            .foregroundStyle(.orange)
                            .frame(width: 64, height: 64)
                            .background(.orange.opacity(0.14), in: .circle)
                        Text("Choose an audio file").font(.headline)
                        Text("From Files, iCloud Drive or another app’s folder.")
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 28)
                    .background(.fill.quinary, in: .rect(cornerRadius: 28))
                    .overlay(RoundedRectangle(cornerRadius: 28).strokeBorder(.quaternary, style: StrokeStyle(lineWidth: 1.5, dash: [6, 5])))
                    .contentShape(.rect(cornerRadius: 28))
                }
                .buttonStyle(.plain)
            }

            if let importError {
                Notice(systemImage: "exclamationmark.triangle", text: importError)
            }

            VStack(spacing: 10) {
                Button {
                    center.startSharing()
                } label: {
                    Label("Start Sharing", systemImage: "dot.radiowaves.left.and.right")
                        .font(.body.weight(.semibold))
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .buttonBorderShape(.capsule)
                .controlSize(.large)
                .tint(.orange)
                .disabled(state.shareSource == nil)

                Text("Everyone on this network will get an alert.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                Text("To share another app’s audio, share from a Mac.")
                    .font(.footnote)
                    .foregroundStyle(.tertiary)
            }
            .frame(maxWidth: .infinity)
        }
        .fileImporter(isPresented: $choosesFile, allowedContentTypes: [.audio]) { result in
            do {
                try center.importFile(result.get())
                importError = nil
            } catch {
                importError = "Couldn’t open the file: \(error.localizedDescription)"
            }
        }
    }
}

private struct SharingLive: View {
    let center: ShareCenter

    var body: some View {
        let state = center.state
        VStack(alignment: .leading, spacing: 22) {
            NetworkProblems(state: state)

            VStack(alignment: .leading, spacing: 16) {
                HStack(spacing: 14) {
                    if case let .file(url) = state.shareSource {
                        FileArtwork(url: url, size: 72)
                    }
                    VStack(alignment: .leading, spacing: 3) {
                        HStack(spacing: 7) {
                            Circle().fill(.orange).frame(width: 8, height: 8)
                                .background(Circle().fill(.orange.opacity(0.25)).frame(width: 18, height: 18))
                            Text("Live").font(.caption.weight(.semibold)).foregroundStyle(.orange)
                        }
                        Text(state.shareSource?.name ?? "Audio").font(.title3.weight(.semibold)).lineLimit(2)
                        Text(problem(state) ?? "Audio file").font(.subheadline).foregroundStyle(.secondary).lineLimit(1)
                    }
                    Spacer(minLength: 0)
                }
                if let file = center.sharedFile {
                    FileTransport(center: center, duration: file.duration)
                }
            }
            .padding(16)
            .card()

            VStack(alignment: .leading, spacing: 10) {
                Text("Sound").font(.subheadline.weight(.semibold))
                Picker("Sound", selection: Binding(get: { state.layout }, set: { center.setSpeakerLayout($0.speakerLayout) })) {
                    ForEach(ServerState.Layout.allCases) { layout in
                        Text(layout.title).tag(layout)
                    }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .controlSize(.large)
                if state.layout == .stereoPair {
                    Text("Listeners pick Left or Right on their own device.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            }

            VStack(alignment: .leading, spacing: 4) {
                Text(state.listeners.isEmpty ? "Listening" : "Listening · \(state.listeners.count)")
                    .font(.subheadline.weight(.semibold))
                    .padding(.horizontal, 6)
                    .padding(.bottom, 4)
                VStack(spacing: 0) {
                    if state.listeners.isEmpty {
                        Text("No one has joined yet. Nearby devices got an alert.")
                            .font(.callout)
                            .foregroundStyle(.secondary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(16)
                    }
                    ForEach(Array(state.listeners.enumerated()), id: \.element.id) { index, member in
                        if index > 0 { Divider().padding(.leading, 62) }
                        HStack(spacing: 12) {
                            Image(systemName: "hifispeaker")
                                .foregroundStyle(.secondary)
                                .frame(width: 36, height: 36)
                                .background(.fill.tertiary, in: .circle)
                            Text(member.name).lineLimit(1)
                            Spacer(minLength: 8)
                            ListenerChannel(channel: member.channel)
                        }
                        .padding(.horizontal, 14)
                        .padding(.vertical, 10)
                    }
                }
                .card(cornerRadius: 24)
            }
        }
    }

    private func problem(_ state: ServerState) -> String? {
        if let problem = state.shareProblem { return problem }
        if center.pausedPosition != nil { return "Paused for everyone" }
        if center.sharedFile != nil, !center.isFilePlaying, !center.session.isPreparingPlayback { return "Finished playing" }
        return nil
    }
}

/// A listener's side: "L" in the listening blue, "R" in the sharing orange, or both channels.
private struct ListenerChannel: View {
    let channel: ServerState.Channel?

    var body: some View {
        switch channel {
        case .left, .right:
            Text(channel == .left ? "L" : "R")
                .font(.footnote.weight(.bold))
                .foregroundStyle(channel == .left ? .white : .black)
                .frame(minWidth: 26, minHeight: 26)
                .background(channel == .left ? Color.accentColor : Color.orange, in: .capsule)
                .accessibilityLabel(channel == .left ? "Left" : "Right")
        default:
            Text("Both").font(.subheadline).foregroundStyle(.secondary)
        }
    }
}

/// The file's position and transport, for everyone listening.
private struct FileTransport: View {
    let center: ShareCenter
    let duration: Double

    @State private var scrubbing: Double?

    var body: some View {
        VStack(spacing: 14) {
            TimelineView(.periodic(from: .now, by: 0.5)) { _ in
                let position = scrubbing ?? center.filePosition
                VStack(spacing: 2) {
                    Slider(value: Binding(get: { position }, set: { scrubbing = $0 }), in: 0 ... max(duration, 0.1)) { editing in
                        if !editing, let scrubbing {
                            center.seekSharing(to: scrubbing)
                            self.scrubbing = nil
                        }
                    }
                    .tint(.orange)
                    .accessibilityLabel("Position")
                    HStack {
                        Text(Self.format(position))
                        Spacer()
                        Text("−" + Self.format(max(duration - position, 0)))
                    }
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
                }
            }
            HStack(spacing: 36) {
                Button {
                    center.seekSharing(to: center.filePosition - 15)
                } label: {
                    Image(systemName: "gobackward.15").font(.title2).frame(width: 48, height: 48)
                }
                .accessibilityLabel("Back 15 Seconds")
                Button {
                    if center.isFilePlaying { center.pauseSharing() } else { center.resumeSharing() }
                } label: {
                    Image(systemName: center.isFilePlaying ? "pause.fill" : "play.fill")
                        .font(.title2)
                        .contentTransition(.symbolEffect(.replace))
                        .foregroundStyle(.black)
                        .frame(width: 64, height: 64)
                        .background(.orange, in: .circle)
                }
                .accessibilityLabel(center.isFilePlaying ? "Pause for Everyone" : "Play for Everyone")
                Button {
                    center.seekSharing(to: center.filePosition + 15)
                } label: {
                    Image(systemName: "goforward.15").font(.title2).frame(width: 48, height: 48)
                }
                .accessibilityLabel("Forward 15 Seconds")
            }
            .buttonStyle(.plain)
            .foregroundStyle(.primary)
            .frame(maxWidth: .infinity)
        }
    }

    static func format(_ seconds: Double) -> String {
        let total = Int(seconds.rounded(.down))
        return String(format: "%d:%02d", total / 60, total % 60)
    }
}

/// A file's embedded artwork, or a note on an orange tile.
struct FileArtwork: View {
    let url: URL
    var size: CGFloat

    @State private var metadata: SharedFileMetadata?

    var body: some View {
        Group {
            if let artwork = metadata?.artwork {
                Image(uiImage: artwork).resizable().scaledToFill()
            } else {
                Image(systemName: "music.note")
                    .font(.system(size: size * 0.4, weight: .medium))
                    .foregroundStyle(.orange)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .background(.orange.opacity(0.15))
            }
        }
        .frame(width: size, height: size)
        .clipShape(.rect(cornerRadius: size * 0.25))
        .accessibilityHidden(true)
        .task(id: url) {
            metadata = await SharedFileMetadata.load(url)
        }
    }
}
