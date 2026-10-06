'use strict';
const element = (id) => document.getElementById(id);
const sections = ['devices', 'packages'];
const drafts = new Map();
const sharedMedia = {};
let instanceID = 0;
let selectedMachine = 'q800';
const familySelections = new Map();
let catalog;
let manifest;
let requestID = 0;
let planRevision = 0;
let worker;
const pending = new Map();

function fail(message) {
  manifest = null;
  element('generate-image').disabled = true;
  element('plan-content').hidden = true;
  element('runtime-status').textContent = message;
}

function request(request, forge = false) {
  return new Promise((resolve, reject) => {
    const id = ++requestID;
    const timeout = setTimeout(() => {
      pending.delete(id);
      reject(new Error('The local planner did not respond. Reload to retry.'));
    }, 30000);
    pending.set(id, { resolve, reject, timeout });
    worker.postMessage({ id, request, forge });
  });
}

function textNode(tag, content, className) {
  const node = document.createElement(tag);
  node.textContent = content;
  if (className) node.className = className;
  return node;
}

function machineID() {
  return selectedMachine;
}

function selectMachine(id) {
  selectedMachine = id;
  const machine = catalog.machines.find((item) => item.id === id);
  familySelections.set(machine.family, id);
  renderMachines();
  renderHardware();
  renderOptions();
  renderContainers();
  renderMedia();
  updatePlan();
}

function renderMachines() {
  const current = catalog.machines.find((item) => item.id === machineID());
  element('machines').replaceChildren();
  for (const family of ['Macintosh', 'Amiga', 'Atari']) {
    const card = textNode('div', '', 'family-card');
    card.classList.toggle('selected', current.family === family);
    card.append(textNode('span', family === 'Macintosh' ? '▣' : '▱', 'machine-symbol'), textNode('h3', family));
    const copy = { Macintosh: 'Quadra and Macintosh II models', Amiga: 'One family, model and CPU options', Atari: 'Falcon, CT60/CT63 and TT030' };
    card.append(textNode('p', copy[family]));
    const button = textNode('button', `Configure ${family}`, 'configure-button');
    button.type = 'button';
    button.setAttribute('aria-controls', 'hardware-config');
    button.setAttribute('aria-expanded', String(current.family === family));
    button.addEventListener('click', () => {
      selectMachine(familySelections.get(family) || catalog.machines.find((item) => item.family === family).id);
      element('model').focus();
    });
    card.append(button);
    element('machines').append(card);
  }
}

function renderHardware() {
  const current = catalog.machines.find((item) => item.id === machineID());
  element('hardware-title').textContent = `Configure ${current.family}`;
  const models = [...new Set(catalog.machines.filter((item) => item.family === current.family).map((item) => item.model))];
  element('model').replaceChildren(...models.map((name) => { const option = textNode('option', name); option.value = name; return option; }));
  element('model').value = current.model;
  element('variant').replaceChildren(...catalog.machines.filter((item) => item.family === current.family && item.model === current.model).map((item) => {
    const option = textNode('option', `${item.variant} · ${item.status}`); option.value = item.id; return option;
  }));
  element('variant').value = current.id;
  element('hardware-status').textContent = current.description;
  element('memory').replaceChildren(...(current.memoryBanks || []).map((bank) => {
    const label = textNode('label', `${bank.label} (MiB)`, 'version-field');
    const input = document.createElement('input');
    Object.assign(input, {type:'number', min:'0', max:'4096', step:'1', value:draft().memoryMiB[bank.id]});
    input.addEventListener('change', (event) => {
      event.stopPropagation();
      if (input.value === '' || !input.validity.valid) { fail('Enter a whole memory size between 0 and 4096 MiB.'); return; }
      draft().memoryMiB[bank.id] = Number(input.value);
      updatePlan();
    });
    label.append(input); return label;
  }));
}

