# @sqliteai/sqlite-js

[![npm version](https://badge.fury.io/js/@sqliteai%2Fsqlite-js.svg)](https://www.npmjs.com/package/@sqliteai/sqlite-js)
[![License](https://img.shields.io/badge/license-Elastic%202.0-blue.svg)](LICENSE.md)

> SQLite JS extension packaged for Node.js

**SQLite JS** is a powerful extension that brings JavaScript capabilities to SQLite. With this extension, you can create custom SQLite functions, aggregates, window functions, and collation sequences using JavaScript code, allowing for flexible and powerful data manipulation directly within your SQLite database.

## Features

- ✅ **JavaScript UDFs** - Custom SQLite functions in JavaScript
- ✅ **QuickJS Integration** - Fast JavaScript execution inside SQLite
- ✅ **Cross-platform** - Works on macOS, Linux (glibc/musl), and Windows
- ✅ **Zero configuration** - Automatically detects and loads the correct binary for your platform
- ✅ **TypeScript native** - Full type definitions included
- ✅ **Modern ESM + CJS** - Works with both ES modules and CommonJS

## Installation

```bash
npm install @sqliteai/sqlite-js
```

The package automatically downloads the correct native extension for your platform during installation.

### Supported Platforms

| Platform | Architecture | Package |
|----------|-------------|---------|
| macOS | ARM64 (Apple Silicon) | `@sqliteai/sqlite-js-darwin-arm64` |
| macOS | x86_64 (Intel) | `@sqliteai/sqlite-js-darwin-x86_64` |
| Linux | ARM64 (glibc) | `@sqliteai/sqlite-js-linux-arm64` |
| Linux | ARM64 (musl/Alpine) | `@sqliteai/sqlite-js-linux-arm64-musl` |
| Linux | x86_64 (glibc) | `@sqliteai/sqlite-js-linux-x86_64` |
| Linux | x86_64 (musl/Alpine) | `@sqliteai/sqlite-js-linux-x86_64-musl` |
| Windows | x86_64 | `@sqliteai/sqlite-js-win32-x86_64` |

## sqlite-js API

For detailed information on how to use the JS extension features, see the [main documentation](https://github.com/sqliteai/sqlite-js/blob/main/README.md).

## Usage

```typescript
import { getExtensionPath } from '@sqliteai/sqlite-js';
import Database from 'better-sqlite3';

const db = new Database(':memory:');
db.loadExtension(getExtensionPath());

// Ready to use
const version = db.prepare('SELECT js_version()').pluck().get();
console.log('JS extension version:', version);
```

## Examples

For complete, runnable examples, see the [sqlite-extensions-guide](https://github.com/sqliteai/sqlite-extensions-guide/tree/main/examples/node).

These examples are generic and work with all SQLite extensions: `sqlite-vector`, `sqlite-sync`, `sqlite-js`, and `sqlite-ai`.

## API Reference

### `getExtensionPath(): string`

Returns the absolute path to the SQLite JS extension binary for the current platform.

**Returns:** `string` - Absolute path to the extension file (`.so`, `.dylib`, or `.dll`)

**Throws:** `ExtensionNotFoundError` - If the extension binary cannot be found for the current platform

**Example:**
```typescript
import { getExtensionPath } from '@sqliteai/sqlite-js';

const path = getExtensionPath();
// => '/path/to/node_modules/@sqliteai/sqlite-js-darwin-arm64/js.dylib'
```

---

### `getExtensionInfo(): ExtensionInfo`

Returns detailed information about the extension for the current platform.

**Returns:** `ExtensionInfo` object with the following properties:
- `platform: Platform` - Current platform identifier (e.g., `'darwin-arm64'`)
- `packageName: string` - Name of the platform-specific npm package
- `binaryName: string` - Filename of the binary (e.g., `'js.dylib'`)
- `path: string` - Full path to the extension binary

**Throws:** `ExtensionNotFoundError` - If the extension binary cannot be found

**Example:**
```typescript
import { getExtensionInfo } from '@sqliteai/sqlite-js';

const info = getExtensionInfo();
console.log(`Running on ${info.platform}`);
console.log(`Extension path: ${info.path}`);
```

---

### `getCurrentPlatform(): Platform`

Returns the current platform identifier.

**Returns:** `Platform` - One of:
- `'darwin-arm64'` - macOS ARM64
- `'darwin-x86_64'` - macOS x86_64
- `'linux-arm64'` - Linux ARM64 (glibc)
- `'linux-arm64-musl'` - Linux ARM64 (musl)
- `'linux-x86_64'` - Linux x86_64 (glibc)
- `'linux-x86_64-musl'` - Linux x86_64 (musl)
- `'win32-x86_64'` - Windows x86_64

**Throws:** `Error` - If the platform is unsupported

---

### `isMusl(): boolean`

Detects if the system uses musl libc (Alpine Linux, etc.).

**Returns:** `boolean` - `true` if musl is detected, `false` otherwise

---

### `class ExtensionNotFoundError extends Error`

Error thrown when the SQLite JS extension cannot be found for the current platform.

## Related Projects

- **[@sqliteai/sqlite-vector](https://www.npmjs.com/package/@sqliteai/sqlite-vector)** - Vector search and similarity matching
- **[@sqliteai/sqlite-ai](https://www.npmjs.com/package/@sqliteai/sqlite-ai)** - On-device AI inference and embedding generation
- **[@sqliteai/sqlite-sync](https://www.npmjs.com/package/@sqliteai/sqlite-sync)** - Sync on-device databases with the cloud

## License

This project is licensed under the [Elastic License 2.0](LICENSE.md).

For production or managed service use, please [contact SQLite Cloud, Inc](mailto:info@sqlitecloud.io) for a commercial license.

## Contributing

Contributions are welcome! Please see the [main repository](https://github.com/sqliteai/sqlite-js) to open an issue.

## Support

- 📖 [Documentation](https://github.com/sqliteai/sqlite-js/blob/main/README.md)
- 🐛 [Report Issues](https://github.com/sqliteai/sqlite-js/issues)
