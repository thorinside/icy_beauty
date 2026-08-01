# Icy Beauty documentation fact check

Audit baseline: repository commit `dbcad73` (31 July 2026).

This audit compares the public documentation (`README.md`,
`docs/PARAMETERS.md`, `docs/index.html`, and `docs/parameters.html`) with the
plug-in implementation, its native tests, the pinned Expert Sleepers API, and
official Expert Sleepers and MIDI Association material. The supporting
`ASSETS.md`, `DEPENDENCIES.md`, and `TARGET_VALIDATION.md` files were also
checked against the release workflow, pinned submodule, harness source, and
retained evidence. Descriptions such as “woody”, “icy”, and “glass-like” are
artistic guidance rather than testable claims and are outside the factual
corrections below.

All corrections identified below are incorporated in the accompanying public
documentation changes.

## Corrections identified

### 1. Name the CV pitch anchor without relying on an octave convention

**Baseline wording:** “0 V at C3” / “0 V is C3”.

**Evidence:** The implementation anchors 0 V to MIDI note 48 at
130.8127826502993 Hz and applies `2^volts`, so every added volt is exactly one
octave (`icy_beauty.cpp:547-560`; `tests/icy_beauty_test.cpp:611-655`). The
MIDI Association explicitly notes that octave labels such as C3, C4, and C5
vary by convention while MIDI note numbers do not.

**Recommended wording:** “0 V is MIDI note 48 (about 130.81 Hz; C3 in
scientific pitch notation), and each added volt raises the pitch by one
octave.”

This preserves the useful note name while removing an avoidable ambiguity.

### 2. Do not call the Count limit a physical-bus limit

**Baseline wording:** Count is limited by the “physical buses” after the gate
input.

**Evidence:** API v13 defines 12 input buses, 8 output buses, and 44 auxiliary
buses, for 64 buses in total (`distingNT_API/include/distingnt/api.h:63-72`).
The plug-in calculates room using `kNT_lastBus - gateBus`, not the number of
front-panel input jacks (`icy_beauty.cpp:373-393`). Its derived pitch routes
can therefore continue into higher-numbered output or auxiliary buses when
the host permits that routing. The Input 9 -> Inputs 10/11/12 example remains
correct.

**Recommended wording:** “Count is limited by the selected Voices, the
11-CV-per-group maximum, the other groups' reservations, and the available
higher-numbered disting NT buses after the selected gate bus.”

### 3. Describe the Tone layers as simultaneous, not sequential

**Baseline wording:** Raising Tone adds the octave, “then” or “raising it
further” brings the high interval forward.

**Evidence:** Both upper-layer gains are nonzero at Tone 0 and both rise
linearly over the whole range. The octave-like layer changes from 0.035 to
0.130; the high layer changes from 0.280 to 0.820
(`icy_beauty.cpp:1213-1216`). There is no handoff threshold at which one layer
starts after the other.

**Recommended wording:** “Raising Tone increases both upper layers across the
whole range, with the high interval coming forward more strongly, while also
reducing smoothing.”

### 4. Scope the five-volt claim to Icy Beauty's own contribution

**Baseline wording:** The badge and signal-flow copy describe a “+/-5 V
output”.

**Evidence:** Icy Beauty applies a soft limiter to its contribution, with a
4.5 V knee and a ceiling approached from inside +/-5 V
(`icy_beauty.cpp:26-29`, `icy_beauty.cpp:895-909`). In the default Add mode,
that limited contribution is added to the bus's existing contents, so the
final shared bus is not itself guaranteed to remain inside +/-5 V
(`icy_beauty.cpp:1201-1204`, `icy_beauty.cpp:1334-1335`). The native test
verifies that an isolated contribution remains strictly inside the ceiling
(`tests/icy_beauty_test.cpp:943-973`).

**Recommended wording:** “Icy Beauty's contribution is soft-limited inside a
+/-5 V ceiling before Add or Replace routing.” Do not imply that Add mode
limits other signals already on the destination bus.

### 5. Make channel-pressure scope explicit

**Baseline wording:** Channel pressure applies the response “across the
chord”.

