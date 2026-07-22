import { AudioManager as BrowserAudioManager } from './js/audio-manager.js';
import { DSP_PARAM_PACKERS } from './js/audio/dsp-params.generated.js';

window.dspParamPackers = DSP_PARAM_PACKERS;

const noop = () => {};
const ASSET_CHUNK_BYTES = 192 * 1024;
const IR_ASSET_HEADER_BYTES = 32;
const IR_ASSET_MAGIC = 0x31415445;

function encodeBase64(bytes) {
  let binary = '';
  for (let offset = 0; offset < bytes.length; offset += 0x8000) {
    binary += String.fromCharCode(...bytes.subarray(offset, offset + 0x8000));
  }
  return btoa(binary);
}

function normalizePlugin(plugin, owner) {
  const logical = owner?.getCurrentPipeline?.().find(candidate => candidate.id === plugin.id);
  const normalized = {
    id: plugin.id,
    type: plugin.type,
    name: plugin.name || logical?.name || logical?.constructor?.name || plugin.type,
    enabled: plugin.enabled !== false,
    parameters: plugin.parameters || {},
    inputBus: plugin.inputBus ?? 0,
    outputBus: plugin.outputBus ?? 0,
    channel: plugin.channel ?? null
  };
  if (plugin.wasmParams instanceof Float32Array) {
    normalized.wasmParams = Array.from(plugin.wasmParams);
    normalized.wasmParamsHash = plugin.wasmParamsHash >>> 0;
  }
  if (plugin.wasmParamBytes instanceof Uint8Array) {
    normalized.wasmParamBytes = Array.from(plugin.wasmParamBytes);
  }
  return normalized;
}

class NativePort {
  constructor(owner) {
    this.owner = owner;
    this.onmessage = null;
    this.listeners = new Set();
    this.assetOperations = new Map();
    this.assetResidents = new Map();
  }

  postMessage(message, reason = '') {
    if (!message || typeof message !== 'object') return;
    if (message.type === 'setPluginAsset') {
      this.queueAssetOperation(message, () => this.setPluginAsset(message));
      return;
    }
    if (message.type === 'clearPluginAsset') {
      // Recreating an editor deserializes IR plug-ins before their assets are resolved.
      // The running native pipeline already owns the matching asset, so keep it until
      // the restored UI either replays that asset or the user explicitly clears it.
      if (this.owner.preserveReadyNativePipelineDuringStartup &&
          window.app?.initialized !== true) {
        this.acknowledgePreservedAssetClear(message);
        return;
      }
      this.queueAssetOperation(message, () => this.clearPluginAsset(message));
      return;
    }
    if (reason === 'pipeline-master-bypass' && typeof message.masterBypass === 'boolean') {
      void window.__effetuneHostCall('pipeline/masterBypass', {
        value: message.masterBypass
      }).catch(error => console.error('[EffeTune Mixwright] master bypass update failed', error));
      return;
    }
    if (message.type === 'updatePlugins') {
      return window.__effetuneHostCall('pipeline/rebuild', {
        pipeline: this.owner.currentPipeline,
        plugins: (message.plugins || []).map(plugin => normalizePlugin(plugin, this.owner))
      }).then(result => {
        if (result.skippedUnsupported && !this.owner.unsupportedWarningShown) {
          this.owner.unsupportedWarningShown = true;
          window.uiManager?.setError?.('Some effects are unavailable and were bypassed.', false);
        }
        this.owner.synchronizeNativeAssetMembership();
        this.owner.scheduleLatencyService();
        return result;
      }).catch(error => console.error('[EffeTune Mixwright] pipeline rebuild failed', error));
    } else if (message.type === 'updatePlugin' && message.plugin) {
      const pipelineA = this.owner.pipelineA || [];
      const pipelineB = this.owner.pipelineB || [];
      const inPipelineA = pipelineA.some(candidate => candidate.id === message.plugin.id);
      const inPipelineB = pipelineB.some(candidate => candidate.id === message.plugin.id);
      if (!inPipelineA && !inPipelineB) return;
      const pipeline = inPipelineA ? 'A' : 'B';
      const logicalOwner = { getCurrentPipeline: () => inPipelineA ? pipelineA : pipelineB };
      return window.__effetuneHostCall('pipeline/updatePlugin', {
        pipeline,
        plugin: normalizePlugin(message.plugin, logicalOwner)
      }).then(result => {
        if (result.rebuildAssets) this.owner.synchronizeNativeAssetMembership();
        this.owner.scheduleLatencyService();
        return result;
      })
        .catch(error => console.error('[EffeTune Mixwright] parameter update failed', error));
    }
  }

