# Target-hardware validation

This document defines repeatable physical checks for the current public
Icy Beauty identity, `ThIb`. Native tests are necessary preflight, but they do
not replace a run on a physical Expert Sleepers disting NT.

## Identity boundary

The source GUID changed from the development identity `NsIb` to the public
identity `ThIb`. Presets and evidence are GUID-bound. Historical measurements
captured with `NsIb` remain useful engineering context, but they are not
exact-object proof for a `ThIb` build.

## Preflight

1. Record the disting NT firmware and `nt_helper` versions.
2. Start from a clean checkout.
3. Run:

   ```sh
   git submodule update --init
   make verify
   make endurance
   ```

4. Install the resulting `plugins/icy_beauty.o`.
5. Confirm the loaded algorithm reports GUID `ThIb`.
6. Select **Voices: 8**, **Output 1**, and **MIDI Omni**.
7. Set Tone, Motion, Grain, Resonance, and Release to 100 for the
   highest-processing valid patch.

## Thirty-minute endurance

With the physical NT connected and the `nt_helper` MCP server available at
`http://127.0.0.1:3847/mcp`, run:

```sh
make hardware-endurance
```

The harness:

- reruns the native and ARM preflight;
- creates a preset containing only eight-voice `ThIb`;
- applies the maximum-control patch;
- sends dense eight-note MIDI activity for 30 uninterrupted wall-clock
  minutes;
- varies velocity, voice replacement, sustain, pitch bend, modulation wheel,
  polyphonic aftertouch, and channel pressure;
- polls the live preset and processing state every ten seconds; and
- releases every note, waits through the tail, and checks final
  responsiveness.

A passing run must retain the exact source commit and plug-in SHA-256, show no
crash, dropout, stuck voice, invalid sample, or feature reduction, and record
normal loading within the host’s memory limits.

`make hardware-endurance-smoke` exercises the same physical plumbing for five
seconds without submitting acceptance evidence.

## Physical MIDI latency

The latency harness requires an existing eight-voice `ThIb` preset in slot 0
with all five sound controls at 100:

```sh
make hardware-latency
```

It temporarily adds **USB audio (to host)** in slot 1, routes host channel 1
from the same output used by Icy Beauty, and captures the disting NT USB stream
while sending eight isolated A4 note-ons at velocity 127.

Every trial must:

- use the CoreMIDI send and PortAudio ADC timestamps from the same process;
- detect onset only after four consecutive qualifying samples;
- include observed audio-clock discontinuity in the conservative bound;
- remain below 10 ms;
- provide at least 60 dB signal-to-baseline separation; and
- complete without input overflow, underflow, null input, or capacity overrun.

On success or failure, the harness removes the USB slot, saves, and compares
the `ThIb` preset and every parameter with the starting snapshot.

`make hardware-latency-smoke` runs one guarded trial without evidence
submission.

## Historical development evidence

The repository retains two pre-public-identity records:

| Record | Build identity | Result |
| --- | --- | --- |
| `verification/hardware-runs/icy-beauty-20260729T205925Z/target-hardware-endurance.json` | `NsIb` at commit `c01865e` | 1,800.007 seconds, 3,599 activity cycles, 182 successful live polls, 29% maximum observed processing |
| `verification/hardware-runs/icy-beauty-latency-20260729T221816Z/target-midi-latency.json` | `NsIb` at commit `8f846d5` | eight passing trials, 1.329 ms maximum conservative bound, no capture status errors |

Those reports and their raw hashes have not been rewritten to claim the `ThIb`
identity. Run the procedures above whenever exact evidence is needed for a
current tagged release.
