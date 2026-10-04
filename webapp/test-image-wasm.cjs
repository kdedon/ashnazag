const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
globalThis.crypto ??= require('node:crypto').webcrypto;
globalThis.performance ??= require('node:perf_hooks').performance;
require(path.join(__dirname, 'build/site/wasm_exec.js'));

async function main() {
  const go = new Go();
  const ready = new Promise(resolve => { globalThis.auxPlannerReady = resolve; });
  const timer = setTimeout(() => { throw new Error('Image WASM timed out'); }, 30000);
  const { instance } = await WebAssembly.instantiate(fs.readFileSync(path.join(__dirname, 'build/site/planner.wasm')), go.importObject);
  go.run(instance).catch(error => { throw error; });
  await ready;
  assert.equal(JSON.parse(auxAssemble()).ok, false);
  assert.equal(JSON.parse(auxAssemble(8192*1048576, 1048576, 1048576, () => {}, () => {}, () => {})).ok, false);
  const boot = Buffer.alloc(96*512);
  boot.write('ER'); boot.writeUInt16BE(512, 2); boot.writeUInt32BE(96, 4);
  function entry(index, start, count, name, type) {
    const off = index*512;
    boot.write('PM', off); boot.writeUInt32BE(3, off+4);
    boot.writeUInt32BE(start, off+8); boot.writeUInt32BE(count, off+12);
    boot.write(name, off+16); boot.write(type, off+48);
  }
  entry(1, 1, 63, 'Apple', 'Apple_partition_map');
  entry(2, 64, 16, 'Driver', 'Apple_Driver');
  entry(3, 80, 16, 'MacOS', 'Apple_HFS');
  const root = Buffer.alloc(16384);
  const sb = 8192;
  root.writeUInt32BE(0x011954, sb+1372);
  root.writeUInt32BE(16, sb+36);
  root.writeUInt32BE(4096, sb+48);
  root.writeUInt32BE(1024, sb+52);
  root.writeUInt32BE(4, sb+56);
  root.writeUInt32BE(0x7c269d38, sb+132);
  const read = data => (off, length) => new Uint8Array(data.subarray(off, off+length));
  let chunks = [];
  let result = JSON.parse(auxAssemble(boot.length, root.length, 4*1048576, read(boot), read(root), chunk => chunks.push(Buffer.from(chunk))));
  assert.equal(result.ok, true, result.error);
  const image = Buffer.concat(chunks);
  assert.equal(image.length, boot.length+root.length+4*1048576);
  assert.equal(image.readUInt32BE(512+4), 5);
  assert.deepEqual(image.subarray(boot.length,boot.length+root.length),root);
  assert.equal(image.readUInt32BE(4*512+0x88),0xabadbabe);
  assert.ok(image.subarray(boot.length+root.length).every(value=>value===0));
  result = JSON.parse(auxAssemble(boot.length, root.length, 4*1048576, () => { throw new Error('read failed'); }, read(root), () => {}));
  assert.equal(result.ok,false);
  if (process.argv[2]) {
    const original = fs.readFileSync(process.argv[2]);
    const rootStart = original.readUInt32BE(4*512+8)*512;
    const rootSize = original.readUInt32BE(4*512+12)*512;
    const swapSize = original.readUInt32BE(5*512+12)*512;
    const base = Buffer.from(original.subarray(0,rootStart));
    base.writeUInt32BE(rootStart/512,4);
    for(let i=1;i<=3;i++) base.writeUInt32BE(3,i*512+4);
    base.fill(0,4*512,6*512);
    const rootData = original.subarray(rootStart,rootStart+rootSize);
    chunks=[];
    result=JSON.parse(auxAssemble(base.length,rootSize,swapSize,read(base),read(rootData),chunk=>chunks.push(Buffer.from(chunk))));
    assert.equal(result.ok,true,result.error);
    assert.deepEqual(Buffer.concat(chunks),original,'native/WASM disk image differs');
    console.log('PASS: real native/WASM disk image byte parity');
  }
  clearTimeout(timer);
  console.log('PASS: WASM image assembly, limits, root preservation, swap and callback failure');
  process.exit(0);
}
main().catch(error=>{console.error(error);process.exit(1);});
