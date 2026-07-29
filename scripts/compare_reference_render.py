#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11,<3.12"
# dependencies = [
#   "librosa==0.11.0",
#   "matplotlib==3.11.1",
#   "soundfile==0.14.0",
# ]
# ///
"""Compare a native Icy Beauty phrase render with the exposed reference notes."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path

import numpy as np
import soundfile as sf

from analyze_reference import (
    event_envelope_features,
    event_spectral_features,
    midi_frequency,
    ratio_db,
    rms,
    rounded,
)


CANDIDATE_EVENT_BLOCKS = (375, 2171, 3079, 4871)
FRAMES_PER_BLOCK = 64
EXPECTED_SAMPLE_RATE = 48000


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Quantify an Icy Beauty phrase render against the reference."
    )
    parser.add_argument("candidate", type=Path, help="mono renderer WAV")
    parser.add_argument(
        "--reference-report",
        type=Path,
        default=Path("analysis/reference/strong-note-analysis.json"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("analysis/candidate/comparison.json"),
    )
    parser.add_argument(
        "--require-match",
        action="store_true",
        help="return nonzero unless every objective model gate passes",
    )
    return parser.parse_args()


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def median(values: list[float]) -> float:
    return float(np.median(np.asarray(values, dtype=np.float64)))


def peak_frequency_near(
    signal: np.ndarray, sample_rate: int, event_time: float, root_midi: int
) -> tuple[float, float]:
    start = int(round((event_time + 0.05) * sample_rate))
    end = min(signal.size, start + int(round(0.30 * sample_rate)))
    segment = signal[start:end]
    window = np.hanning(segment.size)
    transform = np.abs(np.fft.rfft(segment * window, n=65536)) ** 2
    frequencies = np.fft.rfftfreq(65536, 1.0 / sample_rate)
    expected = midi_frequency(root_midi)
    search = (
        (frequencies >= expected * 2.0 ** (-0.5 / 12.0))
        & (frequencies <= expected * 2.0 ** (0.5 / 12.0))
    )
    indices = np.flatnonzero(search)
    peak_index = indices[int(np.argmax(transform[search]))]
    measured = float(frequencies[peak_index])
    cents = 1200.0 * math.log2(measured / expected)
    return measured, cents


def candidate_event_features(
    mono: np.ndarray, sample_rate: int, event_time: float, root_midi: int
) -> dict:
    measured_frequency, pitch_cents = peak_frequency_near(
        mono, sample_rate, event_time, root_midi
    )
    event_sample = int(round(event_time * sample_rate))
    initial = mono[
        event_sample : min(mono.size, event_sample + int(0.20 * sample_rate))
    ]
    return {
        "envelope": event_envelope_features(mono, sample_rate, event_time),
        "spectrum": event_spectral_features(
            mono, sample_rate, event_time, root_midi
        ),
        "pitch": {
            "measured_fundamental_hz": rounded(measured_frequency, 3),
            "offset_cents": rounded(pitch_cents, 2),
        },
        "peak": rounded(
            float(
                np.max(
                    np.abs(
                        mono[
                            event_sample : min(
                                mono.size,
                                event_sample + int(1.0 * sample_rate),
                            )
                        ]
                    )
                )
            ),
            6,
        ),
        "initial_rms_dbfs": rounded(
            20.0 * math.log10(max(rms(initial), 1.0e-12)), 2
        ),
    }


def compare_event(reference: dict, candidate: dict) -> dict:
    reference_spectrum = reference["spectrum"]
    candidate_spectrum = candidate["spectrum"]
    reference_envelope = reference["envelope"]
    candidate_envelope = candidate["envelope"]
    return {
        "pitch_error_cents": rounded(abs(candidate["pitch"]["offset_cents"]), 2),
        "attack_error_ms": rounded(
            abs(
                candidate_envelope["attack_10_to_90_ms"]
                - reference_envelope["attack_10_to_90_ms"]
            ),
            2,
        ),
        "centroid_error_octaves": rounded(
            abs(
                math.log2(
                    candidate_spectrum["added_spectral_centroid_hz"]
                    / reference_spectrum["added_spectral_centroid_hz"]
                )
            ),
            4,
        ),
        "rolloff_error_octaves": rounded(
            abs(
                math.log2(
                    candidate_spectrum["added_85_percent_rolloff_hz"]
                    / reference_spectrum["added_85_percent_rolloff_hz"]
                )
            ),
            4,
        ),
        "second_harmonic_error_db": rounded(
            abs(
                candidate_spectrum["harmonics_relative_db"][1]
                - reference_spectrum["harmonics_relative_db"][1]
            ),
            2,
        ),
        "tail_500ms_error_db": rounded(
            abs(
                candidate_envelope["rms_500_to_700ms_relative_db"]
                - reference_envelope["rms_500_to_700ms_relative_db"]
            ),
            2,
        ),
        "tail_900ms_error_db": rounded(
            abs(
                candidate_envelope["rms_900_to_1100ms_relative_db"]
                - reference_envelope["rms_900_to_1100ms_relative_db"]
            ),
            2,
        ),
    }


def build_comparison(reference: dict, candidate_path: Path) -> dict:
    audio, sample_rate = sf.read(
        candidate_path, dtype="float32", always_2d=True
    )
    if sample_rate != EXPECTED_SAMPLE_RATE or audio.shape[1] != 1:
        raise SystemExit(
            f"expected a 48 kHz mono candidate, received "
            f"{sample_rate} Hz / {audio.shape[1]} channels"
        )
    mono = audio[:, 0]
    candidate_times = [
        block * FRAMES_PER_BLOCK / sample_rate
        for block in CANDIDATE_EVENT_BLOCKS
    ]
    reference_intervals = reference["phrase"]["onset_intervals_seconds"]
    candidate_intervals = np.diff(candidate_times)
    for expected, actual in zip(reference_intervals, candidate_intervals):
        if abs(float(expected) - float(actual)) > FRAMES_PER_BLOCK / sample_rate:
            raise SystemExit("candidate renderer no longer follows reference timing")

    candidate_events = []
    event_comparisons = []
    for event_time, reference_event in zip(
        candidate_times, reference["events"]
    ):
        root_midi = reference_event["inferred_root"]["midi"]
        features = candidate_event_features(
            mono, sample_rate, event_time, root_midi
        )
        candidate_events.append(
            {
                "index": reference_event["index"],
                "time_seconds": rounded(event_time, 6),
                "root_midi": root_midi,
                "root_note": reference_event["inferred_root"]["note"],
                **features,
            }
        )
        event_comparisons.append(compare_event(reference_event, features))

    aggregates = {}
    for metric in event_comparisons[0]:
        aggregates[f"median_{metric}"] = rounded(
            median([event[metric] for event in event_comparisons]), 4
        )
        aggregates[f"max_{metric}"] = rounded(
            max(event[metric] for event in event_comparisons), 4
        )

    peak = float(np.max(np.abs(mono)))
    gates = {
        "pitch": {
            "limit": "max <= 15 cents",
            "value": aggregates["max_pitch_error_cents"],
            "passed": aggregates["max_pitch_error_cents"] <= 15.0,
        },
        "attack": {
            "limit": "median error <= 15 ms",
            "value": aggregates["median_attack_error_ms"],
            "passed": aggregates["median_attack_error_ms"] <= 15.0,
        },
        "spectral_centroid": {
            "limit": "median error <= 0.35 octaves",
            "value": aggregates["median_centroid_error_octaves"],
            "passed": aggregates["median_centroid_error_octaves"] <= 0.35,
        },
        "spectral_rolloff": {
            "limit": "median error <= 0.50 octaves",
            "value": aggregates["median_rolloff_error_octaves"],
            "passed": aggregates["median_rolloff_error_octaves"] <= 0.50,
        },
        "second_harmonic": {
            "limit": "median error <= 8 dB",
            "value": aggregates["median_second_harmonic_error_db"],
            "passed": aggregates["median_second_harmonic_error_db"] <= 8.0,
        },
        "release_500ms": {
            "limit": "median error <= 6 dB",
            "value": aggregates["median_tail_500ms_error_db"],
            "passed": aggregates["median_tail_500ms_error_db"] <= 6.0,
        },
        "release_900ms": {
            "limit": "median error <= 8 dB",
            "value": aggregates["median_tail_900ms_error_db"],
            "passed": aggregates["median_tail_900ms_error_db"] <= 8.0,
        },
        "headroom": {
            "limit": "peak < 0.98",
            "value": rounded(peak, 6),
            "passed": peak < 0.98,
        },
    }
    return {
        "comparison_version": 1,
        "reference": {
            "analysis_path": "analysis/reference/strong-note-analysis.json",
            "source_sha256": reference["source"]["sha256"],
        },
        "candidate": {
            "path": candidate_path.as_posix(),
            "sha256": file_sha256(candidate_path),
            "sample_rate_hz": sample_rate,
            "channels": int(audio.shape[1]),
            "duration_seconds": rounded(audio.shape[0] / sample_rate, 3),
            "peak": rounded(peak, 6),
        },
        "phrase": {
            "root_sequence": reference["phrase"]["root_sequence"],
            "event_times_seconds": [
                rounded(value, 6) for value in candidate_times
            ],
            "onset_intervals_seconds": [
                rounded(value, 6) for value in candidate_intervals
            ],
            "gate_duration_seconds": 0.5,
        },
        "candidate_events": candidate_events,
        "event_errors": event_comparisons,
        "aggregates": aggregates,
        "gates": gates,
        "passed": all(gate["passed"] for gate in gates.values()),
        "scope": (
            "Objective dry-mono model gate for pitch, attack, dark spectral "
            "balance, second harmonic, release, and headroom. It does not "
            "replace owner listening acceptance and deliberately excludes the "
            "reference recording's stereo/string production residue."
        ),
    }


def main() -> int:
    args = parse_args()
    if not args.reference_report.is_file():
        raise SystemExit(
            f"reference report does not exist: {args.reference_report}; "
            "run scripts/analyze_reference.py first"
        )
    if not args.candidate.is_file():
        raise SystemExit(f"candidate WAV does not exist: {args.candidate}")
    reference = json.loads(args.reference_report.read_text(encoding="utf-8"))
    comparison = build_comparison(reference, args.candidate)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(comparison, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    for name, gate in comparison["gates"].items():
        status = "PASS" if gate["passed"] else "FAIL"
        print(f"{status}: {name}: {gate['value']} ({gate['limit']})")
    print(f"INFO: retained {args.output}")
    if args.require_match and not comparison["passed"]:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
