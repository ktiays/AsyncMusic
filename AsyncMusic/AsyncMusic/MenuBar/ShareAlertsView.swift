//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import AsyncMusicCore
import SwiftUI

/// The alert that grows out of the menu bar icon when a share starts. One share gets one card;
/// several stack up behind the newest, page with the arrows or a swipe, and fan out into
/// separate cards while the pointer rests on them. Closing it (click elsewhere, Esc, or waiting)
/// is how a share is ignored; it stays listed in the panel.
struct ShareAlertsView: View {
    let center: ShareCenter
    /// Where the icon is, from the view's leading edge: the arrow points there.
    let anchorX: CGFloat
    /// When the alert closes by itself.
    let deadline: Date
    let duration: TimeInterval

    @State private var page = 0
    @State private var isFanned = false
    @State private var isShown = false

    private var alerts: [ShareCenter.NearbyShare] { center.alerts }
    private static let width: CGFloat = 340

    var body: some View {
        VStack(spacing: 0) {
            Color.clear
                .frame(width: 22, height: 9)
                .glassEffect(.regular, in: Arrow())
                .offset(x: anchorX - Self.width / 2, y: 1)
            Group {
                if alerts.count > 1, isFanned {
                    fanned
                } else {
                    stacked
                }
            }
        }
        .frame(width: Self.width)
        .scaleEffect(isShown ? 1 : 0.4, anchor: UnitPoint(x: anchorX / Self.width, y: 0))
        .opacity(isShown ? 1 : 0)
        .onAppear {
            withAnimation(.spring(response: 0.38, dampingFraction: 0.72)) { isShown = true }
        }
        .onHover { hovering in
            withAnimation(.spring(response: 0.35, dampingFraction: 0.8)) { isFanned = hovering }
        }
        .onChange(of: alerts.map(\.id)) { old, new in
            // A new share goes to the front.
            if let first = new.first, first != old.first { page = 0 }
            page = min(page, max(new.count - 1, 0))
        }
        .animation(.spring(response: 0.35, dampingFraction: 0.8), value: alerts.map(\.id))
        .animation(.spring(response: 0.35, dampingFraction: 0.8), value: page)
    }

    // MARK: Stacked

    private var stacked: some View {
        ZStack(alignment: .top) {
            // The older shares peek out below the front card.
            ForEach(Array(alerts.indices.dropFirst().prefix(2)), id: \.self) { depth in
                RoundedRectangle(cornerRadius: 20)
                    .fill(.clear)
                    .glassEffect(.regular, in: .rect(cornerRadius: 20))
                    .opacity(depth == 1 ? 0.8 : 0.55)
                    .padding(.horizontal, CGFloat(depth) * 12)
                    .offset(y: CGFloat(depth) * 8)
            }
            if let share = alerts[safe: page] {
                AlertCard(share: share, header: alerts.count > 1 ? pager : nil, deadline: deadline, duration: duration) {
                    center.join(share)
                }
                .id(share.id)
                .transition(.asymmetric(insertion: .scale(scale: 0.96, anchor: .top).combined(with: .opacity), removal: .opacity))
            }
        }
        .padding(.bottom, CGFloat(min(alerts.count - 1, 2)) * 8)
        .gesture(DragGesture(minimumDistance: 20).onEnded { value in
            if value.translation.width < -20 { page = min(page + 1, alerts.count - 1) }
            if value.translation.width > 20 { page = max(page - 1, 0) }
        })
    }

    private var pager: AnyView {
        AnyView(HStack(spacing: 8) {
            HStack(spacing: -6) {
                ForEach(alerts.prefix(3)) { share in
                    DeviceTile(kind: share.sourceKind, size: 20)
                        .background(.background, in: .circle)
                }
            }
            Text("\(alerts.count) shares nearby").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
            Spacer()
            Button {
                page = max(page - 1, 0)
            } label: {
                Image(systemName: "chevron.left")
            }
            .disabled(page == 0)
            .accessibilityLabel("Previous share")
            Text("\(page + 1) / \(alerts.count)").font(.caption.monospacedDigit()).foregroundStyle(.secondary)
            Button {
                page = min(page + 1, alerts.count - 1)
            } label: {
                Image(systemName: "chevron.right")
            }
            .disabled(page >= alerts.count - 1)
            .accessibilityLabel("Next share")
        }
        .buttonStyle(.borderless)
        .controlSize(.small))
    }