function renderOptions() {
  const machine = catalog.machines.find((item) => item.id === machineID());
  for (const section of sections) {
    const previous = new Set(draft()[section]);
    const target = element(section);
    target.replaceChildren();
    const options = catalog[section].filter((item) => section === 'devices' ? (machine.deviceIds || []).includes(item.id) : !item.machineIds?.length || item.machineIds.includes(machine.id));
    for (const option of options) {
      const label = document.createElement('label');
      label.className = 'option';
      const input = document.createElement('input');
      Object.assign(input, { type: 'checkbox', name: section, value: option.id, checked: Boolean(option.required) || previous.has(option.id), disabled: Boolean(option.required) || option.status === 'planned' });
      const copy = textNode('span', '', 'option-copy');
      const title = textNode('span', '', 'option-title');
      title.append(textNode('strong', option.label), textNode('span', option.required ? 'Required · built in' : option.status, 'badge'));
      copy.append(title, textNode('p', option.description));
      label.append(input, copy);
      target.append(label);
    }
    if (!options.length) target.append(textNode('p', 'No options defined for this profile yet.', 'muted'));
  }
}

function draft() {
  const id = machineID();
  if (!drafts.has(id)) {
    const machine = catalog.machines.find((item) => item.id === id);
    drafts.set(id, {instances: [], devices:[...(machine.defaultDevices || [])], packages:[], desktop:'none', boot:{login:'console', defaultSession:'console', animation:false}, memoryMiB:Object.fromEntries((machine.memoryBanks || []).map((bank) => [bank.id, bank.defaultMiB]))});
  }
  return drafts.get(id);
}

function mediaBindings(media, prefix) {
  return Object.fromEntries(Object.entries(media).map(([role, files]) => [role, files.map((_, index) => `${prefix}/${role}/${index}`)]));
}

function mediaField(requirement, media, owner) {
  const field = textNode('div', '', 'media-field');
  const label = textNode('label', requirement.label);
  const input = document.createElement('input');
  input.type = 'file';
  input.multiple = Boolean(requirement.multiple);
  input.id = `${owner}-${requirement.id}`;
  label.htmlFor = input.id;
  const status = textNode('span', '', 'media-status');
  status.setAttribute('aria-live', 'polite');
  const clear = textNode('button', 'Clear media', 'text-button');
  clear.type = 'button';
  clear.setAttribute('aria-label', `Clear ${requirement.label} for ${owner}`);
  const refresh = () => {
    const files = media[requirement.id] || [];
    status.textContent = files.length ? `Selected: ${files.map((file) => `${file.name} (${(file.size / 1048576).toFixed(2)} MiB)`).join(', ')}` : 'Media needed before image generation.';
    clear.hidden = files.length === 0;
  };
  input.addEventListener('change', (event) => {
    event.stopPropagation();
    if (input.files.length) media[requirement.id] = [...input.files];
    refresh();
    updatePlan();
  });
  clear.addEventListener('click', () => {
    delete media[requirement.id];
    input.value = '';
    refresh();
    updatePlan();
  });
  refresh();
  field.append(label, input, status, clear);
  return field;
}

function guestRequirements(instance, profile) {
  const environment = profile.environments.find((item) => item.id === instance.version);
  const requirements = [...(environment?.mediaRequirements || [])];
  if (profile.id === 'macenv' && instance.romMode !== 'host') requirements.push({id:'rom', label:instance.romMode === 'override' ? 'Macintosh ROM override' : 'Macintosh ROM'});
  return requirements;
}

