const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const os = require('node:os');

globalThis.crypto ??= require('node:crypto').webcrypto;
globalThis.performance ??= require('node:perf_hooks').performance;

const build = path.join(__dirname, 'build');
require(path.join(build, 'site', 'wasm_exec.js'));

const selection = (machine, devices, extra = {}) => ({ machine, devices, packages: [], containers: [], ...extra });
const q800 = ['scsi53c96', 'scc'];
const falcon = ['falcon-ide', 'ikbd', 'falcon-video'];
const cases = [
  ['Repeated Mac versions', { action: 'plan', selection: selection('q800', q800, { containerInstances: [{ id: 'mac1', profile: 'macenv', version: 'macos81', media: { system: ['mac1/system/0'] } }, { id: 'mac2', profile: 'macenv', version: 'macos761', media: { system: ['mac2/system/0'] } }] }) }, true],
  ['Graphical startup and memory budget', { action: 'plan', selection: selection('q800', q800, { packages: ['x11'], desktop: 'twm', boot: { login: 'xdm', defaultSession: 'guest:six', animation: true }, containerInstances: [{ id: 'six', profile: 'macenv', version: 'system6', memoryMiB: 8 }] }) }, true],
  ['Quadra image recipe', { action: 'plan', selection: selection('q800', ['adb','framebuffer','scc','scsi53c96']) }, true],
  ['catalog', { action: 'catalog' }, true],
  ['Mac boot plan', { action: 'plan', selection: selection('q800', q800) }, true],
  ['Mac guests and packages', { action: 'plan', selection: selection('q800', [...q800, 'sonic'], { packages: ['x11', 'apkg'], containers: ['macenv', 'tosenv'] }) }, true],
  ['Falcon boot plan', { action: 'plan', selection: selection('falcon030', falcon) }, true],
  ['Missing boot storage', { action: 'plan', selection: selection('q800', ['scc']) }, false],
  ['Incompatible hardware', { action: 'plan', selection: selection('q800', [...q800, 'falcon-ide']) }, false],
  ['Unsupported Falcon guest', { action: 'plan', selection: selection('falcon030', falcon, { containers: ['macenv'] }) }, false],
  ['Planned machine', { action: 'plan', selection: selection('tt030', []) }, false],
  ['Unknown request field', { action: 'catalog', typo: true }, false],
];

async function main() {
  const go = new Go();
  const ready = new Promise((resolve) => { globalThis.auxPlannerReady = resolve; });
  const timeout = setTimeout(() => { throw new Error('WASM initialization timed out'); }, 10000);
  const { instance } = await WebAssembly.instantiate(fs.readFileSync(path.join(build, 'site', 'planner.wasm')), go.importObject);
  go.run(instance).catch((error) => { console.error(error); process.exit(1); });
  await ready;
  clearTimeout(timeout);
  assert.equal(typeof globalThis.auxPlanner, 'function');

  const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'aux-wasm-test-'));
  try {
    for (const [label, request, expectedOK] of cases) {
      const input = JSON.stringify(request);
      const inputPath = path.join(temporary, 'request.json');
      fs.writeFileSync(inputPath, input);
      const inputFD = fs.openSync(inputPath, 'r');
      let native;
      try {
        native = spawnSync(path.join(build, 'auxplan'), { stdio: [inputFD, 'pipe', 'pipe'], encoding: 'utf8', timeout: 10000 });
      } finally {
        fs.closeSync(inputFD);
      }
      assert.ifError(native.error);
      assert.equal(native.status, expectedOK ? 0 : 1, `${label}: native exit (${native.stderr})`);
      const wasm = globalThis.auxPlanner(input);
      assert.equal(wasm, native.stdout.trim(), `${label}: native/WASM JSON mismatch`);
      const response = JSON.parse(wasm);
      assert.equal(response.ok, expectedOK, `${label}: unexpected validation result`);
      if (response.manifest) assert.equal(response.manifest.imageBuildSupported, label === 'Quadra image recipe');
    }
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
  }
  assert.equal(JSON.parse(globalThis.auxPlanner()).ok, false);
  assert.equal(JSON.parse(globalThis.auxPlanner('x'.repeat(1024 * 1024 + 1))).ok, false);
  console.log(`PASS: ${cases.length} native/WASM parity cases and 2 bridge boundary checks`);
  process.exit(0);
}
main().catch((error) => { console.error(error); process.exit(1); });
