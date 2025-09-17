// swift-tools-version: 6.1
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "js",
    platforms: [.macOS(.v11), .iOS(.v11)],
    products: [
        // Products can be used to vend plugins, making them visible to other packages.
        .plugin(
            name: "jsPlugin",
            targets: ["jsPlugin"]),
        .library(
            name: "js",
            targets: ["js"])
    ],
    targets: [
        // Build tool plugin that invokes the Makefile
        .plugin(
            name: "jsPlugin",
            capability: .buildTool(),
            path: "packages/swift/plugin"
        ),
        // js library target
        .target(
            name: "js",
            dependencies: [],
            path: "packages/swift/extension",
            plugins: ["jsPlugin"]
        ),
    ]
)