#!/usr/bin/env python3
"""Measure and record physical MIDI-note-on latency for Icy Beauty AC-005.

The test temporarily adds the disting NT USB audio (to host) algorithm after
the existing eight-voice NsIb slot, routes USB channel 1 from NsIb's output,
captures that channel while sending isolated CoreMIDI note-ons, and restores
the original NsIb-only preset in all exit paths.
"""

from __future__ import annotations

import argparse
import array
import bisect
import json
import math
import os
import shlex
import shutil
import subprocess
import sys
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

from target_hardware_endurance import (
    McpClient,
    git_snapshot,
    load_substrate_connection,
    run_command,
    sha256_file,
    utc_now,
    validate_target_slot,
)


ANALYZER_VERSION = 1
PROJECT_ID = "0966eaa1-b61c-4a7f-a172-d7a37df994dd"
CRITERION_KEY = "ac-005"
PLUGIN_GUID = "NsIb"
USB_AUDIO_GUID = "usbt"
PRESET_NAME = "Icy Beauty"
DEVICE_NAME = "disting NT"
SAMPLE_RATE = 48_000
INPUT_CHANNELS = 12
SELECTED_INPUT = 1
MINIMUM_EVIDENCE_TRIALS = 8
LATENCY_LIMIT_MS = 10.0
PROCESSING_LIMIT_PERCENT = 75.0
BASELINE_WINDOW_SECONDS = 0.050
BASELINE_GUARD_SECONDS = 0.005
SEARCH_WINDOW_SECONDS = 0.050
SOUNDING_WINDOW_SECONDS = 0.020
MINIMUM_SIGNAL_PEAK = 0.005
MINIMUM_SEPARATION_DB = 60.0
DETECTION_WINDOW_SAMPLES = 4
MAXIMUM_TIMESTAMP_DISCONTINUITY_SECONDS = 0.000250
EXPECTED_EVIDENCE_FIELDS = {
    "maxProcessingUsePercent",
    "maxMidiNoteOnLatencyMs",
}
DEFAULT_PROCESSING_REPORT = (
    "verification/hardware-runs/icy-beauty-20260729T205925Z/"
    "target-hardware-endurance.json"
)


class LatencyError(RuntimeError):
    """Raised when a physical latency result cannot be trusted."""


def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def normalize_nt_value(value):
    if isinstance(value, str):
        return value.rstrip()
    if isinstance(value, list):
        return [normalize_nt_value(item) for item in value]
    if isinstance(value, dict):
        return {
            key: normalize_nt_value(item)
            for key, item in value.items()
        }
    return value


def slot_parameter_map(slot: dict) -> dict:
    return {
        item.get("parameter_name"): normalize_nt_value(item.get("value"))
        for item in slot.get("parameters", [])
        if isinstance(item, dict) and isinstance(item.get("parameter_name"), str)
    }


def canonical_slot(slot: dict) -> dict:
    algorithm = slot.get("algorithm") or {}
    return {
        "slotIndex": slot.get("slot_index"),
        "guid": algorithm.get("guid"),
        "name": normalize_nt_value(algorithm.get("name")),
        "parameterCount": slot.get("parameter_count"),
        "parameters": slot_parameter_map(slot),
    }


def validate_initial_preset(preset: dict, slot: dict) -> tuple[bool, str]:
    slots = preset.get("slots") if isinstance(preset, dict) else None
    populated = [item for item in slots or [] if isinstance(item, dict)]
    if normalize_nt_value(preset.get("name")) != PRESET_NAME:
        return False, "active preset is not Icy Beauty"
    if len(populated) != 1:
        return False, "expected the active preset to contain only NsIb"
    algorithm = populated[0].get("algorithm") or {}
    if populated[0].get("slot_index") != 0 or algorithm.get("guid") != PLUGIN_GUID:
        return False, "slot 0 is not NsIb"
    valid, reason = validate_target_slot(slot)
    if not valid:
        return False, reason
    return True, ""


def wait_for_slot(
    client: McpClient, slot_index: int, guid: str, timeout: float = 20.0
) -> dict:
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = client.call_tool(
            "show_slot", {"slot_index": slot_index}, timeout=15.0
        )
        algorithm = last.get("algorithm") if isinstance(last, dict) else None
        if isinstance(algorithm, dict) and algorithm.get("guid") == guid:
            return last
        time.sleep(0.25)
    raise LatencyError(
        "slot %d did not become %s: %s" % (slot_index, guid, last)
    )


def edit_and_verify_parameter(
    client: McpClient, slot_index: int, name: str, value
) -> dict:
    edit = client.call_tool(
        "edit_parameter",
        {
            "slot_index": slot_index,
            "parameter_number": name,
            "value": value,
        },
        timeout=20.0,
    )
    readback = client.call_tool(
        "show_parameter",
        {"slot_index": slot_index, "parameter_number": name},
        timeout=15.0,
    )
    observed = normalize_nt_value(readback.get("value"))
    if observed != value:
        raise LatencyError(
            "%s read back as %r after setting %r" % (name, observed, value)
        )
    return {"edit": edit, "readback": readback, "observed": observed}


