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
 const timer=setTimeout(()=>{throw new Error('WASM root generation timed out');},60000);
 const {instance}=await WebAssembly.instantiate(fs.readFileSync(path.join(__dirname,'build/site/planner.wasm')),go.importObject);
 go.run(instance).catch(error=>{throw error;});await ready;
 const dir=fs.mkdtempSync(path.join(os.tmpdir(),'ashroot-wasm-'));
 try {
  const quadra=Boolean(process.argv[2]);
  const size=quadra?64:4;
  const sources=quadra?['02','03','10'].map(name=>({name,data:fs.readFileSync(path.join(process.argv[2],name))})): [{name:'base',data:archive()},{name:'overlay',data:archive()}];
  const native=path.join(dir,'native.img'),wasm=path.join(dir,'wasm.img');
  const args=['-output',native,'-size',String(size)];
  for(const [index,source] of sources.entries()) {
    const input=path.join(dir,'input'+index+'.cpio');fs.writeFileSync(input,source.data);args.push('-archive',input);
  }
  let kernel;
  if(quadra){kernel=fs.readFileSync(process.argv[3]);args.push('-recipe','quadra-console','-kernel',process.argv[3]);}
  const run=spawnSync(path.join(__dirname,'build/ashfs'),args,{encoding:'utf8',timeout:60000});
  assert.equal(run.status,0,run.stderr);
  const fd=fs.openSync(wasm,'wx+');fs.ftruncateSync(fd,size*1048576);
  const metadata={mode:quadra?'quadra-console':'layers',sizeMiB:size,sources:sources.map(s=>({name:s.name,size:s.data.length}))};
  const read=data=>(offset,length)=>new Uint8Array(data.subarray(offset,offset+length));
  const callbacks=sources.map(s=>read(s.data));
  if(quadra){metadata.kernelSize=kernel.length;callbacks.push(read(kernel));}
  let result;
  try {result=JSON.parse(auxRootFilesystem(JSON.stringify(metadata),callbacks,(offset,bytes)=>{assert.equal(fs.writeSync(fd,bytes,0,bytes.length,offset),bytes.length);}));}
  finally{fs.closeSync(fd);}
  assert.equal(result.ok,true,result.error);
  assert.deepEqual(fs.readFileSync(wasm),fs.readFileSync(native),'native/WASM root differs');
  const check=spawnSync('python3',[path.join(__dirname,'../kernel/mac/diskroot/ufscheck.py'),wasm],{encoding:'utf8',timeout:60000});
  assert.equal(check.status,0,check.stdout+check.stderr);
  assert.equal(JSON.parse(auxRootFilesystem('{}',[],()=>{})).ok,false);
  if(quadra && process.argv[4]) fs.copyFileSync(wasm,process.argv[4],fs.constants.COPYFILE_EXCL);
  console.log('PASS: '+(quadra?'Quadra recipe':'Layered archives')+' native/WASM root parity and independent UFS checker');
 } finally {fs.rmSync(dir,{recursive:true,force:true});}
 clearTimeout(timer);process.exit(0);
}
main().catch(error=>{console.error(error);process.exit(1);});
