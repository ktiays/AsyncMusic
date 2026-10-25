// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "MacAudioCapture",
    platforms: [.macOS(.v15)],
    products: [.library(name: "MacAudioCapture", targets: ["MacAudioCapture"])],
    targets: [
        .target(name: "NativeCapture", publicHeadersPath: "include",
                cxxSettings: [.unsafeFlags(["-fobjc-arc"])],
                linkerSettings: [.linkedFramework("CoreAudio", .when(platforms: [.macOS])), .linkedFramework("Foundation")]),
        .target(name: "MacAudioCapture", dependencies: ["NativeCapture"]),
        .testTarget(name: "MacAudioCaptureTests", dependencies: ["MacAudioCapture"]),
    ],
    cxxLanguageStandard: .cxx17
)
