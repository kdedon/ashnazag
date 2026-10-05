'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');
const { spawnSync } = require('node:child_process');
const { webcrypto } = require('node:crypto');

class InputFile {
  constructor(source, start = 0, size) {
    if (Array.isArray(source)) source = Buffer.concat(source.map(part => Buffer.from(part)));
    this.source = source;
    this.start = start;
    this.size = size ?? (typeof source === 'string' ? fs.statSync(source).size : source.length);
  }
  slice(start = 0, end = this.size) {
    start = Math.max(0, Math.min(this.size, start));
    end = Math.max(start, Math.min(this.size, end));
    return new InputFile(this.source, this.start + start, end - start);
  }
  read() {
    const bytes = Buffer.alloc(this.size);
    if (typeof this.source === 'string') {
      const fd = fs.openSync(this.source, 'r');
      try { assert.equal(fs.readSync(fd, bytes, 0, bytes.length, this.start), bytes.length); }
      finally { fs.closeSync(fd); }
    } else this.source.copy(bytes, 0, this.start, this.start + this.size);
    return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
  }
  async arrayBuffer() { return this.read(); }
}

function archive(contents) {
  const chunks = [];
  let ino = 1;
  for (const [name, content] of [...Object.entries(contents), ['TRAILER!!!', Buffer.alloc(0)]]) {
    const data = Buffer.from(content), names = Buffer.from(name + '\0');
    const fields = [ino++, 0o100644, 0, 0, 1, 0, data.length, 0, 0, 0, 0, names.length, 0];
    chunks.push(Buffer.from('070701' + fields.map(n => n.toString(16).padStart(8, '0')).join('')), names,
      Buffer.alloc((4 - (110 + names.length) % 4) % 4), data, Buffer.alloc((4 - data.length % 4) % 4));
  }
  return Buffer.concat(chunks);
}

function coreArchive() {
  const swap = Buffer.alloc(0x1220);
  for (const [offset, value] of [[0x1216, 0xe581], [0x121e, 0xe581], [0x1034, 0x780b], [0x1044, 0x780b], [0x1054, 0x780b]]) swap.writeUInt16BE(value, offset);
  return archive({
    'etc/profile': 'TERM=amiga\n\tif sioc\n',
    'usr/sbin/shutdown': '#!/sbin/sh\nif /usr/amiga/bin/sioc && test x\n',
    'usr/sbin/rc0': '#!/sbin/sh\n/sbin/umountall\n', 'usr/sbin/rc6': '#!/sbin/sh\n/sbin/umountall\n',
    'etc/motd': 'Amiga Version 2.1\n', 'etc/screendefs': '# screens\namiga\n',
    'etc/group': 'root::0:\ndisplay::99:\n', 'etc/vfstab': '/dev/dsk/c0d0s1 /dev/rdsk/c0d0s1 / s5 1 yes -\n',
    'usr/sbin/swap': swap,
  });
}

