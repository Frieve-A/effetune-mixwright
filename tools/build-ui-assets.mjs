import { cp, mkdir, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(scriptDirectory, '..');
const productName = 'EffeTune Mixwright';
const vstLocaleAdditions = {
  en: {
    'roomEq.action.import': 'Import\u2026',
    'roomEq.delete.title': 'Delete imported measurement',
    'roomEq.delete.confirm':
      'Delete imported measurement \u201c{name}\u201d? This cannot be undone.',
    'roomEq.delete.error': 'The imported measurement could not be deleted.'
  },
  ja: {
    'roomEq.action.import': '測定をインポート\u2026',
    'roomEq.delete.title': 'インポートした測定を削除',
    'roomEq.delete.confirm':
      'インポートした測定「{name}」を削除しますか？この操作は元に戻せません。',
    'roomEq.delete.error': 'インポートした測定を削除できませんでした。'
  }
};

function applyProductBranding(sourceText) {
  return sourceText
    .replaceAll('Frieve EffeTune', productName)
    .replace(/EffeTune(?! Mixwright)/g, productName);
}

function argument(name) {
  const index = process.argv.indexOf(name);
  return index >= 0 ? process.argv[index + 1] : null;
}

const source = path.resolve(argument('--source') || path.join(projectRoot, 'external', 'effetune'));
const output = path.resolve(argument('--out') || path.join(projectRoot, 'build', 'webview-assets'));
const outputRelative = path.relative(projectRoot, output);
if (!outputRelative || outputRelative.startsWith('..') || path.isAbsolute(outputRelative)) {
  throw new Error(`Refusing to replace an asset directory outside the project: ${output}`);
}
if (source === output || !source.endsWith(path.join('external', 'effetune'))) {
  throw new Error(`Unexpected EffeTune source directory: ${source}`);
}

await rm(output, { recursive: true, force: true });
await mkdir(output, { recursive: true });

for (const entry of ['js', 'plugins', 'presets', 'images']) {
  await cp(path.join(source, entry), path.join(output, entry), { recursive: true });
}
for (const entry of ['effetune.css']) {
  await cp(path.join(source, entry), path.join(output, entry));
}
await cp(path.join(projectRoot, 'ui-shim', 'vst-bootstrap.js'),
         path.join(output, 'vst-bootstrap.js'));
await cp(path.join(projectRoot, 'ui-shim', 'vst-audio-manager.js'),
         path.join(output, 'vst-audio-manager.js'));
await mkdir(path.join(output, 'features', 'measurement'), { recursive: true });
await cp(path.join(source, 'features', 'measurement', 'dataStorage.js'),
         path.join(output, 'features', 'measurement', 'dataStorage.js'));
await cp(path.join(projectRoot, 'THIRD-PARTY-NOTICES.txt'),
         path.join(output, 'THIRD-PARTY-NOTICES.txt'));

let html = await readFile(path.join(source, 'effetune.html'), 'utf8');
const upstreamDocumentTitle = '<title>EffeTune</title>';
const upstreamHeaderTitle = '<h1>Frieve EffeTune<img';
const upstreamFooterTitle = 'EffeTune version <span id="app-version"></span>';
const upstreamIconAlt = 'alt="EffeTune Icon"';
if (!html.includes(upstreamDocumentTitle) || !html.includes(upstreamHeaderTitle) ||
    !html.includes(upstreamFooterTitle) || !html.includes(upstreamIconAlt)) {
  throw new Error('Unable to locate the upstream EffeTune branding');
}
html = applyProductBranding(html)
  .replace(/\s*<link rel="stylesheet" href="effetune-(?:mobile|library)\.css">/g, '')
  .replace(/\s*<link rel="manifest" href="manifest\.json">/g, '')
  .replace(/<meta http-equiv="Content-Security-Policy"[^>]*>/,
    `<meta http-equiv="Content-Security-Policy" content="default-src 'self' blob: data:; script-src 'self' 'unsafe-inline' 'unsafe-eval' blob:; connect-src 'self' blob:; img-src 'self' data: blob:; style-src 'self' 'unsafe-inline' blob:;">`)
  .replace(/\s*<script>\s*\(function\(\) \{\s*var measurementId =[\s\S]*?<\/script>/, '')
  .replace(/\s*<button class="pipeline-menu-item" id="doubleBlindTestButton">[\s\S]*?<\/button>/, '')
  .replace(/\s*<script src="js\/vendor\/(?:jszip-3\.10\.1\.min\.js|jsmediatags-3\.9\.5\.min\.js)"><\/script>/g, '')
  .replace('</head>', '    <script src="vst-bootstrap.js"></script>\n</head>');
await writeFile(path.join(output, 'effetune.html'), html, 'utf8');
await rm(path.join(output, 'js', 'vendor'), { recursive: true, force: true });

const appPath = path.join(output, 'js', 'app.js');
let app = await readFile(appPath, 'utf8');
const originalImport = `import { AudioManager } from './audio-manager.js';`;
if (!app.includes(originalImport)) {
  throw new Error('Unable to locate the AudioManager import in js/app.js');
}
app = app.replace(originalImport,
  `import { AudioManager } from '../vst-audio-manager.js';`);
const libraryConstantsImport = `import { normalizeMusicLibraryStartupView } from './library/constants.js';`;
if (!app.includes(libraryConstantsImport)) {
  throw new Error('Unable to locate the music-library constants import');
}
app = app.replace(libraryConstantsImport,
  `const normalizeMusicLibraryStartupView = () => 'tracks';`);
const startupViewPreference = /    async applyStartupViewPreference\(\) \{[\s\S]*?\n    \}\n\n    \/\*\*\n     \* Initialize and build pipeline/;
if (!startupViewPreference.test(app)) {
  throw new Error('Unable to locate the music-library startup preference');
}
app = app.replace(startupViewPreference, `    async applyStartupViewPreference() {}

    /**
     * Initialize and build pipeline`);

const unconditionalPipelineReset = `        // Set the pipeline in audioManager
        this.audioManager.pipelineA = plugins;
        this.audioManager.setCurrentPipeline('A');`;
const upstreamDualPipelineRestore = `        // Set the pipeline in audioManager
        this.audioManager.pipelineA = plugins;
        if (restoredDualPipeline) {
            this.audioManager.pipelineB = restoredPipelineB;
            this.audioManager.setCurrentPipeline(restoredCurrentPipeline);
        } else {
            this.audioManager.setCurrentPipeline('A');
        }`;
if (app.includes(unconditionalPipelineReset)) {
  app = app.replace(unconditionalPipelineReset, `        // A dual-pipeline VST state was already assigned above. Only the
        // legacy single-pipeline path should force pipeline A.
        if (!(savedState && savedState.pipelineA && savedState.pipelineB !== undefined)) {
            this.audioManager.pipelineA = plugins;
            this.audioManager.setCurrentPipeline('A');
        }`);
} else if (!app.includes(upstreamDualPipelineRestore)) {
  throw new Error('Unable to locate the initial pipeline assignment in js/app.js');
}
const dualPipelineRestoreMarker = `        // Handle dual pipeline format
        if (savedState && savedState.pipelineA && savedState.pipelineB !== undefined) {`;
if (!app.includes(dualPipelineRestoreMarker)) {
  throw new Error('Unable to locate the saved-state pipeline restoration');
}
app = app.replace(dualPipelineRestoreMarker, `        const automationWatermark =
            savedState?.automation?.logicalPluginIdWatermark;
        if (Number.isInteger(automationWatermark) && automationWatermark >= 0) {
            this.pluginManager.nextPluginId = Math.max(
                this.pluginManager.nextPluginId, automationWatermark + 1);
        }

${dualPipelineRestoreMarker}`);
const createRestoredPlugin = `const plugin = this.pluginManager.createPlugin(pluginState.name);`;
const restoredPluginMatches = app.split(createRestoredPlugin).length - 1;
if (restoredPluginMatches !== 3) {
  throw new Error(`Expected three state-restoration plug-in creation sites, found ${restoredPluginMatches}`);
}
const restoredPluginWithReservation = `if (Number.isInteger(pluginState.id) && pluginState.id > 0) {
                        this.pluginManager.nextPluginId = Math.max(
                            this.pluginManager.nextPluginId, pluginState.id + 1);
                    }
                    ${createRestoredPlugin}
                    if (Number.isInteger(pluginState.id) && pluginState.id > 0) {
                        plugin.id = pluginState.id;
                    }`;
app = app.replaceAll(createRestoredPlugin, restoredPluginWithReservation);
const restoredPluginFailure =
  "console.warn(`Failed to create plugin '${pluginState.name}': ${error.message}`);";
const restoredPluginFailureMatches = app.split(restoredPluginFailure).length - 1;
if (restoredPluginFailureMatches !== 3) {
  throw new Error(`Expected three state-restoration failure sites, found ${restoredPluginFailureMatches}`);
}
app = app.replaceAll(restoredPluginFailure, `${restoredPluginFailure}
                    if (!this.audioManager.unsupportedWarningShown) {
                        this.audioManager.unsupportedWarningShown = true;
                        this.startupWarningMessage = 'Some effects are unavailable and were bypassed.';
                        this.uiManager.setError('Some effects are unavailable and were bypassed.', false);
                    }`);
const restoreDoubleBlind = /    restoreDoubleBlindTestFromUrl\(\) \{[\s\S]*?\n    \}\n\n    setStartupWarning/;
if (!restoreDoubleBlind.test(app)) {
  throw new Error('Unable to locate Double Blind Test URL restoration');
}
app = app.replace(restoreDoubleBlind, `    restoreDoubleBlindTestFromUrl() {}

    setStartupWarning`);
const publishInitialConfig = `            windowRef.appConfig = config;`;
if (!app.includes(publishInitialConfig)) {
  throw new Error('Unable to locate the initial config publication in js/app.js');
}
app = app.replace(publishInitialConfig, `${publishInitialConfig}
            if (Number.isInteger(config.columns) && config.columns >= 1 && config.columns <= 8) {
                localStorage.setItem('pipelineColumns', String(config.columns));
            }`);
await writeFile(appPath, app, 'utf8');

const historyPath = path.join(output, 'js', 'ui', 'pipeline', 'history-manager.js');
let history = await readFile(historyPath, 'utf8');
const historySerialization =
  `this.pipelineManager.core.getSerializablePluginState(plugin, true, false, false)`;
const historySerializationMatches = history.split(historySerialization).length - 1;
if (historySerializationMatches !== 2) {
  throw new Error(`Expected two history serialization sites, found ${historySerializationMatches}`);
}
history = history.replaceAll(historySerialization,
  `({ ...${historySerialization}, id: plugin.id })`);
const historyCreate =
  `const plugin = this.pipelineManager.pluginManager.createPlugin(pluginState.nm);`;
const historyCreateMatches = history.split(historyCreate).length - 1;
if (historyCreateMatches !== 3) {
  throw new Error(`Expected three history restoration sites, found ${historyCreateMatches}`);
}
history = history.replaceAll(historyCreate, `if (Number.isInteger(pluginState.id) && pluginState.id > 0) {
                        this.pipelineManager.pluginManager.nextPluginId = Math.max(
                            this.pipelineManager.pluginManager.nextPluginId, pluginState.id + 1);
                    }
                    ${historyCreate}
                    if (plugin && Number.isInteger(pluginState.id) && pluginState.id > 0) {
                        plugin.id = pluginState.id;
                    }`);
const historyRebuild = `this.audioManager.rebuildPipeline();`;
if (history.split(historyRebuild).length - 1 !== 1) {
  throw new Error('Expected one undo/redo pipeline rebuild site');
}
history = history.replace(historyRebuild, `void this.audioManager.synchronizeHistoryState();`);
const historyWorkletUpdate = `            // Update worklet directly without rebuilding pipeline
            this.pipelineManager.core.updateWorkletPlugins();`;
if (history.split(historyWorkletUpdate).length - 1 !== 1) {
  throw new Error('Expected one redundant undo/redo worklet update site');
}
history = history.replace(historyWorkletUpdate, '');
await writeFile(historyPath, history, 'utf8');

const pipelineManagerPath = path.join(output, 'js', 'ui', 'pipeline-manager.js');
let pipelineManager = await readFile(pipelineManagerPath, 'utf8');
const fileProcessorImport = `import { FileProcessor } from './pipeline/file-processor.js';\n`;
const fileProcessorConstruction = `        this.fileProcessor = new FileProcessor(this);`;
const droppedAudioMethod = `    /**
     * Process dropped audio files
     * Delegates to FileProcessor
     * @param {File[]} files - Array of audio files to process
     */
    async processDroppedAudioFiles(files) {
        await this.fileProcessor.processDroppedAudioFiles(files);
    }`;
if (!pipelineManager.includes(fileProcessorImport) ||
    !pipelineManager.includes(fileProcessorConstruction) ||
    !pipelineManager.includes(droppedAudioMethod)) {
  throw new Error('Unable to locate the offline file-processing UI integration');
}
pipelineManager = pipelineManager
  .replace(fileProcessorImport, '')
  .replace(fileProcessorConstruction, '        this.fileProcessor = null;')
  .replace(droppedAudioMethod, '    async processDroppedAudioFiles() {}');
await writeFile(pipelineManagerPath, pipelineManager, 'utf8');

const uiManagerPath = path.join(output, 'js', 'ui-manager.js');
let uiManager = await readFile(uiManagerPath, 'utf8');
const uiManagerLibraryConstantsImport =
  `import { normalizeMusicLibraryStartupView } from './library/constants.js';`;
if (!uiManager.includes(uiManagerLibraryConstantsImport)) {
  throw new Error('Unable to locate the UI music-library constants import');
}
uiManager = uiManager.replace(uiManagerLibraryConstantsImport,
  `const normalizeMusicLibraryStartupView = () => 'tracks';`);
const urlReflectionGate = `        this.urlReflectionEnabled = true;`;
if (uiManager.split(urlReflectionGate).length - 1 !== 1) {
  throw new Error('Expected one URL reflection gate in js/ui-manager.js');
}
uiManager = uiManager.replace(urlReflectionGate, `        // The VST WebView has no address bar, and its pipeline is persisted in
        // the plug-in state chunk rather than in localStorage, so reflecting it
        // would only re-encode the whole pipeline on every parameter change.
        this.urlReflectionEnabled = false;`);
for (const excludedImport of [
  `import { AudioPlayer } from './ui/audio-player.js';\n`,
  `import { LibraryManagerV2 } from './library/library-manager-v2.js';\n`,
  `import { createWebCatalogRecoveryController } from './library/repository/catalog-client-factory.js';\n`,
  `import { LibraryView } from './ui/library/library-view.js';\n`,
  `import { CatalogPlaybackBridge } from './ui/audio-player/catalog-playback-bridge.js';\n`,
  `import { resolveWebPlaybackSelection } from './ui/playback-selection-router.js';\n`,
  `import { resolveWebCueSiblingFiles } from './ui/web-cue-source-resolver.js';\n`
]) {
  if (!uiManager.includes(excludedImport)) {
    throw new Error(`Unable to locate excluded UI import: ${excludedImport.trim()}`);
  }
  uiManager = uiManager.replace(excludedImport, '');
}
const musicInitializers = `        this.initOpenMusicButton();
        this.initOpenLibraryButton();
        this.initLibraryRecovery();`;
if (!uiManager.includes(musicInitializers)) {
  throw new Error('Unable to locate the music/library UI initializers');
}
uiManager = uiManager.replace(musicInitializers,
  `        // Music player and library are excluded from the VST UI.`);
const libraryRecoveryInitialization = `        this.libraryRecoveryApi = window.electronAPI?.libraryRecoveryV1 ||
            createWebCatalogRecoveryController();
        this.libraryRecoveryState = window.electronAPI?.libraryRecoveryV1
            ? { apiVersion: 1, status: 'initializing', available: false, canReset: false }
            : this.libraryRecoveryApi.getState();`;
if (!uiManager.includes(libraryRecoveryInitialization)) {
  throw new Error('Unable to locate music-library recovery initialization');
}
uiManager = uiManager.replace(libraryRecoveryInitialization, `        this.libraryRecoveryApi = null;
        this.libraryRecoveryState = {
            apiVersion: 1,
            status: 'unavailable',
            available: false,
            canReset: false
        };`);
const libraryShortcut = /\n            if \(\(e\.ctrlKey \|\| e\.metaKey\) && !e\.shiftKey && !e\.altKey && String\(e\.key\)\.toLowerCase\(\) === 'l'\) \{[\s\S]*?\n            \}\n/;
if (!libraryShortcut.test(uiManager)) {
  throw new Error('Unable to locate the music-library keyboard shortcut');
}
uiManager = uiManager.replace(libraryShortcut, '\n');
const doubleBlindImport = `import { DoubleBlindTest } from './ui/double-blind-test/double-blind-test.js';\n`;
if (!uiManager.includes(doubleBlindImport)) {
  throw new Error('Unable to locate Double Blind Test import');
}
uiManager = uiManager.replace(doubleBlindImport, '');
const doubleBlindMethods = /    \/\*\*\n     \* Get \(creating on first use\)[\s\S]*?    getDoubleBlindTest\(\) \{[\s\S]*?\n    \}\n\n    \/\*\* Is the Double Blind Test mode currently open\? \*\/\n    isDoubleBlindActive\(\) \{[\s\S]*?\n    \}/;
if (!doubleBlindMethods.test(uiManager)) {
  throw new Error('Unable to locate Double Blind Test UI methods');
}
uiManager = uiManager.replace(doubleBlindMethods, `    getDoubleBlindTest() { return null; }

    isDoubleBlindActive() { return false; }`);
const languagePreferenceRefresh = `        await this.loadTranslations(targetLocale, requestGeneration);
        return this.userLanguage;`;
if (!uiManager.includes(languagePreferenceRefresh)) {
  throw new Error('Unable to locate the language preference refresh point');
}
uiManager = uiManager.replace(languagePreferenceRefresh,
  `        await this.loadTranslations(targetLocale, requestGeneration);
        if (requestGeneration === this.translationRequestGeneration) {
            this.refreshDynamicLocalizedUI();
        }
        return this.userLanguage;`);
const localizationInitializer = `    /**
     * Initialize localization system
     */
    async initLocalization() {`;
if (!uiManager.includes(localizationInitializer)) {
  throw new Error('Unable to locate the localization initializer');
}
uiManager = uiManager.replace(localizationInitializer, `    refreshDynamicLocalizedUI() {
        const searchManager = this.pluginListManager?.searchManager;
        const currentTab = searchManager?.currentTab || 'effects';
        const searchText = searchManager?.isSearchActive
            ? String(searchManager.searchInput?.value || '')
            : '';

        if (typeof searchManager?.switchToTab === 'function') {
            searchManager.switchToTab(currentTab);
            if (searchText) {
                if (currentTab === 'effects') {
                    searchManager.filterPlugins(searchText);
                } else {
                    searchManager.filterPresets(searchText);
                }
            }
        } else {
            this.pluginListManager?.initPluginList?.();
        }

        this.pipelineManager?.updatePipelineUI?.(true);
        this.refreshRangeFillStyling();
    }

${localizationInitializer}`);
await writeFile(uiManagerPath, uiManager, 'utf8');
await rm(path.join(output, 'js', 'ui', 'double-blind-test'), { recursive: true, force: true });

const electronIntegrationPath = path.join(output, 'js', 'electron-integration.js');
let electronIntegration = await readFile(electronIntegrationPath, 'utf8');
const documentationLinkPatch = /  patchDocumentationLinks\(\) \{[\s\S]*?\n  \}\n\n  \/\*\*\n   \* Initialize event listeners/;
if (!documentationLinkPatch.test(electronIntegration)) {
  throw new Error('Unable to locate the Electron documentation-link patch');
}
electronIntegration = electronIntegration.replace(documentationLinkPatch,
  `  patchDocumentationLinks() {}

  /**
   * Initialize event listeners`);
electronIntegration = applyProductBranding(electronIntegration);
await writeFile(electronIntegrationPath, electronIntegration, 'utf8');

for (const relativePath of [
  ['js', 'electron', 'presetIntegration.js'],
  ['js', 'ui', 'pipeline', 'pipeline-ai-dialog.js']
]) {
  const assetPath = path.join(output, ...relativePath);
  const assetSource = await readFile(assetPath, 'utf8');
  await writeFile(assetPath, applyProductBranding(assetSource), 'utf8');
}

const localesPath = path.join(output, 'js', 'locales');
for (const localeFile of await readdir(localesPath)) {
  if (!localeFile.endsWith('.json5')) continue;
  const localePath = path.join(localesPath, localeFile);
  const localeSource = await readFile(localePath, 'utf8');
  let localized = applyProductBranding(localeSource);
  const additions = vstLocaleAdditions[path.basename(localeFile, '.json5')];
  if (additions) {
    const closing = localized.match(/\s*}\s*$/);
    if (!closing) throw new Error(`Unable to extend VST locale: ${localeFile}`);
    const fields = Object.entries(additions)
      .map(([key, value]) => `  ${JSON.stringify(key)}: ${JSON.stringify(value)}`)
      .join(',\n');
    localized = `${localized.slice(0, closing.index).trimEnd()},\n${fields}\n}\n`;
  }
  await writeFile(localePath, localized, 'utf8');
}

const browserAudioManagerPath = path.join(output, 'js', 'audio-manager.js');
let browserAudioManager = await readFile(browserAudioManagerPath, 'utf8');
const offlineImport = `import { OfflineProcessor } from './audio/offline-processor.js';\n`;
const offlineConstruction =
  `        this.offlineProcessor = new OfflineProcessor(this.contextManager, this.audioEncoder);`;
if (!browserAudioManager.includes(offlineImport) ||
    !browserAudioManager.includes(offlineConstruction)) {
  throw new Error('Unable to locate the offline processor integration');
}
browserAudioManager = browserAudioManager
  .replace(offlineImport, '')
  .replace(offlineConstruction, '        this.offlineProcessor = null;');
await writeFile(browserAudioManagerPath, browserAudioManager, 'utf8');

const configIntegrationPath = path.join(output, 'js', 'electron', 'configIntegration.js');
let configIntegration = await readFile(configIntegrationPath, 'utf8');
const libraryConfigImport = `import {
  MUSIC_LIBRARY_STARTUP_VIEWS,
  normalizeMusicLibraryStartupView
} from '../library/constants.js';`;
if (!configIntegration.includes(libraryConfigImport)) {
  throw new Error('Unable to locate music-library config constants import');
}
configIntegration = configIntegration.replace(libraryConfigImport,
  `const MUSIC_LIBRARY_STARTUP_VIEWS = Object.freeze(['tracks']);
const normalizeMusicLibraryStartupView = () => 'tracks';`);
await writeFile(configIntegrationPath, configIntegration, 'utf8');

const clipboardPath = path.join(output, 'js', 'ui', 'pipeline', 'clipboard-manager.js');
let clipboard = await readFile(clipboardPath, 'utf8');
const doubleBlindPaste = /\n                    \/\/ A Double Blind Test share URL[\s\S]*?\n                    }\n\n                    \/\/ Ignore all pipeline pasting while the Double Blind Test is open\.[\s\S]*?\n                    }\n/;
if (!doubleBlindPaste.test(clipboard)) {
  throw new Error('Unable to locate Double Blind Test clipboard branches');
}
clipboard = clipboard.replace(doubleBlindPaste, '\n');
await writeFile(clipboardPath, clipboard, 'utf8');

const columnsPath = path.join(output, 'js', 'ui', 'pipeline', 'pipeline-column-manager.js');
let columns = await readFile(columnsPath, 'utf8');
const saveColumns = `            localStorage.setItem('pipelineColumns', columns);`;
if (!columns.includes(saveColumns)) {
  throw new Error('Unable to locate column preference persistence');
}
columns = columns.replace(saveColumns, `${saveColumns}
            if (window.electronIntegration?.saveConfig) {
                void window.electronIntegration.saveConfig({ columns });
            }`);
await writeFile(columnsPath, columns, 'utf8');
await rm(path.join(output, 'js', 'library'), { recursive: true, force: true });
await rm(path.join(output, 'js', 'ui', 'library'), { recursive: true, force: true });
await rm(path.join(output, 'js', 'ui', 'audio-player'), { recursive: true, force: true });
await rm(path.join(output, 'js', 'ui', 'audio-player.js'), { force: true });
await rm(path.join(output, 'js', 'ui', 'pipeline', 'file-processor.js'), { force: true });
await rm(path.join(output, 'js', 'audio', 'offline-processor.js'), { force: true });
await writeFile(path.join(output, '.stamp'), `${productName} WebView assets\n`, 'utf8');
