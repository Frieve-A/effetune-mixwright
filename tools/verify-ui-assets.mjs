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
const pluginBase = await readFile(path.join(assets, 'plugins', 'plugin-base.js'), 'utf8');
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
const savedStateDeclaration = app.indexOf('let savedState = transientPipelineState;');
const automationWatermarkReservation =
  app.indexOf('savedState?.automation?.logicalPluginIdWatermark');
const appPluginCreation = app.indexOf('this.pluginManager.createPlugin(pluginState.name)');
if (!app.includes('plugin.id = pluginState.id') || appIdReservation < 0 ||
    savedStateDeclaration < 0 || automationWatermarkReservation < savedStateDeclaration ||
    automationWatermarkReservation > appPluginCreation ||
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
const historyRouteStart = audioAdapter.indexOf('  synchronizeHistoryState()');
const historyRouteEnd = audioAdapter.indexOf('}).then(result =>', historyRouteStart);
const historyRoute = historyRouteStart < 0 || historyRouteEnd < 0
  ? '' : audioAdapter.slice(historyRouteStart, historyRouteEnd);
// Splits an object literal into its own properties, so a contract about a
// request can name the property it means instead of matching text that happens
// to appear anywhere in the surrounding route.
const objectLiteralProperties = literal => {
  const properties = [];
  let depth = 0;
  let start = 0;
  for (let index = 0; index < literal.length; index++) {
    const character = literal[index];
    if ('([{'.includes(character)) depth += 1;
    else if (')]}'.includes(character)) depth -= 1;
    else if (character === ',' && depth === 0) {
      properties.push(literal.slice(start, index));
      start = index + 1;
    }
  }
  properties.push(literal.slice(start));
  return properties;
};
// Reads what a bridge request actually binds to one of its payload properties:
// the inline expression where there is one, and otherwise the local that a
// shorthand property names. Stating a contract about this value is what keeps it
// from being satisfied by an identifier that merely occurs in the route.
const requestPayloadValue = (route, name) => {
  const call = audioAdapter.indexOf(`__effetuneHostCall('${route}'`);
  const open = call < 0 ? -1 : audioAdapter.indexOf('{', call);
  if (open < 0) return null;
  let depth = 0;
  let close = -1;
  for (let index = open; index < audioAdapter.length && close < 0; index++) {
    if (audioAdapter[index] === '{') depth += 1;
    else if (audioAdapter[index] === '}' && --depth === 0) close = index;
  }
  if (close < 0) return null;
  for (const property of objectLiteralProperties(audioAdapter.slice(open + 1, close))) {
    const separator = property.indexOf(':');
    if ((separator < 0 ? property : property.slice(0, separator)).trim() !== name) continue;
    if (separator >= 0) return property.slice(separator + 1);
    const declaration = audioAdapter.lastIndexOf(`const ${name} =`, call);
    return declaration < 0
      ? null
      : audioAdapter.slice(declaration, audioAdapter.indexOf(';', declaration));
  }
  return null;
};
// A bound target is overlaid with the value automation is playing unless the
// request names it, so every route that publishes a pipeline image has to carry
// the gestures it collected -- the edits themselves, mapped to their payloads,
// however the expression is spelled. Without them a preset load, an undo, or a
// correction moves the UI alone.
for (const route of ['pipeline/rebuild', 'pipeline/updatePlugin', 'pipeline/restoreHistory']) {
  const automationEdits = requestPayloadValue(route, 'automationEdits');
  if (automationEdits === null || !/(\w+)\s*=>\s*\1\.payload/.test(automationEdits)) {
    throw new Error(`${route} does not carry the automation edits it collected`);
  }
}
if (!historyRoute.includes("this.nativePort.collectAutomationEdits('A', plugin)") ||
    !historyRoute.includes("this.nativePort.collectAutomationEdits('B', plugin)")) {
  throw new Error('History synchronization does not collect both pipelines of automation edits');
}
// Collecting an edit has already advanced the adopted baseline to the value it
// carries, and that baseline is the only record of what the native side holds.
// A route whose request is refused therefore has to put every collected edit
// back, or the baseline claims a value only the editor has: a later user edit
// to that same value is read as agreement and never emitted, and nothing
// afterwards can notice the editor, the DSP and the saved project have parted.
const historyFailureLog = audioAdapter.indexOf('[EffeTune Mixwright] history restore failed');
const historyReconcile = historyFailureLog < 0
  ? -1
  : audioAdapter.lastIndexOf('this.nativePort.reconcileAutomationEdits(edits);',
    historyFailureLog);
if (historyRouteStart < 0 || historyFailureLog < 0 || historyReconcile < historyRouteStart) {
  throw new Error('A refused history restore strands the adopted baseline on values ' +
    'the native side never received');
}
const coalescedUpdate = audioAdapter.indexOf("__effetuneHostCall('pipeline/updatePlugin'");
const coalescedUpdateEnd = audioAdapter.indexOf('parameter update failed', coalescedUpdate);
const coalescedUpdateRoute = coalescedUpdate < 0 || coalescedUpdateEnd < 0
  ? '' : audioAdapter.slice(coalescedUpdate, coalescedUpdateEnd);
if (!/automationEdits: \w+\.map\(\w+ => \w+\.payload\)/.test(coalescedUpdateRoute) ||
    audioAdapter.includes("__effetuneHostCall('automation/edit'")) {
  throw new Error('A coalesced plug-in update does not carry its gestures in one request');
}
// The plug-in adopts the user's value as its own DSP value whether or not the
// host takes the edit transaction, so a request that was answered at all leaves
// both sides on the value it carried. There is no second authority for the
// editor to converge on, and therefore no rollback, no correction and nothing
// the answer has to say about the individual edits.
//
// The grace timer this replaced was not merely redundant. A bridge round trip
// slower than its 250 ms deadline made it republish the value the user had
// moved away from, and a republished value reaches performEdit -- so a
// Write-armed lane recorded a reverse-motion point the user never performed,
// purely because the native side was busy. Nothing here may come back.
const withdrawnRollbackMachinery = [
  'AUTOMATION_EDIT_GRACE_MS', 'automationEditGraceMs', 'pendingAutomationEdits',
  'trackPendingAutomationEdit', 'takePendingAutomationEdit',
  'resolvePendingAutomationDelta', 'restoreRolledBackAutomationEdit',
  'rejectPendingAutomationEdit', 'rolledBackTo', 'automationCorrections',
  'automationCorrection', 'reconcileUnappliedCorrection',
  'republishAdoptedPlugin', 'restoredAutomationValues',
  'automationEditsAccepted', 'automationEditsRestored'
].filter(name => audioAdapter.includes(name));
if (withdrawnRollbackMachinery.length > 0) {
  throw new Error('The shim still carries automation rollback machinery: ' +
    withdrawnRollbackMachinery.join(', '));
}
// The upstream controls report values only, so the touch boundary a host records
// automation inside is derived from the pointer and from nothing else. Which
// phase each listener is registered in is therefore part of the contract, not an
// implementation detail. The four pointer events are taken in the capture phase,
// so a control that stops propagation, or one inside a subtree we never see,
// cannot hide the boundary. 'blur' must be taken in neither phase: it does not
// bubble but it does traverse capture, and upstream renders every parameter as
// focusable inputs -- so a capturing listener fires for the blur that pressing
// on one control dispatches on the control that held the focus, ending the touch
// that press had just opened and degrading the rest of the drag into one-shots.
const pointerGestureStart = audioAdapter.indexOf('this.pointerGestureListeners = [');
const pointerGestureEnd = audioAdapter.indexOf('];', pointerGestureStart);
const pointerGestureListeners = pointerGestureStart < 0 || pointerGestureEnd < 0
  ? '' : audioAdapter.slice(pointerGestureStart, pointerGestureEnd);
const requiredPointerGestureListeners = [
  "['pointerdown', this.beginPointerGesture, true]",
  "['pointerup', this.endPointerGesture, true]",
  "['pointercancel', this.endPointerGesture, true]",
  "['lostpointercapture', this.endPointerGesture, true]",
  "['blur', this.abandonPointerGesture, false]"
];
if (pointerGestureListeners === '' ||
    requiredPointerGestureListeners.some(entry => !pointerGestureListeners.includes(entry)) ||
    !/for \(const \[type, listener, capture\] of this\.pointerGestureListeners\) \{\s*window\.addEventListener\?\.\(type, listener, capture\);/
      .test(audioAdapter)) {
  throw new Error(
    'The pointer gesture listeners are not registered in the phases the touch boundary needs');
}
// One pointer stopping being down is one pointer, not the gesture: a second
// finger releasing mid-drag leaves the user still holding the control, and a
// touch ended there loses the rest of the drag. A mouse reports every one of
// its buttons under a single pointer id, so the id alone cannot answer that
// question either -- what remains down after a release is what buttons names.
//
// The record of which pointers are down is a hint the platform can strip, and
// never an authority. Chromium opens a <select> popup on the press and takes
// capture, so the page sees the pointerdown and no release of any kind: no
// pointerup, no pointercancel, no lostpointercapture, and the focus never
// leaves the page, so no window blur either. Upstream renders filter types,
// crossover slopes and oversampling factors as <select>, and a native context
// menu loses the release the same way. An id left behind by one of those makes
// "no pointer is down" unreachable, so the close is never reached again: every
// control the user touches afterwards keeps a host write lane open, and the
// block keeps ignoring the automation the host plays back into it.
//
// So the set is rebuilt by the press that starts the next interaction -- a
// primary press, which a second contact is explicitly not -- and emptied by the
// close itself. What it may never be is trusted to drain on its own.
const beginPointerGestureStart = audioAdapter.indexOf('this.beginPointerGesture = event => {');
const beginPointerGestureEnd =
  audioAdapter.indexOf('this.endPointerGesture', beginPointerGestureStart);
const beginPointerGestureBody = beginPointerGestureStart < 0 || beginPointerGestureEnd < 0
  ? '' : audioAdapter.slice(beginPointerGestureStart, beginPointerGestureEnd);
const closeGestureStart = audioAdapter.indexOf('  closePointerGesture() {');
const closeGestureEnd = audioAdapter.indexOf('\n  postMessage(', closeGestureStart);
const closeGestureBody = closeGestureStart < 0 || closeGestureEnd < 0
  ? '' : audioAdapter.slice(closeGestureStart, closeGestureEnd);
if (beginPointerGestureBody === '' || closeGestureBody === '' ||
    !/if \(event\?\.isPrimary !== false\) this\.livePointerIds\.clear\(\);[\s\S]{0,120}?livePointerIds\.add\(/
      .test(beginPointerGestureBody) ||
    !closeGestureBody.includes('this.livePointerIds.clear();')) {
  throw new Error('A pointer release the page never receives poisons every later touch');
}
if (!/endPointerGesture = event => \{[\s\S]{0,400}?if \(event\?\.type === 'pointerup' && \(event\.buttons \?\? 0\) !== 0\) return;[\s\S]{0,200}?livePointerIds\.delete\([\s\S]{0,200}?if \(this\.livePointerIds\.size === 0\) this\.closePointerGesture\(\);/
  .test(audioAdapter)) {
  throw new Error('An open touch does not survive a second pointer releasing inside it');
}
// A state restore ends every open touch and then asks the editor to reload, but
// the reload is a request the old context outlives: a drag still in progress
// there can flush one more value across it, and that value reopens a touch the
// reloaded page -- which starts with no gesture targets at all -- can never
// name to close. The page announcing its own startup is the boundary that ends
// it, and it is a boundary only because no other host/getInfo carries the flag.
const startupHandshakes =
  audioAdapter.split("__effetuneHostCall('host/getInfo', { startup: true })").length - 1;
if (startupHandshakes !== 1 ||
    !/initAudio\(\) \{[\s\S]{0,700}?__effetuneHostCall\('host\/getInfo', \{ startup: true \}\)/
      .test(audioAdapter)) {
  throw new Error('A reloaded page does not announce itself, so a touch the ' +
    'destroyed context left open can never be ended');
}
// Every value of a drag asks for the touch, not only the first. The native open
// is idempotent, so the drag is still one beginEdit, and a close the native side
// made on its own -- a suspend, a state restore -- is recovered from on the very
// next value instead of leaving the rest of the drag with no touch window.
if (!/markAutomationGesture\(edit\) \{[\s\S]*?edit\.payload\.beginGesture = true;/
      .test(audioAdapter)) {
  throw new Error('A dragged value does not ask for the touch it belongs to');
}
// The port is the last thing that can end a touch, because it releases the
// listeners that would otherwise have derived the close.
const portCloseStart = audioAdapter.indexOf('\n  close() {');
const portCloseEnd = audioAdapter.indexOf('\n  dispatch(', portCloseStart);
const portCloseBody = portCloseStart < 0 || portCloseEnd < 0
  ? '' : audioAdapter.slice(portCloseStart, portCloseEnd);
if (!portCloseBody.includes('this.closePointerGesture();') ||
    !/for \(const \[type, listener, capture\] of this\.pointerGestureListeners\) \{\s*window\.removeEventListener\?\.\(type, listener, capture\);/
      .test(portCloseBody)) {
  throw new Error('A closed port can leave a touch open for the rest of the session');
}
// A failed frame carries every value the drag emitted for an identity, and each
// edit names only the value the edit before it replaced -- so the first is the
// only one that still names what the native side holds.
const reconcileStart = audioAdapter.indexOf('  reconcileAutomationEdits(');
const reconcileEnd = audioAdapter.indexOf('\n  assetKey(', reconcileStart);
const reconcileBody = reconcileStart < 0 || reconcileEnd < 0
  ? '' : audioAdapter.slice(reconcileStart, reconcileEnd);
if (reconcileBody === '' ||
    !/new Set\(\)/.test(reconcileBody) ||
    !/\.has\(identity\)\) continue;/.test(reconcileBody)) {
  throw new Error(
    'A failed frame does not return each identity to the one value the native side holds');
}
// One ordered list still holds every gesture a frame collected, because the
// native side applies it from the front: the entry it ends on is the value the
// block-start pin keeps playing, so a gesture kept anywhere else would be
// applied out of the order the user made it in.
const queuePluginUpdateStart = audioAdapter.indexOf('  queuePluginUpdate(');
const queuePluginUpdateEnd =
  audioAdapter.indexOf('\n  schedulePluginUpdateFlush()', queuePluginUpdateStart);
const queuePluginUpdateBody = queuePluginUpdateStart < 0 || queuePluginUpdateEnd < 0
  ? '' : audioAdapter.slice(queuePluginUpdateStart, queuePluginUpdateEnd);
if (queuePluginUpdateBody === '' ||
    (queuePluginUpdateBody.match(/\.push\(/g) || []).length !== 1 ||
    !/queuePluginUpdate\(pipeline, \w+, edits\)/.test(updatePluginRoute)) {
  throw new Error('The gestures of one frame do not share a single ordered queue');
}
// Change detection adopts the value where it detects it, not where an answer
// for it comes back. Two answers that arrive out of order would otherwise move
// the baseline backwards, and a knob returned to where it started inside one
// frame would emit no edit to return it.
const collectStart = audioAdapter.indexOf('  collectAutomationEdits(');
const collectEnd = audioAdapter.indexOf('\n  reconcileAutomationEdits(', collectStart);
const collectBody = collectStart < 0 || collectEnd < 0
  ? '' : audioAdapter.slice(collectStart, collectEnd);
if (collectBody === '' ||
    !/adoptedAutomationValues\.set\(identity, normalized\);\s*edits\.push\(/.test(collectBody)) {
  throw new Error('A detected automation change is not adopted where it is detected');
}
// One displayed frame costs one bridge request no matter how many events a drag
// emitted: an individual update joins a per-plug-in queue that a scheduled flush
// publishes. Every other request drains that queue first, so a coalesced image
// can never land after the rebuild, restore, or asset operation that replaced it.
const postMessageStart = audioAdapter.indexOf('  postMessage(message');
const bridgeOrdering = postMessageStart < 0 || updatePluginRouteStart < 0
  ? '' : audioAdapter.slice(postMessageStart, updatePluginRouteStart);
const historyFlush = historyRoute.indexOf('this.nativePort.flushPluginUpdates();');
if (!updatePluginRoute.includes('this.queuePluginUpdate(') ||
    !/queuePluginUpdate\([^)]*\) \{[\s\S]*?this\.pendingPluginUpdates\.set\([\s\S]*?this\.schedulePluginUpdateFlush\(\);/
      .test(audioAdapter) ||
    !/flushPluginUpdates\(\) \{[\s\S]*?this\.pendingPluginUpdates\.clear\(\);/.test(audioAdapter) ||
    !/message\.type !== 'updatePlugin'[\s\S]{0,40}flushPluginUpdates\(\);/.test(bridgeOrdering) ||
    historyFlush < 0 || historyFlush > historyRoute.indexOf('this.serializePipeline(')) {
  throw new Error(
    'Coalesced plug-in updates are not published once per frame ahead of every other request');
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
// Host automation playback, preset recall and undo/redo all reach the editor
// through setParameters() followed by syncUIControls(). Widgets a plug-in builds
// by hand are invisible to the control registry, so they only follow through the
// refresh-hook registry. These three properties are what make that path exist at
// all; losing any one of them silently freezes every hand-built control again.
if (!/registerUIRefresh\(fn\) \{/.test(pluginBase) ||
    !/isGraphPointerActive\(\) \{/.test(pluginBase) ||
    !pluginBase.includes('this._uiRefreshHooks = []') ||
    !pluginBase.includes('this._graphPointerProbes')) {
  throw new Error('PluginBase no longer exposes the UI refresh-hook registry');
}
// The early return in syncUIControls() must consider both registries. Guarding it
// on the control registry alone is the original defect: a plug-in that registers
// no helper controls never reaches the hook loop, which is exactly the case for
// the hand-built graph editors.
if (!/syncUIControls\(\) \{\s*if \(!this\._syncedUIControls\.length && !this\._uiRefreshHooks\.length\) return;/
      .test(pluginBase) ||
    !/if \(this\.isGraphPointerActive\(\)\) return;/.test(pluginBase)) {
  throw new Error('syncUIControls() no longer runs the refresh hooks for control-less plug-ins');
}
// A rebuilt UI must not inherit hooks or pointer probes bound to detached DOM.
if ((pluginBase.match(/this\._uiRefreshHooks = \[\]/g) || []).length < 3 ||
    (pluginBase.match(/this\._graphPointerProbes(?: = new Set\(\)|\.clear\(\))/g) || []).length < 3) {
  throw new Error('The UI refresh registries are not reset on createUI() and cleanup()');
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