async function runWorker(data, options = {}) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'ash-quadra-worker-'));
  const handles = new Set(), messages = [];
  let scope;
  const root = {
    async getFileHandle(name) {
      const filename = path.join(directory, name);
      return {
        async createSyncAccessHandle() {
          const fd = fs.openSync(filename, 'w+');
          handles.add(fd);
          return {
            truncate(size) { fs.ftruncateSync(fd, size); },
            write(bytes, { at }) { if (options.failWrite) throw new Error('Injected storage failure'); return fs.writeSync(fd, bytes, 0, bytes.length, at); },
            flush() { fs.fsyncSync(fd); },
            close() { if (handles.delete(fd)) fs.closeSync(fd); },
          };
        },
        async getFile() { return new InputFile(filename); },
      };
    },
    async removeEntry(name) { fs.unlinkSync(path.join(directory, name)); },
  };
  scope = vm.createContext({
    console, performance, crypto: webcrypto, TextEncoder, TextDecoder, Uint8Array, ArrayBuffer, DataView,
    WebAssembly, setTimeout, clearTimeout, Blob: InputFile,
    File: class extends InputFile { constructor(parts) { super(Buffer.concat(parts.map(part => Buffer.from(part)))); } },
    FileReaderSync: class { readAsArrayBuffer(file) { return file.read(); } },
    FileSystemFileHandle: { prototype: { createSyncAccessHandle() {} } },
    navigator: { storage: { async estimate() { return { quota: options.quota ?? 8 * 1073741824, usage: 0 }; }, async getDirectory() { return root; } } },
    async fetch(url) {
      const bytes = fs.readFileSync(path.join(__dirname, 'build/site', url));
      return { ok: true, async arrayBuffer() { return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength); } };
    },
    postMessage(message) { messages.push(message); },
    importScripts(...urls) { for (const url of urls) vm.runInContext(fs.readFileSync(path.join(__dirname, 'build/site', url), 'utf8'), scope, { filename: url }); },
  });
  scope.self = scope;
  try {
    vm.runInContext(fs.readFileSync(path.join(__dirname, 'quadra-worker.js'), 'utf8'), scope, { filename: 'quadra-worker.js' });
    await scope.onmessage({ data });
    assert.equal(handles.size, 0, 'worker leaked open storage handles');
    const terminal = messages.filter(message => ['complete', 'error'].includes(message.type));
    assert.equal(terminal.length, 1, JSON.stringify(messages));
    return { result: terminal[0], messages, directory, cleanup() { fs.rmSync(directory, { recursive: true, force: true }); } };
  } catch (error) {
    for (const fd of handles) fs.closeSync(fd);
    fs.rmSync(directory, { recursive: true, force: true });
    throw error;
  }
}

function kernelFixture() {
  const bytes = Buffer.alloc(512);
  Buffer.from([0x7f, 69, 76, 70, 1, 2, 1]).copy(bytes);
  bytes.writeUInt16BE(2, 16); bytes.writeUInt16BE(4, 18); bytes.writeUInt32BE(1, 20);
  bytes.writeUInt32BE(0x100000, 24); bytes.writeUInt32BE(52, 28);
  bytes.writeUInt16BE(52, 40); bytes.writeUInt16BE(32, 42); bytes.writeUInt16BE(1, 44);
  for (const [offset, value] of [[52, 1], [56, 256], [60, 0x100000], [64, 0x100000], [68, 32], [72, 512], [76, 5], [80, 4]]) bytes.writeUInt32BE(value, offset);
  bytes.fill(0x4e, 256, 288);
  return bytes;
}

function donorFixture() {
  const bytes = Buffer.alloc(128 * 512);
  bytes.write('ER'); bytes.writeUInt16BE(512, 2); bytes.writeUInt32BE(128, 4);
  bytes.writeUInt16BE(1, 16); bytes.writeUInt32BE(64, 18); bytes.writeUInt16BE(32, 22); bytes.writeUInt16BE(1, 24);
  const partitions = [['Apple', 'Apple_partition_map', 1, 63], ['Macintosh', 'Apple_Driver', 64, 32], ['MacOS', 'Apple_HFS', 96, 32]];
  for (const [index, [name, type, start, blocks]] of partitions.entries()) {
    const at = (index + 1) * 512;
    bytes.write('PM', at); bytes.writeUInt32BE(3, at + 4); bytes.writeUInt32BE(start, at + 8); bytes.writeUInt32BE(blocks, at + 12);
    bytes.write(name, at + 16); bytes.write(type, at + 48); bytes.writeUInt32BE(blocks, at + 84); bytes.writeUInt32BE(0x33, at + 88);
  }
  bytes.fill(0x42, 64 * 512, 96 * 512);
  return bytes;
}

