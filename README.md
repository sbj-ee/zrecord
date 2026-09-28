# zrecord

A Qt6 multi-track audio recorder and editor for Linux, with a live input filter chain, voice effects, and export to multiple formats.

## Features

- Multi-track timeline: add/remove tracks, per-track mute/solo/record-arm/gain, zoomable and scrollable
- Record from any input device via PortAudio onto the armed track, with a big Record/Stop button and a live scrolling waveform while recording
- Non-destructive editing: click-drag to select a region, then Cut/Copy/Paste/Delete/Silence, Fade In/Out and Normalize/Amplify, all undoable (Undo/Redo)
- Move clips between tracks, per-track volume envelopes, labels, and a spectrogram view
- Import existing audio files (WAV, FLAC, OGG, AIFF, ...) onto a track, resampled to the project rate
- Save/open a project (tracks, clips, and their timeline positions) as a `*.zrproj` folder; reopening restores it exactly, and saving is atomic
- Mic input volume control (adjusts the system source volume via PipeWire/PulseAudio; flagged above 100%), a digital input gain in dB (default 0 dB), and an INPUT CLIP light (with a per-take count) for input that is already clipped: 3+ consecutive full-scale samples, so a single loud peak doesn't count
- Live filter chain applied while recording: gain, high-pass, low-pass, noise gate (with attack/release), compressor, limiter
- Selectable voice effects: Robot Voice, Echo, Deep Voice, Chipmunk, Distortion
- Voice Changer (Edit menu): pitch shift in semitones that keeps the length, with an independent formant control, Deeper/Higher/Robot/Chipmunk/Custom presets, and a Preview before Apply (one undo step)
- Playback of the mixed project with a moving playhead; click the ruler to seek, stopped or playing
- dB level meter per channel: RMS bar, peak bar, peak hold with readout and a latching clip light, plus an optional input (pre-gain) tick; range and decay are set from its right-click menu. It shows what is being recorded (after input gain and live filters), or the playback mix
- Clipped audio (runs of full-scale samples) is painted red in the waveform
- Export a mixdown to WAV, FLAC, OGG Vorbis, or MP3 (all via libsndfile)

## Keyboard shortcuts

Every shortcut is also listed next to its entry in the menus.

**Project**

| Key | Action |
| --- | --- |
| Ctrl+N | New project |
| Ctrl+O | Open project (File > Open Recent lists the last 8) |
| Ctrl+S | Save (asks for a folder only the first time) |
| Ctrl+Shift+S | Save As |
| Ctrl+I | Import audio onto the selected track |
| Ctrl+E | Export a mixdown |
| Ctrl+Q | Quit |

**Transport**

| Key | Action |
| --- | --- |
| R | Start / stop recording |
| Space | Play / stop from the playhead |
| Home / End | Playhead to the start / end of the project\* |
| Left / Right | Playhead back / forward one second\* |

\* Click the timeline first. These keys only work while it has focus, so a focused slider or list keeps its own arrow keys. They seek during playback.

**Editing**

| Key | Action |
| --- | --- |
| Ctrl+Z / Ctrl+Shift+Z | Undo / Redo |
| Ctrl+A | Select all |
| Ctrl+X / Ctrl+C / Ctrl+V | Cut / Copy / Paste (at the playhead) |
| Delete | Delete the selection |
| Ctrl+L | Silence the selection |
| Ctrl+R | Apply the filter settings to the selection |
| Ctrl+B | Add a label (selection, or playhead) |
| Ctrl+Shift+N / Ctrl+Shift+W | Add / remove a track |

**Tools and view**

| Key | Action |
| --- | --- |
| F1 / F5 / F2 | Select / Move / Envelope tool |
| Ctrl+= (or Ctrl++, Ctrl+1) | Zoom in |
| Ctrl+- (or Ctrl+3) | Zoom out |
| Ctrl+F | Zoom to fit the project |
| Ctrl+mouse wheel | Zoom around the pointer |
| Mouse wheel | Scroll the timeline |
| Alt while dragging | Bypass snapping |

## Dependencies

Tested on Ubuntu/Pop!_OS (Debian-based). Install build dependencies:

```bash
sudo apt-get install build-essential cmake qt6-base-dev portaudio19-dev libsndfile1-dev libsamplerate0-dev dpkg-dev
```

MP3 export requires a libsndfile build with MPEG support (1.2.x from recent Debian/Ubuntu releases includes this out of the box).

## Building

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

The binary is produced at `build/zrecord`. Run it directly:

```bash
./build/zrecord
```

## Building a .deb package

The project uses CPack to produce a Debian package with dependencies detected automatically:

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
cd build
cpack -G DEB
```

This produces `build/zrecord_<version>_amd64.deb`. Packaging stops with an error if `dpkg-shlibdeps` (package `dpkg-dev`) is missing, since the package's dependencies come from it. Install it with:

```bash
sudo apt install ./build/zrecord_*_amd64.deb
```

Builds from anything other than the release tag are versioned `<version>+g<commit>` (for example `1.1.1+g1a2b3c4`), in the package and in Help > About; configure with `-DZRECORD_RELEASE=ON` for a plain version. Pushing a `v<version>` tag runs the release workflow, which builds, tests and attaches the `.deb` to a GitHub Release.

This installs the `zrecord` binary to `/usr/bin` and a desktop entry to `/usr/share/applications`, so it also appears in your application launcher.

## Design notes

- [Plugin support scoping](docs/plugin-support-scope.md) — proposal for hosting third-party effects.

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for what changed between releases.

## License

zrecord is released under the [MIT License](LICENSE). Copyright (c) 2026 Stephen B. Johnson.

The .deb links dynamically against the system's libraries, each under its own license (shipped in its own package): Qt 6 (LGPL-3.0), PortAudio (MIT-style), libsndfile (LGPL-2.1-or-later) and libsamplerate (BSD-2-Clause). Nothing third-party is bundled or statically linked; the FFT and the voice changer DSP are zrecord's own code. If you redistribute zrecord with Qt or libsndfile bundled or linked statically (e.g. an AppImage), you must then meet their LGPL terms.

## Notes

- If recording or playback seems stalled or silent, check your system's mic input volume/mute state (e.g. via `wpctl status` or your desktop's sound settings) before assuming it's an app bug.
