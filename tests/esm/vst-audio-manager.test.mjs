import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import path from 'node:path';
import test from 'node:test';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

import * as irContract from '../../external/effetune/js/ir-library/ir-plugin-contract.js';

const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const source = await readFile(path.join(projectRoot, 'ui-shim', 'vst-audio-manager.js'), 'utf8');
const generatedCatalogSource = await readFile(
  path.join(projectRoot, 'external', 'effetune', 'js', 'audio', 'dsp-params.generated.js'), 'utf8');
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

function catalogExports() {
  const transformed = generatedCatalogSource
    .replaceAll('export const ', 'const ')
    .replaceAll('export function ', 'function ');
  const context = {};
  vm.runInNewContext(`${transformed}\nthis.catalog = { DSP_AUTOMATION_CATALOG, ` +
    'normalizeDSPAutomationValue, denormalizeDSPAutomationValue, ' +
    'packDSPAutomationValue, unpackDSPAutomationValue };', context);
  return context.catalog;
}

// The object the shim registers its listeners on. A Map keyed by event type can
// answer neither question the pointer-gesture contract turns on: whether a
// listener asked for the capture phase, and what an event whose target is a
// descendant does to a listener on the window. Both are modelled here, and
// nothing else is. An event reaches a window listener when it is targeted at the
// window itself, when the listener is capturing -- every event traverses the
// capture phase on its way down to a descendant -- or when it bubbles back up.
function createEventTarget() {
  return {
    registrations: [],
    addEventListener(type, listener, options) {
      this.registrations.push({
        type,
        listener,
        capture: options === true || options?.capture === true
      });
    },
    removeEventListener(type, listener, options) {
      const capture = options === true || options?.capture === true;
      const index = this.registrations.findIndex(entry => entry.type === type &&
        entry.listener === listener && entry.capture === capture);
      if (index >= 0) this.registrations.splice(index, 1);
    },
    listenersFor(type) {
      return this.registrations.filter(entry => entry.type === type);
    },
    hasListener(type) { return this.listenersFor(type).length > 0; },
    dispatch(type, options = {}) {
      const { target = this, bubbles = false, ...rest } = options;
      const event = { type, target, currentTarget: this, bubbles, ...rest };
      for (const entry of [...this.registrations]) {
        if (entry.type !== type) continue;
        if (target !== this && !entry.capture && !bubbles) continue;
        entry.listener(event);
      }
      return event;
    }
  };
}

function createNativePort() {
  const hostCalls = [];
  const context = {
    ArrayBuffer,
    btoa,
    clearTimeout,
    console,
    DataView,
    document: createEventTarget(),
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
    window: Object.assign(createEventTarget(), {
      app: { initialized: false },
      __effetuneHostCall: async (type, payload) => {
        hostCalls.push({ type, payload });
        return type === 'pipeline/assetCommit' ? { ok: true, state: 3 } : { ok: true };
      }
    })
  };
  const catalog = catalogExports();
  Object.assign(context, catalog);
  vm.runInNewContext(`${source.slice(classStart, classEnd)}\n` +
    'this.NativePort = NativePort;', context);
  const owner = {
    preserveReadyNativePipelineDuringStartup: true,
    currentPipeline: 'A',
    pipelineA: [],
    pipelineB: [],
    getCurrentPipeline() {
      return this.currentPipeline === 'B' ? this.pipelineB : this.pipelineA;
    },
    applyHostAutomationDeltas() {},
    synchronizeNativeAssetMembership() {},
    scheduleLatencyService() {}
  };
  const port = new context.NativePort(owner);
  const node = { port };
  context.window.workletNode = node;
  return { context, hostCalls, node, port };
}

// A bridge that never answers the request a gesture travelled in. Nothing is
// waiting on that answer any more -- the value is the plug-in's own from the
// moment it is sent -- so silence must simply leave everything where it is.
function silenceAutomationEdits({ context, hostCalls }) {
  context.window.__effetuneHostCall = async (type, payload) => {
    hostCalls.push({ type, payload });
    return type === 'pipeline/updatePlugin' ? new Promise(() => {}) : { ok: true };
  };
}

// A gesture travels inside the plug-in update that carries the image it belongs
// to, so the gestures a frame published are read back out of that one payload.
function automationEditPayloads(hostCalls) {
  return hostCalls.flatMap(call => call.type === 'pipeline/updatePlugin'
    ? (call.payload.automationEdits || []) : []);
}

async function createRestoredEditorFixture() {
  const fixture = createNativePort();
  const { context, port } = fixture;
  const methodStart = source.indexOf('  seedRestoredAutomationBaseline()');
  const methodEnd = source.indexOf('\n  async synchronizeNativeContext(', methodStart);
  assert.notEqual(methodStart, -1, 'restored automation baseline seed is missing');
  assert.notEqual(methodEnd, -1, 'restored automation method boundary is missing');
  vm.runInNewContext(`this.RestoredManager = class {${source.slice(methodStart, methodEnd)}\n};` +
    `this.rebuildPipeline = ({${source.slice(rebuildStart, rebuildEnd)}}).rebuildPipeline;`,
  context);

  const manager = new context.RestoredManager();
  Object.assign(manager, {
    currentPipeline: 'A',
    pipelineA: [],
    pipelineB: null,
    nativePort: port,
    hostAutomationApplyDepth: 0,
    preserveReadyNativePipelineDuringStartup: true,
    getCurrentPipeline() { return this.currentPipeline === 'B' ? this.pipelineB : this.pipelineA; },
    synchronizeNativeAssetMembership() {},
    scheduleLatencyService() {}
  });
  port.owner = manager;

  const nativeSnapshot = {
    pipeline: 'A', pluginId: 17, pluginType: 'DCOffsetPlugin',
    parameterKey: 'of', elementIndex: 0, normalized: 0.625
  };
  manager.applyHostAutomationDelta(nativeSnapshot);
  assert.equal(port.deferredHostAutomationDeltas.size, 1,
    'a snapshot arriving before restored plug-in construction remains queued');

  const plugin = {
    id: 17,
    type: 'DCOffsetPlugin',
    name: 'DC Offset',
    enabled: true,
    parameters: { of: 0 },
    getParameters() { return { ...this.parameters }; },
    setParameters(parameters) { this.parameters = parameters; },
    setEnabled(enabled) { this.enabled = enabled; }
  };
  manager.pipelineA = [plugin];
  await context.rebuildPipeline.call(manager);
  assert.equal(port.deferredHostAutomationDeltas.size, 0,
    'the restored baseline replays the early native snapshot');
  assert.equal(plugin.parameters.of, 0.25,
    'the replayed native snapshot wins over the serialized editor value');
  return { ...fixture, manager, plugin, nativeSnapshot };
}

test('reopened editor first bound edit uses the native snapshot without manual seeding', async () => {
  const { hostCalls, manager, plugin, port } = await createRestoredEditorFixture();
  plugin.parameters.of = 0.5;
  await port.postMessage({ type: 'updatePlugin', plugin });
  await new Promise(resolve => setTimeout(resolve, 0));

  const [sent] = automationEditPayloads(hostCalls);
  assert.ok(sent, 'the first restored-editor edit is emitted as host automation');
  assert.equal(sent.normalized, 0.75);
  manager.applyHostAutomationDelta({ ...sent, normalized: 0.875 });
  await new Promise(resolve => setTimeout(resolve, 20));

  assert.equal(plugin.parameters.of, 0.75,
    'a differing host delta replaces the optimistic first edit');
  assert.equal(port.adoptedAutomationValues.get('A:17:DCOffsetPlugin:of:0'), 0.875);
});

// A host with no Write lane armed answers the edit transaction with a refusal.
// That is the host declining to record the automation, not declining the edit,
// so the knob, the plug-in model and the baseline all stay where the user put
// them and nothing is sent a second time.
test('reopened editor first bound edit survives a host that will not record it', async () => {
  const { context, hostCalls, plugin, port } = await createRestoredEditorFixture();
  context.window.__effetuneHostCall = async (type, payload) => {
    hostCalls.push({ type, payload });
    return { ok: true };
  };
  plugin.parameters.of = 0.5;
  await port.postMessage({ type: 'updatePlugin', plugin });
  await new Promise(resolve => setTimeout(resolve, 20));

  assert.deepEqual(hostCalls.map(call => call.type), ['pipeline/updatePlugin'],
    'the refusal is not answered with a second request of any kind');
  assert.equal(plugin.parameters.of, 0.5,
    'the knob stays where the user left it');
  assert.equal(port.adoptedAutomationValues.get('A:17:DCOffsetPlugin:of:0'), 0.75,
    'and the baseline holds the value the plug-in adopted');
});

// The removed grace timer republished the previous value once a round trip
// outlasted 250 ms, and a republished value reaches performEdit -- so a
// Write-armed lane recorded a reverse-motion point the user never performed
// purely because the native side was busy. A plug-in update that rebuilds the
// whole native pipeline inside the request routinely takes that long.
test('a bridge slower than the old grace interval writes no reverse-motion edit', async () => {
  const fixture = await createRestoredEditorFixture();
  const { hostCalls, plugin, port } = fixture;
  silenceAutomationEdits(fixture);
  plugin.parameters.of = 0.5;
  // The request itself never settles, so nothing but a timer could end this.
  void port.postMessage({ type: 'updatePlugin', plugin });
  await new Promise(resolve => setTimeout(resolve, 400));

  assert.deepEqual(automationEditPayloads(hostCalls).map(edit => edit.normalized), [0.75],
    'the one value the user asked for is the only edit that ever travels');
  assert.equal(hostCalls.filter(call => call.type === 'pipeline/updatePlugin').length, 1,
    'and no image is republished behind it');
  assert.equal(plugin.parameters.of, 0.5, 'the knob is not moved under the user');
  assert.equal(port.adoptedAutomationValues.get('A:17:DCOffsetPlugin:of:0'), 0.75);
});

test('bound UI edits are gestures and do not precommit the native plugin update', async () => {
  const { context, hostCalls, port } = createNativePort();
  const [descriptor] = context.DSP_AUTOMATION_CATALOG.DCOffsetPlugin;
  assert.equal(descriptor.key, 'of');
  assert.equal(descriptor.publicName, 'offset');
  context.window.app.initialized = true;
  const plugin = { id: 17, type: 'DCOffsetPlugin', parameters: { of: 0 } };
  port.owner.pipelineA = [plugin];
  port.owner.pipelineB = [];
  port.owner.hostAutomationApplyDepth = 0;
  port.owner.applyHostAutomationDelta = delta => hostCalls.push({ type: 'reconcile', delta });
  port.rememberAdoptedPlugin('A', plugin);

  plugin.parameters.of = 0.5;
  await port.postMessage({ type: 'updatePlugin', plugin });
  await new Promise(resolve => setTimeout(resolve, 0));

  assert.deepEqual(hostCalls.map(call => call.type), ['pipeline/updatePlugin'],
    'the image and the gesture it carries cost one request between them');
  assert.equal(hostCalls[0].payload.plugin.parameters.of, 0.5,
    'the ordinary full image remains a non-authoritative UI payload');
  const [edit] = automationEditPayloads(hostCalls);
  assert.equal(edit.normalized, 0.75,
    'the generated conversion drives the VST gesture value');
  assert.equal(edit.parameterKey, descriptor.key,
    'the generated key crosses the WebView-to-native message boundary');
  assert.notEqual(edit.parameterKey, descriptor.publicName,
    'the display-only public name is not serialized as identity');
  assert.equal(port.adoptedAutomationValues.get('A:17:DCOffsetPlugin:of:0'), 0.75,
    'the gesture -- not the full image -- advances the host-adopted authority');
  assert.deepEqual(hostCalls.map(call => call.type), ['pipeline/updatePlugin'],
    'and nothing is sent behind it');
});

// The upstream controls report values only -- 'input' on a slider, 'change' on a
// select -- so the touch boundary a host records automation inside has to be
// derived from the pointer instead. A drag is one touch however many values it
// emits, and an edit made with no pointer down is a complete touch of its own.
test('a pointer drag derives one touch boundary per target instead of one per value',
  async () => {
    const { context, hostCalls, port } = createNativePort();
    const plugin = { id: 17, type: 'DCOffsetPlugin', parameters: { of: 0 } };
    port.owner.pipelineA = [plugin];
    port.owner.pipelineB = [];
    port.owner.hostAutomationApplyDepth = 0;
    port.owner.applyHostAutomationDelta = delta => hostCalls.push({ type: 'reconcile', delta });
    port.rememberAdoptedPlugin('A', plugin);

    context.window.dispatch('pointerdown');
    // Every value differs from the baseline the drag started on: change
    // detection compares against the adopted value, which the answers only
    // advance once the frame below publishes them.
    for (const value of [-0.5, 0.25, 0.5]) {
      plugin.parameters.of = value;
      void port.postMessage({ type: 'updatePlugin', plugin });
    }
    await new Promise(resolve => setTimeout(resolve, 0));

    const dragged = automationEditPayloads(hostCalls);
    assert.deepEqual(dragged.map(edit => edit.normalized), [0.25, 0.625, 0.75],
      'every value of the drag reaches the native side');
    // Every value asks for the touch, and the native open is idempotent, so the
    // drag is still the one beginEdit a host keys its automation writer on --
    // pinned natively by testRepeatedBeginKeepsOneTouchAndReopensAfterANativeClose.
    // Asking only once left a drag whose touch the native side had closed on its
    // own -- a suspend, a state restore, an editor detach -- with no touch window
    // for the rest of its values, and nothing on either side able to notice.
    assert.deepEqual(dragged.map(edit => edit.beginGesture), [true, true, true],
      'every value of the drag asks for the touch, so a lost one is recoverable');
    assert.deepEqual(dragged.map(edit => edit.endGesture), [false, false, false],
      'no value inside the drag ends the touch');
    assert.equal(hostCalls.filter(call => call.type === 'automation/endGesture').length, 0,
      'the touch stays open while the pointer is down');

    context.window.dispatch('pointerup');
    await new Promise(resolve => setTimeout(resolve, 0));

    const closes = hostCalls.filter(call => call.type === 'automation/endGesture');
    assert.equal(closes.length, 1, 'releasing the pointer closes the touch exactly once');
    assert.deepEqual(Array.from(closes[0].payload.targets, target => ({ ...target })), [{
      pipeline: 'A',
      pluginId: 17,
      pluginType: 'DCOffsetPlugin',
      parameterKey: 'of',
      elementIndex: 0
    }], 'the close names the target the drag moved, and no value');

    // No pointer is down, so this is a keyboard arrow or a typed value: one
    // complete touch of its own, which is what it has always been.
    plugin.parameters.of = 1;
    void port.postMessage({ type: 'updatePlugin', plugin });
    await new Promise(resolve => setTimeout(resolve, 0));
    const [discrete] = automationEditPayloads(hostCalls).slice(-1);
    assert.equal(discrete.normalized, 1);
    assert.equal(discrete.beginGesture, true,
      'an edit made with no pointer down opens its own touch');
    assert.equal(discrete.endGesture, true,
      'an edit made with no pointer down ends its own touch');
  });