def add_usb_capture(client: McpClient, source_output: str) -> dict:
    add = client.call_tool(
        "add", {"guid": USB_AUDIO_GUID, "slot_index": 1}, timeout=30.0
    )
    slot = wait_for_slot(client, 1, USB_AUDIO_GUID)
    edits = {
        "USB channel 1 from": edit_and_verify_parameter(
            client, 1, "USB channel 1 from", source_output
        )
    }
    for channel in range(2, INPUT_CHANNELS + 1):
        name = "USB channel %d from" % channel
        edits[name] = edit_and_verify_parameter(client, 1, name, "None")

    saved = client.call_tool("save", {}, timeout=20.0)
    if saved.get("success") is not True:
        raise LatencyError("temporary USB capture topology did not save: %s" % saved)

    preset = client.call_tool("show_preset", {}, timeout=15.0)
    nsib_slot = client.call_tool("show_slot", {"slot_index": 0}, timeout=15.0)
    usb_slot = client.call_tool("show_slot", {"slot_index": 1}, timeout=15.0)
    routing = client.call_tool("show_routing", {}, timeout=15.0)
    populated = [
        item for item in preset.get("slots", []) if isinstance(item, dict)
    ]
    if (
        len(populated) != 2
        or [item.get("slot_index") for item in populated] != [0, 1]
        or [
            (item.get("algorithm") or {}).get("guid")
            for item in populated
        ]
        != [PLUGIN_GUID, USB_AUDIO_GUID]
    ):
        raise LatencyError("temporary topology is not NsIb then usbt")
    nsib_parameters = slot_parameter_map(nsib_slot)
    usb_parameters = slot_parameter_map(usb_slot)
    if nsib_parameters.get("Output") != source_output:
        raise LatencyError("NsIb output changed during capture setup")
    if usb_parameters.get("USB channel 1 from") != source_output:
        raise LatencyError("USB channel 1 is not fed by the NsIb output")
    for channel in range(2, INPUT_CHANNELS + 1):
        if usb_parameters.get("USB channel %d from" % channel) != "None":
            raise LatencyError("USB capture has an unexpected additional channel")
    return {
        "add": add,
        "edits": edits,
        "save": saved,
        "preset": preset,
        "nsibSlot": nsib_slot,
        "usbSlot": usb_slot,
        "routing": routing,
        "sourceOutput": source_output,
        "hostCaptureInput": SELECTED_INPUT,
    }


def restore_nsib_only(
    client: McpClient, original_preset: dict, original_slot: dict
) -> dict:
    before = client.call_tool("show_preset", {}, timeout=15.0)
    slot_one = next(
        (
            item
            for item in before.get("slots", [])
            if isinstance(item, dict) and item.get("slot_index") == 1
        ),
        None,
    )
    removed = None
    if slot_one is not None:
        guid = (slot_one.get("algorithm") or {}).get("guid")
        if guid != USB_AUDIO_GUID:
            raise LatencyError(
                "refusing to remove unexpected slot 1 algorithm %r" % guid
            )
        removed = client.call_tool(
            "remove", {"slot_index": 1}, timeout=30.0
        )
    saved = client.call_tool("save", {}, timeout=20.0)
    if saved.get("success") is not True:
        raise LatencyError("restored NsIb-only preset did not save: %s" % saved)
    after = client.call_tool("show_preset", {}, timeout=15.0)
    slot = client.call_tool("show_slot", {"slot_index": 0}, timeout=15.0)
    if normalize_nt_value(after) != normalize_nt_value(original_preset):
        raise LatencyError("preset summary did not return to its original state")
    if canonical_slot(slot) != canonical_slot(original_slot):
        raise LatencyError("NsIb slot did not return to its original state")
    return {
        "before": before,
        "remove": removed,
        "save": saved,
        "after": after,
        "slot": slot,
        "restoredExactly": True,
    }


def build_capture_app(
    source_path: Path, plist_path: Path, app_path: Path
) -> dict:
    clang = shutil.which("clang")
    pkg_config = shutil.which("pkg-config")
    if not clang or not pkg_config:
        raise LatencyError("clang and pkg-config are required")
    flags = run_command(
        [pkg_config, "--cflags", "--libs", "portaudio-2.0"], timeout=10.0
    )
    if flags.returncode != 0:
        raise LatencyError(
            "pkg-config could not resolve portaudio-2.0: %s"
            % flags.stderr.strip()[:1000]
        )
    executable_path = (
        app_path / "Contents" / "MacOS" / "IcyBeautyLatencyCapture"
    )
    info_plist_path = app_path / "Contents" / "Info.plist"
    executable_path.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(plist_path, info_plist_path)
    command = [
        clang,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Wconversion",
        "-Werror",
        str(source_path),
        *shlex.split(flags.stdout),
        "-framework",
        "CoreMIDI",
        "-framework",
        "CoreFoundation",
        "-lm",
        "-o",
        str(executable_path),
    ]
    compiled = run_command(command, timeout=30.0)
    if compiled.returncode != 0 or not executable_path.is_file():
        raise LatencyError(
            "native capture helper failed to build: %s"
            % compiled.stderr.strip()[:3000]
        )
    codesign = shutil.which("codesign")
    if not codesign:
        raise LatencyError("codesign is required for the foreground capture app")
    sign_command = [
        codesign,
        "--force",
        "--sign",
        "-",
        str(app_path),
    ]
    signed = run_command(sign_command, timeout=30.0)
    if signed.returncode != 0:
        raise LatencyError(
            "capture app signing failed: %s" % signed.stderr.strip()[:2000]
        )
    verify_command = [
        codesign,
        "--verify",
        "--strict",
        "--verbose=2",
        str(app_path),
    ]
    verified = run_command(verify_command, timeout=15.0)
    if verified.returncode != 0:
        raise LatencyError(
            "capture app signature did not verify: %s"
            % verified.stderr.strip()[:2000]
        )
    return {
        "path": str(app_path),
        "executable": {
            "path": str(executable_path),
            "sha256": sha256_file(executable_path),
        },
        "infoPlist": {
            "path": str(info_plist_path),
            "sha256": sha256_file(info_plist_path),
        },
        "adHocSigned": True,
        "source": {
            "path": str(source_path),
            "sha256": sha256_file(source_path),
        },
        "compileCommand": command,
        "signCommand": sign_command,
        "verifyCommand": verify_command,
    }