**Evidence:** Pressure state is stored and read per MIDI channel
(`icy_beauty.cpp:100-104`, `icy_beauty.cpp:809-823`,
`icy_beauty.cpp:865-866`). In Omni mode, notes on other MIDI channels are not
affected by a channel-pressure message on one channel.

**Recommended wording:** “Channel pressure applies the same Resonance/Motion
response to all active notes on that MIDI channel.”

### 6. Qualify the sustain-pedal promise for voice stealing

**Baseline wording:** Sustain “holds released notes until the pedal is
lifted.”

**Evidence:** Pedal-down does keep released notes gated, but when all MIDI
voices are occupied the allocator deliberately reuses the oldest ordinary
released voice first, then the oldest sustain-held voice, then the oldest
physically held voice (`icy_beauty.cpp:682-723`). The native test locks down
that order (`tests/icy_beauty_test.cpp:879-940`).

**Recommended wording:** “Sustain keeps released notes sounding until
pedal-up, unless polyphony pressure causes the voice to be reused.”

### 7. Say that Count reductions use the short internal fade

**Baseline wording:** Decreasing Count “releases the removed voices”.

**Evidence:** A Count reduction calls `fastReleaseVoice`, which uses the
internal 5 ms transition rather than the user Release setting
(`icy_beauty.cpp:29`, `icy_beauty.cpp:924-930`,
`icy_beauty.cpp:1218-1224`, `icy_beauty.cpp:1287-1290`).

**Recommended wording:** “Decreasing Count quickly fades the removed voices.”
This avoids suggesting that they always follow the programmed Release time.

### 8. Avoid calling the five parameters the complete sound surface

**Baseline wording:** `docs/index.html` calls the five normal parameters “the
complete sound surface”.

**Evidence:** The five parameters are the main timbre/envelope controls, but
velocity changes level and brightness, modulation wheel changes Motion,
pressure changes Motion and Resonance, pitch bend changes pitch, and Setup
contains two transpose controls (`icy_beauty.cpp:740-870`,
`icy_beauty.cpp:1205-1323`).

**Recommended wording:** Call them “the five main sound parameters” or “the
main sound-shaping surface”. The README's narrower claim that these five
parameters always address the same signal path is accurate.

### 9. Make the install scan step match the official procedure

**Baseline wording:** “Rescan plug-ins and add Icy Beauty.”

**Evidence:** The official user manual says `.o` plug-ins belong in
`/programs/plug-ins` and that the module scans that folder at startup and when
the card is remounted. It also says plug-in algorithms can then be loaded from
the Add algorithm menu. Firmware 1.13 added a SysEx reset/rescan command, but
“Rescan plug-ins” on its own can sound like a universal front-panel command.

**Recommended wording:** “Restart the module or remount the card so it scans
the plug-in folder, then load Icy Beauty from Add algorithm.” If NT Helper is
named, its supported reset/rescan operation may be offered as an alternative.

### 10. State the API/firmware support floor

**Baseline state:** The README badge correctly says API v13, but the install
section gives no corresponding firmware version.

**Evidence:** `pluginEntry()` reports `kNT_apiVersionCurrent`, and the pinned
header defines that value as API v13 (`icy_beauty.cpp:1367-1374`;
`distingNT_API/include/distingnt/api.h:30-50`). Expert Sleepers' firmware
notes identify disting NT firmware 1.15.0 as the release that updated the C++
API to v13.

**Recommended wording:** “Requires disting NT firmware 1.15.0 or newer (API
v13).”

### 11. Call the upper layer a near-octave

**Baseline wording:** The public guides call the second oscillator layer an
“octave”.

**Evidence:** Its phase increment is `2 + 1/127` times the main oscillator,
approximately 2.007874 times or 6.8 cents above an exact octave. The highest
layer is approximately 2.507874 times the main oscillator
(`icy_beauty.cpp:1280-1285`).

**Recommended wording:** Use “near-octave” for the second layer. “High
interval” or “higher interval” remains an accurate nontechnical description of
the third layer.

### 12. Restore the missing Release values in the Markdown patch table

**Baseline state:** In `docs/PARAMETERS.md`, the Frozen Prairie, Red Cedar Bell,
and Polar Night rows omit a numeric cell under Release, shifting their sound
descriptions into the Release column.

