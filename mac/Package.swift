// swift-tools-version: 6.0
import Foundation
import PackageDescription

// With only the Command Line Tools installed (no Xcode), SwiftPM's default build
// system does not pass the swift-testing macro plugin directory to the compiler,
// so `@Test` fails to expand. Point it there explicitly in that setup only.
let cltTestingPlugins = "/Library/Developer/CommandLineTools/usr/lib/swift/host/plugins/testing"
let testSwiftSettings: [SwiftSetting] =
    FileManager.default.fileExists(atPath: cltTestingPlugins)
        && !FileManager.default.fileExists(atPath: "/Applications/Xcode.app")
    ? [.unsafeFlags(["-plugin-path", cltTestingPlugins])]
    : []

let package = Package(
    name: "VRScreen",
    platforms: [.macOS(.v14)],
    products: [
        .library(name: "MirrorProtocol", targets: ["MirrorProtocol"]),
        .executable(name: "vrscreen-sender", targets: ["vrscreen-sender"]),
        .executable(name: "vrscreen-viewer", targets: ["vrscreen-viewer"]),
    ],
    targets: [
        .target(name: "MirrorProtocol"),
        // The executables are glue around C-callback / AppKit APIs (VideoToolbox,
        // ScreenCaptureKit, Network.framework). All their mutable state is confined
        // to one serial DispatchQueue each, which Swift 6 strict concurrency cannot
        // express without pervasive @unchecked Sendable, so they build in Swift 5 mode.
        .executableTarget(
            name: "vrscreen-sender",
            dependencies: ["MirrorProtocol"],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        .executableTarget(
            name: "vrscreen-viewer",
            dependencies: ["MirrorProtocol"],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        .testTarget(
            name: "MirrorProtocolTests",
            dependencies: ["MirrorProtocol"],
            swiftSettings: testSwiftSettings
        ),
        // Sender-only logic (shortcut table, hotkey event handling, CLI spec parsing).
        .testTarget(
            name: "SenderTests",
            dependencies: ["vrscreen-sender", "MirrorProtocol"],
            swiftSettings: testSwiftSettings + [.swiftLanguageMode(.v5)]
        ),
    ]
)
