# zrecord

A Qt6 desktop audio recorder for Linux with a live filter chain, voice effects, and export to multiple formats.

## Features

- Record from any input device via PortAudio, with a big Record/Stop button and live scrolling waveform
- Mic input volume control (adjusts the system source volume via PipeWire/PulseAudio)
- Live filter chain: gain, high-pass, low-pass, noise gate (with attack/release)
- Selectable voice effects: Robot Voice, Echo, Deep Voice, Chipmunk, Distortion
- Playback of the current recording
- Export to WAV, FLAC, OGG Vorbis, or MP3 (all via libsndfile)

## Dependencies

Tested on Ubuntu/Pop!_OS (Debian-based). Install build dependencies:

```bash
sudo apt-get install build-essential cmake qt6-base-dev portaudio19-dev libsndfile1-dev
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

This produces `build/zrecord_<version>_amd64.deb`. Install it with:

```bash
sudo apt install ./build/zrecord_0.1.0_amd64.deb
```

This installs the `zrecord` binary to `/usr/bin` and a desktop entry to `/usr/share/applications`, so it also appears in your application launcher.

## Notes

- If recording or playback seems stalled or silent, check your system's mic input volume/mute state (e.g. via `wpctl status` or your desktop's sound settings) before assuming it's an app bug.