function checkImage(file, rootMiB, swapMiB, directory, kernel, provisioned) {
  const fd = fs.openSync(file.source, 'r');
  const header = Buffer.alloc(64 * 512);
  try { fs.readSync(fd, header, 0, header.length, 0); } finally { fs.closeSync(fd); }
  assert.equal(header.toString('ascii', 0, 2), 'ER');
  assert.equal(header.readUInt32BE(4) * 512, file.size);
  const partitions = [];
  for (let index = 1; index <= header.readUInt32BE(516); index++) {
    const at = index * 512;
    assert.equal(header.toString('ascii', at, at + 2), 'PM');
    partitions.push({ type: header.toString('ascii', at + 48, at + 80).replace(/\0.*$/, ''), start: header.readUInt32BE(at + 8) * 512, size: header.readUInt32BE(at + 12) * 512 });
  }
  const hfs = partitions.find(part => part.type === 'Apple_HFS');
  const unix = partitions.filter(part => part.type === 'Apple_UNIX_SVR2');
  assert(hfs, 'missing HFS boot volume'); assert.equal(unix.length, 2);
  assert.equal(unix[0].size, rootMiB * 1048576); assert.equal(unix[1].size, swapMiB * 1048576);
  assert.equal(unix[0].start + unix[0].size, unix[1].start);
  assert.equal(unix[1].start + unix[1].size, file.size);
  const rootFile = path.join(directory, 'root-check.img');
  fs.writeFileSync(rootFile, Buffer.from(file.slice(unix[0].start, unix[0].start + unix[0].size).read()));
  const check = spawnSync('python3', [path.join(__dirname, '../kernel/mac/diskroot/ufscheck.py'), rootFile, '-l'], { encoding: 'utf8', timeout: 60000 });
  assert.equal(check.status, 0, check.stdout + check.stderr);
  assert.match(check.stdout, /\/etc\/default\/mac/);
  if (provisioned) assert.match(check.stdout, /\/amiga\/apps\/test-1\/Apps\/readme/);
  const hfsck = path.join(__dirname, '../toolchain/bin/hfsck');
  if (fs.existsSync(hfsck)) {
    const volume = path.join(directory, 'boot-check.hfs');
    fs.writeFileSync(volume, Buffer.from(file.slice(hfs.start, hfs.start + hfs.size).read()));
    const hfsCheck = spawnSync(hfsck, [volume], { encoding: 'utf8', timeout: 60000 });
    assert.equal(hfsCheck.status, 0, hfsCheck.stdout + hfsCheck.stderr);
  }
  const mkbb = path.join(__dirname, '../kernel/mac/bootblk/build/mkbb');
  if (fs.existsSync(mkbb)) {
    const kernelFile = path.join(directory, 'kernel.elf'); fs.writeFileSync(kernelFile, Buffer.from(kernel.read()));
    const bootCheck = spawnSync(mkbb, ['check', file.source, kernelFile], { encoding: 'utf8', timeout: 60000 });
    assert.equal(bootCheck.status, 0, bootCheck.stdout + bootCheck.stderr);
  }
}

// Export, import and rebuild must give the same bytes in WASM and natively.
async function recipeRoundTrip(base) {
  const selection = { preset: 'quadra800', machine: 'q800', settings: { devices: ['adb', 'framebuffer', 'scc', 'scsi53c96'] } };
  const build = async (expect) => {
    const run = await runWorker({ ...base, swapMiB: 4, selection, ...(expect ? { expect } : {}) });
    try {
      assert.equal(run.result.type, 'complete', run.result.error);
      return { recipe: run.result.recipe, mismatches: run.result.mismatches, image: fs.readFileSync(run.result.file.source) };
    } finally { run.cleanup(); }
  };
  const first = await build();
  const recipe = JSON.parse(first.recipe);
  assert.equal(recipe.formatVersion, 2);
  assert.deepEqual(recipe.preset, { id: 'quadra800', revision: 1 });
  assert.deepEqual(recipe.changes, { swapMiB: 4, provision: [{ kind: 'app', family: 'amiga', id: 'test-1' }] });
  assert.deepEqual(recipe.inputs.map(input => input.role), ['amix-tape', 'kernel', 'boot-donor', 'package:test-1']);
  assert.deepEqual(recipe.inputs[0].parts.map(part => part.size), base.archives.map(source => source.file.size));
  delete recipe.inputs[0].parts;
  assert(first.image.includes(Buffer.from(JSON.stringify(recipe, null, 2) + '\n')), 'image does not carry its recipe without tape parts');
  const second = await build(first.recipe);
  assert.equal(second.recipe, first.recipe);
  assert.equal(second.mismatches.length, 0);
  assert(second.image.equals(first.image), 'WASM rebuild is not byte-identical');
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'ash-recipe-'));
  try {
    const file = (name, input) => { const target = path.join(directory, name); fs.writeFileSync(target, Buffer.from(input.read())); return target; };
    fs.mkdirSync(path.join(directory, 'tapes'));
    for (const source of base.archives) file(path.join('tapes', source.name), source.file);
    const output = path.join(directory, 'native.img');
    const native = spawnSync(path.join(__dirname, 'build/ashforge'), ['build', '-recipe', file('recipe.json', new InputFile(Buffer.from(first.recipe))), '-tapes', path.join(directory, 'tapes'),
      '-kernel', file('kernel', base.kernel), '-donor', file('donor', base.donor), '-package', `app:amiga:test-1=${file('package', base.provision[0].file)}`, '-output', output], { encoding: 'utf8', timeout: 120000 });
    assert.equal(native.status, 0, native.stderr);
    assert(fs.readFileSync(output).equals(first.image), 'native rebuild differs from WASM');
  } finally { fs.rmSync(directory, { recursive: true, force: true }); }
}

