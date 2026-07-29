# Target-hardware validation

This file defines the repeatable target run and identifies the retained passing AC-004 evidence. The accelerated `make endurance` regression cannot replace physical disting NT evidence.

Run this procedure after the approved synthesis surface and MIDI behavior are complete, so the test covers the most CPU-intensive valid eight-voice patch without omitting or reducing any approved feature.

## Setup

1. Record the disting NT firmware version and relevant host/API configuration.
2. Run `make verify` and `make endurance`, then install the resulting `plugins/icy_beauty.o` through the supported SD-card workflow.
3. Confirm the plug-in loads normally using only the memory made available by the host/API.
4. Select **Voices: 8**, route to Output 1, and select MIDI Omni.
5. Set the five sound controls to the valid combination that produces the highest processing load. Keep every approved synthesis feature enabled at its normal quality.
6. Connect the disting NT over USB MIDI and launch `nt_helper` with its MCP
   server available at `http://127.0.0.1:3847/mcp`.

## Thirty-minute stream

For 30 uninterrupted wall-clock minutes, repeatedly send eight-note note-on/note-off groups while varying velocity and forcing voice replacement. During the same stream, repeatedly operate sustain, full-range pitch bend, modulation wheel, polyphonic aftertouch when supported, and channel pressure. Keep the synth active rather than leaving a static chord sounding.

Observe loading, host/API memory behavior, and target responsiveness for the
entire run. Do not lower voice count, disable a sound feature, or reduce
synthesis quality to obtain a pass. The native preflight remains responsible
for sample-level finite-audio, continuous-output, and final note-cleanup
assertions; the physical run supplies the missing uninterrupted wall-clock
target evidence.

Digital capture from the NT would require adding the separate **USB audio (to
host)** algorithm. This harness does not add it, because the approved physical
preset must contain only `NsIb`.

With the physical NT and `nt_helper` connected, the repeatable harness is:

```sh
make hardware-endurance
```

It reruns the accelerated native endurance and ARM preflight, creates a preset
containing only eight-voice `NsIb`, applies the full valid sound-control
settings, mirrors the committed native dense-MIDI pattern for 30 wall-clock
minutes, and polls the live preset and CPU state every ten seconds through
`nt_helper` MCP. After MIDI cleanup it waits through the release and performs a
final physical response check. It writes a JSON report under
`verification/hardware-runs/` and submits AC-004 evidence only after a complete
pass. Use `make hardware-endurance-smoke` to exercise the same physical
plumbing for five seconds without submitting evidence.

## Retained AC-004 pass record

The physical run retained at
`verification/hardware-runs/icy-beauty-20260729T205925Z/target-hardware-endurance.json`
passed on disting NT firmware v1.17.0 with nt_helper 1.39.0. It tested the
plugin built from `c01865e` at eight voices with Tone, Motion, Grain,
Resonance, and Release all at 100. The run completed 1,800.007 seconds and
3,599 dense-MIDI cycles, with 182 successful physical preset/CPU polls and no
failed checks. Its complete AC-004 evidence was submitted and retained by the
acceptance tracker. Commit `5050052` records the report.

A qualifying report records all of the following:

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

## AC-005 processing and latency evidence

Every retained processing-meter value from the passing physical run is at or
below 29%, which is below the approved 75% ceiling. `make script-test` checks
all 182 retained polls rather than trusting only the report summary. The native
render test also proves that, once the host invokes the plug-in's MIDI callback,
the maximum-control eight-voice patch produces finite audio in the immediately
following render block.

The physical latency gate is:

```sh
make hardware-latency
```

It requires the existing approved preset to contain exactly eight-voice NsIb
in slot 0 with Output 1, MIDI Omni, and all five sound controls at 100. The
harness then performs the following guarded sequence:

1. Add the built-in **USB audio (to host)** algorithm in slot 1.
2. Route `USB channel 1 from` to the same `Output 1` used by NsIb and set USB
   channels 2-12 to `None`.
3. Save and read back the two-slot topology through `nt_helper`, including the
   routing graph.
4. Open the exact 12-channel `disting NT` PortAudio input at 48 kHz and select
   host input 1.
5. Send eight isolated A4 note-ons at velocity 127 through the exact CoreMIDI
   destination `disting NT`, allowing the maximum Release tail to become
   silent between trials.
6. Bracket every immediate CoreMIDI send with `Pa_GetStreamTime()` and use
   each callback's `inputBufferAdcTime`. PortAudio defines these values on the
   same stream clock, so the captured sample and MIDI-send interval can be
   compared without independent-process launch timing.
7. Detect onset only after four consecutive physical samples satisfy the
   signal gate. The reported upper bound starts at the earliest possible MIDI
   send time, ends after the complete onset-confirmation window, and includes
   the maximum observed adjacent-block timestamp discontinuity.
8. Require every trial to have at least 60 dB signal-to-baseline separation,
   no capture overflow/underflow, and a conservative upper latency below
   10 ms.
9. Retain the raw float capture, timing JSON, verified 48 kHz/24-bit mono WAV,
   hashes, topology, analysis, and the prior 182-check processing report.
10. In every success, failure, timeout, or interruption path, remove slot 1,
    save, and compare the preset and all NsIb parameters with the original
    snapshot before considering the test complete.

macOS attributes audio-input permission to the ad-hoc-signed foreground
**Icy Beauty Latency Capture** app declared by
`scripts/latency_capture_Info.plist`. The first run may require approving its
microphone dialog. The app records only the selected disting NT USB stream.

`make hardware-latency-smoke` runs one physical trial without evidence
submission. A full passing run submits AC-005 only when all eight latency
trials pass, the retained maximum processing use is at most 75%, the checkout
was clean at test start, and the NsIb-only preset was restored exactly.

### Retained AC-005 pass record

The evidence-grade run retained at
`verification/hardware-runs/icy-beauty-latency-20260729T221816Z/target-midi-latency.json`
passed against clean commit `8f846d5`. All eight physical note-ons passed with
conservative upper bounds of 1.176, 1.066, 1.038, 1.328, 1.120, 1.177, 0.763,
and 0.980 ms. The recorded maximum is 1.329 ms after upward rounding, safely
below the 10 ms limit. The largest captured block-timestamp discontinuity,
included in every bound, was 0.026 ms.

The capture contained 3,678,208 samples over 76.629 seconds with no PortAudio
status flags, null input, or capacity overrun. The verified 48 kHz, 24-bit PCM
mono WAV has SHA-256
`23a83b8a917f0128b466f9bcff7f119047627a6e89fedd2bdf3c33637255fbb1`.
The timing JSON has SHA-256
`17081997628c6525d3276d603185f6520b992a04ba1a25d7540e3e6aa63bc699`.

The two-slot routing graph showed NsIb Output 1 feeding only USB host channel
1. After capture, slot 1 was removed and every original NsIb parameter was
compared with the starting snapshot; exact restoration passed. Combined with
the retained 182-check, 30-minute processing maximum of 29%, this evidence was
accepted by the Portal tracker and changed AC-005 to `met`.