// A release the WebView never sees is the case that leaves the host believing
// the user's hand is still on the control, so every other way a pointer can
// stop being down has to close the touch as well.
test('a cancelled pointer and a lost window focus each close an open touch', async () => {
  for (const closingEvent of ['pointercancel', 'lostpointercapture', 'blur']) {
    const { context, hostCalls, port } = createNativePort();
    const plugin = { id: 17, type: 'DCOffsetPlugin', parameters: { of: 0 } };
    port.owner.pipelineA = [plugin];
    port.owner.pipelineB = [];
    port.owner.hostAutomationApplyDepth = 0;
    port.owner.applyHostAutomationDelta = () => {};
    port.rememberAdoptedPlugin('A', plugin);

    context.window.dispatch('pointerdown');
    plugin.parameters.of = 0.5;
    void port.postMessage({ type: 'updatePlugin', plugin });
    await new Promise(resolve => setTimeout(resolve, 0));
    assert.equal(automationEditPayloads(hostCalls)[0].endGesture, false,
      `the touch is open before ${closingEvent}`);

    context.window.dispatch(closingEvent);
    await new Promise(resolve => setTimeout(resolve, 0));

    assert.equal(hostCalls.filter(call => call.type === 'automation/endGesture').length, 1,
      `${closingEvent} closes the open touch`);
    assert.equal(port.pointerGestureOpen, false,
      `${closingEvent} leaves no gesture open`);
  }
});

// One plug-in and one drag, ready to have a touch opened on it.
function createTouchFixture() {
  const fixture = createNativePort();
  const plugin = { id: 17, type: 'DCOffsetPlugin', parameters: { of: 0 } };
  fixture.port.owner.pipelineA = [plugin];
  fixture.port.owner.pipelineB = [];
  fixture.port.owner.hostAutomationApplyDepth = 0;
  fixture.port.owner.applyHostAutomationDelta = () => {};
  fixture.port.rememberAdoptedPlugin('A', plugin);
  const drag = async value => {
    plugin.parameters.of = value;
    void fixture.port.postMessage({ type: 'updatePlugin', plugin });
    await new Promise(resolve => setTimeout(resolve, 0));
  };
  const closes = () =>
    fixture.hostCalls.filter(call => call.type === 'automation/endGesture');
  return { ...fixture, plugin, drag, closes };
}

// Upstream renders every parameter as a focusable <input type="range"> beside an
// <input type="number">, so pressing on one control while another holds focus
// dispatches a blur on that other control. 'blur' does not bubble, but it does
// traverse the capture phase, so a capturing window listener sees every one of
// them: the press that had just opened the touch closed it again, and the rest
// of that drag was emitted as one-shot edits -- both original symptoms, on every
// drag after the first interaction.
test('a blur inside the editor leaves an open touch alone and a window blur ends it',
  async () => {
    const { context, drag, closes, hostCalls, port } = createTouchFixture();

    context.window.dispatch('pointerdown');
    await drag(0.5);

    // The control the press took the focus away from: a descendant of the
    // window, and an event that neither bubbles nor targets the window.
    context.window.dispatch('blur', { target: { tagName: 'INPUT', type: 'range' } });
    await new Promise(resolve => setTimeout(resolve, 0));
    assert.equal(closes().length, 0,
      'a blur on a control inside the editor does not end the touch');
    assert.equal(port.pointerGestureOpen, true,
      'the touch the press opened is still open');

    await drag(0.75);
    assert.deepEqual(automationEditPayloads(hostCalls).map(edit => edit.endGesture),
      [false, false],
      'every value of the drag is still emitted inside the one touch');

    context.window.dispatch('blur');
    await new Promise(resolve => setTimeout(resolve, 0));
    assert.equal(closes().length, 1,
      'a blur on the window itself ends the touch the pointer events never closed');
    assert.equal(port.pointerGestureOpen, false, 'and leaves no gesture open');

    // The behaviour above is what the registration below is for, and the phase
    // is the whole of it: every other listener the gesture uses is capturing.
    const registrations = context.window.listenersFor('blur');
    assert.equal(registrations.length, 1, 'the window carries one blur listener');
    assert.equal(registrations[0].capture, false,
      'the blur listener does not ask for the capture phase, so only a blur ' +
      'targeted at the window itself can reach it');
    assert.deepEqual(['pointerdown', 'pointerup', 'pointercancel', 'lostpointercapture']
      .map(type => context.window.listenersFor(type).every(entry => entry.capture)),
    [true, true, true, true],
    'every pointer listener is capturing, so no control can hide the boundary');
  });

// A pointer gesture is not one boolean. A second finger lifting reports a
// pointerup of its own while the user is still holding the control with the
// first -- and closing the touch there degrades the rest of that drag into
// one-shot edits. A second contact is not a fresh interaction either: the
// platform marks it isPrimary false, which is exactly what distinguishes it
// from the press that starts one.
test('a touch survives a second pointer and ends when the last one lifts', async () => {
  const { context, drag, closes, hostCalls, port } = createTouchFixture();

  context.window.dispatch('pointerdown', { pointerId: 1, isPrimary: true, buttons: 1 });
  await drag(0.5);
  context.window.dispatch('pointerdown', { pointerId: 2, isPrimary: false, buttons: 1 });
  context.window.dispatch('pointerup', { pointerId: 2, isPrimary: false, buttons: 0 });
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.equal(closes().length, 0,
    'lifting the second pointer does not end the touch the first still holds');
  assert.equal(port.pointerGestureOpen, true, 'the touch is still open');

  await drag(0.75);
  assert.deepEqual(automationEditPayloads(hostCalls).map(edit => edit.endGesture),
    [false, false],
    'the values after the second pointer still ride inside the same touch');

  context.window.dispatch('pointerup', { pointerId: 1, isPrimary: true, buttons: 0 });
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.equal(closes().length, 1, 'the last pointer lifting ends the touch');
  assert.equal(port.pointerGestureOpen, false, 'and leaves no gesture open');
});

// A mouse reports every one of its buttons under one pointer id, so the id
// alone cannot say whether the hand let go: pressing the right button mid-drag
// and releasing it raises a pointerdown and a pointerup that name the very
// pointer still holding the control. What separates them is buttons, which
// names what remains down after the release.
test('a touch survives a second mouse button pressed and released inside it', async () => {
  const { context, drag, closes, hostCalls, port } = createTouchFixture();

  context.window.dispatch('pointerdown',
    { pointerId: 1, isPrimary: true, button: 0, buttons: 1 });
  await drag(0.5);
  context.window.dispatch('pointerdown',
    { pointerId: 1, isPrimary: true, button: 2, buttons: 3 });
  context.window.dispatch('pointerup',
    { pointerId: 1, isPrimary: true, button: 2, buttons: 1 });
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.equal(closes().length, 0,
    'releasing the second button does not end the touch the first one holds');
  assert.equal(port.pointerGestureOpen, true, 'the touch is still open');

  await drag(0.75);
  assert.deepEqual(automationEditPayloads(hostCalls).map(edit => edit.endGesture),
    [false, false],
    'the values after the second button still ride inside the same touch');

  context.window.dispatch('pointerup',
    { pointerId: 1, isPrimary: true, button: 0, buttons: 0 });
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.equal(closes().length, 1, 'letting go of the last button ends the touch');
  assert.equal(port.pointerGestureOpen, false, 'and leaves no gesture open');
});

// The release the page never hears. Chromium opens a <select> popup on the
// press and takes capture, so the page sees the pointerdown and nothing else:
// no pointerup, no pointercancel, no lostpointercapture, and the focus never
// leaves the page, so no window blur either. Upstream renders filter types,
// crossover slopes and oversampling factors as <select>, and a native context
// menu on right-click loses the release the same way, so this is an ordinary
// interaction rather than an exotic one.
//
// A record of which pointers are down that only a matching release can empty
// never empties again after one of these: "no pointer is down" is unreachable,
// every later edit is stamped as sitting inside a touch, and the close that
// would end it is never reached. The host keeps a write lane open on every
// control the user touches afterwards, and the block keeps ignoring the
// automation the host plays back into each of them for the rest of the session.
test('a press whose release never arrives does not poison every later touch',
  async () => {
    const { context, drag, closes, hostCalls, port } = createTouchFixture();

    // A touchscreen press on a <select>: every contact is a pointer of its own,
    // and this one is released into the popup the page cannot see.
    context.window.dispatch('pointerdown', { pointerId: 2, isPrimary: true, buttons: 1 });

    // The control the user drags next, a whole interaction later.
    context.window.dispatch('pointerdown', { pointerId: 3, isPrimary: true, buttons: 1 });
    await drag(0.5);
    assert.equal(port.pointerGestureOpen, true, 'the later drag still opens a touch');
    assert.deepEqual(automationEditPayloads(hostCalls).map(edit => edit.endGesture), [false],
      'and its values still ride inside that touch');
    assert.equal(closes().length, 0, 'which is not ended while the pointer is down');

    context.window.dispatch('pointerup', { pointerId: 3, isPrimary: true, buttons: 0 });
    await new Promise(resolve => setTimeout(resolve, 0));

    assert.equal(closes().length, 1,
      'releasing the later drag still ends the touch it opened');
    assert.equal(port.pointerGestureOpen, false, 'and leaves no gesture open');
    assert.equal(port.livePointerIds.size, 0,
      'and leaves no pointer behind that a later release would have to account for');

    // The same class with a mouse, whose buttons all share one pointer id: a
    // right-click opens a native context menu that swallows the release.
    context.window.dispatch('pointerdown',
      { pointerId: 1, isPrimary: true, button: 2, buttons: 2 });
    context.window.dispatch('pointerdown',
      { pointerId: 1, isPrimary: true, button: 0, buttons: 1 });
    await drag(0.25);
    assert.equal(port.pointerGestureOpen, true,
      'the drag after the lost right-click release opens its own touch');

    context.window.dispatch('pointerup',
      { pointerId: 1, isPrimary: true, button: 0, buttons: 0 });
    await new Promise(resolve => setTimeout(resolve, 0));
    assert.equal(closes().length, 2, 'and releasing it ends that touch too');
    assert.equal(port.pointerGestureOpen, false, 'leaving no gesture open');
  });

// Closing the port releases the pointer listeners, so a touch still open here
// can never be closed by anything afterwards: the host would keep believing the
// user's hand is on the control, and the block would keep ignoring the
// automation the host plays back into it for the rest of the session.
test('closing the port ends a touch nothing else could release', async () => {
  const { context, drag, closes, port } = createTouchFixture();

  context.window.dispatch('pointerdown');
  await drag(0.5);
  port.close();
  await new Promise(resolve => setTimeout(resolve, 0));

  assert.equal(closes().length, 1, 'closing the port ends the open touch');
  assert.deepEqual(Array.from(closes()[0].payload.targets, target => ({ ...target })), [{
    pipeline: 'A',
    pluginId: 17,
    pluginType: 'DCOffsetPlugin',
    parameterKey: 'of',
    elementIndex: 0
  }], 'the close names the target the drag moved');
  assert.equal(port.pointerGestureOpen, false, 'and leaves no gesture open');
  for (const type of ['pointerdown', 'pointerup', 'pointercancel',
    'lostpointercapture', 'blur']) {
    assert.equal(context.window.hasListener(type), false,
      `a closed port leaves no ${type} listener behind`);
  }
});

test('a host delta that repeats the value a gesture sent changes nothing', async () => {
  const { hostCalls, port } = createNativePort();
  const reconciled = [];
  const plugin = { id: 17, type: 'DCOffsetPlugin', parameters: { of: 0 } };
  port.owner.pipelineA = [plugin];
  port.owner.hostAutomationApplyDepth = 0;
  port.owner.applyHostAutomationDelta = delta => reconciled.push(delta);
  port.rememberAdoptedPlugin('A', plugin);

  plugin.parameters.of = 0.5;
  await port.postMessage({ type: 'updatePlugin', plugin });
  await new Promise(resolve => setTimeout(resolve, 0));
  const [delta] = automationEditPayloads(hostCalls);
  port.owner.applyHostAutomationDelta(delta);

  assert.deepEqual(reconciled.map(item => item.normalized), [0.75],
    'an echo of the gesture is applied once and asks for the same value');
  assert.equal(port.adoptedAutomationValues.get('A:17:DCOffsetPlugin:of:0'), 0.75);
  assert.deepEqual(hostCalls.map(call => call.type), ['pipeline/updatePlugin'],
    'and nothing travels back out for it');
});

