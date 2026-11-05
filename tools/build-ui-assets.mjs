import { cp, mkdir, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(scriptDirectory, '..');
const productName = 'EffeTune Mixwright';
const vstTourLocales = {
  en: {
    'tour.step.welcome.body': 'Take a short tour to learn how to process audio from your DAW, add effects, and adjust your sound.',
    'tour.step.sound.body': 'Insert Mixwright on a track or bus in your DAW, then start playback or enable input monitoring. The DAW supplies the audio.',
    'tour.step.views.title': 'Explore the sound',
    'tour.step.views.body': 'Visualizer displays the native pipeline output. Use Effect Pipeline to return to your effects.',
    'tour.step.settings.body': 'Open Settings for Configuration (language, theme, startup view, and controllers) and Backup / Restore.',
    'tour.step.help.body': 'Open Settings and choose Help to read the guide. Each effect also has a ? button. Choose Take the tour in Settings to see this tour again.'
  },
  ja: {
    'tour.step.welcome.body': 'DAWの音声を処理し、エフェクトを追加して音を調整する方法を紹介します。',
    'tour.step.sound.body': 'DAWのトラックやバスにMixwrightを挿入し、再生または入力モニターを開始してください。音声はDAWから供給されます。',
    'tour.step.views.title': '音を見てみよう',
    'tour.step.views.body': 'Visualizerでネイティブパイプラインの出力を表示できます。エフェクト編集に戻るにはEffect Pipelineを使います。',
    'tour.step.settings.body': '設定からConfiguration（言語、テーマ、起動画面、コントローラー）やバックアップ／復元を開けます。',
    'tour.step.help.body': '設定のHelpからガイドを開けます。各エフェクトの?ボタンでも説明を読めます。設定のツアー項目から、このツアーを再度開けます。'
  }
};
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

async function readText(file) {
  return (await readFile(file, 'utf8')).replaceAll('\r\n', '\n');
}

const source = path.resolve(argument('--source') || path.join(projectRoot, 'external', 'effetune'));
const output = path.resolve(argument('--out') || path.join(projectRoot, 'build', 'webview-assets'));
const outputRelative = path.relative(projectRoot, output);
if (!outputRelative || outputRelative.startsWith('..') || path.isAbsolute(outputRelative)) {
  throw new Error(`Refusing to replace an asset directory outside the project: ${output}`);
}
const sourceRelative = path.relative(source, output);
const outputToSource = path.relative(output, source);
if (!sourceRelative || (!sourceRelative.startsWith('..') && !path.isAbsolute(sourceRelative)) ||
    (!outputToSource.startsWith('..') && !path.isAbsolute(outputToSource))) {
  throw new Error(`Unexpected EffeTune source directory: ${source}`);
}
// Validate the explicit source before replacing any generated output. Local
// upstream previews use the same generator as the pinned shipping submodule.
await readFile(path.join(source, 'dsp', 'CMakeLists.txt'), 'utf8');
await readFile(path.join(source, 'effetune.html'), 'utf8');

await rm(output, { recursive: true, force: true });
await mkdir(output, { recursive: true });

for (const entry of ['js', 'plugins', 'presets', 'images', 'css']) {
  await cp(path.join(source, entry), path.join(output, entry), { recursive: true });
}
// Omit the player-lyrics system preset without touching saved or imported layouts.
const visualizerPresetsDirectory = path.join(output, 'presets', 'visualizer');
for (const entry of await readdir(visualizerPresetsDirectory)) {
  if (!entry.endsWith('.json')) continue;
  const presetPath = path.join(visualizerPresetsDirectory, entry);
  const presets = JSON.parse(await readText(presetPath));
  if (!Object.hasOwn(presets, 'Lyric Stage')) continue;
  delete presets['Lyric Stage'];
  await writeFile(presetPath, JSON.stringify(presets, null, 2) + '\n', 'utf8');
}
// Shared modal/controller styling lives in the upstream library stylesheet.
for (const entry of ['effetune-mobile.css', 'pipeline-analyzer.css', 'effetune-network-music.css']) {
  await rm(path.join(output, 'css', entry), { force: true });
}
await cp(path.join(projectRoot, 'ui-shim', 'vst-bootstrap.js'),
         path.join(output, 'vst-bootstrap.js'));
await cp(path.join(projectRoot, 'ui-shim', 'vst-audio-manager.js'),
         path.join(output, 'vst-audio-manager.js'));
await mkdir(path.join(output, 'features', 'measurement'), { recursive: true });
await cp(path.join(source, 'features', 'measurement', 'dataStorage.js'),
         path.join(output, 'features', 'measurement', 'dataStorage.js'));
await cp(path.join(source, 'features', 'measurement', 'measurement-model.js'),
         path.join(output, 'features', 'measurement', 'measurement-model.js'));
await mkdir(path.join(output, 'features', 'measurement', 'audio-utils'), { recursive: true });
for (const entry of ['channel-selection.js', 'output-routing.js']) {
  await cp(path.join(source, 'features', 'measurement', 'audio-utils', entry),
           path.join(output, 'features', 'measurement', 'audio-utils', entry));
}
await cp(path.join(projectRoot, 'THIRD-PARTY-NOTICES.txt'),
         path.join(output, 'THIRD-PARTY-NOTICES.txt'));

let html = await readText(path.join(source, 'effetune.html'));
const upstreamDocumentTitle = '<title>EffeTune</title>';
const upstreamHeaderTitle = '<h1>Frieve EffeTune<img';
const upstreamFooterTitle = 'EffeTune version <span id="app-version"></span>';
const upstreamIconAlt = 'alt="EffeTune Icon"';
if (!html.includes(upstreamDocumentTitle) || !html.includes(upstreamHeaderTitle) ||
    !html.includes(upstreamFooterTitle) || !html.includes(upstreamIconAlt)) {
  throw new Error('Unable to locate the upstream EffeTune branding');
}
html = applyProductBranding(html)
  .replace(/\s*<link rel="stylesheet" href="css\/effetune-mobile\.css">/g, '')
  .replace('</head>', '    <link rel="stylesheet" href="css/effetune-library.css">\n</head>')
  .replace(/\s*<link rel="manifest" href="manifest\.json">/g, '')
  .replace(/<meta http-equiv="Content-Security-Policy"[^>]*>/,
    `<meta http-equiv="Content-Security-Policy" content="default-src 'self' blob: data:; script-src 'self' 'unsafe-inline' 'unsafe-eval' blob:; connect-src 'self' blob:; img-src 'self' data: blob:; style-src 'self' 'unsafe-inline' blob:;">`)
  .replace(/\s*<script>\s*\(function\(\) \{\s*var measurementId =[\s\S]*?<\/script>/, '')
  .replace(/\s*<script>\s*\/\/ Apply the Web startup view preference[\s\S]*?<\/script>/, '')
  .replace(/\s*<button class="pipeline-menu-item" id="doubleBlindTestButton"[^>]*>[\s\S]*?<\/button>/, '')
  .replace(/\s*<button\b[^>]*\bid="browserButton"[^>]*>[\s\S]*?<\/button>/, '')
  .replace(/\s*<button class="header-button pipeline-analyzer-button" id="pipelineAnalyzerButton"[\s\S]*?<\/button>/, '')
  .replace(/\s*<aside class="pipeline-analyzer-panel" id="pipelineAnalyzerPanel" hidden><\/aside>/, '')
  .replace(/\s*<script src="js\/vendor\/jsmediatags-3\.9\.5\.min\.js"><\/script>/g, '')
  .replace('</head>', '    <script src="vst-bootstrap.js"></script>\n</head>');
await writeFile(path.join(output, 'effetune.html'), html, 'utf8');
const backupDialogPath = path.join(output, 'js', 'user-data-backup', 'dialog.js');
let backupDialog = await readText(backupDialogPath);
const backupDownload = '        downloadable(output.blob, output.fileName);';
if (backupDialog.split(backupDownload).length - 1 !== 1) {
  throw new Error('Unable to locate the upstream backup download');
}
backupDialog = backupDialog.replace(backupDownload,
  '        await window.__effetuneExportBackup(output.blob, output.fileName, controller.signal);');
await writeFile(backupDialogPath, backupDialog, 'utf8');
const backupArchivePath = path.join(output, 'js', 'user-data-backup', 'archive.js');
const backupArchive = await readText(backupArchivePath);
const zipLoader = "new URL('../vendor/jszip-3.10.2.min.js', import.meta.url)";
if (backupArchive.split(zipLoader).length - 1 !== 1) {
  throw new Error('Unable to locate the upstream JSZip backup loader');
}
for (const entry of await readdir(path.join(output, 'js', 'vendor'))) {
  await rm(path.join(output, 'js', 'vendor', entry), { recursive: true, force: true });
}
await cp(path.join(projectRoot, 'ui-shim', 'vendor', 'jszip-3.10.2.min.js'),
         path.join(output, 'js', 'vendor', 'jszip-3.10.2.min.js'));

const appPath = path.join(output, 'js', 'app.js');
let app = await readText(appPath);
const originalImport = `import { AudioManager } from './audio-manager.js';`;
if (!app.includes(originalImport)) {
  throw new Error('Unable to locate the AudioManager import in js/app.js');
}
app = app.replace(originalImport,
  `import { AudioManager } from '../vst-audio-manager.js';`);
const remoteStartup = `            // LAN remote control (remote-v1); does nothing unless enabled in main.
            import('./remote/remote-control.js')
                .then(module => module.startRemoteControl(window))
                .catch(error => console.warn('Remote control unavailable:', error));
`;
if (app.split(remoteStartup).length !== 2) throw new Error('Unable to locate excluded remote startup');
app = app.replace(remoteStartup, '');
const libraryConstantsImport = `import { normalizeMusicLibraryStartupView } from './library/constants.js';`;
if (!app.includes(libraryConstantsImport)) {
  throw new Error('Unable to locate the music-library constants import');
}
app = app.replace(libraryConstantsImport,
  `const normalizeMusicLibraryStartupView = () => 'tracks';`);
const earlyStartupView = `            // Open the configured startup view before the effect pipeline can paint.
            // The same promise is awaited once startup content has been handled.
            void this.applyStartupViewPreference();
`;
if (app.split(earlyStartupView).length - 1 !== 1) {
  throw new Error('Unable to locate the early startup-view navigation in js/app.js');
}
// Native DSP readiness arrives during initAudio. Keep the final awaited
// navigation, while app-starting still hides the effect pipeline.
app = app.replace(earlyStartupView, '');
const startupViewPreference = /    applyStartupViewPreference\(\) \{[\s\S]*?\n    \}\n\n    async openConfiguredStartupView\(\) \{[\s\S]*?\n    \}\n\n    \/\*\*\n     \* Initialize and build pipeline/;
if (!startupViewPreference.test(app)) {
  throw new Error('Unable to locate the music-library startup preference');
}
app = app.replace(startupViewPreference, `    applyStartupViewPreference() {
        this.startupViewPreferencePromise ??= this.openConfiguredStartupView();
        return this.startupViewPreferencePromise;
    }

    async openConfiguredStartupView() {
        if (this.startupConfig?.startupView === 'visualizer') {
            try {
                if (await this.uiManager?.showVisualizerView?.() && this.startupConfig.visualizerStartupExpanded === true) {
                    this.uiManager.visualizerView.setExpanded(true);
                }
            }
            catch (error) {
                console.error('Error opening Visualizer startup view:', error);
                this.uiManager?.setError?.('Visualizer could not be opened. Please try the Visualizer button.', true);
            }
        }
    }

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
const restoreDoubleBlind = /    async restoreDoubleBlindTestFromUrl\(\) \{[\s\S]*?\n    \}\n\n    setStartupWarning/;
if (!restoreDoubleBlind.test(app)) {
  throw new Error('Unable to locate Double Blind Test URL restoration');
}
app = app.replace(restoreDoubleBlind, `    async restoreDoubleBlindTestFromUrl() {}

    setStartupWarning`);
app = app.replace(/const LIBRARY_STYLESHEET = 'effetune-library\.css';\n/, '');
const initialStartupClass = /function applyInitialStartupViewClass\(config, windowRef = window\) \{[\s\S]*?\n\}\n/;
if (!initialStartupClass.test(app)) {
  throw new Error('Unable to locate the initial library startup class helper');
}
app = app.replace(initialStartupClass, 'function applyInitialStartupViewClass() {}\n');
app = app.replace(`            applyInitialStartupViewClass(config, windowRef);\n`, '');
const publishInitialConfig = `            windowRef.appConfig = config;`;
if (!app.includes(publishInitialConfig)) {
  throw new Error('Unable to locate the initial config publication in js/app.js');
}
app = app.replace(publishInitialConfig, `            if (!windowRef.__effetuneNativeSpectrum) config.spectrumOverlayQuality = 'normal';
${publishInitialConfig}
            if (Number.isInteger(config.columns) && config.columns >= 1 && config.columns <= 8) {
                localStorage.setItem('pipelineColumns', String(config.columns));
            }`);
await writeFile(appPath, app, 'utf8');

const routingDialogPath = path.join(
  output, 'js', 'ui', 'pipeline', 'pipeline-routing-dialog.js');
let routingDialog = await readText(routingDialogPath);
const upstreamChannelOptions = `        // Define channel options - changed for multi-channel support
        const channelOptions = [
            { text: 'Stereo', value: '' },  // Default - process first 2 channels only (null)
            { text: 'All', value: 'A' },    // All channels
            { text: 'Left', value: 'L' },   // Left channel only
            { text: 'Right', value: 'R' },  // Right channel only
            { text: '3+4', value: '34' },   // Channels 3 & 4 as stereo pair
            { text: '5+6', value: '56' },   // Channels 5 & 6 as stereo pair
            { text: '7+8', value: '78' },   // Channels 7 & 8 as stereo pair
            { text: '9+10', value: '910' },
            { text: '11+12', value: '1112' },
            { text: '13+14', value: '1314' },
            { text: '15+16', value: '1516' }
        ];

        // Routing can be prepared before the output device is active.
        for (let i = 3; i <= 16; i++) {
            channelOptions.push({ text: \`Ch \${i}\`, value: String(i) });
        }

        // Fixed-channel hosts expose their supported layout; other hosts may prepare routing offline.
        const outputChannelCount = this.pipelineCore.audioManager?.outputChannelCount ?? 16;
        channelOptions.filter(option => getPluginExecutionChannelMode(option.value || null, outputChannelCount)).forEach(option => {
            const optionEl = document.createElement('option');
            optionEl.value = option.value;
            optionEl.textContent = option.text;
            // Compare plugin.channel with option value
            const currentChannelValue = plugin.channel === null ? '' : plugin.channel;
            optionEl.selected = currentChannelValue === option.value;
            channelSelect.appendChild(optionEl);
        });`;
if (routingDialog.split(upstreamChannelOptions).length - 1 !== 1) {
  throw new Error('Unable to locate the upstream 16-channel routing options');
}
routingDialog = routingDialog.replace(upstreamChannelOptions, `        // The VST engine supports up to eight channels. Keep a restored wider
        // route visible without offering it for a new selection.
        const currentChannelValue = plugin.channel === null ? '' : plugin.channel;
        const channelOptions = [
            { text: 'Stereo', value: '' },
            { text: 'All', value: 'A' },
            { text: 'Left', value: 'L' },
            { text: 'Right', value: 'R' },
            { text: '3+4', value: '34' },
            { text: '5+6', value: '56' },
            { text: '7+8', value: '78' }
        ];
        for (let i = 3; i <= 8; i++) {
            channelOptions.push({ text: \`Ch \${i}\`, value: String(i) });
        }
        if (!channelOptions.some(option => option.value === currentChannelValue)) {
            channelOptions.push({
                text: \`Unavailable in this plug-in (\${currentChannelValue})\`,
                value: currentChannelValue,
                disabled: true
            });
        }

        channelOptions.forEach(option => {
            const optionEl = document.createElement('option');
            optionEl.value = option.value;
            optionEl.textContent = option.text;
            optionEl.disabled = option.disabled === true;
            optionEl.selected = currentChannelValue === option.value;
            channelSelect.appendChild(optionEl);
        });`);
await writeFile(routingDialogPath, routingDialog, 'utf8');

const visualizerViewPath = path.join(output, 'js', 'visualizer', 'visualizer-view.js');
let visualizerView = await readText(visualizerViewPath);
const playerMetadata = /    metadata\(\) \{[\s\S]*?\n    \}(?=\n    lyrics\()/;
const playerLyrics = /    lyrics\(\) \{[\s\S]*?\n    \}(?=\n    playbackTarget\()/;
if (!playerMetadata.test(visualizerView) || !playerLyrics.test(visualizerView)) {
  throw new Error('Unable to locate the Visualizer metadata provider');
}
visualizerView = visualizerView.replace(playerMetadata, `    metadata() {
        return this.uiManager.audioManager.getNowPlayingMetadata();
    }`).replace(playerLyrics, '    lyrics() { return null; }');
// The native host owns project transport. Neither the excluded player nor an
// editor-local wall clock can represent stopped playback, seeks, or loops.
const counterProvider = /    counters\(now = performance\.now\(\)\) \{[\s\S]*?\n    \}\n(?=    resetCounter)/;
const videoProvider = /    videoFrame\(\) \{[\s\S]*?\n    \}\n(?=    async openPresets)/;
if (!counterProvider.test(visualizerView) || !videoProvider.test(visualizerView)) {
  throw new Error('Unable to locate the Visualizer transport and video providers');
}
visualizerView = visualizerView.replace(counterProvider, `    counters(now = performance.now()) {
        const transport = this.uiManager.audioManager.getHostTransport();
        return new Map(this.layout.items.filter(item => item.type === 'counter').map(item => [item.id, {
            time: transport ? Math.max(0, transport.projectTimeSamples / transport.sampleRate) : 0,
            trackIndex: 0, external: false, playing: transport?.playing === true,
            speed: 1, source: 'vst-host-project', stamp: now / 1000
        }]));
    }
`).replace(videoProvider, '    videoFrame() { return null; }\n');
for (const integration of [
  "import { SynchronizedVideo } from '../ui/audio-player/synchronized-video.js';\n",
  "import { formatTrackInformation, getTrackInformation } from '../ui/audio-player/track-information.js';\n",
  "import { hasPlaybackRegionDescriptor } from '../ui/audio-player/playback-region.js';\n",
  "import { getMidiLyricsData } from '../midi/lyrics-timeline.js';\n",
  '        this.video = new SynchronizedVideo();\n',
  "        this.video.setEnabled(this.layout.items.some(item => item.type === 'video'));\n",
  '        this.video.setActive(!!this.feed?.visible);\n',
  '        this.video.setActive(visible || !!this.feed?.visible);\n'
]) {
  if (visualizerView.split(integration).length !== 2) {
    throw new Error('Unable to locate an excluded Visualizer video integration');
  }
  visualizerView = visualizerView.replace(integration, '');
}
const visualizerMidiProvider = /        this\.sources = new VisualizerSources\(uiManager\.audioManager,\n[\s\S]*?\n(?=        this\.currentPresetName)/;
if (!visualizerMidiProvider.test(visualizerView)) throw new Error('Unable to locate the Visualizer MIDI source');
visualizerView = visualizerView.replace(visualizerMidiProvider, `        this.sources = new VisualizerSources(uiManager.audioManager,
            () => uiManager.audioManager.getLiveMidiPresentation?.() ?? null);
`);
await writeFile(visualizerViewPath, visualizerView, 'utf8');

const visualizerMidiPath = path.join(output, 'js', 'visualizer', 'visualizer-midi.js');
let visualizerMidi = await readText(visualizerMidiPath);
const liveMidiBeat = '                beat: null, chord: inferMidiChord(notes) };';
const liveMidiClock = '                position: presentation.position, playing: true, paused: false, speed: 1,';
if (visualizerMidi.split(liveMidiBeat).length !== 2 || visualizerMidi.split(liveMidiClock).length !== 2) {
  throw new Error('Unable to locate the Visualizer live MIDI clock');
}
visualizerMidi = visualizerMidi.replace(liveMidiClock, `                position: presentation.position, playing: presentation.playing, paused: presentation.paused, speed: 1,`)
  .replace(liveMidiBeat, `                beat: presentation.beat, beatAt: presentation.beatAt, chord: inferMidiChord(notes) };`);
await writeFile(visualizerMidiPath, visualizerMidi, 'utf8');

const visualizerSourcesPath = path.join(output, 'js', 'visualizer', 'visualizer-sources.js');
const visualizerSources = await readText(visualizerSourcesPath);
const eventListProvider = /    getMidiEventList\(\) \{[\s\S]*?\n    \}(?=\n\n    _syncMidiMixerMetering)/;
if (!eventListProvider.test(visualizerSources)) throw new Error('Unable to locate the Visualizer MIDI event list source');
await writeFile(visualizerSourcesPath, visualizerSources.replace(eventListProvider, `    getMidiEventList() {
        const presentation = this.getMidiPresentation();
        return presentation?.eventRows ? { live: true, rows: presentation.eventRows,
            position: presentation.eventRows.at(-1)?.seconds ?? 0 } : null;
    }`).replace("import { getMidiEventList } from '../midi/event-list.js';\n", ''), 'utf8');

const midiEventListPath = path.join(output, 'js', 'visualizer', 'visualizer-midi-event-list.js');
const midiEventList = await readText(midiEventListPath);
const eventListPosition = 'cell(row.position, 1, y, position.flatMap((text, index) => [';
const eventListHeading = 'COLUMNS.forEach((label, column) => cell(label, column, headerHeight / 2));';
if (!midiEventList.includes(eventListPosition) || !midiEventList.includes(eventListHeading)) {
  throw new Error('Unable to locate the Visualizer MIDI event list position display');
}
await writeFile(midiEventListPath, midiEventList
  .replace(eventListPosition, 'cell(row.position, 1, y, data?.live ? null : position.flatMap((text, index) => [')
  .replace(eventListHeading, `COLUMNS.forEach((label, column) => cell(data?.live && column === 1 ? 'Time (s)' : label, column, headerHeight / 2));`), 'utf8');

const visualizerRendererPath = path.join(output, 'js', 'visualizer', 'visualizer-renderer.js');
let visualizerRenderer = await readText(visualizerRendererPath);
for (const integration of [
  "import { isPwaNetworkArtworkUrl } from '../network-music/pwa-connection.js';\n",
  "            if (isPwaNetworkArtworkUrl(url)) image.crossOrigin = 'anonymous';\n"
]) {
  if (visualizerRenderer.split(integration).length !== 2) {
    throw new Error('Unable to locate excluded network artwork integration');
  }
  visualizerRenderer = visualizerRenderer.replace(integration, '');
}
await writeFile(visualizerRendererPath, visualizerRenderer, 'utf8');

const tourPath = path.join(output, 'js', 'ui', 'onboarding-tour.js');
let tour = await readText(tourPath);
for (const [original, replacement] of [
  ["    { id: 'browser', target: '#browserButton' },\n", ''],
  ["{ id: 'sound', target: '#openMusicButton', mobileTarget: '#mobilePlayerView .mobile-open-music', mobileBody: true, view: 'player' }", "{ id: 'sound', target: '#pipeline' }"],
  ["{ id: 'views', target: '#openLibraryButton', mobileTarget: '.mobile-bottom-button[data-view=\"library\"]', mobileBody: true }", "{ id: 'views', target: '#visualizerButton' }"],
  ["{ id: 'help', target: '#whatsThisLink', mobileTarget: null, mobileBody: true }", "{ id: 'help', target: '#settingsMenuButton' }"]
]) {
  if (tour.split(original).length !== 2) throw new Error('Unable to adapt an upstream tour target');
  tour = tour.replace(original, replacement);
}
await writeFile(tourPath, tour, 'utf8');

const eventHandlerPath = path.join(output, 'js', 'ui', 'pipeline', 'ui-event-handler.js');
let eventHandler = await readText(eventHandlerPath);
const musicAttachmentsImport = "import { attachLyricsFiles, attachArtworkFiles } from '../audio-player/lyrics-file-links.js';\n";
const musicExtensions = /^import \{ SUPPORTED_AUDIO_EXTENSIONS \} from '\.\.\/\.\.\/library\/constants\.js';\n\nconst PLAYBACK_AUDIO_EXTENSIONS[\s\S]*?\n\}(?=\n\nfunction isPlaylistFile)/m;
const musicFiles = /                    \/\/ Check for music files\n                    const musicFiles = files\.filter\(isSupportedPlaybackAudioFile\);\n/;
const musicDrop = /                    \/\/ Process music files \(if any and no preset files\)\n[\s\S]*?\n                    \}(?=\s*return false;)/;
if (eventHandler.split(musicAttachmentsImport).length !== 2 || !musicDrop.test(eventHandler) ||
    !musicExtensions.test(eventHandler) || !musicFiles.test(eventHandler)) {
  throw new Error('Unable to locate the excluded music lyrics attachment workflow');
}
eventHandler = eventHandler.replace(musicAttachmentsImport, '').replace(musicDrop, '')
  .replace(musicExtensions, 'const PLAYLIST_EXTENSION_PATTERN = /\\.(m3u8?|pls|xspf)$/i;')
  .replace(musicFiles, '');
await writeFile(eventHandlerPath, eventHandler, 'utf8');

const visualizerEditorPath = path.join(output, 'js', 'visualizer', 'visualizer-editor.js');
let visualizerEditor = await readText(visualizerEditorPath);
const visualizerAddChoices = 'byLabel(ITEM_TYPES.map(type =>';
if (visualizerEditor.split(visualizerAddChoices).length - 1 !== 1) {
  throw new Error('Unable to locate Visualizer add-item choices');
}
visualizerEditor = visualizerEditor.replace(visualizerAddChoices,
  "byLabel(ITEM_TYPES.filter(type => !['lyrics', 'video', 'playback-control', 'seek-bar', 'track-info'].includes(type)).map(type =>");
const visualizerChannels = /^            const channels = \[.*?\];\n(?=[\s\S]*?this\.field\(properties, this\.t\('visualizer\.channel')/gm;
if ([...visualizerEditor.matchAll(visualizerChannels)].length !== 1) {
  throw new Error('Unable to locate Visualizer channel choices');
}
visualizerEditor = visualizerEditor.replace(visualizerChannels, `            const channels = [['', '1–2'], ['L', 'L'], ['R', 'R'], ...Array.from({ length: 3 }, (_, i) => [\`\${i * 2 + 3}\${i * 2 + 4}\`, \`\${i * 2 + 3}–\${i * 2 + 4}\`]), ...Array.from({ length: 8 }, (_, i) => [\`\${i + 1}\`, \`\${i + 1}\`])];
            const unavailableChannel = item.channel && !channels.some(([value]) => value === item.channel);
            if (unavailableChannel) channels.push([item.channel, \`Unavailable in this plug-in (\${item.channel})\`]);
`).replace("            this.field(properties, this.t('visualizer.channel', 'Channel'),", `            const channelSelect = this.field(properties, this.t('visualizer.channel', 'Channel'),`)
  .replace('{ values: channels, disabled: midiMode });', `{ values: channels, disabled: midiMode });
            if (unavailableChannel) {
                for (const option of channelSelect.options) option.disabled = option.value === item.channel;
            }`);
await writeFile(visualizerEditorPath, visualizerEditor, 'utf8');

const pitchMeterPath = path.join(output, 'plugins', 'analyzer', 'pitch_meter.js');
let pitchMeter = await readText(pitchMeterPath);
const pitchMeterTelemetryStart = `        const snapshot = this.parseTelemetryFrame(frame);
        if (!snapshot) return;
        if (this.activeGeneration === null) {`;
if (pitchMeter.split(pitchMeterTelemetryStart).length - 1 !== 1) {
  throw new Error('Unable to locate the Pitch Meter telemetry epoch boundary');
}
// Native topology rebuilds replace kernels while retaining the UI and tap ID.
// Frame counters, generations and time fences belong to the physical source.
pitchMeter = pitchMeter.replace(pitchMeterTelemetryStart, `        const snapshot = this.parseTelemetryFrame(frame);
        if (!snapshot) return;
        if (frame.source && frame.source !== this.telemetrySource) {
            this.telemetrySource = frame.source;
            this.activeGeneration = null;
            this.generationFence = null;
            this.timeFence = null;
            this.clearHistory();
        }
        if (this.activeGeneration === null) {`);
await writeFile(pitchMeterPath, pitchMeter, 'utf8');

const fifteenBandGeqPath = path.join(output, 'plugins', 'eq', 'fifteen_band_geq.js');
let fifteenBandGeq = await readText(fifteenBandGeqPath);
const fifteenBandGeqRefresh = `            if (this.sliders && this.sliders[i]) {
                this.sliders[i].value = this['b' + i];
                window.uiManager?.refreshRangeFillStyling?.(this.sliders[i]);
            }
            if (this.valueDisplays && this.valueDisplays[i]) {
                this.valueDisplays[i].textContent = this['b' + i].toFixed(1) + ' dB';
            }`;
if (fifteenBandGeq.split(fifteenBandGeqRefresh).length - 1 !== 1) {
  throw new Error('Unable to locate the Fifteen Band GEQ refresh pair');
}
fifteenBandGeq = fifteenBandGeq.replace(fifteenBandGeqRefresh, `            const slider = this.sliders?.[i];
            const valueDisplay = this.valueDisplays?.[i];
            if (this.isHeldByUser(slider) || this.isHeldByUser(valueDisplay)) continue;
            if (slider) {
                slider.value = this['b' + i];
                window.uiManager?.refreshRangeFillStyling?.(slider);
            }
            if (valueDisplay) {
                valueDisplay.textContent = this['b' + i].toFixed(1) + ' dB';
            }`);
await writeFile(fifteenBandGeqPath, fifteenBandGeq, 'utf8');

const tiltEqPath = path.join(output, 'plugins', 'eq', 'tilt_eq.js');
let tiltEq = await readText(tiltEqPath);
const tiltEqPivotRefresh = `            pivotLogSlider.value = this.f0;
            window.uiManager?.refreshRangeFillStyling?.(pivotLogSlider);
            // The Hz box only commits on change/Enter, so a half-typed entry lives
            // in the DOM and nowhere else; overwriting it mid-edit would discard it.
            if (!this.isHeldByUser(pivotHzInput)) {
                pivotHzInput.value = Math.round(Math.exp(this.f0));
            }`;
if (tiltEq.split(tiltEqPivotRefresh).length - 1 !== 1) {
  throw new Error('Unable to locate the Tilt EQ pivot refresh pair');
}
tiltEq = tiltEq.replace(tiltEqPivotRefresh, `            const pivotFrequencyHeld =
                this.isHeldByUser(pivotLogSlider) || this.isHeldByUser(pivotHzInput);
            if (!pivotFrequencyHeld) {
                pivotLogSlider.value = this.f0;
                window.uiManager?.refreshRangeFillStyling?.(pivotLogSlider);
                pivotHzInput.value = Math.round(Math.exp(this.f0));
            }`);
await writeFile(tiltEqPath, tiltEq, 'utf8');

const pluginManagerPath = path.join(output, 'js', 'plugin-manager.js');
let pluginManager = await readText(pluginManagerPath);
const pluginManagerClass = 'export class PluginManager {';
const pluginAvailability = '    isPluginAvailable(name) {\n';
const pluginLookup = '        const PluginClass = this.pluginClasses[name];';
if ([pluginManagerClass, pluginAvailability, pluginLookup].some(anchor => pluginManager.split(anchor).length !== 2)) {
  throw new Error('Unable to locate plug-in availability and creation for the SoundFont Note Player alias');
}
pluginManager = pluginManager.replace(pluginManagerClass, `// Existing states and presets can refer to the instrument by its old name or stable DSP type.
function resolvePluginName(name) {
    return name === 'SFZ Note Player' || name === 'SFZNotePlayerPlugin' ? 'SoundFont Note Player' : name;
}

${pluginManagerClass}`)
  .replace(pluginAvailability, `${pluginAvailability}        name = resolvePluginName(name);\n`)
  .replace(pluginLookup, '        const PluginClass = this.pluginClasses[resolvePluginName(name)];');
await writeFile(pluginManagerPath, pluginManager, 'utf8');

const historyPath = path.join(output, 'js', 'ui', 'pipeline', 'history-manager.js');
let history = await readText(historyPath);
const historySerialization =
  `this.pipelineManager.core.getSerializablePluginState(plugin, true, false, false)`;
const historySerializationMatches = history.split(historySerialization).length - 1;
if (historySerializationMatches !== 1) {
  throw new Error(`Expected one shared history serialization site, found ${historySerializationMatches}`);
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

const itemBuilderPath = path.join(output, 'js', 'ui', 'pipeline', 'pipeline-item-builder.js');
let itemBuilder = await readText(itemBuilderPath);
const saveUIState = `    saveUIState() {
        this.pipelineManager?.historyManager?.saveState();`;
if (itemBuilder.split(saveUIState).length !== 2) {
  throw new Error('Unable to locate effect UI state persistence');
}
itemBuilder = itemBuilder.replace(saveUIState, `    saveUIState() {
        this.audioManager.persistPluginUIState();
        this.pipelineManager?.historyManager?.saveState();`);
await writeFile(itemBuilderPath, itemBuilder, 'utf8');

const sectionHandlerPath = path.join(output, 'js', 'ui', 'pipeline', 'pipeline-section-handler.js');
let sectionHandler = await readText(sectionHandlerPath);
const sectionUIState = `        this.updateAllTooltips();
        this.pipelineManager?.historyManager?.saveState();
        this.pipelineCore.updateURL();`;
if (sectionHandler.split(sectionUIState).length !== 2) {
  throw new Error('Unable to locate section UI state persistence');
}
sectionHandler = sectionHandler.replace(sectionUIState, `        this.updateAllTooltips();
        this.pipelineCore.itemBuilder.saveUIState();`);
await writeFile(sectionHandlerPath, sectionHandler, 'utf8');

// Only Web MIDI is available in the VST WebView. Drop the inactive Android
// integration before it pulls the excluded music player into the module graph.
const midiManagerPath = path.join(output, 'js', 'midi', 'midi-controller-manager.js');
let midiManager = await readText(midiManagerPath);
for (const [original, replacement] of [
  ["import { isAndroidHost, getAndroidBridge } from '../android/bridge.js';\n", ''],
  ["import { requestAndroidMidiAccess, prepareAndroidMidiConfiguration, subscribeAndroidMidiActions } from '../android/midi.js';\n", ''],
  ['    if (isAndroidHost(this.window)) this.unsubscribeNativeActions = subscribeAndroidMidiActions(this.window);\n', ''],
  ['isAndroidHost(this.window) ? [] : this.store.mappings', 'this.store.mappings'],
  ["isAndroidHost(this.window) || typeof this.navigator?.requestMIDIAccess === 'function'", "typeof this.navigator?.requestMIDIAccess === 'function'"],
  [`    if (isAndroidHost(this.window)) {
      await this.syncNativeConfiguration();
      if (this.disposed) return;
    }
`, ''],
  [`  syncNativeConfiguration() {
    const config = prepareAndroidMidiConfiguration(this.store, this.adapter, this.learnActive);
    const pending = (this.nativeConfigPromise || Promise.resolve()).catch(() => {}).then(() =>
      getAndroidBridge().request('midi.configure', config));
    this.nativeConfigPromise = pending;
    return pending;
  }

`, ''],
  [`request = isAndroidHost(this.window) ? requestAndroidMidiAccess()
        : Promise.resolve(this.navigator.requestMIDIAccess({ sysex }));`,
    'request = Promise.resolve(this.navigator.requestMIDIAccess({ sysex }));'],
  ['    const wasLearning = this.learnActive;\n', ''],
  [`    if (wasLearning && !this.disposed && isAndroidHost(this.window)) {
      void this.syncNativeConfiguration().catch(error => console.error('Android MIDI learn cleanup failed:', error));
    }
`, '']
]) {
  if (!midiManager.includes(original)) throw new Error('Unable to adapt Android MIDI integration');
  midiManager = midiManager.replaceAll(original, replacement);
}
await writeFile(midiManagerPath, midiManager, 'utf8');
await rm(path.join(output, 'js', 'android', 'midi.js'), { force: true });

// Controller targets share a pure player-speed utility even when no player is packaged.
await cp(path.join(source, 'js', 'ui', 'audio-player', 'playback-speed.js'),
         path.join(output, 'js', 'midi', 'playback-speed.js'));
const appTargetsPath = path.join(output, 'js', 'midi', 'app-targets.js');
let appTargets = await readText(appTargetsPath);
const speedImport = "from '../ui/audio-player/playback-speed.js'";
if (appTargets.split(speedImport).length - 1 !== 1) {
  throw new Error('Expected one controller playback-speed utility import');
}
await writeFile(appTargetsPath, appTargets.replace(speedImport, "from './playback-speed.js'"), 'utf8');
const controllerDialogPath = path.join(output, 'js', 'midi', 'midi-mapping-dialog.js');
let controllerDialog = await readText(controllerDialogPath);
const appChoices = 'for (const param of Object.keys(APP_TARGETS)) {';
if (controllerDialog.split(appChoices).length - 1 !== 1) {
  throw new Error('Expected one controller application target choice list');
}
controllerDialog = controllerDialog.replace(appChoices,
  "for (const param of ['masterBypass', 'abToggle', 'preset']) {");
await writeFile(controllerDialogPath, controllerDialog, 'utf8');

// The Visualizer uses a dependency-free viewport helper shared with the library UI.
await cp(path.join(source, 'js', 'ui', 'library', 'library-view-shared.js'),
         path.join(output, 'js', 'ui', 'visualizer-shared.js'));
const viewportImport = "from '../ui/library/library-view-shared.js'";
if (visualizerEditor.split(viewportImport).length - 1 !== 1) {
  throw new Error('Expected one Visualizer viewport helper import');
}
await writeFile(visualizerEditorPath,
  visualizerEditor.replace(viewportImport, "from '../ui/visualizer-shared.js'"), 'utf8');

const pipelineManagerPath = path.join(output, 'js', 'ui', 'pipeline-manager.js');
let pipelineManager = await readText(pipelineManagerPath);
const fileProcessorImport = `import { FileProcessor } from './pipeline/file-processor.js';\n`;
const fileProcessorConstruction = `        this.fileProcessor = enableFileProcessing ? new FileProcessor(this) : null;`;
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
let uiManager = await readText(uiManagerPath);
const networkMusicInitializer = '            void this.initializeNetworkMusic();\n';
const networkMusicMethod = /    async initializeNetworkMusic\(\) \{[\s\S]*?\n    \}\n/;
if (uiManager.split(networkMusicInitializer).length !== 2 || !networkMusicMethod.test(uiManager)) {
  throw new Error('Unable to locate the excluded network music initialization');
}
uiManager = uiManager.replace(networkMusicInitializer, '').replace(networkMusicMethod, '');
await rm(path.join(output, 'js', 'network-music'), { recursive: true, force: true });
const browserImport = "import { BrowserView } from './browser/browser-view.js';\n";
const browserProvider = /    showBrowserView\(\) \{[\s\S]*?\n    \}\n(?=\n    updateViewSwitchButtons)/;
if (uiManager.split(browserImport).length !== 2 || !browserProvider.test(uiManager)) {
  throw new Error('Unable to locate the excluded desktop Browser integration');
}
uiManager = uiManager.replace(browserImport, '')
  .replace(browserProvider, '    showBrowserView() { return Promise.resolve(false); }\n');
const browserButtonSetup = /        this\.browserButton = document\.getElementById\('browserButton'\);\n        if \(this\.browserButton && window\.electronAPI\?\.browser\) \{[\s\S]*?\n        \}\n/;
if (!browserButtonSetup.test(uiManager)) {
  throw new Error('Unable to locate the desktop Browser button setup');
}
uiManager = uiManager.replace(browserButtonSetup, '');
const pipelineManagerConstruction = `        this.pipelineManager = new PipelineManager(audioManager, pluginManager, this.expandedPlugins, this.pluginListManager);`;
if (uiManager.split(pipelineManagerConstruction).length - 1 !== 1) {
  throw new Error('Unable to locate the PipelineManager construction');
}
uiManager = uiManager.replace(pipelineManagerConstruction,
  `        this.pipelineManager = new PipelineManager(audioManager, pluginManager, this.expandedPlugins, this.pluginListManager, { enableFileProcessing: false });`);
const pipelinePerformanceRefresh = `        this.updatePipelineCpuUsage(this.pipelineCpuAveragePercent);`;
if (uiManager.split(pipelinePerformanceRefresh).length - 1 !== 1) {
  throw new Error('Unable to locate the pipeline performance status refresh');
}
uiManager = uiManager.replace(pipelinePerformanceRefresh, `        this.updatePipelineCpuUsage(
            this.audioManager?.pipelineCpuAveragePercent ?? this.pipelineCpuAveragePercent
        );`);
const pipelineAnalyzerInitialization = `        this.initPipelineAnalyzerBootstrap();
        this.initPipelineAnalyzerMenuIntegration();`;
if (uiManager.split(pipelineAnalyzerInitialization).length - 1 !== 1) {
  throw new Error('Unable to locate the Pipeline Analyzer initialization');
}
uiManager = uiManager.replace(pipelineAnalyzerInitialization, `        // Pipeline Analyzer replays the JavaScript DSP pipeline in a Web Worker.
        // That is not the VST native DSP path, so the VST build must not expose it
        // or restore a saved open state that starts analysis in the background.
        this.pipelineAnalyzerUI = null;
        this.pipelineAnalyzerController = null;`);

// EffeTune 2.8.0 defers optional features behind module loaders.  The VST
// bundle removes those feature trees, so remove the loaders as well rather
// than leaving dynamic imports that make the deleted modules reachable.
for (const [name, nextName] of [
  ['loadAudioPlayerClass', 'loadLibraryFeatureModules'],
  ['loadLibraryFeatureModules', 'loadWebPlaybackResolvers'],
  ['loadWebPlaybackResolvers', 'loadPipelineAnalyzerModules'],
  ['loadPipelineAnalyzerModules', 'loadDoubleBlindTestClass']
]) {
  const loader = new RegExp(`function ${name}\\(\\) \\{[\\s\\S]*?\\n\\}\\n\\n(?=function ${nextName})`);
  if (!loader.test(uiManager)) {
    throw new Error(`Unable to locate excluded lazy loader: ${name}`);
  }
  uiManager = uiManager.replace(loader, '');
}
const doubleBlindLoader = /function loadDoubleBlindTestClass\(\) \{[\s\S]*?\n\}\n\n(?=function isPipelineAnalyzerStoredOpen)/;
if (!doubleBlindLoader.test(uiManager)) {
  throw new Error('Unable to locate excluded lazy loader: loadDoubleBlindTestClass');
}
uiManager = uiManager.replace(doubleBlindLoader, '');
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
  `import { attachLyricsFiles, attachArtworkFiles } from './ui/audio-player/lyrics-file-links.js';\n`,
  `import { getDsdPlaybackErrorKey } from './ui/audio-player/dsd-source.js';\n`,
  `import { SCORE_EXTENSIONS } from './midi/score-file.js';\n`,
  `import { PipelineAnalyzerController } from './pipeline-analyzer/controller.js';\n`,
  `import { PipelineAnalyzerUI } from './pipeline-analyzer/ui.js';\n`,
  `import { AudioPlayer } from './ui/audio-player.js';\n`,
  `import { LibraryManagerV2 } from './library/library-manager-v2.js';\n`,
  `import { createWebCatalogRecoveryController } from './library/repository/catalog-client-factory.js';\n`,
  `import { LibraryView } from './ui/library/library-view.js';\n`,
  `import { CatalogPlaybackBridge } from './ui/audio-player/catalog-playback-bridge.js';\n`,
  `import { resolveWebPlaybackSelection } from './ui/playback-selection-router.js';\n`,
  `import { resolveWebCueSiblingFiles } from './ui/web-cue-source-resolver.js';\n`
]) {
  if (uiManager.includes(excludedImport)) uiManager = uiManager.replace(excludedImport, '');
}
const musicInitializers = `        this.initOpenMusicButton();
        this.initOpenLibraryButton();
        this.initLibraryRecovery();`;
if (!uiManager.includes(musicInitializers)) {
  throw new Error('Unable to locate the music/library UI initializers');
}
uiManager = uiManager.replace(musicInitializers,
  `        this.initOpenLibraryButton();`);
// The shared initializer owns Effects/Visualizer navigation as well as Library.
const viewNavigationInitializer = /    initOpenLibraryButton\(\) \{[\s\S]*?\n    \}\n/;
if (!viewNavigationInitializer.test(uiManager)) {
  throw new Error('Unable to locate shared view navigation initialization');
}
uiManager = uiManager.replace(viewNavigationInitializer, `    initOpenLibraryButton() {
        this.effectPipelineButton = document.getElementById('effectPipelineButton');
        this.visualizerButton = document.getElementById('visualizerButton');
        this.visualizerButton?.addEventListener('click', () => this.showVisualizerView());
        this.effectPipelineButton?.addEventListener('click', event => {
            this.showEffectPipelineView({ returnFocus: event.currentTarget });
        });
    }
`);
const libraryRecoveryInitialization = `        this.libraryRecoveryApi = window.electronAPI?.libraryRecoveryV1 || null;
        this.libraryRecoveryState = {
            apiVersion: 1,
            status: 'initializing',
            available: false,
            canReset: false
        };`;
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
uiManager = uiManager.replace("        loadStylesheet('effetune-library.css');\n", '');
const webLibraryRecoveryMethod = /    async ensureWebLibraryRecoveryController\(\) \{[\s\S]*?\n    \}\n\n    deferLibraryStartupView\(initialView\)/;
if (!webLibraryRecoveryMethod.test(uiManager)) {
  throw new Error('Unable to locate current web-library recovery loader');
}
uiManager = uiManager.replace(webLibraryRecoveryMethod,
  `    async ensureWebLibraryRecoveryController() { return null; }

    deferLibraryStartupView(initialView)`);
const libraryManagerMethod = /    async ensureLibraryManager\(options = \{\}\) \{[\s\S]*?\n    \}\n\n    createLibraryManager\(\)/;
if (!libraryManagerMethod.test(uiManager)) {
  throw new Error('Unable to locate current lazy library manager');
}
uiManager = uiManager.replace(libraryManagerMethod,
  `    async ensureLibraryManager() { return null; }

    createLibraryManager()`);
const audioPlayerMethod = /    async createAudioPlayer\(filePaths, replaceExisting = false\) \{[\s\S]*?\n    \}\n\n    \/\*\*\n     \* Load a preset/;
if (!audioPlayerMethod.test(uiManager)) {
  throw new Error('Unable to locate current lazy audio-player method');
}
uiManager = uiManager.replace(audioPlayerMethod,
  `    async createAudioPlayer() { return null; }

    /**
     * Load a preset`);
const webPlaybackSelectionMethod = /    async openWebPlaybackSelection\(files, gestureResume, \{ fileHandles = null \} = \{\}\) \{[\s\S]*?\n    \}\n\n    \/\*\*\n     \* Initialize music library button/;
if (!webPlaybackSelectionMethod.test(uiManager)) {
  throw new Error('Unable to locate current web playback selection method');
}
const webMusicPickerConstants = /^const WEB_MUSIC_FILE_ACCEPT = [\s\S]*?^\}\];\n/m;
const webMusicPicker = /    initOpenMusicButton\(\) \{[\s\S]*?(?=    async openWebPlaybackSelection\()/;
if (!webMusicPickerConstants.test(uiManager) || !webMusicPicker.test(uiManager)) {
  throw new Error('Unable to locate the excluded music file picker');
}
uiManager = uiManager.replace(webMusicPickerConstants, '').replace(webMusicPicker, '');
uiManager = uiManager.replace(webPlaybackSelectionMethod,
  `    async openWebPlaybackSelection() { return false; }

    /**
     * Initialize music library button`);
const libraryShortcut = /\n            if \(\(e\.ctrlKey \|\| e\.metaKey\) && !e\.shiftKey && !e\.altKey && String\(e\.key\)\.toLowerCase\(\) === 'l'\) \{[\s\S]*?\n            \}\n/;
if (!libraryShortcut.test(uiManager)) {
  throw new Error('Unable to locate the music-library keyboard shortcut');
}
uiManager = uiManager.replace(libraryShortcut, '\n');
const doubleBlindImport = `import { DoubleBlindTest } from './ui/double-blind-test/double-blind-test.js';\n`;
if (uiManager.includes(doubleBlindImport)) uiManager = uiManager.replace(doubleBlindImport, '');
const doubleBlindMethods = /    \/\*\*\n     \* Get \(creating on first use\)[\s\S]*?    async getDoubleBlindTest\(\) \{[\s\S]*?\n    \}\n\n    \/\*\* Is the Double Blind Test mode currently open\? \*\/\n    isDoubleBlindActive\(\) \{[\s\S]*?\n    \}/;
if (!doubleBlindMethods.test(uiManager)) {
  throw new Error('Unable to locate Double Blind Test UI methods');
}
uiManager = uiManager.replace(doubleBlindMethods, `    async getDoubleBlindTest() { return null; }

    isDoubleBlindActive() { return false; }`);
const pipelineAnalyzerMethods = /    setPipelineAnalyzerOpen\(open\) \{[\s\S]*?\n    \}\n\n    updateURL\(\)/;
if (!pipelineAnalyzerMethods.test(uiManager)) {
  throw new Error('Unable to locate current Pipeline Analyzer methods');
}
uiManager = uiManager.replace(pipelineAnalyzerMethods, `    setPipelineAnalyzerOpen() { return false; }

    initPipelineAnalyzerBootstrap() {}

    ensurePipelineAnalyzer() { return Promise.resolve(null); }

    isPipelineAnalyzerOpen() { return false; }

    initPipelineAnalyzerMenuIntegration() {}

    disposePipelineAnalyzerIntegration() {}

    updateURL()`);
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
// The A/B action availability policy is shared with ordinary pipeline actions.
await mkdir(path.join(output, 'js', 'ui', 'double-blind-test'), { recursive: true });
await cp(path.join(source, 'js', 'ui', 'double-blind-test', 'pipeline-availability.js'),
         path.join(output, 'js', 'ui', 'double-blind-test', 'pipeline-availability.js'));
await rm(path.join(output, 'js', 'browser'), { recursive: true, force: true });

const electronIntegrationPath = path.join(output, 'js', 'electron-integration.js');
let electronIntegration = await readText(electronIntegrationPath);
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
  const assetSource = await readText(assetPath);
  await writeFile(assetPath, applyProductBranding(assetSource), 'utf8');
}

const localesPath = path.join(output, 'js', 'locales');
for (const localeFile of await readdir(localesPath)) {
  if (!localeFile.endsWith('.json5')) continue;
  const localePath = path.join(localesPath, localeFile);
  const localeSource = await readText(localePath);
  let localized = applyProductBranding(localeSource);
  // Other locales use the supported English workflow until translated here.
  for (const [key, value] of Object.entries(vstTourLocales[path.basename(localeFile, '.json5')] || vstTourLocales.en)) {
    const field = `${JSON.stringify(key)}:`;
    const expression = new RegExp(`(${field.replaceAll('.', '\\.')}\\s*)"(?:[^"\\\\]|\\\\.)*"`);
    if (!expression.test(localized)) throw new Error(`Unable to adapt tour locale: ${localeFile}: ${key}`);
    localized = localized.replace(expression, (_match, prefix) => prefix + JSON.stringify(value));
  }
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
let browserAudioManager = await readText(browserAudioManagerPath);
const audioEncoderImport = `import { AudioEncoder } from './audio/audio-encoder.js';\n`;
if (browserAudioManager.includes(audioEncoderImport)) {
  browserAudioManager = browserAudioManager.replace(audioEncoderImport, '');
}
const audioEncoderConstruction = `        this.audioEncoder = new AudioEncoder();`;
if (browserAudioManager.includes(audioEncoderConstruction)) {
  browserAudioManager = browserAudioManager.replace(audioEncoderConstruction,
    '        this.audioEncoder = null;');
}
const offlineImport = `import { OfflineProcessor } from './audio/offline-processor.js';\n`;
if (browserAudioManager.includes(offlineImport)) {
  browserAudioManager = browserAudioManager.replace(offlineImport, '');
}
const offlineLoader = /    async _ensureOfflineProcessor\(\) \{[\s\S]*?\n    \}\n/;
if (!offlineLoader.test(browserAudioManager)) {
  throw new Error('Unable to locate current lazy offline processor loader');
}
browserAudioManager = browserAudioManager.replace(offlineLoader,
  `    async _ensureOfflineProcessor() { return null; }
`);
const offlineProcessMethod = /    async processAudioFile\(file, progressCallback = null, outputSettings = null\) \{[\s\S]*?\n    \}\n\n    \/\*\*\n     \* Cancel the current offline audio processing operation/;
if (!offlineProcessMethod.test(browserAudioManager)) {
  throw new Error('Unable to locate current offline processing method');
}
browserAudioManager = browserAudioManager.replace(offlineProcessMethod,
  `    async processAudioFile() { return null; }

    /**
     * Cancel the current offline audio processing operation`);
await writeFile(browserAudioManagerPath, browserAudioManager, 'utf8');

const configIntegrationPath = path.join(output, 'js', 'electron', 'configIntegration.js');
let configIntegration = await readText(configIntegrationPath);
const networkSettingsImport = "import { NetworkMusicSettings } from '../network-music/settings-view.js';\n";
const networkSettingsConstruction = `  const networkSettings = isElectron ? new NetworkMusicSettings({
    root: document.getElementById('config-panel-network'), api: networkMusicAPI, remoteSettings: remoteControlSettings, translate: t
  }) : null;
  await networkSettings?.mount();`;
const networkSettingsRefresh = 'await Promise.all([refreshOpenHomeStatus(), refreshAirPlayStatus(), refreshLibrarySharing()]);';
const browserRulesImport = "import { validateRules } from '../browser/url-rules.js';\n";
const browserRulesSave = /  document.getElementById\('browser-rules-save'\)\?\.addEventListener\('click', async \(\) => \{[\s\S]*?\n  \}\);\n/;
if (configIntegration.split(networkSettingsImport).length !== 2 ||
    configIntegration.split(networkSettingsConstruction).length !== 2 ||
    configIntegration.split(networkSettingsRefresh).length !== 2 ||
    configIntegration.split(browserRulesImport).length !== 2 || !browserRulesSave.test(configIntegration)) {
  throw new Error('Unable to locate excluded network/browser settings dependencies');
}
configIntegration = configIntegration.replace(networkSettingsImport, '')
  .replace(networkSettingsConstruction, '  const networkSettings = null;')
  .replace(networkSettingsRefresh, 'await Promise.all([refreshOpenHomeStatus(), refreshAirPlayStatus()]);')
  .replace(browserRulesImport, '').replace(browserRulesSave, '');
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
const configCategories = 'const categories = Object.keys(categoryPanels);';
const configPanels = '${categories.map(category => `\n          <div class="config-category-panel" id="config-panel-${category}">';
if (configIntegration.split(configCategories).length - 1 !== 1 ||
    configIntegration.split(configPanels).length - 1 !== 1) {
  throw new Error('Unable to locate categorized settings navigation and panels');
}
configIntegration = configIntegration.replace(configCategories,
  "const categories = ['general', 'startup', 'display', 'controllers'];");
// Keep nonselectable panels for upstream initialization and translated control IDs.
configIntegration = configIntegration.replace(configPanels,
  '${Object.keys(categoryPanels).map(category => `\n          <div class="config-category-panel" id="config-panel-${category}" hidden>');
const browserStartup = /            \$\{isElectron \? `<div class="radio-container">\n              <input type="radio" name="startup-view" id="startup-view-browser"[\s\S]*?\n            <\/div>` : ''\}\n/;
const browserCategory = /      browser: `[\s\S]*?`,\n(?=      network:)/;
const startupChoices = "const startupViews = isElectron ? ['effects', 'library', 'visualizer', 'browser'] : ['effects', 'library', 'visualizer'];";
if (!browserStartup.test(configIntegration) || !browserCategory.test(configIntegration) ||
    !configIntegration.includes(startupChoices)) {
  throw new Error('Unable to locate the excluded desktop Browser settings');
}
configIntegration = configIntegration.replace(browserStartup, '').replace(browserCategory, '')
  .replace(startupChoices, "const startupViews = ['effects', 'visualizer'];");
await writeFile(configIntegrationPath, configIntegration, 'utf8');

const clipboardPath = path.join(output, 'js', 'ui', 'pipeline', 'clipboard-manager.js');
let clipboard = await readText(clipboardPath);
const doubleBlindPaste = /\n                    \/\/ A Double Blind Test share URL[\s\S]*?\n                    }\n\n                    \/\/ Ignore all pipeline pasting while the Double Blind Test is open\.[\s\S]*?\n                    }\n/;
if (!doubleBlindPaste.test(clipboard)) {
  throw new Error('Unable to locate Double Blind Test clipboard branches');
}
clipboard = clipboard.replace(doubleBlindPaste, '\n');
await writeFile(clipboardPath, clipboard, 'utf8');

const columnsPath = path.join(output, 'js', 'ui', 'pipeline', 'pipeline-column-manager.js');
let columns = await readText(columnsPath);
const saveColumns = `            getSettingsStorage().setItem('pipelineColumns', columns);`;
if (!columns.includes(saveColumns)) {
  throw new Error('Unable to locate column preference persistence');
}
columns = columns.replace(saveColumns, `${saveColumns}
            if (window.electronIntegration?.saveConfig) {
                void window.electronIntegration.saveConfig({ columns });
            }`);
await writeFile(columnsPath, columns, 'utf8');
await rm(path.join(output, 'js', 'library'), { recursive: true, force: true });
// Imported layouts may contain lyrics. Retain the pure renderer dependencies,
// while the music player and its file association workflow remain excluded.
await mkdir(path.join(output, 'js', 'library', 'metadata'), { recursive: true });
for (const helper of ['lyrics.js', 'text-encoding.js']) {
  await cp(path.join(source, 'js', 'library', 'metadata', helper),
           path.join(output, 'js', 'library', 'metadata', helper));
}
await rm(path.join(output, 'js', 'ui', 'library'), { recursive: true, force: true });
await rm(path.join(output, 'js', 'ui', 'audio-player'), { recursive: true, force: true });
await rm(path.join(output, 'js', 'ui', 'audio-player.js'), { force: true });
await rm(path.join(output, 'js', 'pipeline-analyzer'), { recursive: true, force: true });
await rm(path.join(output, 'js', 'ui', 'pipeline', 'file-processor.js'), { force: true });
await rm(path.join(output, 'js', 'audio', 'offline-processor.js'), { force: true });
await rm(path.join(output, 'js', 'audio', 'dsd'), { recursive: true, force: true });
// Keep the live MIDI state and sampler helpers, without the standalone score player.
for (const file of [
  'android/player-context.js', 'audio/midi-offline.js', 'midi/musicxml-parser.js',
  'midi/score-file.js', 'midi/score-worker.js', 'midi/smf-parser.js', 'midi/smf-worker.js'
]) {
  await rm(path.join(output, 'js', file), { force: true });
}
const streamedBankPath = path.join(output, 'js', 'sfz', 'streamed-bank.js');
let streamedBank = await readText(streamedBankPath);
const residentRanges = `    const ranges = storage ? sfzResidentRanges(parsed.regions, samples, storage.mandatory)
      : new Map(paths.map(path => [path, []]));
    if (!storage) for (const path of new Set([...oscillators.samples.keys(), ...effectSamplePaths(parsed.effects)]))
      ranges.get(path)?.push([0, samples.get(path).frames]);`;
const buildStreamedBank = `  const build = async (context, budget = maxBytes, estimate = estimateEffectBytes, previous = null) => {`;
const requiredPages = `    const required = [];
    for (const [sample, path] of paths.entries()) for (const [first, end] of ranges.get(path))
      for (let pageIndex = Math.floor(first / 4096); pageIndex <= Math.floor((end - 1) / 4096); pageIndex++)
        required.push(Object.freeze({ sample, pageIndex }));
    const minimum = storage ? maximumPlan.minimumSlots : required.length + 1;`;
const providerPreload = '        await provider.preload(required);';
for (const pattern of [residentRanges, buildStreamedBank, requiredPages, providerPreload]) {
  if (streamedBank.split(pattern).length !== 2) throw new Error('Unable to configure a fully resident native instrument');
}
streamedBank = streamedBank.replace(residentRanges, `    const ranges = storage ? sfzResidentRanges(parsed.regions, samples, new Set(paths))
      : new Map(paths.map(path => [path, [[0, samples.get(path).frames]]]));`)
  .replace(buildStreamedBank, `  // The native host retains decoded PCM for topology and audio-configuration changes.
  const replayBytes = factory.samples.reduce((total, sample) => total + sample.frames * sample.channels * 4, 0);
${buildStreamedBank}
    budget -= replayBytes;
    if (budget < 1) throw sfzError('too-large', 'The instrument exceeds the SoundFont memory limit.');`)
  .replace(requiredPages, `    const required = factory.samples.reduce((count, sample) => count + Math.ceil(sample.frames / 4096), 0);
    const minimum = storage ? maximumPlan.minimumSlots : required + 1;`)
  .replace(providerPreload, '        // Mixwright uploads all resident pages in bounded batches before native commit.');
await writeFile(streamedBankPath, streamedBank, 'utf8');
await writeFile(path.join(output, '.stamp'), `${productName} WebView assets\n`, 'utf8');
