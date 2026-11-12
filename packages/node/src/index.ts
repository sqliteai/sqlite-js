import { resolve } from 'node:path';
import { existsSync } from 'node:fs';
import { createRequire } from 'node:module';
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
  /** The underlying error that prevented the extension from loading, if any */
  readonly cause?: unknown;

  constructor(message: string, options?: { cause?: unknown }) {
    super(message);
    this.name = 'ExtensionNotFoundError';
    if (options && 'cause' in options) {
      this.cause = options.cause;
    }
  }
}

type PlatformPackageResult =
  | { status: 'found'; path: string }
  | { status: 'not-installed' }
  | { status: 'invalid'; reason: string }
  | { status: 'error'; error: unknown };

/**
 * Returns true when `error` means that `packageName` itself could not be
 * resolved, as opposed to the package being present but failing to load.
 */
function isPackageNotFound(error: unknown, packageName: string): boolean {
  const code = (error as { code?: unknown } | null)?.code;
  const message = (error as { message?: unknown } | null)?.message;
  return (
    code === 'MODULE_NOT_FOUND' &&
    typeof message === 'string' &&
    message.includes(`'${packageName}'`)
  );
}

/**
 * Attempts to load the platform-specific package
 */
function tryLoadPlatformPackage(packageName: string): PlatformPackageResult {
  let platformPackage: { path?: unknown } | undefined;

  try {
    // A bare `require` is not available in the ESM build, so create one that
    // resolves relative to this module. tsup shims `import.meta.url` in the
    // CommonJS build, so the same code works in both formats.
    const localRequire = createRequire(import.meta.url);
    platformPackage = localRequire(packageName);
  } catch (error) {
    if (isPackageNotFound(error, packageName)) {
      // Expected when the optional dependency was not installed
      return { status: 'not-installed' };
    }
    return { status: 'error', error };
  }

  if (typeof platformPackage?.path !== 'string') {
    return {
      status: 'invalid',
      reason: `"${packageName}" does not export a binary path`,
    };
  }

  if (!existsSync(platformPackage.path)) {
    return {
      status: 'invalid',
      reason: `the binary "${platformPackage.path}" exported by "${packageName}" does not exist`,
    };
  }

  return { status: 'found', path: platformPackage.path };
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
  const currentPlatform = getCurrentPlatform();
  const packageName = getPlatformPackageName();

  // Try to load from platform-specific package
  const result = tryLoadPlatformPackage(packageName);

  switch (result.status) {
    case 'found':
      return resolve(result.path);

    case 'error': {
      const detail =
        result.error instanceof Error ? result.error.message : String(result.error);
      throw new ExtensionNotFoundError(
        `SQLite JS extension not found for platform: ${currentPlatform}\n\n` +
        `The platform-specific package "${packageName}" could not be loaded:\n` +
        `  ${detail}`,
        { cause: result.error }
      );
    }

    case 'invalid':
      throw new ExtensionNotFoundError(
        `SQLite JS extension not found for platform: ${currentPlatform}\n\n` +
        `The platform-specific package "${packageName}" is installed, but ${result.reason}.\n\n` +
        `Try reinstalling it: npm install --force`
      );

    case 'not-installed':
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