  assetKey(message) {
    return `${message.pluginId}:${message.slot >>> 0}`;
  }

  acknowledgePreservedAssetClear(message) {
    queueMicrotask(() => this.dispatch({
      type: 'assetState',
      pluginId: message.pluginId,
      slot: message.slot >>> 0,
      state: 0,
      operationRevision: message.operationRevision,
      ...(Number.isSafeInteger(message.replayEpoch) && { replayEpoch: message.replayEpoch })
    }));
  }

  queueAssetOperation(message, operation) {
    const key = this.assetKey(message);
    const previous = this.assetOperations.get(key) || Promise.resolve();
    const current = previous.catch(noop).then(operation).catch(error =>
      this.rejectPluginAsset(message, error));
    this.assetOperations.set(key, current);
    const cleanup = () => {
      if (this.assetOperations.get(key) === current) this.assetOperations.delete(key);
    };
    void current.then(cleanup, cleanup);
  }

  async setPluginAsset(message) {
    const payload = message.payload instanceof ArrayBuffer
      ? new Uint8Array(message.payload)
      : ArrayBuffer.isView(message.payload)
        ? new Uint8Array(message.payload.buffer, message.payload.byteOffset,
          message.payload.byteLength)
        : null;
    if (!payload || payload.byteLength < IR_ASSET_HEADER_BYTES) {
      throw new Error('Invalid native DSP asset payload');
    }
    const header = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
    if (header.getUint32(0, true) !== IR_ASSET_MAGIC) {
      throw new Error('Invalid native DSP asset header');
    }
    const metadata = {
      pluginId: message.pluginId,
      slot: message.slot >>> 0,
      formatTag: message.formatTag >>> 0,
      channels: header.getUint32(4, true),
      frames: header.getUint32(8, true),
      topology: header.getUint32(16, true),
      headBlock: message.headBlock >>> 0,
      rateDivider: message.rateDivider >>> 0,
      pathCount: message.pathCount >>> 0,
      inputCount: message.inputCount >>> 0,
      processingChannels: message.processingChannels >>> 0,
      footprintBytes: message.footprintBytes,
      byteSize: payload.byteLength,
      operationRevision: message.operationRevision
    };
    await window.__effetuneHostCall('pipeline/assetBegin', metadata);
    for (let offset = 0; offset < payload.byteLength; offset += ASSET_CHUNK_BYTES) {
      const chunk = payload.subarray(offset, Math.min(offset + ASSET_CHUNK_BYTES,
        payload.byteLength));
      await window.__effetuneHostCall('pipeline/assetChunk', {
        pluginId: message.pluginId,
        slot: message.slot >>> 0,
        operationRevision: message.operationRevision,
        offset,
        data: encodeBase64(chunk)
      });
    }
    const result = await window.__effetuneHostCall('pipeline/assetCommit', {
      pluginId: message.pluginId,
      slot: message.slot >>> 0,
      operationRevision: message.operationRevision
    });
    const resident = {
      pluginId: message.pluginId,
      slot: message.slot >>> 0,
      operationRevision: message.operationRevision,
      replayEpoch: Number.isSafeInteger(message.replayEpoch) ? message.replayEpoch : null,
      state: result.state >>> 0
    };
    this.assetResidents.set(this.assetKey(message), resident);
    this.dispatchAssetState(resident);
    this.pollAssetState(resident);
  }

  async clearPluginAsset(message) {
    await window.__effetuneHostCall('pipeline/assetClear', {
      pluginId: message.pluginId,
      slot: message.slot >>> 0
    });
    this.assetResidents.delete(this.assetKey(message));
    this.dispatch({
      type: 'assetState',
      pluginId: message.pluginId,
      slot: message.slot >>> 0,
      state: 0,
      operationRevision: message.operationRevision,
      ...(Number.isSafeInteger(message.replayEpoch) && { replayEpoch: message.replayEpoch })
    });
    this.owner.scheduleLatencyService();
  }

