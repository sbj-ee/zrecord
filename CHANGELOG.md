# Changelog

All notable changes to zrecord are recorded here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.2.0]

zrecord became a multi-track editor. 0.1.0 recorded into a single buffer and
exported it; this release adds a timeline, editing, and the undo history that
makes editing safe.

### Added

- **Multi-track timeline** with per-track mute, solo, record-arm and gain, a
  zoomable and scrollable view, and a ruler.
- **Non-destructive editing** with full undo/redo: select a range, then cut,
  copy, paste, delete or silence it. Select All (`Ctrl+A`) covers a whole track.
- **Projects**: save and reopen work in progress as a `.zrproj` folder (a JSON
  manifest plus one WAV per clip). Export still produces a flat mixdown.
- **Import** existing audio files onto a track.
- **Apply filters to a selection** — the recording filter chain (gain,
  high/low-pass, noise gate, compressor, limiter, voice effects) can now be
  applied destructively to audio already on the timeline.
- **Fade in / fade out** over a selection, and **crossfade** between two
  adjacent clips (equal-power, merging the pair so the timeline never holds
  overlapping clips).
- **Clip dragging** along the timeline and between tracks, singly or several at
  once, with snapping to clip edges, the playhead and zero (hold `Alt` to
  bypass, and a guide line shows what a drag latched onto).
- **Labels and markers** on a timeline strip: `Ctrl+B` labels the selection or
  the playhead; double-click renames, right-click offers rename or delete.
- **Spectrogram view** per track, toggled from the track header.
- **Keyboard control** throughout, following Audacity's bindings where they
  apply (`Space`, `R`, `Ctrl+Z`, `Ctrl+X/C/V`, `Ctrl+1`/`3`/`F`, `F1`/`F5`), plus
  a menu bar that advertises every shortcut.
- **Recording indicator** in the toolbar with elapsed time, a mute (cough)
  button that records silence without interrupting the take, and a stop button.
- **Continuous integration** building, testing and packaging every change, and a
  test suite covering the audio model, DSP, undo/redo, snapping geometry, the
  FFT, and the interaction layer.

### Changed

- The toolbar folds into an overflow menu when the window is narrow, instead of
  forcing the window wider than the screen.
- Recording leaves the playhead at the *start* of the new take, so it can be
  played back immediately.
- Exports are written as 24-bit PCM.

### Known limitations

- Labels sit at absolute positions and do not follow ripple edits.
- The spectrogram recomputes on every repaint rather than caching.
- No envelope editing and no plugin (LADSPA/LV2/VST) support.

## [0.1.0]

### Added

- Qt6 audio recorder: record from any PortAudio input device with a live
  scrolling waveform and level meter.
- Live filter chain — gain, high-pass, low-pass, noise gate, compressor,
  limiter — and voice effects (robot, echo, deep, chipmunk, distortion).
- Mic input volume control via PipeWire/PulseAudio.
- Playback of the current recording, and export to WAV, FLAC, OGG Vorbis or MP3.
- `.deb` packaging via CPack.

[Unreleased]: https://github.com/sbj-ee/zrecord/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/sbj-ee/zrecord/releases/tag/v0.2.0
[0.1.0]: https://github.com/sbj-ee/zrecord/releases/tag/v0.1.0
