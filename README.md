# EffeTune Mixwright

EffeTune Mixwright brings EffeTune's C++ DSP engine and desktop interface to the VST® 3 format. Windows x64 is the primary target, with macOS arm64 and x86_64 as secondary targets. The plug-in handles one to eight audio channels and supports five-bus processing pipelines, a WebView interface, A/B state management, telemetry, and 1×, 2×, 4×, and 8× oversampling.

## Requirements

- Git (Windows users must run `git config --global core.longpaths true`)
- CMake 3.24 or later, Ninja, and Node.js 22 or later
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

VST is a registered trademark of Steinberg Media Technologies GmbH.
