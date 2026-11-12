#!/usr/bin/env node
/**
 * Smoke test for the built package (dist/).
 *
 * The unit tests run against the TypeScript sources, so they cannot catch
 * problems that only appear in the bundled output, such as a CommonJS-only
 * construct leaking into the ESM build. This script installs the built package
 * and a stub platform package into a temporary project, then loads it through
 * both `import` and `require`, exactly as a consumer would, both directly and
 * after bundling the consumer with esbuild (which compiles `import` consumers
 * to CommonJS, a setup where `import.meta.url` is not available).
 *
 * Run after `npm run build`:  npm run test:dist
 */
import { spawnSync } from 'node:child_process';
import { cpSync, mkdirSync, mkdtempSync, readFileSync, realpathSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { build } from 'esbuild';

const packageDir = join(dirname(fileURLToPath(import.meta.url)), '..');
const distDir = join(packageDir, 'dist');
const manifest = JSON.parse(readFileSync(join(packageDir, 'package.json'), 'utf8'));

const { getPlatformPackageName, getBinaryName } = await import(
  pathToFileURL(join(distDir, 'index.mjs')).href
);
const platformPackageName = getPlatformPackageName();

// realpath: on macOS the temp dir is behind a symlink (/var -> /private/var)
const projectDir = realpathSync(mkdtempSync(join(tmpdir(), 'sqlite-js-smoke-')));
const nodeModules = join(projectDir, 'node_modules');
const platformDir = join(nodeModules, ...platformPackageName.split('/'));

let failures = 0;

function installMainPackage() {
  const dir = join(nodeModules, ...manifest.name.split('/'));
  mkdirSync(dir, { recursive: true });
  cpSync(distDir, join(dir, 'dist'), { recursive: true });
  writeFileSync(join(dir, 'package.json'), JSON.stringify(manifest, null, 2));
}

/** Installs a stub platform package whose entry point is `source`. */
function installPlatformPackage(source) {
  rmSync(platformDir, { recursive: true, force: true });
  mkdirSync(platformDir, { recursive: true });
  writeFileSync(join(platformDir, getBinaryName()), '');
  writeFileSync(
    join(platformDir, 'package.json'),
    JSON.stringify({ name: platformPackageName, version: '0.0.0', main: 'index.js' })
  );
  writeFileSync(join(platformDir, 'index.js'), source);
}

function uninstallPlatformPackage() {
  rmSync(platformDir, { recursive: true, force: true });
}

// Prints the outcome of getExtensionPath() as JSON.
const probeBody = `
  try {
    console.log(JSON.stringify({ path: getExtensionPath() }));
  } catch (e) {
    console.log(JSON.stringify({ name: e.name, message: e.message, cause: e.cause?.message }));
  }
`;

// Each way a consumer can load the package: [label, script run with node].
const consumers = [
  ['import', 'probe-import.mjs'],
  ['require', 'probe-require.cjs'],
  ['import, esbuild bundle to cjs', join('bundles', 'import-cjs.cjs')],
  ['import, esbuild bundle to esm', join('bundles', 'import-esm.mjs')],
  ['require, esbuild bundle to cjs', join('bundles', 'require-cjs.cjs')],
];

async function writeConsumers() {
  writeFileSync(
    join(projectDir, 'probe-import.mjs'),
    `import { getExtensionPath } from ${JSON.stringify(manifest.name)};\n${probeBody}`
  );
  writeFileSync(
    join(projectDir, 'probe-require.cjs'),
    `const { getExtensionPath } = require(${JSON.stringify(manifest.name)});\n${probeBody}`
  );

  const bundles = [
    ['probe-import.mjs', 'cjs', 'import-cjs.cjs'],
    ['probe-import.mjs', 'esm', 'import-esm.mjs'],
    ['probe-require.cjs', 'cjs', 'require-cjs.cjs'],
  ];
  for (const [entry, format, outfile] of bundles) {
    await build({
      absWorkingDir: projectDir,
      entryPoints: [entry],
      outfile: join('bundles', outfile),
      bundle: true,
      platform: 'node',
      format,
      logLevel: 'error',
    });
  }
}

function run(script) {
  const result = spawnSync(process.execPath, [script], { cwd: projectDir, encoding: 'utf8' });
  if (result.status !== 0) {
    throw new Error(`node exited with ${result.status}:\n${result.stderr}`);
  }
  return JSON.parse(result.stdout.trim());
}

function check(description, assertion) {
  for (const [label, script] of consumers) {
    const name = `${description} (${label})`;
    try {
      assertion(run(script));
      console.log(`  ✓ ${name}`);
    } catch (error) {
      failures++;
      console.error(`  ✗ ${name}\n    ${error.message.replace(/\n/g, '\n    ')}`);
    }
  }
}

function expect(condition, message) {
  if (!condition) throw new Error(message);
}

try {
  installMainPackage();
  await writeConsumers();
  console.log(`Smoke testing ${manifest.name} dist/ with ${platformPackageName}`);

  installPlatformPackage(
    `module.exports = { path: require('path').join(__dirname, ${JSON.stringify(getBinaryName())}) };`
  );
  check('resolves the binary of an installed platform package', (r) => {
    expect(r.path === join(platformDir, getBinaryName()), `unexpected result: ${JSON.stringify(r)}`);
  });

  uninstallPlatformPackage();
  check('reports a missing platform package as not installed', (r) => {
    expect(r.name === 'ExtensionNotFoundError', `unexpected result: ${JSON.stringify(r)}`);
    expect(r.message.includes('is not installed'), `unexpected message: ${r.message}`);
  });

  installPlatformPackage(`throw new Error('platform package failed to load');`);
  check('surfaces the error of a platform package that fails to load', (r) => {
    expect(r.name === 'ExtensionNotFoundError', `unexpected result: ${JSON.stringify(r)}`);
    expect(r.message.includes('platform package failed to load'), `message hides the cause: ${r.message}`);
    expect(r.cause === 'platform package failed to load', `cause not set: ${JSON.stringify(r)}`);
  });

  installPlatformPackage(`module.exports = { path: '/nonexistent/js.bin' };`);
  check('reports a platform package whose binary is missing', (r) => {
    expect(r.name === 'ExtensionNotFoundError', `unexpected result: ${JSON.stringify(r)}`);
    expect(r.message.includes('/nonexistent/js.bin'), `unexpected message: ${r.message}`);
  });
} finally {
  rmSync(projectDir, { recursive: true, force: true });
}

if (failures > 0) {
  console.error(`\n${failures} smoke test(s) failed`);
  process.exit(1);
}
console.log('\nAll smoke tests passed');
