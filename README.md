# Icy Beauty

<p align="center">
  <img src="docs/icy-beauty-header.svg" alt="Icy Beauty — a northern polyphonic voice for disting NT" width="100%">
</p>

<p align="center">
  <strong>A northern voice / une voix du Nord</strong><br>
  Canadian breadth. Nordic restraint. Eurorack voltage.
</p>

<p align="center">
  <a href="LICENSE"><img alt="MIT licence" src="https://img.shields.io/badge/licence-MIT-cb2b2b"></a>
  <img alt="disting NT API v13" src="https://img.shields.io/badge/disting%20NT-API%20v13-17324d">
  <img alt="Plugin GUID ThIb" src="https://img.shields.io/badge/GUID-ThIb-3a8274">
  <img alt="Bipolar five volt output" src="https://img.shields.io/badge/output-%C2%B15%20V-e9b949">
</p>

Icy Beauty is one focused polyphonic synthesizer algorithm for the
[Expert Sleepers disting NT](https://www.expert-sleepers.co.uk/distingNT.html).
It makes clear, soft-edged tones that can move from warm and woody to bright,
grainy, and glass-like.

## The algorithm

Every note follows the same simple path:

1. A rounded main tone gives the note its body.
2. A quieter octave and a high interval add colour.
3. **Tone** balances those layers and sets the overall brightness.
4. **Motion** adds slow pitch movement; **Grain** adds faster irregular texture.
5. **Resonance** adds a tuned ring that follows the note.
6. **Release** controls how long the note fades after it is released.

```text
main tone + octave + high interval
                │
             Tone
                │
       Motion + Grain
                │
           Resonance
                │
            Release
                │
         Eurorack output
```

There are no alternate synthesis engines or hidden modes. The five sound
parameters always control this signal path.

## Sound parameters

| Parameter | Default | At low settings | At high settings |
| --- | ---: | --- | --- |
| **Tone** | 50% | Rounded, dark, and centred on the main note | Brighter, with more octave and high-interval colour |
| **Motion** | 50% | Stable pitch | Slow independent pitch movement on each voice, up to about ±13 cents |
| **Grain** | 50% | Clean and smooth | Fine random pitch variation plus an audible layer of noise |
| **Resonance** | 50% | Dry and direct | A stronger tuned, glass-like ring |
| **Release** | 65% | Short, plucked notes | Long fades, up to about 12 seconds |

All five are normal disting NT parameters and can be mapped to CV with the
host’s parameter mapping.

The [parameter field guide](docs/PARAMETERS.md) explains the useful ranges,
control interactions, and four starting patches in more detail.

## Setup and routing

| Parameter | Default | Description |
| --- | ---: | --- |
| **Voices** | 8 | Chooses 1–16 shared voices when the algorithm is added |
| **Gate groups** | 4 | Chooses 0–6 independent CV/gate control sets when the algorithm is added |
| **MIDI channel** | Omni | Responds to every MIDI channel, or one selected channel from 1–16 |
| **Semitones** | 0 | Transposes MIDI and CV voices from −11 to +11 semitones |
| **Octaves** | 0 | Transposes MIDI and CV voices from −4 to +4 octaves |
| **Output** | Output 1 | Chooses the destination bus |
| **Output mode** | Add | Adds Icy Beauty to the bus; **Replace** overwrites the bus instead |

A fresh instance is MIDI-only: every Gate input defaults to **None** and every
CV Count defaults to **0**. Raising a Count reserves that many voices for its
gate group. MIDI uses the unreserved voices, and MIDI and CV never steal from
one another.

## MIDI control

| Control | Response |
| --- | --- |
| Note velocity | Changes level and adds a small amount of brightness |
| Pitch bend | Fixed ±2-semitone range |
| Modulation wheel (CC 1) | Increases Motion |
| Sustain pedal (CC 64) | Holds released notes until the pedal is lifted |
| Polyphonic aftertouch | Raises Resonance and adds a smaller amount of Motion to one note |
| Channel pressure | Applies the same pressure response across the chord |
| Stop / panic | MIDI Stop, System Reset, All Sound Off (CC 120), and All Notes Off (CC 123) clear every voice |

## CV/gate control

Each configured gate group exposes three controls:

- **Gate input N** chooses the gate bus.
- **Gate N CV count** reserves 0–11 voices for that group.
- **Gate N sample & hold** chooses tracked or sampled pitch.

Pitch buses follow immediately after the selected gate bus. For example, with
**Gate input 1 = Input 9** and **Gate 1 CV count = 2**:

| Input | Function |
| --- | --- |
| Input 9 | Gate 1 |
| Input 10 | Gate 1 pitch +1 |
| Input 11 | Gate 1 pitch +2 |

Pitch tracks 1 V/octave, with 0 V at C3. With sample & hold **Off**, pitches
track while the gate is high. With it **On**, pitches are captured on each
rising edge and held until the next edge.

Gate detection rises above 1.0 V and falls below 0.5 V. Increasing a Count
while its gate is already high waits for the next rising edge; decreasing a
Count releases the removed voices. Counts are limited by the selected Voices,
the 11-CV group maximum, and the physical buses following the gate input.
A new rising edge during an audible release tail resumes the voice smoothly
from its current level.

After a MIDI stop or panic, a gate that is still high must go low and rise
again before its CV voices restart.

## Install

1. Download `icy_beauty-plugin.zip` from the latest
   [GitHub release](https://github.com/thorinside/icy_beauty/releases).
2. Unzip it at the root of the disting NT SD card.
3. Rescan plug-ins and add **Icy Beauty**.

The archive installs `icy_beauty.o` at
`/programs/plug-ins/icy_beauty.o`. No compiler or development toolchain is
required.

## Licence

Icy Beauty’s source code and documentation are available under the
[MIT Licence](LICENSE), © 2026 Neal Sanche. It is an independent third-party
project and is not affiliated with or endorsed by Expert Sleepers.
