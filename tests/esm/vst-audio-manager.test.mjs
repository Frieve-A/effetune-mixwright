import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import path from 'node:path';
import test from 'node:test';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

import * as irContract from '../../external/effetune/js/ir-library/ir-plugin-contract.js';

const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const source = await readFile(path.join(projectRoot, 'ui-shim', 'vst-audio-manager.js'), 'utf8');
const pluginBaseSource = await readFile(
  path.join(projectRoot, 'external', 'effetune', 'plugins', 'plugin-base.js'), 'utf8');
const irReverbSource = await readFile(
  path.join(projectRoot, 'external', 'effetune', 'plugins', 'reverb', 'ir_reverb.js'), 'utf8');
const classStart = source.indexOf('const noop =');
const classEnd = source.indexOf('\nfunction fakeNode', classStart);
const rebuildStart = source.indexOf('  async rebuildPipeline()');
const rebuildEnd = source.indexOf('\n  serializePipeline(', rebuildStart);

assert.notEqual(classStart, -1, 'NativePort class is missing');
assert.notEqual(classEnd, -1, 'NativePort class boundary is missing');
assert.notEqual(rebuildStart, -1, 'AudioManager.rebuildPipeline is missing');
assert.notEqual(rebuildEnd, -1, 'AudioManager.rebuildPipeline boundary is missing');

function createNativePort() {
  const hostCalls = [];
  const context = {
    ArrayBuffer,
    btoa,
    clearTimeout,
    console,
    DataView,
    document: {},
    Float32Array,
    performance,
    queueMicrotask,
    setTimeout,
    Uint32Array,
    Uint8Array,
    MutationObserver: class {
      observe() {}
      disconnect() {}
    },
    window: {
      app: { initialized: false },
      __effetuneHostCall: async (type, payload) => {
        hostCalls.push({ type, payload });
        return type === 'pipeline/assetCommit' ? { ok: true, state: 3 } : { ok: true };
      }
    }
  };
  vm.runInNewContext(
    `${source.slice(classStart, classEnd)}\nthis.NativePort = NativePort;`,
    context
  );
  const owner = {
    preserveReadyNativePipelineDuringStartup: true,
    scheduleLatencyService() {}
  };
  const port = new context.NativePort(owner);
  const node = { port };
  context.window.workletNode = node;
  return { context, hostCalls, node, port };
}

function preparedIrResult({ frames = 4, channels = 1, sampleRate = 48000,
  topology = channels === 1 ? 1 : 2, paths = [] } = {}) {
  const payload = new ArrayBuffer(32 + paths.length * 12 + channels * frames * 4);
  const header = new DataView(payload);
  header.setUint32(0, 0x31415445, true);
  header.setUint32(4, channels, true);
  header.setUint32(8, frames, true);
  header.setUint32(12, sampleRate, true);
  header.setUint32(16, topology, true);
  const sampleFrames = new Uint32Array([0, frames - 1]);
  const edcDb = new Float32Array([0, -60]);
  const envelope = new Float32Array([1, 0]);
  return {
    channels: Array.from({ length: channels }, () => new Float32Array(frames)),
    sampleRate,
    frames,
    topology,
    payload,
    asset: {
      formatTag: 1,
      channels,
      frames,
      sampleRate,
      topology,
      pathCount: paths.length,
      inputCount: new Set(paths.map(path => path.inputSlot)).size
    },
    analysis: {
      frames,
      sampleFrames,
      envelope,
      edcDb,
      rt60Seconds: 0.5,
      peakDb: 0,
      l1GainUpperBound: 2,
      l1GainUpperBoundDb: 20 * Math.log10(2),
      original: { frames, sampleFrames, envelope, edcDb, rt60Seconds: 0.5, peakDb: 0 },
      onsetFrame: 0,
      leadingSilenceFrames: 0,
      cutFrame: frames,
      sourceStartFrame: 0,
      truncated: false,
      initialNormalizationGains: new Float32Array([1]),
      finalNormalizationGains: new Float32Array([1])
    }
  };
}

test('preserved startup asset clears acknowledge without reaching the native host', async () => {
  const { hostCalls, port } = createNativePort();
  const events = [];
  port.addEventListener('message', event => events.push(event.data));

  port.postMessage({
    type: 'clearPluginAsset',
    pluginId: 17,
    slot: 0,
    operationRevision: 41,
    replayEpoch: 3
  });
  await new Promise(resolve => queueMicrotask(resolve));

  assert.deepEqual(JSON.parse(JSON.stringify(events)), [{
    type: 'assetState',
    pluginId: 17,
    slot: 0,
    state: 0,
    operationRevision: 41,
    replayEpoch: 3
  }]);
  assert.deepEqual(hostCalls, []);
});

