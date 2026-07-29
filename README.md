# Icy Beauty

Icy Beauty is a focused software synthesizer plug-in for the Expert Sleepers disting NT. It exposes one curated, fixed signal path built from paired triangle oscillators, animated tuning, noise instability, tone shaping, and a tuned resonator. It listens in MIDI Omni mode and routes audio to Output 1 by default. The **Voices** specification configures one to eight shared voices (four by default) for both MIDI and polyphonic CV/gate playing.

The deliberately small sound surface contains exactly five host-mappable controls: **Tone** moves from dark toward glassy harmonics, **Motion** adds animated instability, **Grain** introduces noisy pitch texture, **Resonance** strengthens tuned ringing, and **Release** ranges from short decays to haunting tails. There are no selectable synthesis engines or general-purpose routing matrix.

CV/gate control exposes one shared **Gate** input followed by one **Pitch** CV input per configured voice. The defaults follow the disting NT sequential-input convention: Gate uses Input 1, and Pitch 1 onward use Input 2 onward. A rising gate triggers the configured pitches together; a falling gate releases them. When MIDI and CV contend for the shared voice pool, held MIDI notes take priority: MIDI may replace a CV voice, while a CV gate retrigger preserves held MIDI notes.

This remains an incremental delivery. Expressive MIDI mappings and target-hardware performance/listening acceptance are future work.

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

`make endurance` additionally simulates 30 minutes of continuous eight-voice dense MIDI. It cycles eight-note note-on/note-off activity, voice replacement, sustain, velocity, pitch bend, modulation wheel, channel pressure, and polyphonic pressure while checking every rendered sample, one-second audio continuity, and final voice release. This accelerated native check is a regression test, not a substitute for the required 30-minute run on a physical disting NT. See [`TARGET_VALIDATION.md`](TARGET_VALIDATION.md) for the target procedure and remaining acceptance evidence.

## Canonical reference

The owner-supplied comparison excerpt is retained locally at `model/cononical.wav`, as required by the approved Spec. It is development reference material only: the build does not read, embed, copy, or package it.

## Dependency and licensing

The Expert Sleepers `distingNT_API` dependency is pinned as a Git submodule. See [`DEPENDENCIES.md`](DEPENDENCIES.md) for its revision and license.
