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
            url: "https://github.com/sqliteai/sqlite-js/releases/download/1.3.2/js-apple-xcframework-1.3.2.zip",
            checksum: "5622ed8a3a52ee5d5d10974aa58b2fd701872503d1c56eba5477fa1866be071c"
        ),
        .target(
            name: "js",
            dependencies: ["jsBinary"],
            path: "packages/swift"
        ),
    ]
)