test('a restored asset set advances after the preserved startup clear', async () => {
  const { context, hostCalls, node } = createNativePort();
  vm.runInContext(
    `${pluginBaseSource}\nthis.PluginBase = PluginBase;\n` +
      'this.plugin = new (class extends PluginBase {}); this.plugin.id = 17;',
    vm.createContext(context)
  );

  vm.runInContext(`
    this.plugin.clearWasmAsset(0);
    const payload = new ArrayBuffer(32);
    new DataView(payload).setUint32(0, 0x31415445, true);
    this.plugin.setWasmAsset(0, { payload, footprintBytes: 32 });
  `, context);
  await new Promise(resolve => setTimeout(resolve, 0));

  assert.deepEqual(hostCalls.map(call => call.type), [
    'pipeline/assetBegin',
    'pipeline/assetChunk',
    'pipeline/assetCommit'
  ]);
  assert.equal(context.plugin._wasmAssetDeliveries.has(node), false);
});

test('a preserved startup rebuild registers native asset membership', async () => {
  const window = { app: { initialized: false }, pipeline: null };
  const context = { window };
  vm.runInNewContext(
    `this.rebuildPipeline = ({${source.slice(rebuildStart, rebuildEnd)}}).rebuildPipeline;`,
    context
  );
  const calls = [];
  const pipeline = [{ id: 17 }];
  const manager = {
    preserveReadyNativePipelineDuringStartup: true,
    getCurrentPipeline: () => pipeline,
    synchronizeNativeAssetMembership: () => calls.push('membership'),
    dispatchEvent: type => calls.push(type)
  };

  assert.equal(await context.rebuildPipeline.call(manager), '');
  assert.equal(manager.pipeline, pipeline);
  assert.equal(window.pipeline, pipeline);
  assert.deepEqual(calls, ['membership', 'audioGraphRebuilt']);
});

test('startup membership lets a recreated IR Reverb reach ready', async () => {
  const irId = 'aaaaaaaaaaaaaaaaaaaaaaaa';
  const entry = { irId, fileLabel: 'Room.wav', composition: 'single', channels: 1 };
  const { context, hostCalls, node } = createNativePort();
  context.window.irLibraryService = {
    get(id) { return id === irId ? entry : null; },
    async resolveDecodedPcm() {
      return { channels: [new Float32Array([1, 0, 0, 0])], sampleRate: 48000 };
    },
    store: { async updateAnalysis() {} }
  };
  const workerClient = {
    async prepare(request) {
      return preparedIrResult({
        frames: request.channels[0].length,
        channels: request.channels.length,
        sampleRate: request.sampleRate,
        topology: request.options.topology,
        paths: request.options.paths
      });
    },
    async emit(request) {
      return preparedIrResult({
        frames: Math.min(request.channels[0].length, request.options.maxFrames),
        channels: request.options.assetChannels,
        sampleRate: request.sampleRate,
        topology: request.options.topology,
        paths: request.options.paths
      });
    },
    close() {}
  };
  context.window.irReverbRuntime = {
    ...irContract,
    createIrPreparationWorkerClient: () => workerClient
  };

  vm.runInContext(
    `${pluginBaseSource}\n${irReverbSource}\n` +
      'this.plugin = new IRReverbPlugin(); this.plugin.id = 17;',
    vm.createContext(context)
  );
  context.plugin.setSerializedParameters({ ir: irId, cr: 'full' });
  const startupMembership = new Map();
  context.plugin.setWasmAssetTargetResolver(plugin =>
    startupMembership.get(plugin.id) === plugin ? [node] : []);
  vm.runInContext(
    `this.rebuildPipeline = ({${source.slice(rebuildStart, rebuildEnd)}}).rebuildPipeline;`,
    context
  );
  const manager = {
    preserveReadyNativePipelineDuringStartup: true,
    getCurrentPipeline: () => [context.plugin],
    synchronizeNativeAssetMembership() {
      startupMembership.set(context.plugin.id, context.plugin);
    },
    dispatchEvent() {}
  };
  await context.rebuildPipeline.call(manager);
  assert.equal(await context.plugin._assetResolutionPromise, true);
  await new Promise(resolve => setTimeout(resolve, 0));

  assert.equal(context.plugin._statusMessage, 'Room.wav is in use.');
  assert.deepEqual(hostCalls.map(call => call.type), [
    'pipeline/assetBegin',
    'pipeline/assetChunk',
    'pipeline/assetCommit'
  ]);
  context.plugin.cleanup();
});
