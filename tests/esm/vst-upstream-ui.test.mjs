import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import path from 'node:path';
import test from 'node:test';
import { pathToFileURL } from 'node:url';
import vm from 'node:vm';
import { createFakeDocument } from '../../external/effetune/tests/helpers/fake-dom.mjs';

const assetsIndex = process.argv.indexOf('--assets');
assert.ok(assetsIndex >= 0, 'Pass --assets <generated WebView directory>');
const assets = path.resolve(process.argv[assetsIndex + 1]);
const assetText = relative => readFile(path.join(assets, relative), 'utf8');
const assetModule = relative => import(pathToFileURL(path.join(assets, relative)).href);

test('shared history snapshots and restoration retain native logical plugin IDs', async () => {
  const { HistoryManager } = await assetModule('js/ui/pipeline/history-manager.js');
  const previousWindow = globalThis.window;
  const previousDocument = globalThis.document;
  globalThis.window = {};
  globalThis.document = { querySelector: () => null };
  try {
    let synchronized = 0;
    const audioManager = {
      pipelineA: [{ id: 42, name: 'Gain', value: 2 }], pipelineB: [], currentPipeline: 'A', workletNode: {},
      dispatchEvent() {}, setMasterBypass() {},
      getCurrentPipeline() { return this.currentPipeline === 'B' ? this.pipelineB : this.pipelineA; },
      synchronizeHistoryState() { synchronized++; }
    };
    const pluginManager = {
      nextPluginId: 1,
      createPlugin(name) {
        return { id: this.nextPluginId++, name, setEnabled(value) { this.enabled = value; },
          setParameters(parameters) { this.value = parameters.value; } };
      }
    };
    const history = new HistoryManager({ audioManager, pluginManager, expandedPlugins: new Set(), core: {
      getSerializablePluginState: plugin => ({ nm: plugin.name, en: true, value: plugin.value }),
      updatePipelineUI() {}
    } });
    const snapshot = history.createSnapshot();
    assert.equal(snapshot.pipelineA[0].id, 42);
    history.history = [snapshot];
    history.historyIndex = 0;
    assert.equal(history.matchesRecordedPluginState(audioManager.pipelineA[0]), true);
    history.loadStateFromHistory();
    assert.equal(audioManager.pipelineA[0].id, 42);
    assert.equal(audioManager.pipelineA[0].value, 2);
    assert.ok(pluginManager.nextPluginId > 42);
    assert.equal(synchronized, 1);
  } finally {
    globalThis.window = previousWindow;
    globalThis.document = previousDocument;
  }
});

test('bundled controller module graph exposes VST app choices and steps presets', async () => {
  const { APP_TARGETS, getAppTarget } = await assetModule('js/midi/app-targets.js');
  const { MidiMappingDialog } = await assetModule('js/midi/midi-mapping-dialog.js');
  const dialog = new MidiMappingDialog({ manager: {}, windowRef: {
    document: { createElement: tag => new Element(tag) }
  } });
  const select = new Element('select');
  const existing = { type: '_global', param: 'playPause', element: 0 };
  dialog.populateParameterSelect(select, '_global', existing);
  assert.deepEqual(select.children.map(option => option.value), ['masterBypass:0', 'abToggle:0', 'preset:0']);
  assert.equal(existing.param, 'playPause');
  assert.equal(getAppTarget(existing.param), APP_TARGETS.playPause);
  const loads = [];
  const presetManager = {
    currentPresetName: 'Alpha',
    async getLoadablePresets() { return { Beta: {}, Alpha: {} }; },
    async loadPreset(name) { loads.push(name); this.currentPresetName = name; }
  };
  APP_TARGETS.preset.run({ pipelineManager: { presetManager } }, 1);
  await until(() => loads.length === 1);
  assert.deepEqual(loads, ['Beta']);
});