function instanceCard(instance) {
  const profile = catalog.containers.find((item) => item.id === instance.profile);
  const machine = catalog.machines.find((item) => item.id === machineID());
  const card = textNode('section', '', 'container-instance');
  card.setAttribute('aria-label', `${profile.label} ${instance.id}`);
  const heading = textNode('div', '', 'instance-heading');
  const remove = textNode('button', 'Remove', 'remove-container');
  remove.type = 'button';
  remove.setAttribute('aria-label', `Remove ${profile.label} ${instance.id}`);
  remove.addEventListener('click', () => {
    draft().instances = draft().instances.filter((item) => item.id !== instance.id);
    renderContainers();
    element('container-types').querySelector('button:not(:disabled)')?.focus();
    updatePlan();
  });
  heading.append(textNode('h3', `${profile.label} · ${instance.id}`), remove);
  const versionLabel = textNode('label', 'OS environment', 'version-field');
  const version = document.createElement('select');
  for (const env of profile.environments || []) { const option = textNode('option', env.label); option.value = env.id; version.append(option); }
  version.value = instance.version;
  const fields = textNode('div', '', 'media-fields');
  const renderFields = () => {
    fields.replaceChildren(...guestRequirements(instance, profile).map((requirement) => mediaField(requirement, instance.media, instance.id)));
    if (instance.version === 'system6') fields.prepend(textNode('p', 'System 6 support is planned: 24-bit mode, ROM compatibility and A/UX 2.x extraction still need validation.', 'rom-warning'));
  };
  version.addEventListener('change', (event) => {
    event.stopPropagation();
    instance.version = version.value;
    delete instance.media.system;
    renderFields();
    renderSharedMedia();
    renderStartup();
    updatePlan();
  });
  versionLabel.append(version);
  card.append(heading, versionLabel, textNode('p', 'Available recipes are experimental. Selected media still require validation.', 'instance-help'));
  if (profile.id === 'macenv') {
    card.append(textNode('p', 'Uses shared A/UX support media below the guest list.', 'instance-help'));
    if (machine.family === 'Macintosh') {
      const romLabel = textNode('label', 'ROM source', 'version-field');
      const select = document.createElement('select');
      for (const [value, label] of [['host', `Use ${machine.model} host ROM (recommended)`], ['override', 'Override host ROM with a file']]) {
        const option = textNode('option', label); option.value = value; select.append(option);
      }
      select.value = instance.romMode;
      const warning = textNode('p', 'A ROM override may require extra compatibility handling and slow the guest down. The chosen ROM must match the guest environment.', 'rom-warning');
      warning.hidden = instance.romMode !== 'override';
      select.addEventListener('change', (event) => {
        event.stopPropagation();
        instance.romMode = select.value;
        if (instance.romMode === 'host') delete instance.media.rom;
        warning.hidden = instance.romMode !== 'override';
        renderFields(); updatePlan();
      });
      romLabel.append(select); card.append(romLabel, warning);
    } else card.append(textNode('p', 'This host needs a compatible Macintosh ROM file for each Mac environment.', 'instance-help'));
  }
  const budgetLabel = textNode('label', 'Guest RAM budget (MiB)', 'version-field');
  const budget = document.createElement('input');
  Object.assign(budget, {type:'number', min:profile.id==='tosenv'?'1':'8', max:profile.id==='tosenv'?'14':'512', step:'1', value:instance.memoryMiB});
  budget.dataset.guestBudget = instance.id;
  budget.addEventListener('change', (event) => { event.stopPropagation(); instance.memoryMiB = Number(budget.value); updatePlan(); });
  budgetLabel.append(budget);
  card.append(budgetLabel, textNode('p', 'Planned per-guest allocation. The estimate includes every configured guest running at once; runtime enforcement is pending.', 'instance-help'));
  renderFields(); card.append(fields);
  return card;
}

