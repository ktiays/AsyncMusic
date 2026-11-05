//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import Foundation
import Observation
import os

/// Vends the server's Mach service to the menu bar app: decodes its commands and sends it the
/// server's state whenever any of it changes.
final class ServiceListener: NSObject, NSXPCListenerDelegate {
    /// Handles a command from the app.
    var onCommand: ((ServerCommand) -> Void)?
    /// Called when the last app disconnects.
    var onAllClientsGone: (() -> Void)?

    private let center: ShareCenter
    private let listener = NSXPCListener(machServiceName: AsyncMusicService.machServiceName)
    private var clients: [NSXPCConnection] = []
    private var sentState: ServerState?
    private var isPushScheduled = false
    private let log = Logger(subsystem: AsyncMusicService.serverBundleIdentifier, category: "Service")

    init(center: ShareCenter) {
        self.center = center
        super.init()
    }

    func resume() {
        listener.delegate = self
        listener.setConnectionCodeSigningRequirement(AsyncMusicService.codeSigningRequirement(identifier: AsyncMusicService.appBundleIdentifier))
        listener.resume()
        observeState()
    }

    nonisolated func listener(_ listener: NSXPCListener, shouldAcceptNewConnection connection: NSXPCConnection) -> Bool {
        connection.exportedInterface = NSXPCInterface(with: ServerXPC.self)
        connection.remoteObjectInterface = NSXPCInterface(with: ClientXPC.self)
        nonisolated(unsafe) let unsafeConnection = connection
        connection.exportedObject = XPCMessageReceiver { [weak self] data in
            self?.receive(data)
        }
        connection.invalidationHandler = { [weak self] in
            DispatchQueue.main.async { self?.remove(unsafeConnection) }
        }
        connection.resume()
        DispatchQueue.main.async { [weak self] in self?.add(unsafeConnection) }
        return true
    }

    private func add(_ connection: NSXPCConnection) {
        log.info("App connected (pid \(connection.processIdentifier))")
        clients.append(connection)
        send(center.state, to: connection)
    }

    private func remove(_ connection: NSXPCConnection) {
        guard clients.contains(where: { $0 === connection }) else { return }
        log.info("App disconnected (pid \(connection.processIdentifier))")
        clients.removeAll { $0 === connection }
        if clients.isEmpty { onAllClientsGone?() }
    }

    private func receive(_ data: Data) {
        do {
            let command = try JSONDecoder().decode(ServerCommand.self, from: data)
            onCommand?(command)
        } catch {
            log.error("Unreadable command: \(error.localizedDescription)")
        }
    }

    // MARK: - State

    /// Pushes the state once per change, coalescing a burst of changes into one message.
    private func observeState() {
        let state = withObservationTracking {
            center.state
        } onChange: { [weak self] in
            DispatchQueue.main.async { self?.schedulePush() }
        }
        push(state)
    }

    private func schedulePush() {
        guard !isPushScheduled else { return }
        isPushScheduled = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.03) { [weak self] in
            guard let self else { return }
            isPushScheduled = false
            observeState()
        }
    }

    private func push(_ state: ServerState) {
        guard state != sentState else { return }
        sentState = state
        for client in clients { send(state, to: client) }
    }

    private func send(_ state: ServerState, to connection: NSXPCConnection) {
        guard let data = try? JSONEncoder().encode(state) else { return }
        let proxy = connection.remoteObjectProxyWithErrorHandler { _ in } as? ClientXPC
        proxy?.update(data)
    }
}