  dispatchAssetState(resident) {
    this.dispatch({
      type: 'assetState',
      pluginId: resident.pluginId,
      slot: resident.slot,
      state: resident.state,
      operationRevision: resident.operationRevision,
      ...(resident.replayEpoch !== null && { replayEpoch: resident.replayEpoch })
    });
    this.owner.scheduleLatencyService();
  }

  pollAssetState(resident) {
    const status = resident.state & 0xff;
    if (status !== 1 && status !== 2) return;
    setTimeout(async () => {
      if (this.assetResidents.get(`${resident.pluginId}:${resident.slot}`) !== resident) return;
      try {
        const result = await window.__effetuneHostCall('pipeline/assetState', {
          pluginId: resident.pluginId,
          slot: resident.slot
        });
        if (this.assetResidents.get(`${resident.pluginId}:${resident.slot}`) !== resident) return;
        const next = result.state >>> 0;
        if (next !== resident.state) {
          resident.state = next;
          this.dispatchAssetState(resident);
        }
        this.pollAssetState(resident);
      } catch (error) {
        console.error('[EffeTune Mixwright] asset state polling failed', error);
      }
    }, 16);
  }

  async rejectPluginAsset(message, error) {
    const key = this.assetKey(message);
    const retained = this.assetResidents.get(key) || null;
    let retainedState = retained?.state >>> 0;
    try {
      const result = await window.__effetuneHostCall('pipeline/assetState', {
        pluginId: message.pluginId,
        slot: message.slot >>> 0
      });
      retainedState = result.state >>> 0;
    } catch (_) {
      retainedState = 0;
    }
    const residentRetained = Boolean(retained && (retainedState & 0xff) >= 1 &&
      (retainedState & 0xff) <= 3);
    this.dispatch({
      type: 'assetLoadRejected',
      pluginId: message.pluginId,
      slot: message.slot >>> 0,
      reason: 'native-host',
      operationRevision: message.operationRevision,
      replayFailure: Number.isSafeInteger(message.replayEpoch),
      residentRetained,
      ...(Number.isSafeInteger(message.replayEpoch) && { replayEpoch: message.replayEpoch }),
      ...(residentRetained && {
        retainedOperationRevision: retained.operationRevision,
        retainedAssetState: retainedState,
        ...(retained.replayEpoch !== null && { retainedReplayEpoch: retained.replayEpoch })
      })
    });
    console.error('[EffeTune Mixwright] native DSP asset transfer failed', error);
  }

  addEventListener(type, listener) {
    if (type === 'message') this.listeners.add(listener);
  }
  removeEventListener(type, listener) {
    if (type === 'message') this.listeners.delete(listener);
  }
  start() {}
  close() {}
  dispatch(data) {
    const event = { data, currentTarget: this, target: this };
    this.onmessage?.(event);
    for (const listener of this.listeners) listener(event);
  }
}

function fakeNode(port) {
  return { port, connect() { return this; }, disconnect() {} };
}

function fakeAudioContext(sampleRate, channels) {
  return {
    sampleRate,
    state: 'running',
    currentTime: 0,
    destination: { channelCount: channels || 2, maxChannelCount: channels || 2 },
    resume: async () => {},
    suspend: async () => {},
    close: async () => {},
    createGain: () => ({ gain: { value: 1, setValueAtTime() {}, linearRampToValueAtTime() {} },
      connect() { return this; }, disconnect() {} }),
    createBufferSource: () => ({ connect() { return this; }, disconnect() {}, start() {}, stop() {} })
  };
}

export class AudioManager extends BrowserAudioManager {
  constructor(...args) {
    super(...args);
    this.nativePort = new NativePort(this);
    this.nativeNode = fakeNode(this.nativePort);
    this.nativePort.onmessage = event => this.handleWorkletMessage(event, this.nativeNode);
    window.workletNode = this.nativeNode;
    this.contextManager.workletNode = this.nativeNode;
    this.ioManager.sourceNode = fakeNode({ postMessage: noop });
    this.telemetryTimer = setInterval(() => this.pollNativeTelemetry(), 1000 / 60);
    this.nativeContextGeneration = 0;
    this.preserveReadyNativePipelineDuringStartup = false;
    this.nativeContextSync = null;
    this.telemetryPoll = null;
    this.telemetryWasHidden = document.hidden;
    this.lastHiddenContextPoll = 0;
    this.latencyServiceTimer = null;
  }

