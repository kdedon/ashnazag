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
  const presets = JSON.parse(globalThis.auxForge(JSON.stringify({ action: 'presets' })));
  const nativePresets = spawnSync(path.join(build, 'ashforge'), ['presets'], { encoding: 'utf8', timeout: 10000 });
  assert.equal(nativePresets.status, 0, nativePresets.stderr);
  assert.deepEqual(presets.presets, JSON.parse(nativePresets.stdout));
  assert.deepEqual(presets.presets.filter((preset) => preset.status === 'available').map((preset) => preset.id), ['quadra800']);
  for (const [recipe, expected] of [['{', /not a forge recipe/], [JSON.stringify({ formatVersion: 3 }), /newer than this forge/], [JSON.stringify({ formatVersion: 1 }), /invalid recipe/]]) {
    const result = JSON.parse(globalThis.auxForge(JSON.stringify({ action: 'import', recipe })));
    assert.equal(result.ok, false);
    assert.match(result.error, expected);
  }
  // Format 1 recipes listed segments 02, 03 and 10; import turns them into the tape input.
  const hash = c => c.repeat(64);
  const old = { formatVersion: 1, forgeVersion: '0.1.0', preset: { id: 'quadra800', revision: 1 }, changes: {},
    lock: { formatVersion: 1, layoutPolicyVersion: 1, bindings: [], artifacts: [], warnings: [] },
    inputs: [['amix-02', 'a'], ['amix-03', 'b'], ['amix-10', 'c'], ['kernel', 'd'], ['boot-donor', 'e']].map(([role, c], i) => ({ role, size: 100 + i, sha256: hash(c) })) };
  const upgraded = JSON.parse(globalThis.auxForge(JSON.stringify({ action: 'import', recipe: JSON.stringify(old) })));
  assert.equal(upgraded.ok, true, upgraded.error);
  assert.equal(upgraded.recipe.formatVersion, 2);
  assert.deepEqual(upgraded.recipe.inputs.map(input => [input.role, input.size]), [['amix-tape', 303], ['kernel', 103], ['boot-donor', 104]]);
  // The tape splitter names what it cannot find.
  const tar = (name, data) => {
    const header = Buffer.alloc(512);
    header.write(name, 0); header.write('0000644\0', 100); header.write('0000000\0', 108); header.write('0000000\0', 116);
    header.write(data.length.toString(8).padStart(11, '0') + '\0', 124); header.write('00000000000\0', 136);
    header.write('        ', 148); header.write('0', 156); header.write('ustar\0' + '00', 257);
    let sum = 0; for (const byte of header) sum += byte;
    header.write(sum.toString(8).padStart(6, '0') + '\0 ', 148);
    return Buffer.concat([header, data, Buffer.alloc((512 - data.length % 512) % 512), Buffer.alloc(1024)]);
  };
  const split = parts => globalThis.auxAmixTape(JSON.stringify({ names: parts.map(p => p[0]), sizes: parts.map(p => p[1].length) }),
    parts.map(p => (offset, length) => new Uint8Array(p[1].subarray(offset, offset + length))));
  for (const [parts, expected] of [
    [[['t.tar', tar('Tape/02', Buffer.alloc(1000, 1))]], /segment 02 is damaged \(t\.tar:Tape\/02.*segments 03, 10 not found/],
    [[['a.tar.bz2', Buffer.from('BZh91AY&SY' + 'x'.repeat(50))]], /tape part 1 \(a\.tar\.bz2\)/],
    [[['empty', Buffer.alloc(0)]], /tape part 1 is empty/],
  ]) {
    const result = split(parts);
    assert.equal(result.ok, false);
    assert.match(result.error, expected);
  }
  console.log(`PASS: ${cases.length} native/WASM parity cases, 2 bridge boundary checks, presets, recipe import and tape checks`);
  process.exit(0);
}
main().catch((error) => { console.error(error); process.exit(1); });