test('a differing host delta replaces the value the gesture adopted', async () => {
  const { hostCalls, port } = createNativePort();
  const reconciled = [];
  const plugin = { id: 17, type: 'DCOffsetPlugin', parameters: { of: 0 } };
  port.owner.pipelineA = [plugin];
  port.owner.hostAutomationApplyDepth = 0;
  port.owner.applyHostAutomationDelta = delta => reconciled.push(delta);
  port.rememberAdoptedPlugin('A', plugin);

  plugin.parameters.of = 0.5;
  await port.postMessage({ type: 'updatePlugin', plugin });
  await new Promise(resolve => setTimeout(resolve, 0));
  const [sent] = automationEditPayloads(hostCalls);
  port.owner.applyHostAutomationDelta({ ...sent, normalized: 0.625 });
  await new Promise(resolve => setTimeout(resolve, 20));

  assert.deepEqual(reconciled.map(item => item.normalized), [0.625],
    'host automation takes the lane back once the touch is over');
  assert.deepEqual(hostCalls.map(call => call.type), ['pipeline/updatePlugin'],
    'and the value it brought never travels back out as a gesture');
});

// Silence used to arm a rollback. The value is the plug-in's own from the moment
// it is sent, so an answer that never comes has nothing to undo.
test('an unanswered bridge gesture is left exactly as the user made it', async () => {
  const fixture = createNativePort();
  const { hostCalls, port } = fixture;
  const reconciled = [];
  const plugin = { id: 17, type: 'DCOffsetPlugin', parameters: { of: 0 } };
  port.owner.pipelineA = [plugin];
  port.owner.hostAutomationApplyDepth = 0;
  port.owner.applyHostAutomationDelta = delta => reconciled.push(delta);
  port.rememberAdoptedPlugin('A', plugin);
  silenceAutomationEdits(fixture);

  plugin.parameters.of = 0.5;
  // The request itself never settles, so nothing but a timer could end this.
  void port.postMessage({ type: 'updatePlugin', plugin });
  await new Promise(resolve => setTimeout(resolve, 30));

  assert.deepEqual(reconciled, [], 'nothing is put back');
  assert.equal(plugin.parameters.of, 0.5);
  assert.equal(port.adoptedAutomationValues.get('A:17:DCOffsetPlugin:of:0'), 0.75);
  assert.deepEqual(automationEditPayloads(hostCalls).map(edit => edit.normalized), [0.75],
    'and the one value the user asked for is the only edit that travelled');
});

test('an unbound gesture keeps the sent value instead of rolling back', async () => {
  const { context, hostCalls, port } = createNativePort();
  const reconciled = [];
  const plugin = { id: 17, type: 'DCOffsetPlugin', parameters: { of: 0 } };
  port.owner.pipelineA = [plugin];
  port.owner.hostAutomationApplyDepth = 0;
  port.owner.applyHostAutomationDelta = delta => reconciled.push(delta);
  port.rememberAdoptedPlugin('A', plugin);
  context.window.__effetuneHostCall = async (type, payload) => {
    hostCalls.push({ type, payload });
    return { ok: true };
  };

  plugin.parameters.of = 0.5;
  await port.postMessage({ type: 'updatePlugin', plugin });
  await new Promise(resolve => setTimeout(resolve, 15));

  assert.deepEqual(reconciled, [],
    'a refused allocation never rolls the value the user set back');
  assert.equal(port.adoptedAutomationValues.get('A:17:DCOffsetPlugin:of:0'), 0.75,
    'the sent value becomes the adopted baseline when no lane was assigned');
});

test('a bound gesture keeps the accepted value without waiting for a host delta', async () => {
  const { context, hostCalls, plugin, port } = createEchoingPluginFixture();
  context.window.__effetuneHostCall = async (type, payload) => {
    hostCalls.push({ type, payload });
    // A host that records the edit is free never to echo it back to the editor.
    return { ok: true };
  };

  plugin.parameters.of = 0.5;
  await port.postMessage({ type: 'updatePlugin', plugin });
  await new Promise(resolve => setTimeout(resolve, 20));

  assert.equal(plugin.parameters.of, 0.5,
    'the knob stays where the gesture left it instead of rolling back');
  assert.equal(port.adoptedAutomationValues.get('A:17:DCOffsetPlugin:of:0'), 0.75,
    'the value the user set becomes the adopted baseline');
  assert.equal(hostCalls.filter(call => call.type === 'pipeline/updatePlugin').length, 1,
    'no correction is republished over the value the host already took');
});

test('a plug-in enable toggle emits the synthetic node-enable gesture', async () => {
  const { hostCalls, port } = createNativePort();
  const plugin = { id: 17, type: 'DCOffsetPlugin', enabled: true, parameters: { of: 0 } };
  port.owner.pipelineA = [plugin];
  port.owner.pipelineB = [];
  port.owner.hostAutomationApplyDepth = 0;
  port.owner.applyHostAutomationDelta = () => {};

  await port.postMessage({ type: 'updatePlugin', plugin });
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.equal(automationEditPayloads(hostCalls).length, 0,
    'the first update only seeds the enable baseline');

  plugin.enabled = false;
  await port.postMessage({ type: 'updatePlugin', plugin });
  await new Promise(resolve => setTimeout(resolve, 0));

  const edits = automationEditPayloads(hostCalls);
  assert.equal(edits.length, 1, 'only the changed enable state becomes a gesture');
  assert.equal(edits[0].parameterKey, '__enabled');
  assert.equal(edits[0].elementIndex, 0);
  assert.equal(edits[0].normalized, 0);
});

test('a host node-enable delta toggles the plug-in and adopts its value', async () => {
  const { context, hostCalls, port } = createNativePort();
  const classBody = source.slice(source.indexOf('  applyHostAutomationDeltas('),
    source.indexOf('\n  async synchronizeNativeContext(', source.indexOf('  applyHostAutomationDeltas(')));
  vm.runInNewContext(`this.Manager = class {${classBody}\n};`, context);
  const plugin = {
    id: 17,
    type: 'DCOffsetPlugin',
    name: 'DC Offset',
    enabled: true,
    parameters: { of: 0 },
    getParameters() { return { ...this.parameters }; },
    setParameters(next) { this.parameters = { ...next }; },
    setEnabled(enabled) { this.enabled = enabled; }
  };
  const manager = new context.Manager();
  Object.assign(manager, {
    currentPipeline: 'A',
    pipelineA: [plugin],
    pipelineB: [],
    hostAutomationApplyDepth: 0,
    nativePort: port,
    getCurrentPipeline() { return this.pipelineA; },
    synchronizeNativeAssetMembership() {},
    scheduleLatencyService() {}
  });
  port.owner = manager;

  assert.equal(manager.applyHostAutomationDelta({
    pipeline: 'A', pluginId: 17, pluginType: 'DCOffsetPlugin',
    parameterKey: '__enabled', elementIndex: 0, normalized: 0
  }), true);
  assert.equal(plugin.enabled, false, 'the host delta disables the plug-in');
  assert.equal(port.adoptedAutomationValues.get('A:17:DCOffsetPlugin:__enabled:0'), 0);

  await port.postMessage({ type: 'updatePlugin', plugin });
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.equal(automationEditPayloads(hostCalls).length, 0,
    'an already adopted enable state does not echo back as a gesture');
});

test('a reopened editor enable toggle emits exactly one node-enable gesture', async () => {
  const { hostCalls, manager, plugin, port } = await createRestoredEditorFixture();

  plugin.enabled = false;
  await port.postMessage({ type: 'updatePlugin', plugin });
  await new Promise(resolve => setTimeout(resolve, 0));

  const edits = automationEditPayloads(hostCalls);
  assert.equal(edits.length, 1,
    'the restored baseline covers __enabled, so the first toggle is a real gesture');
  assert.equal(edits[0].parameterKey, '__enabled');
  assert.equal(edits[0].normalized, 0);
  await new Promise(resolve => setTimeout(resolve, 15));

  const disabled = {
    id: 18,
    type: 'DCOffsetPlugin',
    name: 'DC Offset',
    enabled: false,
    parameters: { of: 0 },
    getParameters() { return { ...this.parameters }; },
    setParameters(next) { this.parameters = { ...next }; },
    setEnabled(enabled) { this.enabled = enabled; }
  };
  manager.pipelineA = [plugin, disabled];
  manager.seedRestoredAutomationBaseline();
  await port.postMessage({ type: 'updatePlugin', plugin: disabled });
  await new Promise(resolve => setTimeout(resolve, 0));

  assert.equal(automationEditPayloads(hostCalls).filter(edit =>
    edit.pluginId === 18).length, 0,
  'an effect restored in the disabled state never emits an untouched gesture');
});

test('a host node-enable delta drives the upstream transition without echoing', async () => {
  const { context, hostCalls, port } = createNativePort();
  const classBody = source.slice(source.indexOf('  applyHostAutomationDeltas('),
    source.indexOf('\n  async synchronizeNativeContext(', source.indexOf('  applyHostAutomationDeltas(')));
  vm.runInNewContext(`this.Manager = class {${classBody}\n};`, context);
  const toggles = [];
  context.document.querySelector = selector =>
    selector === '.pipeline-item[data-plugin-id="17"] .toggle-button'
      ? { classList: { toggle: (name, force) => toggles.push([name, force]) } }
      : null;
  const plugin = {
    id: 17,
    type: 'DCOffsetPlugin',
    name: 'DC Offset',
    enabled: true,
    animating: true,
    parameters: { of: 0 },
    getParameters() { return { ...this.parameters }; },
    setParameters(next) { this.parameters = { ...next }; },
    // Mirrors PluginBase.setEnabled: assign, publish the update, resync the redraw loop.
    setEnabled(enabled) {
      if (this.enabled === enabled) return;
      this.enabled = enabled;
      port.postMessage({ type: 'updatePlugin', plugin: this });
      this.animating = enabled;
    }
  };
  const manager = new context.Manager();
  Object.assign(manager, {
    currentPipeline: 'A',
    pipelineA: [plugin],
    pipelineB: [],
    hostAutomationApplyDepth: 0,
    nativePort: port,
    getCurrentPipeline() { return this.pipelineA; },
    synchronizeNativeAssetMembership() {},
    scheduleLatencyService() {}
  });
  port.owner = manager;

  assert.equal(manager.applyHostAutomationDelta({
    pipeline: 'A', pluginId: 17, pluginType: 'DCOffsetPlugin',
    parameterKey: '__enabled', elementIndex: 0, normalized: 0
  }), true);
  await new Promise(resolve => setTimeout(resolve, 0));

  assert.equal(plugin.enabled, false);
  assert.equal(plugin.animating, false,
    'the upstream transition stops the redraw loop instead of leaving it orphaned');
  assert.deepEqual(toggles, [['off', true]], 'the pipeline item toggle reflects the host state');
  assert.deepEqual(hostCalls, [], 'the applied host state never echoes back to the native side');
  assert.equal(manager.hostAutomationApplyDepth, 0);

  assert.equal(manager.applyHostAutomationDelta({
    pipeline: 'A', pluginId: 17, pluginType: 'DCOffsetPlugin',
    parameterKey: '__enabled', elementIndex: 0, normalized: 1
  }), true);
  await new Promise(resolve => setTimeout(resolve, 0));

  assert.equal(plugin.animating, true, 'returning to enabled restarts the redraw loop');
  assert.deepEqual(toggles[1], ['off', false]);
  assert.deepEqual(hostCalls, []);
});

function createEchoingPluginFixture({ type = 'DCOffsetPlugin', name = 'DC Offset',
  parameters = { of: 0 }, retain = next => next } = {}) {
  const fixture = createNativePort();
  const { context, port } = fixture;
  const classBody = source.slice(source.indexOf('  applyHostAutomationDeltas('),
    source.indexOf('\n  async synchronizeNativeContext(',
      source.indexOf('  applyHostAutomationDeltas(')));
  vm.runInNewContext(`this.Manager = class {${classBody}\n};`, context);
  // Mirrors PluginBase: every accepted mutation republishes the full image, and
  // retain() stands for the coercion a plug-in applies to the value it is given.
  const plugin = {
    id: 17,
    type,
    name,
    enabled: true,
    parameters: { ...parameters },
    getParameters() { return { ...this.parameters }; },
    setParameters(next) {
      this.parameters = retain({ ...next });
      port.postMessage({ type: 'updatePlugin', plugin: this });
    },
    setEnabled(enabled) {
      if (this.enabled === enabled) return;
      this.enabled = enabled;
      port.postMessage({ type: 'updatePlugin', plugin: this });
    }
  };
  const manager = new context.Manager();
  Object.assign(manager, {
    currentPipeline: 'A',
    pipelineA: [plugin],
    pipelineB: [],
    hostAutomationApplyDepth: 0,
    nativePort: port,
    getCurrentPipeline() { return this.pipelineA; },
    synchronizeNativeAssetMembership() {},
    scheduleLatencyService() {}
  });
  port.owner = manager;
  port.rememberAdoptedPlugin('A', plugin);
  return { ...fixture, manager, plugin };
}

// A plug-in update that cannot reuse the running instances rebuilds the whole
// native pipeline inside the request, so an answer can take far longer than a
// drag's own cadence. Nothing waits on it: the value was the plug-in's own when
// it was sent, so the slow answer only confirms what both sides already hold.
test('a slow but answered request needs nothing taken back', async () => {
  const { context, hostCalls, plugin, port } = createEchoingPluginFixture();
  let answer = null;
  context.window.__effetuneHostCall = async (type, payload) => {
    hostCalls.push({ type, payload });
    if (type !== 'pipeline/updatePlugin') return { ok: true };
    return new Promise(resolve => { answer = resolve; });
  };

  plugin.parameters.of = 0.5;
  void port.postMessage({ type: 'updatePlugin', plugin });
  await new Promise(resolve => setTimeout(resolve, 30));
  assert.equal(plugin.parameters.of, 0.5,
    'the still-unanswered gesture is not moved under the user');

  answer({ ok: true });
  await new Promise(resolve => setTimeout(resolve, 20));

  assert.equal(plugin.parameters.of, 0.5,
    'and the answer leaves it exactly there');
  assert.equal(port.adoptedAutomationValues.get('A:17:DCOffsetPlugin:of:0'), 0.75);
  assert.equal(hostCalls.filter(call => call.type === 'pipeline/updatePlugin').length, 1,
    'one gesture costs one request, however long the native side takes');
});