function renderContainers() {
  const types = element('container-types');
  types.replaceChildren();
  const profiles = catalog.containers.filter((item) => item.machineIds.includes(machineID()));
  for (const profile of profiles) {
    const add = textNode('button', `+ Add ${profile.label}${profile.status === 'planned' ? ' (planned)' : ''}`, 'add-container');
    add.type = 'button';
    add.disabled = profile.status === 'planned';
    add.addEventListener('click', () => {
      const instance = { id: `guest-${++instanceID}`, profile: profile.id, version: profile.environments[0].id, memoryMiB:profile.id==='tosenv'?14:32, romMode: profile.id === 'macenv' ? (catalog.machines.find((item) => item.id === machineID()).family === 'Macintosh' ? 'host' : 'file') : '', media: {} };
      draft().instances.push(instance);
      renderContainers();
      element('containers').lastElementChild.querySelector('select').focus();
      updatePlan();
    });
    types.append(add);
  }
  const cards = draft().instances.map(instanceCard);
  element('containers').replaceChildren(...cards);
  renderSharedMedia();
  renderStartup();
  if (!cards.length) element('containers').append(textNode('p', profiles.some((item) => item.status !== 'planned') ? 'No guest environments added. Add the same OS again for another version.' : 'Guest environments are not available for this machine yet.', 'muted'));
}

function renderMedia() {
  const machine = catalog.machines.find((item) => item.id === machineID());
  element('base-media').replaceChildren(...(machine.mediaRequirements || []).filter((requirement) => !['aux','aux2'].includes(requirement.id)).map((requirement) => mediaField(requirement, sharedMedia, 'base')));
  if (!machine.mediaRequirements?.length) element('base-media').append(textNode('p', 'Media requirements will appear when this machine has an image recipe.', 'muted'));
}

function renderSharedMedia() {
  const guests = draft().instances.filter((item) => item.profile === 'macenv');
  const roles = new Set(guests.map((item) => item.version === 'system6' ? 'aux2' : 'aux'));
  const machine = catalog.machines.find((item) => item.id === machineID());
  element('shared-guest-section').hidden = !guests.length;
  element('shared-guest-media').replaceChildren(...(machine.mediaRequirements || []).filter((item) => roles.has(item.id)).map((item) => mediaField(item, sharedMedia, 'shared')));
}

function renderStartup() {
  const state = draft();
  const hasX = state.packages.includes('x11');
  if (!hasX) { state.desktop='none'; state.boot.login='console'; }
  element('desktop').disabled = !hasX;
  element('desktop').value = state.desktop;
  const canGraphical = hasX && state.desktop !== 'none';
  element('login').querySelector('option[value="xdm"]').disabled = !canGraphical;
  if (!canGraphical) state.boot.login='console';
  element('login').value = state.boot.login;
  const sessions = state.boot.login === 'xdm' ? [{id:`desktop:${state.desktop}`,label:`X desktop · ${state.desktop}`}, ...state.instances.map((guest) => {
    const profile=catalog.containers.find((item) => item.id===guest.profile);
    const environment=profile.environments.find((item) => item.id===guest.version);
    return {id:`guest:${guest.id}`,label:`${environment.label} · ${guest.id}`};
  })] : [{id:'console',label:'Console'}];
  if (!sessions.some((item) => item.id === state.boot.defaultSession)) state.boot.defaultSession=sessions[0].id;
  element('default-session').replaceChildren(...sessions.map((item) => { const option=textNode('option',item.label);option.value=item.id;return option; }));
  element('default-session').value=state.boot.defaultSession;
  element('default-session').disabled=state.boot.login !== 'xdm';
  element('boot-animation').checked=state.boot.animation;
}

function fillList(id, values) {
  element(id).replaceChildren(...values.map((value) => textNode('li', value)));
}

