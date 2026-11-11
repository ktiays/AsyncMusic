//
//  Created by ktiays on 2026/11/11.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import SwiftUI

/// The same settings as the Mac's: the name others see, how new shares are announced, and
/// Diagnostics.
struct SettingsView: View {
    let center: ShareCenter

    @Environment(\.dismiss) private var dismiss
    @State private var name = ""

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField(ServiceSettings.systemDeviceName, text: $name)
                        .textInputAutocapitalization(.words)
                        .autocorrectionDisabled()
                        .submitLabel(.done)
                        .onSubmit(saveName)
                } header: {
                    Text("Name")
                } footer: {
                    Text("Other devices see this name when you share or listen.")
                }

                Section {
                    Picker("New Shares", selection: Binding(get: { center.settings.alertBehavior }, set: setAlertBehavior)) {
                        ForEach(ServiceSettings.AlertBehavior.allCases) { behavior in
                            Text(behavior.title).tag(behavior)
                        }
                    }
                } header: {
                    Text("Alerts")
                } footer: {
                    Text("While AsyncMusic plays in the background, a share starting nearby shows a notification you can join from. With Only List, shares just appear in the app.")
                }

                Section {
                    NavigationLink("Diagnostics") {
                        DiagnosticsView(session: center.session)
                    }
                } footer: {
                    Text("Clock sync, latency and the session log, for checking how well devices play together.")
                }
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") {
                        saveName()
                        dismiss()
                    }
                }
            }
            .onAppear { name = center.settings.customDeviceName }
            .onDisappear(perform: saveName)
        }
    }

    private func saveName() {
        var settings = center.settings
        settings.customDeviceName = name.trimmingCharacters(in: .whitespacesAndNewlines)
        center.updateSettings(settings)
    }

    private func setAlertBehavior(_ behavior: ServiceSettings.AlertBehavior) {
        var settings = center.settings
        settings.alertBehavior = behavior
        center.updateSettings(settings)
        if behavior == .showAlert { ShareNotifications.requestAuthorization() }
    }
}
