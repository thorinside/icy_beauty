import hashlib
import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUN_DIRECTORY = (
    ROOT
    / "verification"
    / "hardware-runs"
    / "icy-beauty-latency-20260729T221816Z"
)
REPORT_PATH = RUN_DIRECTORY / "target-midi-latency.json"
LATENCY_LIMIT_MS = 10.0
PROCESSING_LIMIT_PERCENT = 75.0


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


class TargetLatencyEvidenceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.report = json.loads(REPORT_PATH.read_text(encoding="utf-8"))

    def test_eight_physical_trials_pass_with_conservative_bounds(self):
        report = self.report
        analysis = report["latencyAnalysis"]
        self.assertTrue(report["passed"])
        self.assertTrue(report["physicalTestPassed"])
        self.assertTrue(analysis["passed"])
        self.assertEqual(analysis["trialCount"], 8)
        self.assertEqual(len(analysis["trials"]), 8)
        self.assertEqual(
            max(
                trial["conservativeUpperLatencyMilliseconds"]
                for trial in analysis["trials"]
            ),
            analysis["maximumConservativeLatencyMilliseconds"],
        )
        self.assertLess(
            analysis["maximumConservativeLatencyMilliseconds"],
            LATENCY_LIMIT_MS,
        )
        self.assertTrue(
            all(
                trial["passed"]
                and trial["signalToBaselineDb"] >= 60.0
                and trial["conservativeUpperLatencyMilliseconds"]
                < LATENCY_LIMIT_MS
                for trial in analysis["trials"]
            )
        )

    def test_capture_is_exact_nt_input_one_without_stream_errors(self):
        summary = self.report["capture"]["timing"]["summary"]
        self.assertEqual(summary["device"], "disting NT")
        self.assertEqual(summary["destination"], "disting NT")
        self.assertEqual(summary["deviceMatches"], 1)
        self.assertEqual(summary["destinationMatches"], 1)
        self.assertEqual(summary["sampleRate"], 48_000)
        self.assertEqual(summary["deviceInputChannels"], 12)
        self.assertEqual(summary["selectedInput"], 1)
        self.assertEqual(summary["callbackStatusFlags"], 0)
        self.assertEqual(summary["nullInputCount"], 0)
        self.assertFalse(summary["capacityExceeded"])
        self.assertTrue(summary["midiPassed"])
        self.assertTrue(summary["rawPassed"])

    def test_usb_capture_topology_and_nsib_only_restoration_are_retained(self):
        topology = self.report["captureTopology"]
        self.assertEqual(topology["sourceOutput"], "Output 1")
        self.assertEqual(topology["hostCaptureInput"], 1)
        self.assertEqual(
            [
                slot["algorithm"]["guid"]
                for slot in topology["preset"]["slots"]
            ],
            ["NsIb", "usbt"],
        )
        self.assertEqual(
            topology["routing"][0]["output_buses"], ["Output 1"]
        )
        self.assertEqual(
            topology["routing"][1]["input_buses"], ["Output 1"]
        )
        restoration = self.report["presetRestoration"]
        self.assertTrue(restoration["restoredExactly"])
        self.assertEqual(
            [
                slot["algorithm"]["guid"]
                for slot in restoration["after"]["slots"]
            ],
            ["NsIb"],
        )

    def test_processing_and_submitted_ac005_values_are_within_limits(self):
        processing = self.report["processingEvidence"]
        evidence = self.report["evidence"]
        self.assertTrue(processing["passed"])
        self.assertEqual(processing["checkCount"], 182)
        self.assertEqual(
            processing["maximumObservedProcessingUsePercent"], 29
        )
        self.assertLessEqual(
            processing["maximumObservedProcessingUsePercent"],
            PROCESSING_LIMIT_PERCENT,
        )
        self.assertTrue(evidence["submitted"])
        self.assertEqual(evidence["criterionStatus"], "met")
        self.assertEqual(evidence["values"]["maxProcessingUsePercent"], 29)
        self.assertGreaterEqual(
            evidence["values"]["maxMidiNoteOnLatencyMs"],
            self.report["latencyAnalysis"][
                "maximumConservativeLatencyMilliseconds"
            ],
        )
        self.assertLess(
            evidence["values"]["maxMidiNoteOnLatencyMs"],
            LATENCY_LIMIT_MS,
        )

    def test_retained_capture_hashes_match_the_report(self):
        capture = self.report["capture"]
        for key in ("raw", "wav"):
            path = RUN_DIRECTORY / Path(capture[key]["path"]).name
            self.assertEqual(sha256(path), capture[key]["sha256"])
        timing_path = RUN_DIRECTORY / Path(capture["timing"]["path"]).name
        self.assertEqual(
            sha256(timing_path), capture["timing"]["sha256"]
        )
        processing_path = (
            ROOT
            / "verification"
            / "hardware-runs"
            / "icy-beauty-20260729T205925Z"
            / "target-hardware-endurance.json"
        )
        self.assertEqual(
            sha256(processing_path),
            self.report["processingEvidence"]["sha256"],
        )
        app = self.report["nativeCaptureApp"]
        app_root = (
            RUN_DIRECTORY
            / "tools"
            / "Icy Beauty Latency Capture.app"
            / "Contents"
        )
        self.assertEqual(
            sha256(app_root / "MacOS" / "IcyBeautyLatencyCapture"),
            app["executable"]["sha256"],
        )
        self.assertEqual(
            sha256(app_root / "Info.plist"),
            app["infoPlist"]["sha256"],
        )


if __name__ == "__main__":
    unittest.main()
