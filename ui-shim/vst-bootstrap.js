(() => {
  'use strict';

  window.__EFFETUNE_VST__ = true;
  window.pipelineStateLoaded = true;
  window.addEventListener('contextmenu', event => event.preventDefault(), true);
  const needsHomeKeyForEditing = target => {
    const tagName = String(target?.tagName || '').toLowerCase();
    return tagName === 'input' || tagName === 'textarea' || tagName === 'select' ||
      target?.isContentEditable === true;
  };
  // Home is a common DAW transport shortcut. Keep every modifier combination
  // outside editable controls from acquiring a second action inside the UI.
  window.addEventListener('keydown', event => {
    if (event.key !== 'Home' || needsHomeKeyForEditing(event.target)) {
      return;
    }
    event.preventDefault();
    event.stopImmediatePropagation();
  }, true);

  const style = document.createElement('style');
  style.textContent = `
    html { background-color: var(--et-base); }
    html.effetune-vst-host, html.effetune-vst-host body { min-width: 720px !important; }
    .subtitle-container,
    #openMusicButton, #openLibraryButton, #whatsThisLink,
    #audioConfigSettingsButton, #benchmarkSettingsButton,
    #measurementSettingsButton, #resetAudioSettingsButton,
    #installAppButton, #installAppElement, #doubleBlindTestButton { display: none !important; }
    .config-dialog { width: min(640px, calc(100vw - 32px)) !important; }
    .config-dialog .device-section { display: none !important; }
    .config-dialog .device-section:has(#language-select) { display: block !important; }
    .config-dialog .device-section:has(#theme-select),
    .config-dialog .device-section:has(#frequency-preview-sound),
    .config-dialog .device-section:has(#sfz-size-limit),
    .config-dialog .device-section:has(#startup-view-effects),
    .config-dialog .device-section:has(#spectrum-overlay-display) { display: block !important; }
    .config-dialog .radio-container:has(#startup-view-library),
    html:not(.effetune-vst-native-spectrum) .config-dialog .spectrum-overlay-row:has(#spectrum-overlay-quality) { display: none !important; }
    .config-dialog #physical-control-section { display: block !important; }
    .vst-os-controls {
      display: inline-flex;
      align-items: flex-end;
      gap: 6px;
      white-space: nowrap;
    }
    .vst-os-control {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      color: var(--et-text-secondary);
      font-size: 14px;
      line-height: normal;
    }
    .vst-os-control select {
      min-width: 66px;
      height: 30px;
      padding: 4px;
      border: 1px solid var(--et-border-strong);
      border-radius: 4px;
      background: var(--et-input-gradient);
      color: var(--et-text-primary);
      font: inherit;
      color-scheme: var(--et-color-scheme);
      box-shadow: inset 0 1px 3px rgba(0, 0, 0, 0.24),
                  inset 0 1px 0 rgba(255, 255, 255, 0.035);
      transition: background var(--et-transition-fast),
                  border-color var(--et-transition-fast),
                  box-shadow var(--et-transition-fast),
                  color var(--et-transition-fast);
    }
    .vst-os-control select:focus {
      outline: none;
      border-color: var(--et-accent);
      box-shadow: inset 0 1px 3px rgba(0, 0, 0, 0.24), var(--et-focus-ring);
    }
    .vst-os-control select option {
      background-color: var(--et-surface-13);
      color: var(--et-text-primary);
    }
    .vst-os-control select option:hover {
      background-color: var(--et-surface-20);
      color: var(--et-text-primary);
    }
    .vst-os-control select option:checked {
      background-color: var(--et-accent-pressed);
      color: var(--et-on-accent);
    }
    .vst-os-warning {
      align-self: center;
      color: #ffb347;
      font-size: 11px;
      display: none;
    }
    .vst-measurement-delete {
      color: #ff9b9b !important;
    }
    .vst-measurement-delete:disabled {
      color: var(--et-text-muted) !important;
    }
    .vst-measurement-delete-overlay {
      position: fixed;
      inset: 0;
      z-index: 1200;
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 16px;
      background: rgba(0, 0, 0, 0.62);
    }
    .vst-measurement-delete-dialog {
      box-sizing: border-box;
      width: min(460px, calc(100vw - 32px));
      padding: 22px;
      border-radius: 8px;
      background: var(--et-panel-gradient);
      color: var(--et-text-primary);
      box-shadow: 0 22px 54px rgba(0, 0, 0, 0.5),
                  inset 0 1px 0 rgba(255, 255, 255, 0.065),
                  inset 0 0 0 1px var(--et-border-strong);
    }
    .vst-measurement-delete-dialog h2 {
      margin: 0 0 12px;
      font-size: 20px;
    }
    .vst-measurement-delete-dialog p {
      margin: 0 0 20px;
      color: var(--et-text-secondary);
      overflow-wrap: anywhere;
    }
    .vst-measurement-delete-dialog .dialog-buttons {
      display: flex;
      justify-content: flex-end;
      gap: 8px;
    }
    .vst-measurement-delete-dialog .vst-measurement-delete-confirm {
      color: #ffb0b0 !important;
    }
    .vst-third-party-notices-overlay {
      z-index: 1100 !important;
    }
    .vst-third-party-notices-dialog {
      box-sizing: border-box;
      width: min(760px, calc(100vw - 32px));
      height: min(640px, calc(100vh - 32px));
      padding: 20px;
      display: flex;
      flex-direction: column;
      background: var(--et-panel-gradient);
      color: var(--et-text-primary);
      border-radius: 8px;
      box-shadow: 0 22px 54px rgba(0, 0, 0, 0.5),
                  inset 0 1px 0 rgba(255, 255, 255, 0.065),
                  inset 0 0 0 1px var(--et-border-strong);
    }
    .vst-third-party-notices-dialog h2 {
      margin: 0 0 16px;
      text-align: center;
    }
    .about-dialog .dialog-buttons,
    .vst-third-party-notices-dialog .dialog-buttons {
      gap: 8px;
    }
    .vst-third-party-notices-content {
      flex: 1;
      min-height: 0;
      margin: 0;
      padding: 14px;
      overflow: auto;
      border: 1px solid var(--et-border-strong);
      border-radius: 4px;
      background: rgba(0, 0, 0, 0.24);
      color: var(--et-text-secondary);
      font: 12px/1.5 ui-monospace, SFMono-Regular, Consolas, monospace;
      white-space: pre-wrap;
      overflow-wrap: anywhere;
      user-select: text;
    }
  `;
  document.head.appendChild(style);

  window.__effetuneHostCall = async (type, payload = {}) => {
    if (typeof window.vst_hostMessage !== 'function') {
      throw new Error('The EffeTune Mixwright native bridge is unavailable');
    }
    const result = await window.vst_hostMessage(JSON.stringify({ type, payload }));
    if (!result || result.ok !== true) {
      throw new Error(result?.error || `Native bridge request failed: ${type}`);
    }
    return result;
  };

  const sfzChunkBytes = 192 * 1024;
  const sfzMaximumBytes = 1024 * 1024 * 1024;
  // Keep the upstream SFZ response contract, including its size-limit code,
  // separate from host calls that throw native error messages.
  const sfzCall = async (operation, payload = {}) => {
    const response = await window.vst_hostMessage(JSON.stringify({ type: `sfz/${operation}`, payload }));
    if (response?.ok !== true) {
      throw { code: response?.code === 'too-large' ? 'too-large' : 'storage-failed' };
    }
    return response.data;
  };
  const sfzResponse = async action => {
    try {
      return { ok: true, data: await action() };
    } catch (error) {
      return { ok: false, code: error?.code === 'too-large' ? 'too-large' : 'storage-failed' };
    }
  };
  const readSfzFile = async (request, action) => {
    const maxBytes = request?.maxBytes;
    if (!Number.isSafeInteger(maxBytes) || maxBytes < 1 || maxBytes > sfzMaximumBytes) {
      throw { code: 'storage-failed' };
    }
    const opened = await sfzCall('openRead', request);
    if (opened === null) return null;
    const readId = opened?.readId;
    if (typeof readId !== 'string' || !/^[a-f0-9]{24}$/.test(readId)) {
      throw { code: 'storage-failed' };
    }
    try {
      const size = opened.size;
      if (!Number.isSafeInteger(size) || size < 0) throw { code: 'storage-failed' };
      if (size > maxBytes) throw { code: 'too-large' };
      const readRange = async (offset, length) => {
        if (!Number.isSafeInteger(offset) || !Number.isSafeInteger(length) || offset < 0 ||
            length < 1 || length > sfzChunkBytes || offset + length > size) {
          throw { code: 'storage-failed' };
        }
        const chunk = await sfzCall('readChunk', { readId, offset, length });
        if (typeof chunk?.base64 !== 'string' || chunk.base64.length > 4 * Math.ceil(length / 3)) {
          throw { code: 'storage-failed' };
        }
        const binary = atob(chunk.base64);
        if (binary.length !== length) throw { code: 'storage-failed' };
        return Uint8Array.from(binary, character => character.charCodeAt(0));
      };
      return await action(size, readRange);
    } finally {
      await sfzCall('closeRead', { readId });
    }
  };
  let sfzReads = Promise.resolve();
  const withSfzRead = (request, action) => {
    // A preparation can overlap with other banks and plug-in instances. Keep
    // each complete read/probe within the native bridge's bounded session limit.
    const pending = sfzReads.then(() => readSfzFile(request, action));
    sfzReads = pending.catch(() => {});
    return pending;
  };
  const sfzLibraryV1 = Object.freeze({
    apiVersion: 1,
    select: () => sfzResponse(() => sfzCall('select')),
    list: () => sfzResponse(() => sfzCall('list')),
    remove: request => sfzResponse(() => sfzCall('remove', request)),
    readRelative: request => sfzResponse(() => withSfzRead(request, async (size, readRange) => {
      const bytes = new Uint8Array(size);
      for (let offset = 0; offset < size; offset += sfzChunkBytes) {
        bytes.set(await readRange(offset, Math.min(sfzChunkBytes, size - offset)), offset);
      }
      return bytes;
    })),
    statRelative: request => sfzResponse(() => withSfzRead({ ...request, maxBytes: sfzMaximumBytes },
      async (size, readRange) => {
        const { readSfzAudioHeader } = await import('./js/sfz/bank.js');
        return { size, ...await readSfzAudioHeader(readRange, size) };
      }))
  });

  const noop = () => {};
  const asyncNoop = async () => ({ success: true });
  const eventNoop = () => noop;
  let backupExportActive = false;
  window.__effetuneExportBackup = async (blob, fileName, signal) => {
    if (backupExportActive) throw new Error('A backup export is already running.');
    if (!blob || blob.size <= 0 || blob.size > 256 * 1024 * 1024) {
      throw new Error('The backup file exceeds the supported size.');
    }
    const checkCancelled = () => {
      if (signal?.aborted) throw new DOMException('Backup export cancelled.', 'AbortError');
    };
    backupExportActive = true;
    try {
      checkCancelled();
      const result = await window.__effetuneHostCall('backup/exportBegin', {
        defaultName: fileName, totalBytes: blob.size
      });
      if (result.cancelled) throw new DOMException('Backup export cancelled.', 'AbortError');
      for (let offset = 0; offset < blob.size; offset += 192 * 1024) {
        checkCancelled();
        const bytes = new Uint8Array(await blob.slice(offset, offset + 192 * 1024).arrayBuffer());
        let binary = '';
        for (let start = 0; start < bytes.length; start += 8192) {
          binary += String.fromCharCode(...bytes.subarray(start, start + 8192));
        }
        checkCancelled();
        await window.__effetuneHostCall('backup/exportChunk', { offset, data: btoa(binary) });
      }
      checkCancelled();
      await window.__effetuneHostCall('backup/exportCommit');
    } catch (error) {
      await window.__effetuneHostCall('backup/exportCancel').catch(() => {});
      throw error;
    } finally {
      backupExportActive = false;
    }
  };
  window.addEventListener('pagehide', () => {
    if (backupExportActive) void window.__effetuneHostCall('backup/exportCancel').catch(() => {});
  });
  const productionUrl = new URL('https://effetune.frieve.com/');
  const githubUrl = new URL('https://github.com/Frieve-A/effetune-mixwright');
  const normalizeExternalUrl = value => {
    const source = String(value || '');
    try {
      let url = new URL(source, productionUrl);
      if (url.protocol === 'choc:' || url.protocol === 'effetune-mixwright:' ||
          url.hostname === 'choc.localhost' || url.hostname === 'choc.choc' ||
          url.hostname === 'effetune-mixwright.localhost') {
        url = new URL(`${url.pathname}${url.search}${url.hash}`, productionUrl);
      }
      if (url.origin === productionUrl.origin) {
        if (url.pathname.endsWith('.md')) {
          url.pathname = url.pathname.replace(/\.md$/, '.html');
        } else if (url.pathname !== '/' && !url.pathname.endsWith('/') &&
                   !/\.[^/]+$/.test(url.pathname)) {
          url.pathname += '.html';
        }
      }
      return url.toString();
    } catch {
      return source;
    }
  };
  const openExternalUrl = url => window.__effetuneHostCall('host/openExternal', {
    url: normalizeExternalUrl(url)
  });
  const showThirdPartyNotices = async () => {
    try {
      const response = await fetch('THIRD-PARTY-NOTICES.txt');
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const notices = await response.text();

      const overlay = document.createElement('div');
      overlay.className = 'modal-overlay vst-third-party-notices-overlay';
      overlay.setAttribute('role', 'dialog');
      overlay.setAttribute('aria-modal', 'true');
      overlay.setAttribute('aria-labelledby', 'vst-third-party-notices-title');
      overlay.innerHTML = `
        <div class="vst-third-party-notices-dialog">
          <h2 id="vst-third-party-notices-title">Third party notices</h2>
          <pre class="vst-third-party-notices-content"></pre>
          <div class="dialog-buttons">
            <button type="button" id="vst-third-party-notices-close">Close</button>
          </div>
        </div>`;
      overlay.querySelector('.vst-third-party-notices-content').textContent = notices;
      document.body.appendChild(overlay);

      const close = () => {
        document.removeEventListener('keydown', handleKeyDown);
        overlay.remove();
      };
      const handleKeyDown = event => {
        if (event.key === 'Escape') close();
      };
      overlay.querySelector('#vst-third-party-notices-close').addEventListener('click', close);
      document.addEventListener('keydown', handleKeyDown);
      overlay.querySelector('#vst-third-party-notices-close').focus();
    } catch {
      window.uiManager?.setError?.(
        'The bundled third-party notices could not be loaded. Reopen or reinstall the plug-in.',
        true);
    }
  };
  const showAboutDialog = async () => {
    const info = await window.__effetuneHostCall('host/getInfo');
    if (typeof window.electronIntegration?.showAboutDialog !== 'function') {
      throw new Error('The About dialog is unavailable');
    }
    await window.electronIntegration.showAboutDialog({ version: info.version });

    const description = document.querySelector('.about-dialog .about-description');
    if (description) description.textContent = 'Multi-Effect Audio Plug-in for DAWs';
    const buttons = document.querySelector('.about-dialog .dialog-buttons');
    const closeButton = buttons?.querySelector('#close-button');
    if (!buttons || !closeButton || document.getElementById('vst-third-party-notices-button')) {
      return;
    }
    const noticesButton = document.createElement('button');
    noticesButton.type = 'button';
    noticesButton.id = 'vst-third-party-notices-button';
    noticesButton.textContent = 'Third party notices';
    noticesButton.addEventListener('click', () => void showThirdPartyNotices());
    buttons.insertBefore(noticesButton, closeButton);
  };
  window.electronAPI = {
    sfzLibraryV1,
    platform: navigator.platform.toLowerCase().includes('mac') ? 'darwin' : 'win32',
    getPath: async () => 'vst-user-data',
    joinPaths: async (...parts) => parts.filter(Boolean).join('/'),
    fileExists: async path => (await window.__effetuneHostCall('storage/fileExists', { path })).exists,
    readFile: async path => window.__effetuneHostCall('storage/readFile', { path }),
    saveFile: async (path, content) => window.__effetuneHostCall('storage/writeFile', { path, content }),
    savePipelineStateToFile: asyncNoop,
    sendPipelineStateForClose: asyncNoop,
    getAppVersion: async () => (await window.__effetuneHostCall('host/getInfo')).version,
    isFirstLaunch: async () => false,
    loadConfig: async () => {
      const [result, info] = await Promise.all([
        window.__effetuneHostCall('config/load'),
        window.__effetuneHostCall('host/getInfo')
      ]);
      window.__effetuneNativeSpectrum = info.nativeSpectrumTap === true;
      document.documentElement.classList.toggle('effetune-vst-native-spectrum', window.__effetuneNativeSpectrum);
      const config = result.config || {};
      return { success: true, config: { ...config,
        startupView: config.startupView === 'visualizer' ? 'visualizer' : 'effects',
        spectrumOverlayQuality: window.__effetuneNativeSpectrum && config.spectrumOverlayQuality === 'hq' ? 'hq' : 'normal' } };
    },
    saveConfig: async config => window.__effetuneHostCall('config/save', { config }),
    loadAudioPreferences: async () => ({ success: true, preferences: {} }),
    saveAudioPreferences: asyncNoop,
    getAudioDevices: async () => ({ success: true, devices: [] }),
    getCommandLinePresetFile: async () => null,
    getUpdateInfo: async () => null,
    getUserPresetsForTray: async () => ({}),
    requestMicrophoneAccess: async () => false,
    clearMicrophonePermission: asyncNoop,
    updateApplicationMenu: asyncNoop,
    updateTrayMenu: asyncNoop,
    signalReadyForMusicFiles: asyncNoop,
    signalReadyForUpdates: asyncNoop,
    forceCheckForUpdates: asyncNoop,
    rendererPing: asyncNoop,
    armRendererWatchdog: asyncNoop,
    relaunchApp: asyncNoop,
    openExternal: openExternalUrl,
    openExternalUrl,
    showOpenDialog: async () => window.__effetuneHostCall('dialog/openPreset'),
    showSaveDialog: async options => window.__effetuneHostCall('dialog/savePreset', {
      defaultName: options?.defaultPath?.split(/[\\/]/).pop() || 'preset.effetune_preset'
    }),
    onIPC: eventNoop,
    onExportPreset: eventNoop,
    onImportPreset: eventNoop,
    onOpenPresetFile: eventNoop,
    onOpenMusicFile: eventNoop,
    onOpenMusicFiles: eventNoop,
    onProcessAudioFiles: eventNoop,
    onRequestPipelineStateForClose: eventNoop,
    onConfigApp: eventNoop,
    onConfigAudio: eventNoop,
    onBackupRestore: eventNoop,
    onLoadUserPreset: eventNoop,
    onSavePreset: eventNoop,
    onSavePresetAs: eventNoop,
    onShowAboutDialog: eventNoop,
    library: {}
  };

  document.addEventListener('drop', event => {
    const files = Array.from(event.dataTransfer?.files || []);
    if (files.some(file => /\.(?:mp3|wav|ogg|flac|opus|m4a|aac|webm|mp4)$/i.test(file.name))) {
      event.preventDefault();
      event.stopImmediatePropagation();
    }
  }, true);

  const maximumMeasurementImportBytes = 128 * 1024 * 1024;
  let measurementStoragePromise = null;
  const translatedText = (key, fallback, params = {}) => {
    const translated = window.uiManager?.t?.(key, params);
    return translated && translated !== key ? translated : fallback;
  };
  const getMeasurementStorage = async () => {
    if (!measurementStoragePromise) {
      measurementStoragePromise = import('./features/measurement/dataStorage.js')
        .then(async module => {
          await module.default.initialize();
          return module.default;
        })
        .catch(error => {
          measurementStoragePromise = null;
          throw error;
        });
    }
    return measurementStoragePromise;
  };
  const roomEqPlugins = () => {
    const audio = window.audioManager;
    return [...new Set([...(audio?.pipelineA || []), ...(audio?.pipelineB || [])])]
      .filter(plugin => plugin?.constructor === window.RoomEqPlugin);
  };
  const roomEqPluginFor = select => {
    const pluginId = Number(select?.id?.slice('room-eq-measurement-'.length));
    return roomEqPlugins().find(plugin => plugin.id === pluginId) || null;
  };
  const updateImportedDeleteButtons = () => {
    for (const row of document.querySelectorAll('.room-eq-measurement-row')) {
      const button = row.querySelector('.vst-measurement-delete');
      const select = row.querySelector('select[id^="room-eq-measurement-"]');
      if (!button || !select) continue;
      const plugin = roomEqPluginFor(select);
      button.disabled = true;
      const measurementId = plugin?.measurementId;
      if (!measurementId) continue;
      void getMeasurementStorage().then(storage => {
        if (button.isConnected && roomEqPluginFor(select)?.measurementId === measurementId) {
          button.disabled = storage.getMeasurementById(measurementId)?.imported !== true;
        }
      }).catch(() => {});
    }
  };
  const refreshImportedMeasurement = async (measurementId, targetPlugin, storage) => {
    const plugins = roomEqPlugins();
    await Promise.all(plugins.map(plugin => plugin._refreshMeasurements(false)));
    if (targetPlugin && plugins.includes(targetPlugin)) {
      const measurement = storage.getMeasurementById(measurementId);
      targetPlugin.setParameters({
        ms: measurementId,
        mn: measurement?.name || 'Measurement',
        rp: 0
      });
      await targetPlugin._renderMeasurement();
    }
    updateImportedDeleteButtons();
  };
  const importMeasurementText = async (jsonText, targetPlugin = null) => {
    if (typeof jsonText !== 'string' ||
        new Blob([jsonText]).size > maximumMeasurementImportBytes) {
      throw new Error('Measurement files must be at most 128 MB.');
    }
    const storage = await getMeasurementStorage();
    const measurementId = await storage.importMeasurementFromJSON(jsonText);
    if (!measurementId) {
      throw new Error('The selected file is not a valid measurement export.');
    }
    await refreshImportedMeasurement(measurementId, targetPlugin, storage);
    return measurementId;
  };
  const importMeasurementFile = async (file, targetPlugin = null) => {
    if (!file || !/\.json$/i.test(file.name || '')) {
      throw new Error('Select a measurement JSON file exported by the desktop application.');
    }
    if (!Number.isFinite(file.size) || file.size > maximumMeasurementImportBytes) {
      throw new Error('Measurement files must be at most 128 MB.');
    }
    return importMeasurementText(await file.text(), targetPlugin);
  };
  const confirmImportedMeasurementDeletion = measurement => new Promise(resolve => {
    const overlay = document.createElement('div');
    overlay.className = 'modal-overlay vst-measurement-delete-overlay';
    overlay.setAttribute('role', 'dialog');
    overlay.setAttribute('aria-modal', 'true');
    overlay.setAttribute('aria-labelledby', 'vst-measurement-delete-title');
    overlay.innerHTML = `
      <div class="vst-measurement-delete-dialog">
        <h2 id="vst-measurement-delete-title"></h2>
        <p></p>
        <div class="dialog-buttons">
          <button type="button" class="vst-measurement-delete-cancel"></button>
          <button type="button" class="vst-measurement-delete-confirm"></button>
        </div>
      </div>`;
    const cancel = overlay.querySelector('.vst-measurement-delete-cancel');
    const confirm = overlay.querySelector('.vst-measurement-delete-confirm');
    overlay.querySelector('h2').textContent = translatedText(
      'roomEq.delete.title', 'Delete imported measurement');
    overlay.querySelector('p').textContent = translatedText(
      'roomEq.delete.confirm',
      `Delete imported measurement \u201c${measurement.name}\u201d? This cannot be undone.`,
      { name: measurement.name });
    cancel.textContent = translatedText('ui.cancelButton', 'Cancel');
    confirm.textContent = translatedText('menu.edit.delete', 'Delete');
    const finish = result => {
      document.removeEventListener('keydown', handleKeyDown);
      overlay.remove();
      resolve(result);
    };
    const handleKeyDown = event => {
      if (event.key === 'Escape') finish(false);
    };
    cancel.addEventListener('click', () => finish(false));
    confirm.addEventListener('click', () => finish(true));
    document.addEventListener('keydown', handleKeyDown);
    document.body.appendChild(overlay);
    cancel.focus();
  });
  const clearDeletedMeasurementReferences = async measurementId => {
    const plugins = roomEqPlugins();
    const referencingPlugins = [];
    for (const plugin of plugins) {
      const parameters = {};
      if (plugin.measurementId === measurementId) {
        parameters.ms = '';
        parameters.mn = '';
        parameters.rp = 0;
      }
      for (let index = 0; index < 8; index += 1) {
        if (plugin.channelMeasurementIds?.[index] !== measurementId) continue;
        parameters[`ms${index}`] = '';
        parameters[`mn${index}`] = '';
      }
      if (Object.keys(parameters).length === 0) continue;
      plugin.setParameters(parameters);
      referencingPlugins.push(plugin);
    }
    await Promise.all(referencingPlugins.map(plugin => plugin._renderMeasurement()));
    return plugins;
  };
  const deleteImportedMeasurement = async targetPlugin => {
    const measurementId = targetPlugin?.measurementId;
    if (!measurementId) return false;
    const storage = await getMeasurementStorage();
    const measurement = storage.getMeasurementById(measurementId);
    if (measurement?.imported !== true) {
      throw new Error('Only measurements imported into this plug-in can be deleted here.');
    }
    if (!await confirmImportedMeasurementDeletion(measurement)) return false;
    const plugins = await clearDeletedMeasurementReferences(measurementId);
    if (!await storage.deleteMeasurement(measurementId)) {
      throw new Error(translatedText(
        'roomEq.delete.error', 'The imported measurement could not be deleted.'));
    }
    await Promise.all(plugins.map(plugin => plugin._refreshMeasurements(false)));
    updateImportedDeleteButtons();
    return true;
  };
  window.__effetuneMeasurementImport = {
    maximumBytes: maximumMeasurementImportBytes,
    deleteImported: deleteImportedMeasurement,
    importFile: importMeasurementFile,
    importText: importMeasurementText
  };

  const enhanceRoomEqMeasurementRows = root => {
    const rows = [];
    if (root?.matches?.('.room-eq-measurement-row')) rows.push(root);
    rows.push(...(root?.querySelectorAll?.('.room-eq-measurement-row') || []));
    for (const row of rows) {
      if (row.querySelector('.vst-measurement-import')) continue;
      const select = row.querySelector('select[id^="room-eq-measurement-"]');
      if (!select) continue;
      const input = document.createElement('input');
      input.type = 'file';
      input.accept = '.json,application/json';
      input.hidden = true;
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'room-eq-refresh vst-measurement-import';
      button.textContent = translatedText('roomEq.action.import', 'Import\u2026');
      button.title = 'Import a measurement JSON file exported by the desktop application';
      button.addEventListener('click', () => input.click());
      input.addEventListener('change', () => {
        const file = input.files?.[0];
        input.value = '';
        if (!file) return;
        const targetPlugin = roomEqPluginFor(select);
        button.disabled = true;
        void importMeasurementFile(file, targetPlugin)
          .catch(error => {
            console.error('Measurement import failed:', error);
            window.uiManager?.setError?.(
              error?.message || 'The measurement could not be imported.', true);
          })
          .finally(() => {
            if (button.isConnected) button.disabled = false;
          });
      });
      const deleteButton = document.createElement('button');
      deleteButton.type = 'button';
      deleteButton.className = 'room-eq-refresh vst-measurement-delete';
      deleteButton.textContent = translatedText('menu.edit.delete', 'Delete');
      deleteButton.title = translatedText(
        'roomEq.delete.title', 'Delete imported measurement');
      deleteButton.disabled = true;
      deleteButton.addEventListener('click', () => {
        deleteButton.disabled = true;
        void deleteImportedMeasurement(roomEqPluginFor(select))
          .catch(error => {
            console.error('Measurement deletion failed:', error);
            window.uiManager?.setError?.(
              error?.message || 'The imported measurement could not be deleted.', true);
          })
          .finally(updateImportedDeleteButtons);
      });
      select.addEventListener('change', updateImportedDeleteButtons);
      row.append(input, button, deleteButton);
      updateImportedDeleteButtons();
    }
  };

  document.addEventListener('DOMContentLoaded', () => {
    document.body.classList.remove('view-library');
    enhanceRoomEqMeasurementRows(document);
    new MutationObserver(records => {
      for (const record of records) {
        for (const node of record.addedNodes) {
          if (node.nodeType === Node.ELEMENT_NODE) enhanceRoomEqMeasurementRows(node);
        }
      }
    }).observe(document.body, { childList: true, subtree: true });
    const headerButtons = document.querySelector('.header-buttons');
    if (!headerButtons || document.querySelector('.vst-os-controls')) return;

    const settingsMenu = document.getElementById('settingsMenu');
    const settingsMenuButton = document.getElementById('settingsMenuButton');
    if (settingsMenu && !document.getElementById('helpSettingsButton')) {
      const helpButton = document.createElement('button');
      helpButton.type = 'button';
      helpButton.className = 'settings-menu-item';
      helpButton.id = 'helpSettingsButton';
      helpButton.textContent = 'Help';

      const githubButton = document.createElement('button');
      githubButton.type = 'button';
      githubButton.className = 'settings-menu-item';
      githubButton.id = 'githubSettingsButton';
      githubButton.textContent = 'GitHub';

      const aboutButton = document.createElement('button');
      aboutButton.type = 'button';
      aboutButton.className = 'settings-menu-item';
      aboutButton.id = 'aboutSettingsButton';
      aboutButton.textContent = 'About';

      const updateHelpLabels = () => {
        for (const [button, translationKey, fallback] of [
          [helpButton, 'menu.help.help', 'Help'],
          [aboutButton, 'menu.help.about', 'About']
        ]) {
          const translated = window.uiManager?.t?.(translationKey);
          button.textContent = translated && translated !== translationKey ? translated : fallback;
        }
      };
      settingsMenuButton?.addEventListener('click', updateHelpLabels);
      helpButton.addEventListener('click', () => {
        settingsMenu.classList.remove('show');
        void openExternalUrl(productionUrl).catch(() => {
          window.uiManager?.setError?.('Unable to open Help in the default browser.', true);
        });
      });
      githubButton.addEventListener('click', () => {
        settingsMenu.classList.remove('show');
        void openExternalUrl(githubUrl).catch(() => {
          window.uiManager?.setError?.('Unable to open GitHub in the default browser.', true);
        });
      });
      aboutButton.addEventListener('click', () => {
        settingsMenu.classList.remove('show');
        void showAboutDialog().catch(() => {
          window.uiManager?.setError?.('Unable to open the About dialog.', true);
        });
      });
      settingsMenu.appendChild(helpButton);
      settingsMenu.appendChild(githubButton);
      settingsMenu.appendChild(aboutButton);
      updateHelpLabels();
    }

    const controls = document.createElement('div');
    controls.className = 'vst-os-controls';
    controls.setAttribute('role', 'group');
    controls.setAttribute('aria-label', 'Upsampling settings');
    controls.innerHTML = `
      <label class="vst-os-control">
        <span>Upsampling Factor:</span>
        <select data-vst-os-factor aria-label="Upsampling factor">
          <option value="1">1×</option><option value="2">2×</option>
          <option value="4">4×</option><option value="8">8×</option>
        </select>
      </label>
      <label class="vst-os-control">
        <span>Phase:</span>
        <select data-vst-os-phase aria-label="Upsampling phase">
          <option value="linear">Linear</option><option value="minimum">Minimum</option>
        </select>
      </label>
      <label class="vst-os-control">
        <span>Quality:</span>
        <select data-vst-os-quality aria-label="Upsampling quality">
          <option value="low">Low</option><option value="medium" selected>Medium</option>
          <option value="high">High</option><option value="ultra">Ultra</option>
        </select>
      </label>
      <span class="vst-os-warning" title="Very high effective sample rates can overload heavy pipelines">⚠ CPU</span>`;
    headerButtons.insertBefore(controls, headerButtons.querySelector('.settings-menu-container'));

    const apply = async () => {
      const factor = Number(controls.querySelector('[data-vst-os-factor]').value);
      const phase = controls.querySelector('[data-vst-os-phase]').value;
      const quality = controls.querySelector('[data-vst-os-quality]').value;
      try {
        const info = await window.__effetuneHostCall('os/set', { factor, phase, quality });
        if (window.audioManager?.audioContext) {
          const host = await window.__effetuneHostCall('host/getInfo');
          await window.audioManager.synchronizeNativeContext(host);
          controls.querySelector('.vst-os-warning').style.display =
            host.engineSampleRate > 768000 ? 'inline' : 'none';
          window.uiManager?.updateSampleRateDisplay?.();
        }
        return info;
      } catch (error) {
        window.uiManager?.setError?.(error.message, true);
        return null;
      }
    };
    controls.addEventListener('change', apply);

    window.__effetuneHostCall('host/getInfo').then(info => {
      controls.querySelector('[data-vst-os-factor]').value = String(info.oversamplingFactor || 1);
      controls.querySelector('[data-vst-os-phase]').value = info.oversamplingPhase || 'linear';
      controls.querySelector('[data-vst-os-quality]').value = info.oversamplingQuality || 'medium';
      controls.querySelector('.vst-os-warning').style.display =
        info.engineSampleRate > 768000 ? 'inline' : 'none';
    }).catch(() => {});
  });
})();