test('an enable gesture the host will not record still stays toggled', async () => {
  const { context, hostCalls, plugin, port } = createEchoingPluginFixture();
  context.window.__effetuneHostCall = async (type, payload) => {
    hostCalls.push({ type, payload });
    return { ok: true };
  };

  plugin.enabled = false;
  await port.postMessage({ type: 'updatePlugin', plugin });
  await new Promise(resolve => setTimeout(resolve, 20));

  assert.equal(plugin.enabled, false, 'the node the user switched off stays off');
  assert.equal(hostCalls.filter(call => call.type === 'pipeline/updatePlugin').length, 1,
    'and the node is not put back into the native graph behind the user');
  assert.deepEqual(automationEditPayloads(hostCalls).map(edit =>
    [edit.parameterKey, edit.normalized]), [['__enabled', 0]],
  'the one toggle the user made is the only node-enable gesture that travels');
  assert.equal(port.adoptedAutomationValues.get('A:17:DCOffsetPlugin:__enabled:0'), 0);
});

// A host is free to record some lanes and not others -- one armed for Write and
// one not. Neither answer changes what the plug-in plays: every gesture of the
// bundle is the plug-in's own value.
test('a bundle the host records only in part keeps every gesture in it', async () => {
  const { context, hostCalls, port } = createNativePort();
  const reconciled = [];
  const plugin = { id: 17, type: 'DCOffsetPlugin', enabled: true, parameters: { of: 0 } };
  port.owner.pipelineA = [plugin];
  port.owner.pipelineB = [];
  port.owner.hostAutomationApplyDepth = 0;
  port.owner.applyHostAutomationDelta = delta => reconciled.push(delta);
  port.rememberAdoptedPlugin('A', plugin);
  context.window.__effetuneHostCall = async (type, payload) => {
    hostCalls.push({ type, payload });
    return { ok: true };
  };

  plugin.parameters.of = 0.5;
  plugin.enabled = false;
  await port.postMessage({ type: 'updatePlugin', plugin });
  await new Promise(resolve => setTimeout(resolve, 15));

  assert.deepEqual(automationEditPayloads(hostCalls).map(edit => edit.parameterKey),
    ['of', '__enabled'],
    'one request carries every gesture the frame collected');
  assert.deepEqual(reconciled, [], 'and none of them is put back');
  assert.equal(port.adoptedAutomationValues.get('A:17:DCOffsetPlugin:of:0'), 0.75);
  assert.equal(port.adoptedAutomationValues.get('A:17:DCOffsetPlugin:__enabled:0'), 0);
  assert.deepEqual(hostCalls.map(call => call.type), ['pipeline/updatePlugin'],
    'nothing is sent a second time');
});

// Change detection re-derives the normalized value from the plug-in's own
// storage, so the baseline has to hold that canonical form. A logarithmic
// frequency is not a fixed point of the normalized -> plain -> packed -> plain
// -> normalized round trip: a baseline holding the raw host number would read
// the very next image of that plug-in as a gesture the user never made.
test('a host value of a lossy descriptor is adopted in its canonical storage form',
  async () => {
    const fixture = createEchoingPluginFixture({
      type: 'AutoFilterPlugin', name: 'Auto Filter', parameters: { lf: 99.92 }
    });
    const { context, hostCalls, manager, plugin } = fixture;
    const { port } = fixture;
    const identity = 'A:17:AutoFilterPlugin:lf:0';
    const descriptor = context.DSP_AUTOMATION_CATALOG.AutoFilterPlugin
      .find(candidate => candidate.key === 'lf');
    assert.equal(descriptor.normalization, 'log');
    const canonical = value => context.normalizeDSPAutomationValue(descriptor,
      context.unpackDSPAutomationValue(descriptor, value));
    const played = canonical(1000);
    assert.equal(Object.is(context.packDSPAutomationValue(descriptor,
      context.denormalizeDSPAutomationValue(descriptor, played)), 1000), false,
    'the value exercises a storage round trip that does not return the same number');

    manager.applyHostAutomationDelta({
      pipeline: 'A', pluginId: 17, pluginType: 'AutoFilterPlugin',
      parameterKey: 'lf', elementIndex: 0, normalized: played
    });
    await new Promise(resolve => setTimeout(resolve, 20));

    assert.equal(port.adoptedAutomationValues.get(identity),
      canonical(plugin.parameters.lf),
      'the baseline holds the value the plug-in actually stores');
    assert.deepEqual(hostCalls, [],
      'and the host value never travels back out as a gesture of its own');
  });

// The same invariant where the plug-in itself coerces the write. Mirrors
// G726ADPCMSimulatorPlugin: the value is snapped to a 0.1 grid, so what the
// plug-in retains is not what the write asked it to store.
test('a host value a quantizing plug-in cannot retain is adopted in the form it kept',
  async () => {
    const fixture = createEchoingPluginFixture({
      type: 'G726ADPCMSimulatorPlugin',
      name: 'G726 ADPCM Simulator',
      parameters: { re: -3 },
      retain: next => ({ ...next, re: Math.round(next.re * 10) / 10 })
    });
    const { context, hostCalls, manager, plugin, port } = fixture;
    const identity = 'A:17:G726ADPCMSimulatorPlugin:re:0';
    const descriptor = context.DSP_AUTOMATION_CATALOG.G726ADPCMSimulatorPlugin
      .find(candidate => candidate.key === 're');
    const canonical = value => context.normalizeDSPAutomationValue(descriptor,
      context.unpackDSPAutomationValue(descriptor, value));
    const asked = 0.765;
    const written = context.packDSPAutomationValue(descriptor,
      context.denormalizeDSPAutomationValue(descriptor, asked));
    const snapped = Math.round(written * 10) / 10;
    assert.equal(Object.is(written, snapped), false,
      'the write asks for a value off the grid the plug-in snaps to');

    manager.applyHostAutomationDelta({
      pipeline: 'A', pluginId: 17, pluginType: 'G726ADPCMSimulatorPlugin',
      parameterKey: 're', elementIndex: 0, normalized: asked
    });
    await new Promise(resolve => setTimeout(resolve, 20));

    assert.equal(plugin.parameters.re, snapped,
      'the plug-in retains its own snapped value');
    assert.equal(port.adoptedAutomationValues.get(identity), canonical(snapped),
      'and the baseline holds that value, not the one the write asked for');
    assert.notEqual(port.adoptedAutomationValues.get(identity), asked,
      'the two really do differ, so the re-derivation is what is under test');
    assert.deepEqual(hostCalls, [],
      'so the very next image of the plug-in reports no gesture at all');
  });

test('a failed plugin update reconciles without republishing to the native pipeline', async () => {
  const { context, hostCalls, plugin, port } = createEchoingPluginFixture();
  context.window.__effetuneHostCall = async (type, payload) => {
    hostCalls.push({ type, payload });
    throw new Error('rejected');
  };

  plugin.parameters.of = 0.5;
  await port.postMessage({ type: 'updatePlugin', plugin });
  await new Promise(resolve => setTimeout(resolve, 0));

  assert.equal(plugin.parameters.of, 0);
  assert.deepEqual(hostCalls.map(call => call.type), ['pipeline/updatePlugin'],
    'a value the native side never accepted needs no correction sent back to it');
});

test('a failed rebuild reconciles without republishing to the native pipeline', async () => {
  const { context, hostCalls, plugin, port } = createEchoingPluginFixture();
  context.window.__effetuneHostCall = async (type, payload) => {
    hostCalls.push({ type, payload });
    throw new Error('rejected');
  };

  plugin.parameters.of = 0.5;
  await port.postMessage({ type: 'updatePlugins', plugins: [plugin] });
  await new Promise(resolve => setTimeout(resolve, 0));

  assert.equal(plugin.parameters.of, 0);
  assert.deepEqual(hostCalls.map(call => call.type), ['pipeline/rebuild'],
    'a rejected rebuild leaves the native image untouched, so no echo is needed');
});

// Two requests for the same target can be in flight at once, and nothing
// promises the answers come back in the order they were sent. The value is
// adopted where the change is detected instead, so the answers decide nothing
// and the baseline can never be walked backwards by a stale one.
test('an answer arriving after a newer gesture cannot move the baseline back', async () => {
  const { context, hostCalls, port } = createNativePort();
  const pendingResponses = [];
  const plugin = { id: 17, type: 'DCOffsetPlugin', enabled: true, parameters: { of: 0 } };
  port.owner.pipelineA = [plugin];
  port.owner.pipelineB = [];
  port.owner.hostAutomationApplyDepth = 0;
  port.owner.applyHostAutomationDelta = () => {};
  port.rememberAdoptedPlugin('A', plugin);
  context.window.__effetuneHostCall = async (type, payload) => {
    hostCalls.push({ type, payload });
    if (type !== 'pipeline/updatePlugin' || !payload.automationEdits?.length) {
      return { ok: true };
    }
    return new Promise(resolve => pendingResponses.push(resolve));
  };

  plugin.parameters.of = 0.5;
  // Neither request is answered yet, so both gestures stay in flight.
  void port.postMessage({ type: 'updatePlugin', plugin });
  await new Promise(resolve => setTimeout(resolve, 0));
  plugin.parameters.of = 0.25;
  void port.postMessage({ type: 'updatePlugin', plugin });
  await new Promise(resolve => setTimeout(resolve, 0));

  const edits = automationEditPayloads(hostCalls);
  assert.equal(edits.length, 2, 'both knob positions were sent while the host stayed silent');
  assert.equal(pendingResponses.length, 2);

  // The newer gesture is answered first; the stale one arrives afterwards.
  pendingResponses[1]({ ok: true });
  await new Promise(resolve => setTimeout(resolve, 0));
  const identity = 'A:17:DCOffsetPlugin:of:0';
  assert.equal(port.adoptedAutomationValues.get(identity), edits[1].normalized);

  pendingResponses[0]({ ok: true });
  await new Promise(resolve => setTimeout(resolve, 0));

  assert.equal(port.adoptedAutomationValues.get(identity), edits[1].normalized,
    'the superseded response leaves the latest adopted value untouched');
});

// One pointer drag: the DOM emits an input event per mouse report, so several
// full images of the same plug-in are published inside a single displayed frame.
function createDragFixture() {
  const fixture = createNativePort();
  const { context, port } = fixture;
  const frames = [];
  context.requestAnimationFrame = callback => frames.push(callback);
  context.cancelAnimationFrame = handle => { frames[handle - 1] = null; };
  const plugins = [17, 18].map(id => ({
    id, type: 'DCOffsetPlugin', enabled: true, parameters: { of: 0 }
  }));
  port.owner.pipelineA = plugins;
  port.owner.pipelineB = [];
  port.owner.hostAutomationApplyDepth = 0;
  port.owner.applyHostAutomationDelta = () => {};
  port.rememberAdoptedPlugin('A', plugins[0]);
  port.rememberAdoptedPlugin('A', plugins[1]);
  const [descriptor] = context.DSP_AUTOMATION_CATALOG.DCOffsetPlugin;
  const drag = (plugin, value) => {
    plugin.parameters.of = value;
    return port.postMessage({ type: 'updatePlugin', plugin });
  };
  const normalized = value => context.normalizeDSPAutomationValue(descriptor,
    context.unpackDSPAutomationValue(descriptor, value));
  return { ...fixture, drag, frames, normalized, plugins };
}

test('a drag coalesces into one plug-in update per frame and lands on its last value', async () => {
  const { drag, frames, hostCalls, normalized, plugins, port } = createDragFixture();
  const positions = [0.1, 0.2, 0.3, 0.4];
  const dispatched = positions.map(value => drag(plugins[0], value));

  assert.deepEqual(hostCalls, [], 'no bridge round trip is made before the frame');
  assert.equal(frames.length, 1, 'the whole burst arms a single frame');

  frames[0]();
  await Promise.all(dispatched);

  const updates = hostCalls.filter(call => call.type === 'pipeline/updatePlugin');
  assert.equal(updates.length, 1, 'the burst becomes one plug-in update');
  assert.equal(updates[0].payload.plugin.parameters.of, 0.4,
    'the value the drag ended on is the one the DSP receives');
  assert.deepEqual(automationEditPayloads(hostCalls).map(edit => edit.normalized),
    positions.map(normalized),
    'every gesture of the drag still reaches the host, in the order it was made');
  assert.deepEqual(Array.from(updates[0].payload.automationEdits, edit => edit.normalized),
    positions.map(normalized),
    'the whole frame of gestures travels inside the one image it belongs to');
  assert.equal(port.pendingPluginUpdates.size, 0);
  assert.equal(port.adoptedAutomationValues.get('A:17:DCOffsetPlugin:of:0'),
    normalized(0.4), 'the gesture the drag settled on becomes the adopted baseline');
});

