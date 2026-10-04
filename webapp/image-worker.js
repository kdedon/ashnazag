/* global Go, auxAssemble */
'use strict';
self.onmessage = async ({ data }) => {
  if (data.type !== 'assemble') return;
  self.onmessage = null;
  let activeOutput = null;
  try {
    const { boot, root, swapMiB, temporaryName } = data;
    if (!(boot instanceof Blob) || !(root instanceof Blob) || !Number.isInteger(swapMiB) || swapMiB < 4 || swapMiB > 2048) {
      throw new Error('Choose boot and root images and 4–2048 MiB of swap.');
    }
    const total = boot.size + root.size + swapMiB * 1048576;
    importScripts('./storage.js');
    const output = await createImageOutput(total, temporaryName);
    activeOutput = output;
    postMessage({ type: 'storage', diskBacked: output.diskBacked });
    importScripts('./wasm_exec.js');
    const ready = new Promise(resolve => { globalThis.auxPlannerReady = resolve; });
    const go = new Go();
    const response = await fetch('./planner.wasm');
    if (!response.ok) throw new Error(`Image engine download failed (${response.status}).`);
    const { instance } = await WebAssembly.instantiate(await response.arrayBuffer(), go.importObject);
    go.run(instance).catch(error => postMessage({ type: 'error', error: error.message }));
    await ready;
    const reader = new FileReaderSync();
    const read = file => (offset, length) => new Uint8Array(reader.readAsArrayBuffer(file.slice(offset, offset + length)));
    let bytes = 0;
    let reported = 0;
    const result = JSON.parse(auxAssemble(boot.size, root.size, swapMiB * 1048576, read(boot), read(root), chunk => {
      if (bytes + chunk.byteLength > total) throw new Error('Image writer exceeded the expected size.');
      output.write(bytes, chunk);
      bytes += chunk.byteLength;
      if (bytes - reported >= 1048576 || bytes === total) {
        postMessage({ type: 'progress', bytes, total });
        reported = bytes;
      }
    }));
    if (!result.ok) throw new Error(result.error);
    if (bytes !== total) throw new Error('Image size verification failed.');
    const file = await output.finish();
    postMessage({ type: 'complete', file });
  } catch (error) {
    if (activeOutput) await activeOutput.remove();
    postMessage({ type: 'error', error: error.name === 'QuotaExceededError' ? 'Browser storage ran out of space. Free space or reduce the image size.' : error.message });
  }
};
