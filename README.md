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
| **Voices** | 4 | Chooses 1–8 shared voices when the algorithm is added |
| **MIDI channel** | Omni | Responds to every MIDI channel, or one selected channel from 1–16 |
| **Output** | Output 1 | Chooses the destination bus |
| **Output mode** | Add | Adds Icy Beauty to the bus; **Replace** overwrites the bus instead |
| **Gate** | Input 1 | Shared gate input for CV control |
| **Pitch 1…8** | Inputs 2…9 | One 1 V/octave pitch input for each configured voice |

MIDI and CV use the same voice pool. If all voices are busy, the algorithm
reuses a released voice first, then the oldest held voice.

## MIDI control

| Control | Response |
| --- | --- |
| Note velocity | Changes level and adds a small amount of brightness |
| Pitch bend | Fixed ±2-semitone range |
| Modulation wheel (CC 1) | Increases Motion |
| Sustain pedal (CC 64) | Holds released notes until the pedal is lifted |
| Polyphonic aftertouch | Raises Resonance and adds a smaller amount of Motion to one note |
| Channel pressure | Applies the same pressure response across the chord |

## CV/gate control

A rising Gate plays all configured CV voices using the current Pitch inputs. A
falling Gate releases them. Pitch follows 1 V/octave.

With four voices, the default inputs are:

| Input | Function |
| --- | --- |
| Input 1 | Gate |
| Input 2 | Pitch 1 |
| Input 3 | Pitch 2 |
| Input 4 | Pitch 3 |
| Input 5 | Pitch 4 |

Choosing more voices adds the matching Pitch inputs, up to Pitch 8 on Input 9.
Held MIDI notes take priority if MIDI and CV need the same voice.

## Output level

The algorithm produces a proper bipolar Eurorack signal:

- clean response through **±4.5 V**;
- smooth limiting above that level; and
- a maximum synth output inside **±5 V** (**10 Vpp**).

In **Add** mode, the ±5 V limit applies to Icy Beauty before it is added to the
signal already on the bus.

## Install

1. Download `icy_beauty-plugin.zip` from the latest
   [GitHub release](https://github.com/thorinside/icy_beauty/releases).
2. Unzip it at the root of the disting NT SD card.
3. Rescan plug-ins and add **Icy Beauty**.

The archive installs `icy_beauty.o` at
`/programs/plug-ins/icy_beauty.o`. No compiler or development toolchain is
required.

The public plug-in GUID is `ThIb`. If a preset used the earlier development
GUID `NsIb`, remove that old algorithm and add Icy Beauty again.

## Licence

Icy Beauty’s source code and documentation are available under the
[MIT Licence](LICENSE), © 2026 Neal Sanche. It is an independent third-party
project and is not affiliated with or endorsed by Expert Sleepers.