async function updatePlan() {
  const revision = ++planRevision;
  element('generate-image').disabled = true;
  manifest = null;
  if ([...document.querySelectorAll('#memory input, input[data-guest-budget]')].some((input) => input.value === '' || !input.validity.valid)) {
    fail('Enter a valid whole-number memory size within the field limits.');
    return;
  }
  const selection = {
    machine: machineID(),
    memoryMiB: {...draft().memoryMiB},
    desktop:draft().desktop, boot:{...draft().boot},
    baseMedia: mediaBindings(Object.fromEntries((catalog.machines.find((item) => item.id === machineID()).mediaRequirements || []).filter((requirement) => sharedMedia[requirement.id]).map((requirement) => [requirement.id, sharedMedia[requirement.id]])), 'base'),
    containerInstances: draft().instances.map((instance) => ({
      id: instance.id, profile: instance.profile, version: instance.version, memoryMiB:instance.memoryMiB, romMode: instance.romMode,
      media: mediaBindings(instance.media, instance.id),
    })),
  };
  element('summary-title').textContent = catalog.machines.find((item) => item.id === selection.machine).label;
  for (const section of sections) selection[section] = [...document.querySelectorAll(`input[name="${section}"]:checked`)].map((input) => input.value);
  try {
    const response = await request({ action: 'plan', selection });
    if (revision !== planRevision) return;
    manifest = response.manifest;
    element('generate-image').disabled = !manifest.imageBuildSupported;
    element('assembly-status').textContent = manifest.imageBuildSupported
      ? 'Continue to the Quadra builder to supply local media and a prebuilt kernel, then download your disk image.'
      : 'This selection needs recipe support. The Quadra console builder below supports the default Quadra devices without guests, packages or a desktop.';
    element('runtime-status').textContent = 'Configuration checked locally';
    element('memory-summary').textContent = Object.entries(manifest.selection.memoryMiB).map(([id, value]) => `${catalog.machines.find((item) => item.id === machineID()).memoryBanks.find((bank) => bank.id === id).label}: ${value} MiB`).join(' · ');
    const estimate=manifest.memoryEstimate;
    element('memory-budget').textContent=`${estimate.totalMiB} MiB estimated / ${estimate.physicalMiB} MiB installed · ${estimate.remainingMiB >= 0 ? `${estimate.remainingMiB} MiB remaining` : `${-estimate.remainingMiB} MiB over budget`}`;
    element('memory-warnings').replaceChildren(...estimate.warnings.map((warning) => textNode('p',warning)));
    fillList('builtins', manifest.kernel.builtIn);
    fillList('modules', manifest.kernel.modules.length ? manifest.kernel.modules : ['None selected']);
    element('builtin-count').textContent = manifest.kernel.builtIn.length;
    element('module-count').textContent = manifest.kernel.modules.length;
    fillList('required-inputs', manifest.requiredInputs);
    fillList('guest-summary', draft().instances.length ? draft().instances.map((instance) => {
      const profile = catalog.containers.find((item) => item.id === instance.profile);
      const missing = guestRequirements(instance, profile).filter((item) => !instance.media[item.id]?.length).length;
      const env = profile.environments.find((item) => item.id === instance.version);
      return `${profile.label}: ${env.label} · ${missing ? `${missing} media slots empty` : guestRequirements(instance, profile).length ? 'media selected, unverified' : 'uses shared base media'}`;
    }) : ['No guest environments selected']);
    fillList('build-steps', manifest.steps);
    element('warnings').replaceChildren(...manifest.warnings.map((warning) => textNode('p', warning)));
    element('plan-content').hidden = false;
  } catch (error) {
    if (revision === planRevision) fail(error.message);
  }
}

let presets = [];
let activePreset = null;

function applySelection(preset, settings, recipe = '') {
  activePreset = preset;
  selectMachine(preset.machine);
  draft().devices = [...settings.devices];
  renderOptions();
  updatePlan();
  window.setQuadraSettings?.({ preset: preset.id, rootMiB: settings.rootMiB, swapMiB: settings.swapMiB, recipe });
  renderPresets();
}

function renderPresets() {
  element('presets').replaceChildren(...presets.map((preset) => {
    const card = textNode('div', '', 'family-card');
    card.classList.toggle('selected', activePreset?.id === preset.id);
    card.append(textNode('h3', preset.label), textNode('p', preset.description), textNode('span', preset.runnable ? 'runnable' : 'not yet runnable', 'badge'));
    const button = textNode('button', preset.status === 'planned' ? 'Planned' : `Use ${preset.label}`, 'configure-button');
    button.type = 'button';
    button.disabled = preset.status === 'planned';
    button.addEventListener('click', () => {
      applySelection(preset, preset.settings);
      element('preset-status').textContent = `${preset.label} preset applied.`;
    });
    card.append(button);
    return card;
  }));
}

