#if os(macOS)
import AppKit
import MacAudioCapture
import Observation

/// The apps whose audio this Mac can share, found from the running apps and the audio processes.
@Observable
final class AudioApplicationCatalog {
    private(set) var applications: [AudioApplication] = []
    private(set) var error: String?

    func refresh() {
        // AsyncMusic Server lives inside the app; neither is offered as a source.
        let ownApp = Self.outermostApp(containing: Bundle.main.bundleURL).standardizedFileURL
        var groups: [String: (url: URL?, name: String?, bundleIDs: Set<String>, processes: [AudioProcess])] = [:]
        // App selection is independent of whether an app has opened an audio client yet.
        for app in NSWorkspace.shared.runningApplications where app.activationPolicy == .regular {
            guard app.bundleIdentifier != Bundle.main.bundleIdentifier,
                  let url = app.bundleURL?.standardizedFileURL, !Self.isInside(url, ownApp) else { continue }
            groups[url.path] = (url, app.localizedName, Set(app.bundleIdentifier.map { [$0] } ?? []), [])
        }
        let processes: [AudioProcess]
        do {
            processes = try AudioProcess.snapshot()
            error = nil
        } catch {
            processes = []
            self.error = error.localizedDescription
        }
        for process in processes {
            if process.bundleID == Bundle.main.bundleIdentifier { continue }
            let executable = URL(fileURLWithPath: process.executablePath)
            let components = executable.pathComponents
            let appIndex = components.firstIndex { $0.hasSuffix(".app") }
            let appURL = appIndex.map { URL(fileURLWithPath: NSString.path(withComponents: Array(components.prefix($0+1)))) }
            if let appURL, Self.isInside(appURL.standardizedFileURL, ownApp) { continue }
            // Audio-capable foreground apps stay selectable when paused. Background
            // system services and command-line players appear only while outputting audio.
            if appURL == nil || process.executablePath.hasPrefix("/System/Library/") {
                guard process.isOutputRunning else { continue }
            }
            let id = appURL?.path ?? "pid:\(process.pid)"
            if groups[id] == nil { groups[id] = (appURL, nil, [], []) }
            groups[id]?.processes.append(process)
            if !process.bundleID.isEmpty { groups[id]?.bundleIDs.insert(process.bundleID) }
        }
        applications = groups.map { id, group in
            let displayName = group.name ?? group.url.map { FileManager.default.displayName(atPath: $0.path) }
                ?? group.processes.first.map { URL(fileURLWithPath: $0.executablePath).lastPathComponent } ?? "Audio Process"
            let name = displayName.hasSuffix(".app") ? String(displayName.dropLast(4)) : displayName
            return AudioApplication(id: id, name: name.isEmpty ? "Audio Process" : name, appURL: group.url,
                                    processIDs: group.processes.map(\.objectID).sorted(),
                                    bundleIDs: group.bundleIDs.sorted(),
                                    isPlaying: group.processes.contains(where: \.isOutputRunning))
        }.sorted { a, b in
            a.isPlaying == b.isPlaying ? a.name.localizedStandardCompare(b.name) == .orderedAscending : a.isPlaying
        }
    }

    /// The first `.app` on the path to `url`, or `url` itself when it isn't inside one.
    private static func outermostApp(containing url: URL) -> URL {
        let components = url.standardizedFileURL.pathComponents
        guard let index = components.firstIndex(where: { $0.hasSuffix(".app") }) else { return url }
        return URL(fileURLWithPath: NSString.path(withComponents: Array(components.prefix(index + 1))))
    }

    private static func isInside(_ url: URL, _ app: URL) -> Bool {
        url == app || url.path.hasPrefix(app.path + "/")
    }
}
#endif
