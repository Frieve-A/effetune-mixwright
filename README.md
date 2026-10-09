# EffeTune Mixwright

EffeTune Mixwright brings EffeTune's C++ DSP engine and desktop interface to the VST® 3 format. Windows x64 is the primary target, with macOS arm64 and x86_64 as secondary targets. The plug-in handles one to eight audio channels and supports five-bus processing pipelines, a WebView interface, A/B state management, telemetry, and 1×, 2×, 4×, and 8× oversampling.

## Requirements

- Git (Windows users must run `git config --global core.longpaths true`)
- CMake 3.24 or later, Ninja, Node.js 22 or later, and Python 3.10 or later
- Windows: Visual Studio 2022 Build Tools with the **Desktop development with C++** workload, plus the WebView2 Runtime
- macOS: A current version of Xcode and its Command Line Tools

Release builds for Windows enable AVX2 and FMA in the resampler by default. On systems without AVX2 support, configure the project with `-DEFFETUNE_ENABLE_AVX2=OFF` to select the SSE2 implementation instead.

## Clone and Build

```sh
git clone --recursive <repository-url> effetune-vst
cd effetune-vst
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

On Windows, run these commands from the x64 Native Tools Command Prompt for VS 2022. On macOS, replace the preset name with `mac-release`.

The main build artifacts are:

- VST3: `build/windows-release/VST3/Release/EffeTune Mixwright.vst3`
- Headless host: `build/windows-release/src/tools/effetune-headless.exe`
- WebView assets: `build/windows-release/webview-assets`

## Windows Editor Startup

WebView resources retain extended path prefixes and use native path separators,
including when the host loads the module through a `\\?\` path.
WebView2 creation failures show `EFFETUNE-UI-RUNTIME` with installation or repair
guidance. `EFFETUNE-UI-TIMEOUT` indicates an operation that did not finish within
the editor's startup deadline; it does not imply that a browser process started.
Detailed creation errors go to the Windows debugger output.

Windows configuration generates a narrowly adapted CHOC WebView header with
creation-error callbacks using [tools/build-choc-webview-header.mjs](tools/build-choc-webview-header.mjs).
The pinned CHOC source, license, and version metadata remain intact. The generator
rejects unexpected changes to the construction code when the dependency is updated.

## Visualizer and Saved Data

Use the **Visualizer** header button to view and edit analyzer layouts fed by the
native pipeline output. The **Effect Pipeline** button returns to effect editing.
Visualizer presets and imported images use the VST WebView's local storage.
The music player and music library remain outside the VST interface.
Upstream's LAN remote control is not available in this VST build.

Use **Settings → Configuration → Controllers** to open controller mapping settings.
Controller mappings can toggle master bypass, switch A/B, and step through saved
presets. Player transport controls are omitted from the mapping choices.

**Configuration → General** includes the upstream language and theme choices.
It also selects sine or bandpass noise for frequency audition and the SFZ bank
size limit (64, 128, 256, 512, or 1024 MiB; 256 MiB by default).
**Startup** selects the Effect Pipeline or Visualizer view when the editor opens;
the pipeline itself continues to come from the host's plug-in state.
**Display** selects instantaneous or Peak Hold spectra for per-effect Spectrum
Overlays. These preferences are saved in the Mixwright UI configuration.

**Settings → Backup / Restore** transfers saved pipeline, effect and Visualizer
presets, impulse responses and measurements in an `.effetune_backup` archive.
Choose a destination in the native save dialog; restoration previews the selected
items before writing them. Archive creation is limited to 256 MB. Native export
stages the archive and replaces the chosen file only after all chunks arrive.

The pinned upstream uses Normal PCM transport for per-effect Spectrum Overlays.
When built against upstream's native Spectrum Tap API, Mixwright uses the shared
Normal/HQ analyzer in After/Compare modes and exposes the HQ quality setting.
Availability depends on the analyzer accepting the engine's oversampled rate;
unsupported capture rates disable spectrum analysis without interrupting audio.
The bridge also publishes capture generations, positions, analysis window age and
remaining tap delay. Visual synchronization is not enabled yet: mapping these
positions to the host output clock, applying additional audio output delay and
reporting that applied delay to the host are still required. Visualizer analyzer
sources have their own analysis settings.

SFZ Note Player reads instruments and samples directly from the selected folder.
Choose **Select SFZ Folder…**, then select an SFZ file when the folder contains
several instruments. Folder references persist locally; the source files must
remain available, and moving or deleting them makes the instrument unavailable
until you select it again. **Remove** removes the local library reference and
leaves the source files in place. Projects store local SFZ identifiers, so choose
the instrument again when opening a project on another machine. The configured limit applies to each bank's preparation and
decoded sample data, with a native SFZ asset budget of 1 GiB. Impulse-response
assets retain their separate 32 MiB per-asset and 128 MiB total budgets.

When an active Notes analyzer and SFZ Note Player read the same input with matching
note ranges, Mixwright reuses their note analysis where routing and timing allow it.
Changing the input, note range or an intervening audio effect restores independent
SFZ analysis.

## Room EQ Measurements

Electron and VST WebViews intentionally keep separate browser-storage profiles and origins.
To reuse a measurement, export it as JSON from EffeTune, open Room EQ in EffeTune Mixwright,
and choose **Import...** beside the measurement list. The imported copy receives a new local
identifier and remains available to other Room EQ instances in the same VST WebView profile.
Select an imported measurement and choose **Delete** to remove that local copy and its stored
impulse responses after confirmation. Room EQ instances that reference the selected copy switch
to **No measurement** and aligned bypass before the local data is removed.

Enable **Include impulse responses in measurement JSON exports** before exporting when Room EQ's
phase-correction mode needs impulse-response data. Minimum- and linear-phase magnitude correction
can use exports without impulse responses. Import is limited to explicitly selected JSON files of
at most 128 MB.

## Headless Processing

```sh
build/windows-release/src/tools/effetune-headless.exe \
  --input input.wav --output output.wav --gain-db -6
