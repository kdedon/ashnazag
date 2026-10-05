/* global newImageTemporary, cleanAbandonedImages */
'use strict';
(() => {
  const panel = document.getElementById('quadra-image-builder');
  panel.innerHTML = `<div class="section-heading"><span class="step">05</span><div><h2>Build a Quadra disk image</h2><p>Create the boot partition, root filesystem and swap in one local build.</p></div></div>
    <p class="field-help">This recipe installs the Quadra console system with its default devices. Packages, guests, graphical login and custom drivers are not included. Supply a prebuilt Quadra kernel; kernel compilation is not yet available in the browser.</p>
    <form id="quadra-form" aria-label="Complete Quadra image build">
      <div class="media-fields">
        <label class="media-field">AMIX 2.1 tape archive (one or more parts)<input id="quadra-tape" type="file" multiple required><span class="field-help">Select every part, such as both <code>.tar.bz2</code> archives, or a <code>.tap</code> image or the segment files. Missing segments are named.</span></label>
        <label class="media-field">Prebuilt Quadra kernel (m68k ELF)<input id="quadra-kernel" type="file" required></label>
        <label class="media-field">A/UX boot donor disk<input id="quadra-donor" type="file" accept=".img,.dsk,application/octet-stream" required><span class="field-help">Supplies the Apple partition map and disk drivers. The builder creates a new HFS boot partition for the selected kernel.</span></label>
      </div>
      <p class="field-help">All media stays on your computer.</p>
      <div class="hardware-fields">
        <label class="version-field">Root filesystem (MiB)<input id="quadra-root" type="number" min="64" max="2048" step="4" value="64" required></label>
        <label class="version-field">Swap (MiB)<input id="quadra-swap" type="number" min="4" max="2048" step="1" value="64" required></label>
      </div>
      <p class="field-help">Root filesystems support up to 2 GiB, plus up to 2 GiB of swap. Browser disk storage requires space for the final disk and the temporary root image. The small boot image is held in memory. The memory fallback limits each image to 256 MiB. Download before changing inputs or leaving this page.</p>
      <button id="quadra-build" class="primary-button" type="submit">Forge Quadra disk image <span aria-hidden="true">↓</span></button>
      <button id="quadra-cancel" class="text-button" type="button" hidden>Cancel build</button>
      <progress id="quadra-progress" aria-label="Quadra build progress" hidden></progress>
      <p id="quadra-status" role="status" aria-live="polite" class="field-help">Choose the source files to begin.</p>
      <p><a id="quadra-download" download="ash-nazag-q800.img" hidden>Download ash-nazag-q800.img</a></p>
      <p><a id="quadra-recipe" download="ash-nazag-q800-recipe.json" hidden>Export build recipe (JSON)</a></p>
      <p class="field-help">The recipe lists your selections and each input's size and SHA-256, never file names or contents. The image carries it at <code>/etc/forge/recipe.json</code>.</p>
    </form>`;
  const get = name => document.getElementById(`quadra-${name}`);
  const form = get('form'), button = get('build'), cancel = get('cancel');
  const status = get('status'), progress = get('progress'), download = get('download'), recipeLink = get('recipe');
  const tape = get('tape'), kernel = get('kernel'), donor = get('donor'), root = get('root'), swap = get('swap');
  const inputs = [tape, kernel, donor, root, swap];
  let worker = null, leases = [], url = null, recipeURL = null, generation = 0, imported = '';
  const abandoned = cleanAbandonedImages().catch(() => {});
  const controls = busy => {
    for (const input of [...inputs, button]) input.disabled = busy;
    cancel.hidden = !busy; progress.hidden = !busy;
    form.setAttribute('aria-busy', String(busy));
  };
  async function reset() {
    generation++;
    worker?.terminate(); worker = null;
    if (url) URL.revokeObjectURL(url);
    url = null; download.hidden = true; download.removeAttribute('href');
    if (recipeURL) URL.revokeObjectURL(recipeURL);
    recipeURL = null; recipeLink.hidden = true; recipeLink.removeAttribute('href');
    const old = leases; leases = [];
    await Promise.all(old.map(lease => lease.remove().catch(() => {})));
  }
  async function fail(message) {
    await reset(); controls(false); status.textContent = message;
  }
  form.addEventListener('submit', async event => {
    event.preventDefault();
    if (form.getAttribute('aria-busy') === 'true' || !form.reportValidity()) return;
    controls(true); progress.removeAttribute('value');
    const pending = reset(), run = generation;
    await pending;
    if (run !== generation) return;
    status.textContent = 'Preparing local build storage…';
    try {
      await abandoned;
      for (let i = 0; i < 2; i++) {
        const lease = await newImageTemporary();
        if (run !== generation) { await lease.remove(); return; }
        leases.push(lease);
      }
      worker = new Worker('./quadra-worker.js');
      worker.onmessage = ({ data }) => {
        if (run !== generation) return;
        if (data.type === 'status') { status.textContent = data.message; progress.removeAttribute('value'); }
        else if (data.type === 'progress') {
          progress.max = data.total; progress.value = data.bytes;
          status.textContent = `Assembling disk image: ${Math.floor(data.bytes / data.total * 100)}%.`;
        } else if (data.type === 'error') void fail(`Quadra build failed: ${data.error}`);
        else if (data.type === 'complete') {
          if (!(data.file instanceof Blob) || !data.file.size || data.file.size !== data.bytes) {
            void fail('The builder returned an invalid disk image.'); return;
          }
          url = URL.createObjectURL(data.file); download.href = url; download.hidden = false;
          recipeURL = URL.createObjectURL(new Blob([data.recipe], { type: 'application/json' })); recipeLink.href = recipeURL; recipeLink.hidden = false;
          worker.terminate(); worker = null; controls(false);
          status.textContent = `Quadra disk image ready (${(data.file.size / 1048576).toFixed(1)} MiB). Download it below.` + (data.mismatches.length ? ` Inputs differ from the imported recipe (${data.mismatches.join(', ')}), so the image differs too.` : '');
          download.focus();
        }
      };
      worker.onerror = event => { event.preventDefault(); if (run === generation) void fail(`Quadra worker failed: ${event.message}`); };
      worker.onmessageerror = () => { if (run === generation) void fail('The Quadra worker returned unreadable data.'); };
      worker.postMessage({ type: 'quadra', tape: Array.from(tape.files), kernel: kernel.files[0], donor: donor.files[0], rootMiB: Number(root.value), swapMiB: Number(swap.value), temporaryNames: leases.map(lease => lease.name), selection: { preset: 'quadra800', machine: 'q800', settings: { devices: window.forgeDevices?.('q800') ?? ['adb', 'framebuffer', 'scc', 'scsi53c96'] } }, expect: imported });
    } catch (error) { if (run === generation) await fail(`Quadra build failed: ${error.message}`); }
  });
  cancel.addEventListener('click', () => { void fail('Quadra build cancelled.'); });
  for (const input of inputs) input.addEventListener('change', () => {
    void reset(); controls(false); status.textContent = 'Inputs changed. Forge a new disk image.';
  });
  window.addEventListener('pagehide', () => { void reset(); controls(false); });
  window.setQuadraSettings = settings => {
    if (settings.preset !== 'quadra800') return;
    root.value = settings.rootMiB; swap.value = settings.swapMiB; imported = settings.recipe;
    void reset(); controls(false);
  };
  window.openQuadraBuilder = () => { panel.scrollIntoView({ behavior: 'smooth', block: 'start' }); tape.focus({ preventScroll: true }); };
})();
