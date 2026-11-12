# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [1.4.1] - 2026-11-12

### Fixed

- **Node.js: the package could not load the extension when imported as an ES module.** `import { getExtensionPath } from '@sqliteai/sqlite-js'`, as in the README, threw `ExtensionNotFoundError` saying the platform package (for example `@sqliteai/sqlite-js-linux-x86_64`) was not installed, even when it was. The ESM build could not use `require` to load the platform package, and the real error was discarded. The package now loads correctly from native ES modules, from CommonJS, and from esbuild and webpack bundles in both CommonJS and ESM output, including webpack bundles of the CommonJS build, which also failed before.
- **Node.js: errors that prevented the platform package from loading were hidden.** `ExtensionNotFoundError` now says "not installed" only when the platform package is actually missing. Any other error raised while loading it is included in the message and available as the error's `cause`, and a platform package whose binary is missing gets its own message.

## [1.4.0] - 2026-11-12

Four changes in this release can alter the behavior of SQL that works on 1.3.7. JavaScript now has a
memory limit, 64 MB by default, where it had none, so code that allocates more than that fails with
"out of memory" until the limit is raised — see `js_set_memory_limit()` under **Added**. The other
three are under **Changed**: the functions that run JavaScript can no longer be called from views,
triggers and other schema objects stored in the database; a whole number that came back as REAL now
comes back as INTEGER; and `js_set_max_stack_size()` rejects a size larger than the calling thread's
stack.

### Added

- **`js_set_memory_limit()`**: sets the most memory, in bytes, that JavaScript may allocate on a connection, with 0 meaning no limit. There is now a default limit of 64 MB, where memory was previously unbounded and runaway JavaScript could exhaust the process. **This can stop SQL that works today**: code that legitimately allocates more than 64 MB, such as one large string or buffer built in JavaScript, now fails with "out of memory". Call `js_set_memory_limit(0)` for the old unbounded behavior, or a larger figure for a bound you choose, and build with `-DSQLITE_JS_DEFAULT_MEMORY_LIMIT=bytes` to change the default. Reaching the limit leaves the connection usable rather than breaking it: garbage is collected before the limit is reached and as the limit changes, so a new limit below what a connection has already allocated still leaves it working. A limit below the memory that is still *in use*, such as a large value held in a global, is the one case that blocks further allocation until the limit is raised again.
- **`sqlite3_interrupt()` now stops running JavaScript**: the statement fails with an "interrupted" error, even if the JavaScript catches exceptions, and Ctrl-C in the `sqlite3` shell does this. Previously no interrupt handler was installed, so JavaScript ran to the end on its own terms and a loop such as `for (;;) {}` could only be stopped by killing the process. This needs SQLite 3.41.0 or later, which added `sqlite3_is_interrupted()`; against an older library the extension loads and behaves as before.
- **`ArrayBuffer` and typed array values become BLOBs.** A function that returned `new Uint8Array([1, 2, 3])` or an `ArrayBuffer` previously produced NULL, so a function could not even give back the BLOB it was handed: a BLOB argument arrives as an `ArrayBuffer`, but `(function (args) { return args[0]; })` turned `x'0102'` into NULL. Typed arrays are now accepted as `db.exec()` parameters too, where they previously bound NULL, and a typed array uses its own byte range, so a view onto part of a buffer is just that part.

### Changed

- **The functions that run JavaScript are registered `SQLITE_DIRECTONLY`.** Every function this extension registers except `js_version` now runs only from top-level SQL: a view, trigger, CHECK constraint, generated column or index **stored in the database** can no longer call it, and fails with an error such as "unsafe use of js_eval()". Previously a database file from an untrusted source could run arbitrary JavaScript, read files through `js_load_text()` or change the runtime simply by being queried. TEMP views and triggers are unaffected, since only the connection itself can create them, and the functions you create with `js_create_scalar()`, `js_create_aggregate()`, `js_create_window()` and `js_create_collation()` are not given the flag, so a stored view or trigger can still call those. **If you keep `js_*` calls in a stored view or trigger, move them into top-level SQL or recreate the object as TEMP.** A database can still hold JavaScript in its `js_functions` table, which `js_init_table(1)` runs, so call that only on databases you trust.
- **Whole numbers are returned as INTEGER.** A number from JavaScript now becomes an INTEGER if its value is a whole number that `Number.isSafeInteger()` accepts, from -(2^53 - 1) to 2^53 - 1, and a REAL otherwise. The result previously followed the value's internal representation in the engine rather than the value itself, which was inconsistent: a function returning its REAL argument unchanged gave back REAL `3.0`, while one computing `6 / 2` gave the INTEGER `3`. The same SQL can therefore now report `integer` where it reported `real` — note that a REAL column holding `3.0` round-trips through JavaScript as the INTEGER `3`, because JavaScript has only one number type.
- **`js_set_max_stack_size()` rejects a size larger than the calling thread's stack**, reporting the largest size allowed, where it previously accepted any value. A size beyond the real stack is no limit at all: QuickJS's check would never trip and code that calls itself without end would crash the process instead of failing with "Maximum call stack size exceeded".

