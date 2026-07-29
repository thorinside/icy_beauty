# AC-004 target-hardware validation

This file defines the repeatable target run; it does **not** record a passing result. The accelerated `make endurance` regression cannot replace physical disting NT evidence.

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

If the project owner explicitly approves counting an NsIb-only session that was
already running before the harness was written, close that interval without
recreating the preset:

```sh
python3 scripts/target_hardware_endurance.py \
  --duration-seconds 5 \
  --responsiveness-interval-seconds 2 \
  --existing-session-start '<ISO-8601 timestamp>' \
  --existing-session-note '<durable start and configuration-transition proof>'
```

This mode still reruns preflight, verifies the current eight-voice slot and all
configured values, drives a final dense-MIDI closeout, waits through release,
and submits the full owner-approved residency interval rather than only the
closeout duration.

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
