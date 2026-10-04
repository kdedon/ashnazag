/* global Go, auxRootFilesystem, createImageOutput */
'use strict';
importScripts('./storage.js');
self.onmessage = async ({ data }) => {
  if (data.type !== 'filesystem') return;
  self.onmessage = null;
  let output;
  try {
    const { mode, archives, kernel, sizeMiB, temporaryName } = data;
    if (!['layers', 'quadra-console'].includes(mode)) throw new Error('Choose a supported filesystem recipe.');
    if (!Array.isArray(archives) || !archives.length || archives.length > 64) throw new Error('Choose 1–64 archives.');
    for (const { file } of archives) {
      if (!(file instanceof Blob) || !file.size || file.size > 8 * 1073741824) throw new Error('Choose nonempty SVR4 cpio archives up to 8 GiB each.');
    }
    if (mode === 'quadra-console' && (!(kernel instanceof Blob) || kernel.size < 52 || kernel.size > 8 * 1073741824)) throw new Error('Choose a prebuilt m68k kernel ELF.');
    if (!Number.isInteger(sizeMiB) || sizeMiB < 4 || sizeMiB > 2048 || sizeMiB % 4) throw new Error('Filesystem size must be 4–2048 MiB in multiples of 4.');
    const provision = data.provision ?? [];
    if (!Array.isArray(provision) || provision.length > 64) throw new Error('Choose at most 64 provisioning packages.');
    let packageBytes = 0;
    for (const item of provision) {
      if (!item || !(item.file instanceof Blob) || !item.file.size || !['template', 'app'].includes(item.kind) || !['mac', 'tos', 'amiga'].includes(item.family) || typeof item.sha256 !== 'string' || !/^[a-f0-9]{64}$/.test(item.sha256)) throw new Error('Invalid provisioning package.');
      packageBytes += item.file.size;
      if (packageBytes > 256 * 1048576) throw new Error('Provisioning packages exceed the 256 MiB working-memory budget.');
    }
    const packageMetadata = provision.map(({ kind, family, id, sha256, file }) => ({ kind, family, id, sha256, size: file.size }));
    output = await createImageOutput(sizeMiB * 1048576, temporaryName);
    postMessage({ type: 'status', message: 'Layering archives and checking filesystem entries…' });
    importScripts('./wasm_exec.js');
    const ready = new Promise(resolve => { globalThis.auxPlannerReady = resolve; });
    const go = new Go();
    const response = await fetch('./planner.wasm');
    if (!response.ok) throw new Error(`Filesystem engine download failed (${response.status}).`);
    const { instance } = await WebAssembly.instantiate(await response.arrayBuffer(), go.importObject);
    go.run(instance).catch(error => postMessage({ type: 'error', error: error.message }));
    await ready;
    const reader = new FileReaderSync();
    let bytes = 0, reported = 0;
    const files = archives.map(source => source.file);
    if (mode === 'quadra-console') files.push(kernel);
    files.push(...provision.map(item => item.file));
    const metadata = { mode, sizeMiB, sources: archives.map(source => ({ name: source.name, size: source.file.size })), kernelSize: kernel?.size || 0, provision: packageMetadata };
    const result = JSON.parse(auxRootFilesystem(JSON.stringify(metadata),
      files.map(file => (offset, length) => new Uint8Array(reader.readAsArrayBuffer(file.slice(offset, offset + length)))) ,
      (offset, chunk) => {
        output.write(offset, chunk);
        bytes += chunk.byteLength;
        if (bytes - reported >= 1048576) {
          reported = bytes;
          postMessage({ type: 'status', message: `Writing filesystem: ${(bytes / 1048576).toFixed(1)} MiB written…` });
        }
      }));
    if (!result.ok) throw new Error(result.error);
    const file = await output.finish();
    postMessage({ type: 'complete', file, report: result.report });
  } catch (error) {
    if (output) await output.remove();
    postMessage({ type: 'error', error: error.message });
  }
};
