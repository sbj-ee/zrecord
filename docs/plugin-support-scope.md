# Scoping: plugin support

Status: **proposal, not agreed.** Written to make the decisions explicit before
any code is committed to.

## What this is for

zrecord has a fixed set of built-in effects. Every real editor lets you load
third-party ones. This document scopes what that would take, recommends a
starting standard, and — most importantly — names one prerequisite that has to
be dealt with first.

## The prerequisite: a real-time-safe audio path (done)

When this was first written, `AudioEngine::handleInput` allocated a block on
every callback, took a mutex shared with the UI, and grew the capture buffer
without bound, and the playback callback locked the whole project. Hosting
third-party code in that path would have turned latent dropouts into audible
ones that users blame on zrecord rather than on the plugin, so it was made a
hard blocker for any live-chain hosting.

That work is now done (see the [Unreleased] changelog):

- the capture block is preallocated when recording starts and never grown on
  the audio thread (a larger host block is processed in slices);
- captured audio leaves through a lock-free ring buffer drained by the UI;
- filter settings reach the callback via `try_lock` (never blocking), and
  parameter changes update coefficients in place without allocating;
- playback renders from an atomically published, immutable snapshot of the
  project, with no lock and no allocation in the callback.

What remains specific to plugins is their own behaviour: a plugin can still
allocate, lock or overrun inside `run()`. That is a reason to keep live hosting
last (Phase 4) and to preallocate every port buffer at instantiation, not a
blocker for the offline phases.

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

**Phase 4 — live chain.** The engine side is ready (see above). Plugins run
in the recording path with every buffer preallocated at instantiation.

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
prerequisite has since been done on its own merits, so it no longer adds to
this estimate.
