//
//  Created by ktiays on 2026/10/15.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import SwiftUI

struct ContentView: View {
    @State private var session = ClockSyncSession()
    @State private var showsDiagnostics = false

    var body: some View {
        NavigationStack {
            Group {
                switch session.role {
                case .off:
                    WelcomeView(session: session)
                case .leader:
                    HostView(session: session)
                case .follower:
                    JoinView(session: session)
                }
            }
            .animation(.default, value: session.role)
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    Button {
                        showsDiagnostics = true
                    } label: {
                        Label("Diagnostics", systemImage: "waveform.path.ecg")
                    }
                    .help("Diagnostics")
                }
            }
            .navigationDestination(isPresented: $showsDiagnostics) {
                DiagnosticsView(session: session)
            }
        }
        #if os(macOS)
        .frame(minWidth: 420, minHeight: 560)
        #endif
    }
}

#Preview {
    ContentView()
}
