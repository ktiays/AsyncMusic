//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import AppKit
import SettingsKit
import SwiftUI

/// The sidebar sections of the Settings window.
enum SettingsSectionID: Hashable {
    case general
    case alerts
    case about

    /// The Settings window's scene identifier, for `openWindow(id:)`.
    static let windowID = "settings"

    static let groups: [SettingsGroup<Self>] = [
        SettingsGroup(id: "app", sections: [
            SettingsSection("General", id: .general, systemImage: "gearshape", iconColor: .gray,
                            keywords: ["Name", "Login", "Server", "Background", "Service", "Menu Bar", "Dock", "Window"]),
            SettingsSection("Share Alerts", id: .alerts, systemImage: "bell.badge", iconColor: .red,
                            keywords: ["Notifications"]),
            SettingsSection("About", id: .about, systemImage: "info.circle", iconColor: .gray,
                            keywords: ["Version", "Diagnostics", "Quit"]),
        ]),
    ]
}

/// The Form content of one Settings section.
struct SettingsContent: View {
    let section: SettingsSectionID
    let client: ServiceClient
    var openDiagnostics: () -> Void

    @Bindable private var settings = AppSettings.shared

    @ContentBuilder var body: some View {
        switch section {
        case .general:
            Section {
                TextField("Name", text: $settings.customDeviceName, prompt: Text(AppSettings.systemDeviceName))
            } header: {
                Text("This Mac")
            } footer: {
                Text("Other devices see this name when you share or listen.")
            }
            Section {
                Toggle(isOn: Binding(get: { client.isEnabled }, set: { client.setEnabled($0) })) {
                    Text("AsyncMusic Server")
                    Text(serviceStatus)
                }
                Toggle(isOn: Binding(get: { client.startsAtLogin }, set: { client.setStartsAtLogin($0) })) {
                    Text("Start Server at Login")
                    Text("Listens for shares from login, without opening AsyncMusic.")
                }
                .disabled(!client.isEnabled)
                if client.loginItemNeedsApproval {
                    LabeledContent("Allow in Login Items") {
                        Button("Open…") { client.openLoginItemsSettings() }
                    }
                }
            } header: {
                Text("Background Service")
            } footer: {
                if let error = client.loginItemError {
                    Text(error).foregroundStyle(.red)
                } else {
                    Text("The server listens for shares and alerts you after you quit AsyncMusic, until you log out. Turned off, this Mac stops listening and sharing until you turn it back on.")
                }
            }
            Section {
                Toggle(isOn: $settings.showsMenuBarIcon) {
                    Text("Show in Menu Bar")
                    Text("AsyncMusic opens from its menu bar icon. Turned off, it opens in a window and stays in the Dock.")
                }
                Toggle("Open AsyncMusic at Login", isOn: Binding(get: { settings.opensAtLogin }, set: { settings.setOpensAtLogin($0) }))
            } header: {
                Text("App")
            } footer: {
                if let error = settings.loginItemError {
                    Text(error).foregroundStyle(.red)
                }
            }
        case .alerts:
            Section {
                Picker("When someone shares", selection: $settings.alertBehavior) {
                    ForEach(AppSettings.AlertBehavior.allCases) { behavior in
                        Text(behavior.title).tag(behavior)
                    }
                }
            } footer: {
                Text("“Only List” skips the alert; shares still appear in the Listen tab.")
            }
        case .about:
            Section {
                LabeledContent("Version", value: Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "1.0")
                LabeledContent("Diagnostics") {
                    Button("Open…", action: openDiagnostics)
                }
            }
            Section {
                LabeledContent("Quit AsyncMusic") {
                    Button("Quit") { NSApp.terminate(nil) }
                }
            } footer: {
                Text("Quitting closes AsyncMusic. AsyncMusic Server keeps listening for shares until you log out, unless you turn it off in General.")
            }
        }
    }

    private var serviceStatus: String {
        switch client.status {
        case .running: "Running"
        case .starting: "Starting…"
        case .off: "Off"
        case let .failed(message): message
        }
    }
}
#endif
