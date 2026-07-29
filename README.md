# Icy Beauty

Icy Beauty is a focused software synthesizer plug-in for the Expert Sleepers disting NT. It exposes a disting NT API factory, listens in MIDI Omni mode, renders an icy dual-triangle voice, and routes audio to Output 1 by default. The **Voices** specification configures one to eight shared voices (four by default) for both MIDI and polyphonic CV/gate playing.

CV/gate control exposes one shared **Gate** input followed by one **Pitch** CV input per configured voice. The defaults follow the disting NT sequential-input convention: Gate uses Input 1, and Pitch 1 onward use Input 2 onward. A rising gate triggers the configured pitches together; a falling gate releases them. When MIDI and CV contend for the shared voice pool, held MIDI notes take priority: MIDI may replace a CV voice, while a CV gate retrigger preserves held MIDI notes.

This is an early delivery slice, not the complete approved instrument. The five final sound controls, expressive MIDI mappings, and target-hardware performance/listening acceptance remain future work.

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
