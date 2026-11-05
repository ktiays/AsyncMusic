//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import AppKit
import SwiftUI

/// Settings, shown in place of the panel's main content.
struct SettingsPanel: View {
    let center: ShareCenter
    var openDiagnostics: () -> Void
    var back: () -> Void

    @Bindable private var settings = AppSettings.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 4) {
                Button(action: back) {
                    Image(systemName: "chevron.left").font(.body.weight(.semibold))
                }
                .buttonStyle(.borderless)
                .keyboardShortcut(.cancelAction)
                .accessibilityLabel("Back")
                Text("Settings").font(.headline)
            }

            PanelSection("This Mac") {
                PanelRow("Name") {
                    TextField("Name", text: $settings.customDeviceName, prompt: Text(AppSettings.systemDeviceName))
                        .textFieldStyle(.plain)
                        .multilineTextAlignment(.trailing)
                        .foregroundStyle(.secondary)
                        .labelsHidden()
                }
                Divider().padding(.leading, 12)
                PanelRow("Open at Login") {
                    Toggle("Open at Login", isOn: Binding(get: { settings.opensAtLogin }, set: { settings.setOpensAtLogin($0) }))
                        .toggleStyle(.switch)
                        .controlSize(.small)
                        .labelsHidden()
                }
            }
            if let error = settings.loginItemError {
                Text(error).font(.footnote).foregroundStyle(.red).padding(.horizontal, 6)
            }

            PanelSection("Share Alerts") {
                PanelRow("When someone shares") {
                    Picker("When someone shares", selection: $settings.alertBehavior) {
                        ForEach(AppSettings.AlertBehavior.allCases) { behavior in
                            Text(behavior.title).tag(behavior)
                        }
                    }
                    .pickerStyle(.menu)
                    .labelsHidden()
                    .fixedSize()
                }
                Divider().padding(.leading, 12)
                PanelRow("Play sound") {
                    Toggle("Play sound", isOn: $settings.playsAlertSound)
                        .toggleStyle(.switch)
                        .controlSize(.small)
                        .labelsHidden()
                }
            }
            Text("“Only List” skips the alert; shares still appear in the Listen tab.")
                .font(.footnote)
                .foregroundStyle(.secondary)
                .padding(.horizontal, 6)

            HStack {
                Text("Version \(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "1.0")")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                Spacer()
                Button("Diagnostics…", action: openDiagnostics)
                Button("Quit AsyncMusic") { NSApp.terminate(nil) }
            }
            .controlSize(.small)
        }
    }
}
#endif
