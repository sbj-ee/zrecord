# zrecord

A Qt6 multi-track audio recorder and editor for Linux, with per-track effect stacks (filters, dynamics and voice effects), and export to multiple formats.

## Features

- Multi-track timeline: add/remove tracks, per-track mute/solo/record-arm/gain, zoomable and scrollable
- Record from any input device via PortAudio onto the armed track, with a big Record/Stop button and a live scrolling waveform while recording
- Non-destructive editing: click-drag to select a region, then Cut/Copy/Paste/Delete/Silence, Fade In/Out and Normalize/Amplify, all undoable (Undo/Redo)
- Move clips between tracks, per-track volume envelopes, labels, and a spectrogram view
- Import existing audio files (WAV, FLAC, OGG, AIFF, ...) onto a track, resampled to the project rate
- Save/open a project (tracks, clips, and their timeline positions) as a `*.zrproj` folder; reopening restores it exactly, and saving is atomic
- Mic input volume control (adjusts the system source volume via PipeWire/PulseAudio; flagged above 100%), a digital input gain in dB (default 0 dB), and an INPUT CLIP light (with a per-take count) for input that is already clipped: 3+ consecutive full-scale samples, so a single loud peak doesn't count
- Records the raw input: a take is exactly what the device delivers, with only the input gain applied
- Per-track, non-destructive effect stacks applied on playback and export: gain, high-pass, low-pass, noise gate (with attack/release), compressor, limiter, and the Robot, Echo, Deep Voice, Chipmunk and Distortion voice effects. The track's FX button opens a dialog to add, remove, reorder, bypass and adjust them, heard live while playing; stacks are saved with the project, and Apply Track Effects (Ctrl+R) bakes a stack into the audio as one undo step
- Voice Changer (Edit menu): pitch shift in semitones that keeps the length, with an independent formant control, Deeper/Higher/Robot/Chipmunk/Custom presets, and a Preview before Apply (one undo step)
- Playback of the mixed project with a moving playhead; click the ruler to seek, stopped or playing
- dB level meter per channel: RMS bar, peak bar, peak hold with readout and a latching clip light, plus an optional input (pre-gain) tick; range and decay are set from its right-click menu. It shows what is being recorded (after input gain), or the playback mix after track effects
- Clipped audio (runs of full-scale samples) is painted red in the waveform
- Dropout detection: input lost while recording (a driver overflow, or zrecord falling behind) is filled with silence of the same length, so the rest of the take stays in sync, and marked with a "Dropout 12 ms" label; the status bar gives the count and total lost after Stop
- Takes stream to disk while recording (a 32-bit float WAV per take, kept readable if zrecord is killed), so long takes don't fill memory; a full disk stops the take and keeps what was captured, with a message saying so
- Autosave and crash recovery: an autosave journal after every edit, and after a crash a Recover Unsaved Work dialog restores the project and any take that was being recorded, or discards it
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
| Ctrl+R | Apply the selected (or armed) track's effects to its audio |
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

- Effects are heard on playback, not while recording: zrecord doesn't monitor the input through the speakers, and a take is recorded raw so its effects can still be changed afterwards. The FX button is lit on any track whose playback differs from its waveform.

- Recording streams each take to a file: into the project's `takes/` folder once the project is saved, otherwise into the recovery folder (`~/.local/share/zrecord/recovery`). Saving copies takes into the project's clip files and removes the take files. After Stop the take is also loaded into memory, like the rest of the project.
- Crash recovery keeps one session folder per running zrecord under `~/.local/share/zrecord/recovery`, holding the autosave journal. A clean exit, or saving, removes it. After a crash, the next launch offers to restore or discard it. The undo history isn't restored.
- A take file past 4 GiB (about 3 hours of 48 kHz stereo) keeps growing, but its WAV header sizes stay at the 4 GiB maximum. zrecord reads the whole file; other tools may stop at 4 GiB unless they go by the file's size.

- If a take shows "Dropout" labels, input was lost there and replaced with silence (so the rest stays in time). Frequent dropouts usually mean the system is overloaded or the audio device's buffer is too small for it.
- If recording or playback seems stalled or silent, check your system's mic input volume/mute state (e.g. via `wpctl status` or your desktop's sound settings) before assuming it's an app bug.
