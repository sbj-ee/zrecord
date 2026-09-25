# Changelog

All notable changes to zrecord are recorded here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.3.0] - 2026-09-25

A reliability and editing release. It fixes the data-loss, crash, leak and
glitch bugs found in review (see Fixed). It adds a moving playhead with
click-to-seek, a dB peak meter, Normalize / Amplify and an offline Voice
Changer. Projects now store clips as 32-bit float WAV; older projects still
open, and are converted on their next save.

### Added

- **Click-to-seek and a following view.** Click or drag along the ruler to
  move the playhead, whether stopped or playing. During playback it jumps
  there without restarting. A click in a lane with the Select tool also
  seeks. The view pages along to keep the playhead in sight during
  playback.
- **Normalize / Amplify** (Edit menu). Scale a time selection, or clips
  picked with the Move tool (on any tracks), to a target peak in dBFS, or
  by a fixed gain in dB. It is one undo step. The dialog shows the current
  and resulting peak. If the result would pass full scale, a red CLIP
  indicator appears and OK needs "Allow clipping".
- **Voice Changer** (Edit menu). An offline effect on a time selection, or
  on clips picked with the Move tool. It shifts pitch by up to +/-12
  semitones and keeps the length unchanged. A separate formant control
  keeps shifted voices natural (0 leaves the vocal resonances where they
  were), or moves them for a cartoon voice. There is an optional robot
  monotone. Presets: Deeper, Higher, Robot, Chipmunk and Custom (touching
  any control switches to Custom). **Preview** plays the processed
  selection through the normal playback engine without changing anything.
  Apply is one undo step. It uses a built-in phase vocoder with
  cepstral-envelope formant shifting, so there is no new dependency.
- **dB peak meter.** It replaces the linear level bar and reads -60 to
  +3 dBFS, with green, amber and red zones. The bar falls back smoothly,
  and a peak-hold marker with a readout keeps the recent maximum for
  1.5 s. A CLIP light comes on at 0 dBFS and stays lit until you click the
  meter. It shows the input while recording and the mix while playing.
  Peaks are taken as the maximum since the last screen update, so a brief
  over can't slip between updates.
- **Help > About** shows the version, with the Qt and PortAudio versions.

### Changed

- **Development builds say so.** Any build other than the release tag is
  versioned `<version>+g<commit>` (e.g. `0.2.0+g1a2b3c4`) in the .deb and
  the About box. `-DZRECORD_RELEASE=ON` gives the plain version.
- **Packaging.** Building the .deb now fails with a clear message if
  `dpkg-shlibdeps` is missing, rather than producing a package with no
  dependencies. The package now recommends `pulseaudio-utils` (for `pactl`,
  used by the mic volume slider). libsamplerate is a new build and run
  dependency.
- **Project folders.** Clip audio now lives in an `audio-<id>/` folder that
  each save replaces (older projects with `audio/` still open, and are
  converted on their next save).
- **CI and releases.** The tests also run under AddressSanitizer and
  UndefinedBehaviorSanitizer in CI. The .deb's metadata is checked on
  every build. A tag-triggered workflow (`v*`) builds, tests and publishes
  a GitHub Release with the .deb attached.

### Fixed

- **Saved projects no longer corrupt loud audio.** Clips were stored as 24-bit
  PCM without clipping, so any sample past full scale (after gain or echo)
  wrapped around on save: 1.2 reopened as -0.8. Project clips are now stored as
  32-bit float WAV, which reopens exactly. Projects saved as 24-bit still load,
  and are converted to float on their next save.
  Integer-PCM exports (WAV/FLAC) now clip instead of wrapping.
- **The limiter now actually prevents clipping.** It ran first in the filter
  chain, so gain, the compressor and the voice effects after it could push the
  signal straight past the ceiling (-1 dB limiter + 12 dB gain gave +6 dBFS).
  It now runs last, and its row sits at the bottom of the filter panel.
- **A failed Open no longer wipes the current project.** If a clip file was
  missing or unreadable, Open had already cleared the project before giving
  up, and the undo history still pointed at the discarded tracks (the next
  Undo read freed memory). Projects now load all-or-nothing: on failure
  nothing changes; on success the undo history is cleared as before.
- **Playback that ends by itself no longer leaks the audio stream.** The
  finished PortAudio stream was never closed, and the next Play overwrote it,
  leaking the device handles every time a project played to the end. It is
  now released as soon as playback finishes, and New/Open stop playback
  before replacing the project.
- **Closing the window with undo history no longer touches freed memory.** The
  undo stack was torn down after the window's other members, and its last
  change notification ran against the half-destroyed window.
- **Edits no longer duplicate the whole track.** Every edit used to keep two
  full copies of the track's audio for undo, so a 0.1 s edit on a 10-minute
  stereo take cost ~440 MB. Clip audio is now stored in shared, immutable
  chunks: undo snapshots copy only clip headers and an edit copies only the
  chunks it touches (~1 MB per small edit; the 10-minute take itself now
  costs 238 MB instead of 675 MB).
