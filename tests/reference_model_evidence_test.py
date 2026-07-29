import hashlib
import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REFERENCE_ANALYSIS = ROOT / "analysis/reference/strong-note-analysis.json"
BASELINE_COMPARISON = (
    ROOT / "analysis/candidate/pre-model-baseline-comparison.json"
)
CURRENT_COMPARISON = (
    ROOT / "analysis/candidate/current-default-comparison.json"
)
REFERENCE_SHA256 = (
    "e19a959009e8045a9be01a132b8168f971930465d60e4fc8e1820babfcf26d5a"
)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


class ReferenceModelEvidenceTest(unittest.TestCase):
    def load_json(self, path):
        self.assertTrue(path.is_file(), f"missing retained evidence: {path}")
        return json.loads(path.read_text(encoding="utf-8"))

    def test_reference_selects_the_exposed_phrase(self):
        report = self.load_json(REFERENCE_ANALYSIS)
        self.assertEqual(report["source"]["sha256"], REFERENCE_SHA256)
        self.assertEqual(
            sha256(ROOT / report["source"]["path"]), REFERENCE_SHA256
        )
        self.assertGreaterEqual(report["selection"]["focus_start_seconds"], 12.5)
        self.assertEqual(
            report["phrase"]["root_sequence"], ["D3", "F#3", "B2", "C#3"]
        )
        self.assertEqual(
            report["phrase"]["root_midi_sequence"], [50, 54, 47, 49]
        )
        self.assertEqual(len(report["events"]), 4)
        for event in report["events"]:
            self.assertGreater(event["time_seconds"], 12.5)
            self.assertGreater(event["envelope"]["energy_step_db"], 8.0)
            self.assertTrue(event["polyphonic_notes_near_onset"])

    def test_current_render_passes_every_objective_model_gate(self):
        comparison = self.load_json(CURRENT_COMPARISON)
        candidate = ROOT / comparison["candidate"]["path"]
        self.assertEqual(sha256(candidate), comparison["candidate"]["sha256"])
        self.assertTrue(comparison["passed"])
        self.assertTrue(comparison["gates"])
        for name, gate in comparison["gates"].items():
            self.assertTrue(gate["passed"], f"failed model gate: {name}")

    def test_model_changes_close_the_recorded_baseline_gap(self):
        baseline = self.load_json(BASELINE_COMPARISON)
        current = self.load_json(CURRENT_COMPARISON)
        self.assertFalse(baseline["passed"])
        self.assertTrue(current["passed"])
        for gate in (
            "attack",
            "spectral_centroid",
            "second_harmonic",
            "release_900ms",
            "headroom",
        ):
            self.assertFalse(
                baseline["gates"][gate]["passed"],
                f"baseline unexpectedly passed {gate}",
            )
            self.assertTrue(
                current["gates"][gate]["passed"],
                f"current render did not close {gate}",
            )


if __name__ == "__main__":
    unittest.main()