test('bundled Visualizer module graph retains viewport menu clamping', async () => {
  await assetModule('js/visualizer/visualizer-editor.js');
  const { clampMenuToViewport } = await assetModule('js/ui/visualizer-shared.js');
  const previousWindow = globalThis.window;
  globalThis.window = { innerWidth: 640, innerHeight: 480 };
  try {
    const menu = { style: { left: '630px', top: '470px' },
      getBoundingClientRect: () => ({ width: 120, height: 90 }) };
    clampMenuToViewport(menu);
    assert.deepEqual(menu.style, { left: '516px', top: '386px' });
    menu.style = { left: '-20px', top: '-10px' };
    clampMenuToViewport(menu);
    assert.deepEqual(menu.style, { left: '4px', top: '4px' });
  } finally { globalThis.window = previousWindow; }
});

test('bundled Visualizer reads transient host metadata without an internal player', async () => {
  const { VisualizerView } = await assetModule('js/visualizer/visualizer-view.js');
  let metadata = null;
  const view = { uiManager: { audioManager: { getNowPlayingMetadata: () => metadata } } };
  assert.equal(VisualizerView.prototype.metadata.call(view), null);
  metadata = { title: '曲名 🎵', album: 'アルバム', artist: '演奏者',
    artwork: [{ src: 'data:image/png;base64,iVBORw0KGgo=' }] };
  assert.equal(VisualizerView.prototype.metadata.call(view), metadata);
  metadata = { title: '', album: 'Album only', artist: '', artwork: [] };
  assert.equal(VisualizerView.prototype.metadata.call(view), metadata, 'partial tags do not require a title');
  metadata = null;
  assert.equal(VisualizerView.prototype.metadata.call(view), null);
});

test('categorized settings select supported UI panels and retain category selection', async () => {
  const source = await assetText('js/electron/configIntegration.js');
  const selectCategory = source.match(/  function selectCategory\(category\) \{[\s\S]*?\n  \}/)?.[0];
  assert.ok(selectCategory);
  const categories = ['general', 'startup', 'display', 'controllers'];
  assert.ok(source.includes(`const categories = ${JSON.stringify(categories).replaceAll('"', "'").replaceAll(',', ', ')};`));
  assert.ok(source.includes('id="config-panel-${category}" hidden>'));
  const unsupported = ['powerSaving', 'offlineOutput', 'remoteControl'];
  const nodes = Object.fromEntries([...categories, ...unsupported].flatMap(category => {
    const panel = new Element('div');
    panel.hidden = true;
    return [[`config-panel-${category}`, panel], [`config-category-${category}`, new Element('button')]];
  }));
  const context = { document: { getElementById: id => nodes[id] } };
  vm.runInNewContext(`const categories = ${JSON.stringify(categories)}; let selectedConfigCategory = 'general';
    ${selectCategory}
    this.select = selectCategory; this.selected = () => selectedConfigCategory;`, context);
  context.select('general');
  assert.equal(nodes['config-panel-general'].hidden, false);
  for (const category of ['startup', 'display']) {
    context.select(category);
    assert.equal(nodes[`config-panel-${category}`].hidden, false);
    assert.equal(nodes['config-panel-general'].hidden, true);
  }
  context.select('controllers');
  assert.equal(nodes['config-panel-general'].hidden, true);
  assert.equal(nodes['config-panel-controllers'].hidden, false);
  assert.equal(context.selected(), 'controllers');
  context.select(context.selected());
  assert.equal(nodes['config-panel-controllers'].hidden, false);
  for (const category of [...unsupported, 'startup', 'display']) assert.equal(nodes[`config-panel-${category}`].hidden, true);
});

test('VST config loads supported startup views and normal PCM quality without losing preferences', async () => {
  for (const startupView of ['effects', 'visualizer', 'library', undefined]) {
    const context = await bootstrap(async () => ({ ok: true,
      config: { startupView, theme: 'paper', spectrumOverlayQuality: 'hq', spectrumOverlayPeakHold: true, future: 42 } }));
    const { config } = await context.window.electronAPI.loadConfig();
    assert.equal(config.startupView, startupView === 'visualizer' ? 'visualizer' : 'effects');
    assert.equal(config.spectrumOverlayQuality, 'normal');
    assert.equal(config.spectrumOverlayPeakHold, true);
    assert.equal(config.theme, 'paper');
    assert.equal(config.future, 42);
  }
});

