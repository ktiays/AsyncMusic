//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import Foundation

/// How the menu bar app and AsyncMusic Server find each other.
///
/// The server runs as a launchd job in the user's GUI session, which the app loads when it
/// starts (`ServiceLauncher`), so the server outlives the app until the user logs out. launchd
/// vends the job's Mach service, which the app connects to. The job isn't a registered
/// background item; the optional login item (`loginAgentPlistName`) only loads it at login.
nonisolated enum AsyncMusicService {
    static let teamIdentifier = "V8K4KDZ2JN"
    static let appBundleIdentifier = "me.ktiays.asyncmusic"
    static let serverBundleIdentifier = "me.ktiays.asyncmusic.server"
    /// The launchd job that runs the server.
    static let jobLabel = serverBundleIdentifier
    static let machServiceName = serverBundleIdentifier
    /// The login item in the app's `Contents/Library/LaunchAgents`, registered with
    /// `SMAppService.agent` when the user wants the server started at login.
    static let loginAgentPlistName = "me.ktiays.asyncmusic.server.login.plist"
    /// Has the server load its job and exit, which is what the login item runs.
    static let startArgument = "--start-service"
    /// In the app's defaults: set while the user has the service turned off. The login item
    /// reads it too, so a service turned off stays off at login.
    static let disabledKey = "ServiceClient.isDisabledByUser"

    /// Accepts only code signed by this team with the given bundle identifier.
    static func codeSigningRequirement(identifier: String) -> String {
        "identifier \"\(identifier)\" and anchor apple generic and certificate leaf[subject.OU] = \"\(teamIdentifier)\""
    }
}

/// What the server exports: the app sends it JSON-encoded `ServerCommand`s.
@objc nonisolated protocol ServerXPC {
    func send(_ command: Data)
}

/// What the app exports: the server sends it a JSON-encoded `ServerState` whenever it changes.
@objc nonisolated protocol ClientXPC {
    func update(_ state: Data)
}

/// Receives XPC messages on XPC's queue and hands their payload to the main actor.
nonisolated final class XPCMessageReceiver: NSObject, ServerXPC, ClientXPC, @unchecked Sendable {
    private let handler: @MainActor (Data) -> Void

    init(handler: @escaping @MainActor (Data) -> Void) {
        self.handler = handler
    }

    func send(_ command: Data) { deliver(command) }
    func update(_ state: Data) { deliver(state) }

    private func deliver(_ data: Data) {
        let handler = self.handler
        DispatchQueue.main.async { handler(data) }
    }
}
#endif
