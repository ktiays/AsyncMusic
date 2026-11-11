//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import Foundation
#if canImport(UIKit)
import UIKit
#endif

/// This device's stable identity in a group: an ID kept across launches (so a leader remembers
/// which device it paired as a speaker) and the name shown to others.
nonisolated struct DeviceIdentity: Sendable {
    let id: UInt64
    let name: String

    private static let idKey = "DeviceIdentity.id"

    /// Where the name chosen in settings is kept (the menu bar app's settings on a Mac, the
    /// app's own elsewhere).
    static let customNameKey = "AppSettings.deviceName"

    @MainActor
    static func current() -> DeviceIdentity {
        let id = storedID()
        let custom = UserDefaults.standard.string(forKey: customNameKey)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return DeviceIdentity(id: id, name: custom.isEmpty ? systemName : custom)
    }

    /// The name used when none was chosen.
    @MainActor
    static var systemName: String {
        #if os(macOS)
        Host.current().localizedName ?? "Mac"
        #else
        // Apps only see the generic model name ("iPhone"), so tell same-model devices apart
        // with a short code derived from the ID.
        "\(UIDevice.current.name) \(String(storedID() & 0xFFFF, radix: 16, uppercase: true))"
        #endif
    }

    private static func storedID() -> UInt64 {
        let defaults = UserDefaults.standard
        var id = (defaults.object(forKey: idKey) as? NSNumber)?.uint64Value ?? 0
        if id == 0 {
            id = .random(in: 1 ... .max)
            defaults.set(NSNumber(value: id), forKey: idKey)
        }
        return id
    }
}