test('Visualizer startup preference opens once without changing host pipeline state', async () => {
  const source = await assetText('js/app.js');
  const methods = ['applyStartupViewPreference', 'openConfiguredStartupView'].map(name => {
    const match = source.match(new RegExp(`    (?:async )?${name}\\([^\\n]*\\) \\{[\\s\\S]*?\\n    \\}`));
    assert.ok(match);
    return match[0];
  });
  const App = vm.runInNewContext(`(class { ${methods.join('\n')} })`);
  for (const startupView of ['effects', 'visualizer', 'library', undefined]) {
    const app = new App();
    let opened = 0;
    app.startupConfig = { startupView };
    app.uiManager = { async showVisualizerView() { opened++; } };
    await Promise.all([app.applyStartupViewPreference(), app.applyStartupViewPreference()]);
    assert.equal(opened, startupView === 'visualizer' ? 1 : 0);
  }
});

test('a native Spectrum Tap build preserves HQ configuration and reveals the quality control', async () => {
  const context = await bootstrap(async serialized => JSON.parse(serialized).type === 'host/getInfo'
    ? { ok: true, nativeSpectrumTap: true }
    : { ok: true, config: { spectrumOverlayQuality: 'hq', spectrumOverlayPeakHold: true } });
  const { config } = await context.window.electronAPI.loadConfig();
  assert.equal(config.spectrumOverlayQuality, 'hq');
  assert.equal(config.spectrumOverlayPeakHold, true);
  assert.equal(context.window.__effetuneNativeSpectrum, true);
  assert.equal(context.document.documentElement.classList.contains('effetune-vst-native-spectrum'), true);
});

test('bundled settings persist themes, startup view and Peak Hold and restore rejected edits', async () => {
  const { showConfigDialog } = await assetModule('js/electron/configIntegration.js');
  const previous = { window: globalThis.window, document: globalThis.document };
  try {
    for (const success of [true, false]) {
      let saved = { theme: 'midnight', startupView: 'effects', spectrumOverlayPeakHold: false,
        spectrumOverlayQuality: 'normal', future: 42 };
      const calls = [];
      const applied = [];
      const context = await bootstrap(async serialized => {
        const call = JSON.parse(serialized);
        calls.push(call);
        if (call.type === 'config/load') return { ok: true, config: saved };
        if (call.type === 'host/getInfo') return { ok: true, nativeSpectrumTap: false };
        if (success) saved = call.payload.config;
        return { ok: true, success, error: success ? undefined : 'Test save rejection' };
      });
      globalThis.window = context.window;
      globalThis.document = createFakeDocument();
      window.uiManager = { t: key => key, setThemePreference: theme => applied.push(['theme', theme]), setError() {} };
      window.SpectrumOverlay = { setSettings: settings => applied.push(['spectrum', settings]) };
      await showConfigDialog(true, {});
      const theme = document.getElementById('theme-select');
      assert.deepEqual(theme.children.map(option => option.value), ['graphite', 'paper', 'midnight', 'ember', 'mint']);
      const originalError = console.error;
      try {
        if (!success) console.error = () => {};
        theme.value = 'paper';
        await theme.dispatchEvent('change');
        await document.getElementById('startup-view-visualizer').dispatchEvent('change');
        const display = document.getElementById('spectrum-overlay-display');
        display.value = 'peakHold';
        await display.dispatchEvent('change');
        assert.equal(display.value, success ? 'peakHold' : 'instant');
      } finally { console.error = originalError; }
      assert.equal(theme.value, success ? 'paper' : 'midnight');
      assert.equal(saved.startupView, success ? 'visualizer' : 'effects');
      assert.equal(saved.spectrumOverlayPeakHold, success);
      assert.equal(saved.future, 42);
      assert.equal(calls.filter(call => call.type === 'config/save').length, 3);
      assert.deepEqual(applied, success ? [['theme', 'paper'], ['spectrum', { quality: 'normal', peakHold: true }]] : []);
      await document.getElementById('close-btn').dispatchEvent('click');
      await showConfigDialog(true, {});
      assert.equal(document.getElementById('theme-select').value, saved.theme);
      assert.equal(document.getElementById('startup-view-visualizer').checked, success);
      assert.equal(document.getElementById('spectrum-overlay-display').value, success ? 'peakHold' : 'instant');
      await document.getElementById('close-btn').dispatchEvent('click');
    }
  } finally { Object.assign(globalThis, previous); }
});

