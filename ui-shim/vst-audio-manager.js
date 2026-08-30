import { AudioManager as BrowserAudioManager } from './js/audio-manager.js';
import {
  DSP_AUTOMATION_CATALOG,
  DSP_PARAM_PACKERS,
  denormalizeDSPAutomationValue,
  normalizeDSPAutomationValue,
  packDSPAutomationValue,
  unpackDSPAutomationValue
} from './js/audio/dsp-params.generated.js';
import { getPluginExecutionCapabilities } from './js/audio/plugin-execution-capabilities.js';

window.dspParamPackers = DSP_PARAM_PACKERS;

const noop = () => {};
const ASSET_CHUNK_BYTES = 192 * 1024;
const IR_ASSET_HEADER_BYTES = 32;
const IR_ASSET_MAGIC = 0x31415445;
// An upper bound on how long the value a gesture ended on may wait for a frame
// that may never be painted. Short enough to stay inaudible, long enough that an
// ordinary drag still costs one request per displayed frame.
const PLUGIN_UPDATE_FLUSH_DEADLINE_MS = 50;
const HOST_DIAGNOSTIC_VISIBLE_MS = 3000;
// Synthetic per-instance enable target. The native side exposes the same key so
// effect ON/OFF can be automated without duplicating the generated catalog.
const NODE_ENABLE_DESCRIPTOR = { key: '__enabled', element: 0 };

function encodeBase64(bytes) {
  let binary = '';
  for (let offset = 0; offset < bytes.length; offset += 0x8000) {
    binary += String.fromCharCode(...bytes.subarray(offset, offset + 0x8000));
  }
  return btoa(binary);
}

function normalizePlugin(plugin, owner) {
  const logical = owner?.getCurrentPipeline?.().find(candidate => candidate.id === plugin.id);
  const channel = plugin.channel !== undefined ? plugin.channel : (logical?.channel ?? null);
  const executionCapabilities = getPluginExecutionCapabilities(logical) ||
    getPluginExecutionCapabilities(plugin);
  const normalized = {
    id: plugin.id,
    type: plugin.type,
    name: plugin.name || logical?.name || logical?.constructor?.name || plugin.type,
    enabled: plugin.enabled !== false,
    parameters: plugin.parameters || {},
    inputBus: plugin.inputBus ?? 0,
    outputBus: plugin.outputBus ?? 0,
    channel
  };
  if (executionCapabilities) normalized.executionCapabilities = executionCapabilities;
  if (plugin.wasmParams instanceof Float32Array) {
    normalized.wasmParams = Array.from(plugin.wasmParams);
    normalized.wasmParamsHash = plugin.wasmParamsHash >>> 0;
  }
  if (plugin.wasmParamBytes instanceof Uint8Array) {
    normalized.wasmParamBytes = Array.from(plugin.wasmParamBytes);
  }
  return normalized;
}

function automationIdentity(pipeline, pluginId, type, descriptor) {
  return `${pipeline}:${pluginId}:${type}:${descriptor.key}:${descriptor.element}`;
}

function automationDeltaIdentity(delta) {
  const pipeline = delta?.pipeline === 'B' ? 'B' : 'A';
  return `${pipeline}:${delta?.pluginId}:${delta?.pluginType}:` +
    `${delta?.parameterKey}:${delta?.elementIndex >>> 0}`;
}

function readAutomationPlain(parameters, descriptor) {
  if (descriptor.containerKey) {
    const entry = parameters?.[descriptor.containerKey]?.[descriptor.element];
    return descriptor.memberKey ? entry?.[descriptor.memberKey] : entry;
  }
  return parameters?.[descriptor.field];
}

function writeAutomationPlain(parameters, descriptor, value) {
  if (descriptor.containerKey) {
    const container = Array.isArray(parameters[descriptor.containerKey])
      ? parameters[descriptor.containerKey].map(entry =>
        entry && typeof entry === 'object' ? { ...entry } : entry)
      : [];
    if (descriptor.memberKey) {
      container[descriptor.element] = {
        ...(container[descriptor.element] || {}),
        [descriptor.memberKey]: value
      };
    } else {
      container[descriptor.element] = value;
    }
    parameters[descriptor.containerKey] = container;
  } else {
    parameters[descriptor.field] = value;
  }
}

function storageAutomationToNormalized(descriptor, packedValue) {
  const publicValue = descriptor.kind === 'enum' || descriptor.kind === 'bool'
    ? packedValue
    : unpackDSPAutomationValue(descriptor, packedValue);
  return normalizeDSPAutomationValue(descriptor, publicValue);
}

function normalizedAutomationToStorage(descriptor, normalizedValue) {
  const publicValue = denormalizeDSPAutomationValue(descriptor, normalizedValue);
  return descriptor.kind === 'enum' || descriptor.kind === 'bool'
    ? publicValue
    : packDSPAutomationValue(descriptor, publicValue);
}

// The single enumeration of a plug-in's automation targets. Baseline seeding and
// change detection must observe exactly the same set: a target seeded by only one
// of them either loses its first gesture or emits one the user never made.
function automationTargets(plugin) {
  const parameters = plugin.parameters || {};
  const targets = (DSP_AUTOMATION_CATALOG[plugin.type] || []).map(descriptor => ({
    descriptor,
    normalized: storageAutomationToNormalized(
      descriptor, readAutomationPlain(parameters, descriptor))
  }));
  targets.push({
    descriptor: NODE_ENABLE_DESCRIPTOR,
    normalized: plugin.enabled === false ? 0 : 1
  });
  return targets;
}

function getAudioOutputChannelCount(owner) {
  const value = owner?.contextManager?.audioContext?.destination?.channelCount ??
    owner?.audioContext?.destination?.channelCount;
  return Number.isInteger(value) && value >= 1 && value <= 8 ? value : 2;
}

function exposeAudioOutputChannelCount(owner) {
  const outputChannelCount = getAudioOutputChannelCount(owner);
  owner.outputChannelCount = outputChannelCount;
  if (owner.nativeNode) owner.nativeNode.channelCount = outputChannelCount;
  if (owner.workletNode) owner.workletNode.channelCount = outputChannelCount;
  return outputChannelCount;
}

