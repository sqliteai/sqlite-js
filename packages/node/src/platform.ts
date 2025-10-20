import { platform, arch } from 'node:os';
import { existsSync, readFileSync } from 'node:fs';
import { execSync } from 'node:child_process';

/**
 * Supported platform identifiers
 */
export type Platform =
  | 'darwin-arm64'
  | 'darwin-x86_64'
  | 'linux-arm64'
  | 'linux-arm64-musl'
  | 'linux-x86_64'
  | 'linux-x86_64-musl'
  | 'win32-x86_64';

/**
 * Binary extension for each platform
 */
export const PLATFORM_EXTENSIONS: Record<string, string> = {
  darwin: '.dylib',
  linux: '.so',
  win32: '.dll',
} as const;

/**
 * Detects if the system uses musl libc (Alpine Linux, etc.)
 * Uses multiple detection strategies for reliability
 */
export function isMusl(): boolean {
  // Only relevant for Linux
  if (platform() !== 'linux') {
    return false;
  }

  // Strategy 1: Check for musl-specific files
  const muslFiles = [
    '/lib/ld-musl-x86_64.so.1',
    '/lib/ld-musl-aarch64.so.1',
    '/lib/ld-musl-armhf.so.1',
  ];

  for (const file of muslFiles) {
    if (existsSync(file)) {
      return true;
    }
  }

  // Strategy 2: Check ldd version output
  try {
    const lddVersion = execSync('ldd --version 2>&1', {
      encoding: 'utf-8',
      stdio: ['pipe', 'pipe', 'pipe'],
    });

    if (lddVersion.includes('musl')) {
      return true;
    }
  } catch {
    // ldd command failed, continue to next strategy
  }

  // Strategy 3: Check /etc/os-release for Alpine
  try {
    if (existsSync('/etc/os-release')) {
      const osRelease = readFileSync('/etc/os-release', 'utf-8');
      if (osRelease.includes('Alpine') || osRelease.includes('musl')) {
        return true;
      }
    }
  } catch {
    // File read failed, continue to next strategy
  }

  // Strategy 4: Check process.report.getReport() for musl
  try {
    const report = (process as any).report?.getReport?.();
    if (report?.header?.glibcVersionRuntime === '') {
      // Empty glibc version often indicates musl
      return true;
    }
  } catch {
    // Report not available
  }

  return false;
}

/**
 * Gets the current platform identifier
 * @throws {Error} If the platform is unsupported
 */
export function getCurrentPlatform(): Platform {
  const platformName = platform();
  const archName = arch();

  // macOS
  if (platformName === 'darwin') {
    if (archName === 'arm64') return 'darwin-arm64';
    if (archName === 'x64' || archName === 'ia32') return 'darwin-x86_64';
  }

  // Linux (with musl detection)
  if (platformName === 'linux') {
    const muslSuffix = isMusl() ? '-musl' : '';

    if (archName === 'arm64') {
      return `linux-arm64${muslSuffix}` as Platform;
    }
    if (archName === 'x64' || archName === 'ia32') {
      return `linux-x86_64${muslSuffix}` as Platform;
    }
  }

  // Windows
  if (platformName === 'win32') {
    if (archName === 'x64' || archName === 'ia32') return 'win32-x86_64';
  }

  // Unsupported platform
  throw new Error(
    `Unsupported platform: ${platformName}-${archName}. ` +
    `Supported platforms: darwin-arm64, darwin-x86_64, linux-arm64, linux-x86_64, win32-x86_64 ` +
    `(with glibc or musl support for Linux)`
  );
}

/**
 * Gets the package name for the current platform
 */
export function getPlatformPackageName(): string {
  const currentPlatform = getCurrentPlatform();
  return `@sqliteai/sqlite-js-${currentPlatform}`;
}

/**
 * Gets the binary filename for the current platform
 */
export function getBinaryName(): string {
  const platformName = platform();
  const extension = PLATFORM_EXTENSIONS[platformName];

  if (!extension) {
    throw new Error(`Unknown platform: ${platformName}`);
  }

  return `js${extension}`;
}