// A frame coalesces every value a drag emitted, so a failed frame carries
// several edits for the same identity -- and each one names only the value the
// edit before it replaced. Applying them in turn leaves the editor on the
// second-to-last value of the frame while the native side still holds the value
// the frame started from. The editor and its baseline then agree, so change
// detection emits nothing and nothing ever corrects the disagreement.
test('a failed frame returns each identity to the value the native side holds',
  async () => {
    const { context, drag, frames, hostCalls, normalized, plugins, port } =
      createDragFixture();
    const [descriptor] = context.DSP_AUTOMATION_CATALOG.DCOffsetPlugin;
    const identity = 'A:17:DCOffsetPlugin:of:0';
    const reconciled = [];
    port.owner.applyHostAutomationDelta = delta => {
      reconciled.push(delta);
      plugins[0].parameters.of = context.packDSPAutomationValue(descriptor,
        context.denormalizeDSPAutomationValue(descriptor, delta.normalized));
      port.adoptedAutomationValues.set(identity, delta.normalized);
    };

    // The first frame of the drag is answered, so the native side holds the
    // value that frame ended on.
    context.window.dispatch('pointerdown');
    const answered = [0.1, 0.2].map(value => drag(plugins[0], value));
    frames[0]();
    await Promise.all(answered);
    assert.equal(port.adoptedAutomationValues.get(identity), normalized(0.2),
      'the answered frame leaves both sides on the value it ended on');

    // pipeline/updatePlugin genuinely answers false when the native rebuild or
    // the latency synchronization inside it fails.
    context.window.__effetuneHostCall = async (type, payload) => {
      hostCalls.push({ type, payload });
      if (type === 'pipeline/updatePlugin') throw new Error('native rebuild refused');
      return { ok: true };
    };
    const refused = [0.3, 0.4, 0.5].map(value => drag(plugins[0], value));
    frames[1]();
    await Promise.all(refused);

    const failed = hostCalls.filter(call => call.type === 'pipeline/updatePlugin').pop();
    assert.equal(failed.payload.automationEdits.length, 3,
      'the failed frame really did carry three edits for the one identity');
    assert.deepEqual(reconciled.map(delta => delta.normalized), [normalized(0.2)],
      'the identity is returned to the value the native side holds, and only there');
    assert.ok(Math.abs(plugins[0].parameters.of - 0.2) < 1.0e-12,
      'the knob is put back where the native side still is');
    assert.equal(port.adoptedAutomationValues.get(identity), normalized(0.2),
      'and the baseline follows it, so the next real change is still detected');
  });

test('coalescing keeps the newest image of every plug-in the frame touched', async () => {
  const { drag, frames, hostCalls, plugins } = createDragFixture();
  const dispatched = [
    drag(plugins[0], 0.1),
    drag(plugins[1], 0.2),
    drag(plugins[0], 0.3)
  ];

  frames[0]();
  await Promise.all(dispatched);

  const updates = hostCalls.filter(call => call.type === 'pipeline/updatePlugin');
  assert.deepEqual(updates.map(call => [call.payload.plugin.id, call.payload.plugin.parameters.of]),
    [[17, 0.3], [18, 0.2]],
    'a second plug-in edited in the same frame is never dropped by the first');
});

test('a hidden editor still publishes the value a gesture ended on', async () => {
  const { context, drag, frames, hostCalls, plugins } = createDragFixture();
  // A WebView the host stopped painting runs no animation frames, so a gesture
  // that ended just before it was hidden must not stay queued behind one.
  context.document.hidden = true;

  drag(plugins[0], 0.1);
  await drag(plugins[0], 0.4);

  assert.equal(frames.length, 0, 'an unpainted editor is never waited on for a frame');
  const updates = hostCalls.filter(call => call.type === 'pipeline/updatePlugin');
  assert.equal(updates.length, 1);
  assert.equal(updates[0].payload.plugin.parameters.of, 0.4);
});

test('an editor hidden mid-gesture flushes instead of waiting for a frame', async () => {
  const { context, drag, frames, hostCalls, plugins } = createDragFixture();
  const dispatched = drag(plugins[0], 0.4);
  assert.equal(frames.length, 1, 'a painted editor waits for its next frame');

  // The host hides the editor before that frame is ever painted.
  context.document.hidden = true;
  context.document.dispatch('visibilitychange');
  await dispatched;

  assert.equal(frames[0], null, 'the frame that will never run is cancelled');
  const updates = hostCalls.filter(call => call.type === 'pipeline/updatePlugin');
  assert.equal(updates.length, 1, 'the value the gesture ended on is not stranded');
  assert.equal(updates[0].payload.plugin.parameters.of, 0.4);
});

test('a frame that is never painted still publishes before the deadline', async () => {
  const { drag, frames, hostCalls, plugins, port } = createDragFixture();
  // An offscreen or fully occluded WebView keeps document.hidden false and runs
  // no animation frames at all, so the armed frame below never arrives.
  port.pluginUpdateFlushDeadlineMs = 5;

  const dispatched = drag(plugins[0], 0.4);
  assert.equal(frames.length, 1, 'a painted editor is still preferred to the deadline');
  assert.deepEqual(hostCalls, []);

  // Only the deadline can settle this: the frame callback is never invoked.
  assert.equal(await Promise.race([
    dispatched.then(() => 'published'),
    new Promise(resolve => setTimeout(() => resolve('stranded'), 200))
  ]), 'published', 'a pending update without a frame has a deadline of its own');

  assert.equal(frames[0], null, 'the frame that never ran is cancelled');
  const updates = hostCalls.filter(call => call.type === 'pipeline/updatePlugin');
  assert.equal(updates.length, 1, 'the value the gesture ended on is not stranded');
  assert.equal(updates[0].payload.plugin.parameters.of, 0.4);
});

test('an editor being torn down publishes the value the last gesture ended on', async () => {
  const { context, drag, frames, hostCalls, plugins, port } = createDragFixture();
  const dispatched = drag(plugins[0], 0.4);
  assert.equal(frames.length, 1);

  // Closing the editor destroys the page outright, so the pending frame has to
  // leave before it rather than after.
  context.window.dispatch('pagehide');
  await dispatched;

  assert.equal(frames[0], null, 'the frame that will never run is cancelled');
  const updates = hostCalls.filter(call => call.type === 'pipeline/updatePlugin');
  assert.equal(updates.length, 1);
  assert.equal(updates[0].payload.plugin.parameters.of, 0.4);

  port.close();
  assert.equal(context.window.hasListener('pagehide'), false,
    'a closed port releases the listeners that publish for it');
  assert.equal(context.document.hasListener('visibilitychange'), false);
});

test('a later pipeline rebuild never overtakes a coalesced plug-in update', async () => {
  const { drag, hostCalls, plugins, port } = createDragFixture();
  const dispatched = drag(plugins[0], 0.5);

  await port.postMessage({ type: 'updatePlugins', plugins });
  await dispatched;

  assert.deepEqual(
    hostCalls.map(call => call.type).filter(type => type.startsWith('pipeline/')),
    ['pipeline/updatePlugin', 'pipeline/rebuild'],
    'the deferred image is published before the rebuild that replaces it');
  assert.equal(port.pendingPluginUpdates.size, 0);
});

// A frame the editor is still waiting on, with the plug-in reachable through the
// owner so the flush can read it back instead of trusting its own snapshot.
function createPendingFrameFixture() {
  const fixture = createEchoingPluginFixture();
  const frames = [];
  fixture.context.requestAnimationFrame = callback => frames.push(callback);
  fixture.context.cancelAnimationFrame = handle => { frames[handle - 1] = null; };
  return { ...fixture, frames };
}

test('a pending frame publishes the host value written while it waited', async () => {
  const { frames, hostCalls, manager, plugin, port } = createPendingFrameFixture();

  // The user moves the knob; the image is queued for the next frame.
  plugin.parameters.of = 0.5;
  const dispatched = port.postMessage({ type: 'updatePlugin', plugin });
  assert.equal(frames.length, 1, 'the gesture waits for its frame');
  assert.deepEqual(hostCalls, []);

  // The host writes the authoritative value into the plug-in before that frame.
  manager.applyHostAutomationDelta({
    pipeline: 'A', pluginId: 17, pluginType: 'DCOffsetPlugin',
    parameterKey: 'of', elementIndex: 0, normalized: 0.875
  });
  assert.equal(plugin.parameters.of, 0.75, 'the host value reaches the plug-in');

  frames[0]();
  await dispatched;

  const updates = hostCalls.filter(call => call.type === 'pipeline/updatePlugin');
  assert.equal(updates.length, 1);
  assert.equal(updates[0].payload.plugin.parameters.of, 0.75,
    'the frame publishes the plug-in as it stands, not the snapshot the event took, ' +
    'so the host value is not carried back out of the native pipeline');
});

test('host automation deltas are written into the plug-in before the pending frame leaves',
  async () => {
    const { frames, hostCalls, manager, plugin, port } = createPendingFrameFixture();

    plugin.parameters.of = 0.25;
    const dispatched = port.postMessage({ type: 'updatePlugin', plugin });
    assert.deepEqual(hostCalls, [], 'nothing has reached the native side yet');

    manager.applyHostAutomationDeltas([{
      pipeline: 'A', pluginId: 17, pluginType: 'DCOffsetPlugin',
      parameterKey: 'of', elementIndex: 0, normalized: 0.875
    }]);

    assert.deepEqual(hostCalls.map(call => call.type), ['pipeline/updatePlugin'],
      'the queued frame still leaves inside this call, ahead of every later message');
    assert.equal(frames[0], null, 'the frame it no longer needs is cancelled');
    // The flush re-reads the plug-in it publishes, so flushing after the deltas
    // were written is the only order in which that re-read can carry them. A
    // flush that ran first would send the frame before them, and updatePlugin
    // never overlays, so the native state document would keep the stale value
    // until that lane publishes a point again.
    assert.equal(hostCalls[0].payload.plugin.parameters.of, 0.75,
      'the published image holds the host value, not the frame before it');
    assert.deepEqual(Array.from(hostCalls[0].payload.automationEdits,
      edit => edit.normalized), [0.625],
    'and the gesture the user made still travels with it, unthinned');
    await dispatched;
  });

test('the gestures of one frame travel in the order they happened', async () => {
  const { context, frames, hostCalls, plugin, port } = createPendingFrameFixture();
  const [descriptor] = context.DSP_AUTOMATION_CATALOG.DCOffsetPlugin;
  const normalized = value => context.normalizeDSPAutomationValue(descriptor,
    context.unpackDSPAutomationValue(descriptor, value));

  // A drag emits its next event about 8 ms later, well inside the same frame.
  plugin.parameters.of = 0.25;
  port.postMessage({ type: 'updatePlugin', plugin });
  plugin.parameters.of = 0.5;
  const dispatched = port.postMessage({ type: 'updatePlugin', plugin });
  assert.equal(frames.length, 1, 'both messages share the one queued frame');

  frames[0]();
  await dispatched;

  const [update] = hostCalls.filter(call => call.type === 'pipeline/updatePlugin');
  // The native side applies the array from the front, so the entry it ends on is
  // the value the block-start pin keeps playing. A value kept anywhere but this
  // one ordered list would be applied out of turn, and the knob would show one
  // value while the DSP played another.
  assert.deepEqual(Array.from(update.payload.automationEdits, edit => edit.normalized),
    [normalized(0.25), normalized(0.5)],
    'every value the frame collected travels, in the order the user made them');
  assert.equal(port.adoptedAutomationValues.get('A:17:DCOffsetPlugin:of:0'), normalized(0.5),
    'the value the frame ended on is the adopted one');
});

test('the deltas one packet carries for a plug-in cost a single parameter write', () => {
  const { context, port } = createNativePort();
  const classBody = source.slice(source.indexOf('  applyHostAutomationDeltas('),
    source.indexOf('\n  async synchronizeNativeContext(',
      source.indexOf('  applyHostAutomationDeltas(')));
  vm.runInNewContext(`this.Manager = class {${classBody}\n};`, context);
  const descriptors = context.DSP_AUTOMATION_CATALOG.PhaserPlugin.slice(0, 2);
  assert.equal(descriptors.length, 2, 'the plug-in exposes several automation lanes');
  const writes = [];
  // Mirrors the upstream parameter wrapper: every write packs the plug-in's whole
  // parameter image, which is the cost a take moving several lanes multiplies.
  const createPlugin = id => ({
    id,
    type: 'PhaserPlugin',
    name: 'Phaser',
    enabled: true,
    parameters: Object.fromEntries(context.DSP_AUTOMATION_CATALOG.PhaserPlugin
      .map(descriptor => [descriptor.field, descriptor.default])),
    getParameters() { return { ...this.parameters }; },
    setParameters(next) { this.parameters = { ...next }; writes.push(this.id); }
  });
  const moved = createPlugin(23);
  const other = createPlugin(24);
  const manager = new context.Manager();
  Object.assign(manager, {
    currentPipeline: 'A',
    pipelineA: [moved, other],
    pipelineB: [],
    hostAutomationApplyDepth: 0,
    nativePort: port,
    getCurrentPipeline() { return this.pipelineA; },
    synchronizeNativeAssetMembership() {},
    scheduleLatencyService() {}
  });
  port.owner = manager;
  const delta = (pluginId, descriptor, normalized) => ({
    pipeline: 'A',
    pluginId,
    pluginType: 'PhaserPlugin',
    parameterKey: descriptor.key,
    elementIndex: descriptor.element,
    normalized
  });

  manager.applyHostAutomationDeltas([
    delta(23, descriptors[0], 0.2),
    delta(24, descriptors[0], 0.4),
    delta(23, descriptors[1], 0.3)
  ]);

  assert.deepEqual(writes, [23, 24],
    'each plug-in in the packet is written once, however many of its lanes moved');
  for (const [index, descriptor] of descriptors.entries()) {
    const normalized = [0.2, 0.3][index];
    assert.equal(port.adoptedAutomationValues.get(`A:23:PhaserPlugin:${descriptor.key}:0`),
      context.normalizeDSPAutomationValue(descriptor, context.unpackDSPAutomationValue(
        descriptor, moved.parameters[descriptor.field])),
      'every lane of the batched write still adopts the value the plug-in retained');
    assert.ok(Math.abs(moved.parameters[descriptor.field] -
      context.denormalizeDSPAutomationValue(descriptor, normalized)) < 1e-9,
    'and every lane still reaches the plug-in');
  }
  assert.ok(Math.abs(other.parameters[descriptors[0].field] -
    context.denormalizeDSPAutomationValue(descriptors[0], 0.4)) < 1e-9);
});

