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
 const timer=setTimeout(()=>{throw new Error('WASM provisioning timed out');},60000);
 const {instance}=await WebAssembly.instantiate(fs.readFileSync(path.join(__dirname,'build/site/planner.wasm')),go.importObject);
 go.run(instance).catch(error=>{throw error;});await ready;
 const dir=fs.mkdtempSync(path.join(os.tmpdir(),'ashprovision-'));
 try {
  const base=archive(), pkg=fs.readFileSync(path.join(__dirname,'svr4/testdata/ASHtest.pkg'));
  const sha256=require('node:crypto').createHash('sha256').update(pkg).digest('hex');
  const provision=['template','app'].map(kind=>({kind,family:'amiga',id:'test-1',sha256,size:pkg.length}));
  fs.writeFileSync(path.join(dir,'base.cpio'),base);fs.writeFileSync(path.join(dir,'test.pkg'),pkg);
  fs.writeFileSync(path.join(dir,'provision.json'),JSON.stringify(provision.map(p=>({...p,file:'test.pkg'}))));
  const native=path.join(dir,'native.img'),wasm=path.join(dir,'wasm.img');
  const run=spawnSync(path.join(__dirname,'build/ashfs'),['-archive',path.join(dir,'base.cpio'),'-provision',path.join(dir,'provision.json'),'-output',native,'-size','4'],{encoding:'utf8',timeout:60000});
  assert.equal(run.status,0,run.stderr);
  const fd=fs.openSync(wasm,'wx+');fs.ftruncateSync(fd,4*1048576);
  const request={mode:'layers',sizeMiB:4,sources:[{name:'base',size:base.length}],provision};
  const read=data=>(offset,length)=>new Uint8Array(data.subarray(offset,offset+length));
  let result;
  try {result=JSON.parse(auxRootFilesystem(JSON.stringify(request),[read(base),read(pkg),read(pkg)],(offset,bytes)=>{assert.equal(fs.writeSync(fd,bytes,0,bytes.length,offset),bytes.length);}));}
  finally{fs.closeSync(fd);}
  assert.equal(result.ok,true,result.error);
  assert.deepEqual(fs.readFileSync(wasm),fs.readFileSync(native),'native/WASM provisioning differs');
  const extracted=path.join(dir,'extracted');
  const check=spawnSync('python3',[path.join(__dirname,'../kernel/mac/diskroot/ufscheck.py'),wasm,'-x',extracted],{encoding:'utf8',timeout:60000});
  assert.equal(check.status,0,check.stdout+check.stderr);
  for(const root of ['sys','apps'])assert.equal(fs.readFileSync(path.join(extracted,'amiga',root,'test-1/Apps/copy'),'utf8'),'hello\n');
  for(const family of ['amiga','mac','tos'])assert.deepEqual(fs.readFileSync(path.join(extracted,'etc/default',family)),fs.readFileSync(path.join(__dirname,'../etc/default',family)));
  assert.equal(fs.existsSync(path.join(extracted,'home')),false);
  let wrote=false;request.provision[0].sha256='0'.repeat(64);
  assert.equal(JSON.parse(auxRootFilesystem(JSON.stringify(request),[read(base),read(pkg),read(pkg)],()=>{wrote=true;})).ok,false);
  assert.equal(wrote,false,'wrote image before package validation');
  console.log('PASS: system template/app native/WASM image parity, independent extraction, policy staging and corruption refusal');
 } finally {fs.rmSync(dir,{recursive:true,force:true});}
 clearTimeout(timer);process.exit(0);
}
main().catch(error=>{console.error(error);process.exit(1);});
