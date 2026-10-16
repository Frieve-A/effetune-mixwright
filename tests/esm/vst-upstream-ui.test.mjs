import assert from 'node:assert/strict';
import { readFile, readdir } from 'node:fs/promises';
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
      updatePipelineUI() {}, updateAllPluginDisplayState() {}
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

test('Visualizer add-item choices omit Lyrics while retaining every other type', async () => {
  const { VisualizerEditor } = await assetModule('js/visualizer/visualizer-editor.js');
  const { ITEM_TYPES, createDefaultLayout } = await assetModule('js/visualizer/visualizer-model.js');
  const previousDocument = globalThis.document;
  const reachedChoices = new Error('Reached add-item choices');
  let choices;
  globalThis.document = {
    createElement: () => ({
      children: [], classList: { add() {} },
      replaceChildren() { this.children = []; },
      appendChild(child) { this.children.push(child); return child; }
    })
  };
  try {
    const editor = {
      navigationContent: document.createElement('div'), root: document.createElement('div'),
      view: { layout: createDefaultLayout() }, t: key => key,
      group: () => document.createElement('div'), button() {},
      field(_parent, label, _kind, _value, _action, options) {
        if (label !== 'visualizer.item') return;
        choices = options.values.map(([type]) => type);
        throw reachedChoices;
      }
    };
    assert.throws(() => VisualizerEditor.prototype.render.call(editor), error => error === reachedChoices);
    assert.ok(ITEM_TYPES.includes('lyrics'), 'Lyrics remains a supported saved item type');
    assert.deepEqual(choices.toSorted(), ITEM_TYPES.filter(type => type !== 'lyrics').toSorted());
  } finally { globalThis.document = previousDocument; }
});

test('bundled Visualizer system presets omit only Lyric Stage across all aspects', async () => {
  const sourceDirectory = new URL('../../external/effetune/presets/visualizer/', import.meta.url);
  const { VisualizerPresetStore } = await assetModule('js/visualizer/visualizer-preset-store.js');
  const { ASPECT_FILES } = await assetModule('js/visualizer/visualizer-model.js');
  const store = new VisualizerPresetStore({ fetch: async url => ({
    ok: true, json: async () => JSON.parse(await readFile(url, 'utf8'))
  }) });
  const bundled = await store.loadSystemPresets();
  const files = (await readdir(sourceDirectory)).filter(file => file.endsWith('.json')).sort();
  assert.deepEqual((await readdir(path.join(assets, 'presets/visualizer')))
    .filter(file => file.endsWith('.json')).sort(), files);
  for (const file of files) {
    const expected = JSON.parse(await readFile(new URL(file, sourceDirectory), 'utf8'));
    delete expected['Lyric Stage'];
    const actual = JSON.parse(await assetText(`presets/visualizer/${file}`));
    assert.deepEqual(actual, expected, `${file} retains all other presets, including Cinema Strip`);
    const aspect = Object.keys(ASPECT_FILES).find(aspect => ASPECT_FILES[aspect] === file);
    assert.deepEqual(bundled[aspect], expected);
  }
});

test('existing Lyrics layouts survive normalization, sharing and same-name user preset storage', async () => {
  const { normalizeLayout, snapshotLayout, encodeLayoutShare, decodeLayoutShare } =
    await assetModule('js/visualizer/visualizer-model.js');
  const { VisualizerPresetStore } = await assetModule('js/visualizer/visualizer-preset-store.js');
  const upstream = JSON.parse(await readFile(new URL(
    '../../external/effetune/presets/visualizer/16x9.json', import.meta.url), 'utf8'));
  const original = upstream['Lyric Stage'];
  const lyrics = original.items.filter(item => item.type === 'lyrics');
  assert.ok(lyrics.length > 0);
  const normalized = normalizeLayout(original);
  assert.deepEqual(normalized.items.filter(item => item.type === 'lyrics'), lyrics);
  const snapshot = snapshotLayout(original);
  assert.deepEqual(decodeLayoutShare(encodeLayoutShare(original)), snapshot);
  const saved = new Map();
  const store = new VisualizerPresetStore();
  store.request = async (_name, _mode, action) => action({
    put: (layout, name) => saved.set(name, structuredClone(layout)),
    get: name => saved.get(name)
  });
  await store.saveUserPreset('Lyric Stage', original);
  assert.deepEqual(await store.getUserPreset('Lyric Stage'), snapshot);
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
  const diagnostics = [];
  const App = vm.runInNewContext(`(class { ${methods.join('\n')} })`, {
    console: { error: (...args) => diagnostics.push(args) }
  });
  for (const startupView of ['effects', 'visualizer', 'library', undefined]) {
    const app = new App();
    let opened = 0;
    app.startupConfig = { startupView };
    app.uiManager = { async showVisualizerView() { opened++; } };
    await Promise.all([app.applyStartupViewPreference(), app.applyStartupViewPreference()]);
    assert.equal(opened, startupView === 'visualizer' ? 1 : 0);
  }
  const app = new App();
  const errors = [];
  const failure = new Error('Test navigation failure');
  app.startupConfig = { startupView: 'visualizer' };
  app.uiManager = { async showVisualizerView() { throw failure; },
    setError: (...args) => errors.push(args) };
  await app.applyStartupViewPreference();
  assert.deepEqual(diagnostics, [['Error opening Visualizer startup view:', failure]]);
  assert.deepEqual(errors, [['Visualizer could not be opened. Please try the Visualizer button.', true]]);
});

