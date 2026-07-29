#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11,<3.12"
# dependencies = [
#   "basic-pitch[onnx]==0.4.0",
#   "librosa==0.11.0",
#   "matplotlib==3.11.1",
#   "setuptools<81",
#   "soundfile==0.14.0",
# ]
# ///
"""Extract the exposed Icy Beauty phrase from the owner-supplied reference.

The beginning of the recording contains overlapping synth and string material.
This analysis deliberately selects the four large energy attacks after 12.5
seconds, then uses both a polyphonic transcription model and background-
subtracted spectral measurements. The inferred notes are development targets,
not a claim that the mixed recording contains isolated ground-truth stems.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import logging
import math
import tempfile
import warnings
from pathlib import Path

import librosa
import matplotlib
import numpy as np
import soundfile as sf
from scipy.signal import find_peaks, welch

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


EXPECTED_SHA256 = (
    "e19a959009e8045a9be01a132b8168f971930465d60e4fc8e1820babfcf26d5a"
)
ANALYSIS_VERSION = 1
FOCUS_START_SECONDS = 12.5
EVENT_COUNT = 4
RMS_FRAME_LENGTH = 2048
RMS_HOP_LENGTH = 256
RMS_SLOPE_LAG_FRAMES = 4
CQT_LOWEST_MIDI = 24
CQT_BIN_COUNT = 72
ROOT_MIN_MIDI = 43
ROOT_MAX_MIDI = 59


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyze the four exposed notes in model/cononical.wav."
    )
    parser.add_argument(
        "--reference",
        type=Path,
        default=Path("model/cononical.wav"),
        help="owner-supplied stereo WAV (default: model/cononical.wav)",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("analysis/reference"),
        help="directory for retained JSON, CSV, and plot evidence",
    )
    return parser.parse_args()


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def note_name(midi_note: int) -> str:
    names = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
    return f"{names[midi_note % 12]}{midi_note // 12 - 1}"


def midi_frequency(midi_note: int) -> float:
    return 440.0 * 2.0 ** ((midi_note - 69) / 12.0)


def rms(signal: np.ndarray) -> float:
    if signal.size == 0:
        return 0.0
    return float(np.sqrt(np.mean(np.square(signal, dtype=np.float64))))


def ratio_db(numerator: float, denominator: float) -> float:
    return 20.0 * math.log10(max(numerator, 1.0e-12) / max(denominator, 1.0e-12))


def rounded(value: float, digits: int = 3) -> float:
    return round(float(value), digits)


def find_strong_events(mono: np.ndarray, sample_rate: int) -> tuple[list[float], dict]:
    frame_rms = librosa.feature.rms(
        y=mono,
        frame_length=RMS_FRAME_LENGTH,
        hop_length=RMS_HOP_LENGTH,
        center=True,
    )[0]
    frame_db = librosa.amplitude_to_db(frame_rms, ref=1.0, top_db=None)
    slope = np.full_like(frame_db, -120.0)
    slope[RMS_SLOPE_LAG_FRAMES:] = (
        frame_db[RMS_SLOPE_LAG_FRAMES:] - frame_db[:-RMS_SLOPE_LAG_FRAMES]
    )
    times = librosa.frames_to_time(
        np.arange(slope.size), sr=sample_rate, hop_length=RMS_HOP_LENGTH
    )
    eligible = times >= FOCUS_START_SECONDS
    minimum_distance = max(1, int(0.75 * sample_rate / RMS_HOP_LENGTH))
    peak_indices, _ = find_peaks(
        np.where(eligible, slope, -120.0),
        distance=minimum_distance,
        prominence=2.0,
    )
    if peak_indices.size < EVENT_COUNT:
        raise RuntimeError(
            f"found only {peak_indices.size} strong events after "
            f"{FOCUS_START_SECONDS:.1f}s"
        )
    strongest = sorted(
        peak_indices, key=lambda index: float(slope[index]), reverse=True
    )[:EVENT_COUNT]
    selected = sorted(strongest)
    event_times = [float(times[index]) for index in selected]
    diagnostics = {
        "frame_times": times,
        "frame_db": frame_db,
        "slope_db": slope,
        "selected_indices": selected,
    }
    return event_times, diagnostics


def basic_pitch_transcription(
    mono: np.ndarray, sample_rate: int, focus_start: float
) -> list[dict]:
    # Basic Pitch's ONNX extra avoids TensorFlow and CoreML platform coupling.
    logging.getLogger().setLevel(logging.ERROR)
    warnings.filterwarnings("ignore", category=UserWarning)
    import basic_pitch  # noqa: PLC0415
    from basic_pitch.inference import Model, predict  # noqa: PLC0415

    model_path = (
        Path(basic_pitch.__file__).resolve().parent
        / "saved_models"
        / "icassp_2022"
        / "nmp.onnx"
    )
    if not model_path.is_file():
        raise RuntimeError(f"Basic Pitch ONNX model is missing: {model_path}")

    focus_sample = int(focus_start * sample_rate)
    with tempfile.TemporaryDirectory(prefix="icy-beauty-reference-") as temporary:
        audio_path = Path(temporary) / "strong-notes.wav"
        sf.write(audio_path, mono[focus_sample:], sample_rate, subtype="PCM_24")
        _, _, note_events = predict(
            audio_path,
            Model(model_path),
            onset_threshold=0.45,
            frame_threshold=0.30,
            minimum_note_length=80.0,
            minimum_frequency=55.0,
            maximum_frequency=2500.0,
        )

    transcription = []
    for start, end, pitch, confidence, _pitch_bends in note_events:
        transcription.append(
            {
                "start_seconds": float(start + focus_start),
                "end_seconds": float(end + focus_start),
                "midi": int(pitch),
                "note": note_name(int(pitch)),
                "confidence": float(confidence),
            }
        )
    return sorted(
        transcription,
        key=lambda note: (note["start_seconds"], note["midi"], note["end_seconds"]),
    )


def cqt_root_evidence(
    mono: np.ndarray, sample_rate: int, event_times: list[float]
) -> list[dict]:
    power = np.abs(
        librosa.cqt(
            y=mono,
            sr=sample_rate,
            hop_length=RMS_HOP_LENGTH,
            fmin=librosa.midi_to_hz(CQT_LOWEST_MIDI),
            n_bins=CQT_BIN_COUNT,
            bins_per_octave=12,
        )
    ) ** 2
    times = librosa.frames_to_time(
        np.arange(power.shape[1]), sr=sample_rate, hop_length=RMS_HOP_LENGTH
    )
    root_evidence = []
    for event_time in event_times:
        before = (times >= event_time - 0.15) & (times < event_time - 0.02)
        after = (times >= event_time + 0.01) & (times < event_time + 0.25)
        gain_db = 10.0 * np.log10(
            (np.mean(power[:, after], axis=1) + 1.0e-12)
            / (np.mean(power[:, before], axis=1) + 1.0e-12)
        )
        root_midis = np.arange(ROOT_MIN_MIDI, ROOT_MAX_MIDI + 1)
        root_bins = root_midis - CQT_LOWEST_MIDI
        ranked = sorted(
            root_midis,
            key=lambda midi_note: float(gain_db[midi_note - CQT_LOWEST_MIDI]),
            reverse=True,
        )
        root = int(ranked[0])
        alternatives = [
            {
                "midi": int(midi_note),
                "note": note_name(int(midi_note)),
                "added_power_db": rounded(
                    gain_db[int(midi_note) - CQT_LOWEST_MIDI], 2
                ),
            }
            for midi_note in ranked[:3]
        ]
        root_evidence.append(
            {
                "midi": root,
                "note": note_name(root),
                "frequency_hz": rounded(midi_frequency(root), 3),
                "added_power_db": rounded(gain_db[root_bins[root - ROOT_MIN_MIDI]], 2),
                "alternatives": alternatives,
            }
        )
    return root_evidence


def event_envelope_features(
    mono: np.ndarray, sample_rate: int, event_time: float
) -> dict:
    window_samples = max(1, int(round(0.005 * sample_rate)))
    smoothed = np.sqrt(
        np.convolve(
            np.square(mono, dtype=np.float64),
            np.ones(window_samples, dtype=np.float64) / window_samples,
            mode="same",
        )
    )
    event_sample = int(round(event_time * sample_rate))
    before = smoothed[
        max(0, event_sample - int(0.12 * sample_rate)) : max(
            1, event_sample - int(0.02 * sample_rate)
        )
    ]
    after = smoothed[
        event_sample : min(smoothed.size, event_sample + int(0.25 * sample_rate))
    ]
    baseline = float(np.median(before))
    peak_offset = int(np.argmax(after))
    peak = float(after[peak_offset])
    low_threshold = baseline + 0.10 * (peak - baseline)
    high_threshold = baseline + 0.90 * (peak - baseline)
    search_start = max(0, event_sample - int(0.02 * sample_rate))
    search = smoothed[search_start : event_sample + peak_offset + 1]
    low_crossings = np.flatnonzero(search >= low_threshold)
    high_crossings = np.flatnonzero(search >= high_threshold)
    attack_ms = 0.0
    if low_crossings.size and high_crossings.size:
        attack_ms = (
            (int(high_crossings[0]) - int(low_crossings[0]))
            * 1000.0
            / sample_rate
        )

    def local_rms(start_offset: float, duration: float) -> float:
        start = max(0, event_sample + int(start_offset * sample_rate))
        end = min(mono.size, start + int(duration * sample_rate))
        return rms(mono[start:end])

    pre_rms = local_rms(-0.12, 0.12)
    initial_rms = local_rms(0.0, 0.20)
    return {
        "energy_step_db": rounded(ratio_db(initial_rms, pre_rms), 2),
        "attack_10_to_90_ms": rounded(attack_ms, 2),
        "peak_delay_ms": rounded(peak_offset * 1000.0 / sample_rate, 2),
        "rms_0_to_200ms_dbfs": rounded(
            20.0 * math.log10(max(initial_rms, 1.0e-12)), 2
        ),
        "rms_500_to_700ms_relative_db": rounded(
            ratio_db(local_rms(0.50, 0.20), initial_rms), 2
        ),
        "rms_900_to_1100ms_relative_db": rounded(
            ratio_db(local_rms(0.90, 0.20), initial_rms), 2
        ),
    }


def event_spectral_features(
    mono: np.ndarray, sample_rate: int, event_time: float, root_midi: int
) -> dict:
    analysis_duration = 0.35
    event_sample = int(round(event_time * sample_rate))
    duration_samples = int(round(analysis_duration * sample_rate))
    before_end = max(0, event_sample - int(0.03 * sample_rate))
    before = mono[max(0, before_end - duration_samples) : before_end]
    after_start = min(mono.size, event_sample + int(0.03 * sample_rate))
    after = mono[after_start : min(mono.size, after_start + duration_samples)]
    common_length = min(before.size, after.size)
    if common_length < 4096:
        raise RuntimeError("reference event is too close to an audio boundary")
    before = before[-common_length:]
    after = after[:common_length]

    frequencies, before_power = welch(
        before,
        sample_rate,
        window="hann",
        nperseg=4096,
        noverlap=3072,
        nfft=16384,
    )
    _, after_power = welch(
        after,
        sample_rate,
        window="hann",
        nperseg=4096,
        noverlap=3072,
        nfft=16384,
    )
    added_power = np.maximum(after_power - before_power, 0.0)
    audible = (frequencies >= 40.0) & (frequencies <= 8000.0)
    audible_power = float(np.sum(added_power[audible]))
    if audible_power <= 1.0e-20:
        raise RuntimeError(f"no added spectral power near {event_time:.3f}s")

    fundamental = midi_frequency(root_midi)
    harmonic_powers = []
    for harmonic in range(1, 9):
        frequency = harmonic * fundamental
        half_width = max(8.0, frequency * 0.025)
        band = np.abs(frequencies - frequency) <= half_width
        harmonic_powers.append(float(np.sum(added_power[band])))
    fundamental_power = max(harmonic_powers[0], 1.0e-20)
    harmonic_relative_db = [
        rounded(10.0 * math.log10(max(power, 1.0e-20) / fundamental_power), 2)
        for power in harmonic_powers
    ]
    spectral_centroid = float(
        np.sum(frequencies[audible] * added_power[audible]) / audible_power
    )
    cumulative = np.cumsum(added_power[audible])
    audible_frequencies = frequencies[audible]
    rolloff_index = int(np.searchsorted(cumulative, 0.85 * cumulative[-1]))
    rolloff = float(audible_frequencies[min(rolloff_index, cumulative.size - 1)])

    frequency_resolution = float(frequencies[1] - frequencies[0])
    pitch_search_half_width = max(
        frequency_resolution, fundamental * (2.0 ** (0.5 / 12.0) - 1.0)
    )
    fundamental_search = (
        np.abs(frequencies - fundamental) <= pitch_search_half_width
    )
    search_indices = np.flatnonzero(fundamental_search)
    peak_frequency = float(
        frequencies[
            search_indices[
                int(np.argmax(added_power[fundamental_search]))
            ]
        ]
    )
    pitch_offset_cents = 1200.0 * math.log2(peak_frequency / fundamental)

    harmonic_fraction = min(1.0, sum(harmonic_powers) / audible_power)
    return {
        "measured_fundamental_hz": rounded(peak_frequency, 3),
        "pitch_offset_cents": rounded(pitch_offset_cents, 2),
        "added_spectral_centroid_hz": rounded(spectral_centroid, 1),
        "added_85_percent_rolloff_hz": rounded(rolloff, 1),
        "harmonics_relative_db": harmonic_relative_db,
        "harmonic_band_power_fraction": rounded(harmonic_fraction, 4),
        "non_harmonic_or_layer_power_fraction": rounded(
            1.0 - harmonic_fraction, 4
        ),
    }


def event_stereo_features(
    stereo: np.ndarray, sample_rate: int, event_time: float
) -> dict:
    start = int(round(event_time * sample_rate))
    end = min(stereo.shape[0], start + int(0.35 * sample_rate))
    segment = stereo[start:end]
    left = segment[:, 0]
    right = segment[:, 1]
    correlation = float(np.corrcoef(left, right)[0, 1])
    mid = 0.5 * (left + right)
    side = 0.5 * (left - right)
    return {
        "left_right_correlation": rounded(correlation, 4),
        "side_to_mid_db": rounded(ratio_db(rms(side), rms(mid)), 2),
    }


def nearby_transcribed_notes(
    transcription: list[dict], event_time: float
) -> list[dict]:
    nearby = [
        note
        for note in transcription
        if event_time - 0.035 <= note["start_seconds"] <= event_time + 0.045
    ]
    return [
        {
            "midi": note["midi"],
            "note": note["note"],
            "start_seconds": rounded(note["start_seconds"], 3),
            "duration_seconds": rounded(
                note["end_seconds"] - note["start_seconds"], 3
            ),
            "confidence": rounded(note["confidence"], 3),
        }
        for note in sorted(nearby, key=lambda item: item["midi"])
    ]


def build_report(
    reference_path: Path,
    stereo: np.ndarray,
    sample_rate: int,
    event_times: list[float],
    diagnostics: dict,
    transcription: list[dict],
    roots: list[dict],
) -> dict:
    mono = np.mean(stereo, axis=1)
    selected_indices = diagnostics["selected_indices"]
    events = []
    for index, (event_time, root) in enumerate(zip(event_times, roots), start=1):
        events.append(
            {
                "index": index,
                "time_seconds": rounded(event_time, 3),
                "onset_slope_db": rounded(
                    diagnostics["slope_db"][selected_indices[index - 1]], 2
                ),
                "inferred_root": root,
                "polyphonic_notes_near_onset": nearby_transcribed_notes(
                    transcription, event_time
                ),
                "envelope": event_envelope_features(
                    mono, sample_rate, event_time
                ),
                "spectrum": event_spectral_features(
                    mono, sample_rate, event_time, root["midi"]
                ),
                "stereo": event_stereo_features(
                    stereo, sample_rate, event_time
                ),
            }
        )

    intervals = np.diff(event_times)
    shortest = float(np.min(intervals))
    root_sequence = [event["inferred_root"]["note"] for event in events]
    report = {
        "analysis_version": ANALYSIS_VERSION,
        "source": {
            "path": reference_path.as_posix(),
            "sha256": file_sha256(reference_path),
            "duration_seconds": rounded(stereo.shape[0] / sample_rate, 3),
            "sample_rate_hz": sample_rate,
            "channels": int(stereo.shape[1]),
        },
        "selection": {
            "focus_start_seconds": FOCUS_START_SECONDS,
            "reason": (
                "The opening contains overlapping synth and string material; "
                "the four strongest later energy attacks are the requested target."
            ),
            "method": (
                "Top four peaks in the 21.33 ms log-RMS rise after 12.5 s, "
                "separated by at least 0.75 s."
            ),
        },
        "phrase": {
            "root_sequence": root_sequence,
            "root_midi_sequence": [
                event["inferred_root"]["midi"] for event in events
            ],
            "onset_intervals_seconds": [rounded(value, 3) for value in intervals],
            "normalized_onset_intervals": [
                rounded(value / shortest, 3) for value in intervals
            ],
        },
        "events": events,
        "interpretation": {
            "tonal_field": "B minor / D major",
            "sound_target": (
                "The newly exposed low-note layer is fundamental-forward and "
                "dark, with changing upper partial or chord content and a "
                "clearly audible stereo production layer."
            ),
            "caveat": (
                "This is a mixed stereo excerpt, not an isolated stem. Root "
                "notes are inferred from background-subtracted CQT power and "
                "cross-checked with Basic Pitch; upper notes and non-harmonic "
                "power can include the overlapping strings and prior tails."
            ),
        },
        "transcription": {
            "model": "Basic Pitch 0.4.0 ONNX",
            "settings": {
                "onset_threshold": 0.45,
                "frame_threshold": 0.30,
                "minimum_note_length_ms": 80.0,
                "minimum_frequency_hz": 55.0,
                "maximum_frequency_hz": 2500.0,
            },
            "notes": [
                {
                    "start_seconds": rounded(note["start_seconds"], 3),
                    "end_seconds": rounded(note["end_seconds"], 3),
                    "midi": note["midi"],
                    "note": note["note"],
                    "confidence": rounded(note["confidence"], 3),
                }
                for note in transcription
                if note["end_seconds"] >= FOCUS_START_SECONDS
            ],
        },
    }
    return report


def write_event_csv(path: Path, report: dict) -> None:
    fieldnames = [
        "event",
        "time_seconds",
        "root_midi",
        "root_note",
        "onset_slope_db",
        "energy_step_db",
        "attack_ms",
        "centroid_hz",
        "rolloff_hz",
        "side_to_mid_db",
        "polyphonic_notes",
    ]
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fieldnames)
        writer.writeheader()
        for event in report["events"]:
            writer.writerow(
                {
                    "event": event["index"],
                    "time_seconds": event["time_seconds"],
                    "root_midi": event["inferred_root"]["midi"],
                    "root_note": event["inferred_root"]["note"],
                    "onset_slope_db": event["onset_slope_db"],
                    "energy_step_db": event["envelope"]["energy_step_db"],
                    "attack_ms": event["envelope"]["attack_10_to_90_ms"],
                    "centroid_hz": event["spectrum"][
                        "added_spectral_centroid_hz"
                    ],
                    "rolloff_hz": event["spectrum"][
                        "added_85_percent_rolloff_hz"
                    ],
                    "side_to_mid_db": event["stereo"]["side_to_mid_db"],
                    "polyphonic_notes": " ".join(
                        note["note"]
                        for note in event["polyphonic_notes_near_onset"]
                    ),
                }
            )


def write_plot(
    path: Path,
    mono: np.ndarray,
    sample_rate: int,
    diagnostics: dict,
    report: dict,
) -> None:
    figure, axes = plt.subplots(3, 1, figsize=(12, 9), constrained_layout=True)
    sample_step = max(1, sample_rate // 500)
    sample_times = np.arange(0, mono.size, sample_step) / sample_rate
    axes[0].plot(sample_times, mono[::sample_step], color="#315d8a", linewidth=0.7)
    axes[0].axvspan(0.0, FOCUS_START_SECONDS, color="#b7b7b7", alpha=0.25)
    axes[0].set(
        title="Owner reference: opening context excluded, exposed notes selected",
        ylabel="Amplitude",
        xlim=(0.0, mono.size / sample_rate),
    )

    frame_times = diagnostics["frame_times"]
    focus_frames = frame_times >= FOCUS_START_SECONDS
    axes[1].plot(
        frame_times[focus_frames],
        diagnostics["frame_db"][focus_frames],
        color="#555555",
        linewidth=0.8,
        label="RMS",
    )
    axes[1].plot(
        frame_times[focus_frames],
        diagnostics["slope_db"][focus_frames],
        color="#be4d25",
        linewidth=0.8,
        label="21.33 ms rise",
    )
    axes[1].set(
        title="Energy evidence",
        ylabel="dB / dB rise",
        xlim=(FOCUS_START_SECONDS, mono.size / sample_rate),
    )
    axes[1].legend(loc="upper right")

    for event in report["events"]:
        event_time = event["time_seconds"]
        for axis in axes[:2]:
            axis.axvline(event_time, color="#9e2f2f", linewidth=0.9)
        axes[0].text(
            event_time,
            0.95,
            f'{event["index"]}: {event["inferred_root"]["note"]}',
            rotation=90,
            ha="right",
            va="top",
            fontsize=8,
            transform=axes[0].get_xaxis_transform(),
        )

    harmonic_numbers = np.arange(1, 9)
    for event in report["events"]:
        axes[2].plot(
            harmonic_numbers,
            event["spectrum"]["harmonics_relative_db"],
            marker="o",
            linewidth=1.0,
            label=f'{event["index"]}: {event["inferred_root"]["note"]}',
        )
    axes[2].axhline(0.0, color="#777777", linewidth=0.6)
    axes[2].set(
        title="Background-subtracted harmonic structure",
        xlabel="Harmonic",
        ylabel="Level relative to fundamental (dB)",
        xticks=harmonic_numbers,
        ylim=(-60.0, 6.0),
    )
    axes[2].legend(loc="upper right")
    figure.savefig(path, dpi=160)
    plt.close(figure)


def main() -> int:
    args = parse_args()
    reference_path = args.reference
    if not reference_path.is_file():
        raise SystemExit(f"reference WAV does not exist: {reference_path}")
    source_hash = file_sha256(reference_path)
    if source_hash != EXPECTED_SHA256:
        raise SystemExit(
            "reference WAV hash changed: "
            f"expected {EXPECTED_SHA256}, received {source_hash}"
        )

    stereo, sample_rate = sf.read(
        reference_path, dtype="float32", always_2d=True
    )
    if sample_rate != 48000 or stereo.shape[1] != 2:
        raise SystemExit(
            f"expected 48 kHz stereo reference, received "
            f"{sample_rate} Hz / {stereo.shape[1]} channels"
        )
    mono = np.mean(stereo, axis=1)
    event_times, diagnostics = find_strong_events(mono, sample_rate)
    transcription = basic_pitch_transcription(
        mono, sample_rate, FOCUS_START_SECONDS
    )
    roots = cqt_root_evidence(mono, sample_rate, event_times)
    report = build_report(
        reference_path,
        stereo,
        sample_rate,
        event_times,
        diagnostics,
        transcription,
        roots,
    )

    args.output_dir.mkdir(parents=True, exist_ok=True)
    json_path = args.output_dir / "strong-note-analysis.json"
    csv_path = args.output_dir / "strong-note-events.csv"
    plot_path = args.output_dir / "strong-note-analysis.png"
    json_path.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    write_event_csv(csv_path, report)
    write_plot(plot_path, mono, sample_rate, diagnostics, report)

    phrase = " -> ".join(report["phrase"]["root_sequence"])
    intervals = ", ".join(
        f"{value:.3f}" for value in report["phrase"]["onset_intervals_seconds"]
    )
    print(f"PASS: reference {source_hash[:12]}... analyzed")
    print(f"PASS: exposed phrase {phrase}; intervals {intervals} seconds")
    print(f"PASS: retained {json_path}, {csv_path}, and {plot_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
