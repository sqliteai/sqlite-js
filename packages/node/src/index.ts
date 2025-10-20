import { resolve } from 'node:path';
import { existsSync } from 'node:fs';
import {
  getCurrentPlatform,
  getPlatformPackageName,
  getBinaryName,
  type Platform
} from './platform.js';

/**
 * Error thrown when the SQLite JS extension cannot be found
 */
export class ExtensionNotFoundError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'ExtensionNotFoundError';
  }
}

/**
 * Attempts to load the platform-specific package
 * @returns The path to the extension binary, or null if not found
 */
function tryLoadPlatformPackage(): string | null {
  try {
    const packageName = getPlatformPackageName();

    // Try to dynamically import the platform package
    // This works in both CommonJS and ESM
    const platformPackage = require(packageName);

    if (platformPackage?.path && typeof platformPackage.path === 'string') {
      if (existsSync(platformPackage.path)) {
        return platformPackage.path;
      }
    }
  } catch (error) {
    // Platform package not installed or failed to load
    // This is expected when optionalDependencies fail
  }

  return null;
}

/**
 * Gets the absolute path to the SQLite JS extension binary for the current platform
 *
 * @returns Absolute path to the extension binary (.so, .dylib, or .dll)
 * @throws {ExtensionNotFoundError} If the extension binary cannot be found
 *
 * @example
 * ```typescript
 * import { getExtensionPath } from '@sqliteai/sqlite-js';
 *
 * const extensionPath = getExtensionPath();
 * // On macOS ARM64: /path/to/node_modules/@sqliteai/sqlite-js-darwin-arm64/js.dylib
 * ```
 */
export function getExtensionPath(): string {
  // Try to load from platform-specific package
  const platformPath = tryLoadPlatformPackage();
  if (platformPath) {
    return resolve(platformPath);
  }

  // If we reach here, the platform package wasn't installed
  const currentPlatform = getCurrentPlatform();
  const packageName = getPlatformPackageName();

  throw new ExtensionNotFoundError(
    `SQLite JS extension not found for platform: ${currentPlatform}\n\n` +
    `The platform-specific package "${packageName}" is not installed.\n` +
    `This usually happens when:\n` +
    `  1. Your platform is not supported\n` +
    `  2. npm failed to install optional dependencies\n` +
    `  3. You're installing with --no-optional flag\n\n` +
    `Try running: npm install --force`
  );
}

/**
 * Information about the current platform and extension
 */
export interface ExtensionInfo {
  /** Current platform identifier (e.g., 'darwin-arm64') */
  platform: Platform;
  /** Name of the platform-specific npm package */
  packageName: string;
  /** Filename of the binary (e.g., 'js.dylib') */
  binaryName: string;
  /** Full path to the extension binary */
  path: string;
}

/**
 * Gets detailed information about the SQLite JS extension
 *
 * @returns Extension information object
 *
 * @example
 * ```typescript
 * import { getExtensionInfo } from '@sqliteai/sqlite-js';
 *
 * const info = getExtensionInfo();
 * console.log(info);
 * // {
 * //   platform: 'darwin-arm64',
 * //   packageName: '@sqliteai/sqlite-js-darwin-arm64',
 * //   binaryName: 'js.dylib',
 * //   path: '/path/to/js.dylib'
 * // }
 * ```
 */
export function getExtensionInfo(): ExtensionInfo {
  return {
    platform: getCurrentPlatform(),
    packageName: getPlatformPackageName(),
    binaryName: getBinaryName(),
    path: getExtensionPath(),
  };
}

// Default export for CommonJS compatibility
export default {
  getExtensionPath,
  getExtensionInfo,
  ExtensionNotFoundError,
};

// Re-export platform utilities
export { getCurrentPlatform, getPlatformPackageName, getBinaryName, isMusl } from './platform.js';
export type { Platform } from './platform.js';