test('startup navigation waits for native readiness and settles before revealing the view', async () => {
  const appSource = await assetText('js/app.js');
  const uiSource = await assetText('js/ui-manager.js');
  const method = (source, name) => {
    const match = source.match(new RegExp(`    (?:async )?${name}\\([^\\n]*\\) \\{[\\s\\S]*?\\n    \\}`));
    assert.ok(match, `Missing ${name}`);
    return match[0];
  };
  const previous = { window: globalThis.window, document: globalThis.document };
  try {
    for (const [startupView, dspReady, disabled, initiallyReady = false] of [
      ['visualizer', true, false], ['effects', true, false],
      ['visualizer', false, false], ['visualizer', true, true], ['visualizer', true, false, true]
    ]) {
      const document = Object.assign(new Element('document'), {
        documentElement: new Element('html'), body: new Element('body'), hidden: false
      });
      document.documentElement.classList.add('app-starting');
      const pluginsLoaded = Promise.withResolvers();
      const hostInfo = Promise.withResolvers();
      const rebuiltInfo = Promise.withResolvers();
      const viewReady = Promise.withResolvers();
      let hostRequested = false;
      let refreshRequested = false;
      let rebuilds = 0;
      let startupContentHandled = false;
      const openedStatuses = [];
      const window = { location: { search: '' }, audioPreferences: { useWasmDsp: !disabled },
        electronAPI: {}, addEventListener() {}, removeEventListener() {},
        __effetuneHostCall: async (type, payload) => {
          if (type === 'pipeline/rebuild') { rebuilds++; return { ok: true }; }
          if (type !== 'host/getInfo') return { ok: true };
          if (payload?.startup) { hostRequested = true; return hostInfo.promise; }
          refreshRequested = true;
          return rebuiltInfo.promise;
        } };
      Object.assign(globalThis, { window, document });
      const { AudioManager } = await assetModule('vst-audio-manager.js');
      const { VisualizerSources } = await assetModule('js/visualizer/visualizer-sources.js');
      const pipelineA = [{ id: 17 }], pipelineB = [{ id: 42, name: 'DC Offset', enabled: true,
        getParameters: () => ({ of: .25 }),
        getWorkletPluginData(parameters) { return { id: this.id, type: 'DCOffsetPlugin', enabled: true, parameters }; } }];
      const audioManager = new AudioManager();
      clearInterval(audioManager.telemetryTimer);
      Object.assign(audioManager, { pipelineA, pipelineB, currentPipeline: 'B',
        pipelineProcessor: { setPipeline() {}, setMasterBypass() {} },
        applyNativeBypass() {}, applyHostAutomationDeltas() {}
      });
      const sources = new VisualizerSources(audioManager);
      assert.equal(sources.getStatus(), disabled ? 'disabled' : 'unavailable');
      const context = { window, document, console, MIC_DENIED_PREFIX: 'Audio Error: Microphone',
        displayAppVersion: async () => {}, waitForStylesheets: async () => {},
        requestAnimationFrame: callback => queueMicrotask(callback) };
      vm.runInNewContext(`this.App = class { ${['initialize', 'applyStartupViewPreference',
        'openConfiguredStartupView'].map(name => method(appSource, name)).join('\n')} };
        this.Navigation = class { ${method(uiSource, 'showVisualizerView')} };`, context, {
        filename: path.join(assets, 'js/app.js'),
        importModuleDynamically: vm.constants.USE_MAIN_CONTEXT_DEFAULT_LOADER
      });
      const uiManager = Object.assign(new context.Navigation(), {
        updateLoadingProgress() {}, initPluginList() {}, initDragAndDrop() {}, initAudio() {},
        updatePipelineUI() {}, isDoubleBlindActive: () => false, hideLibraryView() {},
        updateViewSwitchButtons() {}, setError: message => assert.fail(message),
        async ensureVisualizerView() {
          openedStatuses.push(sources.getStatus());
          await viewReady.promise;
        },
        visualizerView: { show() { document.body.classList.add('view-visualizer'); }, updateVisibility() {} }
      });
      const app = Object.assign(new context.App(), { startupConfig: { startupView }, audioManager, uiManager,
        pluginManager: { loadPlugins: () => pluginsLoaded.promise },
        initializeAudioWorklet: async () => true,
        initializeAndBuildPipeline: async () => { assert.equal(await audioManager.rebuildPipeline(), ''); return true; },
        setupEventListeners() {}, handleErrors() {}, scheduleDeferredStartupTasks() {},
        hasSavedPipelineState: async () => true,
        maybeStartOnboardingTour: async () => {},
        processCommandLineArguments: async () => { startupContentHandled = true; } });
      window.app = app;
      const initialized = app.initialize();
      assert.deepEqual(openedStatuses, [], 'no navigation while plugin definitions load');
      pluginsLoaded.resolve();
      await until(() => hostRequested);
      assert.deepEqual(openedStatuses, [], 'no navigation before the native startup handshake');
      hostInfo.resolve({ dspReady: initiallyReady, contextGeneration: 7, engineSampleRate: 48000, channels: 2 });
      if (!initiallyReady) {
        await until(() => refreshRequested);
        assert.equal(rebuilds, 1);
        assert.deepEqual(openedStatuses, [], 'no navigation before post-rebuild readiness settles');
        assert.equal(document.documentElement.classList.contains('app-starting'), true);
      }
      rebuiltInfo.resolve({ dspReady, contextGeneration: 7 });
      if (startupView === 'visualizer') {
        await until(() => openedStatuses.length === 1);
        assert.equal(startupContentHandled, true);
        assert.deepEqual(openedStatuses, [disabled ? 'disabled' : dspReady ? 'ready' : 'unavailable']);
        assert.equal(document.documentElement.classList.contains('app-starting'), true);
        assert.notEqual(app.initialized, true);
      }
      viewReady.resolve();
      await initialized;
      await app.applyStartupViewPreference();
      assert.equal(openedStatuses.length, startupView === 'visualizer' ? 1 : 0);
      assert.equal(document.body.classList.contains('view-visualizer'), startupView === 'visualizer');
      assert.equal(document.documentElement.classList.contains('app-starting'), false);
      assert.equal(app.initialized, true);
      assert.equal(audioManager.pipelineA, pipelineA);
      assert.equal(audioManager.pipelineB, pipelineB);
      assert.equal(audioManager.currentPipeline, 'B');
      assert.equal(rebuilds, initiallyReady ? 0 : 1);
      sources.dispose();
      clearTimeout(audioManager.latencyServiceTimer);
      audioManager.nativePort.close();
    }
  } finally { Object.assign(globalThis, previous); }
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

test('bundled settings persist supported choices and restore rejected edits', async () => {
  const bootstrapSource = await assetText('vst-bootstrap.js');
  for (const id of ['frequency-preview-sound', 'sfz-size-limit']) {
    assert.ok(bootstrapSource.includes(`.config-dialog .device-section:has(#${id}),`),
      `${id} must be visible in VST settings`);
  }
  const { showConfigDialog } = await assetModule('js/electron/configIntegration.js');
  const previous = { window: globalThis.window, document: globalThis.document };
  try {
    for (const success of [true, false]) {
      let saved = { theme: 'midnight', startupView: 'effects', spectrumOverlayPeakHold: false,
        spectrumOverlayQuality: 'normal', frequencyPreviewSound: 'sine', sfzMaxSizeMiB: 256, future: 42 };
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
      const sound = document.getElementById('frequency-preview-sound');
      assert.deepEqual(sound.children.map(option => option.value), ['sine', 'bandpassNoise']);
      const sfzLimit = document.getElementById('sfz-size-limit');
      assert.deepEqual(sfzLimit.children.map(option => option.value), ['64', '128', '256', '512', '1024']);
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
        sound.value = 'bandpassNoise';
        await sound.dispatchEvent('change');
        assert.equal(sound.value, success ? 'bandpassNoise' : 'sine');
        sfzLimit.value = '1024';
        await sfzLimit.dispatchEvent('change');
        assert.equal(sfzLimit.value, success ? '1024' : '256');
      } finally { console.error = originalError; }
      assert.equal(theme.value, success ? 'paper' : 'midnight');
      assert.equal(saved.startupView, success ? 'visualizer' : 'effects');
      assert.equal(saved.spectrumOverlayPeakHold, success);
      assert.equal(saved.frequencyPreviewSound, success ? 'bandpassNoise' : 'sine');
      assert.equal(saved.sfzMaxSizeMiB, success ? 1024 : 256);
      assert.equal(saved.future, 42);
      assert.equal(calls.filter(call => call.type === 'config/save').length, 5);
      assert.deepEqual(applied, success ? [['theme', 'paper'], ['spectrum', { quality: 'normal', peakHold: true }]] : []);
      await document.getElementById('close-btn').dispatchEvent('click');
      await showConfigDialog(true, {});
      assert.equal(document.getElementById('theme-select').value, saved.theme);
      assert.equal(document.getElementById('startup-view-visualizer').checked, success);
      assert.equal(document.getElementById('spectrum-overlay-display').value, success ? 'peakHold' : 'instant');
      assert.equal(document.getElementById('frequency-preview-sound').value, saved.frequencyPreviewSound);
      assert.equal(document.getElementById('sfz-size-limit').value, String(saved.sfzMaxSizeMiB));
      await document.getElementById('close-btn').dispatchEvent('click');
    }
  } finally { Object.assign(globalThis, previous); }
});

test('SFZ browser imports files or folders, preserves cancellation and loads a selected instrument', async () => {
  const { NativeSfzLibraryService } = await assetModule('js/sfz/native-service.js');
  const { openSfzLibraryBrowser } = await assetModule('js/sfz/browser.js');
  for (const mode of ['files', 'folder']) for (const cancelled of [true, false]) {
    const banks = ['a', 'b'].map((letter, index) => ({ id: letter.repeat(24), name: `Instrument ${index}`,
      selectedPath: `instrument-${index}.sfz`, importedAt: `2026-10-${14 + index}T00:00:00Z` }));
    const calls = [];
    let registered = [];
    const context = await bootstrap(async serialized => {
      const call = JSON.parse(serialized);
      calls.push(call);
      if (call.type === 'sfz/list') return { ok: true, data: registered };
      assert.equal(call.type, 'sfz/select');
      if (!cancelled) registered = banks;
      return { ok: true, data: cancelled ? null : { banks } };
    });
    const service = new NativeSfzLibraryService(context.window.electronAPI.sfzLibraryV1);
    service.readMetadata = null;
    const loaded = [];
    const previous = { document: globalThis.document, window: globalThis.window };
    Object.assign(globalThis, { document: context.document, window: context.window });
    try {
      const browser = openSfzLibraryBrowser({ service, onLoad: entry => loaded.push(entry.id) });
      await browser.render();
      const button = flatten(browser.element).find(node => node.tagName === 'BUTTON' &&
        node.textContent === (mode === 'files' ? 'Import files…' : 'Import folder…'));
      await button.click();
      const selection = calls.filter(call => call.type === 'sfz/select');
      assert.equal(selection.length, 1);
      assert.deepEqual(selection[0].payload, { mode });
      const rows = flatten(browser.element).filter(node => node.classList.contains('ir-library-entry'));
      assert.equal(rows.length, cancelled ? 0 : 2);
      if (!cancelled) {
        assert.deepEqual((await service.list()).map(entry => entry.importedAt), banks.map(bank => bank.importedAt));
        const sort = flatten(browser.element).find(node => node.tagName === 'SELECT');
        sort.value = 'recent';
        await sort.dispatch('change');
        const recent = byClass(browser.element, 'ir-library-entry');
        assert.equal(flatten(recent).find(node => node.tagName === 'STRONG').textContent, banks[1].name);
        await flatten(recent).find(node => node.tagName === 'BUTTON' && node.textContent === 'Load').click();
        assert.deepEqual(loaded, [banks[1].id]);
      } else assert.deepEqual(loaded, []);
      browser.dispose();
    } finally {
      if (previous.document === undefined) delete globalThis.document; else globalThis.document = previous.document;
      if (previous.window === undefined) delete globalThis.window; else globalThis.window = previous.window;
    }
  }
});

const sfzReadId = 'd'.repeat(24);
const sfzRequest = { id: 'a'.repeat(24), relativePath: 'sample.wav', maxBytes: 1024 * 1024 };
function sfzFileHost(bytes, calls, override = () => undefined) {
  return async serialized => {
    const call = JSON.parse(serialized);
    calls.push(call);
    const custom = override(call);
    if (custom !== undefined) return custom;
    if (call.type === 'sfz/openRead') return { ok: true, data: { readId: sfzReadId, size: bytes.length } };
    if (call.type === 'sfz/readChunk') {
      const { offset, length } = call.payload;
      return { ok: true, data: { base64: Buffer.from(bytes.subarray(offset, offset + length)).toString('base64') } };
    }
    return { ok: true, data: true };
  };
}

test('SFZ reads return exact Uint8Array data across bounded native chunks and close the handle', async () => {
  const bytes = Uint8Array.from({ length: 400000 }, (_, index) => index % 256);
  const calls = [];
  const context = await bootstrap(sfzFileHost(bytes, calls));
  const result = await context.window.electronAPI.sfzLibraryV1.readRelative(sfzRequest);
  assert.equal(result.ok, true);
  assert.ok(result.data instanceof Uint8Array);
  assert.deepEqual(result.data, bytes);
  assert.deepEqual(calls.filter(call => call.type === 'sfz/readChunk').map(call => call.payload), [
    { readId: sfzReadId, offset: 0, length: 196608 },
    { readId: sfzReadId, offset: 196608, length: 196608 },
    { readId: sfzReadId, offset: 393216, length: 6784 }
  ]);
  assert.equal(calls.at(-1).type, 'sfz/closeRead');
});

test('SFZ reads preserve missing and size-limit results and bound malformed/native failures', async () => {
  for (const scenario of ['missing', 'too-large', 'failure', 'malformed', 'thrown', 'invalid-size']) {
    const calls = [];
    const context = await bootstrap(sfzFileHost(new Uint8Array(12), calls, call => {
      if (call.type === 'sfz/openRead') {
        if (scenario === 'missing') return { ok: true, data: null };
        if (scenario === 'too-large') return { ok: false, code: 'too-large', error: 'Private path' };
        if (scenario === 'invalid-size') return { ok: true, data: { readId: sfzReadId, size: -1 } };
      }
      if (call.type === 'sfz/readChunk') {
        if (scenario === 'failure') return { ok: false, code: 'storage-failed', error: 'Private path' };
        if (scenario === 'malformed') return { ok: true, data: { base64: 'AQ==' } };
        if (scenario === 'thrown') throw new Error('Private transport error');
      }
    }));
    const result = await context.window.electronAPI.sfzLibraryV1.readRelative(sfzRequest);
    assert.deepEqual(JSON.parse(JSON.stringify(result)), scenario === 'missing' ? { ok: true, data: null } :
      { ok: false, code: scenario === 'too-large' ? 'too-large' : 'storage-failed' });
    assert.equal(calls.filter(call => call.type === 'sfz/closeRead').length,
      ['missing', 'too-large'].includes(scenario) ? 0 : 1);
  }
});

test('SFZ concurrent reads and probes close each job before opening another and recover after failure', async () => {
  const bytes = Uint8Array.from({ length: 400000 }, (_, index) => index % 256);
  const calls = [];
  let active = 0;
  let maximumActive = 0;
  let opened = 0;
  const context = await bootstrap(sfzFileHost(bytes, calls, call => {
    if (call.type === 'sfz/openRead') {
      active++;
      maximumActive = Math.max(maximumActive, active);
      opened++;
      assert.ok(active <= 16, 'native read-session capacity exceeded');
    }
    if (call.type === 'sfz/readChunk' && opened === 9) {
      return { ok: false, code: 'storage-failed' };
    }
    if (call.type === 'sfz/closeRead') active--;
  }));
  const bridge = context.window.electronAPI.sfzLibraryV1;
  const results = await Promise.all(Array.from({ length: 40 }, (_, index) =>
    index % 2 ? bridge.statRelative(sfzRequest) : bridge.readRelative(sfzRequest)));
  assert.equal(results.filter(result => result.ok).length, 39);
  assert.deepEqual(JSON.parse(JSON.stringify(results[8])), { ok: false, code: 'storage-failed' });
  assert.equal(maximumActive, 1);
  assert.equal(active, 0);
  assert.equal(calls.filter(call => call.type === 'sfz/closeRead').length, 40);
  assert.deepEqual(results.at(-2).data, bytes, 'a failed job must not poison subsequent reads');
});

test('more than sixteen concurrent native SFZ bank preparations share bounded read sessions', async () => {
  const { NativeSfzLibraryService } = await assetModule('js/sfz/native-service.js');
  const banks = Array.from({ length: 20 }, (_, index) => ({
    id: index.toString(16).padStart(24, '0'), name: `Instrument ${index}`, selectedPath: `instrument-${index}.sfz`
  }));
  const definition = new TextEncoder().encode('<region> sample=tone.wav key=60');
  const wav = Buffer.alloc(44 + 16);
  wav.write('RIFF'); wav.writeUInt32LE(wav.length - 8, 4); wav.write('WAVEfmt ', 8);
  wav.writeUInt32LE(16, 16); wav.writeUInt16LE(1, 20); wav.writeUInt16LE(1, 22);
  wav.writeUInt32LE(48000, 24); wav.writeUInt32LE(96000, 28);
  wav.writeUInt16LE(2, 32); wav.writeUInt16LE(16, 34); wav.write('data', 36); wav.writeUInt32LE(16, 40);
  let admitted = null;
  let maximumActive = 0;
  let opened = 0;
  let closed = 0;
  const context = await bootstrap(async serialized => {
    const { type, payload } = JSON.parse(serialized);
    if (type === 'sfz/list') return { ok: true, data: banks };
    if (type === 'sfz/openRead') {
      maximumActive = Math.max(maximumActive, admitted ? 2 : 1);
      assert.equal(admitted, null, 'another bank preparation still owns a read session');
      admitted = payload.relativePath === 'tone.wav' ? wav : definition;
      opened++;
      return { ok: true, data: { readId: sfzReadId, size: admitted.length } };
    }
    if (type === 'sfz/readChunk') {
      const { offset, length } = payload;
      return { ok: true, data: { base64: Buffer.from(admitted.subarray(offset, offset + length)).toString('base64') } };
    }
    if (type === 'sfz/closeRead') {
      admitted = null;
      closed++;
      return { ok: true, data: true };
    }
    throw new Error(`Unexpected fixture operation: ${type}`);
  });
  const service = new NativeSfzLibraryService(context.window.electronAPI.sfzLibraryV1);
  const prepared = await Promise.all(banks.map(bank => service.prepare(bank.id, {
    decode: async (_bytes, sampleRate) => ({ sampleRate, channels: [new Float32Array(8)] })
  })));
  assert.ok(prepared.every(bank => bank.regionCount === 1 && bank.descriptor.payload instanceof ArrayBuffer));
  assert.equal(opened, 60, 'each bank reads its definition, probes its sample, then reads the sample');
  assert.equal(closed, opened);
  assert.equal(maximumActive, 1);
  assert.equal(admitted, null);
});

test('SFZ stat probes upstream WAV headers past JUNK chunks without reading sample data', async () => {
  const bytes = new Uint8Array(300000);
  const view = new DataView(bytes.buffer);
  const ascii = (offset, value) => bytes.set(new TextEncoder().encode(value), offset);
  ascii(0, 'RIFF'); view.setUint32(4, bytes.length - 8, true); ascii(8, 'WAVE');
  ascii(12, 'JUNK'); view.setUint32(16, 200000, true);
  const fmt = 200020;
  ascii(fmt, 'fmt '); view.setUint32(fmt + 4, 16, true); view.setUint16(fmt + 8, 1, true);
  view.setUint16(fmt + 10, 2, true); view.setUint32(fmt + 12, 48000, true);
  view.setUint16(fmt + 20, 4, true); view.setUint16(fmt + 22, 16, true);
  ascii(fmt + 24, 'data'); view.setUint32(fmt + 28, 256 * 4, true);
  const calls = [];
  const context = await bootstrap(sfzFileHost(bytes, calls));
  const result = await context.window.electronAPI.sfzLibraryV1.statRelative(sfzRequest);
  assert.deepEqual(JSON.parse(JSON.stringify(result)), {
    ok: true, data: { size: bytes.length, channels: 2, sampleRate: 48000, frames: 256 }
  });
  assert.equal(calls[0].payload.maxBytes, 1024 * 1024 * 1024);
  assert.ok(calls.filter(call => call.type === 'sfz/readChunk').every(call => call.payload.length <= 16));
  assert.equal(calls.at(-1).type, 'sfz/closeRead');
});

test('native SFZ preparation resolves ARIA bank definitions passed through the bridge', async () => {
  const { NativeSfzLibraryService } = await assetModule('js/sfz/native-service.js');
  const entry = { id: 'e'.repeat(24), name: 'ARIA tone', selectedPath: 'program.sfz',
    bankPaths: ['instrument.bank.xml'], importedAt: '2026-10-15T00:00:00Z' };
  const files = new Map([
    ['program.sfz', new TextEncoder().encode('<region> sample=$tone key=60')],
    ['instrument.bank.xml', new TextEncoder().encode(
      '<AriaBank><AriaElement path="program.sfz"/><Define name="$tone" value="*sine"/></AriaBank>')]
  ]);
  const reads = [];
  let current;
  const context = await bootstrap(async serialized => {
    const { type, payload } = JSON.parse(serialized);
    if (type === 'sfz/list') return { ok: true, data: [entry] };
    if (type === 'sfz/openRead') {
      reads.push(payload.relativePath);
      current = files.get(payload.relativePath);
      return { ok: true, data: current ? { readId: sfzReadId, size: current.length } : null };
    }
    if (type === 'sfz/readChunk') return { ok: true, data: {
      base64: Buffer.from(current.subarray(payload.offset, payload.offset + payload.length)).toString('base64') } };
    if (type === 'sfz/closeRead') return { ok: true };
    throw new Error(`Unexpected ARIA operation ${type}`);
  });
  const service = new NativeSfzLibraryService(context.window.electronAPI.sfzLibraryV1);
  assert.equal((await service.list())[0].importedAt, entry.importedAt);
  const prepared = await service.prepare(entry.id);
  assert.equal(prepared.regionCount, 1);
  assert.ok(prepared.descriptor.payload instanceof ArrayBuffer);
  assert.deepEqual(reads, ['program.sfz', 'instrument.bank.xml']);
});

test('bundled tour teaches DAW workflows, persists first run and supports manual reopening', async () => {
  const { OnboardingTour, TOUR_STEPS, shouldAutoStart } = await assetModule('js/ui/onboarding-tour.js');
  const context = await bootstrap(async () => ({ ok: true }));
  const previous = { document: globalThis.document, window: globalThis.window };
  Object.assign(globalThis, { document: context.document, window: context.window });
  Object.assign(context.window, { innerWidth: 1000, innerHeight: 800 });
  context.document.createElement = tag => Object.assign(new Element(tag), {
    style: {}, getBoundingClientRect: () => ({ width: 300, height: 200 })
  });
  const targets = new Map(TOUR_STEPS.filter(step => step.target).map(step => [step.target,
    Object.assign(new Element('button'), { scrollIntoView() {}, getClientRects: () => [] })]));
  context.document.querySelector = selector => targets.get(selector);
  const locale = vm.runInNewContext(`(${await assetText('js/locales/en.json5')})`);
  const saved = [];
  const manager = { layoutMode: { isMobile: false, onChange() {} },
    pluginListManager: { collapseManager: { isCollapsed: false } },
    t: key => locale[key] || key };
  const tour = new OnboardingTour(manager, { saveConfig: async (_electron, config) => saved.push(config) });
  try {
    assert.equal(shouldAutoStart({ initialized: true }), true);
    assert.equal(shouldAutoStart({ initialized: true, hasSavedPipelineState: true }), false);
    tour.start();
    await Promise.resolve();
    assert.equal(saved[0].onboardingTourShown, true);
    assert.equal(shouldAutoStart({ initialized: true, config: saved[0] }), false);
    for (const [index, step] of TOUR_STEPS.entries()) {
      tour.showStep(index);
      assert.equal(tour.root.attributes.get('data-step'), step.id);
      assert.ok(!/Open music|Music Library|Audio Configuration|What's this app/.test(tour.body.textContent));
      if (step.id === 'sound') {
        assert.equal(step.target, '#pipeline');
        assert.match(tour.body.textContent, /DAW/);
      }
      if (step.id === 'views') assert.equal(step.target, '#visualizerButton');
      if (step.id === 'help') assert.match(tour.body.textContent, /Settings.*Help/);
    }
    tour.end(true);
    const source = await assetText('js/ui-manager.js');
    const manual = source.match(/    async startOnboardingTour\(\) \{[\s\S]*?\n    \}/)[0];
    const Navigation = vm.runInNewContext(`(class { ${manual} })`);
    const navigation = Object.assign(new Navigation(), { localizationReady: Promise.resolve(),
      showEffectPipelineView: async () => true, onboardingTour: tour });
    assert.equal(await navigation.startOnboardingTour(), true);
    assert.equal(tour.isActive, true);
    assert.equal(tour.stepIndex, 0);
  } finally {
    tour.end(true);
    if (previous.document === undefined) delete globalThis.document; else globalThis.document = previous.document;
    if (previous.window === undefined) delete globalThis.window; else globalThis.window = previous.window;
  }
});

test('SFZ stat uses upstream AIFF and FLAC metadata and handles short or missing files', async () => {
  // Match the compact header fixtures used by the upstream native SFZ tests.
  const flac = Buffer.alloc(42);
  flac.write('fLaC'); flac[4] = 0x80; flac[7] = 34;
  flac.writeBigUInt64BE((48000n << 44n) | (1n << 41n) | 96000n, 18);
  const aiff = Buffer.alloc(38);
  aiff.write('FORM'); aiff.write('AIFF', 8); aiff.write('COMM', 12);
  aiff.writeUInt32BE(18, 16); aiff.writeUInt16BE(1, 20); aiff.writeUInt32BE(128, 22);
  aiff.writeUInt16BE(16, 26); aiff.writeUInt16BE(16398, 28); aiff.writeUInt32BE(0xac440000, 30);
  for (const [bytes, metadata] of [
    [flac, { channels: 2, frames: 96000, sampleRate: 48000 }],
    [aiff, { channels: 1, frames: 128, sampleRate: 44100 }],
    [new Uint8Array(5), {}]
  ]) {
    const calls = [];
    const context = await bootstrap(sfzFileHost(bytes, calls));
    const result = await context.window.electronAPI.sfzLibraryV1.statRelative(sfzRequest);
    assert.deepEqual(JSON.parse(JSON.stringify(result)), { ok: true, data: { size: bytes.length, ...metadata } });
    assert.equal(calls.at(-1).type, 'sfz/closeRead');
  }
  const context = await bootstrap(async () => ({ ok: true, data: null }));
  assert.deepEqual(JSON.parse(JSON.stringify(await context.window.electronAPI.sfzLibraryV1.statRelative(sfzRequest))),
    { ok: true, data: null });
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
  replaceChildren(...nodes) { this.children = []; this.append(...nodes); }
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
  removeAttribute(name) { this.attributes.delete(name); }
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
    URL, Uint8Array, Blob, DOMException, btoa, atob, console, setTimeout, clearTimeout };
  vm.runInNewContext(await assetText('vst-bootstrap.js'), context, {
    filename: path.join(assets, 'vst-bootstrap.js'),
    importModuleDynamically: vm.constants.USE_MAIN_CONTEXT_DEFAULT_LOADER
  });
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
    const { createDefaultLayout, createItem, normalizeEffect, encodeLayoutShare, decodeLayoutShare } =
      await assetModule('js/visualizer/visualizer-model.js');
    const custom = createItem('analog-meter', 'custom-meter');
    Object.assign(custom.params, { md: 'Custom', de: 'RMS', it: 2, nr: 1000, os: 3, dc: 3 });
    custom.effects = [normalizeEffect({ type: 'edge-fade', shape: 'circle', circleWidth: 0.4 })];
    const layout = createDefaultLayout();
    layout.items = [custom];
    const restored = decodeLayoutShare(encodeLayoutShare(layout));
    assert.equal(restored.items[0].params.md, 'Custom');
    assert.equal(restored.items[0].params.dc, 3);
    assert.equal(restored.items[0].effects[0].circleWidth, 0.4);
    sources.setLayout(restored);
    const meter = messages.filter(message => message.type === 'setVisualizerSources').at(-1).sources[0];
    assert.equal(meter.type, 'AnalogMeterPlugin');
    assert.deepEqual(Array.from(meter.params), [6, 2, 5, 1.5, 1, 1000, 3]);
    restored.items[0].params.dc = 4;
    sources.setLayout(restored);
    assert.equal(messages.filter(message => message.type === 'setVisualizerSources').at(-1).sources[0].tapId, meter.tapId,
      'changing the dial preserves the running native analyzer');
    restored.items[0].params.de = 'True Peak';
    sources.setLayout(restored);
    const changedMeter = messages.filter(message => message.type === 'setVisualizerSources').at(-1).sources[0];
    assert.notEqual(changedMeter.tapId, meter.tapId);
    assert.equal(changedMeter.params[4], 3, 'the selected detector reaches the native parameter block');
    sources.setVisible(false);
    assert.equal(messages.filter(message => message.type === 'setVisualizerSources').at(-1).sources.length, 0);
    sources.dispose();
    assert.equal(audio.telemetryHub.subscribers.size, 0);
  } finally {
    if (previous.document === undefined) delete globalThis.document; else globalThis.document = previous.document;
    if (previous.window === undefined) delete globalThis.window; else globalThis.window = previous.window;
  }
});

