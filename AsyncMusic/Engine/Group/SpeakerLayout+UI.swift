//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AsyncMusicCore
import SwiftUI

extension SpeakerLayout: @retroactive Identifiable {
    /// Layouts offered in the UI, in order.
    static let allCases: [SpeakerLayout] = [.everywhere, .stereoPair]

    public var id: UInt8 { rawValue }

    var title: String {
        switch self {
        case .everywhere: "Everywhere"
        case .stereoPair: "Stereo Pair"
        @unknown default: "Unknown"
        }
    }

    var summary: String {
        switch self {
        case .everywhere: "Every device plays the full mix."
        case .stereoPair: "Two devices play the left and right channels."
        @unknown default: ""
        }
    }

    var systemImage: String {
        switch self {
        case .everywhere: "speaker.wave.3.fill"
        case .stereoPair: "hifispeaker.2.fill"
        @unknown default: "questionmark"
        }
    }
}

extension SpeakerChannel {
    var title: String {
        switch self {
        case .full: "Full Mix"
        case .left: "Left"
        case .right: "Right"
        @unknown default: "Unknown"
        }
    }

    /// One or two letters for a speaker badge.
    var badge: String {
        switch self {
        case .full: "LR"
        case .left: "L"
        case .right: "R"
        @unknown default: "?"
        }
    }

    var tint: Color {
        switch self {
        case .full: .secondary
        case .left: .blue
        case .right: .orange
        @unknown default: .secondary
        }
    }
}
