// js.swift
// Provides the path to the js SQLite extension for use with sqlite3_load_extension.

import Foundation

public struct js {
    /// Returns the absolute path to the js dylib for use with sqlite3_load_extension.
    public static var path: String {
        #if os(macOS)
        return Bundle.main.bundlePath + "/Contents/Frameworks/js.framework/js"
        #else
        return Bundle.main.bundlePath + "/Frameworks/js.framework/js"
        #endif
    }
}