test('host automation application suppresses the WebView update echo', () => {
  const helperStart = source.indexOf('const PLUGIN_UPDATE_FLUSH_DEADLINE_MS');
  const helperEnd = source.indexOf('\nclass NativePort', helperStart);
  const classBody = source.slice(source.indexOf('  applyHostAutomationDeltas('),
    source.indexOf('\n  async synchronizeNativeContext(', source.indexOf('  applyHostAutomationDeltas(')));
  const catalog = catalogExports();
  const [descriptor] = catalog.DSP_AUTOMATION_CATALOG.DCOffsetPlugin;
  assert.equal(descriptor.key, 'of');
  assert.equal(descriptor.publicName, 'offset');
  const context = { ...catalog };
  vm.runInNewContext(`${source.slice(helperStart, helperEnd)}\n` +
    `this.Manager = class {${classBody}\n};`, context);
  let updates = 0;
  const plugin = {
    id: 17,
    parameters: { of: 0 },
    getParameters() { return { ...this.parameters }; },
    setParameters(parameters) {
      this.parameters = parameters;
      updates += 1;
    }
  };
  const manager = new context.Manager();
  manager.pipelineA = [plugin];
  manager.pipelineB = [];
  manager.hostAutomationApplyDepth = 0;
  manager.nativePort = {
    adoptedAutomationValues: new Map(),
    deferredHostAutomationDeltas: new Map()
  };

  manager.applyHostAutomationDelta({
    pipeline: 'A', pluginId: 17, pluginType: 'DCOffsetPlugin',
    parameterKey: descriptor.key, elementIndex: 0, normalized: 0.75
  });
  assert.equal(updates, 1);
  assert.equal(plugin.parameters.of, 0.5);
  assert.equal(manager.hostAutomationApplyDepth, 0);
  assert.equal(manager.nativePort.adoptedAutomationValues.get(
    'A:17:DCOffsetPlugin:of:0'), 0.75);
});

test('host automation refreshes the plug-in UI controls once per frame', () => {
  const { context, port } = createNativePort();
  const classBody = source.slice(source.indexOf('  applyHostAutomationDeltas('),
    source.indexOf('\n  async synchronizeNativeContext(', source.indexOf('  applyHostAutomationDeltas(')));
  vm.runInNewContext(`this.Manager = class {${classBody}\n};`, context);
  const frames = [];
  context.requestAnimationFrame = callback => frames.push(callback);
  let syncs = 0;
  const plugin = {
    id: 17,
    type: 'DCOffsetPlugin',
    name: 'DC Offset',
    enabled: true,
    parameters: { of: 0 },
    getParameters() { return { ...this.parameters }; },
    setParameters(next) { this.parameters = { ...next }; },
    // The upstream write-back from the plug-in model to its DOM controls.
    syncUIControls() { syncs += 1; }
  };
  const manager = new context.Manager();
  Object.assign(manager, {
    currentPipeline: 'A',
    pipelineA: [plugin],
    pipelineB: [],
    hostAutomationApplyDepth: 0,
    nativePort: port,
    getCurrentPipeline() { return this.pipelineA; },
    synchronizeNativeAssetMembership() {},
    scheduleLatencyService() {}
  });
  port.owner = manager;

  // One automation take arriving at the telemetry rate.
  for (let index = 0; index < 4; ++index) {
    manager.applyHostAutomationDelta({
      pipeline: 'A', pluginId: 17, pluginType: 'DCOffsetPlugin',
      parameterKey: 'of', elementIndex: 0, normalized: 0.5 + index / 100
    });
  }

  assert.ok(Math.abs(plugin.parameters.of - 0.06) < 1e-9,
    'every replayed value still reaches the plug-in model');
  assert.equal(syncs, 0, 'the delta stream never redraws the controls inline');
  assert.equal(frames.length, 1, 'the whole burst is coalesced into a single frame');

  frames[0]();
  assert.equal(syncs, 1, 'one redraw carries the value the burst settled on');

  manager.applyHostAutomationDelta({
    pipeline: 'A', pluginId: 17, pluginType: 'DCOffsetPlugin',
    parameterKey: 'of', elementIndex: 0, normalized: 0.75
  });
  assert.equal(frames.length, 2, 'a later delta arms the next frame');
  frames[1]();
  assert.equal(syncs, 2);
});

test('applying a host value suppresses the upstream parameter history', () => {
  const { context, port } = createNativePort();
  const classBody = source.slice(source.indexOf('  applyHostAutomationDeltas('),
    source.indexOf('\n  async synchronizeNativeContext(', source.indexOf('  applyHostAutomationDeltas(')));
  vm.runInNewContext(`this.Manager = class {${classBody}\n};`, context);
  const suppressed = [];
  const plugin = {
    id: 17,
    type: 'DCOffsetPlugin',
    name: 'DC Offset',
    enabled: true,
    parameters: { of: 0 },
    getParameters() { return { ...this.parameters }; },
    // Mirrors the pipeline wrapper: an undo entry is saved every 500 ms of
    // parameter changes unless the plug-in carries the suppression flag.
    setParameters(next) {
      this.parameters = { ...next };
      suppressed.push(this._suppressParameterHistory === true);
    },
    setEnabled(enabled) {
      this.enabled = enabled;
      suppressed.push(this._suppressParameterHistory === true);
    }
  };
  const manager = new context.Manager();
  Object.assign(manager, {
    currentPipeline: 'A',
    pipelineA: [plugin],
    pipelineB: [],
    hostAutomationApplyDepth: 0,
    nativePort: port,
    getCurrentPipeline() { return this.pipelineA; },
    synchronizeNativeAssetMembership() {},
    scheduleLatencyService() {}
  });
  port.owner = manager;

  manager.applyHostAutomationDelta({
    pipeline: 'A', pluginId: 17, pluginType: 'DCOffsetPlugin',
    parameterKey: 'of', elementIndex: 0, normalized: 0.75
  });
  manager.applyHostAutomationDelta({
    pipeline: 'A', pluginId: 17, pluginType: 'DCOffsetPlugin',
    parameterKey: '__enabled', elementIndex: 0, normalized: 0
  });

  assert.deepEqual(suppressed, [true, true],
    'a replayed automation take never fills the undo history');
  assert.equal(plugin._suppressParameterHistory, false,
    'the suppression is released once the applied value settles');
});

// Mirrors the upstream pipeline parameter wrapper's undo bookkeeping: a change
// session opens with an immediate save, any pending trailing save is cleared
// unconditionally (the stale handle is left assigned, as upstream leaves it),
// and the trailing save is re-armed only while history is not suppressed.
function createParameterHistoryFixture() {
  const { context, port } = createNativePort();
  const classBody = source.slice(source.indexOf('  applyHostAutomationDeltas('),
    source.indexOf('\n  async synchronizeNativeContext(', source.indexOf('  applyHostAutomationDeltas(')));
  vm.runInNewContext(`this.Manager = class {${classBody}\n};`, context);
  const saves = [];
  const armed = new Set();
  let nextTimer = 0;
  const plugin = {
    id: 17,
    type: 'DCOffsetPlugin',
    name: 'DC Offset',
    enabled: true,
    parameters: { of: 0 },
    saveStateTimeout: null,
    paramChangeStarted: false,
    getParameters() { return { ...this.parameters }; },
    updateParameters() {
      const suppressHistory = this._suppressParameterHistory === true;
      if (!suppressHistory && !this.paramChangeStarted) {
        saves.push('immediate');
        this.paramChangeStarted = true;
      }
      if (this.saveStateTimeout) armed.delete(this.saveStateTimeout);
      if (!suppressHistory) {
        this.saveStateTimeout = ++nextTimer;
        armed.add(this.saveStateTimeout);
      }
    },
    setParameters(next) {
      this.parameters = { ...next };
      this.updateParameters();
    },
    setEnabled(enabled) {
      this.enabled = enabled;
      this.updateParameters();
    }
  };
  const manager = new context.Manager();
  Object.assign(manager, {
    currentPipeline: 'A',
    pipelineA: [plugin],
    pipelineB: [],
    hostAutomationApplyDepth: 0,
    nativePort: port,
    getCurrentPipeline() { return this.pipelineA; },
    synchronizeNativeAssetMembership() {},
    scheduleLatencyService() {}
  });
  port.owner = manager;
  return {
    manager,
    plugin,
    saves,
    // What the 500 ms timer would do -- but only if it was never cleared.
    flushTrailingSave() {
      if (!armed.has(plugin.saveStateTimeout)) return false;
      armed.delete(plugin.saveStateTimeout);
      plugin.saveStateTimeout = null;
      plugin.paramChangeStarted = false;
      saves.push('trailing');
      return true;
    }
  };
}

test('a host echo leaves the pending undo save of a user gesture armed', () => {
  const { manager, plugin, saves, flushTrailingSave } = createParameterHistoryFixture();

  // The user drags the control: the wrapper saves the opening state and arms the
  // trailing save that will record the value the drag settled on.
  plugin.setParameters({ of: 0.5 });
  assert.deepEqual(saves, ['immediate']);

  // Every gesture now draws a host delta back, and applying it runs suppressed.
  manager.applyHostAutomationDelta({
    pipeline: 'A', pluginId: 17, pluginType: 'DCOffsetPlugin',
    parameterKey: 'of', elementIndex: 0, normalized: 0.75
  });
  manager.applyHostAutomationDelta({
    pipeline: 'A', pluginId: 17, pluginType: 'DCOffsetPlugin',
    parameterKey: '__enabled', elementIndex: 0, normalized: 0
  });

  assert.equal(flushTrailingSave(), true,
    'the trailing save the drag armed survives the suppressed echoes');
  assert.deepEqual(saves, ['immediate', 'trailing'],
    'undo still holds the value the drag ended on');
});

test('replayed automation alone arms no undo save', () => {
  const { manager, saves, flushTrailingSave } = createParameterHistoryFixture();

  for (const normalized of [0.25, 0.5, 0.75]) {
    manager.applyHostAutomationDelta({
      pipeline: 'A', pluginId: 17, pluginType: 'DCOffsetPlugin',
      parameterKey: 'of', elementIndex: 0, normalized
    });
  }
  manager.applyHostAutomationDelta({
    pipeline: 'A', pluginId: 17, pluginType: 'DCOffsetPlugin',
    parameterKey: '__enabled', elementIndex: 0, normalized: 0
  });

  assert.equal(flushTrailingSave(), false, 'a replayed take arms no trailing save');
  assert.deepEqual(saves, [], 'a replayed take never fills the undo history');
});

test('applied log automation is canonical before an unrelated full update', async () => {
  const { context, hostCalls, port } = createNativePort();
  const classBody = source.slice(source.indexOf('  applyHostAutomationDeltas('),
    source.indexOf('\n  async synchronizeNativeContext(', source.indexOf('  applyHostAutomationDeltas(')));
  vm.runInNewContext(`this.Manager = class {${classBody}\n};`, context);
  const descriptors = context.DSP_AUTOMATION_CATALOG.PhaserPlugin;
  const descriptor = descriptors.find(candidate => candidate.key === 'rt');
  assert.equal(descriptor.normalization, 'log');
  const parameters = Object.fromEntries(descriptors.map(candidate =>
    [candidate.field, candidate.default]));
  parameters.st = 4;
  const plugin = {
    id: 23,
    type: 'PhaserPlugin',
    name: 'Phaser',
    enabled: true,
    parameters,
    getParameters() { return { ...this.parameters }; },
    setParameters(next) { this.parameters = { ...next }; }
  };
  const manager = new context.Manager();
  Object.assign(manager, {
    pipelineA: [],
    pipelineB: [],
    hostAutomationApplyDepth: 0,
    nativePort: port,
    synchronizeNativeAssetMembership() {},
    scheduleLatencyService() {}
  });
  port.owner = manager;

  const delta = {
    pipeline: 'A', pluginId: plugin.id, pluginType: plugin.type,
    parameterKey: descriptor.key, elementIndex: descriptor.element, normalized: 0.1
  };
  assert.equal(manager.applyHostAutomationDelta(delta), false);
  const identity = 'A:23:PhaserPlugin:rt:0';
  assert.equal(port.adoptedAutomationValues.get(identity), 0.1,
    'a deferred native value remains raw until a plug-in can apply it');

  manager.pipelineA = [plugin];
  manager.applyHostAutomationDelta(port.deferredHostAutomationDeltas.get(identity));
  const canonical = context.normalizeDSPAutomationValue(descriptor,
    context.unpackDSPAutomationValue(descriptor, plugin.parameters.rt));
  assert.equal(Object.is(canonical, 0.1), false,
    'the real logarithmic descriptor exercises a non-identical numeric round trip');
  assert.equal(port.adoptedAutomationValues.get(identity), canonical,
    'the adopted baseline follows the plain value actually retained by the plug-in');

  plugin.parameters.st = 6;
  await port.postMessage({ type: 'updatePlugin', plugin });
  await new Promise(resolve => setTimeout(resolve, 0));

  assert.equal(automationEditPayloads(hostCalls).length, 0,
    'an unrelated full update does not emit an edit for the untouched log parameter');
});

