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
            url: "https://github.com/sqliteai/sqlite-js/releases/download/1.4.0/js-apple-xcframework-1.4.0.zip",
            checksum: "22aa4fca5107395306edfb4c7a18704981174f153bd6b95bab4464443ea7a934"
        ),
        .target(
            name: "js",
            dependencies: ["jsBinary"],
            path: "packages/swift"
        ),
    ]
)
