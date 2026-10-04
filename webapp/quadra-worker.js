/* global Go, auxForgeRecipe, auxQuadraBoot, auxRootFilesystem, auxAssemble, createImageOutput */
'use strict';
importScripts('./storage.js');
self.onmessage = async ({ data }) => {
  if (data.type !== 'quadra') return;
  self.onmessage = null;
  let rootOutput, diskOutput;
  try {
    const { archives, kernel, donor, rootMiB, swapMiB, temporaryNames } = data;
    if (!Array.isArray(archives) || archives.length !== 3 || archives.some((source, i) => source.name !== ['02', '03', '10'][i] || !(source.file instanceof Blob) || !source.file.size || source.file.size > 8 * 1073741824)) {
      throw new Error('Choose nonempty extracted AMIX segments 02, 03 and 10.');
    }
    if (!(kernel instanceof Blob) || kernel.size < 52 || kernel.size > 32 * 1048576) throw new Error('Choose a prebuilt Quadra kernel ELF up to 32 MiB.');
    if (!(donor instanceof Blob) || donor.size < 1024 || donor.size > 8 * 1073741824) throw new Error('Choose an A/UX boot donor disk up to 8 GiB.');
    if (!Number.isInteger(rootMiB) || rootMiB < 64 || rootMiB > 2048 || rootMiB % 4) throw new Error('Root size must be 64–2048 MiB in multiples of 4.');
    if (!Number.isInteger(swapMiB) || swapMiB < 4 || swapMiB > 2048) throw new Error('Swap size must be 4–2048 MiB.');
    if (!Array.isArray(temporaryNames) || temporaryNames.length !== 2 || temporaryNames[0] === temporaryNames[1] || temporaryNames.some(name => typeof name !== 'string' || !/^ash-image-[a-z0-9-]+$/.test(name))) throw new Error('Invalid temporary image names.');
    const provision = data.provision ?? [];
    if (!Array.isArray(provision) || provision.length > 64) throw new Error('Choose at most 64 provisioning packages.');
    let packageBytes = 0;
    for (const item of provision) {
      if (!item || !(item.file instanceof Blob) || !item.file.size || !['template', 'app'].includes(item.kind) || !['mac', 'tos', 'amiga'].includes(item.family) || typeof item.sha256 !== 'string' || !/^[a-f0-9]{64}$/.test(item.sha256)) throw new Error('Invalid provisioning package.');
      packageBytes += item.file.size;
      if (packageBytes > 256 * 1048576) throw new Error('Provisioning packages exceed the 256 MiB working-memory budget.');
    }
    const packageMetadata = provision.map(({ kind, family, id, sha256, file }) => ({ kind, family, id, sha256, size: file.size }));
    postMessage({ type: 'status', message: 'Loading the local Quadra build engine…' });
    importScripts('./wasm_exec.js');
    const ready = new Promise(resolve => { globalThis.auxPlannerReady = resolve; });
    const go = new Go();
    const response = await fetch('./planner.wasm');
    if (!response.ok) throw new Error(`Quadra engine download failed (${response.status}).`);
    const { instance } = await WebAssembly.instantiate(await response.arrayBuffer(), go.importObject);
    go.run(instance).catch(error => postMessage({ type: 'error', error: error.message }));
    await ready;
    const reader = new FileReaderSync();
    const read = file => (offset, length) => new Uint8Array(reader.readAsArrayBuffer(file.slice(offset, offset + length)));
    let recipe, mismatches = [];
    if (data.selection) {
      postMessage({ type: 'status', message: 'Hashing inputs for the build recipe…' });
      const { preset, machine, settings } = data.selection;
      const selection = { preset, machine, settings: { ...settings, rootMiB, swapMiB }, provision: provision.map(({ kind, family, id }) => ({ kind, family, id })) };
      const files = [...archives.map(source => source.file), kernel, donor, ...provision.map(item => item.file)];
      const made = JSON.parse(auxForgeRecipe(JSON.stringify({ selection, sizes: files.map(file => file.size), expect: data.expect ?? '' }), files.map(read)));
      if (!made.ok) throw new Error(made.error);
      recipe = made.recipe; mismatches = made.mismatches ?? [];
    }
    postMessage({ type: 'status', message: 'Creating the HFS boot partition and installing the kernel…' });
    const bootResult = auxQuadraBoot(donor.size, kernel.size, read(donor), read(kernel));
    if (!bootResult.ok) throw new Error(bootResult.error);
    if (!(bootResult.data instanceof Uint8Array) || bootResult.bytes !== bootResult.data.byteLength || !bootResult.bytes) throw new Error('Boot builder returned invalid image data.');
    const boot = new File([bootResult.data], 'quadra-boot.img');
    bootResult.data = null;
    const total = boot.size + (rootMiB + swapMiB) * 1048576;
    if (total > 8 * 1073741824) throw new Error('The complete disk exceeds 8 GiB.');
    rootOutput = await createImageOutput(rootMiB * 1048576, temporaryNames[0]);
    diskOutput = await createImageOutput(total, temporaryNames[1]);
    postMessage({ type: 'status', message: 'Layering AMIX media and applying Quadra console patches…' });
    const metadata = { mode: 'quadra-console', sizeMiB: rootMiB, sources: archives.map(source => ({ name: source.name, size: source.file.size })), kernelSize: kernel.size, provision: packageMetadata, ...(recipe ? { recipe } : {}) };
    let rootWritten = 0, rootReported = 0;
    const rootResult = JSON.parse(auxRootFilesystem(JSON.stringify(metadata), [...archives.map(source => read(source.file)), read(kernel), ...provision.map(item => read(item.file))], (offset, chunk) => {
      rootOutput.write(offset, chunk);
      rootWritten += chunk.byteLength;
      if (rootWritten - rootReported >= 1048576) {
        rootReported = rootWritten;
        postMessage({ type: 'status', message: `Writing Quadra root filesystem: ${(rootWritten / 1048576).toFixed(1)} MiB written…` });
      }
    }));
    if (!rootResult.ok) throw new Error(rootResult.error);
    const root = await rootOutput.finish();
    postMessage({ type: 'status', message: 'Assembling the boot, root and swap partitions…' });
    let bytes = 0, reported = 0;
    const result = JSON.parse(auxAssemble(boot.size, root.size, swapMiB * 1048576, read(boot), read(root), chunk => {
      diskOutput.write(bytes, chunk);
      bytes += chunk.byteLength;
      if (bytes - reported >= 1048576 || bytes === total) {
        reported = bytes;
        postMessage({ type: 'progress', bytes, total });
      }
    }));
    if (!result.ok) throw new Error(result.error);
    if (bytes !== total) throw new Error('Disk image size verification failed.');
    const file = await diskOutput.finish();
    await rootOutput.remove(); rootOutput = null;
    postMessage({ type: 'complete', file, bytes, report: rootResult.report, recipe, mismatches });
  } catch (error) {
    await Promise.all([rootOutput, diskOutput].filter(Boolean).map(output => output.remove().catch(() => {})));
    postMessage({ type: 'error', error: error.name === 'QuotaExceededError' ? 'Browser storage ran out of space. Free space or reduce the root or swap size.' : error.message });
  }
};
