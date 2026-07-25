import { access, readFile, readdir } from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

const args = process.argv.slice(2);
const assetsIndex = args.indexOf('--assets');
if (assetsIndex < 0 || !args[assetsIndex + 1]) {
  throw new Error('Usage: node tools/verify-ui-assets.mjs --assets <directory>');
}

const assets = path.resolve(args[assetsIndex + 1]);
const html = await readFile(path.join(assets, 'effetune.html'), 'utf8');
const app = await readFile(path.join(assets, 'js', 'app.js'), 'utf8');
const columns = await readFile(
  path.join(assets, 'js', 'ui', 'pipeline', 'pipeline-column-manager.js'), 'utf8');
const audioAdapter = await readFile(path.join(assets, 'vst-audio-manager.js'), 'utf8');
const bootstrap = await readFile(path.join(assets, 'vst-bootstrap.js'), 'utf8');
const uiManager = await readFile(path.join(assets, 'js', 'ui-manager.js'), 'utf8');
const electronIntegration = await readFile(
  path.join(assets, 'js', 'electron-integration.js'), 'utf8');
const presetIntegration = await readFile(
  path.join(assets, 'js', 'electron', 'presetIntegration.js'), 'utf8');
const pipelineAiDialog = await readFile(
  path.join(assets, 'js', 'ui', 'pipeline', 'pipeline-ai-dialog.js'), 'utf8');
const browserAudioManager = await readFile(path.join(assets, 'js', 'audio-manager.js'), 'utf8');
const pipelineManager = await readFile(path.join(assets, 'js', 'ui', 'pipeline-manager.js'), 'utf8');
const clipboard = await readFile(
  path.join(assets, 'js', 'ui', 'pipeline', 'clipboard-manager.js'), 'utf8');
const history = await readFile(
  path.join(assets, 'js', 'ui', 'pipeline', 'history-manager.js'), 'utf8');
const pluginList = await readFile(path.join(assets, 'plugins', 'plugins.txt'), 'utf8');
const vinylSimulator = await readFile(
  path.join(assets, 'plugins', 'lofi', 'vinyl_simulator.js'), 'utf8');
const dspParams = await readFile(
  path.join(assets, 'js', 'audio', 'dsp-params.generated.js'), 'utf8');
const dspMetadata = await readFile(
  path.join(assets, 'plugins', 'dsp', 'effetune-dsp.meta.json'), 'utf8');
const notices = await readFile(path.join(assets, 'THIRD-PARTY-NOTICES.txt'), 'utf8');
const sourceNotices = await readFile(path.join(projectRoot, 'THIRD-PARTY-NOTICES.txt'), 'utf8');
const measurementStorage = await readFile(
  path.join(assets, 'features', 'measurement', 'dataStorage.js'), 'utf8');
const englishLocale = await readFile(path.join(assets, 'js', 'locales', 'en.json5'), 'utf8');
const japaneseLocale = await readFile(path.join(assets, 'js', 'locales', 'ja.json5'), 'utf8');

const requiredFiles = [
  'vst-bootstrap.js',
  'vst-audio-manager.js',
  'THIRD-PARTY-NOTICES.txt',
  'plugins/plugins.txt',
  'plugins/lofi/vinyl_simulator.css',
  'plugins/lofi/vinyl_simulator.js',
  'presets/presets.txt',
  'features/measurement/dataStorage.js',
  'js/locales/en.json5',
  'js/locales/ja.json5'
];
for (const relative of requiredFiles) {
  await access(path.join(assets, ...relative.split('/')));
}
if (notices !== sourceNotices) {
  throw new Error('The generated UI does not contain the complete third-party notices');
}

const requiredHtml = [
  "default-src 'self' blob: data:",
  '<title>EffeTune Mixwright</title>',
  '<h1>EffeTune Mixwright<img',
  'EffeTune Mixwright version <span id="app-version"></span>',
  '<meta property="og:site_name" content="EffeTune Mixwright">',
  '<meta name="twitter:title" content="EffeTune Mixwright - Real-time Audio Effect Processor">',
  'alt="EffeTune Mixwright Icon"',
  'vst-bootstrap.js',
  'js/app.js'
];
for (const fragment of requiredHtml) {
  if (!html.includes(fragment)) {
    throw new Error(`Generated UI is missing required HTML fragment: ${fragment}`);
  }
}

