'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const { File } = require('node:buffer');
const code = fs.readFileSync(`${__dirname}/storage.js`, 'utf8');
function context(extra = {}) {
  const scope = vm.createContext({ navigator: {}, File, Uint8Array, setTimeout, crypto: require('node:crypto').webcrypto, ...extra });
  vm.runInContext(code, scope);
  return scope;
}
(async () => {
  const memory = context();
  const output = await memory.createImageOutput(16, 'ash-image-test');
  output.write(12, new Uint8Array([4, 5]));
  output.write(1, new Uint8Array([7, 8]));
  assert.throws(() => output.write(15, new Uint8Array([1, 2])), /outside/);
  const bytes = new Uint8Array(await (await output.finish()).arrayBuffer());
  assert.deepEqual(Array.from(bytes), [0,7,8,0,0,0,0,0,0,0,0,0,4,5,0,0]);
  assert.throws(() => output.write(0, new Uint8Array([1])), /closed/);
  await assert.rejects(memory.createImageOutput(257 * 1048576, 'ash-image-test'), /256 MiB/);
  await assert.rejects(memory.createImageOutput(9 * 1073741824, 'ash-image-test'), /8 GiB/);
  await assert.rejects(memory.createImageOutput(16, '../other'), /Invalid/);

  let size = 0, closed = false, removed = false, flushed = false;
  const writes = [];
  const access = {
    truncate(n) { size = n; },
    write(data, { at }) { const n = Math.min(2, data.length); writes.push([at, Array.from(data.subarray(0, n))]); return n; },
    close() { closed = true; }, flush() { flushed = true; },
  };
  const directory = {
    async getFileHandle() { return { async createSyncAccessHandle() { return access; }, async getFile() { assert(closed && flushed); return { size }; } }; },
    async removeEntry() { removed = true; },
  };
  const disk = context({ navigator: { storage: { async estimate() { return { quota: 8 * 1073741824, usage: 0 }; }, async getDirectory() { return directory; } } }, FileSystemFileHandle: { prototype: { createSyncAccessHandle() {} } } });
  const large = await disk.createImageOutput(4 * 1073741824, 'ash-image-test');
  assert.equal(large.diskBacked, true);
  large.write(3 * 1073741824, new Uint8Array([1, 2, 3, 4, 5]));
  assert.deepEqual(writes, [[3221225472,[1,2]],[3221225474,[3,4]],[3221225476,[5]]]);
  assert.equal((await large.finish()).size, 4 * 1073741824);
  assert.equal(removed, false);
  await large.remove();
  assert.equal(removed, true);
  disk.navigator.storage.estimate = async () => ({ quota: 100, usage: 90 });
  await assert.rejects(disk.createImageOutput(16, 'ash-image-test'), /insufficient/);
  console.log('Browser storage tests passed: random writes, zero fill, limits, quota, partial writes, large offsets, cleanup.');
})().catch(error => { console.error(error); process.exitCode = 1; });