**Evidence:** The parallel HTML guide contains Release 70, 60, and 92 for
those three patches (`docs/parameters.html` on the audit baseline). Aurora
Glass already contains Release 78 in both formats.

**Recommended correction:** Add Release 70 for Frozen Prairie, 60 for Red
Cedar Bell, and 92 for Polar Night so the Markdown and HTML recipes agree.

## Verified claims

The following concrete claims match the implementation and tests and do not
need factual changes:

- One exported factory, named Icy Beauty, with GUID `ThIb`, instrument tag,
  and API v13 (`icy_beauty.cpp:1339-1374`;
  `tests/icy_beauty_test.cpp:978-1000`).
- Voices range 1-16, default 8; Gate groups range 0-6, default 4
  (`icy_beauty.cpp:9-14`, `icy_beauty.cpp:413-418`).
- Gate input, Count, and sample-and-hold controls default to None, 0, and Off;
  therefore a fresh instance leaves all voices available to MIDI
  (`icy_beauty.cpp:287-315`; `tests/icy_beauty_test.cpp:366-407`).
- Per-group Count is capped at 11 and dynamically constrained by the total
  Voices, other group reservations, and remaining bus numbers
  (`icy_beauty.cpp:350-393`; `tests/icy_beauty_test.cpp:317-363`).
- Pitch routes are gate bus +1, +2, and onward, including the Input 9 example
  (`icy_beauty.cpp:972-988`, `icy_beauty.cpp:1100-1126`;
  `tests/icy_beauty_test.cpp:718-766`).
- Pitch CV uses MIDI note 48 (about 130.81 Hz) at 0 V and doubles frequency
  per volt. Tests verify -5 V through +7 V at 48 kHz within 0.01 cent. Input
  CV is clamped to -8 V through +8 V, and oscillator frequency is also capped
  below Nyquist (`icy_beauty.cpp:547-560`;
  `tests/icy_beauty_test.cpp:611-655`).
- Gates rise only above 1.0 V and fall only below 0.5 V. The state is retained
  between those thresholds (`icy_beauty.cpp:1129-1150`).
- Sample-and-hold Off tracks while the gate is high; On captures each pitch
  on a rising edge and retains it until the next rising edge
  (`icy_beauty.cpp:1100-1126`; `tests/icy_beauty_test.cpp:509-549`).
- A Count increase under a held gate waits for a new edge. CV retriggering
  during an audible tail preserves oscillator/filter/envelope state rather
  than restarting it (`icy_beauty.cpp:1051-1098`;
  `tests/icy_beauty_test.cpp:410-507`, `tests/icy_beauty_test.cpp:552-608`).
- Fixed CV-prefix/MIDI-suffix partitions prevent MIDI and CV from stealing
  one another's reserved voices (`icy_beauty.cpp:359-370`,
  `icy_beauty.cpp:659-723`).
- MIDI Stop (`FC`), System Reset (`FF`), CC 120 (All Sound Off), and CC 123
  (All Notes Off) clear every Icy Beauty voice once the message is accepted
  (`icy_beauty.cpp:772-796`, `icy_beauty.cpp:825-877`). Afterward, a connected
  CV gate must fall below 0.5 V and rise above 1.0 V before it restarts.
- Pitch bend is fixed at +/-2 semitones; CC 1 raises Motion; CC 64 uses the
  standard 64-or-higher pedal-on threshold; polyphonic pressure is per note;
  channel pressure is per channel (`icy_beauty.cpp:562-568`,
  `icy_beauty.cpp:740-870`).
- Semitones covers -11 to +11 and Octaves covers -4 to +4. Both default to
  zero, apply to MIDI and CV, update active voices immediately, and do not
  replace the input CV stored by sample-and-hold (`icy_beauty.cpp:179-186`,
  `icy_beauty.cpp:341-344`, `icy_beauty.cpp:1008-1043`;
  `tests/icy_beauty_test.cpp:658-715`).
- Tone, Motion, Grain, and Resonance default to 50%; Release defaults to 65%
  (`icy_beauty.cpp:166-176`).
- At neutral modulation-wheel/pressure values, Motion 50% is approximately
  +/-6.5 cents at 0.54 Hz and Motion 100% is approximately +/-13 cents at
  1.00 Hz (`icy_beauty.cpp:1264-1273`).
