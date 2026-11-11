//
//  Created by ktiays on 2026/11/11.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import SwiftUI

/// The app's one screen: listen for shares around you and join one, or share an audio file. The
/// Listen / Share switch floats at the bottom, in thumb reach.
struct HomeView: View {
    let center: ShareCenter

    @State private var pendingMode: ShareMode?
    @State private var showsSettings = false

    var body: some View {
        let state = center.state
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 22) {
                    SubtitleFallback(text: subtitle(state))
                    switch state.mode {
                    case .listen: ListenSection(center: center)
                    case .share: ShareSection(center: center)
                    }
                }
                .padding(.horizontal, 20)
                .padding(.top, 4)
                .padding(.bottom, 24)
                .frame(maxWidth: 600)
                .frame(maxWidth: .infinity)
                .animation(.snappy, value: layoutKey(state))
            }
            .navigationTitle(title(state))
            .deviceSubtitle(subtitle(state))
            .toolbar {
                if state.isSharing {
                    ToolbarItem(placement: .topBarTrailing) {
                        Button("Stop", role: .destructive) { center.stopSharing() }
                            .tint(.red)
                    }
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button {
                        showsSettings = true
                    } label: {
                        Label("Settings", systemImage: "gearshape")
                    }
                }
            }
            .safeAreaInset(edge: .bottom, spacing: 0) {
                ModeSwitch(mode: state.mode, select: requestMode)
                    .padding(.bottom, 6)
            }
            .sheet(isPresented: $showsSettings) {
                SettingsView(center: center)
            }
            .alert(pendingPrompt?.title ?? "", isPresented: Binding(get: { pendingMode != nil }, set: { if !$0 { pendingMode = nil } }), presenting: pendingPrompt) { prompt in
                Button(prompt.keepTitle, role: .cancel) {}
                Button(prompt.switchTitle, role: prompt.mode == .listen ? .destructive : nil) { center.setMode(prompt.mode) }
            } message: { prompt in
                Text(prompt.message)
            }
        }
    }

    private func title(_ state: ServerState) -> String {
        switch state.mode {
        case .listen: "Listening"
        case .share: state.isSharing ? "Sharing" : "Share"
        }
    }

    private func subtitle(_ state: ServerState) -> String {
        state.mode == .share && state.isSharing ? "Seen as \(state.deviceName)" : "as \(state.deviceName)"
    }

    /// What changes the screen's shape; not things like the volume, which would lag behind a drag.
    private func layoutKey(_ state: ServerState) -> [AnyHashable] {
        [
            state.mode, state.isSharing, state.joined?.id, state.joinedLayout, state.nearbyOthers.map(\.id), state.listeners.map(\.id),
            state.shareSource?.name, state.isLocalNetworkDenied, state.discoveryError,
        ]
    }

    private var pendingPrompt: ModeSwitchPrompt? {
        pendingMode.map { ModeSwitchPrompt(mode: $0, state: center.state) }
    }

    private func requestMode(_ mode: ShareMode) {
        if center.state.switchInterrupts(to: mode) {
            pendingMode = mode
        } else {
            center.setMode(mode)
        }
    }
}

// MARK: - Mode switch

/// Listen and Share in a floating capsule at the bottom; the selected one is filled, orange
/// for sharing.
private struct ModeSwitch: View {
    let mode: ShareMode
    var select: (ShareMode) -> Void

    @Namespace private var selection

    var body: some View {
        HStack(spacing: 4) {
            item(.listen, title: "Listen", symbol: "headphones")
            item(.share, title: "Share", symbol: "dot.radiowaves.left.and.right")
        }
        .padding(5)
        .modeSwitchBackground()
        .animation(.snappy(duration: 0.3), value: mode)
        .sensoryFeedback(.selection, trigger: mode)
    }

    private func item(_ item: ShareMode, title: String, symbol: String) -> some View {
        let isSelected = item == mode
        return Button {
            select(item)
        } label: {
            Label(title, systemImage: symbol)
                .font(.body.weight(.semibold))
                .lineLimit(1)
                .fixedSize()
                .padding(.horizontal, 20)
                .frame(height: 46)
                // Concrete colors: hierarchical styles turn vibrant (grey) on glass.
                .foregroundStyle(isSelected ? (item == .share ? Color.black : Color.white) : Color.primary)
                .background {
                    if isSelected {
                        Capsule()
                            .fill(item == .share ? Color.orange : Color.accentColor)
                            .matchedGeometryEffect(id: "selection", in: selection)
                    }
                }
                .contentShape(.capsule)
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(isSelected ? .isSelected : [])
    }
}

private extension View {
    @ViewBuilder
    func modeSwitchBackground() -> some View {
        #if os(visionOS)
        glassBackgroundEffect(in: .capsule)
        #else
        if #available(iOS 26.0, *) {
            glassEffect(.regular, in: .capsule)
        } else {
            background(.regularMaterial, in: .capsule)
                .shadow(color: .black.opacity(0.18), radius: 16, y: 8)
        }
        #endif
    }
}

/// The device name under the title where the navigation bar can't show a subtitle.
private struct SubtitleFallback: View {
    let text: String

    var body: some View {
        #if os(visionOS)
        Text(text).font(.subheadline).foregroundStyle(.secondary).padding(.top, -14)
        #else
        if #available(iOS 26.0, *) {
            EmptyView()
        } else {
            Text(text).font(.subheadline).foregroundStyle(.secondary).padding(.top, -14)
        }
        #endif
    }
}

// MARK: - Styles

extension View {
    /// The main action's button: Liquid Glass where the system has it.
    @ViewBuilder
    func prominentButtonStyle() -> some View {
        #if os(visionOS)
        buttonStyle(.borderedProminent)
        #else
        if #available(iOS 26.0, *) {
            buttonStyle(.glassProminent)
        } else {
            buttonStyle(.borderedProminent)
        }
        #endif
    }

    @ViewBuilder
    func secondaryButtonStyle() -> some View {
        #if os(visionOS)
        buttonStyle(.bordered)
        #else
        if #available(iOS 26.0, *) {
            buttonStyle(.glass)
        } else {
            buttonStyle(.bordered)
        }
        #endif
    }

    /// The device name under the title, as the Mac shows it under the app's name.
    @ViewBuilder
    func deviceSubtitle(_ name: String) -> some View {
        #if os(visionOS)
        self
        #else
        if #available(iOS 26.0, *) {
            navigationSubtitle(name)
        } else {
            self
        }
        #endif
    }

    /// A rounded panel set off from the background.
    func card(cornerRadius: CGFloat = 28) -> some View {
        background(.fill.quinary, in: .rect(cornerRadius: cornerRadius))
    }
}

extension ShareDeviceKind {
    /// The symbol for a device of this kind.
    var symbol: String {
        switch self {
        case .laptop: "laptopcomputer"
        case .desktop: "desktopcomputer"
        case .phone: "iphone"
        case .tablet: "ipad"
        case .unknown: "hifispeaker"
        }
    }
}

#Preview {
    HomeView(center: ShareCenter())
}
