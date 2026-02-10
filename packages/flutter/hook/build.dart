// Copyright (c) 2025 SQLite Cloud, Inc.
// Licensed under the Elastic License 2.0 (see LICENSE.md).

import 'dart:io';

import 'package:code_assets/code_assets.dart';
import 'package:hooks/hooks.dart';
import 'package:path/path.dart' as p;

void main(List<String> args) async {
  await build(args, (input, output) async {
    if (!input.config.buildCodeAssets) return;

    final codeConfig = input.config.code;
    final os = codeConfig.targetOS;
    final arch = codeConfig.targetArchitecture;

    final binaryPath = _resolveBinaryPath(os, arch, codeConfig);
    if (binaryPath == null) {
      throw UnsupportedError(
        'sqlite_js does not support $os $arch.',
      );
    }

    final nativeLibDir = p.join(
      input.packageRoot.toFilePath(),
      'native_libraries',
    );
    final file = File(p.join(nativeLibDir, binaryPath));
    if (!file.existsSync()) {
      throw StateError(
        'Pre-built binary not found: ${file.path}. '
        'Run the CI pipeline to populate native_libraries/.',
      );
    }

    output.assets.code.add(
      CodeAsset(
        package: input.packageName,
        name: 'src/native/sqlite_js_extension.dart',
        linkMode: DynamicLoadingBundled(),
        file: file.uri,
      ),
    );
  });
}

String? _resolveBinaryPath(OS os, Architecture arch, CodeConfig config) {
  if (os == OS.android) {
    return switch (arch) {
      Architecture.arm64 => 'android/js_android_arm64.so',
      Architecture.arm => 'android/js_android_arm.so',
      Architecture.x64 => 'android/js_android_x64.so',
      _ => null,
    };
  }

  if (os == OS.iOS) {
    final sdk = config.iOS.targetSdk;
    if (sdk == IOSSdk.iPhoneOS) {
      return 'ios/js_ios_arm64.dylib';
    }
    // Simulator: fat binary (arm64 + x64)
    return 'ios-sim/js_ios-sim.dylib';
  }

  if (os == OS.macOS) {
    return switch (arch) {
      Architecture.arm64 => 'mac/js_mac_arm64.dylib',
      Architecture.x64 => 'mac/js_mac_x64.dylib',
      _ => null,
    };
  }

  if (os == OS.linux) {
    return switch (arch) {
      Architecture.x64 => 'linux/js_linux_x64.so',
      Architecture.arm64 => 'linux/js_linux_arm64.so',
      _ => null,
    };
  }

  if (os == OS.windows) {
    return switch (arch) {
      Architecture.x64 => 'windows/js_windows_x64.dll',
      _ => null,
    };
  }

  return null;
}
