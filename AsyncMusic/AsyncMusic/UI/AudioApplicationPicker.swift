#if os(macOS)
import AppKit
import SwiftUI

struct AudioApplicationPicker: View {
    let session: ClockSyncSession
    @Environment(\.dismiss) private var dismiss
    @State private var catalog = AudioApplicationCatalog()
    @State private var selection: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Choose App Audio")
                .font(.title2.weight(.semibold))
            Text("Choose an app, even if it is silent. Start sharing and its audio will join your speakers when it plays. Audio is delayed to keep speakers in sync.")
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            List(catalog.applications, selection: $selection) { application in
                HStack(spacing: 12) {
                    if let url = application.appURL {
                        Image(nsImage: NSWorkspace.shared.icon(forFile: url.path))
                            .resizable().frame(width: 32, height: 32)
                    } else {
                        Image(systemName: "waveform").frame(width: 32, height: 32)
                    }
                    VStack(alignment: .leading, spacing: 3) {
                        Text(application.name)
                        Text(application.isPlaying ? "Playing audio" : "Ready")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
                .padding(.vertical, 3)
                .tag(application.id)
            }
            .overlay {
                if catalog.applications.isEmpty {
                    ContentUnavailableView("No Audio Apps", systemImage: "waveform",
                                           description: Text(catalog.error ?? "Open an app, then refresh."))
                }
            }

            HStack {
                Button("Refresh", systemImage: "arrow.clockwise") { catalog.refresh() }
                Spacer()
                Button("Cancel", role: .cancel) { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button("Use App Audio") {
                    guard let application = catalog.applications.first(where: { $0.id == selection }) else { return }
                    session.selectAudioApplication(application)
                    dismiss()
                }
                .buttonStyle(.borderedProminent)
                .keyboardShortcut(.defaultAction)
                .disabled(!catalog.applications.contains(where: { $0.id == selection }))
            }
        }
        .padding(24)
        .frame(width: 480, height: 460)
        .task {
            while !Task.isCancelled {
                catalog.refresh()
                do { try await Task.sleep(for: .seconds(2)) } catch { return }
            }
        }
    }
}
#endif