```

The headless host processes WAV files through a fixed Volume pipeline backed by EffeTune's native DSP engine. Audio is processed in blocks of at most 128 frames. Run the executable with `--help` to list all available options.

## Verification

The standard `ctest` suite exercises the upstream DSP, the VST wrapper, state and bridge handling, resampler behavior, UI assets, WebView loading, and the Steinberg Validator. To compare every golden test vector against both the JavaScript and native DSP implementations, run:

```sh
node external/effetune/tools/dsp-parity/run.mjs --native \
  --native-runner ../../build/windows-release/external/effetune/dsp/effetune-dsp-parity-runner.exe
```

The relative path passed to `--native-runner` is resolved from `external/effetune`.

To check unpublished upstream changes without editing the submodule, configure a
separate local build with an explicit source directory:

```sh
cmake --preset windows-release -B build/spectrum-preview \
  -DEFFETUNE_UPSTREAM_SOURCE_DIR=D:/program/proto/effetune
cmake --build build/spectrum-preview --parallel 2
ctest --test-dir build/spectrum-preview --output-on-failure --parallel 2 --timeout 1800
```

The override supplies both shared DSP and WebView sources. Shipping presets still
default to the pinned submodule. Native Spectrum Tap builds can additionally run
`node D:/program/proto/effetune/tools/verify-spectrum-tap.mjs build/spectrum-preview/external/effetune/dsp/effetune_dsp_spectrum_tap_tests.exe`
to verify Normal/HQ parity with the browser.

Latency-changing parameter and asset updates are serviced while audio callbacks continue,
whether transport is playing or stopped. The audio owner captures the current pipeline;
the control service prepares compensation and bypass storage; a later audio-block boundary
applies the matching wet plan, bypass delay, and host latency together. Stale preparations
are recaptured, and allocation failures retain the applied plan with a deferred diagnostic
and bounded retry. Retired storage is reclaimed only by the control service. Host latency
notifications remain debounced and describe applied, not merely prepared, compensation.
Retiming preserves available recent delay history, but does not guarantee click-free
changes or recover samples older than the previous delay capacity.

To benchmark the resampler in a Release build, run `build/windows-release/src/tools/effetune-resampler-bench.exe`.

pluginval at strictness level 10, compatibility testing in target DAWs, testing on physical macOS hardware, code signing, and notarization are separate release-QA steps.

## License

Every distribution must include [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).

The notices include JSZip's bundled lie, immediate, setImmediate, and pako
components, including the zlib-derived source notices. The bundle and the
interface's third-party notices viewer contain the same complete text.

VST is a registered trademark of Steinberg Media Technologies GmbH.
