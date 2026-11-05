//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import Foundation
import Observation
import ServiceManagement

/// The menu bar app's preferences, kept in user defaults.
@Observable
final class AppSettings {
    static let shared = AppSettings()

    enum AlertBehavior: String, CaseIterable, Identifiable {
        case showAlert
        case onlyList

        var id: Self { self }
        var title: String {
            switch self {
            case .showAlert: "Show Alert"
            case .onlyList: "Only List"
            }
        }
    }

    static let deviceNameKey = "AppSettings.deviceName"
    private static let alertBehaviorKey = "AppSettings.alertBehavior"
    private static let playsAlertSoundKey = "AppSettings.playsAlertSound"

    /// The name other devices see. Empty means the Mac's own name.
    var customDeviceName: String {
        didSet { UserDefaults.standard.set(customDeviceName, forKey: Self.deviceNameKey) }
    }

    var alertBehavior: AlertBehavior {
        didSet { UserDefaults.standard.set(alertBehavior.rawValue, forKey: Self.alertBehaviorKey) }
    }

    var playsAlertSound: Bool {
        didSet { UserDefaults.standard.set(playsAlertSound, forKey: Self.playsAlertSoundKey) }
    }

    private(set) var opensAtLogin = SMAppService.mainApp.status == .enabled
    private(set) var loginItemError: String?

    var deviceName: String {
        let trimmed = customDeviceName.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? Self.systemDeviceName : trimmed
    }

    static var systemDeviceName: String { Host.current().localizedName ?? "Mac" }

    private init() {
        let defaults = UserDefaults.standard
        customDeviceName = defaults.string(forKey: Self.deviceNameKey) ?? ""
        alertBehavior = AlertBehavior(rawValue: defaults.string(forKey: Self.alertBehaviorKey) ?? "") ?? .showAlert
        playsAlertSound = defaults.bool(forKey: Self.playsAlertSoundKey)
    }

    func setOpensAtLogin(_ enabled: Bool) {
        do {
            if enabled {
                try SMAppService.mainApp.register()
            } else {
                try SMAppService.mainApp.unregister()
            }
            loginItemError = nil
        } catch {
            loginItemError = error.localizedDescription
        }
        opensAtLogin = SMAppService.mainApp.status == .enabled
    }
}
#endif
