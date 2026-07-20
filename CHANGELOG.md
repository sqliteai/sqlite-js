# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [1.3.3] - 2026-04-07

### Fixed

- **Flutter package publishing**: the pub.dev release now ships the complete set of native libraries.

## [1.3.2] - 2026-04-01

### Fixed

- **Swift Package on Xcode 26**: the package now uses a binary target with a versioned macOS framework, fixing build failures for apps that depend on `sqlite-js` via Swift Package Manager.

## [1.3.1] - 2026-03-11

### Fixed

- **Flutter package on iOS**: iOS device and simulator binaries are now shipped as separate slices, so Flutter apps build and run correctly on both.

## [1.3.0] - 2026-03-06

### Added

- **Parameter binding in `db.exec()`**: values can now be passed as extra arguments and are bound to the statement's placeholders — e.g. `db.exec("SELECT ? AS a, ? AS b", 42, "hello")`. Supported types are `null`, numbers, `BigInt`, booleans, strings and `ArrayBuffer`. Previously all placeholders were left unbound.
- **`BigInt` return values** from JavaScript functions are now converted to SQLite INTEGER instead of silently becoming `NULL`.
- **String exceptions** (`throw "message"`) now surface `message` as the SQL error text instead of a generic error.

### Changed

- Updated the embedded **QuickJS engine to 0.12.1**, including support for import attributes.

### Fixed

- **Crashes when an aggregate or window function failed to initialize**: SQLite still invokes the final/value/inverse callbacks after a failed step, which dereferenced a NULL context and crashed the process.
- **Crashes when using a `Rowset` after it was finalized** (`next()`, `get()`, `name()`, `toArray()`).
- **`js_load_text()` / `js_load_blob()` on files larger than one byte** always failed to load due to a wrong `fread()` argument order.
- **Redundant writes to the JavaScript functions table**: the duplicate check never matched, so every call rewrote the row. A failure while preparing the write could also lead to undefined behavior.
- **Wrong error reported for window functions**: an error raised in `value` was reported as coming from `final`, and an error in `inverse` as coming from `step`.
- **Memory leaks** on extension initialization failure and when the JavaScript runtime could not be created.

## [1.2.3] - 2026-02-11

### Fixed

- **Flutter package**: native libraries are now included in the published pub.dev package.

## [1.2.0] - 2026-02-10

### Added

- **Flutter package** with prebuilt native libraries for all supported platforms.

### Fixed

- **Android 15+ compatibility**: the Android libraries are now built with 16 KB page size support, required by recent devices and by Google Play.

## [1.1.15] - 2025-10-20

### Added

- **Node package** `@sqliteai/sqlite-js`, shipping the prebuilt extension for Node.js projects.

## [1.1.13] - 2025-10-15

### Fixed

- **Memory leak** when the initialization code of an aggregate function failed to evaluate.

## [1.1.11] - 2025-10-02

### Added

- **Android AAR package**, so the extension can be added to Android projects as a Gradle dependency.

## [1.1.8] - 2025-09-17

### Added

- **Swift Package Manager support**: the repository can be added directly as a package dependency in Xcode projects.

## [1.1.7] - 2025-08-29

### Added

- **Apple `xcframework` and musl (Alpine/static) builds**. macOS and iOS binaries are now signed and notarized.

### Changed

- The license now explicitly grants usage for open-source projects.

## [1.1.6] - 2025-08-06

### Fixed

- **Linux builds** failing to link against the math library.

## [1.1.5] - 2025-06-30

### Added

- **`js_set_max_stack_size()`** to configure the JavaScript engine stack limit. Set it to `0` when the same database connection or runtime is used from multiple threads, to avoid spurious "stack overflow" errors.

### Fixed

- **Memory leaks** when a JavaScript function raised an error.

## [1.1.4] - 2025-06-25

### Fixed

- **Window function state leaking between invocations**: the aggregate context was not reset between separate calls, so results could be affected by a previous query. Related memory leaks in the JavaScript context were also fixed.

## [1.1.3] - 2025-05-23

### Fixed

- **Crash on shutdown** caused by an incorrect teardown order of the JavaScript runtime.
- **Android and Apple builds** producing binaries for the wrong architectures.

## [1.1.2] - 2025-04-15

### Added

- **Android and iOS binaries** to the official releases.

## [1.1.0] - 2025-04-10

Initial public release.

### Added

- **Custom SQLite functions written in JavaScript**: `js_create_scalar()`, `js_create_aggregate()`, `js_create_window()` and `js_create_collation()`.
- **`js_eval()`** to evaluate arbitrary JavaScript from SQL, and **`js_load_text()` / `js_load_blob()`** to load code from a file.
- **`js_init_table()`** to persist JavaScript functions inside the database, so they are automatically restored — and can be synced across devices — when the database is opened.
- **`db` global object** to query the current database from JavaScript, together with the `Rowset` class for iterating results.
- **`js_version()`** to retrieve the extension version.
- Prebuilt binaries for Linux (x86/ARM), macOS (x86/ARM) and Windows.
