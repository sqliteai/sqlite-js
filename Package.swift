// swift-tools-version: 6.1
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "js",
    platforms: [.macOS(.v11), .iOS(.v11)],
    products: [
        .library(
            name: "js",
            targets: ["js"])
    ],
    targets: [
        .binaryTarget(
            name: "jsBinary",
            url: "https://github.com/sqliteai/sqlite-js/releases/download/1.4.4/js-apple-xcframework-1.4.4.zip",
            checksum: "72c71adf027192efead5f16113a970a1e556e268797dea672f7eee8f8d32e947"
        ),
        .target(
            name: "js",
            dependencies: ["jsBinary"],
            path: "packages/swift"
        ),
    ]
)