  pollNativeTelemetry() {
    if (this.telemetryPoll) return this.telemetryPoll;
    this.telemetryPoll = this.pollNativeTelemetryOnce()
      .finally(() => { this.telemetryPoll = null; });
    return this.telemetryPoll;
  }

  async pollNativeTelemetryOnce() {
    try {
      const hidden = document.hidden;
      if (hidden) {
        const now = Date.now();
        if (now - this.lastHiddenContextPoll < 250) return;
        this.lastHiddenContextPoll = now;
        this.telemetryWasHidden = true;
      }
      if (!hidden && this.telemetryWasHidden) {
        this.telemetryWasHidden = false;
        await window.__effetuneHostCall('telemetry/discard');
        return;
      }
      const result = await window.__effetuneHostCall(hidden ? 'host/getInfo' : 'telemetry/read');
      this.applyNativeBypass(result.masterBypass === true);
      // The startup pipeline is restored after AudioManager construction. Rebuilding for a
      // context change before App initialization would publish the temporary empty pipeline.
      if (window.app?.initialized === true && result.contextGeneration &&
          result.contextGeneration !== this.nativeContextGeneration) {
        void this.synchronizeNativeContext(result);
      }
      if (hidden) return;
      if (!result.packet || !result.bytes) return;
      const binary = atob(result.packet);
      const bytes = new Uint8Array(binary.length);
      for (let index = 0; index < binary.length; index++) bytes[index] = binary.charCodeAt(index);
      this.telemetryHub?.handleMessage?.({
        type: 'dspTelemetry',
        packet: bytes.buffer,
        bytes: result.bytes,
        droppedFrames: result.droppedFrames || 0
      });
    } catch (_) {
      // Transient bridge failures are retried on the next polling interval.
    }
  }

  async initAudio() {
    try {
      const info = await window.__effetuneHostCall('host/getInfo');
      this.contextManager.audioContext = fakeAudioContext(info.engineSampleRate, info.channels);
      this.nativeContextGeneration = info.contextGeneration || 0;
      this.preserveReadyNativePipelineDuringStartup = info.dspReady === true;
      this.applyNativeBypass(info.masterBypass === true);
      this.contextManager.workletNode = this.nativeNode;
      this.audioContext = this.contextManager.audioContext;
      this.workletNode = this.nativeNode;
      this.ioManager.sourceNode = this.ioManager.sourceNode || fakeNode({ postMessage: noop });
      this.updateExposedProperties();
      return '';
    } catch (error) {
      return `Audio Error: ${error.message}`;
    }
  }

  async initializeAudioWorklet() {
    this.contextManager.workletNode = this.nativeNode;
    this.workletNode = this.nativeNode;
    window.workletNode = this.nativeNode;
    return '';
  }

  updateExposedProperties() {
    this.audioContext = this.contextManager.audioContext;
    this.workletNode = this.nativeNode;
    this.contextManager.workletNode = this.nativeNode;
    this.pipeline = this.getCurrentPipeline();
    window.audioManager = this;
    window.workletNode = this.nativeNode;
    window.pipeline = this.pipeline;
    this.pipelineProcessor.setPipeline(this.pipeline);
    this.pipelineProcessor.setMasterBypass(this.masterBypass);
  }

  async rebuildPipeline() {
    this.pipeline = this.getCurrentPipeline();
    window.pipeline = this.pipeline;
    if (this.preserveReadyNativePipelineDuringStartup && window.app?.initialized !== true) {
      // setCurrentPipeline installs membership-based asset target resolvers before
      // restored IR preparation settles. Populate that membership even though the
      // native topology itself must remain untouched during editor reconstruction.
      this.synchronizeNativeAssetMembership();
      this.dispatchEvent?.('audioGraphRebuilt', {});
      return '';
    }
    const sampleRate = this.audioContext?.sampleRate ?? 44100;
    const plugins = this.pipeline.map(plugin => {
      const parameters = plugin.getParameters({ sampleRate, commitSampleRate: true });
      const payload = typeof plugin.getWorkletPluginData === 'function'
        ? plugin.getWorkletPluginData(parameters)
        : { id: plugin.id, type: plugin.constructor.name, enabled: plugin.enabled, parameters };
      payload.name = plugin.name || payload.name || plugin.constructor.name;
      return payload;
    });
    this.nativePort.postMessage({
      type: 'updatePlugins',
      plugins
    });
    this.dispatchEvent?.('audioGraphRebuilt', {});
    return '';
  }

