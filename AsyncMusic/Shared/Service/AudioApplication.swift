//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import Foundation

/// An app whose audio can be shared: its processes that play audio, grouped by app. The server
/// finds them (`AudioApplicationCatalog`); the app lists them.
nonisolated struct AudioApplication: Identifiable, Equatable, Codable, Sendable {
    let id: String
    let name: String
    let appURL: URL?
    let processIDs: [UInt32]
    let bundleIDs: [String]
    let isPlaying: Bool
}
#endif
