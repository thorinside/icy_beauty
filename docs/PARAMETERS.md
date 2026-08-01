# Icy Beauty parameter field guide

Icy Beauty has five sound controls. Each one always changes the same part of
the algorithm, so it is easy to learn by ear and easy to perform.

## Quick reference

| Control | 0% | 50% | 100% |
| --- | --- | --- | --- |
| Tone | dark and rounded | balanced | bright and layered |
| Motion | still | slow drift | animated ±13-cent movement |
| Grain | clean | fine texture | rough pitch texture and noise |
| Resonance | dry | tuned sheen | strong glass-like ring |
| Release | about 80 ms | about 3.08 s | about 12.08 s |

## Tone

Tone changes both brightness and the balance of the three tone layers.

- The main note stays present across the full range.
- Raising Tone brings up both the near-octave and high-interval layers.
- The high interval grows more strongly across the range.
- The overall sound also becomes less smoothed.

Use low settings for soft basses and woody chords. Use the middle for the
balanced factory sound. Use high settings for bright bells and glass-like
leads.

Tone and Resonance work well as a pair: Tone adds harmonic detail, while
Resonance turns some of that detail into a clearer ring.

## Motion

Motion gives every active voice its own slow pitch movement. The voices do not
move together, so held chords gently spread and close. The values below assume
the modulation wheel and pressure are at zero.

| Setting | Approximate result |
| ---: | --- |
| 0% | no slow pitch movement |
| 25% | very gentle drift |
| 50% | about ±6.5 cents around 0.54 Hz |
| 75% | clearly animated ensemble movement |
| 100% | about ±13 cents around 1 Hz |

This is pitch movement, not a chorus or delay effect. The modulation wheel
raises Motion toward its maximum. Aftertouch adds a smaller amount.

## Grain

Grain adds two kinds of fast, irregular detail:

1. tiny random changes in pitch; and
2. a noise layer mixed into the voice.

The lower half is intentionally subtle. The upper half changes more quickly,
making it easier to find fine frost near the middle without losing the rougher
sounds at the top. At 100%, the random pitch part is about ±2.6 cents.

Motion and Grain have different jobs:

- **Motion** is slow and smooth.
- **Grain** is fast and uneven.

Try adding Grain to a low Tone setting for a weathered, breathy sound. At high
Tone settings, Grain makes the bright layers feel more brittle.

## Resonance

Resonance mixes in a tuned ring that follows every played pitch.

The ranges below assume pressure is at zero; aftertouch can raise Resonance
above the panel setting.

- Below 30%, it adds a small outline to the note.
- Around 50%, it gives the factory sound its clear icy edge.
- Above 70%, the ring becomes an obvious part of the sound.

It does not run away into self-oscillation. High values remain connected to the
played note.

Polyphonic aftertouch raises Resonance on the pressed note. Channel pressure
raises it on every active note on that MIDI channel.

## Release

Release sets the time it takes a note to fade after its key or gate is released.
The lower half gives more precision to short and medium sounds; the upper half
opens into long tails.

| Setting | Programmed fade |
| ---: | ---: |
| 0% | 0.08 s |
| 25% | 0.83 s |
| 50% | 3.08 s |
| 65% | 5.15 s |
| 75% | 6.83 s |
| 100% | 12.08 s |

The audible tail depends on Tone and Resonance. Higher Resonance usually makes
a long release easier to hear.

## Voices

Choose the voice count when adding the algorithm. The range is 1–16 and the
default is 8.

| Voices | Useful for |
| ---: | --- |
| 1 | bass, melody, or an external arpeggiator |
| 2 | intervals and duophonic lines |
| 4 | compact chords |
| 6 | extended chords |
| 8 | the balanced default |
| 12 | dense chords with room for several CV voices |
| 16 | maximum MIDI polyphony or large CV partitions |

MIDI and CV share these voices. Output gain is adjusted for the selected count
so that adding voices does not cause a large level jump.

## CV/gate groups

Choose 0–6 gate groups when adding the algorithm; the default is 4. Each group
adds **Gate input N**, **Gate N CV count**, and **Gate N sample & hold**. On a
fresh instance every Gate input is **None** and every Count is **0**, so all
voices remain available to MIDI until CV control is explicitly connected.

The pitch inputs are the buses immediately after the selected gate. If Gate 1
uses Input 9 with Count 3, its pitches are Inputs 10, 11, and 12. Each Count is
limited to 11 and may be further limited by the voices left after the other
groups' reservations and the available bus positions after the selected gate
bus.

The gate rises above 1.0 V and falls below 0.5 V. Sample & hold **Off** tracks
pitch continuously while the gate is high; **On** captures all pitches on the
rising edge. Increasing a Count under a held gate waits for the next edge.
Decreasing a Count quickly fades the removed voices with an internal 5 ms
transition, independent of Release. A retrigger during an audible release tail
resumes smoothly from the current level.

Gate groups reserve fixed voice partitions in group order. MIDI uses only the
unreserved remainder. Incoming MIDI and CV notes do not steal from the other
partition, but editing Counts can reallocate the pool and quickly fade notes
that no longer fit.

Pitch follows 1 V/octave with 0 V at MIDI note 48 (about 130.81 Hz). At 48 kHz,
the calibration is verified from −5 V through +7 V to within 0.01 cent.
Pitch CV is clamped to the −8 V to +8 V range, and extreme positive values are
also limited below Nyquist, so the usable top end is lower at reduced sample
rates.

## Performance controls

| Gesture | What it controls |
| --- | --- |
| Note velocity | level, with a smaller Tone-like brightness change |
| Pitch bend | pitch, ±2 semitones |
| Modulation wheel | Motion |
| Sustain pedal | holds released notes until pedal-up, unless a voice must be reused |
| Polyphonic aftertouch | per-note Resonance and a smaller amount of Motion |
| Channel pressure | Resonance and a smaller amount of Motion on that MIDI channel |
| MIDI Stop / System Reset | clears every MIDI and CV voice |
| CC 120 / CC 123 | All Sound Off / All Notes Off on an accepted channel; clears every voice |

These assignments are fixed. The five sound parameters can still be mapped to
CV using the disting NT host.

The Setup page also has two global pitch offsets. **Semitones** shifts every
MIDI and CV voice from −11 to +11 semitones, and **Octaves** shifts them from
−4 to +4 octaves. Both default to zero and update held notes immediately. With
Sample & hold enabled, the sampled input CV remains held while either global
offset can still be changed.

## Four starting patches

| Patch | Tone | Motion | Grain | Resonance | Release | Sound |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| **Aurora Glass** | 72 | 58 | 18 | 64 | 78 | bright layers with slow movement and a long tail |
| **Frozen Prairie** | 38 | 32 | 24 | 42 | 70 | broad, restrained, and slightly weathered |
| **Red Cedar Bell** | 55 | 22 | 8 | 82 | 60 | warm body with a clear tuned ring |
| **Polar Night** | 20 | 70 | 45 | 68 | 92 | dark, moving, grainy, and spacious |

For simple external control, map a slow CV to Tone and an envelope to
Resonance. Keep Grain under a knob at first; small changes around the midpoint
are easy to hear and useful in performance.