  serializePipeline(pipeline) {
    const sampleRate = this.audioContext?.sampleRate ?? 44100;
    const logicalOwner = { getCurrentPipeline: () => pipeline };
    return pipeline.map(plugin => {
      const parameters = plugin.getParameters({ sampleRate, commitSampleRate: true });
      const payload = typeof plugin.getWorkletPluginData === 'function'
        ? plugin.getWorkletPluginData(parameters)
        : { id: plugin.id, type: plugin.constructor.name, enabled: plugin.enabled, parameters };
      payload.name = plugin.name || payload.name || plugin.constructor.name;
      return normalizePlugin(payload, logicalOwner);
    });
  }

  synchronizeHistoryState() {
    return window.__effetuneHostCall('pipeline/restoreHistory', {
      pipelineA: this.serializePipeline(this.pipelineA),
      pipelineB: this.pipelineB === null ? null : this.serializePipeline(this.pipelineB),
      pipelineBInitialized: this.pipelineB !== null,
      currentPipeline: this.currentPipeline
    }).then(result => {
      if (result.skippedUnsupported && !this.unsupportedWarningShown) {
        this.unsupportedWarningShown = true;
        window.uiManager?.setError?.('Some effects are unavailable and were bypassed.', false);
      }
      this.synchronizeNativeAssetMembership();
      this.scheduleLatencyService();
      return result;
    }).catch(error => {
      console.error('[EffeTune Mixwright] history restore failed', error);
      return { ok: false, error: error.message };
    });
  }

  synchronizeNativeAssetMembership() {
    this.pipeline = this.getCurrentPipeline();
    this._syncWasmAssetMembership?.(this.nativeNode, this.pipeline, { trackState: true });
  }

  commitPowerTopologyMutation(message, { reason = '' } = {}) {
    this.nativePort.postMessage(message, reason);
    return Promise.resolve(true);
  }

  registerPipelineProcessors() {}
  fadeInOutput() {}
  fadeOutOutput() { return Promise.resolve(); }
  startPowerPolicyController() { return Promise.resolve(false); }
  updateDspTelemetryRate() {}
  reset() { return this.rebuildPipeline(); }

  setMasterBypass(bypass) {
    const value = bypass === true;
    if (this.masterBypass === value) return Promise.resolve();
    this.applyNativeBypass(value);
    this.nativePort.postMessage({ type: 'updatePlugins', masterBypass: value },
      'pipeline-master-bypass');
    return Promise.resolve();
  }

  applyNativeBypass(value) {
    const changed = this.masterBypass !== value;
    this.masterBypass = value;
    this.pipelineProcessor?.setMasterBypass?.(value);
    const core = window.pipelineManager?.core;
    if (core) core.enabled = !value;
    const toggle = core?.masterToggle || document.querySelector('.toggle-button.master-toggle');
    toggle?.classList.toggle('off', value);
    if (changed) core?.updateAllPluginDisplayState?.();
  }

  async synchronizeNativeContext(info = null) {
    if (this.nativeContextSync) return this.nativeContextSync;
    this.nativeContextSync = (async () => {
      const latest = info?.engineSampleRate ? info : await window.__effetuneHostCall('host/getInfo');
      const generation = latest.contextGeneration || 0;
      if (generation === this.nativeContextGeneration) return;
      this.nativeContextGeneration = generation;
      if (this.audioContext) {
        this.audioContext.sampleRate = latest.engineSampleRate;
        this.audioContext.destination.channelCount = latest.channels;
        this.audioContext.destination.maxChannelCount = latest.channels;
      }
      await this.rebuildPipeline();
      window.uiManager?.updateSampleRateDisplay?.();
    })().finally(() => { this.nativeContextSync = null; });
    return this.nativeContextSync;
  }

  scheduleLatencyService() {
    clearTimeout(this.latencyServiceTimer);
    this.latencyServiceTimer = setTimeout(() => {
      this.latencyServiceTimer = null;
      void window.__effetuneHostCall('host/getInfo').catch(() => {});
    }, 250);
  }
}
