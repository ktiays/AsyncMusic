// swift-tools-version: 6.0
//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import PackageDescription

// The core is plain C++17 (built with CMake elsewhere); this manifest only exposes it to the
// Apple app, which imports it through Swift's C++ interoperability.
let package = Package(
    name: "AsyncMusicCore",
    platforms: [.iOS(.v18), .macOS(.v15), .visionOS(.v2)],
    products: [
        .library(name: "AsyncMusicCore", targets: ["AsyncMusicCore"]),
    ],
    targets: [
        .target(
            name: "AsyncMusicCore",
            path: ".",
            exclude: ["tests", "CMakeLists.txt", "README.md", "third_party/alac/LICENSE", "third_party/alac/README.md"],
            sources: ["src", "third_party/alac"],
            publicHeadersPath: "include",
            cSettings: [.headerSearchPath("third_party/alac"), .unsafeFlags(["-w"])],
            cxxSettings: [.headerSearchPath("third_party/alac")]
        ),
    ],
    cxxLanguageStandard: .cxx17
)