- Grain uses a squared response and its maximum random pitch component is
  approximately +/-2.6 cents, alongside a noise layer
  (`icy_beauty.cpp:1217`, `icy_beauty.cpp:1270-1277`,
  `icy_beauty.cpp:1298-1306`).
- The documented Release table follows `0.08 + 12 * value^2` seconds: 0.08,
  0.83, 3.08, 5.15, 6.83, and 12.08 seconds at 0%, 25%, 50%, 65%, 75%, and
  100% respectively (`icy_beauty.cpp:1218-1220`).
- The output defaults to Output 1 in Add mode. Replace mode overwrites the
  selected bus with Icy Beauty's sample (`icy_beauty.cpp:160-164`,
  `icy_beauty.cpp:1201-1204`, `icy_beauty.cpp:1334-1335`).
- The live GitHub release is `v0.6.0` and includes an asset named
  `icy_beauty-plugin.zip`. The release workflow constructs that archive with
  `programs/plug-ins/icy_beauty.o`, so unzipping at the SD-card root produces
  the documented path (`.github/workflows/release.yaml:39-46`).
- The repository's MIT Licence is copyright 2026 Neal Sanche (`LICENSE:1-21`).

## Supporting documentation checked

- `DEPENDENCIES.md` names the exact checked-out API revision
  `cd12d876dbe060859828053efab1cbc98c9df251`, whose licence is MIT. Its
  PortAudio, CoreMIDI/CoreFoundation, FFmpeg/FFprobe, 12-channel capture, and
  48 kHz 24-bit WAV claims match `scripts/target_midi_latency.py` and
  `scripts/portaudio_coremidi_latency.c`.
- `ASSETS.md` correctly separates development fixtures from the release
  payload. The release workflow packages only
  `programs/plug-ins/icy_beauty.o`.
- `TARGET_VALIDATION.md` correctly distinguishes historical `NsIb` evidence
  from the public `ThIb` identity. Its retained endurance and latency figures
  match the JSON evidence files, while the current harness source and tests
  target `ThIb`.

## Primary sources

- [Expert Sleepers disting NT user manual 1.15](https://www.expert-sleepers.co.uk/downloads/manuals/disting_NT_user_manual_1.15.pdf) — official plug-in installation/loading and parameter-mapping behavior.
- [Expert Sleepers disting NT firmware updates](https://www.expert-sleepers.co.uk/distingNTfirmwareupdates.html) — official API v13 introduction in firmware 1.15.0, bus expansion, and reset/rescan history.
- [Expert Sleepers distingNT API](https://github.com/expertsleepersltd/distingNT_API) — official C++ API; this repository pins commit `cd12d876dbe060859828053efab1cbc98c9df251`.
- [Icy Beauty v0.6.0 release](https://github.com/thorinside/icy_beauty/releases/tag/v0.6.0) — live release metadata and the `icy_beauty-plugin.zip` asset.
- [MIDI Association expanded MIDI 1.0 message list](https://midi.org/expanded-midi-1-0-messages-list) — official names and status bytes for Stop and System Reset.
- [MIDI Association MIDI 1.0 Control Change table](https://midi.org/midi-1-0-control-change-messages) — official CC 1, CC 64, CC 120, and CC 123 names and value conventions.
- [MIDI Association discussion of octave naming](https://midi.org/community/midi-specifications/midi-octave-and-note-numbering-standard) — official clarification that MIDI note number 60 is fixed while C3/C4/C5 octave labels vary by convention.

## Verification runs

`make test` passed on the audit baseline, including the factory surface,
dynamic Count bounds, MIDI/CV ownership, gate hysteresis, sample-and-hold,
declicked CV retrigger, 1 V/oct calibration, pitch offsets, adjacent routing,
panic reset, six-group operation, voice reuse, release completion, and the
inside-5-V output ceiling.

After applying the documentation corrections, `make verify` passed the clean
ARM build and inspection, native test suite, and all 27 Python tests. Both HTML
guides also passed balanced-tag, unique-ID, and local-link checks; the Markdown
patch table passed its column-count check; `git diff --check` passed; and the
superseded factual wording scan returned no matches.
