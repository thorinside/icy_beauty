# Icy Beauty

Icy Beauty is a focused software synthesizer plug-in for the Expert Sleepers disting NT. It exposes one curated, fixed signal path built from a fundamental-forward triangle voice, restrained octave color, an intervallic upper shimmer, animated tuning, noise instability, tone shaping, and a tuned resonator. It listens in MIDI Omni mode and routes audio to Output 1 by default. The **Voices** specification configures one to eight shared voices (four by default) for both MIDI and polyphonic CV/gate playing.

The deliberately small sound surface contains exactly five host-mappable controls: **Tone** moves from dark toward glassy harmonics, **Motion** adds animated instability, **Grain** introduces noisy pitch texture, **Resonance** strengthens tuned ringing, and **Release** ranges from short decays to haunting tails. A fresh load centers Tone, Motion, Grain, and Resonance at 50% and sets Release to a medium-long 65%, yielding an immediately playable icy patch without setup. These are ordinary host parameters with no dedicated algorithm-level CV inputs; musicians can assign CV through the disting NT host's parameter-to-CV mapping. There are no selectable synthesis engines or general-purpose routing matrix.

CV/gate control exposes one shared **Gate** input followed by one **Pitch** CV input per configured voice. The defaults follow the disting NT sequential-input convention: Gate uses Input 1, and Pitch 1 onward use Input 2 onward. A rising gate triggers the configured pitches together; a falling gate releases them. When MIDI and CV contend for the shared voice pool, held MIDI notes take priority: MIDI may replace a CV voice, while a CV gate retrigger preserves held MIDI notes.

MIDI velocity controls loudness with a subtler brightness lift. Sustain behaves like a conventional piano pedal, pitch bend has a fixed ±2-semitone range, and the modulation wheel increases Motion. Aftertouch raises Resonance and adds a smaller amount of Motion: polyphonic pressure remains independent per note, while channel pressure applies across the chord. These mappings are fixed and channel-aware, including when the synth listens in Omni mode; they do not add a user-configurable mapping surface.

Audio is calibrated directly in disting NT bus volts. The synth contribution is linear through ±4.5 V, then soft-limited to remain inside ±5 V (10 Vpp). In Add mode, that ceiling applies to Icy Beauty before it is summed with any signal already present on the destination bus.

Presets use the disting NT host's standard mechanism. **Voices** is restored as the factory specification so the host reconstructs the matching dynamic parameter surface; the five sound controls, MIDI channel, output routing, Gate and Pitch assignments, and every other exposed setting are ordinary system-managed parameter values. The plug-in has no additional private setting that requires custom preset JSON.

This remains an incremental delivery. The approved 30-minute target-hardware
endurance run has passed, including a 29% maximum observed processing load.
The physical MIDI-note-on latency gate also passed eight captured trials with
a 1.329 ms maximum conservative bound. The harness temporarily routed Output 1
through the built-in USB audio (to host) algorithm, captured host input 1, and
restored the approved NsIb-only preset exactly. Owner listening acceptance
remains separate.

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

`make verify` rebuilds the ARM Cortex-M7 object, runs a native host-contract/render test, round-trips a non-default six-voice preset through the host-managed specification and parameter contract, verifies that the untouched fresh-load patch produces finite animated audio and a medium-long tail through Output 1 from Omni MIDI, characterizes the five controls at their extremes (brightness, pitch animation, noise roughness, tuned resonance, and release-tail duration), verifies fixed velocity, sustain, pitch-bend, modulation-wheel, polyphonic-aftertouch, and channel-pressure behavior, checks retained reference-model evidence, and inspects the object architecture and `pluginEntry` export.

`make endurance` additionally simulates 30 minutes of continuous eight-voice dense MIDI. It cycles eight-note note-on/note-off activity, voice replacement, sustain, velocity, pitch bend, modulation wheel, channel pressure, and polyphonic pressure while checking every rendered sample, one-second audio continuity, and final voice release. This accelerated native check is a regression test, not a substitute for the retained 30-minute run on a physical disting NT. See [`TARGET_VALIDATION.md`](TARGET_VALIDATION.md) for the target procedures and retained physical evidence.

With the physical NT connected and `nt_helper` MCP available on port 3847,
run the guarded latency gate with:

```sh
make hardware-latency
```

The first local run may show a macOS microphone-permission dialog for
**Icy Beauty Latency Capture**. That small ad-hoc-signed foreground app exists
only so macOS can attribute the NT USB input request to a declared application
identity. `make hardware-latency-smoke` exercises the same routing, capture,
analysis, and rollback with one note-on and never submits acceptance evidence.

## Canonical reference and sonic model

The owner-supplied comparison excerpt is retained locally at
`model/cononical.wav`, as required by the approved Spec. The opening contains
overlaid synth and string material, so development targets the four exposed
energy attacks after 12.5 seconds. Reproducible waveform, polyphonic-note,
envelope, harmonic, and stereo analysis identifies the low phrase as
**D3 → F#3 → B2 → C#3**, with onset intervals of approximately
**2.395 → 1.211 → 2.389 seconds**. The inferred upper notes are retained with
confidence values and an explicit mixed-source caveat.

Run the complete dry-mono model check with:

```sh
make sonic-model
```

That command verifies the source WAV hash, regenerates
`analysis/reference/strong-note-analysis.json`, renders the exact plug-in DSP
playing the inferred phrase, and checks pitch, attack, spectral balance,
second-harmonic level, release shape, and output headroom. The retained
`analysis/candidate/pre-model-baseline.wav` records the former implementation;
`analysis/candidate/current-default.wav` is the modelled fresh-load sound for
auditioning, normalized with ±5 V represented as 0 dBFS. The comparison is an
objective development gate, not a substitute for owner listening acceptance.

The reference remains development material only. The ARM plug-in build does
not read, embed, copy, or package the WAV, analysis dependencies, or derived
reports.

## Dependency and licensing

The Expert Sleepers `distingNT_API` dependency is pinned as a Git submodule. See [`DEPENDENCIES.md`](DEPENDENCIES.md) for its revision and license.
