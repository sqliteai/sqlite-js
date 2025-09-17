// js.swift
// This file serves as a placeholder for the js target.
// The actual SQLite extension is built using the Makefile through the build plugin.

import Foundation

/// Placeholder structure for js
public struct js {
    /// Returns the path to the built js dylib inside the XCFramework
    public static var path: String {
        #if os(macOS)
        return "js.xcframework/macos-arm64_x86_64/js.framework/js"
        #elseif targetEnvironment(simulator)
        return "js.xcframework/ios-arm64_x86_64-simulator/js.framework/js"
        #else
        return "js.xcframework/ios-arm64/js.framework/js"
        #endif
    }
}