import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPORT_PATH = (
    ROOT
    / "verification"
    / "hardware-runs"
    / "icy-beauty-20260729T205925Z"
    / "target-hardware-endurance.json"
)
PROCESSING_LIMIT_PERCENT = 75.0
MINIMUM_DURATION_SECONDS = 30 * 60


class TargetPerformanceEvidenceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.report = json.loads(REPORT_PATH.read_text(encoding="utf-8"))

    def test_physical_run_covers_the_approved_duration(self):
        report = self.report
        self.assertTrue(report["passed"])
        self.assertTrue(report["physicalTestPassed"])
        self.assertGreaterEqual(
            report["contract"]["activeSeconds"], MINIMUM_DURATION_SECONDS
        )
        self.assertGreaterEqual(
            report["midi"]["complete"]["elapsedSeconds"],
            MINIMUM_DURATION_SECONDS,
        )
        self.assertTrue(report["responsiveness"]["coveragePassed"])
        self.assertFalse(report["responsiveness"]["errors"])

    def test_every_retained_processing_measurement_is_within_budget(self):
        responsiveness = self.report["responsiveness"]
        observed = []
        for check in responsiveness["checks"]:
            cpu = check["cpuUsage"]
            observed.extend(
                cpu[name]
                for name in (
                    "cpu1_percent",
                    "cpu2_percent",
                    "total_usage_percent",
                )
            )
            observed.extend(
                slot["usage_percent"] for slot in cpu["slot_usages"]
            )

        self.assertEqual(len(responsiveness["checks"]), 182)
        self.assertEqual(max(observed), 29)
        self.assertEqual(
            max(observed),
            responsiveness["maximumObservedProcessingUsePercent"],
        )
        self.assertLessEqual(max(observed), PROCESSING_LIMIT_PERCENT)


if __name__ == "__main__":
    unittest.main()
