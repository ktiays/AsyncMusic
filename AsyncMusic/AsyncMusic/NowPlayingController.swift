//
//  Created by ktiays on 2026/11/11.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AsyncMusicCore
import AVFoundation
import MediaPlayer
import SwiftUI

/// Shows what plays in the system's media controls (Lock Screen, Control Center, the Dynamic
/// Island, headphones and CarPlay), and takes their commands, as music apps do.
///
/// Listening to a share is live, like a radio: pausing silences this device while it keeps its
/// place in the group, so playing again picks up in sync. Sharing a file controls the file for
/// everyone: pause, play and moving the position.
@MainActor
final class NowPlayingController {
    private let center: ShareCenter
    private var lastContent: Content?
    private var metadata: SharedFileMetadata?
    private var metadataURL: URL?

    private enum Content: Equatable {
        case none
        case listening(title: String, artist: String, isPlaying: Bool)
        case sharing(url: URL, title: String, artist: String, duration: Double, position: Double, isPlaying: Bool)
    }

    init(center: ShareCenter) {
        self.center = center
        registerCommands()
        observe()
    }

    // MARK: - State

    private func observe() {
        let content = withObservationTracking {
            currentContent()
        } onChange: { [weak self] in
            Task { @MainActor in self?.observe() }
        }
        apply(content)
    }

    private func currentContent() -> Content {
        let state = center.state
        if let joined = state.joined {
            let title = joined.source.isEmpty ? "Shared Audio" : joined.source
            return .listening(title: title, artist: joined.name, isPlaying: state.isPlaying && !center.isListeningPaused)
        }
        if state.isSharing, case let .file(url) = state.shareSource, let file = center.sharedFile {
            return .sharing(
                url: url, title: file.name, artist: state.deviceName, duration: file.duration,
                // While playing, a new schedule (a start, a seek) is the change; the position
                // moving along isn't.
                position: center.isFilePlaying ? Double(center.session.player.playback.startLeaderTime) : center.filePosition.rounded(),
                isPlaying: center.isFilePlaying)
        }
        return .none
    }

    private func apply(_ content: Content) {
        guard content != lastContent else { return }
        lastContent = content
        let info = MPNowPlayingInfoCenter.default()
        switch content {
        case .none:
            info.nowPlayingInfo = nil
            setEnabled(play: false, seek: false)

        case let .listening(title, artist, isPlaying):
            info.nowPlayingInfo = [
                MPMediaItemPropertyTitle: title,
                MPMediaItemPropertyArtist: artist,
                MPMediaItemPropertyAlbumTitle: "AsyncMusic",
                MPNowPlayingInfoPropertyIsLiveStream: true,
                MPNowPlayingInfoPropertyMediaType: MPNowPlayingInfoMediaType.audio.rawValue,
                MPNowPlayingInfoPropertyPlaybackRate: isPlaying ? 1.0 : 0.0,
                MPMediaItemPropertyArtwork: Self.appArtwork,
            ]
            setEnabled(play: true, seek: false)

        case let .sharing(url, title, artist, duration, _, isPlaying):
            if url != metadataURL {
                metadataURL = url
                metadata = nil
                Task {
                    let loaded = await SharedFileMetadata.load(url)
                    guard self.metadataURL == url else { return }
                    self.metadata = loaded
                    self.lastContent = nil
                    self.apply(self.currentContent())
                }
            }
            var values: [String: Any] = [
                MPMediaItemPropertyTitle: metadata?.title ?? title,
                MPMediaItemPropertyArtist: metadata?.artist ?? artist,
                MPMediaItemPropertyPlaybackDuration: duration,
                MPNowPlayingInfoPropertyElapsedPlaybackTime: center.filePosition,
                MPNowPlayingInfoPropertyIsLiveStream: false,
                MPNowPlayingInfoPropertyMediaType: MPNowPlayingInfoMediaType.audio.rawValue,
                MPNowPlayingInfoPropertyPlaybackRate: isPlaying ? 1.0 : 0.0,
                MPNowPlayingInfoPropertyDefaultPlaybackRate: 1.0,
            ]
            if let album = metadata?.album { values[MPMediaItemPropertyAlbumTitle] = album }
            if let artwork = metadata?.artwork {
                values[MPMediaItemPropertyArtwork] = Self.artwork(artwork)
            } else {
                values[MPMediaItemPropertyArtwork] = Self.appArtwork
            }
            info.nowPlayingInfo = values
            setEnabled(play: true, seek: true)
        }
    }

