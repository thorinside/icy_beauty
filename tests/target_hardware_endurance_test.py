import importlib.util
import io
import json
import sys
import types
import unittest
from unittest import mock
from pathlib import Path


SCRIPT_PATH = (
    Path(__file__).resolve().parents[1] / "scripts" / "target_hardware_endurance.py"
)
SPEC = importlib.util.spec_from_file_location("target_hardware_endurance", SCRIPT_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class TargetHardwareEnduranceTest(unittest.TestCase):
    def test_parses_streamable_http_sse(self):
        body = "\n".join(
            [
                "id: 1",
                "data:",
                "",
                "event: message",
                "id: 2",
                'data: {"jsonrpc":"2.0","id":7,"result":{"ok":true}}',
                "",
            ]
        )
        self.assertEqual(
            MODULE.parse_mcp_payload(body, "text/event-stream"),
            {"jsonrpc": "2.0", "id": 7, "result": {"ok": True}},
        )

    def test_dense_midi_matches_native_endurance_pattern(self):
        initial, notes = MODULE.initial_midi_bytes()
        self.assertEqual(len(initial), 8)
        self.assertEqual(notes, [36, 39, 43, 46, 50, 53, 57, 60])

        messages, next_notes = MODULE.dense_midi_bytes(1, notes)
        self.assertEqual(messages[0], (0xB0, 64, 127))
        self.assertEqual(messages[1:9], [(0x80, note, 0) for note in notes])
        self.assertEqual(messages[9], (0xB0, 1, 29))
        self.assertEqual(messages[10], (0xE0, 0, 64))
        self.assertEqual(messages[11], (0xD0, 37))
        self.assertEqual(messages[-1], (0xB0, 64, 0))
        self.assertEqual(next_notes, [37, 40, 44, 47, 51, 54, 58, 61])
        self.assertEqual(
            sum(1 for message in messages if message[0] == 0x90), 8
        )
        self.assertEqual(
            sum(1 for message in messages if message[0] == 0xA0), 8
        )

    def test_cleanup_releases_notes_and_all_channels(self):
        messages = MODULE.cleanup_midi_bytes([60, 64])
        self.assertIn((0x80, 60, 0), messages)
        self.assertIn((0x80, 64, 0), messages)
        for channel in range(16):
            self.assertIn((0xB0 | channel, 123, 0), messages)
            self.assertIn((0xE0 | channel, 0, 64), messages)

    def test_midi_progress_event_cannot_collide_with_outer_event_name(self):
        worker = MODULE.MidiWorker(SCRIPT_PATH, sys.executable, 1, 0.5)
        worker.process = types.SimpleNamespace(
            stdout=io.StringIO(
                '{"event":"midi-minute","elapsedSeconds":60.0}\n'
            )
        )
        with mock.patch.object(MODULE, "emit_progress") as progress:
            worker._reader()
        progress.assert_called_once_with(
            "midi-worker",
            workerEvent={
                "event": "midi-minute",
                "elapsedSeconds": 60.0,
            },
        )

    def test_preset_summary_requires_only_nsib_slot_zero(self):
        valid = {
            "name": "Icy Beauty          ",
            "slots": [
                {
                    "slot_index": 0,
                    "algorithm": {"guid": "NsIb", "name": "Icy Beauty"},
                }
            ],
        }
        self.assertEqual(MODULE.validate_preset_summary(valid), (True, ""))
        extra = json.loads(json.dumps(valid))
        extra["slots"].append(
            {"slot_index": 1, "algorithm": {"guid": "note", "name": "Notes"}}
        )
        self.assertFalse(MODULE.validate_preset_summary(extra)[0])

    def test_target_parameter_surface_requires_all_eight_pitch_inputs(self):
        names = MODULE.required_target_parameter_names()
        self.assertIn("Pitch 1", names)
        self.assertIn("Pitch 8", names)
        self.assertEqual(
            len([name for name in names if name.startswith("Pitch ")]), 8
        )

    def test_target_slot_validation_rejects_changed_sound_setting(self):
        parameters = [
            {"parameter_name": name, "value": 0}
            for name in MODULE.required_target_parameter_names()
        ]
        values = {
            **MODULE.SOUND_SETTINGS,
            "Output": "Output 1",
            "MIDI channel": "Omni",
        }
        for parameter in parameters:
            if parameter["parameter_name"] in values:
                parameter["value"] = values[parameter["parameter_name"]]
        slot = {
            "algorithm": {"guid": "NsIb"},
            "parameter_count": 18,
            "parameters": parameters,
        }
        self.assertEqual(MODULE.validate_target_slot(slot), (True, ""))
        next(
            item for item in parameters if item["parameter_name"] == "Grain"
        )["value"] = 99
        self.assertEqual(
            MODULE.validate_target_slot(slot),
            (False, "Grain changed from 100"),
        )

    def test_evidence_values_match_live_form(self):
        report = {
            "contract": {
                "activeSeconds": 1800,
                "responsivenessIntervalSeconds": 10,
            },
            "midi": {
                "ready": {"firmware": "1.12.3"},
                "complete": {"elapsedSeconds": 1800.25},
            },
            "ntHelper": {"serverInfo": {"version": "1.39.0"}},
        }
        values = MODULE.evidence_values(report)
        self.assertEqual(set(values), MODULE.EXPECTED_EVIDENCE_FIELDS)
        self.assertGreater(values["uninterruptedMinutes"], 30)
        self.assertTrue(values["noCrashes"])
        self.assertTrue(values["noInvalidAudio"])

    def test_existing_session_duration_takes_precedence_over_closeout_midi(self):
        report = {
            "contract": {
                "activeSeconds": 5,
                "responsivenessIntervalSeconds": 2,
            },
            "observedSession": {
                "elapsedSeconds": 2200,
            },
            "midi": {
                "ready": {"firmware": "v1.17.0"},
                "complete": {"elapsedSeconds": 5.0},
            },
            "ntHelper": {"serverInfo": {"version": "2.43.18"}},
        }
        values = MODULE.evidence_values(report)
        self.assertAlmostEqual(values["uninterruptedMinutes"], 2200 / 60)
        self.assertIn("began at the four-voice default", values["patchSettings"])

    def test_refuses_short_evidence_submission(self):
        report = {
            "contract": {"activeSeconds": 5},
            "passed": True,
        }
        with self.assertRaisesRegex(MODULE.EnduranceError, "less than 30 minutes"):
            MODULE.submit_evidence(report, Path("/does/not/matter"))


if __name__ == "__main__":
    unittest.main()
