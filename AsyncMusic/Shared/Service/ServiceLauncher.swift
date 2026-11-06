//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import Foundation

/// Loads and unloads AsyncMusic Server's launchd job in the user's GUI session.
///
/// The job is loaded from a plist written for the occasion and removed right after, so it lasts
/// until the user logs out or the app unloads it, and isn't a background item the system keeps
/// starting (or tells the user about). Being launchd's own job, the server isn't the app's child:
/// it keeps running when the app quits.
///
/// launchd doesn't start it again once it exits, however it exits: it can't tell a crash from
/// the user quitting or force quitting it in Activity Monitor, which has to stick. While the job
/// is loaded, a message to its Mach service starts it again, so the app, when it's open, brings it
/// back by reconnecting; otherwise it stays off until the app opens or the user logs in again.
///
/// These calls run `launchctl` and wait for it, so keep them off the main thread.
nonisolated enum ServiceLauncher {
    struct Failure: LocalizedError {
        var message: String
        var errorDescription: String? { message }
    }

    /// The server's executable in the app at `appBundle`.
    static func serverExecutable(inApp appBundle: URL) -> URL {
        appBundle.appending(path: "Contents/Helpers/AsyncMusic Server.app/Contents/MacOS/AsyncMusic Server")
    }

    /// Whether the job is loaded, which is whether the server runs or is about to.
    static var isLoaded: Bool {
        launchctl(["print", serviceTarget]).status == 0
    }

    /// Loads the job running `executable`, which starts the server, unless it is loaded already.
    static func start(executable: URL) throws {
        if isLoaded { return }
        let job: [String: Any] = [
            "Label": AsyncMusicService.jobLabel,
            "ProgramArguments": [executable.path],
            "MachServices": [AsyncMusicService.machServiceName: true],
            "RunAtLoad": true,
            "LimitLoadToSessionType": "Aqua",
            // It plays and captures audio in real time.
            "ProcessType": "Interactive",
        ]
        let url = FileManager.default.temporaryDirectory.appending(path: "\(AsyncMusicService.jobLabel).\(UUID().uuidString).plist")
        try PropertyListSerialization.data(fromPropertyList: job, format: .xml, options: 0).write(to: url)
        defer { try? FileManager.default.removeItem(at: url) }
        let result = launchctl(["bootstrap", domain, url.path])
        // Someone else may have loaded it meanwhile, which is as good.
        if result.status != 0, !isLoaded {
            throw Failure(message: "Couldn’t start AsyncMusic Server (\(result.output.isEmpty ? "launchctl \(result.status)" : result.output)).")
        }
    }

    /// Unloads the job, which stops the server; returns once it has exited.
    static func stop() throws {
        guard isLoaded else { return }
        let result = launchctl(["bootout", serviceTarget])
        if result.status != 0, isLoaded {
            throw Failure(message: "Couldn’t stop AsyncMusic Server (\(result.output.isEmpty ? "launchctl \(result.status)" : result.output)).")
        }
    }

    private static var domain: String { "gui/\(getuid())" }
    private static var serviceTarget: String { "\(domain)/\(AsyncMusicService.jobLabel)" }

    private static func launchctl(_ arguments: [String]) -> (status: Int32, output: String) {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/launchctl")
        process.arguments = arguments
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        do {
            try process.run()
        } catch {
            return (-1, error.localizedDescription)
        }
        let output = pipe.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        return (process.terminationStatus, String(decoding: output, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines))
    }
}
#endif