const forbiddenHtml = [
  'Frieve EffeTune',
  'alt="EffeTune Icon"',
  'EffeTune version <span id="app-version"></span>',
  'effetune-mobile.css',
  'effetune-library.css',
  'manifest.json',
  'googletagmanager.com',
  'google-analytics.com',
  'doubleBlindTestButton',
  'jszip-3.10.1.min.js',
  'jsmediatags-3.9.5.min.js'
];
for (const fragment of forbiddenHtml) {
  if (html.includes(fragment)) {
    throw new Error(`Generated UI still contains excluded feature: ${fragment}`);
  }
}

const productBrandingSources = [
  ['HTML', html],
  ['VST bootstrap', bootstrap],
  ['VST audio adapter', audioAdapter],
  ['Electron integration', electronIntegration],
  ['preset integration', presetIntegration],
  ['pipeline AI dialog', pipelineAiDialog]
];
const localesPath = path.join(assets, 'js', 'locales');
for (const localeFile of await readdir(localesPath)) {
  if (!localeFile.endsWith('.json5')) continue;
  productBrandingSources.push([
    `locale ${localeFile}`,
    await readFile(path.join(localesPath, localeFile), 'utf8')
  ]);
}
for (const [label, source] of productBrandingSources) {
  if (/Frieve EffeTune|EffeTune(?! Mixwright)/.test(source)) {
    throw new Error(`${label} retains legacy product branding`);
  }
}

if (html.indexOf('vst-bootstrap.js') > html.indexOf('js/app.js')) {
  throw new Error('The VST bootstrap must load before the application module');
}
if (!bootstrap.includes('html { background-color: #1e1e1e; }')) {
  throw new Error('The VST document background does not match the dark UI');
}
if (!app.includes("../vst-audio-manager.js")) {
  throw new Error('The application module was not redirected to the VST audio adapter');
}
const hasLegacyVstDualPipelinePatch =
  app.includes('legacy single-pipeline path should force pipeline A');
const hasUpstreamDualPipelineRestore =
  app.includes('let restoredPipelineB = null;') &&
  app.includes('let restoredCurrentPipeline = \'A\';') &&
  app.includes('let restoredDualPipeline = false;') &&
  app.includes('this.audioManager.pipelineB = restoredPipelineB;') &&
  app.includes('this.audioManager.setCurrentPipeline(restoredCurrentPipeline);');
if (!hasLegacyVstDualPipelinePatch && !hasUpstreamDualPipelineRestore) {
  throw new Error('The VST dual-pipeline state restoration patch was not applied');
}
const appIdReservation = app.indexOf('this.pluginManager.nextPluginId = Math.max(');
const appPluginCreation = app.indexOf('this.pluginManager.createPlugin(pluginState.name)');
if (!app.includes('plugin.id = pluginState.id') || appIdReservation < 0 ||
    appPluginCreation < 0 || appIdReservation > appPluginCreation ||
    !app.includes('restoreDoubleBlindTestFromUrl() {}')) {
  throw new Error('Stable restored plug-in IDs or the VST DBT URL exclusion is missing');
}
const historyIdReservation =
  history.indexOf('this.pipelineManager.pluginManager.nextPluginId = Math.max(');
const historyPluginCreation =
  history.indexOf('this.pipelineManager.pluginManager.createPlugin(pluginState.nm)');
const historySynchronizationCalls =
  history.split('void this.audioManager.synchronizeHistoryState();').length - 1;