async function bundledZip() {
  const module = { exports: {} };
  const run = vm.runInThisContext(`(function(module,exports,require){${await assetText('js/vendor/jszip-3.10.2.min.js')}\n})`);
  run(module, module.exports, createRequire(import.meta.url));
  assert.equal(module.exports.version, '3.10.2');
  return module.exports;
}

class Element {
  constructor(tag) {
    this.tagName = tag.toUpperCase();
    this.children = [];
    this.listeners = new Map();
    this.dataset = {};
    this.attributes = new Map();
    this.value = '';
    this._classes = new Set();
    this.classList = {
      add: (...names) => names.forEach(name => this._classes.add(name)),
      remove: (...names) => names.forEach(name => this._classes.delete(name)),
      contains: name => this._classes.has(name),
      toggle: (name, on = !this._classes.has(name)) => {
        if (on) this._classes.add(name); else this._classes.delete(name);
      }
    };
  }
  set className(value) { this._classes = new Set(value.split(/\s+/)); }
  get className() { return [...this._classes].join(' '); }
  set textContent(value) {
    this._text = value;
    if (value === '') this.children = [];
  }
  get textContent() { return this._text || ''; }
  appendChild(node) { node.parentNode = this; this.children.push(node); return node; }
  append(...nodes) { nodes.forEach(node => this.appendChild(node)); }
  remove() {
    if (this.parentNode) this.parentNode.children = this.parentNode.children.filter(node => node !== this);
  }
  contains(node) { return this === node || this.children.some(child => child.contains(node)); }
  addEventListener(type, callback) {
    this.listeners.set(type, [...(this.listeners.get(type) || []), callback]);
  }
  removeEventListener() {}
  async dispatch(type) {
    for (const callback of this.listeners.get(type) || []) {
      await callback({ target: this, currentTarget: this, preventDefault() {} });
    }
  }
  click() { return this.dispatch('click'); }
  setAttribute(name, value) { this.attributes.set(name, String(value)); }
  querySelectorAll() { return []; }
  focus() { globalThis.document.activeElement = this; }
}

const flatten = node => [node, ...node.children.flatMap(flatten)];
const byClass = (node, name) => flatten(node).find(item => item.classList.contains(name));
const until = async predicate => {
  const deadline = Date.now() + 5000;
  while (!predicate()) {
    assert.ok(Date.now() < deadline, 'UI operation did not complete');
    await new Promise(resolve => setTimeout(resolve, 1));
  }
};

async function bootstrap(hostMessage) {
  const document = Object.assign(new Element('document'), {
    documentElement: new Element('html'),
    head: new Element('head'), body: new Element('body'),
    createElement: tag => new Element(tag),
    createTextNode: text => Object.assign(new Element('text'), { textContent: text }),
    getElementById: () => null
  });
  const window = Object.assign(new Element('window'), { vst_hostMessage: hostMessage });
  const context = { document, window, navigator: { platform: 'Win32', userAgent: 'WebView' },
    URL, Uint8Array, Blob, DOMException, btoa, console, setTimeout, clearTimeout };
  vm.runInNewContext(await assetText('vst-bootstrap.js'), context);
  return context;
}

test('bundled ElectronIntegration constructs with the VST subscription surface', async () => {
  const context = await bootstrap(async () => ({ ok: true }));
  const source = (await assetText('js/electron-integration.js'))
    .replace(/^import[\s\S]*?;\r?\n/gm, '').replaceAll('export ', '');
  vm.runInNewContext(`${source}\nthis.integration = new ElectronIntegration();`, context);
  assert.equal(context.integration.isElectron, true);
  assert.equal(typeof context.window.electronAPI.onBackupRestore(() => {}), 'function');
});