function getPluginParameterOptions(owner, commitSampleRate = false) {
  const options = {
    sampleRate: owner?.contextManager?.audioContext?.sampleRate ??
      owner?.audioContext?.sampleRate ?? 44100,
    outputChannelCount: exposeAudioOutputChannelCount(owner)
  };
  if (commitSampleRate) options.commitSampleRate = true;
  return options;
}

class NativePort {
  constructor(owner) {
    this.owner = owner;
    this.onmessage = null;
    this.listeners = new Set();
    this.assetOperations = new Map();
    this.assetResidents = new Map();
    this.adoptedAutomationValues = new Map();
    this.deferredHostAutomationDeltas = new Map();
    this.pendingPluginUpdates = new Map();
    this.cancelPluginUpdateFlush = null;
    this.pluginUpdateFlushDeadlineMs = PLUGIN_UPDATE_FLUSH_DEADLINE_MS;
    // An editor that goes away cannot release the pointer either, so the same
    // deadline that publishes the queued values also ends the touch they belong
    // to. The close publishes first, so the order is the same one a release
    // inside the editor produces.
    this.flushOnEditorTeardown = () => {
      this.closePointerGesture();
      this.flushPluginUpdates();
    };
    // An editor that stops being painted stops running animation frames, so the
    // value a gesture ended on would stay queued until it is shown again. Closing
    // the editor destroys the page outright, which is the deadline nothing else
    // can answer for: whatever is still queued has to leave before it.
    document.addEventListener?.('visibilitychange', this.flushOnEditorTeardown);
    window.addEventListener?.('pagehide', this.flushOnEditorTeardown);
    // The host records automation inside the window between the touch starting
    // and ending, and the upstream controls report values only -- 'input' on a
    // slider, 'change' on a select or a checkbox -- so the boundary has to come
    // from the pointer instead. These listeners are the only source of it, and
    // which phase each is registered in is part of that -- see below.
    this.pointerGestureOpen = false;
    // Every automation identity the open pointer gesture has moved, against the
    // target the close has to name. A click-activated effect toggle claims its
    // target at pointerdown, before its value changes after pointerup.
    this.pointerGestureTargets = new Map();
    this.pointerGestureCloseTimer = null;
    // Which pointers are down right now, rather than whether any is. A second
    // mouse button pressed and released mid-drag, or a second finger lifting,
    // reports a pointerup of its own while the user is still holding the
    // control -- and a touch closed there would degrade the rest of the drag
    // into one-shot edits.
    //
    // The record is a hint the platform can strip, never an authority. Chromium
    // opens a <select> popup on the press and takes capture, so the page sees
    // the pointerdown and no release of any kind: no pointerup, no
    // pointercancel, no lostpointercapture, and the focus never leaves the page,
    // so no blur either. Upstream renders filter types, crossover slopes and
    // oversampling factors as <select>, and a native context menu loses the
    // release the same way. An id left behind by one of those would make "no
    // pointer is down" unreachable for the rest of the session, so the set is
    // rebuilt by the press that starts the next interaction and emptied by the
    // close itself -- it is never trusted to drain on its own.
    this.livePointerIds = new Set();
    this.beginPointerGesture = event => {
      // A pointerup defers its close through the following click. If another
      // press somehow arrives first, finish the old interaction instead of
      // merging two physical gestures into one host touch.
      if (this.pointerGestureCloseTimer !== null) {
        clearTimeout(this.pointerGestureCloseTimer);
        this.pointerGestureCloseTimer = null;
        this.closePointerGesture();
      }
      // A primary press starts an interaction, so anything still in the set is
      // the residue of an earlier one whose release was never delivered. A
      // secondary pointer -- a second finger, isPrimary false -- joins the
      // interaction the primary one is holding, and must not discard it.
      if (event?.isPrimary !== false) this.livePointerIds.clear();
      this.livePointerIds.add(event?.pointerId ?? 'mouse');
      this.pointerGestureOpen = true;

      // The upstream effect power button applies its value in onclick, which
      // runs after pointerup. Open an already-bound node-enable parameter now,
      // while the hand is actually on the button, so the host sees the same
      // mouse-down/value/mouse-up touch shape it sees for a slider.
      const target = this.effectToggleAutomationTarget(event);
      if (target) {
        const identity = automationDeltaIdentity(target);
        if (!this.pointerGestureTargets.has(identity)) {
          this.pointerGestureTargets.set(identity, target);
          void window.__effetuneHostCall('automation/beginGesture', {
            targets: [target]
          }).catch(error =>
            console.error('[EffeTune Mixwright] automation gesture begin failed', error));
        }
      }
    };
    this.endPointerGesture = event => {
      // A mouse reports every one of its buttons under a single pointer id, so
      // releasing a second button raises a pointerup while the first still
      // holds the control. buttons names what remains down after this release,
      // so a pointer that keeps one is not up at all and stays in the set.
      if (event?.type === 'pointerup' && (event.buttons ?? 0) !== 0) return;
      this.livePointerIds.delete(event?.pointerId ?? 'mouse');
      if (this.livePointerIds.size !== 0) return;
      if (event?.type !== 'pointerup') {
        this.closePointerGesture();
        return;
      }
      // DOM activation is pointerdown -> pointerup -> click. Closing here used
      // to make a toggle's later onclick value a complete zero-duration edit.
      // The next task is the first point at which that click is guaranteed to
      // have run; sliders merely keep their existing release semantics with a
      // sub-frame delay.
      this.pointerGestureCloseTimer = setTimeout(() => {
        this.pointerGestureCloseTimer = null;
        if (this.livePointerIds.size === 0) this.closePointerGesture();
      }, 0);
    };
    // A window that loses focus can see no pointer event at all, so this closes
    // whatever is still held rather than one pointer of it.
    this.abandonPointerGesture = () => {
      this.livePointerIds.clear();
      this.closePointerGesture();
    };
    // Every way a pointer can stop being down. A release outside the WebView
    // never reports pointerup here, a drag the browser takes over ends in
    // pointercancel or lostpointercapture, and a window that loses focus can
    // see none of them -- a close that is lost leaves the host believing the
    // user's hand is still on the control.
    //
    // The pointer events are taken in the capture phase and on window, so a
    // control that stops propagation, or one that lives inside a subtree we
    // never see, cannot hide the boundary. 'blur' is the one that must not be:
    // it does not bubble but it does traverse the capture phase, and upstream
    // renders every parameter as focusable inputs, so pressing on one control
    // while another holds focus dispatches a blur on that other control -- and
    // a capturing window listener would end the touch the press just opened.
    // Non-capturing and non-bubbling means window-targeted only, which is the
    // only blur that says the user's hand has left the plug-in entirely.
    this.pointerGestureListeners = [
      ['pointerdown', this.beginPointerGesture, true],
      ['pointerup', this.endPointerGesture, true],
      ['pointercancel', this.endPointerGesture, true],
      ['lostpointercapture', this.endPointerGesture, true],
      ['blur', this.abandonPointerGesture, false]
    ];
    for (const [type, listener, capture] of this.pointerGestureListeners) {
      window.addEventListener?.(type, listener, capture);
    }
  }