test('transformed storage automation round trips without echo or unrelated gestures', async () => {
  const { context, hostCalls, port } = createNativePort();
  const classBody = source.slice(source.indexOf('  applyHostAutomationDeltas('),
    source.indexOf('\n  async synchronizeNativeContext(', source.indexOf('  applyHostAutomationDeltas(')));
  vm.runInNewContext(`this.Manager = class {${classBody}\n};`, context);
  const cases = [
    { type: 'TiltEQPlugin', key: 'f0', initialPacked: 6.91, targetPacked: Math.log(400) },
    { type: 'DigitalErrorEmulatorPlugin', key: 'be', initialPacked: -6, targetPacked: -8 },
    { type: 'G726ADPCMSimulatorPlugin', key: 're', initialPacked: -6, targetPacked: -4 },
    { type: 'SimpleJitterPlugin', key: 'rj', initialPacked: 100, targetPacked: 60 }
  ];
  const plugins = cases.map(({ type, key, initialPacked }, index) => ({
    id: 40 + index,
    type,
    name: type,
    enabled: true,
    parameters: { [key]: initialPacked, unrelated: 0 },
    getParameters() { return { ...this.parameters }; },
    setParameters(parameters) { this.parameters = { ...parameters }; }
  }));
  const manager = new context.Manager();
  Object.assign(manager, {
    currentPipeline: 'A',
    pipelineA: plugins,
    pipelineB: [],
    hostAutomationApplyDepth: 0,
    nativePort: port,
    getCurrentPipeline() { return this.pipelineA; },
    synchronizeNativeAssetMembership() {},
    scheduleLatencyService() {}
  });
  port.owner = manager;

  for (let index = 0; index < cases.length; ++index) {
    const { type, key, initialPacked, targetPacked } = cases[index];
    const plugin = plugins[index];
    const descriptor = context.DSP_AUTOMATION_CATALOG[type]
      .find(candidate => candidate.key === key);
    assert.ok(descriptor, `${type}.${key} is present in the latest generated catalog`);
    const initialNormalized = context.normalizeDSPAutomationValue(
      descriptor, context.unpackDSPAutomationValue(descriptor, initialPacked));
    const initialRoundTrip = context.packDSPAutomationValue(descriptor,
      context.denormalizeDSPAutomationValue(descriptor, initialNormalized));
    assert.ok(Math.abs(initialRoundTrip - initialPacked) < 1e-12,
      `${type}.${key} converts storage -> public -> normalized -> public -> storage`);
    port.rememberAdoptedPlugin('A', plugin);
    const identity = `A:${plugin.id}:${type}:${key}:${descriptor.element}`;
    assert.equal(port.adoptedAutomationValues.get(identity), initialNormalized);

    const targetNormalized = context.normalizeDSPAutomationValue(
      descriptor, context.unpackDSPAutomationValue(descriptor, targetPacked));
    manager.applyHostAutomationDelta({
      pipeline: 'A', pluginId: plugin.id, pluginType: type,
      parameterKey: key, elementIndex: descriptor.element, normalized: targetNormalized
    });
    assert.ok(Math.abs(plugin.parameters[key] - targetPacked) < 1e-12,
      `${type}.${key} host normalized value is retained in packed plug-in storage`);
    assert.equal(port.adoptedAutomationValues.get(identity), targetNormalized);

    await port.postMessage({ type: 'updatePlugin', plugin });
    plugin.parameters.unrelated += 1;
    await port.postMessage({ type: 'updatePlugin', plugin });
  }

  await new Promise(resolve => setTimeout(resolve, 0));
  assert.equal(automationEditPayloads(hostCalls).length, 0,
    'host deltas and unrelated full updates do not echo transformed automation gestures');
});

test('deferred host diagnostics surface actionable messages and clear once healed', () => {
  const classBody = source.slice(source.indexOf('  applyHostAutomationDeltas('),
    source.indexOf('\n  async synchronizeNativeContext(', source.indexOf('  applyHostAutomationDeltas(')));
  const messages = [];
  const errorDisplay = { textContent: '' };
  let pending = null;
  let handles = 0;
  const context = {
    window: {
      uiManager: {
        errorDisplay,
        setError: message => { errorDisplay.textContent = message; messages.push(message); },
        clearError: () => { errorDisplay.textContent = ''; }
      }
    },
    HOST_DIAGNOSTIC_VISIBLE_MS: 3000,
    setTimeout: callback => { pending = callback; return ++handles; },
    clearTimeout: handle => { if (handle === handles) pending = null; },
    DSP_AUTOMATION_CATALOG: {},
    automationIdentity() {},
    denormalizeDSPAutomationValue() {},
    writeAutomationPlain() {}
  };
  vm.runInNewContext(`this.Manager = class {${classBody}\n};`, context);
  const manager = new context.Manager();
  manager.applyHostDiagnostics([{
    code: 'automation-capacity-exhausted',
    message: 'No automation slots remain.'
  }]);
  manager.applyHostDiagnostics([]);
  assert.deepEqual(messages, ['No automation slots remain.']);
  assert.equal(errorDisplay.textContent, 'No automation slots remain.');

  // A processing transaction that recovers cannot retract its own notice, so a
  // diagnostic that stops repeating has to release the status line itself.
  assert.ok(pending, 'a delivered diagnostic schedules its own dismissal');
  pending();
  assert.equal(errorDisplay.textContent, '');

  manager.applyHostDiagnostics([{ code: 'audio-processing-failure', message: 'Second notice.' }]);
  context.window.uiManager.setError('Configuring audio devices...');
  pending();
  assert.equal(errorDisplay.textContent, 'Configuring audio devices...',
    'the dismissal never discards a status message that replaced the diagnostic');
});

test('outer plugin update failure reconciles every optimistic automation edit', async () => {
  const { context, hostCalls, port } = createNativePort();
  const reconciled = [];
  const plugin = { id: 17, type: 'DCOffsetPlugin', parameters: { of: 0 } };
  port.owner.pipelineA = [plugin];
  port.owner.pipelineB = [];
  port.owner.hostAutomationApplyDepth = 0;
  port.owner.applyHostAutomationDelta = delta => reconciled.push(delta);
  port.rememberAdoptedPlugin('A', plugin);
  context.window.__effetuneHostCall = async (type, payload) => {
    hostCalls.push({ type, payload });
    throw new Error('rejected');
  };

  plugin.parameters.of = 0.5;
  await port.postMessage({ type: 'updatePlugin', plugin });

  assert.deepEqual(hostCalls.map(call => call.type), ['pipeline/updatePlugin']);
  assert.equal(reconciled.length, 1);
  assert.equal(reconciled[0].normalized, 0.5,
    'the host-adopted value replaces the rejected optimistic value');
});

// The counterpart of the failure above. A request that was answered at all --
// however the host answered the edit inside it -- reached the native side, so
// the value it carried is the plug-in's own and nothing is put back.
test('a host that will not record the gesture still leaves the user value in place',
  async () => {
    const { context, hostCalls, port } = createNativePort();
    const reconciled = [];
    const plugin = { id: 17, type: 'DCOffsetPlugin', parameters: { of: 0 } };
    port.owner.pipelineA = [plugin];
    port.owner.pipelineB = [];
    port.owner.hostAutomationApplyDepth = 0;
    port.owner.applyHostAutomationDelta = delta => reconciled.push(delta);
    port.rememberAdoptedPlugin('A', plugin);
    context.window.__effetuneHostCall = async (type, payload) => {
      hostCalls.push({ type, payload });
      return { ok: true };
    };

    plugin.parameters.of = 0.5;
    await port.postMessage({ type: 'updatePlugin', plugin });
    await new Promise(resolve => setTimeout(resolve, 0));

    assert.deepEqual(hostCalls.map(call => call.type), ['pipeline/updatePlugin']);
    assert.equal(automationEditPayloads(hostCalls).length, 1);
    assert.deepEqual(reconciled, [], 'nothing is put back');
    assert.equal(port.adoptedAutomationValues.get('A:17:DCOffsetPlugin:of:0'), 0.75,
      'and the value the user asked for is the adopted authority');
  });

test('outer pipeline rebuild failure reconciles optimistic automation edits', async () => {
  const { context, hostCalls, port } = createNativePort();
  const reconciled = [];
  const plugin = { id: 17, type: 'DCOffsetPlugin', parameters: { of: 0 } };
  port.owner.currentPipeline = 'A';
  port.owner.pipelineA = [plugin];
  port.owner.pipelineB = [];
  port.owner.applyHostAutomationDelta = delta => reconciled.push(delta);
  port.rememberAdoptedPlugin('A', plugin);
  context.window.__effetuneHostCall = async (type, payload) => {
    hostCalls.push({ type, payload });
    throw new Error('rejected');
  };

  plugin.parameters.of = 0.5;
  await port.postMessage({ type: 'updatePlugins', plugins: [plugin] });

  assert.deepEqual(hostCalls.map(call => call.type), ['pipeline/rebuild']);
  assert.equal(reconciled.length, 1);
  assert.equal(reconciled[0].normalized, 0.5);
});

test('successful pipeline rebuild adopts only the native authoritative snapshot', async () => {
  const { context, port } = createNativePort();
  const plugin = { id: 17, type: 'DCOffsetPlugin', parameters: { of: 0 } };
  port.owner.pipelineA = [plugin];
  port.rememberAdoptedPlugin('A', plugin);
  plugin.parameters.of = 0.5;
  port.owner.applyHostAutomationDeltas = deltas => {
    for (const delta of deltas || []) {
      port.adoptedAutomationValues.set('A:17:DCOffsetPlugin:of:0', delta.normalized);
    }
  };
  context.window.__effetuneHostCall = async () => ({
    ok: true,
    automationDeltas: [{
      pipeline: 'A', pluginId: 17, pluginType: 'DCOffsetPlugin',
      parameterKey: 'of', elementIndex: 0, normalized: 0.6
    }]
  });

  await port.postMessage({ type: 'updatePlugins', plugins: [plugin] });

  assert.equal(port.adoptedAutomationValues.get('A:17:DCOffsetPlugin:of:0'), 0.6,
    'the sent stale full image is not recorded as host-adopted after success');
});

test('a preset load names the bound targets it means to move', async () => {
  const { hostCalls, port } = createNativePort();
  const moved = { id: 17, type: 'DCOffsetPlugin', enabled: true, parameters: { of: 0 } };
  const untouched = { id: 18, type: 'DCOffsetPlugin', enabled: true, parameters: { of: 0.5 } };
  port.owner.pipelineA = [moved, untouched];
  port.rememberAdoptedPlugin('A', moved);
  port.rememberAdoptedPlugin('A', untouched);
  moved.parameters.of = 0.5;

  await port.postMessage({ type: 'updatePlugins', plugins: [moved, untouched] });

  const rebuild = hostCalls.find(call => call.type === 'pipeline/rebuild');
  assert.ok(rebuild, 'the preset load reaches the native side');
  assert.deepEqual(Array.from(rebuild.payload.automationEdits, edit => ({ ...edit })), [{
    pipeline: 'A',
    pluginId: 17,
    pluginType: 'DCOffsetPlugin',
    parameterKey: 'of',
    elementIndex: 0,
    normalized: 0.75,
    // A preset load is nobody's drag, so each target it names is a complete
    // touch of its own.
    beginGesture: true,
    endGesture: true
  }], 'only the target whose value actually moved travels with the bulk image');
  assert.deepEqual(hostCalls.map(call => call.type), ['pipeline/rebuild'],
    'a bulk image carries its gestures instead of sending them one at a time');
});

test('a history restore names the bound targets it means to move', async () => {
  const { context, hostCalls, port } = createNativePort();
  const historyStart = source.indexOf('  synchronizeHistoryState()');
  const historyEnd = source.indexOf('\n  synchronizeNativeAssetMembership', historyStart);
  assert.notEqual(historyStart, -1);
  vm.runInNewContext(`this.Manager = class {${source.slice(historyStart, historyEnd)}\n};`,
    context);
  const inactive = { id: 21, type: 'DCOffsetPlugin', enabled: true, parameters: { of: 0 } };
  const manager = new context.Manager();
  Object.assign(manager, {
    currentPipeline: 'A',
    pipelineA: [],
    pipelineB: [inactive],
    nativePort: port,
    serializePipeline: pipeline => pipeline,
    seedRestoredAutomationBaseline() {},
    applyHostAutomationDeltas() {},
    synchronizeNativeAssetMembership() {},
    scheduleLatencyService() {},
    unsupportedWarningShown: false
  });
  port.owner = manager;
  port.rememberAdoptedPlugin('B', inactive);
  inactive.parameters.of = -0.5;

  await manager.synchronizeHistoryState();

  const restore = hostCalls.find(call => call.type === 'pipeline/restoreHistory');
  assert.ok(restore, 'the history restore reaches the native side');
  assert.deepEqual(Array.from(restore.payload.automationEdits, edit => ({ ...edit })), [{
    pipeline: 'B',
    pluginId: 21,
    pluginType: 'DCOffsetPlugin',
    parameterKey: 'of',
    elementIndex: 0,
    normalized: 0.25,
    beginGesture: true,
    endGesture: true
  }], 'an undo names the restored target on the pipeline that actually holds it');
});

test('successful history restore applies its native authoritative snapshot', async () => {
  const historyStart = source.indexOf('  synchronizeHistoryState()');
  const historyEnd = source.indexOf('\n  synchronizeNativeAssetMembership', historyStart);
  assert.notEqual(historyStart, -1);
  assert.notEqual(historyEnd, -1);
  const context = {
    window: {
      __effetuneHostCall: async () => ({
        ok: true,
        automationDeltas: [{ normalized: 0.625 }]
      })
    }
  };
  vm.runInNewContext(
    `this.synchronizeHistoryState = ({${source.slice(historyStart, historyEnd)}})` +
      '.synchronizeHistoryState;', context);
  const applied = [];
  const manager = {
    pipelineA: [{ id: 17 }],
    pipelineB: null,
    currentPipeline: 'A',
    nativePort: {
      flushPluginUpdates: () => applied.push('flush'),
      collectAutomationEdits: () => []
    },
    serializePipeline: pipeline => pipeline,
    seedRestoredAutomationBaseline: () => applied.push('seed'),
    applyHostAutomationDeltas: deltas => applied.push(...deltas.map(delta => delta.normalized)),
    synchronizeNativeAssetMembership() {},
    scheduleLatencyService() {},
    unsupportedWarningShown: false
  };

  const result = await context.synchronizeHistoryState.call(manager);

  assert.equal(result.ok, true);
  assert.deepEqual(applied, ['flush', 'seed', 0.625],
    'a coalesced image reaches the native side before the restore that replaces it, ' +
    'and the restored UI image reseeds the baseline before the native snapshot overrides it');
});

