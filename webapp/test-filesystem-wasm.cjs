const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
globalThis.crypto ??= require('node:crypto').webcrypto;
globalThis.performance ??= require('node:perf_hooks').performance;
require(path.join(__dirname, 'build/site/wasm_exec.js'));
function archive() {
  const chunks=[];
  function entry(name,mode,body,ino) {
    body=Buffer.from(body);
    const names=Buffer.from(name+'\0');
    const fields=[ino,mode,0,0,1,0,body.length,0,0,0,0,names.length,0];
    const header=Buffer.from('070701'+fields.map(n=>n.toString(16).padStart(8,'0')).join(''));
    chunks.push(header,names,Buffer.alloc((4-(110+names.length)%4)%4),body,Buffer.alloc((4-body.length%4)%4));
  }
  entry('etc',0o40755,'',1);
  entry('etc/message',0o100644,'Ash Nazag filesystem\n',2);
  entry('message',0o120777,'etc/message',3);
  entry('TRAILER!!!',0,'',0);
  return Buffer.concat(chunks);
}
async function main() {
 const go=new Go();
 const ready=new Promise(resolve=>{globalThis.auxPlannerReady=resolve;});
 const timer=setTimeout(()=>{throw new Error('WASM initialization timed out');},30000);
 const {instance}=await WebAssembly.instantiate(fs.readFileSync(path.join(__dirname,'build/site/planner.wasm')),go.importObject);
 go.run(instance).catch(error=>{throw error;});await ready;
 const dir=fs.mkdtempSync(path.join(os.tmpdir(),'ashfs-wasm-'));
 try {
  const src=process.argv[2]?fs.readFileSync(process.argv[2]):archive();
  const size=Number(process.argv[3] || (process.argv[2]?64:4));
  const input=path.join(dir,'input.cpio'),native=path.join(dir,'native.img'),wasm=path.join(dir,'wasm.img');
  fs.writeFileSync(input,src);
  const run=spawnSync(path.join(__dirname,'build/ashfs'),['-archive',input,'-output',native,'-size',String(size)],{encoding:'utf8',timeout:60000});
  assert.equal(run.status,0,run.stderr);
  const fd=fs.openSync(wasm,'wx+');fs.ftruncateSync(fd,size*1048576);
  let maxRead=0,maxWrite=0;
  let result;
  try {
   result=JSON.parse(auxFilesystem(src.length,size,(offset,length)=>{maxRead=Math.max(maxRead,length);return new Uint8Array(src.subarray(offset,offset+length));},(offset,bytes)=>{maxWrite=Math.max(maxWrite,bytes.length);assert.equal(fs.writeSync(fd,bytes,0,bytes.length,offset),bytes.length);}));
  } finally {fs.closeSync(fd);}
  assert.equal(result.ok,true,result.error);
  const digest = filename => {
   const hash = require('node:crypto').createHash('sha256');
   const file = fs.openSync(filename, 'r');
   const chunk = Buffer.alloc(65536);
   try { let count; while ((count = fs.readSync(file, chunk, 0, chunk.length, null))) hash.update(chunk.subarray(0, count)); } finally { fs.closeSync(file); }
   return hash.digest('hex');
  };
  assert.equal(digest(wasm),digest(native),'native/WASM UFS differ');
  assert.ok(maxRead<=65536,`unbounded read ${maxRead}`);
  assert.ok(maxWrite<=65536,`unbounded write ${maxWrite}`);
  const check=spawnSync('python3',[path.join(__dirname,'../kernel/mac/diskroot/ufscheck.py'),wasm],{encoding:'utf8',timeout:60000});
  assert.equal(check.status,0,check.stdout+check.stderr);
  assert.equal(JSON.parse(auxFilesystem(src.length,5,()=>{},()=>{})).ok,false);
  console.log('PASS: native/WASM filesystem parity, bounded I/O, independent UFS checker and size rejection');
 } finally {fs.rmSync(dir,{recursive:true,force:true});}
 clearTimeout(timer);process.exit(0);
}
main().catch(error=>{console.error(error);process.exit(1);});