window.forgeDevices = (machine) => [...(drafts.get(machine)?.devices ?? catalog.machines.find((item) => item.id === machine).defaultDevices)].sort();

element('recipe-import').addEventListener('change', async (event) => {
  event.stopPropagation();
  const file = event.target.files[0];
  event.target.value = '';
  if (!file) return;
  const status = element('preset-status');
  try {
    if (file.size > 1048576) throw new Error('recipe exceeds 1 MiB');
    const text = await file.text();
    const result = await request({ action: 'import', recipe: text }, true);
    const preset = presets.find((item) => item.id === result.selection.preset);
    applySelection(preset, result.selection.settings, text);
    const notes = [`Recipe imported: ${preset.label}.`];
    if (result.selection.provision?.length) notes.push('Its packages must be supplied with the native builder.');
    if (result.blocked) notes.push(result.blocked);
    if (result.warning) notes.push(result.warning);
    status.textContent = notes.join(' ');
  } catch (error) {
    status.textContent = `Recipe not imported: ${error.message}`;
  }
});

element('generate-image').addEventListener('click', () => { if (manifest?.imageBuildSupported) window.openQuadraBuilder(); });

element('configuration').addEventListener('submit', (event) => event.preventDefault());
element('configuration').addEventListener('change', (event) => {
  if (!catalog) return;
  if (sections.includes(event.target.name)) draft()[event.target.name] = [...document.querySelectorAll(`input[name="${event.target.name}"]:checked`)].map((input) => input.value);
  renderStartup();
  updatePlan();
});
element('model').addEventListener('change', (event) => {
  event.stopPropagation();
  const current = catalog.machines.find((item) => item.id === machineID());
  selectMachine(catalog.machines.find((item) => item.family === current.family && item.model === event.target.value).id);
});
element('variant').addEventListener('change', (event) => { event.stopPropagation(); selectMachine(event.target.value); });

for (const id of ['desktop','login','default-session','boot-animation']) {
  element(id).addEventListener('change', (event) => {
    event.stopPropagation();
    const state=draft();
    if(id==='desktop') state.desktop=event.target.value;
    if(id==='login') state.boot.login=event.target.value;
    if(id==='default-session') state.boot.defaultSession=event.target.value;
    if(id==='boot-animation') state.boot.animation=event.target.checked;
    renderStartup(); updatePlan();
  });
}

(async () => {
  try {
    worker = new Worker('./worker.js');
    worker.onerror = () => fail('Could not start the local planner. Serve the built site over HTTP and reload.');
    worker.onmessage = ({ data }) => {
      if (data.type === 'fatal') {
        for (const item of pending.values()) {
          clearTimeout(item.timeout);
          item.reject(new Error(data.error));
        }
        pending.clear();
        fail(`Planner unavailable: ${data.error}`);
        return;
      }
      const item = pending.get(data.id);
      if (!item) return;
      clearTimeout(item.timeout);
      pending.delete(data.id);
      if (data.ok) item.resolve(data);
      else item.reject(new Error(data.error || 'The configuration could not be planned.'));
    };
    catalog = (await request({ action: 'catalog' })).catalog;
    presets = (await request({ action: 'presets' }, true)).presets;
    renderPresets();
    renderMachines();
    renderHardware();
    renderOptions();
    renderContainers();
    renderMedia();
    await updatePlan();
  } catch (error) {
    element('machines').replaceChildren(textNode('p', 'Build and serve the webapp to load machine profiles.', 'muted'));
    fail(`Planner unavailable: ${error.message}`);
  }
})();