test('Effects and Visualizer buttons navigate through shared upstream view transitions', async () => {
  const context = await bootstrap(async () => ({ ok: true }));
  const source = await assetText('js/ui-manager.js');
  const method = name => {
    const match = source.match(new RegExp(`    (?:async )?${name}\\([^\\n]*\\) \\{[\\s\\S]*?\\n    \\}`));
    assert.ok(match, `Missing ${name}`);
    return match[0];
  };
  vm.runInNewContext(`class Navigation { ${['initOpenLibraryButton', 'showVisualizerView',
    'showEffectPipelineView', 'updateViewSwitchButtons'].map(method).join('\n')} }
    this.navigation = new Navigation();`, context);
  const effects = new Element('button');
  const visualizer = new Element('button');
  context.document.getElementById = id => ({ effectPipelineButton: effects, visualizerButton: visualizer })[id];
  let visible = false;
  const navigation = context.navigation;
  Object.assign(navigation, {
    isDoubleBlindActive: () => false,
    async ensureVisualizerView() { return this.visualizerView; },
    hideLibraryView() {},
    visualizerView: { initialized: Promise.resolve(), layout: {},
      show() { visible = true; }, hide() { visible = false; }, updateVisibility() {} }
  });
  navigation.initOpenLibraryButton();
  await visualizer.click();
  assert.equal(visible, true);
  assert.equal(context.document.body.classList.contains('view-visualizer'), true);
  await effects.click();
  assert.equal(visible, false);
  assert.equal(context.document.body.classList.contains('view-visualizer'), false);
  assert.equal(effects.attributes.get('aria-pressed'), 'true');
});

test('VST backup export chunks binary data and cancels incomplete transfers', async () => {
  const calls = [];
  const context = await bootstrap(async serialized => {
    calls.push(JSON.parse(serialized));
    return { ok: true };
  });
  const bytes = Uint8Array.from({ length: 400000 }, (_, index) => index % 256);
  await context.window.__effetuneExportBackup(new Blob([bytes]), 'saved.effetune_backup');
  assert.equal(calls[0].type, 'backup/exportBegin');
  const chunks = calls.filter(call => call.type === 'backup/exportChunk');
  assert.deepEqual(chunks.map(chunk => chunk.payload.offset), [0, 196608, 393216]);
  assert.deepEqual(Buffer.concat(chunks.map(chunk => Buffer.from(chunk.payload.data, 'base64'))), Buffer.from(bytes));
  assert.equal(calls.at(-1).type, 'backup/exportCommit');
  context.window.vst_hostMessage = async serialized => {
    const call = JSON.parse(serialized);
    calls.push(call);
    return { ok: call.type !== 'backup/exportChunk', error: 'Write failed' };
  };
  await assert.rejects(context.window.__effetuneExportBackup(new Blob([bytes]), 'saved.effetune_backup'), /Write failed/);
  assert.equal(calls.at(-1).type, 'backup/exportCancel');
});

test('Visualizer choices match eight channels and preserve an imported wider selection', async () => {
  const source = await assetText('js/visualizer/visualizer-editor.js');
  const start = source.indexOf('            const channels =');
  const end = source.indexOf('\n        }', start);
  assert.ok(start >= 0 && end > start, 'Visualizer channel controls are missing');
  const create = vm.runInNewContext(`(function(item) {
    const properties = {};
    ${source.slice(start, end)}
    return channelSelect;
  })`);
  const editor = { t: (_key, text) => text, changed() {},
    field: (_parent, _label, _kind, value, _action, options) => ({ value,
      options: options.values.map(([value]) => ({ value, disabled: false })) }) };
  const current = create.call(editor, { channel: null });
  assert.deepEqual(JSON.parse(JSON.stringify(current.options.map(option => option.value))),
    ['', 'L', 'R', '34', '56', '78', '1', '2', '3', '4', '5', '6', '7', '8']);
  const imported = { channel: '910' };
  const restored = create.call(editor, imported);
  assert.equal(restored.value, '910');
  assert.equal(restored.options.at(-1).disabled, true);
  assert.equal(imported.channel, '910');
});

