/* global newImageTemporary, cleanAbandonedImages */
'use strict';
(() => {
  const panel = document.getElementById('filesystem-generator');
  panel.innerHTML = `<details><summary>Advanced · Create a root filesystem</summary>
    <p class="field-help">Layer extracted SVR4 cpio archives, or build a Quadra console root with installation patches and a prebuilt kernel. Permissions, device nodes and links are preserved. The result is a UFS root image; a boot partition and disk assembly are still required.</p>
    <form id="filesystem-form">
      <label class="version-field">Filesystem recipe<select id="filesystem-mode"><option value="layers">Layered archives</option><option value="quadra-console">Quadra console root</option></select></label>
      <div id="filesystem-layers">
        <label class="media-field">SVR4 cpio archives<input id="filesystem-archive" type="file" multiple required></label>
        <p class="field-help">Archives are applied in the order listed below. Later files replace earlier paths. Check the list before building.</p>
        <ol id="filesystem-order"></ol>
      </div>
      <div id="filesystem-quadra" hidden>
        <label class="media-field">AMIX 2.1 tape archive (one or more parts)<input id="filesystem-tape" type="file" multiple disabled required></label>
        <label class="media-field">Prebuilt Quadra kernel (m68k ELF)<input id="filesystem-kernel" type="file" disabled required></label>
        <p class="field-help">Console-only Quadra recipe. Select every tape part: both <code>.tar.bz2</code> archives, a <code>.tap</code> image or the segment files. Kernel compilation and guest environment installation are separate steps.</p>
      </div>
      <label class="version-field">Filesystem size (MiB)<input id="filesystem-size" type="number" min="4" max="2048" step="4" value="64" required></label>
      <p class="field-help">Up to 2 GiB per UFS filesystem with browser disk storage and sufficient quota. The memory fallback supports up to 256 MiB. Leave room for filesystem metadata and free space.</p>
      <button class="primary-button" id="filesystem-build" type="submit">Create UFS image</button>
      <button id="filesystem-cancel" type="button" hidden>Cancel</button>
      <p id="filesystem-status" role="status" aria-live="polite" class="field-help">Choose an archive to begin.</p>
      <a id="filesystem-download" download="ash-nazag-root.img" hidden>Download ash-nazag-root.img</a>
    </form></details>`;
  const get = suffix => document.getElementById(`filesystem-${suffix}`);
  const form = get('form'), archive = get('archive'), size = get('size'), button = get('build');
  const mode = get('mode'), tape = get('tape'), kernel = get('kernel');
  const cancel = get('cancel'), status = get('status'), download = get('download');
  let worker = null, temporary = null, url = null, generation = 0;
  const controls = busy => {
    const quadra = mode.value === 'quadra-console';
    get('layers').hidden = quadra; get('quadra').hidden = !quadra;
    for (const input of [mode, size, button]) input.disabled = busy;
    archive.disabled = busy || quadra;
    for (const input of [tape, kernel]) input.disabled = busy || !quadra;
    cancel.hidden = !busy;
    form.setAttribute('aria-busy', String(busy));
  };
  async function reset() {
    generation++;
    worker?.terminate(); worker = null;
    if (url) URL.revokeObjectURL(url);
    url = null; download.hidden = true; download.removeAttribute('href');
    const old = temporary; temporary = null;
    if (old) await old.remove().catch(() => {});
  }
  async function fail(message) {
    await reset(); controls(false); status.textContent = message;
  }
  form.addEventListener('submit', async event => {
    event.preventDefault();
    if (form.getAttribute('aria-busy') === 'true' || !form.reportValidity()) return;
    controls(true);
    const cleanup = reset();
    const run = generation;
    await cleanup;
    if (run !== generation) return;
    status.textContent = 'Preparing local filesystem storage…';
    try {
      const lease = await newImageTemporary();
      if (run !== generation) { await lease.remove(); return; }
      temporary = lease;
      worker = new Worker('./filesystem-worker.js');
      worker.onmessage = ({ data }) => {
        if (run !== generation) return;
        if (data.type === 'status') status.textContent = data.message;
        else if (data.type === 'error') void fail(`Filesystem generation failed: ${data.error}`);
        else if (data.type === 'complete') {
          if (!(data.file instanceof Blob) || data.file.size !== Number(size.value) * 1048576) {
            void fail('Filesystem generation returned an invalid image.'); return;
          }
          url = URL.createObjectURL(data.file); download.href = url; download.hidden = false;
          worker.terminate(); worker = null; controls(false);
          status.textContent = `UFS filesystem ready (${(data.file.size / 1048576).toFixed(0)} MiB). Download it below.`;
          download.focus();
        }
      };
      worker.onerror = event => { event.preventDefault(); if (run === generation) void fail(`Filesystem worker failed: ${event.message}`); };
      worker.onmessageerror = () => { if (run === generation) void fail('Filesystem worker returned unreadable data.'); };
      const quadra = mode.value === 'quadra-console';
      const sources = quadra ? { tape: Array.from(tape.files) } : { archives: Array.from(archive.files, file => ({ name: file.name, file })) };
      worker.postMessage({ type: 'filesystem', mode: mode.value, ...sources, kernel: quadra ? kernel.files[0] : null, sizeMiB: Number(size.value), temporaryName: temporary.name });
    } catch (error) { if (run === generation) await fail(`Filesystem generation failed: ${error.message}`); }
  });
  cancel.addEventListener('click', () => { void fail('Filesystem generation cancelled.'); });
  for (const input of [mode, archive, tape, kernel, size]) input.addEventListener('change', () => {
    void reset(); controls(false);
    get('order').replaceChildren(...Array.from(archive.files, file => {
      const item = document.createElement('li'); item.textContent = file.name; return item;
    }));
    status.textContent = 'Inputs changed. Create a new filesystem image.';
  });
  window.addEventListener('pagehide', () => { void reset(); controls(false); });
  void cleanAbandonedImages().catch(() => {});
})();
