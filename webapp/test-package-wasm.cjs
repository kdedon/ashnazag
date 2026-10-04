const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { createHash, webcrypto } = require('node:crypto');
globalThis.crypto ??= webcrypto;
globalThis.performance ??= require('node:perf_hooks').performance;
require(path.join(__dirname, 'build/site/wasm_exec.js'));
function archive() {
  const chunks = [];
  function entry(name, mode, body, ino) {
    body = Buffer.from(body);
    const names = Buffer.from(name + '\0');
    const fields = [ino,mode,0,0,1,0,body.length,0,0,0,0,names.length,0];
    chunks.push(Buffer.from('070701' + fields.map(n => n.toString(16).padStart(8, '0')).join('')), names,
      Buffer.alloc((4-(110+names.length)%4)%4), body, Buffer.alloc((4-body.length%4)%4));
  }
  entry('readme',0o100644,'source archive\n',1);
  entry('TRAILER!!!',0,'',0);
  return Buffer.concat(chunks);
}
async function main() {
  const go = new Go();
  const ready = new Promise(resolve => { globalThis.auxPlannerReady = resolve; });
  const timer = setTimeout(() => { throw new Error('WASM initialization timed out'); }, 30000);
  const { instance } = await WebAssembly.instantiate(fs.readFileSync(path.join(__dirname, 'build/site/planner.wasm')), go.importObject);
  go.run(instance).catch(error => { throw error; });
  await ready;
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ashpkg-wasm-'));
  const native = (...args) => {
    const run = spawnSync(path.join(__dirname, 'build/ashpkg'), args, { encoding: 'utf8', timeout: 60000 });
    assert.equal(run.status, 0, run.stderr);
    return run.stdout;
  };
  try {
    const resolve = JSON.parse(fs.readFileSync(path.join(__dirname, 'cmd/ashpkg/testdata/resolve.json')));
    const media = archive(), hash = createHash('sha256').update(media).digest('hex');
    resolve.catalog[0].sources.push({ id:'media', format:'cpio', size:media.length, sha256:hash, location:'user', provenance:'fixture', redistribution:'user-supplied' });
    resolve.catalog[0].operations.push({ type:'extract', source:'media', path:'Assets' });
    fs.writeFileSync(path.join(dir, 'resolve.json'), JSON.stringify(resolve));
    fs.writeFileSync(path.join(dir, 'media.cpio'), media);
    native('resolve','-request',path.join(dir,'resolve.json'),'-output',path.join(dir,'lock.json'));
    const lock = JSON.parse(fs.readFileSync(path.join(dir,'lock.json')));
    const wasmLock = JSON.parse(auxPackages(JSON.stringify({action:'resolve',...resolve})));
    assert.equal(wasmLock.ok,true,wasmLock.error);
    assert.deepEqual(wasmLock.lock,lock);
    const info = { package:'ASHtest', name:'Test application', version:'1', architecture:'m68k', baseDir:'/amiga/apps/test', timestamp:42 };
    const select = { environmentID:'first', packageID:'amiga.fixture', tier:'environment', info };
    fs.writeFileSync(path.join(dir,'build.json'), JSON.stringify({ ...select, lock:'lock.json', inputs:{[hash]:'media.cpio'}, output:'native.pkg', receipt:'receipt.json' }));
    native('build','-request',path.join(dir,'build.json'));
    let output, maxRead=0;
    const build = { action:'build', lock, ...select, inputs:[{sha256:hash,size:media.length}] };
    const reader = (offset,length) => { maxRead=Math.max(maxRead,length); return new Uint8Array(media.subarray(offset,offset+length)); };
    const result = JSON.parse(auxPackages(JSON.stringify(build),[reader],bytes => { output=Buffer.from(bytes); }));
    assert.equal(result.ok,true,result.error);
    assert.deepEqual(output,fs.readFileSync(path.join(dir,'native.pkg')));
    assert.deepEqual(result.receipt,JSON.parse(fs.readFileSync(path.join(dir,'receipt.json'))));
    assert.ok(maxRead<=65536);
    const reject = request => { let writes=0; const res=JSON.parse(auxPackages(JSON.stringify(request),[reader],()=>writes++)); assert.equal(res.ok,false);assert.equal(writes,0); };
    const tampered=structuredClone(build);tampered.lock.bindings[0].recipeSHA256='0'.repeat(64);reject(tampered);
    const planned=structuredClone(build);planned.lock.bindings[0].recipe.status='planned';reject(planned);
    const plan=JSON.parse(fs.readFileSync(path.join(__dirname,'packages/fixtures/ibrowse.json')));
    const unavailable=JSON.parse(auxPackages(JSON.stringify({action:'resolve',catalog:[plan],requests:[{environmentID:'first',family:'amiga',profile:'amiga-3.2',packages:[{id:plan.id,version:plan.upstreamVersion,variant:plan.variant,tier:'environment'}]}]})));
    assert.equal(unavailable.ok,false);
    let writes=0;
    const corrupt=JSON.parse(auxPackages(JSON.stringify(build),[(offset,length)=>new Uint8Array(length)],()=>writes++));
    assert.equal(corrupt.ok,false);assert.equal(writes,0);
    console.log('PASS: native/WASM package and receipt parity, bounded reads, locked recipe/source checks and planned recipe refusal');
  } finally {fs.rmSync(dir,{recursive:true,force:true});}
  clearTimeout(timer);process.exit(0);
}
main().catch(error=>{console.error(error);process.exit(1);});