if (!history.includes('id: plugin.id') ||
    !history.includes('plugin.id = pluginState.id') ||
    historySynchronizationCalls !== 1 ||
    history.includes('this.pipelineManager.core.updateWorkletPlugins();') ||
    historyIdReservation < 0 || historyPluginCreation < 0 ||
    historyIdReservation > historyPluginCreation) {
  throw new Error('Undo/redo history IDs or single native synchronization contract is missing');
}
if (!app.includes('async applyStartupViewPreference() {}') ||
    app.includes('this.uiManager?.showLibraryView?.(') ||
    uiManager.includes('this.toggleLibraryView();') ||
    uiManager.includes('this.initLibraryRecovery();') ||
    uiManager.includes('createWebCatalogRecoveryController')) {
  throw new Error('Music-library startup and keyboard alternate entries remain reachable');
}
if (!pluginList.includes(
      'lofi/vinyl_simulator: Vinyl Simulator | Lo-Fi | VinylSimulatorPlugin | css') ||
    !vinylSimulator.includes('class VinylSimulatorPlugin extends PluginBase') ||
    !dspParams.includes('VinylSimulatorPlugin_PARAMS_HASH') ||
    !dspMetadata.includes('"name": "VinylSimulatorPlugin"')) {
  throw new Error('Vinyl Simulator UI or native DSP metadata is missing');
}
if (!bootstrap.includes('window.electronAPI = {') ||
    html.indexOf('vst-bootstrap.js') > html.indexOf('type="module"') ||
    !bootstrap.includes("/\\.(?:mp3|wav|ogg|flac|opus|m4a|aac|webm|mp4)$/i") ||
    bootstrap.includes("document.addEventListener('paste'") ||
    bootstrap.includes('readClipboardText:') || bootstrap.includes('writeClipboardText:')) {
  throw new Error('Early config API or audio-only file-drop exclusion is missing');
}
if (!bootstrap.includes('const maximumMeasurementImportBytes = 128 * 1024 * 1024;') ||
    !bootstrap.includes("import('./features/measurement/dataStorage.js')") ||
    !bootstrap.includes("input.accept = '.json,application/json';") ||
    !bootstrap.includes('storage.importMeasurementFromJSON(jsonText)') ||
    !bootstrap.includes('storage.deleteMeasurement(measurementId)') ||
    !bootstrap.includes('measurement?.imported !== true') ||
    !bootstrap.includes('confirmImportedMeasurementDeletion(measurement)') ||
    !bootstrap.includes('clearDeletedMeasurementReferences(measurementId)') ||
    !bootstrap.includes("ms: ''") ||
    !bootstrap.includes('targetPlugin.setParameters({') ||
    !bootstrap.includes('window.__effetuneMeasurementImport = {') ||
    !measurementStorage.includes('async importMeasurementFromJSON(jsonString)') ||
    !measurementStorage.includes('data.id = this.generateId();') ||
    !englishLocale.includes('"roomEq.delete.confirm"') ||
    !japaneseLocale.includes('"roomEq.delete.confirm"')) {
  throw new Error(
    'Room EQ measurement import/deletion is missing or can overwrite a stored measurement');
}
if (!bootstrap.includes("const productionUrl = new URL('https://effetune.frieve.com/');") ||
    !bootstrap.includes("url.hostname === 'choc.localhost'") ||
    !bootstrap.includes("url.protocol === 'effetune-mixwright:'") ||
    !bootstrap.includes("url.hostname === 'effetune-mixwright.localhost'") ||
    !bootstrap.includes("url.pathname.replace(/\\.md$/, '.html')") ||
    !bootstrap.includes("const openExternalUrl = url => window.__effetuneHostCall('host/openExternal', {") ||
    !bootstrap.includes('url: normalizeExternalUrl(url)') ||
    !bootstrap.includes('openExternal: openExternalUrl') ||
    !bootstrap.includes('openExternalUrl,') ||
    bootstrap.includes('openExternalUrl: asyncNoop')) {
  throw new Error('External links are not routed through the native host');
}
if (!electronIntegration.includes('patchDocumentationLinks() {}') ||
    electronIntegration.includes("return '/docs/i18n/' + language + '/';")) {
  throw new Error('The VST build still rewrites production documentation links to local files');
}
if (!bootstrap.includes('.subtitle-container,') ||
    !bootstrap.includes('#effectPipelineButton') || !bootstrap.includes('#whatsThisLink') ||
    !bootstrap.includes('<span>Upsampling Factor:</span>') ||
    !bootstrap.includes('<span>Phase:</span>') ||
    !bootstrap.includes('<span>Quality:</span>') ||
    !bootstrap.includes("headerButtons.insertBefore(controls, headerButtons.querySelector('.settings-menu-container'))")) {
  throw new Error('VST-only header controls or hidden single-view entries are missing');
}
if (!bootstrap.includes('.config-dialog .config-dialog-content { display: block !important; }') ||
    !bootstrap.includes('.config-dialog .device-section { display: none !important; }') ||
    !bootstrap.includes('.config-dialog .device-section:has(#language-select) { display: block !important; }') ||
    !bootstrap.includes('.config-dialog .config-dialog-power-column { display: none !important; }')) {
  throw new Error('The VST settings dialog is not restricted to the language setting');
}
if (!bootstrap.includes("helpButton.id = 'helpSettingsButton';") ||
    !bootstrap.includes("[helpButton, 'menu.help.help', 'Help']") ||
    !bootstrap.includes("settingsMenuButton?.addEventListener('click', updateHelpLabels);") ||
    !bootstrap.includes('void openExternalUrl(productionUrl).catch') ||
    !bootstrap.includes('settingsMenu.appendChild(helpButton);')) {
  throw new Error('The VST settings menu Help action is missing or bypasses the native host');
}
if (!bootstrap.includes("const githubUrl = new URL('https://github.com/Frieve-A/effetune-mixwright');") ||
    !bootstrap.includes("githubButton.id = 'githubSettingsButton';") ||
    !bootstrap.includes("githubButton.textContent = 'GitHub';") ||
    !bootstrap.includes('void openExternalUrl(githubUrl).catch') ||
    !bootstrap.includes('settingsMenu.appendChild(githubButton);')) {
  throw new Error('The untranslated VST settings menu GitHub action is missing or bypasses the native host');
}
if (!bootstrap.includes("aboutButton.id = 'aboutSettingsButton';") ||
    !bootstrap.includes("[aboutButton, 'menu.help.about', 'About']") ||
    !bootstrap.includes("window.electronIntegration.showAboutDialog({ version: info.version })") ||
    !bootstrap.includes("description.textContent = 'Multi-Effect Audio Plug-in for DAWs'") ||
    !bootstrap.includes("noticesButton.textContent = 'Third party notices';") ||
    !bootstrap.includes("fetch('THIRD-PARTY-NOTICES.txt')") ||
    !bootstrap.includes("overlay.querySelector('.vst-third-party-notices-content').textContent = notices") ||
    !bootstrap.includes('settingsMenu.appendChild(aboutButton);')) {
  throw new Error('The VST About dialog or complete third-party notices viewer is missing');
}
if (uiManager.includes('double-blind-test/double-blind-test.js') ||
    uiManager.includes("from './ui/audio-player.js'") ||
    uiManager.includes("from './library/") ||
    browserAudioManager.includes("from './audio/offline-processor.js'") ||
    !uiManager.includes('getDoubleBlindTest() { return null; }') ||
    clipboard.includes("searchParams.get('dbt')") ||
    !clipboard.includes("searchParams.get('p')") ||
    !clipboard.includes('JSON.parse(text)') || !html.includes('id="pasteButton"')) {
  throw new Error('DBT alternate entries were not removed without preserving normal paste');
}
if (pipelineManager.includes("from './pipeline/file-processor.js'") ||
    pipelineManager.includes('new FileProcessor(') ||
    pipelineManager.includes('this.fileProcessor.processDroppedAudioFiles(') ||
    !pipelineManager.includes('this.fileProcessor = null;')) {
  throw new Error('The offline file-processing UI remains reachable');
}
if (!audioAdapter.includes('name: plugin.name || logical?.name') ||
    !audioAdapter.includes('bytes: result.bytes') ||
    !audioAdapter.includes("__effetuneHostCall('pipeline/masterBypass'")) {
  throw new Error('The VST display-name or telemetry byte-count contract is missing');
}
for (const fragment of [
  "message.type === 'setPluginAsset'",
  "__effetuneHostCall('pipeline/assetBegin'",
  "__effetuneHostCall('pipeline/assetChunk'",
  "__effetuneHostCall('pipeline/assetCommit'",
  "__effetuneHostCall('pipeline/assetState'",
  'synchronizeNativeAssetMembership()'
]) {
  if (!audioAdapter.includes(fragment)) {
    throw new Error(`The native DSP asset bridge is missing: ${fragment}`);
  }
}
const updatePluginRouteStart =
  audioAdapter.indexOf("} else if (message.type === 'updatePlugin' && message.plugin) {");