test('shared Visualizer sources report readiness, decode host-rate telemetry and clear on hide/restart/dispose', async () => {
  const { AudioManager } = await assetModule('js/audio-manager.js');
  const { TelemetryHub } = await assetModule('js/audio/telemetry-hub.js');
  const { VisualizerSources } = await assetModule('js/visualizer/visualizer-sources.js');
  const context = await bootstrap(async () => ({ ok: true }));
  vm.runInNewContext(await assetText('plugins/analyzer/spectrum_analyzer.js'),
    { ...context, PluginBase: class {}, Float32Array });
  const previous = { document: globalThis.document, window: globalThis.window };
  globalThis.document = context.document;
  globalThis.window = context.window;
  try {
    const messages = [];
    const port = { postMessage: message => messages.push(message) };
    const nativeNode = { port };
    const listeners = new Map();
    const audio = Object.assign(Object.create(AudioManager.prototype), {
      contextManager: { workletNode: nativeNode, audioContext: { sampleRate: 384000 } },
      workletNode: nativeNode, _dspCapabilitiesByNode: new Map([[nativeNode, {}]]),
      telemetryHub: new TelemetryHub({ port }),
      updateDspTelemetryRate() {}, _scheduleVisualSyncUpdate() {},
      addEventListener: (type, callback) => listeners.set(type, callback),
      removeEventListener: type => listeners.delete(type)
    });
    const sources = new VisualizerSources(audio);
    assert.equal(sources.getStatus(), 'ready');
    sources.setLayout({ items: [{ id: 'spectrum', type: 'spectrum', channel: null, params: { pt: 8 } }] });
    sources.setVisible(true);
    let descriptor = messages.filter(message => message.type === 'setVisualizerSources').at(-1).sources[0];
    assert.equal(descriptor.type, 'SpectrumAnalyzerPlugin');
    assert.ok(descriptor.params instanceof Float32Array);
    const packet = new ArrayBuffer(16 + 12 + 129 * 8);
    const view = new DataView(packet);
    view.setUint16(0, 4, true); view.setUint16(2, 1, true);
    view.setUint32(4, descriptor.tapId, true); view.setUint16(12, packet.byteLength - 16, true);
    view.setFloat32(16, 48000, true); view.setUint32(20, 129, true); view.setUint16(24, 8, true);
    for (let index = 0; index < 258; ++index) view.setFloat32(28 + index * 4, -60, true);
    audio.telemetryHub.handleMessage({ type: 'dspTelemetry', packet, bytes: packet.byteLength });
    assert.equal(sources.getFrame('spectrum').sampleRate, 48000);
    context.document.hidden = true;
    await context.document.dispatch('visibilitychange');
    assert.equal(messages.filter(message => message.type === 'setVisualizerSources').at(-1).sources.length, 0);
    assert.equal(sources.getFrame('spectrum'), null);
    context.document.hidden = false;
    await context.document.dispatch('visibilitychange');
    const resumed = messages.filter(message => message.type === 'setVisualizerSources').at(-1).sources[0];
    assert.notEqual(resumed.tapId, descriptor.tapId);
    listeners.get('dspReady')();
    descriptor = messages.filter(message => message.type === 'setVisualizerSources').at(-1).sources[0];
    assert.notEqual(descriptor.tapId, resumed.tapId);
    sources.setVisible(false);
    assert.equal(messages.filter(message => message.type === 'setVisualizerSources').at(-1).sources.length, 0);
    sources.dispose();
    assert.equal(audio.telemetryHub.subscribers.size, 0);
  } finally {
    if (previous.document === undefined) delete globalThis.document; else globalThis.document = previous.document;
    if (previous.window === undefined) delete globalThis.window; else globalThis.window = previous.window;
  }
});

