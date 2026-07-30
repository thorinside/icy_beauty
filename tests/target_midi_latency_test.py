import array
import importlib.util
import math
import sys
import unittest
from pathlib import Path


SCRIPT_PATH = (
    Path(__file__).resolve().parents[1] / "scripts" / "target_midi_latency.py"
)
SCRIPTS_DIR = str(SCRIPT_PATH.parent)
if SCRIPTS_DIR not in sys.path:
    sys.path.insert(0, SCRIPTS_DIR)
SPEC = importlib.util.spec_from_file_location("target_midi_latency", SCRIPT_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def make_capture(latency_seconds=0.004, trials=8):
    sample_rate = MODULE.SAMPLE_RATE
    total_seconds = 2.2
    sample_count = int(total_seconds * sample_rate)
    samples = array.array("f", [0.0]) * sample_count
    trial_timings = []
    for index in range(trials):
        send_before = 100.20 + index * 0.22
        send_after = send_before + 0.000020
        onset = send_before + latency_seconds
        onset_frame = int(round((onset - 100.0) * sample_rate))
        for frame in range(onset_frame, min(onset_frame + 960, sample_count)):
            samples[frame] = 0.2 * math.sin(
                2.0 * math.pi * 440.0 * (frame - onset_frame) / sample_rate
            )
        trial_timings.append(
            {
                "index": index + 1,
                "note": 69,
                "velocity": 127,
                "sendBeforeTime": send_before,
                "sendAfterTime": send_after,
            }
        )
    blocks = []
    for start in range(0, sample_count, 64):
        count = min(64, sample_count - start)
        blocks.append(
            {
                "startFrame": start,
                "frameCount": count,
                "adcTime": 100.0 + start / sample_rate,
                "statusFlags": 0,
            }
        )
    timing = {
        "sampleRate": sample_rate,
        "deviceInputChannels": 12,
        "selectedInput": 1,
        "callbackStatusFlags": 0,
        "nullInputCount": 0,
        "capacityExceeded": False,
        "blocks": blocks,
        "trials": trial_timings,
    }
    return samples, timing


class TargetMidiLatencyTest(unittest.TestCase):
    def test_initial_preset_requires_only_configured_eight_voice_thib(self):
        names = {
            "Output": "Output 1",
            "Output mode": "Add",
            "MIDI channel": "Omni",
            "Gate": "Input 1",
            **{
                "Pitch %d" % index: "Input %d" % (index + 1)
                for index in range(1, 9)
            },
            "Tone": 100,
            "Motion": 100,
            "Grain": 100,
            "Resonance": 100,
            "Release": 100,
        }
        slot = {
            "slot_index": 0,
            "algorithm": {"guid": "ThIb", "name": "Icy Beauty"},
            "parameter_count": 18,
            "parameters": [
                {"parameter_name": name, "value": value}
                for name, value in names.items()
            ],
        }
        preset = {
            "name": "Icy Beauty          ",
            "slots": [
                {
                    "slot_index": 0,
                    "algorithm": {"guid": "ThIb", "name": "Icy Beauty"},
                }
            ],
        }
        self.assertEqual(
            MODULE.validate_initial_preset(preset, slot), (True, "")
        )
        preset["slots"].append(
            {
                "slot_index": 1,
                "algorithm": {"guid": "usbt", "name": "USB audio (to host)"},
            }
        )
        self.assertFalse(MODULE.validate_initial_preset(preset, slot)[0])

    def test_shared_clock_analysis_produces_conservative_sub_10ms_bound(self):
        samples, timing = make_capture(latency_seconds=0.004)
        analysis = MODULE.analyze_capture(samples, timing)
        self.assertTrue(analysis["passed"])
        self.assertEqual(analysis["trialCount"], 8)
        self.assertLess(
            analysis["maximumConservativeLatencyMilliseconds"], 5.0
        )
        for trial in analysis["trials"]:
            self.assertTrue(trial["passed"])
            self.assertGreaterEqual(
                trial["conservativeUpperLatencyMilliseconds"],
                trial["pointLatencyMilliseconds"],
            )
            self.assertGreater(trial["signalToBaselineDb"], 60)

    def test_analysis_rejects_latency_at_or_above_limit(self):
        samples, timing = make_capture(latency_seconds=0.012)
        analysis = MODULE.analyze_capture(samples, timing)
        self.assertFalse(analysis["passed"])
        self.assertGreater(
            analysis["maximumConservativeLatencyMilliseconds"], 10.0
        )

    def test_analysis_rejects_audio_callback_status_flag(self):
        samples, timing = make_capture()
        timing["callbackStatusFlags"] = 2
        with self.assertRaisesRegex(
            MODULE.LatencyError, "overflow or underflow"
        ):
            MODULE.analyze_capture(samples, timing)

    def test_analysis_inflates_bound_for_small_adc_timestamp_jitter(self):
        samples, timing = make_capture()
        for index, block in enumerate(timing["blocks"]):
            if index % 2:
                block["adcTime"] += 0.000020
        analysis = MODULE.analyze_capture(samples, timing)
        self.assertTrue(analysis["passed"])
        self.assertGreaterEqual(
            analysis["timestampUncertaintyMilliseconds"], 0.019
        )
        self.assertTrue(
            all(
                trial["timestampUncertaintyMilliseconds"] >= 0.019
                for trial in analysis["trials"]
            )
        )

    def test_analysis_rejects_large_adc_clock_discontinuity(self):
        samples, timing = make_capture()
        for block in timing["blocks"][10:]:
            block["adcTime"] += 0.001
        with self.assertRaisesRegex(
            MODULE.LatencyError, "audio clock discontinuity"
        ):
            MODULE.analyze_capture(samples, timing)

    def test_round_up_never_understates_evidence_value(self):
        self.assertEqual(MODULE.round_up(4.00001), 4.001)
        self.assertEqual(MODULE.round_up(4.0), 4.0)

    def test_submission_refuses_fewer_than_eight_trials(self):
        report = {
            "physicalTestPassed": True,
            "latencyAnalysis": {
                "passed": True,
                "trialCount": 1,
                "maximumConservativeLatencyMilliseconds": 4.0,
            },
            "processingEvidence": {
                "passed": True,
                "maximumObservedProcessingUsePercent": 29,
            },
            "presetRestoration": {"restoredExactly": True},
        }
        with self.assertRaisesRegex(MODULE.LatencyError, "fewer than eight"):
            MODULE.submit_evidence(report, Path("/does/not/matter"))


if __name__ == "__main__":
    unittest.main()