### Fixed

- **Exceptions thrown by an aggregate's or a window function's `step` or `inverse` were discarded, producing wrong results with no error.** An aggregate whose `step` threw on one row silently skipped that row and returned a plausible value for the rest; the statement now fails with the exception's message.
- **Errors raised while a `db.exec()` result set runs were discarded.** A constraint failure reported by `next()` or `toArray()` was not thrown, so JavaScript that inserted a conflicting row saw a successful call and no row inserted. Such errors are now thrown, as an error in the SQL itself is already thrown from `db.exec()`.
- **A buffer whose bytes could not be read became NULL instead of an error.** A detached `ArrayBuffer` passed to `db.exec()` bound SQL NULL and reported success; it now fails with "ArrayBuffer is detached". The same holds for a typed array whose view no longer falls inside its buffer, after the buffer was detached or resized, in a parameter or in a function's result.
- **Deep recursion in JavaScript could crash the process instead of failing with "Maximum call stack size exceeded".** The engine's 1 MB default stack was used however much stack the calling thread actually had, so a thread with less — a pthread created with the 512 KB default on macOS, for instance — overflowed its real stack first. Each call into JavaScript now measures the stack of the thread it runs on and lowers the size to fit, keeping a margin for the native code that runs between the engine's checks, so a connection used from several threads is safe on all of them.

## [1.3.7] - 2026-11-01

### Fixed

- **Flutter package publishing**: the `LICENSE` symlink inside the Flutter package still pointed at the `LICENSE.md` that was replaced by `LICENSE`, so `dart pub publish` rejected the package and 1.3.6 never reached pub.dev. The Flutter package is published again from this release, which also carries every fix listed under 1.3.6.

## [1.3.6] - 2026-11-01

### Fixed

- **Crashes when loading a function definition with missing code**: a row in the JavaScript functions table whose `step_code` or `final_code` was `NULL` dereferenced a NULL pointer and crashed the process on `js_init_table(1)`. The code required by each function kind is now validated before use.
- **Out-of-bounds read when the JavaScript functions table has an unexpected layout**: the loader selected every column and trusted their order and count, reading past the end of the result in release builds, where the assertion that guarded it is compiled out. It now selects its seven columns explicitly and aborts with a clear error if they do not match.
- **Use-after-free when extension initialization failed**: the global JavaScript context was released twice, because SQLite already invokes the destructor callback when registering a function fails. Depending on where the failure happened this either touched freed memory or leaked the whole JavaScript runtime.
- **Leak when registering a collation failed**: unlike `sqlite3_create_function_v2()`, `sqlite3_create_collation_v2()` does not invoke the destructor callback on failure, so the function context and the reference it held on the JavaScript runtime were never released.
- **Unknown function kinds silently reported success**: a definition whose kind was not `scalar`, `aggregate`, `window` or `collation` registered nothing, leaked its context and still returned `SQLITE_OK`. It is now rejected with an explicit error.
- **`js_load_text()` / `js_load_blob()` on an empty file** reported "out of memory" instead of returning an empty result. A failing `fseek()` or `ftell()` is now reported as a real error, and a file too large for SQLite's maximum length is reported as too big.
- **`js_load_text()` on files with CRLF line endings** could fail on Windows, where the file size and the number of bytes actually read legitimately differ in text mode.
- **Invalid `value` or `inverse` code in a window function** was accepted when the function was created and only failed at the first query. All of a window function's code is now validated at creation time.
- **Collations returning a fractional value** such as `-0.5` were treated as "equal", so no ordering took place; only the sign of the returned value is now considered. A collation that throws or returns a non-number now consistently answers "equal" instead of "less than", which could produce wrong orderings.
- **Unhelpful `Rowset` errors**: an out-of-range column index, or use after the last row, reported "Unknown JavaScript exception" instead of describing the problem.
- **Error reporting from `js_init_table(1)`**: a detailed error was replaced by a bare error code, and attempting to update an already-registered function reported the error code as its message.

### Changed

- The extension now builds without warnings under `-Wall -Wextra`.
- The npm platform packages now ship the `LICENSE` file from the repository root, and declare `Apache-2.0` as their license. They previously referenced a `LICENSE.md` that no longer exists, which aborted the release before any package was published. The remaining references to the Elastic License 2.0 in the Node and Flutter packages were updated to match the Apache License 2.0 the project ships.

## [1.3.5] - 2026-11-01

Not released. The release pipeline failed before publishing, so this version exists
only as an Android artifact on Maven Central. Superseded by 1.3.6.

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