// A restore the native side refuses -- the engine rebuild behind it failed, or
// the latency synchronization did, and the bootstrap turns an ok !== true answer
// into a throw -- never applied anything. Collecting the edits has already
// advanced the adopted baseline to the restored values, and the baseline is the
// only record of what the native side holds, so leaving it there is permanent:
// a later user edit back to a restored value is compared against a baseline
// that already carries it, read as agreement, and never emitted. Nothing
// afterwards can notice, and the editor, the DSP and the saved project stay
// apart for the rest of the session. The two other routes that publish a
// pipeline image return the baseline on failure; this one has to as well.
test('a refused history restore leaves the baseline on the value the native side holds',
  async () => {
    const { context, hostCalls, port } = createNativePort();
    const historyStart = source.indexOf('  synchronizeHistoryState()');
    const historyEnd = source.indexOf('\n  synchronizeNativeAssetMembership', historyStart);
    const findStart = source.indexOf('  findPipelinePlugin(pipeline, pluginId)');
    const findEnd = source.indexOf('\n  applyHostDiagnostics(', findStart);
    const applyStart = source.indexOf('  applyHostAutomationDelta(delta)');
    const applyEnd = source.indexOf('\n  async synchronizeNativeContext(', applyStart);
    assert.notEqual(historyStart, -1, 'synchronizeHistoryState is missing');
    assert.notEqual(findStart, -1, 'findPipelinePlugin is missing');
    assert.notEqual(applyStart, -1, 'applyHostAutomationDelta is missing');
    vm.runInNewContext(`this.Manager = class {${source.slice(historyStart, historyEnd)}\n` +
      `${source.slice(findStart, findEnd)}\n${source.slice(applyStart, applyEnd)}\n};`,
    context);

    const plugin = {
      id: 17,
      type: 'DCOffsetPlugin',
      name: 'DC Offset',
      enabled: true,
      parameters: { of: 0 },
      getParameters() { return { ...this.parameters }; },
      setParameters(next) { this.parameters = { ...next }; }
    };
    const manager = new context.Manager();
    Object.assign(manager, {
      currentPipeline: 'A',
      pipelineA: [plugin],
      pipelineB: null,
      hostAutomationApplyDepth: 0,
      nativePort: port,
      getCurrentPipeline() { return this.pipelineA; },
      serializePipeline: pipeline => pipeline,
      seedRestoredAutomationBaseline() {},
      applyHostAutomationDeltas() {},
      synchronizeNativeAssetMembership() {},
      scheduleLatencyService() {},
      unsupportedWarningShown: false
    });
    port.owner = manager;
    port.rememberAdoptedPlugin('A', plugin);
    const identity = 'A:17:DCOffsetPlugin:of:0';
    assert.equal(port.adoptedAutomationValues.get(identity), 0.5,
      'the native side and the editor start on the same value');

    context.window.__effetuneHostCall = async (type, payload) => {
      hostCalls.push({ type, payload });
      if (type === 'pipeline/restoreHistory') {
        throw new Error('Unable to prepare the master-bypass delay');
      }
      return { ok: true };
    };

    // Undo moves the knob in the editor and is then refused by the native side.
    plugin.parameters.of = 0.5;
    const refused = await manager.synchronizeHistoryState();

    assert.equal(refused.ok, false, 'the refusal is reported to the caller');
    assert.equal(port.adoptedAutomationValues.get(identity), 0.5,
      'the baseline stays on the value the native side still holds');
    assert.equal(plugin.parameters.of, 0,
      'and the editor is returned to it, exactly as a refused rebuild does');

    // The user now moves the knob to the value the refused undo asked for. It
    // differs from what the native side holds, so it has to be emitted.
    plugin.parameters.of = 0.5;
    void port.postMessage({ type: 'updatePlugin', plugin });
    await new Promise(resolve => setTimeout(resolve, 0));

    assert.deepEqual(automationEditPayloads(hostCalls).map(edit => edit.normalized), [0.75],
      'a later edit to the value the refused restore wanted still reaches the native side');
    assert.equal(port.adoptedAutomationValues.get(identity), 0.75,
      'and advances the baseline to it');
  });

test('undo of an unbound toggle does not automate an untouched enable state', async () => {
  const { context, hostCalls, port } = createNativePort();
  const historyStart = source.indexOf('  synchronizeHistoryState()');
  const historyEnd = source.indexOf('\n  synchronizeNativeAssetMembership', historyStart);
  const managerStart = source.indexOf('  seedRestoredAutomationBaseline()');
  const managerEnd = source.indexOf('\n  async synchronizeNativeContext(', managerStart);
  assert.notEqual(historyStart, -1);
  assert.notEqual(managerStart, -1);
  vm.runInNewContext(`this.Manager = class {${source.slice(historyStart, historyEnd)}\n` +
    `${source.slice(managerStart, managerEnd)}\n};`, context);
  const plugin = {
    id: 17,
    type: 'DCOffsetPlugin',
    name: 'DC Offset',
    enabled: true,
    parameters: { of: 0 },
    getParameters() { return { ...this.parameters }; },
    setParameters(next) { this.parameters = { ...next }; },
    setEnabled(enabled) { this.enabled = enabled; }
  };
  const manager = new context.Manager();
  Object.assign(manager, {
    currentPipeline: 'A',
    pipelineA: [plugin],
    pipelineB: null,
    hostAutomationApplyDepth: 0,
    nativePort: port,
    audioContext: { sampleRate: 48000 },
    getCurrentPipeline() { return this.pipelineA; },
    serializePipeline: pipeline => pipeline,
    synchronizeNativeAssetMembership() {},
    scheduleLatencyService() {},
    unsupportedWarningShown: false
  });
  port.owner = manager;

  // The user turned the effect off while the host refused a lane, so the sent
  // value became the adopted baseline without any automation slot behind it.
  port.rememberAdoptedPlugin('A', plugin);
  plugin.enabled = false;
  port.adoptedAutomationValues.set('A:17:DCOffsetPlugin:__enabled:0', 0);

  // Undo recreates the plug-in with its id preserved and the toggle back on.
  plugin.enabled = true;
  await manager.synchronizeHistoryState();

  plugin.parameters.of = 0.5;
  await port.postMessage({ type: 'updatePlugin', plugin });
  await new Promise(resolve => setTimeout(resolve, 0));

  assert.deepEqual(automationEditPayloads(hostCalls).map(edit => edit.parameterKey), ['of'],
    'only the knob the user actually moved claims an automation slot after undo');
});

test('a restored baseline resolves parameters at the change-detection rate', async () => {
  const { context, hostCalls, port } = createNativePort();
  const managerStart = source.indexOf('  seedRestoredAutomationBaseline()');
  const managerEnd = source.indexOf('\n  async synchronizeNativeContext(', managerStart);
  vm.runInNewContext(`this.Manager = class {${source.slice(managerStart, managerEnd)}\n};` +
    `this.rebuildPipeline = ({${source.slice(rebuildStart, rebuildEnd)}}).rebuildPipeline;`,
  context);
  const descriptor = context.DSP_AUTOMATION_CATALOG.RoomEqPlugin
    .find(candidate => candidate.key === 'dy');
  assert.ok(descriptor, 'the sample-rate derived channel delay is an automation target');
  // Mirrors RoomEqPlugin: dy is derived from the resolved rate, and only an
  // explicit commit adopts a new one.
  const plugin = {
    id: 31,
    type: 'RoomEqPlugin',
    name: 'Room EQ',
    enabled: true,
    delayMs: 10,
    gn: 0,
    _sampleRate: 96000,
    getParameters(options = {}) {
      const sampleRate = Number.isFinite(options.sampleRate) && options.sampleRate > 0
        ? options.sampleRate
        : this._sampleRate;
      if (options.commitSampleRate) this._sampleRate = sampleRate;
      return { dy: Math.round(this.delayMs * sampleRate / 1000), gn: this.gn };
    },
    getWorkletPluginData(parameters) {
      return { id: this.id, type: this.type, enabled: this.enabled, parameters };
    },
    setParameters() {},
    setEnabled(enabled) { this.enabled = enabled; }
  };
  const manager = new context.Manager();
  Object.assign(manager, {
    currentPipeline: 'A',
    pipelineA: [plugin],
    pipelineB: null,
    hostAutomationApplyDepth: 0,
    nativePort: port,
    audioContext: { sampleRate: 48000 },
    preserveReadyNativePipelineDuringStartup: true,
    getCurrentPipeline() { return this.pipelineA; },
    synchronizeNativeAssetMembership() {},
    scheduleLatencyService() {},
    dispatchEvent() {}
  });
  port.owner = manager;

  // Editor restore seeds the baseline, then the first full publish resolves and
  // commits the engine rate.
  await context.rebuildPipeline.call(manager);
  context.window.app.initialized = true;
  await context.rebuildPipeline.call(manager);
  await new Promise(resolve => setTimeout(resolve, 0));

  // The user now moves one unrelated control.
  plugin.gn = 3;
  await port.postMessage({
    type: 'updatePlugin',
    plugin: plugin.getWorkletPluginData(plugin.getParameters())
  });
  await new Promise(resolve => setTimeout(resolve, 0));

  assert.deepEqual(automationEditPayloads(hostCalls).map(edit => edit.parameterKey), ['gn'],
    'a rate-derived field seeded at the engine rate never becomes a gesture of its own');
});

test('a sample rate change reseeds the baseline for rate-derived targets', async () => {
  const { context, hostCalls, port } = createNativePort();
  const managerStart = source.indexOf('  seedRestoredAutomationBaseline()');
  const managerEnd = source.indexOf('\n  async synchronizeNativeContext(', managerStart);
  const syncStart = source.indexOf('  async synchronizeNativeContext(');
  const syncEnd = source.indexOf('\n  scheduleLatencyService()', syncStart);
  assert.notEqual(syncStart, -1, 'AudioManager.synchronizeNativeContext is missing');
  assert.notEqual(syncEnd, -1, 'AudioManager.synchronizeNativeContext boundary is missing');
  vm.runInNewContext(`this.Manager = class {${source.slice(managerStart, managerEnd)}\n` +
    `${source.slice(rebuildStart, rebuildEnd)}\n` +
    `${source.slice(syncStart, syncEnd)}\n};`, context);
  const descriptor = context.DSP_AUTOMATION_CATALOG.RoomEqPlugin
    .find(candidate => candidate.key === 'dy');
  assert.ok(descriptor, 'the sample-rate derived channel delay is an automation target');
  // Mirrors RoomEqPlugin: dy is a sample count derived from the resolved rate,
  // and only an explicit commit adopts a new one.
  const plugin = {
    id: 31,
    type: 'RoomEqPlugin',
    name: 'Room EQ',
    enabled: true,
    delayMs: 10,
    gn: 0,
    _sampleRate: 96000,
    getParameters(options = {}) {
      const sampleRate = Number.isFinite(options.sampleRate) && options.sampleRate > 0
        ? options.sampleRate
        : this._sampleRate;
      if (options.commitSampleRate) this._sampleRate = sampleRate;
      return { dy: Math.round(this.delayMs * sampleRate / 1000), gn: this.gn };
    },
    getWorkletPluginData(parameters) {
      return { id: this.id, type: this.type, enabled: this.enabled, parameters };
    },
    setParameters() {},
    setEnabled(enabled) { this.enabled = enabled; }
  };
  const manager = new context.Manager();
  Object.assign(manager, {
    currentPipeline: 'A',
    pipelineA: [plugin],
    pipelineB: null,
    hostAutomationApplyDepth: 0,
    nativePort: port,
    nativeContextGeneration: 1,
    nativeContextSync: null,
    audioContext: { sampleRate: 96000, destination: { channelCount: 2, maxChannelCount: 2 } },
    preserveReadyNativePipelineDuringStartup: false,
    getCurrentPipeline() { return this.pipelineA; },
    synchronizeNativeAssetMembership() {},
    scheduleLatencyService() {},
    dispatchEvent() {}
  });
  port.owner = manager;
  context.window.app.initialized = true;

  await manager.rebuildPipeline();
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.equal(port.adoptedAutomationValues.get('A:31:RoomEqPlugin:dy:0'),
    context.normalizeDSPAutomationValue(descriptor,
      context.unpackDSPAutomationValue(descriptor, 960)),
    'the first publish seeds the delay resolved at the original engine rate');

  // The host switches the engine to 48 kHz, which halves the derived sample count.
  await manager.synchronizeNativeContext({
    engineSampleRate: 48000, channels: 2, contextGeneration: 2
  });
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.equal(plugin._sampleRate, 48000, 'the rebuild committed the new engine rate');

  const rebuilds = hostCalls.filter(call => call.type === 'pipeline/rebuild');
  assert.equal(rebuilds.length, 2, 'the rate change republishes the pipeline');
  assert.deepEqual(Array.from(rebuilds[1].payload.automationEdits,
    edit => edit.parameterKey), [],
  'a rate change only re-expresses values the user never touched, so it names no ' +
    'gesture and forces nothing into a bound slot');

  // The user now moves one unrelated control.
  plugin.gn = 3;
  await port.postMessage({
    type: 'updatePlugin',
    plugin: plugin.getWorkletPluginData(plugin.getParameters())
  });
  await new Promise(resolve => setTimeout(resolve, 0));

  assert.deepEqual(automationEditPayloads(hostCalls).map(edit => edit.parameterKey), ['gn'],
    'a target recomputed by the rate change never claims an automation slot');
});

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
    seedRestoredAutomationBaseline() {},
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
    seedRestoredAutomationBaseline() {},
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
