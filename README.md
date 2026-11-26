# HertzyEVS

Frequency and rotation-speed workbench for the LUCID Triton2 EVS event camera (Sony IMX636), HvA Maintenance Lab.
It records, browses and analyses on the same machine (PC or Jetson): what vibrates at which frequency, and how fast
something turns.

## Download
**[Download HertzyEVS Demo for Windows (zip)](https://github.com/leetjes123/Hertzy-EVS/releases/latest/download/HertzyEVS-Demo-win64.zip)**
— Windows 10/11, 64-bit, nothing to install or build:

1. Unzip the whole folder (do not run it from inside the zip).
2. Double-click `HertzyEVS Demo.exe`. If SmartScreen warns (the app is not signed): *More info* > *Run anyway*.

The demo has every page, with a simulated lab bench (ringing bars + a fan with a tape mark) instead of a camera: draw a
small ROI on the fan blades on the Live page and switch on Rotation speed. Older versions: the
[Releases](https://github.com/leetjes123/Hertzy-EVS/releases) page.

The camera edition for the Triton2 EVS needs the LUCID Arena SDK runtime and is not published here: build it yourself
(see Build) with the Arena SDK installed.

Building the zips: `tools\ship_hertzy.ps1` (see Build).

## Pages
- **Library**: projects (folders) of recordings with thumbnails, stats, descriptions and saved measurements; batch a
  recipe over many clips.
- **Live**: camera view, ROIs with a rolling spectrum and tone, **live RPM** (big readout + speed over time),
  recording (now / armed on an event-rate jump), triggered auto-measure, focus chart, reference overlay, web view (:8080).
- **Analyse**: one clip: activity / ON-OFF / frequency / energy maps, ROIs (drawn or detected), spectra, tones,
  decay, spectrogram, **RPM per ROI + RPM over time**, playback with slow motion and **strobe at the measured speed**,
  named measurements (`<clip>.measure/<name>/`: JSON, CSV, PNG).
- **Camera**: biases, hardware filters, presets, node browser.

## Measuring RPM
1. Draw a small ROI where the moving parts pass by: blade tips, gear teeth or a strip of tape. Keep it off the hub.
2. Rotation speed → *Measure RPM*. Set what passes:
   - **Tape / 1 mark**: one mark per turn (tape, a bolt head, a key way).
   - **Blades / teeth**: N alike marks per turn; the speed is the pass rate / N.
3. Set the expected range (narrower = safer) and the window (Live: longer = more precise, slower to follow).

How it works (`src/rpm.cpp`): the ROI's ON−OFF signal is periodic. The estimator takes the strongest spectral line,
walks down to the lowest frequency f0 that is itself a real line with its harmonics present (so it does not lock onto
2× or 3× the speed), and refines f0 from all its harmonics. With N marks, f0 is the pass rate. The exception is when
every N-th harmonic clearly stands out: then the marks are not alike (imbalance, one odd blade), and f0 is the turn rate
itself. **A bright tape mark that dominates the ROI repeats once per turn: use Marks = 1 for it.**
Precision: about 0.1 % with a 1 s window on the simulator; it is better with longer windows and more harmonics.

*Strobe at this speed* (Analyse → Rotation speed) starts playback with one picture per pass. The rotating part then
stands still and turns slowly with the drift, which lets you check by eye that the number is right.

## Build (Windows)
Qt 6.8.3 (`python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O %USERPROFILE%/Qt`), Visual Studio 2022,
Arena SDK (optional: without it, simulator / viewer only).

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64             # dev edition: simulator + tests
    cmake --build build --config Release --parallel
    build\Release\hertzyevs.exe --sim

Editions (`-DHZ_EDITION=`): `dev` (simulator, tests, scripted screenshots), `full` (camera only), `viewer` (Library +
Analyse, no Arena). Portable zips (`-Clips a.raw,b.raw -Project P` adds recordings):

    cmake -S . -B build-demo -DHZ_EDITION=dev "-DARENA_SDK_DIR="
    cmake --build build-demo --config Release --target hertzyevs hertzyevs_launcher
    cmake -S . -B build-full -DHZ_EDITION=full
    cmake --build build-full --config Release --target hertzyevs hertzyevs_launcher
    powershell -ExecutionPolicy Bypass -File tools\ship_hertzy.ps1 -Edition demo -Zip dist\HertzyEVS-Demo-win64.zip
    powershell -ExecutionPolicy Bypass -File tools\ship_hertzy.ps1 -Edition full -Zip dist\HertzyEVS-win64.zip

Third-party licences: `THIRD_PARTY.md`.
Jetson: see `JETSON.md`.

Tests: `build\Release\hz_tests.exe` (codec, RPM estimator, a simulated recording through tones and RPM, ROIs, library).
With `QT_QPA_PLATFORM=offscreen`, also set `QT_PLUGIN_PATH=%USERPROFILE%\Qt\6.8.3\msvc2022_64\plugins`: windeployqt does
not copy the offscreen plugin next to the exe.

CLI: `hz_cli measure clip.raw --win abs --rect 985 354 1005 366 --rpm 3 --rpm-range 100 3000`.

## Simulator (dev edition)
A lab bench: four dark bars on the left ring at 212.4 / 287.65 / 335.2 / 401.85 Hz after every tap, and a 3-blade fan
on the right (hub at 960, 360) turns at the set speed (default 600 rpm, ±0.5 % wander at 0.2 Hz), with a bright tape
mark on one blade tip. Fan speed, blade count and tape can be changed on the Live page.

## Layout
`src/` C++ (core: `evt3` codec, `engine`, `sim_source` / `arena_source`, `clip`, `roi`, `measure`, `rpm`, `detect`,
`library`; app: `controller` (Live + Camera), `analyse`, `libctl`, `player`, `roictl`, `liveview`, `webview`),
`common/` plot item, table model, DSP, spectra; `qml/` the UI; `icons/` (`make_icons.py` draws the icon); `tools/`.