def run_native_capture(
    app_path: Path,
    raw_path: Path,
    timing_path: Path,
    *,
    trials: int,
    settle_seconds: float,
    interval_seconds: float,
    note_duration_seconds: float,
    tail_seconds: float,
    note: int,
    velocity: int,
) -> tuple[dict, dict]:
    command = [
        "/usr/bin/open",
        "-W",
        "-n",
        str(app_path),
        "--args",
        "--device",
        DEVICE_NAME,
        "--destination",
        DEVICE_NAME,
        "--raw",
        str(raw_path),
        "--timing",
        str(timing_path),
        "--trials",
        str(trials),
        "--settle-seconds",
        str(settle_seconds),
        "--interval-seconds",
        str(interval_seconds),
        "--note-duration-seconds",
        str(note_duration_seconds),
        "--tail-seconds",
        str(tail_seconds),
        "--note",
        str(note),
        "--velocity",
        str(velocity),
    ]
    timeout = (
        settle_seconds
        + (trials - 1) * interval_seconds
        + note_duration_seconds
        + tail_seconds
        + 30.0
    )
    try:
        completed = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        run_command(
            ["/usr/bin/pkill", "-x", "IcyBeautyLatencyCapture"],
            timeout=5.0,
        )
        raise LatencyError(
            "foreground capture app timed out after %.1f seconds" % timeout
        ) from error
    try:
        timing = json.loads(timing_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise LatencyError(
            "foreground capture app did not produce valid timing JSON"
        ) from error
    process = {
        "command": command,
        "exitCode": completed.returncode,
        "stderr": completed.stderr.strip()[-4000:],
    }
    if (
        completed.returncode != 0
        or timing.get("passed") is not True
        or not raw_path.is_file()
    ):
        raise LatencyError(
            "native MIDI/audio capture failed: open exit %d, %s"
            % (completed.returncode, completed.stderr.strip()[-2000:])
        )
    return timing, process


def read_f32le(path: Path, expected_samples: int) -> array.array:
    samples = array.array("f")
    with path.open("rb") as source:
        samples.fromfile(source, expected_samples)
        if source.read(1):
            raise LatencyError("raw capture contains more samples than reported")
    if sys.byteorder != "little":
        samples.byteswap()
    if len(samples) != expected_samples:
        raise LatencyError(
            "raw capture has %d samples; expected %d"
            % (len(samples), expected_samples)
        )
    return samples


def validate_blocks(blocks: list[dict], sample_count: int) -> dict:
    if not blocks:
        raise LatencyError("capture timing contains no audio blocks")
    expected_frame = 0
    previous_end_time = None
    discontinuities = []
    starts = []
    for index, block in enumerate(blocks):
        start_frame = block.get("startFrame")
        frame_count = block.get("frameCount")
        adc_time = block.get("adcTime")
        if (
            not isinstance(start_frame, int)
            or not isinstance(frame_count, int)
            or not isinstance(adc_time, (int, float))
            or frame_count <= 0
            or not math.isfinite(adc_time)
        ):
            raise LatencyError("audio block %d is malformed" % index)
        if start_frame != expected_frame:
            raise LatencyError("audio block frames are not contiguous")
        if block.get("statusFlags") != 0:
            raise LatencyError("audio block %d reports callback flags" % index)
        if previous_end_time is not None:
            discontinuity = adc_time - previous_end_time
            discontinuities.append(discontinuity)
            if adc_time <= starts[-1]:
                raise LatencyError("audio block start timestamps move backwards")
        starts.append(float(adc_time))
        expected_frame += frame_count
        previous_end_time = float(adc_time) + frame_count / SAMPLE_RATE
    if expected_frame != sample_count:
        raise LatencyError("audio block coverage does not match sample count")
    maximum_absolute = max(
        (abs(value) for value in discontinuities), default=0.0
    )
    if maximum_absolute > MAXIMUM_TIMESTAMP_DISCONTINUITY_SECONDS:
        raise LatencyError(
            "audio clock discontinuity %.3f ms exceeds %.3f ms"
            % (
                maximum_absolute * 1000.0,
                MAXIMUM_TIMESTAMP_DISCONTINUITY_SECONDS * 1000.0,
            )
        )
    return {
        "blockStartTimes": starts,
        "maximumAbsoluteDiscontinuitySeconds": maximum_absolute,
        "minimumDiscontinuitySeconds": min(discontinuities, default=0.0),
        "maximumDiscontinuitySeconds": max(discontinuities, default=0.0),
    }


def samples_in_window(
    samples: array.array,
    blocks: list[dict],
    block_starts: list[float],
    start_time: float,
    end_time: float,
) -> list[tuple[float, float]]:
    if end_time <= start_time:
        return []
    first = max(0, bisect.bisect_right(block_starts, start_time) - 1)
    selected = []
    for block in blocks[first:]:
        adc_time = float(block["adcTime"])
        frame_count = int(block["frameCount"])
        block_end = adc_time + frame_count / SAMPLE_RATE
        if adc_time >= end_time:
            break
        if block_end <= start_time:
            continue
        local_start = max(0, math.ceil((start_time - adc_time) * SAMPLE_RATE))
        local_end = min(
            frame_count, math.ceil((end_time - adc_time) * SAMPLE_RATE)
        )
        absolute_start = int(block["startFrame"])
        for frame in range(local_start, local_end):
            selected.append(
                (
                    adc_time + frame / SAMPLE_RATE,
                    float(samples[absolute_start + frame]),
                )
            )
    return selected


def rms(values: list[float]) -> float:
    if not values:
        return 0.0
    return math.sqrt(math.fsum(value * value for value in values) / len(values))


def analyze_capture(samples: array.array, timing: dict) -> dict:
    if timing.get("sampleRate") != SAMPLE_RATE:
        raise LatencyError("capture sample rate is not 48 kHz")
    if timing.get("deviceInputChannels") != INPUT_CHANNELS:
        raise LatencyError("capture did not open all 12 NT input channels")
    if timing.get("selectedInput") != SELECTED_INPUT:
        raise LatencyError("capture did not select USB host input 1")
    if timing.get("callbackStatusFlags") != 0:
        raise LatencyError("PortAudio reported an input overflow or underflow")
    if timing.get("nullInputCount") != 0 or timing.get("capacityExceeded"):
        raise LatencyError("PortAudio capture was incomplete")

    blocks = timing.get("blocks")
    trials = timing.get("trials")
    if not isinstance(blocks, list) or not isinstance(trials, list):
        raise LatencyError("capture timing omitted blocks or trials")
    integrity = validate_blocks(blocks, len(samples))
    block_starts = integrity.pop("blockStartTimes")
    timestamp_uncertainty = integrity[
        "maximumAbsoluteDiscontinuitySeconds"
    ]
    results = []
    for trial in trials:
        send_before = trial.get("sendBeforeTime")
        send_after = trial.get("sendAfterTime")
        if (
            not isinstance(send_before, (int, float))
            or not isinstance(send_after, (int, float))
            or send_after < send_before
        ):
            raise LatencyError("MIDI send timing is malformed")
        baseline = samples_in_window(
            samples,
            blocks,
            block_starts,
            send_before - BASELINE_WINDOW_SECONDS,
            send_before - BASELINE_GUARD_SECONDS,
        )
        search = samples_in_window(
            samples,
            blocks,
            block_starts,
            send_before,
            send_before + SEARCH_WINDOW_SECONDS,
        )
        if len(baseline) < 1000 or len(search) < 1000:
            raise LatencyError("capture does not cover a trial analysis window")
        baseline_values = [value for _, value in baseline]
        baseline_peak = max(abs(value) for value in baseline_values)
        baseline_rms = rms(baseline_values)
        threshold = max(
            0.0001,
            baseline_peak * 8.0,
            baseline_rms * 32.0,
        )

        onset_index = None
        for index in range(
            0, len(search) - DETECTION_WINDOW_SAMPLES + 1
        ):
            window = [
                search[index + offset][1]
                for offset in range(DETECTION_WINDOW_SAMPLES)
            ]
            if (
                max(abs(value) for value in window) >= threshold
                and rms(window) >= threshold / 2.0
            ):
                onset_index = index
                break
        if onset_index is None:
            raise LatencyError(
                "trial %s produced no detectable audio onset"
                % trial.get("index")
            )

        onset_time = search[onset_index][0]
        confirmation_time = (
            search[onset_index + DETECTION_WINDOW_SAMPLES - 1][0]
            + 1.0 / SAMPLE_RATE
        )
        sounding = samples_in_window(
            samples,
            blocks,
            block_starts,
            onset_time,
            onset_time + SOUNDING_WINDOW_SECONDS,
        )
        sounding_values = [value for _, value in sounding]
        sounding_peak = max(abs(value) for value in sounding_values)
        sounding_rms = rms(sounding_values)
        separation_db = 20.0 * math.log10(
            sounding_rms / max(baseline_rms, 1.0e-12)
        )
        send_bracket_ms = (send_after - send_before) * 1000.0
        point_latency_ms = (onset_time - send_after) * 1000.0
        conservative_upper_ms = (
            confirmation_time - send_before + timestamp_uncertainty
        ) * 1000.0
        passed = (
            sounding_peak >= MINIMUM_SIGNAL_PEAK
            and separation_db >= MINIMUM_SEPARATION_DB
            and conservative_upper_ms < LATENCY_LIMIT_MS
            and conservative_upper_ms >= 0.0
        )
        results.append(
            {
                "index": trial.get("index"),
                "note": trial.get("note"),
                "velocity": trial.get("velocity"),
                "sendBeforeTime": send_before,
                "sendAfterTime": send_after,
                "sendBracketMilliseconds": send_bracket_ms,
                "baselinePeak": baseline_peak,
                "baselineRms": baseline_rms,
                "detectionThreshold": threshold,
                "onsetAdcTime": onset_time,
                "onsetConfirmationAdcTime": confirmation_time,
                "pointLatencyMilliseconds": point_latency_ms,
                "conservativeUpperLatencyMilliseconds": (
                    conservative_upper_ms
                ),
                "timestampUncertaintyMilliseconds": (
                    timestamp_uncertainty * 1000.0
                ),
                "soundingPeak": sounding_peak,
                "soundingRms": sounding_rms,
                "signalToBaselineDb": separation_db,
                "passed": passed,
            }
        )

    maximum = max(
        item["conservativeUpperLatencyMilliseconds"] for item in results
    )
    return {
        "passed": bool(results) and all(item["passed"] for item in results),
        "trialCount": len(results),
        "latencyLimitMilliseconds": LATENCY_LIMIT_MS,
        "maximumConservativeLatencyMilliseconds": maximum,
        "measurementResolutionMilliseconds": 1000.0 / SAMPLE_RATE,
        "timestampUncertaintyMilliseconds": timestamp_uncertainty * 1000.0,
        "detectionWindowSamples": DETECTION_WINDOW_SAMPLES,
        "minimumSignalPeak": MINIMUM_SIGNAL_PEAK,
        "minimumSignalToBaselineDb": MINIMUM_SEPARATION_DB,
        "timingIntegrity": integrity,
        "trials": results,
    }


def convert_capture_to_wav(raw_path: Path, wav_path: Path) -> dict:
    ffmpeg = shutil.which("ffmpeg")
    ffprobe = shutil.which("ffprobe")
    if not ffmpeg or not ffprobe:
        raise LatencyError("ffmpeg and ffprobe are required")
    command = [
        ffmpeg,
        "-nostdin",
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-f",
        "f32le",
        "-ar",
        str(SAMPLE_RATE),
        "-ac",
        "1",
        "-i",
        str(raw_path),
        "-c:a",
        "pcm_s24le",
        str(wav_path),
    ]
    converted = run_command(command, timeout=30.0)
    if converted.returncode != 0 or not wav_path.is_file():
        raise LatencyError(
            "FFmpeg capture conversion failed: %s"
            % converted.stderr.strip()[:2000]
        )
    probe_command = [
        ffprobe,
        "-v",
        "error",
        "-select_streams",
        "a:0",
        "-show_entries",
        "stream=codec_name,sample_rate,channels,bits_per_sample,duration",
        "-of",
        "json",
        str(wav_path),
    ]
    probed = run_command(probe_command, timeout=15.0)
    try:
        probe = json.loads(probed.stdout)
        stream = probe["streams"][0]
    except (json.JSONDecodeError, KeyError, IndexError) as error:
        raise LatencyError("FFprobe returned invalid WAV metadata") from error
    if (
        probed.returncode != 0
        or stream.get("codec_name") != "pcm_s24le"
        or stream.get("sample_rate") != str(SAMPLE_RATE)
        or stream.get("channels") != 1
        or stream.get("bits_per_sample") != 24
    ):
        raise LatencyError("captured WAV is not 48 kHz, 24-bit PCM mono")
    return {
        "path": str(wav_path),
        "sha256": sha256_file(wav_path),
        "sizeBytes": wav_path.stat().st_size,
        "command": command,
        "probe": stream,
    }


def processing_evidence(path: Path) -> dict:
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise LatencyError("cannot read retained processing report: %s" % error)
    checks = report.get("responsiveness", {}).get("checks", [])
    values = []
    for check in checks:
        cpu = check.get("cpuUsage", {})
        values.extend(
            cpu.get(name)
            for name in ("cpu1_percent", "cpu2_percent", "total_usage_percent")
            if isinstance(cpu.get(name), (int, float))
        )
        values.extend(
            item.get("usage_percent")
            for item in cpu.get("slot_usages", [])
            if isinstance(item.get("usage_percent"), (int, float))
        )
    maximum = max(values, default=None)
    if (
        report.get("passed") is not True
        or len(checks) < 1
        or not isinstance(maximum, (int, float))
        or maximum > PROCESSING_LIMIT_PERCENT
    ):
        raise LatencyError("retained physical processing evidence does not pass")
    return {
        "path": str(path),
        "sha256": sha256_file(path),
        "sourceCommit": report.get("source", {}).get("git", {}).get("commit"),
        "physicalTestPassed": report.get("physicalTestPassed"),
        "durationSeconds": report.get("midi", {})
        .get("complete", {})
        .get("elapsedSeconds"),
        "checkCount": len(checks),
        "measurementCount": len(values),
        "maximumObservedProcessingUsePercent": maximum,
        "limitPercent": PROCESSING_LIMIT_PERCENT,
        "passed": True,
    }


def round_up(value: float, decimal_places: int = 3) -> float:
    scale = 10**decimal_places
    return math.ceil(value * scale) / scale


def evidence_notes(report: dict) -> str:
    latency = report["latencyAnalysis"]
    processing = report["processingEvidence"]
    wav = report["capture"]["wav"]
    trial_values = ", ".join(
        "%.3f" % item["conservativeUpperLatencyMilliseconds"]
        for item in latency["trials"]
    )
    return (
        "Physical target report %s. On the same approved eight-voice maximum-"
        "control NsIb patch, a temporary USB audio (to host) slot routed USB "
        "channel 1 from NsIb %s and the host captured disting NT input 1 at "
        "48 kHz/24-bit mono. One native process bracketed each CoreMIDI send "
        "and timestamped every PortAudio input block on PortAudio's shared "
        "stream clock. All %d isolated note-ons passed; conservative upper "
        "latencies were [%s] ms, maximum %.3f ms below the 10 ms limit. "
        "The bound uses the earliest possible send time and the end of the "
        "%d-sample onset-confirmation window. WAV SHA-256 %s. The temporary "
        "USB slot was removed and the original NsIb-only preset was verified "
        "exactly restored. Retained 30-minute report %s has %d checks and a "
        "%.2f%% maximum observed processing use, below 75%%. Tested source "
        "commit %s."
        % (
            report["reportPath"],
            report["captureTopology"]["sourceOutput"],
            latency["trialCount"],
            trial_values,
            latency["maximumConservativeLatencyMilliseconds"],
            latency["detectionWindowSamples"],
            wav["sha256"],
            processing["path"],
            processing["checkCount"],
            processing["maximumObservedProcessingUsePercent"],
            report["source"]["git"]["commit"],
        )
    )


def submit_evidence(report: dict, config_path: Path) -> dict:
    latency = report.get("latencyAnalysis", {})
    processing = report.get("processingEvidence", {})
    if report.get("physicalTestPassed") is not True:
        raise LatencyError("refusing evidence from a failed physical test")
    if latency.get("trialCount", 0) < MINIMUM_EVIDENCE_TRIALS:
        raise LatencyError("refusing evidence with fewer than eight trials")
    if (
        latency.get("passed") is not True
        or latency.get("maximumConservativeLatencyMilliseconds", math.inf)
        >= LATENCY_LIMIT_MS
    ):
        raise LatencyError("refusing evidence without sub-10 ms physical latency")
    if (
        processing.get("passed") is not True
        or processing.get("maximumObservedProcessingUsePercent", math.inf)
        > PROCESSING_LIMIT_PERCENT
    ):
        raise LatencyError("refusing evidence without passing processing data")
    if report.get("presetRestoration", {}).get("restoredExactly") is not True:
        raise LatencyError("refusing evidence before exact preset restoration")

    url, headers = load_substrate_connection(config_path)
    client = McpClient(
        url, headers=headers, name="icy-beauty-latency-evidence", timeout=30.0
    )
    client.initialize()
    tracker_response = client.call_tool(
        "get_acceptance_tracker", {"projectId": PROJECT_ID}, timeout=30.0
    )
    tracker = (
        tracker_response.get("data")
        if isinstance(tracker_response, dict)
        else None
    )
    criterion = next(
        (
            item
            for item in (tracker or {}).get("criteria", [])
            if item.get("criterionKey") == CRITERION_KEY
        ),
        None,
    )
    if not criterion:
        raise LatencyError("Substrate tracker does not contain ac-005")
    form_fields = {
        field.get("key")
        for field in (criterion.get("evidenceForm") or {}).get("fields", [])
    }
    if form_fields != EXPECTED_EVIDENCE_FIELDS:
        raise LatencyError(
            "AC-005 evidence form changed: expected %s, observed %s"
            % (sorted(EXPECTED_EVIDENCE_FIELDS), sorted(form_fields))
        )

    values = {
        "maxProcessingUsePercent": processing[
            "maximumObservedProcessingUsePercent"
        ],
        "maxMidiNoteOnLatencyMs": round_up(
            latency["maximumConservativeLatencyMilliseconds"]
        ),
    }
    response = client.call_tool(
        "create_acceptance_evidence",
        {
            "projectId": PROJECT_ID,
            "criterionKey": CRITERION_KEY,
            "result": "passed",
            "notes": evidence_notes(report),
            "values": values,
            "completesCriterion": True,
            "idempotencyKey": report["evidence"]["idempotencyKey"],
        },
        timeout=60.0,
    )
    if response.get("success") is not True:
        raise LatencyError(
            "Substrate rejected AC-005 evidence: %s"
            % response.get("error", response)
        )
    updated = response.get("data")
    updated_criterion = next(
        (
            item
            for item in (updated or {}).get("criteria", [])
            if item.get("criterionKey") == CRITERION_KEY
        ),
        None,
    )
    if not updated_criterion or updated_criterion.get("status") != "met":
        raise LatencyError("AC-005 did not become met after evidence submission")
    return {
        "submitted": True,
        "idempotencyKey": report["evidence"]["idempotencyKey"],
        "values": values,
        "criterionStatus": updated_criterion.get("status"),
        "trackerCounts": (updated or {}).get("counts"),
        "recordedEvidenceCount": len(
            updated_criterion.get("recordedEvidence", [])
        ),
    }


def run_preflight() -> dict:
    command = ["make", "test", "endurance", "script-test", "inspect"]
    completed = run_command(command, timeout=240.0)
    return {
        "passed": completed.returncode == 0,
        "command": command,
        "exitCode": completed.returncode,
        "stdout": completed.stdout.strip()[-12_000:],
        "stderr": completed.stderr.strip()[-4000:],
    }


def run_latency(args) -> tuple[dict, Path]:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    run_directory = (
        Path(args.run_dir)
        if args.run_dir
        else Path("verification/hardware-runs")
        / ("icy-beauty-latency-" + stamp)
    ).resolve()
    report_path = run_directory / "target-midi-latency.json"
    raw_path = run_directory / "disting-nt-input-1.f32le"
    timing_path = run_directory / "capture-timing.json"
    wav_path = run_directory / "disting-nt-input-1.wav"
    app_path = run_directory / "tools" / "Icy Beauty Latency Capture.app"
    try:
        displayed_report_path = str(report_path.relative_to(Path.cwd()))
    except ValueError:
        displayed_report_path = str(report_path)

    starting_git = git_snapshot(Path.cwd())
    report = {
        "analyzer": "icy-beauty-target-midi-latency",
        "analyzerVersion": ANALYZER_VERSION,
        "startedAt": utc_now(),
        "finishedAt": None,
        "passed": False,
        "physicalTestPassed": False,
        "reportPath": displayed_report_path,
        "contract": {
            "criterionKey": CRITERION_KEY,
            "latencyLimitMilliseconds": LATENCY_LIMIT_MS,
            "processingLimitPercent": PROCESSING_LIMIT_PERCENT,
            "trials": args.trials,
            "settleSeconds": args.settle_seconds,
            "intervalSeconds": args.interval_seconds,
            "noteDurationSeconds": args.note_duration_seconds,
            "tailSeconds": args.tail_seconds,
            "note": args.note,
            "velocity": args.velocity,
            "temporaryUsbCaptureSlot": True,
            "evidenceSubmissionRequested": not args.no_submit_evidence,
        },
        "source": {"git": starting_git},
        "evidence": {
            "submitted": False,
            "idempotencyKey": str(uuid.uuid4()),
        },
    }
    write_json(report_path, report)

    nt_client = None
    original_preset = None
    original_slot = None
    mutation_attempted = False
    try:
        if not args.no_submit_evidence and not starting_git["workingTreeClean"]:
            raise LatencyError(
                "evidence submission requires a clean checkout at test start"
            )
        plugin_path = Path(args.plugin).resolve()
        if not plugin_path.is_file():
            raise LatencyError("plugin object is unavailable: %s" % plugin_path)
        script_path = Path(__file__).resolve()
        native_source = script_path.with_name(
            "portaudio_coremidi_latency.c"
        )
        capture_plist = script_path.with_name(
            "latency_capture_Info.plist"
        )
        report["source"].update(
            {
                "plugin": {
                    "path": os.path.relpath(plugin_path, Path.cwd()),
                    "sha256": sha256_file(plugin_path),
                    "sizeBytes": plugin_path.stat().st_size,
                },
                "script": {
                    "path": os.path.relpath(script_path, Path.cwd()),
                    "sha256": sha256_file(script_path),
                },
                "nativeCaptureSource": {
                    "path": os.path.relpath(native_source, Path.cwd()),
                    "sha256": sha256_file(native_source),
                },
                "captureAppInfoPlist": {
                    "path": os.path.relpath(capture_plist, Path.cwd()),
                    "sha256": sha256_file(capture_plist),
                },
            }
        )
        report["processingEvidence"] = processing_evidence(
            Path(args.processing_report).resolve()
        )
        report["preflight"] = run_preflight()
        if report["preflight"]["passed"] is not True:
            raise LatencyError("native and ARM preflight failed")
        report["nativeCaptureApp"] = build_capture_app(
            native_source, capture_plist, app_path
        )

        nt_client = McpClient(
            args.nt_mcp_url,
            name="icy-beauty-latency-nt-helper",
            timeout=20.0,
        )
        nt_client.initialize()
        report["ntHelper"] = {
            "url": args.nt_mcp_url,
            "protocolVersion": nt_client.protocol_version,
            "serverInfo": nt_client.server_info,
        }
        original_preset = nt_client.call_tool(
            "show_preset", {}, timeout=15.0
        )
        original_slot = nt_client.call_tool(
            "show_slot", {"slot_index": 0}, timeout=15.0
        )
        valid, reason = validate_initial_preset(
            original_preset, original_slot
        )
        if not valid:
            raise LatencyError("initial physical preset is invalid: %s" % reason)
        source_output = slot_parameter_map(original_slot)["Output"]
        report["originalPreset"] = {
            "preset": original_preset,
            "slot": original_slot,
            "canonicalSlot": canonical_slot(original_slot),
        }

        mutation_attempted = True
        report["captureTopology"] = add_usb_capture(
            nt_client, source_output
        )
        report["cpuDuringCaptureTopology"] = nt_client.call_tool(
            "show_cpu", {}, timeout=15.0
        )

        timing, process = run_native_capture(
            app_path,
            raw_path,
            timing_path,
            trials=args.trials,
            settle_seconds=args.settle_seconds,
            interval_seconds=args.interval_seconds,
            note_duration_seconds=args.note_duration_seconds,
            tail_seconds=args.tail_seconds,
            note=args.note,
            velocity=args.velocity,
        )
        samples = read_f32le(raw_path, timing["sampleCount"])
        report["capture"] = {
            "raw": {
                "path": str(raw_path),
                "sha256": sha256_file(raw_path),
                "sizeBytes": raw_path.stat().st_size,
                "sampleCount": len(samples),
                "format": "48000 Hz mono f32le",
            },
            "timing": {
                "path": str(timing_path),
                "sha256": sha256_file(timing_path),
                "process": process,
                "summary": {
                    key: timing.get(key)
                    for key in (
                        "device",
                        "destination",
                        "deviceMatches",
                        "destinationMatches",
                        "sampleRate",
                        "deviceInputChannels",
                        "selectedInput",
                        "sampleFormat",
                        "framesPerBufferRequested",
                        "streamInputLatencySeconds",
                        "sampleCount",
                        "blockCount",
                        "callbackStatusFlags",
                        "nullInputCount",
                        "capacityExceeded",
                        "midiPassed",
                        "rawPassed",
                    )
                },
            },
        }
        report["capture"]["wav"] = convert_capture_to_wav(
            raw_path, wav_path
        )
        analysis = analyze_capture(samples, timing)
        report["latencyAnalysis"] = analysis
        if analysis["passed"] is not True:
            raise LatencyError(
                "physical MIDI-to-audio latency did not pass every trial"
            )
    except KeyboardInterrupt:
        report["failure"] = {"message": "test interrupted"}
    except Exception as error:  # noqa: BLE001 - retain physical failure details
        report["failure"] = {"message": str(error)}
    finally:
        if (
            nt_client is not None
            and original_preset is not None
            and original_slot is not None
            and mutation_attempted
        ):
            try:
                report["presetRestoration"] = restore_nsib_only(
                    nt_client, original_preset, original_slot
                )
            except Exception as error:  # noqa: BLE001
                report["presetRestoration"] = {
                    "restoredExactly": False,
                    "error": str(error),
                }
                report["failure"] = {
                    "message": "preset restoration failed: %s" % error
                }

    report["physicalTestPassed"] = (
        report.get("latencyAnalysis", {}).get("passed") is True
        and report.get("processingEvidence", {}).get("passed") is True
        and report.get("preflight", {}).get("passed") is True
        and report.get("presetRestoration", {}).get("restoredExactly") is True
    )
    report["passed"] = report["physicalTestPassed"]
    report["finishedAt"] = utc_now()
    write_json(report_path, report)

    if report["passed"] and not args.no_submit_evidence:
        try:
            report["evidence"] = submit_evidence(
                report, Path(args.substrate_config).expanduser()
            )
        except Exception as error:  # noqa: BLE001
            report["evidence"]["error"] = str(error)
            report["passed"] = False
            report["failure"] = {
                "message": "evidence submission failed: %s" % error
            }
        report["finishedAt"] = utc_now()
        write_json(report_path, report)
    return report, report_path


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Measure physical CoreMIDI note-on to disting NT USB-audio "
            "latency and optionally submit passing AC-005 evidence."
        )
    )
    parser.add_argument("--plugin", default="plugins/icy_beauty.o")
    parser.add_argument(
        "--processing-report", default=DEFAULT_PROCESSING_REPORT
    )
    parser.add_argument("--trials", type=int, default=MINIMUM_EVIDENCE_TRIALS)
    parser.add_argument("--settle-seconds", type=float, default=9.5)
    parser.add_argument("--interval-seconds", type=float, default=9.5)
    parser.add_argument("--note-duration-seconds", type=float, default=0.1)
    parser.add_argument("--tail-seconds", type=float, default=0.5)
    parser.add_argument("--note", type=int, default=69)
    parser.add_argument("--velocity", type=int, default=127)
    parser.add_argument("--run-dir")
    parser.add_argument(
        "--nt-mcp-url", default="http://127.0.0.1:3847/mcp"
    )
    parser.add_argument(
        "--substrate-config", default="~/.codex/config.toml"
    )
    parser.add_argument("--no-submit-evidence", action="store_true")
    return parser


def main(argv=None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if not 1 <= args.trials <= 32:
        parser.error("trials must be between 1 and 32")
    if args.settle_seconds < 0.25:
        parser.error("settle time must be at least 0.25 seconds")
    if args.interval_seconds <= args.note_duration_seconds:
        parser.error("trial interval must exceed note duration")
    if not 0 <= args.note <= 127 or not 1 <= args.velocity <= 127:
        parser.error("note and velocity are outside the MIDI range")
    report, report_path = run_latency(args)
    print(
        json.dumps(
            {
                "passed": report.get("passed"),
                "physicalTestPassed": report.get("physicalTestPassed"),
                "report": str(report_path),
                "maximumConservativeLatencyMilliseconds": report.get(
                    "latencyAnalysis", {}
                ).get("maximumConservativeLatencyMilliseconds"),
                "evidence": report.get("evidence"),
                "failure": report.get("failure"),
            },
            indent=2,
            sort_keys=True,
        )
    )
    return 0 if report.get("passed") else 1


if __name__ == "__main__":
    raise SystemExit(main())
