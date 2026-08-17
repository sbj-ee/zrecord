# Scoping: plugin support

Status: **proposal, not agreed.** Written to make the decisions explicit before
any code is committed to.

## What this is for

zrecord has a fixed set of built-in effects. Every real editor lets you load
third-party ones. This document scopes what that would take, recommends a
starting standard, and — most importantly — names one prerequisite that has to
be dealt with first.

## The prerequisite: the audio callback is not real-time safe

`AudioEngine::handleInput` runs on the PortAudio callback thread and currently
does three things no real-time audio callback should:

```cpp
std::vector<float> block(frameCount * channels_, 0.0f);   // heap allocation
std::lock_guard<std::mutex> lock(captureMutex_);          // unbounded blocking
captureBuffer_.insert(...);                               // reallocates, grows forever
```

Today this mostly works: buffers are small, the mutex is barely contended, and a
desktop machine absorbs the jitter. It is still a latent source of dropouts
under load, and it is a **hard blocker for hosting third-party code in the live
chain** — a plugin is free to allocate, lock, or simply take too long, and any
of those turn a latent problem into an audible one that users will blame on
zrecord rather than on the plugin.

**Nothing should be hosted in the live chain until this is fixed.** The fix is
well-understood and independent of plugins:

- preallocate the processing block at `startRecording` and reuse it;
- replace the capture mutex with a lock-free ring buffer drained by the UI
  thread;
- grow `captureBuffer_` off the audio thread.

This is worth doing on its own merits, and it is the reason the phasing below
puts offline effects before live ones.

## Which standard

| | Hosting cost | Ecosystem on Linux | Licensing | Verdict |
|---|---|---|---|---|
| **LADSPA** | Trivial — one header, ~200 lines | Large and packaged (swh, calf, tap) | Permissive | **Start here** |
| **LV2** | Real — needs `lilv`, ports, atoms, worker threads | The modern Linux standard | Permissive | Phase 2 |
| **VST2** | Moderate | Large | SDK withdrawn by Steinberg | **Avoid** |
| **VST3** | High | Large | Dual-licensed, heavy | No |
| **CLAP** | Moderate, clean | Small but growing | Permissive | Reconsider later |

**Recommendation: LADSPA first.** Not because it is the best format — LV2 is —
but because the hard part of this work is not the plugin API. It is discovery,
lifetime, parameter UI generation, chain integration, persistence and crash
safety. LADSPA lets all of that be built and proven against a header-only C API
with no new hard dependency, and that scaffolding is what LV2 would reuse.

## Phasing

Each phase is independently shippable and independently useful.

**Phase 1 — host core, no UI.** Scan `LADSPA_PATH` (defaulting to the standard
Linux directories), `dlopen` each library, enumerate descriptors, expose a
`PluginDescriptor` (name, maker, ports with ranges and defaults). Instantiate,
connect ports, run, destroy. Entirely headless and unit-testable against a
known plugin, or against a fixture plugin built by the test suite.

**Phase 2 — offline only.** Add plugins to *Apply to Selection*, alongside the
existing `FilterChain`. This is the safe integration: `ApplyEffectCommand`
already builds a chain, processes a buffer and writes it back, off the audio
thread and with no deadline. A plugin that misbehaves here costs a stall, not a
dropout.

**Phase 3 — parameter UI.** Generate controls from port descriptors (a slider
per control port, honouring range, default, logarithmic and toggle hints).
No plugin-provided GUIs — that is a separate and much larger problem.

**Phase 4 — live chain.** Only after the real-time work above. Plugins run in
the recording path with preallocated buffers.

**Phase 5 — persistence.** Plugin identity plus parameter values in
`project.json`, and a decision about what happens when a project is opened on a
machine where a plugin is missing (proposal: keep the settings, disable the
effect, tell the user — never silently drop them).

## Risks worth deciding on now

**Crash isolation.** A third-party plugin shares the process. A segfault in one
takes zrecord down and loses unsaved work. Options: accept it (what Audacity
did for years), sandbox in a helper process (expensive, adds IPC to the audio
path), or mitigate — scan and instantiate defensively, and make autosave good
enough that a crash is survivable. **Recommendation: mitigate.** Full isolation
is not worth its cost at this scale, but "you lose your work" is not acceptable,
so autosave becomes a prerequisite for shipping this.

**Discovery is not standardised in practice.** Paths vary by distribution.
Needs a configurable search path with sensible defaults, and a UI that says
*why* nothing was found.

**Testing.** Real plugins make tests depend on what is installed. Build a
minimal LADSPA plugin as a test fixture so the host has something deterministic
to load in CI.

## What this is not

Not MIDI, not instruments, not plugin-provided GUIs, not VST. Effects with
generated parameter controls only.

## Rough size

Phase 1 and 2 together are the bulk of the value and are perhaps a week of
focused work including tests. Phases 3–5 are comparable again. The real-time
prerequisite is separate and should be scheduled on its own merits.
