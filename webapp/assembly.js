'use strict';

(() => {
  const get = (id) => document.getElementById(`prepared-${id}`);
  const form = get('assembly-form');
  const boot = get('boot');
  const root = get('root');
  const swap = get('swap');
  const submit = get('assemble');
  const cancel = get('cancel');
  const progress = get('progress');
  const status = get('status');
  const download = get('download');
  let activeWorker = null;
  let outputURL = null;
  let temporary = null;
  let starting = false;
  let generation = 0;
  const cleanup = cleanAbandonedImages().catch(() => {});

  function releaseTemporary() {
    const old = temporary;
    temporary = null;
    if (old) old.remove().catch(() => {});
  }

  function clearDownload() {
    download.hidden = true;
    download.removeAttribute('href');
    if (outputURL) URL.revokeObjectURL(outputURL);
    outputURL = null;
    releaseTemporary();
  }

  function finish(message, keepOutput = false) {
    generation++;
    starting = false;
    if (activeWorker) activeWorker.terminate();
    activeWorker = null;
    if (!keepOutput) releaseTemporary();
    for (const input of [boot, root, swap, submit]) input.disabled = false;
    cancel.hidden = true;
    progress.hidden = true;
    form.setAttribute('aria-busy', 'false');
    status.textContent = message;
  }

  form.addEventListener('submit', async (event) => {
    event.preventDefault();
    if (activeWorker || starting || !form.reportValidity()) return;
    clearDownload();
    const swapMiB = Number(swap.value);
    if (!boot.files[0]?.size || !root.files[0]?.size) {
      status.textContent = 'Choose a nonempty boot disk and UFS root image.';
      return;
    }
    if (!Number.isSafeInteger(swapMiB) || swapMiB < 4 || swapMiB > 2048) {
      status.textContent = 'Enter a whole swap size from 4 to 2048 MiB.';
      return;
    }
    for (const input of [boot, root, swap, submit]) input.disabled = true;
    cancel.hidden = false;
    progress.hidden = false;
    progress.removeAttribute('value');
    form.setAttribute('aria-busy', 'true');
    status.textContent = 'Preparing local image assembly…';
    const currentGeneration = ++generation;
    try {
      starting = true;
      await cleanup;
      const lease = await newImageTemporary();
      if (currentGeneration !== generation) { await lease.remove(); return; }
      temporary = lease;
      starting = false;
      const worker = new Worker('./image-worker.js');
      activeWorker = worker;
      worker.onmessage = ({ data }) => {
        if (activeWorker !== worker) return;
        if (data.type === 'progress') {
          if (Number.isFinite(data.bytes) && Number.isFinite(data.total) && data.total > 0) {
            progress.max = data.total;
            progress.value = Math.max(0, Math.min(data.bytes, data.total));
            status.textContent = `Assembling disk image: ${Math.floor(progress.value / data.total * 100)}%.`;
          }
        } else if (data.type === 'complete') {
          if (!(data.file instanceof Blob) || !data.file.size) {
            finish('Assembly returned an empty or invalid image. Retry with valid prepared inputs.');
            return;
          }
          outputURL = URL.createObjectURL(data.file);
          download.href = outputURL;
          download.hidden = false;
          finish(`Disk image ready (${(data.file.size / 1048576).toFixed(1)} MiB). Download it below.`, true);
          download.focus();
        } else if (data.type === 'error') {
          finish(`Assembly failed: ${String(data.error || 'Unknown image engine error.')}`);
        }
      };
      worker.onerror = (error) => {
        if (activeWorker !== worker) return;
        error.preventDefault();
        finish(`Assembly failed: ${error.message || 'The image worker could not start.'}`);
      };
      worker.onmessageerror = () => {
        if (activeWorker === worker) finish('Assembly failed: the image worker returned unreadable data.');
      };
      worker.postMessage({ type: 'assemble', boot: boot.files[0], root: root.files[0], swapMiB, temporaryName: temporary.name });
    } catch (error) {
      if (currentGeneration !== generation) return;
      starting = false;
      finish(`Assembly failed: ${error.message}`);
    }
  });

  cancel.addEventListener('click', () => {
    finish('Assembly cancelled. Your input files are unchanged.');
  });
  for (const input of [boot, root, swap]) {
    input.addEventListener('change', () => {
      clearDownload();
      status.textContent = 'Prepared inputs changed. Assemble again to create a new image.';
    });
  }
  window.addEventListener('pagehide', () => {
    if (activeWorker || starting || outputURL) finish('Assemble again to create a new downloadable image.');
    clearDownload();
  });
})();
