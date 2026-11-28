// swift-tools-version: 5.9

import PackageDescription

let package = Package(
    name: "VoiceStick",
    platforms: [
        .macOS(.v12)
    ],
    products: [
        .executable(name: "VoiceStickApp", targets: ["VoiceStickApp"])
    ],
    dependencies: [
        .package(url: "https://github.com/sparkle-project/Sparkle", from: "2.6.0"),
        .package(url: "https://github.com/LebJe/TOMLKit.git", from: "0.6.0"),
    ],
    targets: [
        .target(name: "MotionInput"),
        .testTarget(name: "MotionInputTests", dependencies: ["MotionInput"]),
        .executableTarget(
            name: "VoiceStickApp",
            dependencies: [
                "CZlib", "MotionInput",
                .product(name: "Sparkle", package: "Sparkle"),
                .product(name: "TOMLKit", package: "TOMLKit"),
            ],
            path: "Sources/VoiceStickApp",
            exclude: ["Info.plist"],
            linkerSettings: [
                .unsafeFlags([
                    "-Xlinker", "-sectcreate",
                    "-Xlinker", "__TEXT",
                    "-Xlinker", "__info_plist",
                    "-Xlinker", "Sources/VoiceStickApp/Info.plist",
                ])
            ]
        ),
        .testTarget(name: "VoiceStickAppTests", dependencies: ["VoiceStickApp"]),
        .target(
            name: "CZlib",
            path: "Sources/CZlib",
            publicHeadersPath: "."
        )
    ]
)
