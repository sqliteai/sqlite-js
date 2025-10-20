import { describe, it, expect } from 'vitest';
import {
  getCurrentPlatform,
  getPlatformPackageName,
  getBinaryName,
  isMusl,
  getExtensionPath,
  getExtensionInfo,
  ExtensionNotFoundError
} from './index';

describe('Platform Detection', () => {
  it('getCurrentPlatform() returns a valid platform', () => {
    const platform = getCurrentPlatform();
    const validPlatforms = [
      'darwin-arm64',
      'darwin-x86_64',
      'linux-arm64',
      'linux-arm64-musl',
      'linux-x86_64',
      'linux-x86_64-musl',
      'win32-x86_64',
    ];

    expect(validPlatforms).toContain(platform);
  });

  it('getPlatformPackageName() returns correct package name format', () => {
    const packageName = getPlatformPackageName();

    expect(packageName.startsWith('@sqliteai/sqlite-js-')).toBe(true);

    expect(packageName).toMatch(
      /^@sqliteai\/sqlite-js-(darwin|linux|win32)-(arm64|x86_64)(-musl)?$/
    );
  });

  it('getBinaryName() returns correct extension', () => {
    const binaryName = getBinaryName();

    expect(binaryName).toMatch(
      /^js\.(dylib|so|dll)$/
    );
  });

  it('isMusl() returns a boolean', () => {
    expect(typeof isMusl()).toBe('boolean');
  });
});

describe('Extension Path Resolution', () => {
  it('getExtensionPath() returns a string or throws', () => {
    try {
      const path = getExtensionPath();
      expect(typeof path).toBe('string');
      expect(path.length).toBeGreaterThan(0);
    } catch (error) {
      expect(error instanceof ExtensionNotFoundError).toBe(true);
    }
  });

  it('getExtensionInfo() returns complete info object', () => {
    try {
      const info = getExtensionInfo();

      expect(info.platform).toBeTruthy();
      expect(info.packageName).toBeTruthy();
      expect(info.binaryName).toBeTruthy();
      expect(info.path).toBeTruthy();

      expect(typeof info.platform).toBe('string');
      expect(typeof info.packageName).toBe('string');
      expect(typeof info.binaryName).toBe('string');
      expect(typeof info.path).toBe('string');
    } catch (error) {
      expect(error instanceof ExtensionNotFoundError).toBe(true);
    }
  });
});

describe('Error Handling', () => {
  it('ExtensionNotFoundError has correct properties', () => {
    const error = new ExtensionNotFoundError('Test message');

    expect(error instanceof Error).toBe(true);
    expect(error.name).toBe('ExtensionNotFoundError');
    expect(error.message).toBe('Test message');
  });
});
