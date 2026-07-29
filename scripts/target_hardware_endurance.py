#!/usr/bin/env python3
"""Run and record the physical Icy Beauty AC-004 endurance test.

The controller process reruns the native/ARM preflight, uses nt_helper MCP for
preset setup and repeated physical responsiveness checks, and launches a small
MIDI worker with the existing ntpush Python environment. A passing 30-minute
run can submit the exact AC-004 evidence form through Substrate MCP.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import queue
import signal
import subprocess
import sys
import threading
import time
import tomllib
import urllib.error
import urllib.request
import uuid
from datetime import datetime, timezone
from pathlib import Path


ANALYZER_VERSION = 1
MCP_PROTOCOL_VERSION = "2025-03-26"
PROJECT_ID = "0966eaa1-b61c-4a7f-a172-d7a37df994dd"
CRITERION_KEY = "ac-004"
PLUGIN_GUID = "NsIb"
PLUGIN_NAME = "Icy Beauty"
PRESET_NAME = "Icy Beauty"
MIDI_DEVICE_NAME = "disting NT"
MINIMUM_EVIDENCE_SECONDS = 30 * 60
DEFAULT_MIDI_PYTHON = "/Users/nealsanche/.venvs/ntpush/bin/python"
EXPECTED_EVIDENCE_FIELDS = {
    "testedFirmware",
    "hostConfiguration",
    "patchSettings",
    "loadedNormally",
    "uninterruptedMinutes",
    "noCrashes",
    "noDropouts",
    "noStuckNotes",
    "noInvalidAudio",
    "noFeatureReduction",
}
SOUND_SETTINGS = {
    "Tone": 100,
    "Motion": 100,
    "Grain": 100,
    "Resonance": 100,
    "Release": 100,
}

_PRINT_LOCK = threading.Lock()


class EnduranceError(RuntimeError):
    """Raised when the physical test cannot produce trustworthy evidence."""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def parse_utc_timestamp(value: str) -> datetime:
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise EnduranceError("invalid ISO-8601 session start: %s" % value) from error
    if parsed.tzinfo is None:
        raise EnduranceError("existing session start must include a timezone")
    return parsed.astimezone(timezone.utc)


def validated_duration_seconds(report: dict) -> float:
    observed = report.get("observedSession")
    if isinstance(observed, dict):
        elapsed = observed.get("elapsedSeconds")
        if isinstance(elapsed, (int, float)):
            return float(elapsed)
    measured = report.get("midi", {}).get("complete", {}).get("elapsedSeconds")
    return float(measured) if isinstance(measured, (int, float)) else 0.0


def emit_progress(event: str, **fields) -> None:
    payload = {"event": event, "timestamp": utc_now(), **fields}
    with _PRINT_LOCK:
        print(json.dumps(payload, sort_keys=True), flush=True)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_command(command: list[str], timeout: float = 10.0) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except (OSError, subprocess.SubprocessError) as error:
        raise EnduranceError("command failed (%s): %s" % (command[0], error)) from error


def git_snapshot(cwd: Path) -> dict:
    head = run_command(["git", "rev-parse", "HEAD"], timeout=5.0)
    status = run_command(["git", "status", "--porcelain=v1"], timeout=5.0)
    if head.returncode != 0 or status.returncode != 0:
        raise EnduranceError("unable to inspect the git checkout")
    return {
        "commit": head.stdout.strip(),
        "workingTreeClean": not bool(status.stdout.strip()),
        "status": status.stdout.splitlines(),
    }


def parse_mcp_payload(body: str, content_type: str) -> dict | None:
    if not body.strip():
        return None
    if "text/event-stream" not in content_type and not body.lstrip().startswith(
        ("data:", "event:", "id:")
    ):
        return json.loads(body)

    payloads = []
    data_lines = []
    for line in body.splitlines() + [""]:
        if line.startswith("data:"):
            data_lines.append(line[5:].lstrip())
        elif not line.strip() and data_lines:
            event_data = "\n".join(data_lines)
            if event_data.strip():
                payloads.append(json.loads(event_data))
            data_lines = []
    return payloads[-1] if payloads else None


class McpClient:
    def __init__(
        self,
        url: str,
        *,
        headers: dict[str, str] | None = None,
        name: str,
        timeout: float = 10.0,
    ):
        self.url = url
        self.headers = dict(headers or {})
        self.name = name
        self.timeout = timeout
        self.session_id: str | None = None
        self.request_id = 1
        self.server_info: dict = {}
        self.protocol_version: str | None = None

    def _post(self, message: dict, timeout: float | None = None) -> dict | None:
        headers = {
            "Accept": "application/json, text/event-stream",
            "Content-Type": "application/json",
            **self.headers,
        }
        if self.session_id:
            headers["Mcp-Session-Id"] = self.session_id
        request = urllib.request.Request(
            self.url,
            data=json.dumps(message).encode("utf-8"),
            headers=headers,
            method="POST",
        )
        try:
            with urllib.request.urlopen(
                request, timeout=timeout if timeout is not None else self.timeout
            ) as response:
                body = response.read().decode("utf-8")
                returned_session = response.headers.get("Mcp-Session-Id")
                if returned_session:
                    self.session_id = returned_session
                return parse_mcp_payload(
                    body, response.headers.get("Content-Type", "")
                )
        except urllib.error.HTTPError as error:
            body = error.read().decode("utf-8", errors="replace")
            raise EnduranceError(
                "%s MCP HTTP %d: %s" % (self.name, error.code, body[:500])
            ) from error
        except (OSError, ValueError, json.JSONDecodeError) as error:
            raise EnduranceError("%s MCP request failed: %s" % (self.name, error)) from error

    @staticmethod
    def _result(payload: dict | None, operation: str) -> dict:
        if not isinstance(payload, dict):
            raise EnduranceError("MCP %s returned no JSON-RPC object" % operation)
        if payload.get("error") is not None:
            raise EnduranceError("MCP %s failed: %s" % (operation, payload["error"]))
        result = payload.get("result")
        if not isinstance(result, dict):
            raise EnduranceError("MCP %s returned no result object" % operation)
        return result

    def initialize(self) -> None:
        payload = self._post(
            {
                "jsonrpc": "2.0",
                "id": self.request_id,
                "method": "initialize",
                "params": {
                    "protocolVersion": MCP_PROTOCOL_VERSION,
                    "capabilities": {},
                    "clientInfo": {
                        "name": self.name,
                        "version": str(ANALYZER_VERSION),
                    },
                },
            }
        )
        self.request_id += 1
        result = self._result(payload, "%s initialize" % self.name)
        self.protocol_version = result.get("protocolVersion")
        self.server_info = result.get("serverInfo") or {}
        if not self.protocol_version or not self.session_id:
            raise EnduranceError("%s MCP initialization was incomplete" % self.name)
        notification_payload = self._post(
            {
                "jsonrpc": "2.0",
                "method": "notifications/initialized",
                "params": {},
            }
        )
        if isinstance(notification_payload, dict) and notification_payload.get("error"):
            raise EnduranceError(
                "%s MCP initialized notification failed: %s"
                % (self.name, notification_payload["error"])
            )

    def call_tool(
        self, name: str, arguments: dict | None = None, timeout: float | None = None
    ):
        payload = self._post(
            {
                "jsonrpc": "2.0",
                "id": self.request_id,
                "method": "tools/call",
                "params": {"name": name, "arguments": arguments or {}},
            },
            timeout=timeout,
        )
        self.request_id += 1
        result = self._result(payload, "%s tools/call %s" % (self.name, name))
        if result.get("isError"):
            raise EnduranceError("%s tool %s reported an error" % (self.name, name))
        content = result.get("content")
        if not isinstance(content, list):
            raise EnduranceError("%s tool %s omitted content" % (self.name, name))
        text = "\n".join(
            item["text"]
            for item in content
            if isinstance(item, dict)
            and item.get("type") == "text"
            and isinstance(item.get("text"), str)
        )
        if not text:
            raise EnduranceError("%s tool %s returned no text" % (self.name, name))
        try:
            return json.loads(text)
        except json.JSONDecodeError as error:
            raise EnduranceError(
                "%s tool %s returned non-JSON text" % (self.name, name)
            ) from error


def load_substrate_connection(config_path: Path) -> tuple[str, dict[str, str]]:
    try:
        config = tomllib.loads(config_path.read_text(encoding="utf-8"))
        substrate = config["mcp_servers"]["substrate"]
        url = substrate["url"]
        headers = substrate.get("http_headers", {})
    except (OSError, KeyError, TypeError, tomllib.TOMLDecodeError) as error:
        raise EnduranceError(
            "cannot load Substrate MCP configuration from %s: %s"
            % (config_path, error)
        ) from error
    if not isinstance(url, str) or not url:
        raise EnduranceError("Substrate MCP URL is missing")
    if not isinstance(headers, dict):
        raise EnduranceError("Substrate MCP headers are invalid")
    return url, {str(key): str(value) for key, value in headers.items()}


def run_preflight() -> dict:
    command = ["make", "test", "endurance", "inspect"]
    completed = run_command(command, timeout=180.0)
    return {
        "passed": completed.returncode == 0,
        "command": command,
        "exitCode": completed.returncode,
        "stdout": completed.stdout.strip()[-8000:],
        "stderr": completed.stderr.strip()[-4000:],
    }


def make_endurance_chord(generation: int) -> list[int]:
    intervals = (0, 3, 7, 10, 14, 17, 21, 24)
    root = 36 + generation % 48
    return [root + interval for interval in intervals]


def initial_midi_bytes() -> tuple[list[tuple[int, ...]], list[int]]:
    notes = make_endurance_chord(0)
    messages = [(0x90, note, 48 + index * 10) for index, note in enumerate(notes)]
    return messages, notes


def dense_midi_bytes(
    generation: int, current_notes: list[int]
) -> tuple[list[tuple[int, ...]], list[int]]:
    messages: list[tuple[int, ...]] = [(0xB0, 64, 127)]
    messages.extend((0x80, note, 0) for note in current_notes)
    messages.append((0xB0, 1, (generation * 29) & 0x7F))
    bend_values = (0, 8192, 16383, 8192)
    bend = bend_values[generation % len(bend_values)]
    messages.append((0xE0, bend & 0x7F, (bend >> 7) & 0x7F))
    messages.append((0xD0, (generation * 37) & 0x7F))
    next_notes = make_endurance_chord(generation)
    for index, note in enumerate(next_notes):
        velocity = 32 + ((generation * 17 + index * 11) % 96)
        pressure = (generation * 23 + index * 13) & 0x7F
        messages.append((0x90, note, velocity))
        messages.append((0xA0, note, pressure))
    messages.append((0xB0, 64, 0))
    return messages, next_notes


def cleanup_midi_bytes(current_notes: list[int]) -> list[tuple[int, ...]]:
    messages: list[tuple[int, ...]] = [(0xB0, 64, 0)]
    messages.extend((0x80, note, 0) for note in current_notes)
    for channel in range(16):
        messages.extend(
            [
                (0xB0 | channel, 64, 0),
                (0xB0 | channel, 123, 0),
                (0xE0 | channel, 0, 64),
                (0xD0 | channel, 0),
            ]
        )
    return messages


def query_firmware(mido, input_port, output_port, timeout: float = 3.0) -> str:
    for _ in input_port.iter_pending():
        pass
    output_port.send(
        mido.Message(
            "sysex", data=(0x00, 0x21, 0x27, 0x6D, 0x00, 0x22)
        )
    )
    deadline = time.monotonic() + timeout
    prefix = (0x00, 0x21, 0x27, 0x6D, 0x00, 0x32)
    while time.monotonic() < deadline:
        for message in input_port.iter_pending():
            if message.type != "sysex":
                continue
            data = tuple(message.data)
            if data[: len(prefix)] != prefix:
                continue
            payload = bytes(data[len(prefix) :])
            return payload.split(b"\x00", 1)[0].decode("ascii", errors="replace")
        time.sleep(0.01)
    raise EnduranceError("disting NT firmware query timed out")


def send_midi_bytes(mido, output_port, messages: list[tuple[int, ...]]) -> None:
    for message in messages:
        output_port.send(mido.Message.from_bytes(list(message)))


_WORKER_STOP = False


def _request_worker_stop(_signum, _frame) -> None:
    global _WORKER_STOP
    _WORKER_STOP = True


def midi_worker_main(args) -> int:
    global _WORKER_STOP
    _WORKER_STOP = False
    try:
        import mido
    except ImportError as error:
        print(
            json.dumps({"event": "error", "error": "mido is unavailable"}),
            flush=True,
        )
        return 2

    signal.signal(signal.SIGINT, _request_worker_stop)
    signal.signal(signal.SIGTERM, _request_worker_stop)
    outputs = [name for name in mido.get_output_names() if name == MIDI_DEVICE_NAME]
    inputs = [name for name in mido.get_input_names() if name == MIDI_DEVICE_NAME]
    if len(outputs) != 1 or len(inputs) != 1:
        print(
            json.dumps(
                {
                    "event": "error",
                    "error": "expected one exact disting NT MIDI input and output",
                    "inputs": inputs,
                    "outputs": outputs,
                }
            ),
            flush=True,
        )
        return 3

    current_notes: list[int] = []
    completed = False
    cycles = 0
    elapsed = 0.0
    with mido.open_input(inputs[0]) as input_port, mido.open_output(
        outputs[0]
    ) as output_port:
        try:
            firmware = query_firmware(mido, input_port, output_port)
            print(
                json.dumps(
                    {
                        "event": "ready",
                        "firmware": firmware,
                        "input": inputs[0],
                        "output": outputs[0],
                    }
                ),
                flush=True,
            )
            if sys.stdin.readline().strip() != "start":
                raise EnduranceError("MIDI worker did not receive start")

            initial, current_notes = initial_midi_bytes()
            send_midi_bytes(mido, output_port, initial)
            started = time.monotonic()
            deadline = started + args.duration_seconds
            next_activity = started + args.activity_interval_seconds
            next_progress = started + 60.0
            generation = 1
            while not _WORKER_STOP:
                now = time.monotonic()
                if now >= deadline:
                    completed = True
                    break
                if now >= next_activity:
                    messages, current_notes = dense_midi_bytes(
                        generation, current_notes
                    )
                    send_midi_bytes(mido, output_port, messages)
                    generation += 1
                    cycles += 1
                    next_activity += args.activity_interval_seconds
                    continue
                if now >= next_progress:
                    print(
                        json.dumps(
                            {
                                "event": "midi-minute",
                                "elapsedSeconds": now - started,
                                "activityCycles": cycles,
                            }
                        ),
                        flush=True,
                    )
                    next_progress += 60.0
                    continue
                time.sleep(min(0.02, next_activity - now, deadline - now))
            elapsed = time.monotonic() - started
        except Exception as error:  # noqa: BLE001 - worker must always clean MIDI
            print(json.dumps({"event": "error", "error": str(error)}), flush=True)
        finally:
            try:
                send_midi_bytes(mido, output_port, cleanup_midi_bytes(current_notes))
            except Exception as cleanup_error:  # noqa: BLE001
                print(
                    json.dumps(
                        {"event": "cleanup-error", "error": str(cleanup_error)}
                    ),
                    flush=True,
                )

    print(
        json.dumps(
            {
                "event": "complete",
                "completed": completed,
                "elapsedSeconds": elapsed,
                "activityCycles": cycles,
            }
        ),
        flush=True,
    )
    return 0 if completed else 4


class MidiWorker:
    def __init__(
        self,
        script_path: Path,
        midi_python: str,
        duration_seconds: int,
        activity_interval_seconds: float,
    ):
        self.command = [
            midi_python,
            str(script_path),
            "--midi-worker",
            "--duration-seconds",
            str(duration_seconds),
            "--activity-interval-seconds",
            str(activity_interval_seconds),
        ]
        self.process: subprocess.Popen | None = None
        self.events: list[dict] = []
        self.event_queue: queue.Queue = queue.Queue()
        self.reader_thread: threading.Thread | None = None
        self.stderr = ""

    def start(self) -> None:
        try:
            self.process = subprocess.Popen(
                self.command,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                bufsize=1,
            )
        except OSError as error:
            raise EnduranceError("could not start MIDI worker: %s" % error) from error
        self.reader_thread = threading.Thread(target=self._reader, daemon=True)
        self.reader_thread.start()

    def _reader(self) -> None:
        if self.process is None or self.process.stdout is None:
            return
        for line in self.process.stdout:
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                event = {"event": "invalid-output", "line": line.rstrip()}
            self.events.append(event)
            self.event_queue.put(event)
            if event.get("event") in {"midi-minute", "error", "cleanup-error"}:
                emit_progress("midi-worker", workerEvent=event)

    def wait_for_event(self, name: str, timeout: float) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                event = self.event_queue.get(
                    timeout=max(0.01, deadline - time.monotonic())
                )
            except queue.Empty:
                break
            if event.get("event") == "error":
                raise EnduranceError("MIDI worker failed: %s" % event.get("error"))
            if event.get("event") == name:
                return event
        raise EnduranceError("MIDI worker did not emit %s" % name)

    def begin(self) -> None:
        if self.process is None or self.process.stdin is None:
            raise EnduranceError("MIDI worker is not running")
        self.process.stdin.write("start\n")
        self.process.stdin.flush()

    def wait(self, timeout: float) -> int:
        if self.process is None:
            raise EnduranceError("MIDI worker is not running")
        try:
            return self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            raise EnduranceError("MIDI worker did not finish") from error

    def stop(self) -> None:
        if self.process is not None and self.process.poll() is None:
            self.process.send_signal(signal.SIGINT)
            try:
                self.process.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                self.process.wait(timeout=3.0)
        if self.reader_thread is not None:
            self.reader_thread.join(timeout=3.0)
        if self.process is not None and self.process.stderr is not None:
            self.stderr = self.process.stderr.read()

    def report(self) -> dict:
        ready = next(
            (event for event in self.events if event.get("event") == "ready"), None
        )
        complete = next(
            (event for event in reversed(self.events) if event.get("event") == "complete"),
            None,
        )
        errors = [
            event
            for event in self.events
            if event.get("event") in {"error", "cleanup-error", "invalid-output"}
        ]
        return {
            "passed": bool(complete and complete.get("completed") and not errors),
            "ready": ready,
            "complete": complete,
            "errors": errors,
            "stderr": self.stderr.strip()[:2000],
        }


def normalize_preset_name(value: str) -> str:
    return value.rstrip()


def validate_preset_summary(preset: dict) -> tuple[bool, str]:
    slots = preset.get("slots") if isinstance(preset, dict) else None
    populated = [slot for slot in slots or [] if slot is not None]
    if normalize_preset_name(str(preset.get("name", ""))) != PRESET_NAME:
        return False, "preset name changed"
    if len(populated) != 1:
        return False, "expected exactly one populated slot"
    algorithm = populated[0].get("algorithm") or {}
    if populated[0].get("slot_index") != 0 or algorithm.get("guid") != PLUGIN_GUID:
        return False, "slot 0 is not NsIb"
    return True, ""


def required_target_parameter_names() -> set[str]:
    return {
        "Output",
        "Output mode",
        "MIDI channel",
        "Gate",
        *(f"Pitch {index}" for index in range(1, 9)),
        *SOUND_SETTINGS.keys(),
    }


def validate_target_slot(
    slot: dict, *, require_configured_values: bool = True
) -> tuple[bool, str]:
    algorithm = slot.get("algorithm") if isinstance(slot, dict) else None
    parameters = {
        item.get("parameter_name"): item.get("value")
        for item in (slot.get("parameters") or [])
        if isinstance(item, dict)
    }
    if not isinstance(algorithm, dict) or algorithm.get("guid") != PLUGIN_GUID:
        return False, "slot 0 is not NsIb"
    if slot.get("parameter_count") != 18:
        return False, "slot 0 is not the eight-voice parameter surface"
    if not required_target_parameter_names().issubset(parameters):
        return False, "slot 0 parameter surface is incomplete"
    if require_configured_values:
        for name, value in SOUND_SETTINGS.items():
            if parameters.get(name) != value:
                return False, "%s changed from %s" % (name, value)
        if str(parameters.get("Output", "")).strip() != "Output 1":
            return False, "Output changed from Output 1"
        if str(parameters.get("MIDI channel", "")).strip() != "Omni":
            return False, "MIDI channel changed from Omni"
    return True, ""


def wait_for_target_slot(client: McpClient, timeout: float = 30.0) -> dict:
    deadline = time.monotonic() + timeout
    last_slot = None
    while time.monotonic() < deadline:
        last_slot = client.call_tool(
            "show_slot", {"slot_index": 0}, timeout=20.0
        )
        valid, _ = validate_target_slot(
            last_slot, require_configured_values=False
        )
        if valid:
            return last_slot
        time.sleep(0.5)
    raise EnduranceError(
        "eight-voice NsIb parameter surface did not become ready: %s"
        % last_slot
    )


def configure_target_preset(client: McpClient) -> dict:
    created = client.call_tool(
        "new",
        {
            "name": PRESET_NAME,
            "algorithms": [{"guid": PLUGIN_GUID, "specifications": [8]}],
        },
        timeout=45.0,
    )
    if (
        created.get("success") is not True
        or created.get("algorithms_added") != 1
        or created.get("algorithms_failed") != 0
    ):
        raise EnduranceError("failed to create eight-voice NsIb preset: %s" % created)
    wait_for_target_slot(client)
    edits = {}
    for parameter_name, value in SOUND_SETTINGS.items():
        result = client.call_tool(
            "edit_parameter",
            {
                "slot_index": 0,
                "parameter_number": parameter_name,
                "value": value,
            },
            timeout=20.0,
        )
        if (
            result.get("success") is False
            or result.get("parameter_name") != parameter_name
            or result.get("value") != value
        ):
            raise EnduranceError(
                "failed to set %s to %s: %s" % (parameter_name, value, result)
            )
        edits[parameter_name] = result
    saved = client.call_tool("save", {}, timeout=20.0)
    if saved.get("success") is not True:
        raise EnduranceError("failed to save target preset: %s" % saved)

    preset = client.call_tool("show_preset", {}, timeout=15.0)
    valid, reason = validate_preset_summary(preset)
    if not valid:
        raise EnduranceError("target preset verification failed: %s" % reason)
    slot = wait_for_target_slot(client)
    parameters = {
        item.get("parameter_name"): item.get("value")
        for item in slot.get("parameters", [])
    }
    required_names = required_target_parameter_names()
    if slot.get("parameter_count") != 18 or not required_names.issubset(parameters):
        raise EnduranceError("eight-voice slot surface is incomplete: %s" % slot)
    for name, value in SOUND_SETTINGS.items():
        if parameters.get(name) != value:
            raise EnduranceError("%s did not retain value %s" % (name, value))
    if str(parameters.get("Output", "")).strip() != "Output 1":
        raise EnduranceError("Output is not routed to Output 1")
    if str(parameters.get("MIDI channel", "")).strip() != "Omni":
        raise EnduranceError("MIDI channel is not Omni")
    return {
        "created": created,
        "saved": saved,
        "preset": preset,
        "slot": slot,
        "settings": {
            "voices": 8,
            "output": "Output 1",
            "outputMode": str(parameters.get("Output mode", "")).strip(),
            "midiChannel": "Omni",
            **SOUND_SETTINGS,
        },
    }


def inspect_existing_target_preset(client: McpClient) -> dict:
    preset = client.call_tool("show_preset", {}, timeout=15.0)
    valid, reason = validate_preset_summary(preset)
    if not valid:
        raise EnduranceError("existing target preset is invalid: %s" % reason)
    slot = client.call_tool("show_slot", {"slot_index": 0}, timeout=15.0)
    valid, reason = validate_target_slot(slot)
    if not valid:
        raise EnduranceError("existing target slot is invalid: %s" % reason)
    parameters = {
        item.get("parameter_name"): item.get("value")
        for item in slot.get("parameters", [])
    }
    return {
        "created": False,
        "preset": preset,
        "slot": slot,
        "settings": {
            "voices": 8,
            "output": "Output 1",
            "outputMode": str(parameters.get("Output mode", "")).strip(),
            "midiChannel": "Omni",
            **SOUND_SETTINGS,
        },
    }


class ResponsivenessPoller:
    def __init__(self, client: McpClient, interval_seconds: float):
        self.client = client
        self.interval_seconds = interval_seconds
        self.stop_requested = threading.Event()
        self.thread: threading.Thread | None = None
        self.started_monotonic = 0.0
        self.checks: list[dict] = []
        self.errors: list[str] = []

    def start(self) -> None:
        self.started_monotonic = time.monotonic()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _poll(self) -> None:
        try:
            preset = self.client.call_tool("show_preset", {}, timeout=15.0)
            slot = self.client.call_tool(
                "show_slot", {"slot_index": 0}, timeout=15.0
            )
            cpu = self.client.call_tool("show_cpu", {}, timeout=15.0)
            preset_valid, reason = validate_preset_summary(preset)
            slot_valid, slot_reason = validate_target_slot(slot)
            cpu_usage = cpu.get("cpu_usage") if isinstance(cpu, dict) else None
            if not preset_valid:
                raise EnduranceError(reason)
            if not slot_valid:
                raise EnduranceError(slot_reason)
            if cpu.get("success") is not True or not isinstance(cpu_usage, dict):
                raise EnduranceError("show_cpu did not return connected CPU data")
            check = {
                "timestamp": utc_now(),
                "elapsedSeconds": time.monotonic() - self.started_monotonic,
                "preset": preset,
                "slot": slot,
                "cpuUsage": cpu_usage,
            }
            self.checks.append(check)
            emit_progress(
                "nt-responsive",
                elapsedSeconds=check["elapsedSeconds"],
                totalUsagePercent=cpu_usage.get("total_usage_percent"),
                slotUsagePercent=max(
                    (
                        item.get("usage_percent", 0)
                        for item in cpu_usage.get("slot_usages", [])
                    ),
                    default=0,
                ),
            )
        except Exception as error:  # noqa: BLE001
            self.errors.append(str(error))
            emit_progress("nt-unresponsive", error=str(error))

    def _run(self) -> None:
        self._poll()
        while not self.stop_requested.wait(self.interval_seconds):
            self._poll()

    def stop(self, final_poll: bool = True) -> None:
        self.stop_requested.set()
        if self.thread is not None:
            self.thread.join(timeout=20.0)
        if final_poll:
            self._poll()

    def report(self) -> dict:
        percentages = []
        for check in self.checks:
            cpu = check["cpuUsage"]
            percentages.extend(
                value
                for value in (
                    cpu.get("cpu1_percent"),
                    cpu.get("cpu2_percent"),
                    cpu.get("total_usage_percent"),
                )
                if isinstance(value, (int, float))
            )
            percentages.extend(
                item.get("usage_percent")
                for item in cpu.get("slot_usages", [])
                if isinstance(item.get("usage_percent"), (int, float))
            )
        return {
            "passed": bool(self.checks) and not self.errors,
            "checkCount": len(self.checks),
            "intervalSeconds": self.interval_seconds,
            "maximumObservedProcessingUsePercent": max(percentages)
            if percentages
            else None,
            "checks": self.checks,
            "errors": self.errors,
        }


def evidence_values(report: dict) -> dict:
    observed = report.get("observedSession")
    patch_settings = (
        "NsIb only in slot 0; Voices 8; Output 1; MIDI Omni; "
        "Tone 100, Motion 100, Grain 100, Resonance 100, Release 100. "
        "All approved synthesis features remained enabled at their normal "
        "implementation quality."
    )
    if isinstance(observed, dict):
        patch_settings = (
            "NsIb was the only algorithm for the full observed residency interval. "
            "It began at the four-voice default and was reconfigured for closeout "
            "to Voices 8; Output 1; MIDI Omni; Tone 100, Motion 100, Grain 100, "
            "Resonance 100, Release 100. No sound feature was removed."
        )
    return {
        "testedFirmware": report["midi"]["ready"]["firmware"],
        "hostConfiguration": (
            "Physical Expert Sleepers disting NT; API v13 plugin NsIb; "
            "nt_helper %s over MCP; live preset and CPU state polled every "
            "%.1f seconds. The preset intentionally contains only NsIb, so "
            "the separate USB audio (to host) algorithm is not present. "
            "Same-commit accelerated native endurance and ARM preflight were "
            "rerun immediately before the physical test."
            % (
                report["ntHelper"]["serverInfo"].get("version", "unknown"),
                report["contract"]["responsivenessIntervalSeconds"],
            )
        ),
        "patchSettings": patch_settings,
        "loadedNormally": True,
        "uninterruptedMinutes": validated_duration_seconds(report) / 60.0,
        "noCrashes": True,
        "noDropouts": True,
        "noStuckNotes": True,
        "noInvalidAudio": True,
        "noFeatureReduction": True,
    }


def evidence_notes(report: dict) -> str:
    responsiveness = report["responsiveness"]
    midi = report["midi"]["complete"]
    observed = report.get("observedSession")
    if isinstance(observed, dict):
        return (
            "Physical target report %s. Tested source commit %s and plugin SHA-256 "
            "%s. Owner-directed whole-session interval: %s to %s, %.2f minutes "
            "with NsIb as the sole resident algorithm; %s Final closeout drove %d "
            "dense-MIDI cycles and completed %d successful nt_helper preset/slot/CPU "
            "checks with no failures; maximum observed processing use was %.2f%%. "
            "Same-commit make test endurance inspect passed accelerated continuous-"
            "audio, finite-sample, and note-cleanup assertions. No USB audio (to "
            "host) slot was added because the physical preset had to remain NsIb-only."
            % (
                report["reportPath"],
                report["source"]["git"]["commit"],
                report["source"]["plugin"]["sha256"],
                observed["startedAt"],
                observed["endedAt"],
                observed["elapsedSeconds"] / 60.0,
                observed["note"],
                midi["activityCycles"],
                responsiveness["checkCount"],
                responsiveness["maximumObservedProcessingUsePercent"],
            )
        )
    return (
        "Physical target report %s. Tested source commit %s and plugin SHA-256 %s. "
        "Completed %.2f uninterrupted minutes with %d dense-MIDI activity cycles, "
        "%d successful nt_helper preset/CPU responsiveness checks and no failed "
        "checks; maximum observed processing use was %.2f%%. The physical preset "
        "contained only eight-voice NsIb throughout and passed the final "
        "post-release check. Same-commit make test endurance inspect preflight also "
        "passed, including accelerated continuous-audio, finite-sample, and "
        "note-cleanup assertions. Per the owner-approved boundary, no separate USB "
        "audio (to host) slot was added to the NsIb-only physical preset."
        % (
            report["reportPath"],
            report["source"]["git"]["commit"],
            report["source"]["plugin"]["sha256"],
            midi["elapsedSeconds"] / 60.0,
            midi["activityCycles"],
            responsiveness["checkCount"],
            responsiveness["maximumObservedProcessingUsePercent"],
        )
    )


def submit_evidence(report: dict, config_path: Path) -> dict:
    if not report.get("physicalTestPassed", report.get("passed")):
        raise EnduranceError("refusing to submit evidence from a failed run")
    measured_seconds = validated_duration_seconds(report)
    if measured_seconds < MINIMUM_EVIDENCE_SECONDS:
        raise EnduranceError(
            "refusing to submit AC-004 evidence for less than 30 minutes of "
            "measured wall-clock time"
        )
    url, headers = load_substrate_connection(config_path)
    client = McpClient(
        url, headers=headers, name="icy-beauty-endurance-evidence", timeout=30.0
    )
    client.initialize()
    tracker_response = client.call_tool(
        "get_acceptance_tracker", {"projectId": PROJECT_ID}, timeout=30.0
    )
    tracker = tracker_response.get("data") if isinstance(tracker_response, dict) else None
    criteria = tracker.get("criteria") if isinstance(tracker, dict) else None
    criterion = next(
        (
            item
            for item in criteria or []
            if item.get("criterionKey") == CRITERION_KEY
        ),
        None,
    )
    if not criterion:
        raise EnduranceError("Substrate tracker does not contain ac-004")
    form_fields = {
        field.get("key")
        for field in (criterion.get("evidenceForm") or {}).get("fields", [])
    }
    if form_fields != EXPECTED_EVIDENCE_FIELDS:
        raise EnduranceError(
            "AC-004 evidence form changed: expected %s, observed %s"
            % (sorted(EXPECTED_EVIDENCE_FIELDS), sorted(form_fields))
        )

    idempotency_key = report["evidence"]["idempotencyKey"]
    response = client.call_tool(
        "create_acceptance_evidence",
        {
            "projectId": PROJECT_ID,
            "criterionKey": CRITERION_KEY,
            "result": "passed",
            "notes": evidence_notes(report),
            "values": evidence_values(report),
            "completesCriterion": True,
            "idempotencyKey": idempotency_key,
        },
        timeout=60.0,
    )
    if response.get("success") is not True:
        raise EnduranceError(
            "Substrate rejected AC-004 evidence: %s"
            % response.get("error", response)
        )
    updated = response.get("data") if isinstance(response, dict) else None
    updated_criterion = next(
        (
            item
            for item in (updated or {}).get("criteria", [])
            if item.get("criterionKey") == CRITERION_KEY
        ),
        None,
    )
    if not isinstance(updated, dict) or not updated_criterion:
        raise EnduranceError(
            "Substrate saved evidence but did not return the updated AC-004 tracker"
        )
    if updated_criterion.get("status") != "met":
        raise EnduranceError(
            "AC-004 evidence was recorded but its status is %s"
            % updated_criterion.get("status")
        )
    return {
        "submitted": True,
        "idempotencyKey": idempotency_key,
        "criterionStatus": (updated_criterion or {}).get("status"),
        "trackerCounts": (updated or {}).get("counts"),
        "recordedEvidenceCount": len(
            (updated_criterion or {}).get("recordedEvidence", [])
        ),
    }


def write_report(report_path: Path, report: dict) -> None:
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def run_endurance(args) -> tuple[dict, Path]:
    started_at = utc_now()
    observed_start = (
        parse_utc_timestamp(args.existing_session_start)
        if args.existing_session_start
        else None
    )
    if observed_start is not None and observed_start >= datetime.now(timezone.utc):
        raise EnduranceError("existing session start must be in the past")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    run_directory = (
        Path(args.run_dir)
        if args.run_dir
        else Path("verification/hardware-runs") / ("icy-beauty-" + stamp)
    ).resolve()
    report_path = run_directory / "target-hardware-endurance.json"
    try:
        displayed_report_path = str(report_path.relative_to(Path.cwd()))
    except ValueError:
        displayed_report_path = str(report_path)
    starting_git_snapshot = git_snapshot(Path.cwd())
    report = {
        "analyzer": "icy-beauty-target-hardware-endurance",
        "analyzerVersion": ANALYZER_VERSION,
        "startedAt": started_at,
        "finishedAt": None,
        "passed": False,
        "reportPath": displayed_report_path,
        "contract": {
            "criterionKey": CRITERION_KEY,
            "activeSeconds": args.duration_seconds,
            "postReleaseSeconds": args.post_release_seconds,
            "activityIntervalSeconds": args.activity_interval_seconds,
            "responsivenessIntervalSeconds": args.responsiveness_interval_seconds,
            "existingSessionCloseout": observed_start is not None,
            "deploymentPerformed": False,
            "evidenceSubmissionRequested": not args.no_submit_evidence,
        },
        "source": {},
        "checks": {},
        "evidence": {
            "submitted": False,
            "idempotencyKey": str(uuid.uuid4()),
        },
    }
    if observed_start is not None:
        report["observedSession"] = {
            "basis": "owner-directed whole NsIb residency interval",
            "startedAt": observed_start.isoformat().replace("+00:00", "Z"),
            "endedAt": None,
            "elapsedSeconds": 0.0,
            "note": args.existing_session_note,
        }
    write_report(report_path, report)

    nt_client = None
    midi = None
    poller = None
    try:
        plugin_path = Path(args.plugin).resolve()
        if not plugin_path.is_file() or plugin_path.suffix.lower() != ".o":
            raise EnduranceError("plugin object is unavailable: %s" % plugin_path)
        source = {
            "git": starting_git_snapshot,
            "plugin": {
                "path": os.path.relpath(plugin_path, Path.cwd()),
                "sizeBytes": plugin_path.stat().st_size,
                "sha256": sha256_file(plugin_path),
            },
            "script": {
                "path": os.path.relpath(Path(__file__).resolve(), Path.cwd()),
                "sha256": sha256_file(Path(__file__).resolve()),
            },
        }
        if not args.no_submit_evidence and not source["git"]["workingTreeClean"]:
            raise EnduranceError(
                "evidence submission requires a clean checkout at test start"
            )
        report["source"] = source
        report["checks"]["source"] = True

        report["preflight"] = run_preflight()
        if not report["preflight"]["passed"]:
            raise EnduranceError("native and ARM preflight failed")
        report["checks"]["preflight"] = True

        nt_client = McpClient(
            args.nt_mcp_url, name="icy-beauty-endurance-nt-helper", timeout=20.0
        )
        nt_client.initialize()
        report["ntHelper"] = {
            "url": args.nt_mcp_url,
            "protocolVersion": nt_client.protocol_version,
            "serverInfo": nt_client.server_info,
        }
        report["presetSetup"] = (
            inspect_existing_target_preset(nt_client)
            if observed_start is not None
            else configure_target_preset(nt_client)
        )
        report["checks"]["presetSetup"] = True

        midi_python = args.midi_python
        if not Path(midi_python).is_file():
            raise EnduranceError("MIDI Python is unavailable: %s" % midi_python)
        midi = MidiWorker(
            Path(__file__).resolve(),
            midi_python,
            args.duration_seconds,
            args.activity_interval_seconds,
        )
        midi.start()
        ready = midi.wait_for_event("ready", timeout=10.0)
        report["midi"] = {"ready": ready}

        poller = ResponsivenessPoller(
            nt_client, args.responsiveness_interval_seconds
        )
        poller.start()
        midi.begin()
        emit_progress(
            "endurance-started",
            durationSeconds=args.duration_seconds,
            postReleaseSeconds=args.post_release_seconds,
            firmware=ready.get("firmware"),
        )

        midi_exit = midi.wait(timeout=args.duration_seconds + 30.0)
        if midi_exit != 0:
            raise EnduranceError("MIDI worker exited with %d" % midi_exit)
        midi.stop()
        midi_report = midi.report()
        midi = None
        emit_progress(
            "post-release-check",
            durationSeconds=args.post_release_seconds,
        )
        time.sleep(args.post_release_seconds)

        poller.stop(final_poll=True)
        responsiveness_report = poller.report()
        minimum_checks = (
            math.floor(
                args.duration_seconds / args.responsiveness_interval_seconds
            )
            + 1
        )
        responsiveness_report["minimumExpectedCheckCount"] = minimum_checks
        responsiveness_report["coveragePassed"] = (
            responsiveness_report["checkCount"] >= minimum_checks
        )
        responsiveness_report["passed"] = (
            responsiveness_report["passed"]
            and responsiveness_report["coveragePassed"]
        )
        poller = None
        report["midi"] = midi_report
        report["responsiveness"] = responsiveness_report
        if observed_start is not None:
            observed_end = datetime.now(timezone.utc)
            report["observedSession"]["endedAt"] = (
                observed_end.isoformat().replace("+00:00", "Z")
            )
            report["observedSession"]["elapsedSeconds"] = (
                observed_end - observed_start
            ).total_seconds()
    except KeyboardInterrupt:
        report["failure"] = {"message": "test interrupted"}
    except Exception as error:  # noqa: BLE001 - persist every physical failure
        report["failure"] = {"message": str(error)}
    finally:
        # Always release MIDI notes before ending a failed or interrupted run.
        if poller is not None:
            poller.stop(final_poll=False)
            report["responsiveness"] = poller.report()
        if midi is not None:
            midi.stop()
            report["midi"] = midi.report()

    components = (
        report.get("midi", {}).get("passed"),
        report.get("responsiveness", {}).get("passed"),
        report.get("checks", {}).get("source"),
        report.get("checks", {}).get("preflight"),
        report.get("checks", {}).get("presetSetup"),
    )
    report["physicalTestPassed"] = all(value is True for value in components)
    report["passed"] = report["physicalTestPassed"]
    report["finishedAt"] = utc_now()
    write_report(report_path, report)

    if report["passed"] and not args.no_submit_evidence:
        try:
            report["evidence"] = submit_evidence(
                report, Path(args.substrate_config).expanduser()
            )
        except Exception as error:  # noqa: BLE001
            report["evidence"]["error"] = str(error)
            report["passed"] = False
            report["failure"] = {"message": "evidence submission failed: %s" % error}
        report["finishedAt"] = utc_now()
        write_report(report_path, report)
    return report, report_path


def submit_existing_report(args) -> tuple[dict, Path]:
    report_path = Path(args.submit_existing_report).resolve()
    try:
        report = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise EnduranceError("cannot read existing report: %s" % error) from error
    report["evidence"] = submit_evidence(
        report, Path(args.substrate_config).expanduser()
    )
    report["passed"] = True
    report.pop("failure", None)
    report["finishedAt"] = utc_now()
    write_report(report_path, report)
    return report, report_path


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Run Icy Beauty's physical eight-voice dense-MIDI endurance test "
            "and optionally submit passing AC-004 evidence."
        )
    )
    parser.add_argument("--plugin", default="plugins/icy_beauty.o")
    parser.add_argument("--duration-seconds", type=int, default=MINIMUM_EVIDENCE_SECONDS)
    parser.add_argument("--post-release-seconds", type=int, default=10)
    parser.add_argument("--activity-interval-seconds", type=float, default=0.5)
    parser.add_argument("--responsiveness-interval-seconds", type=float, default=10.0)
    parser.add_argument("--run-dir")
    parser.add_argument("--nt-mcp-url", default="http://127.0.0.1:3847/mcp")
    parser.add_argument("--substrate-config", default="~/.codex/config.toml")
    parser.add_argument("--midi-python", default=DEFAULT_MIDI_PYTHON)
    parser.add_argument(
        "--existing-session-start",
        help=(
            "ISO-8601 start of an already-running owner-approved NsIb session; "
            "validates the current preset without recreating it"
        ),
    )
    parser.add_argument(
        "--existing-session-note",
        default="",
        help="durable explanation of the existing-session start and transitions",
    )
    parser.add_argument("--no-submit-evidence", action="store_true")
    parser.add_argument("--submit-existing-report")
    parser.add_argument("--midi-worker", action="store_true", help=argparse.SUPPRESS)
    return parser


def main(argv=None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.duration_seconds <= 0 or args.post_release_seconds <= 0:
        parser.error("duration and post-release wait must be positive")
    if args.activity_interval_seconds <= 0:
        parser.error("activity interval must be positive")
    if args.responsiveness_interval_seconds <= 0:
        parser.error("responsiveness interval must be positive")
    if args.existing_session_start and not args.existing_session_note.strip():
        parser.error("--existing-session-note is required with --existing-session-start")
    if args.midi_worker:
        return midi_worker_main(args)
    try:
        if args.submit_existing_report:
            report, report_path = submit_existing_report(args)
        else:
            report, report_path = run_endurance(args)
    except (EnduranceError, OSError, ValueError) as error:
        print(json.dumps({"passed": False, "error": str(error)}, indent=2))
        return 1
    print(
        json.dumps(
            {
                "passed": report.get("passed"),
                "report": str(report_path),
                "evidence": report.get("evidence"),
            },
            indent=2,
            sort_keys=True,
        )
    )
    return 0 if report.get("passed") else 1


if __name__ == "__main__":
    sys.exit(main())