async function main() {
  const [tapeDir, kernelPath, donorPath, outputPath] = process.argv.slice(2);
  assert(!tapeDir || (kernelPath && donorPath), 'Usage: node test-quadra-worker.cjs [tape-part,... kernel.elf donor.img [output.img]]');
  const kernel = new InputFile(kernelPath || kernelFixture());
  const donor = new InputFile(donorPath || donorFixture());
  const sources = tapeDir ? { tape: tapeDir.split(',').map(name => Object.assign(new InputFile(name), { name: path.basename(name) })) }
    : { archives: ['02', '03', '10'].map((name, index) => ({ name, file: new InputFile(index === 0 ? coreArchive() : archive({})) })) };
  const request = { type: 'quadra', ...sources, kernel, donor, rootMiB: 64, swapMiB: 256, temporaryNames: ['ash-image-root-test', 'ash-image-final-test'] };
  if (!tapeDir) {
    const data = fs.readFileSync(path.join(__dirname, 'svr4/testdata/ASHtest.pkg'));
    request.provision = [{kind:'app', family:'amiga', id:'test-1', sha256:require('node:crypto').createHash('sha256').update(data).digest('hex'), file:new InputFile(data)}];
  }
  const success = await runWorker(request);
  try {
    assert.equal(success.result.type, 'complete', success.result.error);
    assert(success.result.file.size > 256 * 1048576, 'test must exercise disk storage above memory fallback limit');
    assert.equal(success.result.bytes, success.result.file.size);
    assert(!fs.existsSync(path.join(success.directory, request.temporaryNames[0])), 'temporary root was not removed');
    assert(success.messages.some(message => message.type === 'progress'), 'no build progress');
    checkImage(success.result.file, request.rootMiB, request.swapMiB, success.directory, kernel, !tapeDir);
    if (outputPath) fs.copyFileSync(success.result.file.source, outputPath, fs.constants.COPYFILE_EXCL);
  } finally { success.cleanup(); }
  if (!tapeDir) await recipeRoundTrip(request);
  for (const [label, changes, options, expected] of [
    ['invalid package', { provision: [{kind:'app'}] }, {}, /provisioning/i],
    ['incomplete tape', { archives: undefined, tape: [Object.assign(new InputFile(Buffer.alloc(8192)), { name: 'tape.img' })] }, {}, /segments 02, 03, 10 not found/],
    ['invalid kernel', { kernel: new InputFile(Buffer.alloc(52)) }, {}, /kernel|ELF|executable/i],
    ['storage quota', {}, { quota: 1 }, /space|quota/i],
    ['write failure', {}, { failWrite: true }, /Injected storage failure/],
  ]) {
    const failure = await runWorker({ ...request, ...changes }, options);
    try {
      assert.equal(failure.result.type, 'error', label);
      assert.match(failure.result.error, expected, label);
      assert.deepEqual(fs.readdirSync(failure.directory), [], `${label}: leftover temporary files`);
    } finally { failure.cleanup(); }
  }
  console.log(`PASS: ${tapeDir ? 'real-media' : 'synthetic'} Quadra worker builds >256 MiB disk; UFS, partition map, progress, storage errors, and cleanup verified`);
}
const timer = setTimeout(() => { console.error('Quadra worker test timed out'); process.exit(1); }, 180000);
main().then(() => { clearTimeout(timer); process.exit(0); }, error => { console.error(error); process.exit(1); });