test('bundled Pitch Meter scopes telemetry counters and fences to the native source', async () => {
  const { TelemetryHub } = await assetModule('js/audio/telemetry-hub.js');
  const hub = new TelemetryHub();
  const context = vm.createContext({
    window: { dspTelemetryHub: hub },
    PluginBase: class {
      constructor() { this.enabled = true; this._sectionEnabled = true; }
      registerProcessor() {}
    }
  });
  for (const file of ['note_spectrogram', 'pitch_meter']) {
    vm.runInContext(await assetText(`plugins/analyzer/${file}.js`), context);
  }
  const pitch = new context.window.PitchMeterPlugin();
  pitch.id = 42;
  assert.equal(pitch.ensureDspTelemetrySubscription(), true);
  const send = (instanceId, frameIndex, generation, timeSeconds, midi) => {
    hub.setSources({ 42: { instanceId } });
    const packet = new ArrayBuffer(16 + 44);
    const view = new DataView(packet);
    view.setUint16(0, 26, true); view.setUint16(2, 1, true);
    view.setUint32(4, 42, true); view.setUint16(12, 44, true);
    view.setFloat32(16, 48000, true); view.setFloat32(20, timeSeconds, true);
    view.setFloat32(24, 0.01, true); view.setUint32(28, frameIndex, true);
    view.setUint32(32, generation, true); view.setFloat32(36, 440, true);
    view.setFloat32(40, midi, true); view.setFloat32(44, 0, true);
    view.setFloat32(48, 0.9, true); view.setFloat32(52, -12, true);
    view.setUint16(56, 1, true);
    hub.handleMessage({ type: 'dspTelemetry', packet, bytes: packet.byteLength });
    assert.equal(hub.getStats().subscriberErrors, 0);
  };
  send(1, 100, 4, 10, 69);
  assert.equal(pitch.currentNote, 'A4');
  send(2, 0, 4, 0, 72);
  assert.equal(pitch.currentNote, 'C5', 'a replacement can restart frames in the same generation');
  assert.equal(pitch.lastFrameIndex, 0);
  assert.equal(pitch.writeColumn, 1, 'history belongs to the replacement source');
  send(2, 1, 4, 0.01, 74);
  assert.equal(pitch.currentNote, 'D5');
  send(2, 0, 4, 0, 76);
  send(2, 2, 2, 0.02, 76);
  assert.equal(pitch.currentNote, 'D5', 'same-source stale frames and generations stay rejected');
  pitch.beginTelemetryEpoch();
  assert.equal(pitch.generationFence, 4);
  assert.ok(pitch.timeFence > 0);
  send(2, 2, 4, 0, 76);
  assert.equal(pitch.currentNote, '', 'same-source frames cannot bypass the epoch fences');
  send(3, 0, 2, 0, 76);
  assert.equal(pitch.currentNote, 'E5', 'a replacement clears the previous generation and time fences');
  assert.equal(pitch.activeGeneration, 2);
  assert.equal(pitch.lastFrameIndex, 0);
  assert.equal(pitch.writeColumn, 1);
  send(3, 1, 1, 0.01, 77);
  assert.equal(pitch.currentNote, 'E5', 'older generations still fail after replacement');
  pitch.disposeDspTelemetrySubscription();
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
