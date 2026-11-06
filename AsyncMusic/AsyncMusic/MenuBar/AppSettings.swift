//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import Foundation
import Observation
import ServiceManagement

/// The menu bar app's preferences, kept in user defaults. The ones AsyncMusic Server acts on
/// are sent to it as `service` whenever they change.
@Observable
final class AppSettings {
    static let shared = AppSettings()

    typealias AlertBehavior = ServiceSettings.AlertBehavior

    static let deviceNameKey = "AppSettings.deviceName"
    private static let alertBehaviorKey = "AppSettings.alertBehavior"
    private static let showsMenuBarIconKey = "AppSettings.showsMenuBarIcon"

    /// The name other devices see. Empty means the Mac's own name.
    var customDeviceName: String {
        didSet { UserDefaults.standard.set(customDeviceName, forKey: Self.deviceNameKey) }
    }

    var alertBehavior: AlertBehavior {
        didSet { UserDefaults.standard.set(alertBehavior.rawValue, forKey: Self.alertBehaviorKey) }
    }

    /// Whether the app lives in the menu bar, its window a panel hanging from the icon. Without
    /// the icon it is an ordinary app: a titled main window and a Dock icon. On at first.
    var showsMenuBarIcon: Bool {
        didSet { UserDefaults.standard.set(showsMenuBarIcon, forKey: Self.showsMenuBarIconKey) }
    }

    private(set) var opensAtLogin = SMAppService.mainApp.status == .enabled
    private(set) var loginItemError: String?

    /// What the server needs to know.
    var service: ServiceSettings {
        ServiceSettings(customDeviceName: customDeviceName, alertBehavior: alertBehavior)
    }

    var deviceName: String { service.deviceName }

    static var systemDeviceName: String { ServiceSettings.systemDeviceName }

    private init() {
        let defaults = UserDefaults.standard
        customDeviceName = defaults.string(forKey: Self.deviceNameKey) ?? ""
        alertBehavior = AlertBehavior(rawValue: defaults.string(forKey: Self.alertBehaviorKey) ?? "") ?? .showAlert
        defaults.register(defaults: [Self.showsMenuBarIconKey: true])
        showsMenuBarIcon = defaults.bool(forKey: Self.showsMenuBarIconKey)
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
