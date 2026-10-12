// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "CatboxKit",
    platforms: [.macOS(.v12)],
    products: [
        .library(name: "CatboxKit", targets: ["CatboxKit"]),
        .executable(name: "catbox", targets: ["catbox"]),
    ],
    dependencies: [
        .package(url: "https://github.com/apple/swift-argument-parser", from: "1.3.0"),
    ],
    targets: [
        .target(name: "CatboxKit"),
        .executableTarget(
            name: "catbox",
            dependencies: [
                "CatboxKit",
                .product(name: "ArgumentParser", package: "swift-argument-parser"),
            ]
        ),
        .testTarget(name: "CatboxKitTests", dependencies: ["CatboxKit"]),
    ]
)
