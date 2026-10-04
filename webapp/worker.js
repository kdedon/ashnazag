/* global Go, auxForge, auxPlanner */
'use strict';
let ready;
const started = new Promise((resolve) => { ready = resolve; });
globalThis.auxPlannerReady = ready;
const initialization = (async () => {
  importScripts('./wasm_exec.js');
  const go = new Go();
  const response = await fetch('./planner.wasm');
  if (!response.ok) throw new Error(`Planner download failed (${response.status}).`);
  const result = await WebAssembly.instantiate(await response.arrayBuffer(), go.importObject);
  go.run(result.instance).catch((error) => {
    postMessage({ type: 'fatal', error: error.message });
  });
  await started;
})();
initialization.catch((error) => postMessage({ type: 'fatal', error: error.message }));
self.onmessage = async ({ data }) => {
  try {
    await initialization;
    postMessage({ id: data.id, ...JSON.parse((data.forge ? auxForge : auxPlanner)(JSON.stringify(data.request))) });
  } catch (error) {
    postMessage({ id: data.id, ok: false, error: error.message });
  }
};
