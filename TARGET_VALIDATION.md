# AC-004 target-hardware validation

This file defines the repeatable target run; it does **not** record a passing result. The accelerated `make endurance` regression cannot replace physical disting NT evidence.

Run this procedure after the approved synthesis surface and MIDI behavior are complete, so the test covers the most CPU-intensive valid eight-voice patch without omitting or reducing any approved feature.

## Setup

1. Record the disting NT firmware version and relevant host/API configuration.
2. Run `make verify` and `make endurance`, then install the resulting `plugins/icy_beauty.o` through the supported SD-card workflow.
3. Confirm the plug-in loads normally using only the memory made available by the host/API.
4. Select **Voices: 8**, route to Output 1, and select MIDI Omni.
5. Set the five sound controls to the valid combination that produces the highest processing load. Keep every approved synthesis feature enabled at its normal quality.
6. Connect a MIDI source and audio monitoring/capture setup that can reveal dropouts, invalid output, and notes that continue after the stream stops.

## Thirty-minute stream

For 30 uninterrupted wall-clock minutes, repeatedly send eight-note note-on/note-off groups while varying velocity and forcing voice replacement. During the same stream, repeatedly operate sustain, full-range pitch bend, modulation wheel, polyphonic aftertouch when supported, and channel pressure. Keep the synth active rather than leaving a static chord sounding.

Observe loading, host/API memory behavior, audio, and note state for the entire run. Do not lower voice count, disable a sound feature, or reduce synthesis quality to obtain a pass.

## AC-004 pass record

Record all of the following:

- tested firmware and configuration;
- the exact eight-voice control settings used and why they are the most CPU-intensive valid patch;
- normal plug-in loading within host/API memory;
- at least 30 uninterrupted minutes of dense MIDI activity;
- no crash;
- no audio dropout;
- no stuck note after the final note-offs and sustain release;
- no NaN, Infinity, or other invalid audio value detected by the available monitoring method; and
- no approved sound feature disabled, degraded, or adaptively reduced.

Any failed or unobserved item leaves AC-004 incomplete. Preserve the test notes with the build commit and tested firmware/configuration.