test('bundled JSZip patch preserves cross-realm Uint8Array and ArrayBuffer bytes', async () => {
  const JSZip = await bundledZip();
  const input = vm.runInNewContext(`({
    view: new Uint8Array([99, 0, 127, 255, 77]).subarray(1, 4),
    buffer: new Uint8Array([0, 128, 255, 1]).buffer
  })`);
  const zip = new JSZip();
  zip.file('view.bin', input.view);
  zip.file('buffer.bin', input.buffer);
  const output = await JSZip.loadAsync(await zip.generateAsync({ type: 'uint8array' }));
  assert.deepEqual([...await output.file('view.bin').async('uint8array')], [0, 127, 255]);
  assert.deepEqual([...await output.file('buffer.bin').async('uint8array')], [0, 128, 255, 1]);
});

test('bundled backup dialog creates a real ZIP and restores saved presets through upstream service', async () => {
  const { UserDataBackupService } = await assetModule('js/user-data-backup/service.js');
  const { createUserDataBackupAdapter } = await assetModule('js/user-data-backup/adapters.js');
  const { createDefaultLayout } = await assetModule('js/visualizer/visualizer-model.js');
  const { openUserDataBackupDialog } = await assetModule('js/user-data-backup/dialog.js');
  const JSZip = await bundledZip();
  const loadZip = async () => JSZip;
  const store = (pipeline = {}, layouts = []) => {
    const adapter = createUserDataBackupAdapter({
      presetManager: { readBackupSnapshot: async () => structuredClone(pipeline),
        appendPreset: async (name, data) => { pipeline[name] = structuredClone(data); } },
      pluginPresetStore: { readBackupSnapshot: async () => ({}) },
      visualizerPresetStore: { readBackupSnapshot: async () => structuredClone(layouts),
        appendUserPreset: async (name, layout) => { layouts.push({ name, layout }); } },
      measurementStorage: { readBackupSnapshot: async () => [] },
      irLibrary: { readBackupSnapshot: async () => [] }
    });
    return { pipeline, layouts, service: new UserDataBackupService({ adapter, appVersion: '0.11.1', loadZip }) };
  };
  const context = await bootstrap(async () => ({ ok: true }));
  const previous = { document: globalThis.document, window: globalThis.window };
  globalThis.document = context.document;
  globalThis.window = context.window;
  let output;
  context.window.__effetuneExportBackup = async (blob, fileName) => { output = { blob, fileName }; };
  try {
    const source = store({ Listening: { plugins: [{ nm: 'Volume', vl: -6 }] } },
      [{ name: 'Wide', layout: createDefaultLayout() }]);
    const modal = openUserDataBackupDialog({ service: source.service });
    const button = byClass(modal.element, 'backup-restore-primary');
    await until(() => button.disabled === false);
    await button.click();
    await until(() => !!output && button.disabled === false);
    const bytes = new Uint8Array(await output.blob.arrayBuffer());
    assert.deepEqual([...bytes.subarray(0, 4)], [0x50, 0x4b, 3, 4]);
    modal.close();
    const target = store();
    const restore = openUserDataBackupDialog({ service: target.service });
    await until(() => byClass(restore.element, 'backup-restore-dialog').attributes.get('aria-busy') === 'false');
    const tabs = byClass(restore.element, 'backup-restore-tabs');
    await tabs.children[1].click();
    const input = flatten(restore.element).find(node => node.tagName === 'INPUT' && node.type === 'file');
    input.files = [Object.assign(output.blob, { name: output.fileName })];
    await input.dispatch('change');
    const restoreButton = byClass(restore.element, 'backup-restore-primary');
    await until(() => restoreButton.disabled === false);
    await restoreButton.click();
    await until(() => target.layouts.length === 1 && restoreButton.disabled === false);
    assert.deepEqual(target.pipeline.Listening, source.pipeline.Listening);
    assert.deepEqual(target.layouts[0], source.layouts[0]);
    restore.close();
  } finally {
    if (previous.document === undefined) delete globalThis.document; else globalThis.document = previous.document;
    if (previous.window === undefined) delete globalThis.window; else globalThis.window = previous.window;
  }
});