- **Playback no longer stutters while the timeline repaints.** The playback
  callback waited on the same lock the timeline held for its whole repaint
  (~450 ms for a spectrogram), and allocated on every callback. Playback now
  renders lock- and allocation-free from a snapshot of the project that is
  refreshed every tick, and spectrogram tiles are cached, so a repeat
  spectrogram repaint takes ~1.4 ms instead of ~430 ms.
- **Import converts the sample rate.** A file at a different rate than the
  project (say 48 kHz into 44.1 kHz) was added as-is and played at the wrong
  speed and pitch. It is now resampled to the project rate with libsamplerate
  (new dependency), and the status bar says so. Opening a project likewise
  resamples any clip stored at another rate. A rejected import (channel
  mismatch) no longer leaves an empty "Imported" track behind.
- **Envelope drags can be undone.** Undo after dragging an envelope point
  left the point where it was dragged to, and a single click added two undo
  steps. Each press-drag-release is now exactly one undo step (none if the
  point didn't move).
- **Unsaved changes are protected.** Quit and Open used to discard edits
  without asking, and New asked whenever anything had ever been done, even
  right after a save. The title bar now shows the project name with `*` when
  it has unsaved changes, and Quit, New and Open offer Save / Discard /
  Cancel. Undoing back to the saved state counts as saved; changing a track's
  mute, solo, gain or display counts as a change. Quitting mid-take keeps the
  take and asks as usual. Saving reports in the status bar instead of a
  dialog.
- **Moving a filter control mid-take no longer glitches the take.** Every
  slider tick rebuilt the whole filter chain, resetting the noise gate (it
  re-opened from silence), cutting the echo tail and zeroing the filters'
  state, even for controls that were switched off. Settings changes now
  update coefficients in place; a stage only starts fresh when it is switched
  on.
- **Mono-only input devices work.** Recording always asked the device for
  the project's channel count (stereo by default), so a mono device opened
  directly failed with "Invalid number of channels". A new project now
  defaults to the selected device's channel count, and a mono device
  recording into a stereo project is captured in mono and upmixed.
- **The playhead moves during playback.** It used to stay where playback
  started until you pressed Stop. When playback runs to the end, the
  playhead goes back to its starting point.
- **Recording stops playback.** Starting a take during playback used to
  leave playback running underneath it, and with Play disabled during the
  take there was no way to stop it.
- **Saving is atomic.** A save that failed part-way used to leave new clip
  audio under the old project.json, and project.json itself was written
  without checking. Each save now writes its audio into a new folder,
  replaces project.json atomically, and only then removes the previous
  audio. WAVs of deleted clips no longer build up in the project folder.
- **Clip selections can't go stale.** Clips picked with the Move tool were
  remembered by position, so after an undo, redo or paste a drag could move
  a different clip. The pick is now cleared whenever the tracks change.
- **Audio start-up failures are reported.** If PortAudio failed to start,
  the error was ignored and surfaced later as an empty device list. Record
  and Play now report it.
- **No allocation on the recording thread.** A host that delivered a larger
  audio block than expected made the recording callback allocate. Such
  blocks are now processed in pieces.
- **The timeline's layout no longer waits on the project lock**, the last
  place a repaint could be held up by it.

### Known limitations

- The Voice Changer processes on the UI thread with no progress bar, so a
  long selection freezes the window until it finishes (preview too).
- The Robot preset's buzz has a fixed pitch (about 86 Hz at 44.1 kHz).
- Voice Changer preview plays the processed selection on its own. It does
  not include the live filter chain (gain, filters, voice effects, limiter).
- Deferred from review:
  - Track header mute/solo/gain/display changes are not undoable and are
    written without the project lock (they do now mark the project
    unsaved).
  - Loading a project doesn't validate clip positions or channel counts.
  - The mic volume slider starts a `pactl` process per change and always
    targets the default source rather than the selected device.
- The repository has no LICENSE yet.

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
- **Volume envelopes** per track, drawn with the Envelope tool (click to add a
  point, drag to move, right-click to delete) and applied on playback and export.
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
- No plugin (LADSPA/LV2/VST) support.

## [0.1.0]

### Added

- Qt6 audio recorder: record from any PortAudio input device with a live
  scrolling waveform and level meter.
- Live filter chain — gain, high-pass, low-pass, noise gate, compressor,
  limiter — and voice effects (robot, echo, deep, chipmunk, distortion).
- Mic input volume control via PipeWire/PulseAudio.
- Playback of the current recording, and export to WAV, FLAC, OGG Vorbis or MP3.
- `.deb` packaging via CPack.

[Unreleased]: https://github.com/sbj-ee/zrecord/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/sbj-ee/zrecord/releases/tag/v0.3.0
[0.2.0]: https://github.com/sbj-ee/zrecord/releases/tag/v0.2.0
[0.1.0]: https://github.com/sbj-ee/zrecord/releases/tag/v0.1.0
