# Icy Beauty

Icy Beauty is a focused software synthesizer plug-in for the Expert Sleepers disting NT. The current vertical slice is a playable MIDI instrument: it exposes a disting NT API factory, listens in MIDI Omni mode, renders an icy dual-triangle voice, and routes audio to Output 1 by default.

This is an early delivery slice, not the complete approved instrument. Polyphony, CV/gate control, the five final sound controls, expressive MIDI mappings, and target-hardware performance/listening acceptance remain future work.

## Build

Requirements:

- Arm GNU Toolchain (`arm-none-eabi-c++`, `arm-none-eabi-nm`, and `arm-none-eabi-readelf`)
- A native C++ compiler for the host-side render test
- The pinned `distingNT_API` submodule

After cloning:

```sh
git submodule update --init
make verify
```

The installable module product is `plugins/icy_beauty.o`. Copy that relocatable object to the disting NT plug-ins folder using the module's supported SD-card workflow.

`make verify` rebuilds the ARM Cortex-M7 object, runs a native host-contract/render test, and inspects the object architecture and `pluginEntry` export.

## Canonical reference

The owner-supplied comparison excerpt is retained locally at `model/cononical.wav`, as required by the approved Spec. It is development reference material only: the build does not read, embed, copy, or package it.

## Dependency and licensing

The Expert Sleepers `distingNT_API` dependency is pinned as a Git submodule. See [`DEPENDENCIES.md`](DEPENDENCIES.md) for its revision and license.