const updatePluginRouteEnd = audioAdapter.indexOf('\n    }\n  }', updatePluginRouteStart);
const updatePluginRoute = audioAdapter.slice(updatePluginRouteStart, updatePluginRouteEnd);
if (updatePluginRouteStart < 0 || updatePluginRouteEnd < 0 ||
    !updatePluginRoute.includes('const pipelineA = this.owner.pipelineA || [];') ||
    !updatePluginRoute.includes('const pipelineB = this.owner.pipelineB || [];') ||
    !updatePluginRoute.includes('if (!inPipelineA && !inPipelineB) return;') ||
    !updatePluginRoute.includes("const pipeline = inPipelineA ? 'A' : 'B';") ||
    !updatePluginRoute.includes('normalizePlugin(message.plugin, logicalOwner)') ||
    updatePluginRoute.includes('this.owner.currentPipeline')) {
  throw new Error('Individual plug-in updates are not routed by actual A/B ownership');
}
const restoredUnsupportedWarnings =
  app.split('if (!this.audioManager.unsupportedWarningShown) {').length - 1;
if (restoredUnsupportedWarnings !== 3 ||
    app.split("this.uiManager.setError('Some effects are unavailable and were bypassed.', false);")
      .length - 1 !== 3 ||
    app.split("this.startupWarningMessage = 'Some effects are unavailable and were bypassed.';")
      .length - 1 !== 3) {
  throw new Error('State restoration does not warn once when an effect is unavailable');
}
const hiddenHostInfo = audioAdapter.indexOf("hidden ? 'host/getInfo' : 'telemetry/read'");
const hiddenSkip = audioAdapter.indexOf('if (hidden) return;', hiddenHostInfo);
if (hiddenHostInfo < 0 || hiddenSkip < hiddenHostInfo ||
    audioAdapter.indexOf('synchronizeNativeContext(result)', hiddenHostInfo) > hiddenSkip) {
  throw new Error('Hidden WebViews must poll host context without draining telemetry packets');
}
if (!/window\.app\?\.initialized === true &&\s*result\.contextGeneration/.test(audioAdapter)) {
  throw new Error('Native context synchronization can clear the pipeline before startup restore');
}
if (!audioAdapter.includes('this.preserveReadyNativePipelineDuringStartup = info.dspReady === true;') ||
    !/if \(this\.preserveReadyNativePipelineDuringStartup && window\.app\?\.initialized !== true\) \{[\s\S]*?this\.synchronizeNativeAssetMembership\(\);[\s\S]*?return '';/.test(audioAdapter)) {
  throw new Error('A recreated UI can rebuild an already-running native DSP pipeline');
}
if (!/message\.type === 'clearPluginAsset'[\s\S]*?preserveReadyNativePipelineDuringStartup[\s\S]*?window\.app\?\.initialized !== true[\s\S]*?acknowledgePreservedAssetClear\(message\);[\s\S]*?return;/.test(audioAdapter) ||
    !/acknowledgePreservedAssetClear\(message\)[\s\S]*?queueMicrotask[\s\S]*?type: 'assetState'[\s\S]*?state: 0[\s\S]*?operationRevision: message\.operationRevision/.test(audioAdapter)) {
  throw new Error('A recreated UI can clear an already-running native DSP asset');
}
const fullRebuild = audioAdapter.indexOf("__effetuneHostCall('pipeline/rebuild'");
const fullRebuildLatency = audioAdapter.indexOf('this.owner.scheduleLatencyService();', fullRebuild);
const fullRebuildCatch = audioAdapter.indexOf("pipeline rebuild failed", fullRebuild);
if (fullRebuild < 0 || fullRebuildLatency < fullRebuild ||
    fullRebuildLatency > fullRebuildCatch ||
    !audioAdapter.includes("__effetuneHostCall('pipeline/restoreHistory'") ||
    !audioAdapter.includes('pipelineBInitialized: this.pipelineB !== null')) {
  throw new Error('Full rebuild latency service or dual-pipeline history synchronization is missing');
}
if (!updatePluginRoute.includes('this.owner.scheduleLatencyService();') ||
    !audioAdapter.includes('now - this.lastHiddenContextPoll < 250') ||
    !/scheduleLatencyService\(\) \{[\s\S]*?setTimeout\([\s\S]*?host\/getInfo[\s\S]*?\}, 250\);/.test(audioAdapter)) {
  throw new Error('Dynamic latency servicing is not independently debounced to 250 ms');
}
if (!app.includes("localStorage.setItem('pipelineColumns', String(config.columns))") ||
    !columns.includes('window.electronIntegration.saveConfig({ columns })')) {
  throw new Error('The VST UI preference bridge patch was not applied');
}
if (!uiManager.includes('this.refreshDynamicLocalizedUI();') ||
    !uiManager.includes('refreshDynamicLocalizedUI() {') ||
    !uiManager.includes('searchManager.switchToTab(currentTab);') ||
    !uiManager.includes('this.pipelineManager?.updatePipelineUI?.(true);')) {
  throw new Error('Language changes do not rebuild dynamic localized UI');
}

for (const excluded of [
  'manifest.json',
  'package.json',
  'js/vendor/jszip-3.10.1.min.js',
  'js/vendor/jsmediatags-3.9.5.min.js',
  'js/vendor/music-metadata-browser.mjs',
  'js/ui/double-blind-test',
  'js/library',
  'js/ui/library',
  'js/ui/audio-player',
  'js/ui/audio-player.js',
  'js/ui/pipeline/file-processor.js',
  'js/audio/offline-processor.js'
]) {
  try {
    await access(path.join(assets, ...excluded.split('/')));
    throw new Error(`Generated UI still contains excluded asset: ${excluded}`);
  } catch (error) {
    if (error?.code !== 'ENOENT') throw error;
  }
}

const visitedModules = new Set();
const pendingModules = [path.join(assets, 'js', 'app.js')];
while (pendingModules.length > 0) {
  const modulePath = pendingModules.pop();
  if (visitedModules.has(modulePath)) continue;
  visitedModules.add(modulePath);
  const source = await readFile(modulePath, 'utf8');
  const imports = [
    ...source.matchAll(/^\s*(?:import|export)\s+(?:[^;]*?\s+from\s+)?['"]([^'"]+)['"]\s*;?/gm),
    ...source.matchAll(/\bimport\s*\(\s*['"]([^'"]+)['"]\s*\)/g)
  ];
  for (const match of imports) {
    const specifier = match[1];
    if (!specifier.startsWith('.')) continue;
    const dependency = path.resolve(path.dirname(modulePath), specifier);
    const relative = path.relative(assets, dependency);
    if (!relative || relative.startsWith('..') || path.isAbsolute(relative)) {
      throw new Error(`Generated module escapes the asset root: ${modulePath} -> ${specifier}`);
    }
    try {
      await access(dependency);
    } catch (error) {
      if (error?.code === 'ENOENT') {
        throw new Error(`Generated module has a missing dependency: ${modulePath} -> ${specifier}`);
      }
      throw error;
    }
    pendingModules.push(dependency);
  }
}

console.log(`EffeTune UI asset contract passed: ${assets}`);