  effectToggleAutomationTarget(event) {
    if (event?.button !== undefined && event.button !== 0) return null;
    const toggle = event?.target?.closest?.('.toggle-button');
    if (!toggle || toggle.classList?.contains?.('master-toggle')) return null;
    const item = toggle.closest?.('.pipeline-item');
    const pluginId = Number(item?.dataset?.pluginId);
    if (!Number.isInteger(pluginId)) return null;
    const pipeline = this.owner.currentPipeline === 'B' ? 'B' : 'A';
    const plugins = pipeline === 'B' ? this.owner.pipelineB : this.owner.pipelineA;
    const plugin = plugins?.find(candidate => candidate.id === pluginId);
    const pluginType = plugin?.type || plugin?.constructor?.name;
    if (!plugin || typeof pluginType !== 'string' || pluginType.length === 0) return null;
    return {
      pipeline,
      pluginId,
      pluginType,
      parameterKey: NODE_ENABLE_DESCRIPTOR.key,
      elementIndex: NODE_ENABLE_DESCRIPTOR.element
    };
  }

  // Stamps one automation edit with where it sits inside the touch the user is
  // performing. An edit made with no pointer down -- a keyboard arrow, a typed
  // value, a programmatic change -- is a complete touch of its own, which is
  // both what the host should record and what every edit meant before gestures
  // were reported at all.
  markAutomationGesture(edit) {
    const identity = automationDeltaIdentity(edit.payload);
    // Every edit asks for the touch, not only the first of a drag. The native
    // open is idempotent -- a touch already open stays the one touch the host
    // was told about, so a drag is still one beginEdit -- and asking every time
    // is what makes a disagreement self-healing: a close the native side made
    // on its own, because the component suspended or a state was restored,
    // reopens on the very next value instead of leaving the rest of the drag
    // with no touch window for the host to record into.
    edit.payload.beginGesture = true;
    edit.payload.endGesture = !this.pointerGestureOpen;
    if (this.pointerGestureOpen && !this.pointerGestureTargets.has(identity)) {
      const { pipeline, pluginId, pluginType, parameterKey, elementIndex } = edit.payload;
      this.pointerGestureTargets.set(identity,
        { pipeline, pluginId, pluginType, parameterKey, elementIndex });
    }
    return edit;
  }

  // Ends the touch on every identity the gesture moved, in one request that
  // names them and no value: the values already travelled with the plug-in
  // images that carried them. Those images may still be queued for the next
  // frame, so they are published first -- a close that overtook them would end
  // the touch before the value the user released on reached the host.
  closePointerGesture() {
    if (!this.pointerGestureOpen) return;
    if (this.pointerGestureCloseTimer !== null) {
      clearTimeout(this.pointerGestureCloseTimer);
      this.pointerGestureCloseTimer = null;
    }
    this.pointerGestureOpen = false;
    // The touch is over however it ended, so no pointer this one recorded is
    // down any more. Leaving one behind would keep the next touch from ever
    // reaching an empty set and closing.
    this.livePointerIds.clear();
    const targets = [...this.pointerGestureTargets.values()];
    this.pointerGestureTargets.clear();
    if (targets.length === 0) return;
    this.flushPluginUpdates();
    void window.__effetuneHostCall('automation/endGesture', { targets })
      .catch(error =>
        console.error('[EffeTune Mixwright] automation gesture end failed', error));
  }