    // MARK: Fanned

    private var fanned: some View {
        VStack(spacing: 8) {
            HStack {
                Text("\(alerts.count) shares nearby").font(.subheadline.weight(.semibold))
                Spacer()
                Button("Close") { center.dismissAlerts() }
                    .buttonStyle(.borderless)
                    .controlSize(.small)
            }
            .padding(.horizontal, 14)
            .frame(height: 34)
            .glassEffect(.regular, in: .capsule)

            ForEach(Array(alerts.enumerated()), id: \.element.id) { index, share in
                if index == 0 {
                    AlertCard(share: share, header: nil, deadline: deadline, duration: duration) { center.join(share) }
                } else {
                    CompactAlertRow(share: share) { center.join(share) }
                        .transition(.move(edge: .top).combined(with: .opacity))
                }
            }
        }
    }
}

private struct AlertCard: View {
    let share: ShareCenter.NearbyShare
    let header: AnyView?
    let deadline: Date
    let duration: TimeInterval
    var join: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if let header {
                header
                Divider()
            }
            HStack(alignment: .top, spacing: 12) {
                DeviceTile(kind: share.sourceKind, size: 42)
                VStack(alignment: .leading, spacing: 3) {
                    Text("\(share.name) is sharing audio").font(.headline).fixedSize(horizontal: false, vertical: true)
                    Text(share.summary).font(.subheadline).foregroundStyle(.secondary)
                }
                Spacer(minLength: 0)
            }
            Button(action: join) {
                Text("Join").frame(maxWidth: .infinity)
            }
            .buttonStyle(AlertJoinButtonStyle())
        }
        .padding(16)
        .overlay(alignment: .bottom) {
            TimelineView(.animation(minimumInterval: 0.25)) { context in
                let remaining = max(0, deadline.timeIntervalSince(context.date)) / duration
                GeometryReader { proxy in
                    Capsule().fill(.tint.opacity(0.5)).frame(width: proxy.size.width * remaining, height: 3)
                }
                .frame(height: 3)
                .padding(.horizontal, 22)
                .padding(.bottom, 5)
            }
            .accessibilityHidden(true)
        }
        .glassEffect(.regular, in: .rect(cornerRadius: 20))
    }
}

private struct CompactAlertRow: View {
    let share: ShareCenter.NearbyShare
    var join: () -> Void

    var body: some View {
        HStack(spacing: 12) {
            DeviceTile(kind: share.sourceKind, size: 32)
            VStack(alignment: .leading, spacing: 1) {
                Text(share.name).font(.body.weight(.semibold)).lineLimit(1)
                Text("\(share.summary) · \(share.firstSeen, format: .relative(presentation: .named))")
                    .font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
            Spacer(minLength: 8)
            Button("Join", action: join)
                .buttonStyle(AlertJoinButtonStyle(isCompact: true))
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .glassEffect(.regular, in: .rect(cornerRadius: 18))
    }
}

/// The alert's Join button. The alert never takes keyboard focus from the app in use, and the
/// system's prominent styles draw grey in a window without it, so this one is drawn directly.
private struct AlertJoinButtonStyle: ButtonStyle {
    var isCompact = false

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(isCompact ? .subheadline.weight(.semibold) : .body.weight(.semibold))
            .foregroundStyle(.white)
            .padding(.horizontal, isCompact ? 14 : 0)
            .frame(height: isCompact ? 26 : 32)
            .background(Color.accentColor.opacity(configuration.isPressed ? 0.75 : 1), in: .capsule)
            .contentShape(.capsule)
    }
}

/// The popover arrow pointing at the menu bar icon.
private struct Arrow: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()
        path.move(to: CGPoint(x: rect.minX, y: rect.maxY))
        path.addQuadCurve(to: CGPoint(x: rect.midX, y: rect.minY), control: CGPoint(x: rect.midX - rect.width * 0.18, y: rect.maxY))
        path.addQuadCurve(to: CGPoint(x: rect.maxX, y: rect.maxY), control: CGPoint(x: rect.midX + rect.width * 0.18, y: rect.maxY))
        path.closeSubpath()
        return path
    }
}

extension Array {
    subscript(safe index: Int) -> Element? { indices.contains(index) ? self[index] : nil }
}
#endif
