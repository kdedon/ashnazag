'use strict';

async function createImageOutput(size, temporaryName) {
  if (!/^ash-image-[a-z0-9-]+$/.test(temporaryName)) throw new Error('Invalid temporary image name.');
  if (!Number.isSafeInteger(size) || size <= 0 || size > 8 * 1073741824) throw new Error('Images must be no larger than 8 GiB.');
  const diskBacked = typeof navigator.storage?.getDirectory === 'function' && typeof globalThis.FileSystemFileHandle?.prototype.createSyncAccessHandle === 'function';
  let directory, handle, access, memory;
  const remove = async () => {
    if (access) { try { access.close(); } catch {} access = null; }
    memory = null;
    if (directory) { try { await directory.removeEntry(temporaryName); } catch {} }
  };
  try {
    if (diskBacked) {
      const estimate = await navigator.storage.estimate();
      if (Number.isFinite(estimate.quota) && Number.isFinite(estimate.usage) && estimate.quota - estimate.usage < size) {
        throw new Error('Browser storage has insufficient free space for this image. Free space or reduce the image size.');
      }
      directory = await navigator.storage.getDirectory();
      handle = await directory.getFileHandle(temporaryName, { create: true });
      access = await handle.createSyncAccessHandle();
      access.truncate(0);
      access.truncate(size);
    } else {
      if (size > 256 * 1048576) throw new Error('This browser lacks disk-backed image storage. Use a current browser over HTTPS or localhost for images above 256 MiB.');
      memory = new Uint8Array(size);
    }
  } catch (error) { await remove(); throw error; }
  return {
    diskBacked,
    write(offset, chunk) {
      if (!Number.isSafeInteger(offset) || offset < 0 || offset + chunk.byteLength > size) throw new Error('Image write is outside the output size.');
      if (access) {
        let written = 0;
        while (written < chunk.byteLength) {
          const count = access.write(chunk.subarray(written), { at: offset + written });
          if (count <= 0) throw new Error('Browser storage could not finish writing the image.');
          written += count;
        }
      } else if (memory) memory.set(chunk, offset);
      else throw new Error('Image output is closed.');
    },
    async finish() {
      if (access) { access.flush(); access.close(); access = null; }
      const file = handle ? await handle.getFile() : new File([memory], 'ash-nazag.img', { type: 'application/octet-stream' });
      memory = null;
      if (file.size !== size) throw new Error('Stored image size verification failed.');
      return file;
    },
    remove,
  };
}

async function removeImageTemporary(name) {
  if (!/^ash-image-[a-z0-9-]+$/.test(name) || !navigator.storage?.getDirectory) return;
  const directory = await navigator.storage.getDirectory();
  for (let attempt = 0; attempt < 6; attempt++) {
    try { await directory.removeEntry(name); return; }
    catch (error) {
      if (error.name === 'NotFoundError') return;
      if (attempt === 5) throw error;
      await new Promise(resolve => setTimeout(resolve, 50));
    }
  }
}

async function newImageTemporary() {
  const name = `ash-image-${crypto.randomUUID()}`;
  let release;
  if (navigator.locks) {
    await new Promise((resolve, reject) => {
      navigator.locks.request(name, async () => {
        await new Promise(done => { release = done; resolve(); });
      }).catch(reject);
    });
  }
  return {
    name,
    async remove() {
      try { await removeImageTemporary(name); }
      finally { if (release) release(); }
    },
  };
}

async function cleanAbandonedImages() {
  if (!navigator.locks || !navigator.storage?.getDirectory) return;
  const directory = await navigator.storage.getDirectory();
  for await (const name of directory.keys()) {
    if (!/^ash-image-[a-z0-9-]+$/.test(name)) continue;
    await navigator.locks.request(name, { ifAvailable: true }, async lock => {
      if (lock) await removeImageTemporary(name);
    });
  }
}