  postMessage(message, reason = '') {
    if (!message || typeof message !== 'object') return;
    // Everything except a coalesced plug-in image keeps its issue order across
    // the bridge, so a deferred update can never land after the rebuild, master
    // bypass, or asset operation that replaced it.
    if (message.type !== 'updatePlugin') this.flushPluginUpdates();
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
      return window.__effetuneHostCall('pipeline/masterBypass', {
        value: message.masterBypass
      }).catch(error => {
        console.error('[EffeTune Mixwright] master bypass update failed', error);
        return { ok: false, error: error?.message || String(error) };
      });
    }
    if (message.type === 'updatePlugins') {
      const pipeline = this.owner.currentPipeline;
      const normalizedPlugins = (message.plugins || [])
        .map(plugin => normalizePlugin(plugin, this.owner));
      const edits = normalizedPlugins.flatMap(plugin =>
        this.collectAutomationEdits(pipeline, plugin));
      // The collected gestures are what tells the native side which bound
      // targets this bulk image means to move. Without them a bound target is
      // overlaid with the value automation last played, so only the UI would
      // follow a preset load.
      return window.__effetuneHostCall('pipeline/rebuild', {
        pipeline,
        plugins: normalizedPlugins,
        automationEdits: edits.map(edit => edit.payload)
      }).then(result => {
        this.owner.applyHostAutomationDeltas(result.automationDeltas);
        this.owner.applyNativeExecutionStates?.(result.executionStates);
        if (result.skippedUnsupported && !this.owner.unsupportedWarningShown) {
          this.owner.unsupportedWarningShown = true;
          window.uiManager?.setError?.('Some effects are unavailable and were bypassed.', false);
        }
        this.owner.synchronizeNativeAssetMembership();
        this.owner.scheduleLatencyService();
        return result;
      }).catch(error => {
        this.reconcileAutomationEdits(edits);
        console.error('[EffeTune Mixwright] pipeline rebuild failed', error);
        if (reason === 'audio-manager-rebuild') throw error;
        return { ok: false, error: error?.message || String(error) };
      });
    } else if (message.type === 'updatePlugin' && message.plugin) {
      if (this.owner.hostAutomationApplyDepth > 0) return;
      const pipelineA = this.owner.pipelineA || [];
      const pipelineB = this.owner.pipelineB || [];
      const inPipelineA = pipelineA.some(candidate => candidate.id === message.plugin.id);
      const inPipelineB = pipelineB.some(candidate => candidate.id === message.plugin.id);
      if (!inPipelineA && !inPipelineB) return;
      const pipeline = inPipelineA ? 'A' : 'B';
      const logicalOwner = { getCurrentPipeline: () => inPipelineA ? pipelineA : pipelineB };
      const normalizedPlugin = normalizePlugin(message.plugin, logicalOwner);
      // Change detection has to observe every event: the gesture the host must
      // record is derived here even when the image carrying it is superseded.
      const edits = this.collectAutomationEdits(pipeline, normalizedPlugin);
      // The 250 ms debounce outlives the frame the coalesced image is published
      // in, so the native latency is still read back after it was applied.
      this.owner.scheduleLatencyService();
      return this.queuePluginUpdate(pipeline, normalizedPlugin, edits);
    }
  }

  // A pointer drag emits one input event per mouse report -- well over a hundred
  // a second -- and each one used to become its own synchronous bridge round
  // trip. Only the newest image of a plug-in is audible, so keep just that one
  // per plug-in and publish it once per displayed frame. The gestures collected
  // along the way are all retained: they are what the host records as
  // automation, and thinning them would coarsen the take. They stay in one
  // ordered list, because the native side applies the list from the front, so
  // the entry it ends on is the value the block-start pin keeps playing.
  queuePluginUpdate(pipeline, plugin, edits) {
    let pending = this.pendingPluginUpdates.get(plugin.id);
    if (!pending) {
      pending = { edits: [] };
      pending.dispatched = new Promise(resolve => { pending.dispatch = resolve; });
      this.pendingPluginUpdates.set(plugin.id, pending);
    }
    pending.pipeline = pipeline;
    // The image is kept only as the fallback for a plug-in that has left the
    // pipeline by the time the frame runs; what is published is re-read then.
    pending.plugin = plugin;
    pending.edits.push(...edits);
    this.schedulePluginUpdateFlush();
    return pending.dispatched;
  }

  schedulePluginUpdateFlush() {
    if (this.cancelPluginUpdateFlush) return;
    const flush = () => this.flushPluginUpdates();
    const frame = typeof requestAnimationFrame === 'function' && !document.hidden
      ? requestAnimationFrame(flush)
      : null;
    if (typeof setTimeout !== 'function') {
      if (frame === null) flush();
      else this.cancelPluginUpdateFlush = () => cancelAnimationFrame(frame);
      return;
    }
    // A frame is the moment to prefer, never one to wait on indefinitely: a
    // WebView that is offscreen or fully occluded keeps document.hidden false and
    // still runs no animation frames, and an editor can be destroyed before the
    // next one. Whichever arrives first publishes and cancels the other.
    const deadline = setTimeout(flush, frame === null ? 0 : this.pluginUpdateFlushDeadlineMs);
    this.cancelPluginUpdateFlush = () => {
      clearTimeout(deadline);
      if (frame !== null) cancelAnimationFrame(frame);
    };
  }

  flushPluginUpdates() {
    this.cancelPluginUpdateFlush?.();
    this.cancelPluginUpdateFlush = null;
    if (this.pendingPluginUpdates.size === 0) return;
    const flushed = [...this.pendingPluginUpdates.values()];
    this.pendingPluginUpdates.clear();
    for (const pending of flushed) pending.dispatch(this.sendPluginUpdate(pending));
  }

  // The queued image is the plug-in as it stood when the event fired, but a host
  // value that arrives before the frame is written straight into the plug-in and
  // adopted. Re-reading the plug-in here is what stops the frame from carrying
  // that value away again -- an image the UI, the baseline, and the host all
  // agree with, so nothing would ever detect the loss.
  serializeQueuedPlugin(pipeline, pluginId) {
    const plugins = pipeline === 'B' ? this.owner.pipelineB : this.owner.pipelineA;
    const live = plugins?.find(candidate => candidate.id === pluginId);
    if (!live) return null;
    const parameters = live.getParameters?.(getPluginParameterOptions(this.owner, true)) ||
      live.parameters || {};
    const payload = typeof live.getWorkletPluginData === 'function'
      ? live.getWorkletPluginData(parameters)
      : {
        id: live.id,
        type: live.type || live.constructor?.name,
        enabled: live.enabled,
        parameters
      };
    payload.name = live.name || payload.name || live.constructor?.name;
    return normalizePlugin(payload, { getCurrentPipeline: () => plugins });
  }

  // Every collected gesture travels with the image it belongs to, so the host
  // records them in the order the user made them. The answer says nothing about
  // them on purpose: the plug-in adopts the user's value as its own DSP value
  // whether or not the host takes the edit transaction, so a request that was
  // answered at all leaves both sides on the value already sent. Only a request
  // that never arrived leaves anything to put back.
  sendPluginUpdate({ pipeline, plugin, edits }) {
    return window.__effetuneHostCall('pipeline/updatePlugin', {
      pipeline,
      plugin: this.serializeQueuedPlugin(pipeline, plugin.id) || plugin,
      automationEdits: edits.map(edit => edit.payload)
    }).then(result => {
      this.owner.applyNativeExecutionStates?.(result.executionStates);
      if (result.rebuildAssets) this.owner.synchronizeNativeAssetMembership();
      if (result.skippedUnsupported && !this.owner.unsupportedWarningShown) {
        this.owner.unsupportedWarningShown = true;
        window.uiManager?.setError?.('Some effects are unavailable and were bypassed.', false);
      }
      return result;
    })
      .catch(error => {
        this.reconcileAutomationEdits(edits);
        console.error('[EffeTune Mixwright] parameter update failed', error);
        return { ok: false, error: error?.message || String(error) };
      });
  }

  rememberAdoptedPlugin(pipeline, plugin) {
    for (const { descriptor, normalized } of automationTargets(plugin)) {
      this.adoptedAutomationValues.set(
        automationIdentity(pipeline, plugin.id, plugin.type, descriptor), normalized);
    }
  }

  collectAutomationEdits(pipeline, plugin) {
    const edits = [];
    for (const { descriptor, normalized } of automationTargets(plugin)) {
      const identity = automationIdentity(pipeline, plugin.id, plugin.type, descriptor);
      const adopted = this.adoptedAutomationValues.get(identity);
      if (adopted === undefined) {
        this.adoptedAutomationValues.set(identity, normalized);
        continue;
      }
      if (Object.is(normalized, adopted)) continue;
      const target = {
        pipeline,
        pluginId: plugin.id,
        pluginType: plugin.type,
        parameterKey: descriptor.key,
        elementIndex: descriptor.element
      };
      // The value is adopted where the change is detected, not where an answer
      // for it comes back. The native side takes the user's value whichever way
      // the host answers, so the moment the edit is issued both sides hold it.
      // Adopting here is also what keeps a knob that returns to where it started
      // inside one frame emitting the edit that returns it, and what stops two
      // answers arriving out of order from moving the baseline backwards.
      this.adoptedAutomationValues.set(identity, normalized);
      edits.push(this.markAutomationGesture({
        payload: { ...target, normalized },
        reconcile: { ...target, normalized: adopted }
      }));
    }
    return edits;
  }

  // Used only when pipeline/rebuild or pipeline/updatePlugin itself failed:
  // the native side never received the value at all, so returning the editor and
  // the baseline to the one it still holds is all that is needed, and the echo
  // stays suppressed. An answered request never reaches here -- the plug-in
  // adopts the user's value whether or not the host records the edit, so there
  // is no second authority left for the editor to converge on.
  reconcileAutomationEdits(edits) {
    // One frame coalesces every value a drag emitted, so a failed frame can
    // carry several edits for the same identity -- and each one names only the
    // value the edit before it replaced. The first is therefore the only one
    // that still names what the native side holds; applying the rest in turn
    // would leave the editor on an intermediate value the native side never
    // received, which change detection then reads as agreement and never
    // corrects.
    const returned = new Set();
    for (const edit of edits || []) {
      const identity = automationDeltaIdentity(edit.reconcile);
      if (returned.has(identity)) continue;
      returned.add(identity);
      this.owner.applyHostAutomationDelta(edit.reconcile);
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
    let retainedState;
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
  // Closing the port ends the only path a queued image can still leave through,
  // so it publishes before it releases the listeners that would have done it.
  close() {
    // The port is the only route the close itself can leave through, and the
    // listeners that would have derived it are released just below, so a touch
    // still open here has to end now or never: the host would keep believing
    // the user's hand is on the control, and the block would keep ignoring the
    // automation the host plays back into it for the rest of the session.
    this.closePointerGesture();
    this.flushPluginUpdates();
    document.removeEventListener?.('visibilitychange', this.flushOnEditorTeardown);
    window.removeEventListener?.('pagehide', this.flushOnEditorTeardown);
    for (const [type, listener, capture] of this.pointerGestureListeners) {
      window.removeEventListener?.(type, listener, capture);
    }
  }
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
    this.nativeExecutionStateGeneration = 0;
    this.pendingNativeExecutionStates = null;
    this.preserveReadyNativePipelineDuringStartup = false;
    this.nativeContextSync = null;
    this.telemetryPoll = null;
    this.telemetryWasHidden = document.hidden;
    this.lastHiddenContextPoll = 0;
    this.latencyServiceTimer = null;
    this.pipelineCpuAveragePercent = 0;
    this.lastDispatchedNativeLatencySamples = null;
    this.lastDispatchedNativeLatencyCompensated = null;
    this.lastDispatchedPipelineCpuAveragePercent = null;
    this.hostAutomationApplyDepth = 0;
    this.hostDiagnosticTimer = null;
    this.shownHostDiagnostic = null;
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
      this.applyNativePerformanceStatus(result);
      this.applyHostAutomationDeltas(result.automationDeltas);
      this.applyHostDiagnostics(result.diagnostics);
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
      // The one call a page makes exactly once, before anything can be touched
      // in it. A state restore destroys the JS context by reloading it, and the
      // page that comes back starts with no gesture targets at all -- so this
      // announcement is the native side's only boundary for a touch the dying
      // page opened on its way out and can no longer release. Every other
      // host/getInfo is a poll and says nothing about which context sent it.
      const info = await window.__effetuneHostCall('host/getInfo', { startup: true });
      this.contextManager.audioContext = fakeAudioContext(info.engineSampleRate, info.channels);
      this.nativeContextGeneration = info.contextGeneration || 0;
      this.preserveReadyNativePipelineDuringStartup =
        info.dspReady === true && info.stateReplacementPending !== true;
      this.pendingNativeExecutionStates = this.preserveReadyNativePipelineDuringStartup
        ? info.executionStates
        : null;
      this.applyNativePerformanceStatus(info, false);
      this.applyNativeBypass(info.masterBypass === true);
      this.applyHostAutomationDeltas(info.automationDeltas);
      this.applyHostDiagnostics(info.diagnostics);
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
    exposeAudioOutputChannelCount(this);
    this.pipeline = this.getCurrentPipeline();
    window.audioManager = this;
    window.workletNode = this.nativeNode;
    window.pipeline = this.pipeline;
    this.pipelineProcessor.setPipeline(this.pipeline);
    this.pipelineProcessor.setMasterBypass(this.masterBypass);
  }

  applyNativePerformanceStatus(info, dispatch = true) {
    const currentLatency = Number.isInteger(info?.processingLatencySamples) &&
      info.processingLatencySamples >= 0
      ? info.processingLatencySamples
      : info?.latencySamples;
    const latencySamples = Number.isInteger(currentLatency) && currentLatency >= 0
      ? currentLatency
      : 0;
    const latencyCompensated = info?.latencyCompensated !== false;
    this.dspPipelineLatencySamples = latencySamples;
    this.dspPipelineLatencyCompensated = latencyCompensated;
    if (dispatch &&
        (this.lastDispatchedNativeLatencySamples !== latencySamples ||
         this.lastDispatchedNativeLatencyCompensated !== latencyCompensated)) {
      this.lastDispatchedNativeLatencySamples = latencySamples;
      this.lastDispatchedNativeLatencyCompensated = latencyCompensated;
      this.dispatchEvent('dspLatency', {
        type: 'dspLatency',
        samples: latencySamples,
        sampleRate: this.audioContext?.sampleRate,
        compensated: latencyCompensated
      });
    }

    const cpuAverage = Number.isFinite(info?.pipelineCpuAverage) &&
      info.pipelineCpuAverage >= 0 ? info.pipelineCpuAverage : 0;
    this.pipelineCpuAveragePercent = cpuAverage;
    if (dispatch && this.lastDispatchedPipelineCpuAveragePercent !== cpuAverage) {
      this.lastDispatchedPipelineCpuAveragePercent = cpuAverage;
      this.dispatchEvent('pipelineCpuUsage', { average: cpuAverage });
    }
  }

  applyNativeExecutionStates(states) {
    if (!Array.isArray(states)) return;
    this._resetDspExecutionStateSnapshot();
    const generation = ++this.nativeExecutionStateGeneration;
    for (const state of states) {
      if (!Number.isInteger(state?.pluginId) || typeof state?.pluginType !== 'string' ||
          !['pending', 'active', 'bypassed'].includes(state?.state)) {
        continue;
      }
      const data = {
        type: 'dspExecutionState',
        pluginId: state.pluginId,
        pluginType: state.pluginType,
        state: state.state,
        generation
      };
      if (state.state === 'bypassed') data.reason = state.reason;
      this.handleWorkletMessage({ data }, this.nativeNode);
    }
  }

  async rebuildPipeline() {
    this.pipeline = this.getCurrentPipeline();
    window.pipeline = this.pipeline;
    // The upstream execution-state validator matches notifications against
    // both the logical pipeline and PipelineProcessor's prepared image. The
    // native override owns rebuildPipeline(), so it must keep that second view
    // synchronized even when startup deliberately preserves the running DSP.
    this.pipelineProcessor?.setPipeline?.(this.pipeline);
    this.pipelineProcessor?.setMasterBypass?.(this.masterBypass);
    if (this.preserveReadyNativePipelineDuringStartup && window.app?.initialized !== true) {
      this.seedRestoredAutomationBaseline();
      this.applyNativeExecutionStates?.(this.pendingNativeExecutionStates);
      this.pendingNativeExecutionStates = null;
      // setCurrentPipeline installs membership-based asset target resolvers before
      // restored IR preparation settles. Populate that membership even though the
      // native topology itself must remain untouched during editor reconstruction.
      this.synchronizeNativeAssetMembership();
      this.dispatchEvent?.('audioGraphRebuilt', {});
      return '';
    }
    const parameterOptions = getPluginParameterOptions(this, true);
    const plugins = this.pipeline.map(plugin => {
      const parameters = plugin.getParameters(parameterOptions);
      const payload = typeof plugin.getWorkletPluginData === 'function'
        ? plugin.getWorkletPluginData(parameters)
        : { id: plugin.id, type: plugin.constructor.name, enabled: plugin.enabled, parameters };
      payload.name = plugin.name || payload.name || plugin.constructor.name;
      return payload;
    });
    try {
      await this.nativePort.postMessage({
        type: 'updatePlugins',
        plugins
      }, 'audio-manager-rebuild');
    } catch (error) {
      return `Audio Error: ${error?.message || String(error)}`;
    }
    this.dispatchEvent?.('audioGraphRebuilt', {});
    return '';
  }

  serializePipeline(pipeline) {
    const parameterOptions = getPluginParameterOptions(this, true);
    const logicalOwner = { getCurrentPipeline: () => pipeline };
    return pipeline.map(plugin => {
      const parameters = plugin.getParameters(parameterOptions);
      const payload = typeof plugin.getWorkletPluginData === 'function'
        ? plugin.getWorkletPluginData(parameters)
        : { id: plugin.id, type: plugin.constructor.name, enabled: plugin.enabled, parameters };
      payload.name = plugin.name || payload.name || plugin.constructor.name;
      return normalizePlugin(payload, logicalOwner);
    });
  }

  synchronizeHistoryState() {
    // Undo/redo replaces both pipelines, so any coalesced image still waiting
    // for its frame has to reach the native side before the restore does.
    this.nativePort.flushPluginUpdates();
    const pipelineA = this.serializePipeline(this.pipelineA);
    const pipelineB = this.pipelineB === null ? null : this.serializePipeline(this.pipelineB);
    // Undo and redo carry values the user explicitly asked for, so the targets
    // they move away from the adopted baseline travel with the restore. A bound
    // target the restore does not name keeps being overlaid with the value
    // automation is playing.
    // The whole edit is kept, not only the half that travels: collecting one
    // has already advanced the adopted baseline to the restored value, and the
    // reconcile half is the only record of the value the native side still
    // holds if the restore is refused.
    const edits = [
      ...pipelineA.flatMap(plugin => this.nativePort.collectAutomationEdits('A', plugin)),
      ...(pipelineB || []).flatMap(plugin =>
        this.nativePort.collectAutomationEdits('B', plugin))
    ];
    return window.__effetuneHostCall('pipeline/restoreHistory', {
      pipelineA,
      pipelineB,
      pipelineBInitialized: this.pipelineB !== null,
      currentPipeline: this.currentPipeline,
      automationEdits: edits.map(edit => edit.payload)
    }).then(result => {
      // Undo/redo rebuilds the plug-in instances while keeping their ids, so the
      // adopted baseline still describes the pre-restore values. Reseed from the
      // restored UI image first, then let the native snapshot override it -- the
      // same order the startup restore uses. Without this, an untouched target
      // reads as changed and claims an automation slot the user never asked for.
      this.seedRestoredAutomationBaseline();
      this.applyHostAutomationDeltas(result.automationDeltas);
      if (Array.isArray(result.executionStates)) {
        this.pipeline = this.getCurrentPipeline();
        window.pipeline = this.pipeline;
        this.pipelineProcessor?.setPipeline?.(this.pipeline);
        this.pipelineProcessor?.setMasterBypass?.(this.masterBypass);
        this.applyNativeExecutionStates?.(result.executionStates);
      }
      if (result.skippedUnsupported && !this.unsupportedWarningShown) {
        this.unsupportedWarningShown = true;
        window.uiManager?.setError?.('Some effects are unavailable and were bypassed.', false);
      }
      this.synchronizeNativeAssetMembership();
      this.scheduleLatencyService();
      return result;
    }).catch(error => {
      // A refused restore never reached the native side at all, so the baseline
      // this collection advanced still has to be returned to the values the
      // native side holds -- the same thing pipeline/rebuild and
      // pipeline/updatePlugin do when they fail. Without it a later edit back
      // to the restored value is compared against a baseline that already
      // carries it, read as agreement, and never emitted: the UI and the DSP
      // would stay apart with nothing left to notice it.
      this.nativePort.reconcileAutomationEdits(edits);
      console.error('[EffeTune Mixwright] history restore failed', error);
      return { ok: false, error: error.message };
    });
  }

  synchronizeNativeAssetMembership() {
    this.pipeline = this.getCurrentPipeline();
    this._syncWasmAssetMembership?.(this.nativeNode, this.pipeline, { trackState: true });
  }

  commitPowerTopologyMutation(message, { reason = '' } = {}) {
    return Promise.resolve(this.nativePort.postMessage(message, reason)).then(result => {
      if (result?.ok !== false) return true;
      if (!this.nativeTopologyRollbackPending) {
        this.nativeTopologyRollbackPending = true;
        window.uiManager?.setError?.(
          'The pipeline change could not be applied. The previous pipeline was restored.',
          false
        );
        const reload = () => window.location?.reload?.();
        if (typeof setTimeout === 'function') setTimeout(reload, 0);
        else reload();
      }
      return false;
    });
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
    void this.commitPowerTopologyMutation(
      { type: 'updatePlugins', masterBypass: value },
      { reason: 'pipeline-master-bypass' }
    );
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

  seedRestoredAutomationBaseline() {
    // Change detection reads the parameter image resolved against the engine
    // sample rate, so the baseline has to ask for the same one: a rate-dependent
    // field seeded from the plug-in's own stale rate reads as a user gesture on
    // the very first full update. commitSampleRate is deliberately omitted --
    // reseeding observes the pipeline, it never mutates it.
    const parameterOptions = getPluginParameterOptions(this);
    const rememberPipeline = (side, plugins) => {
      for (const plugin of plugins || []) {
        const type = plugin.type || plugin.constructor?.name;
        const parameters = plugin.getParameters?.(parameterOptions) || plugin.parameters || {};
        this.nativePort.rememberAdoptedPlugin(side, {
          id: plugin.id, type, enabled: plugin.enabled, parameters
        });
      }
    };
    rememberPipeline('A', this.pipelineA);
    rememberPipeline('B', this.pipelineB);
    for (const delta of [...this.nativePort.deferredHostAutomationDeltas.values()]) {
      this.applyHostAutomationDelta(delta);
    }
  }

  applyHostAutomationDeltas(deltas) {
    if (!Array.isArray(deltas)) return;
    // One take moves several lanes of the same effect inside a single telemetry
    // packet, and every parameter write republishes that plug-in's whole packed
    // image upstream. Applying a plug-in's values together costs one of those per
    // packet instead of one per lane.
    const grouped = new Map();
    for (const delta of deltas) {
      if (!delta || !Number.isFinite(delta.normalized)) continue;
      const pipeline = delta.pipeline === 'B' ? 'B' : 'A';
      const key = `${pipeline}:${delta.pluginId}`;
      let group = grouped.get(key);
      if (!group) {
        group = { pipeline, pluginId: delta.pluginId, parameters: [], enables: [] };
        grouped.set(key, group);
      }
      (delta.parameterKey === NODE_ENABLE_DESCRIPTOR.key ? group.enables : group.parameters)
        .push(delta);
    }
    for (const group of grouped.values()) {
      const plugin = this.findPipelinePlugin(group.pipeline, group.pluginId);
      if (group.parameters.length > 0) {
        this.applyHostParameterDeltas(group.pipeline, group.parameters, plugin);
      }
      for (const delta of group.enables) {
        this.applyHostNodeEnableDelta(group.pipeline, delta, plugin);
      }
    }
    // A queued image is a frame older than these values, and the flush re-reads
    // the plug-in it publishes rather than trusting the snapshot the event took.
    // Flushing after the values were written is what makes that re-read carry
    // them: publishing first sends the frame before them, and updatePlugin never
    // overlays, so the native state document would keep that stale value until
    // the lane publishes again -- which a host that only sends points on change
    // may not do for the rest of the take. Flushing inside this call is still
    // what keeps the image ahead of every message that follows it.
    this.nativePort.flushPluginUpdates();
  }

  findPipelinePlugin(pipeline, pluginId) {
    const plugins = pipeline === 'B' ? this.pipelineB : this.pipelineA;
    return plugins?.find(candidate => candidate.id === pluginId);
  }

  applyHostDiagnostics(diagnostics) {
    if (!Array.isArray(diagnostics)) return;
    for (const diagnostic of diagnostics) {
      if (typeof diagnostic?.message === 'string' && diagnostic.message) {
        this.showHostDiagnostic(diagnostic.message);
      }
    }
  }

  // A processing transaction that recovers has no way to retract its notice, so
  // the status line would otherwise keep reporting a condition that already
  // healed. Re-arm the timer while the condition keeps reporting itself.
  showHostDiagnostic(message) {
    window.uiManager?.setError?.(message, false);
    this.shownHostDiagnostic = message;
    if (this.hostDiagnosticTimer) clearTimeout(this.hostDiagnosticTimer);
    this.hostDiagnosticTimer = setTimeout(() => {
      this.hostDiagnosticTimer = null;
      const shown = this.shownHostDiagnostic;
      this.shownHostDiagnostic = null;
      const display = window.uiManager?.errorDisplay;
      // Another status message may have replaced ours in the meantime.
      if (display && display.textContent !== shown) return;
      window.uiManager?.clearError?.();
    }, HOST_DIAGNOSTIC_VISIBLE_MS);
  }

  // A host-authoritative value, written into the plug-in and adopted as the
  // baseline. Nothing is ever sent back for it: the value already came from the
  // native side, so republishing it would only echo it there again.
  applyHostAutomationDelta(delta) {
    if (!delta || !Number.isFinite(delta.normalized)) return;
    const pipeline = delta.pipeline === 'B' ? 'B' : 'A';
    const plugin = this.findPipelinePlugin(pipeline, delta.pluginId);
    if (delta.parameterKey === NODE_ENABLE_DESCRIPTOR.key) {
      return this.applyHostNodeEnableDelta(pipeline, delta, plugin);
    }
    return this.applyHostParameterDeltas(pipeline, [delta], plugin);
  }

  applyHostParameterDeltas(pipeline, deltas, plugin) {
    const resolved = [];
    for (const delta of deltas) {
      const descriptor = (DSP_AUTOMATION_CATALOG[delta.pluginType] || [])
        .find(candidate => candidate.key === delta.parameterKey &&
          candidate.element === delta.elementIndex);
      if (!descriptor) continue;
      const identity = automationIdentity(pipeline, delta.pluginId, delta.pluginType, descriptor);
      this.nativePort.adoptedAutomationValues.set(identity, delta.normalized);
      if (!plugin) {
        this.nativePort.deferredHostAutomationDeltas.set(identity, {
          ...delta, pipeline
        });
        continue;
      }
      this.nativePort.deferredHostAutomationDeltas.delete(identity);
      resolved.push({ delta, descriptor, identity });
    }
    if (resolved.length === 0) return false;
    const parameters = { ...plugin.getParameters() };
    for (const { delta, descriptor } of resolved) {
      writeAutomationPlain(parameters, descriptor,
        normalizedAutomationToStorage(descriptor, delta.normalized));
    }
    this.applyAdoptedPluginMutation(plugin, () => plugin.setParameters(parameters));
    // Change detection re-derives the normalized value from the plug-in's own
    // storage, so the baseline has to hold that canonical form -- not the raw
    // host number, and not the value the write asked for. A plug-in that
    // quantizes on write retains something else, and any later image carrying
    // that retained value would otherwise read as a gesture the user never made.
    const appliedParameters = plugin.getParameters();
    for (const { descriptor, identity } of resolved) {
      this.nativePort.adoptedAutomationValues.set(identity,
        storageAutomationToNormalized(
          descriptor, readAutomationPlain(appliedParameters, descriptor)));
    }
    this.scheduleUIControlSync(plugin);
    return true;
  }

  applyHostNodeEnableDelta(pipeline, delta, plugin) {
    const identity = automationIdentity(pipeline, delta.pluginId, delta.pluginType,
      NODE_ENABLE_DESCRIPTOR);
    this.nativePort.adoptedAutomationValues.set(identity, delta.normalized);
    if (!plugin) {
      this.nativePort.deferredHostAutomationDeltas.set(identity, { ...delta, pipeline });
      return false;
    }
    this.nativePort.deferredHostAutomationDeltas.delete(identity);
    const enabled = delta.normalized >= 0.5;
    if (plugin.enabled !== enabled) {
      // setEnabled owns the transition: it also resynchronizes the per-frame
      // redraw loop, which a bare field assignment would leave stopped forever.
      this.applyAdoptedPluginMutation(plugin, () => plugin.setEnabled(enabled));
      // The upstream toggle only reflects plugin.enabled when it is clicked, so
      // a host-driven change has to refresh that item itself.
      document.querySelector?.(
        `.pipeline-item[data-plugin-id="${plugin.id}"] .toggle-button`)
        ?.classList.toggle('off', !enabled);
      window.pipelineManager?.core?.updateAllPluginDisplayState?.();
    }
    // Same invariant as the parameter path: the collected value is derived back
    // from plugin.enabled, so the baseline adopts that canonical 0/1 form, read
    // after the transition rather than from the value it was asked for.
    this.nativePort.adoptedAutomationValues.set(identity, plugin.enabled === false ? 0 : 1);
    return true;
  }

  // A host-authoritative value must never be mistaken for a user gesture, so the
  // updatePlugin echo the mutation provokes is always suppressed -- including for
  // a reconcile, whose echo is the one that would otherwise be misread. The
  // upstream parameter wrapper saves an undo entry every 500 ms of changes, which
  // automation playback would otherwise keep triggering for the whole take.
  applyAdoptedPluginMutation(plugin, mutate) {
    const suppressedHistory = plugin._suppressParameterHistory === true;
    // That wrapper clears the pending trailing save unconditionally and only
    // skips re-arming it while suppressed, so a suppressed mutation cancels the
    // undo entry a user gesture is still waiting to write -- and every gesture
    // now draws a host echo back through here. Hiding the handle keeps the
    // wrapper's clearTimeout from ever seeing it; nothing re-arms a timer while
    // suppressed, so the still-live one is simply put back afterwards.
    const pendingSave = plugin.saveStateTimeout;
    plugin.saveStateTimeout = null;
    this.hostAutomationApplyDepth += 1;
    plugin._suppressParameterHistory = true;
    try {
      mutate();
    } finally {
      plugin._suppressParameterHistory = suppressedHistory;
      if (!plugin.saveStateTimeout) plugin.saveStateTimeout = pendingSave;
      this.hostAutomationApplyDepth -= 1;
    }
  }

  // Applying a host value updates the plug-in model, but its DOM sliders and
  // number inputs only redraw when the plug-in is asked to. Deltas arrive at the
  // telemetry rate, so collect the touched plug-ins and refresh them once per
  // frame. Writing a control's value never fires input, so no echo follows.
  scheduleUIControlSync(plugin) {
    (this.pendingUIControlSyncs ??= new Set()).add(plugin);
    if (this.uiControlSyncHandle) return;
    const flush = () => {
      this.uiControlSyncHandle = null;
      const pending = this.pendingUIControlSyncs;
      this.pendingUIControlSyncs = new Set();
      for (const target of pending) target.syncUIControls?.();
    };
    if (typeof requestAnimationFrame === 'function') {
      this.uiControlSyncHandle = requestAnimationFrame(flush);
    } else if (typeof setTimeout === 'function') {
      this.uiControlSyncHandle = setTimeout(flush, 0);
    } else {
      flush();
    }
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
      exposeAudioOutputChannelCount(this);
      // The rebuild resolves every rate-derived parameter against the new engine
      // rate while keeping the plug-in ids, so a baseline still describing the
      // old rate would read those untouched targets as gestures and force them
      // into automation slots the user never asked for. Reseeding first is what
      // undo/redo does in the other order for the opposite reason: an undo means
      // to move values, a rate change only re-expresses them.
      this.seedRestoredAutomationBaseline();
      await this.rebuildPipeline();
      window.uiManager?.updateSampleRateDisplay?.();
    })().finally(() => { this.nativeContextSync = null; });
    return this.nativeContextSync;
  }

  scheduleLatencyService() {
    clearTimeout(this.latencyServiceTimer);
    this.latencyServiceTimer = setTimeout(() => {
      this.latencyServiceTimer = null;
      void window.__effetuneHostCall('host/getInfo')
        .then(info => this.applyNativePerformanceStatus(info))
        .catch(() => {});
    }, 250);
  }
}
