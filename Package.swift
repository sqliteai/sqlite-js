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
            url: "https://github.com/sqliteai/sqlite-js/releases/download/1.4.3/js-apple-xcframework-1.4.3.zip",
            checksum: "9614fae861f1dc44e8cc869dc3db3df1d56c805cb8084b17ca57a7a07fda2479"
        ),
        .target(
            name: "js",
            dependencies: ["jsBinary"],
            path: "packages/swift"
        ),
    ]
)