    // MARK: - Commands

    private func registerCommands() {
        let commands = MPRemoteCommandCenter.shared()
        commands.playCommand.addTarget { [weak self] _ in self?.play(true) ?? .commandFailed }
        commands.pauseCommand.addTarget { [weak self] _ in self?.play(false) ?? .commandFailed }
        commands.togglePlayPauseCommand.addTarget { [weak self] _ in
            guard let self else { return .commandFailed }
            return self.play(!self.isPlaying)
        }
        commands.changePlaybackPositionCommand.addTarget { [weak self] event in
            guard let self, let event = event as? MPChangePlaybackPositionCommandEvent, self.center.sharedFile != nil else { return .commandFailed }
            self.center.seekSharing(to: event.positionTime)
            return .success
        }
        // Nothing to skip to: a share is one stream.
        for command in [commands.nextTrackCommand, commands.previousTrackCommand, commands.skipForwardCommand, commands.skipBackwardCommand] {
            command.isEnabled = false
        }
        setEnabled(play: false, seek: false)
    }

    private func setEnabled(play: Bool, seek: Bool) {
        let commands = MPRemoteCommandCenter.shared()
        commands.playCommand.isEnabled = play
        commands.pauseCommand.isEnabled = play
        commands.togglePlayPauseCommand.isEnabled = play
        commands.changePlaybackPositionCommand.isEnabled = seek
    }

    private var isPlaying: Bool {
        switch lastContent {
        case let .listening(_, _, isPlaying): isPlaying
        case let .sharing(_, _, _, _, _, isPlaying): isPlaying
        case .some(.none), nil: false
        }
    }

    private func play(_ play: Bool) -> MPRemoteCommandHandlerStatus {
        if center.joined != nil {
            center.setListeningPaused(!play)
            return .success
        }
        guard center.sharedFile != nil else { return .noActionableNowPlayingItem }
        if play { center.resumeSharing() } else { center.pauseSharing() }
        return .success
    }

    /// The app's mark, for a share without artwork of its own.
    private static let appArtwork: MPMediaItemArtwork = {
        let renderer = ImageRenderer(content: AppGlyph(size: 600).frame(width: 600, height: 600))
        renderer.scale = 1
        return artwork(renderer.uiImage ?? UIImage())
    }()

    /// The system asks for the image on a queue of its own, so the handler must not be tied to
    /// the main actor.
    private nonisolated static func artwork(_ image: UIImage) -> MPMediaItemArtwork {
        MPMediaItemArtwork(boundsSize: image.size) { @Sendable _ in image }
    }
}

/// What a shared file says about itself: its title, artist, album and artwork.
struct SharedFileMetadata: Sendable {
    var title: String?
    var artist: String?
    var album: String?
    var artwork: UIImage?

    static func load(_ url: URL) async -> SharedFileMetadata {
        let asset = AVURLAsset(url: url)
        var result = SharedFileMetadata()
        guard let items = try? await asset.load(.commonMetadata) else { return result }
        for item in items {
            switch item.commonKey {
            case .commonKeyTitle: result.title = try? await item.load(.stringValue)
            case .commonKeyArtist: result.artist = try? await item.load(.stringValue)
            case .commonKeyAlbumName: result.album = try? await item.load(.stringValue)
            case .commonKeyArtwork:
                if let data = try? await item.load(.dataValue) { result.artwork = UIImage(data: data) }
            default: break
            }
        }
        return result
    }
}
