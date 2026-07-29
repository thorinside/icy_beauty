#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <portaudio.h>

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE 48000
#define INPUT_CHANNELS 12
#define SELECTED_INPUT_CHANNEL 0
#define FRAMES_PER_BUFFER 1024
#define MAX_TRIALS 32
#define PACKET_BUFFER_BYTES 1024

typedef struct {
    uint64_t start_frame;
    unsigned long frame_count;
    double adc_time;
    unsigned long status_flags;
} BlockTimestamp;

typedef struct {
    float *samples;
    uint64_t sample_capacity;
    uint64_t sample_count;
    BlockTimestamp *blocks;
    size_t block_capacity;
    size_t block_count;
    unsigned long callback_status_flags;
    unsigned long null_input_count;
    bool capacity_exceeded;
} CaptureState;

typedef struct {
    double scheduled_time;
    double send_before_time;
    double send_after_time;
    double note_off_before_time;
    double note_off_after_time;
    uint8_t note;
    uint8_t velocity;
} TrialTiming;

static void usage(const char *program) {
    fprintf(
        stderr,
        "Usage: %s --device NAME --destination NAME --raw PATH "
        "[--timing PATH] "
        "[--trials N] [--settle-seconds N] [--interval-seconds N] "
        "[--note-duration-seconds N] [--tail-seconds N] "
        "[--note N] [--velocity N]\n",
        program);
}

static bool parse_double(const char *text, double minimum, double maximum,
                         double *value) {
    char *end = NULL;
    errno = 0;
    const double parsed = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed)
            || parsed < minimum || parsed > maximum)
        return false;
    *value = parsed;
    return true;
}

static bool parse_unsigned(const char *text, unsigned long minimum,
                           unsigned long maximum, unsigned long *value) {
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0'
            || parsed < minimum || parsed > maximum)
        return false;
    *value = parsed;
    return true;
}

static void print_json_string(FILE *output, const char *value) {
    const unsigned char *cursor = (const unsigned char *)value;
    fputc('"', output);
    while (*cursor != '\0') {
        const unsigned char character = *cursor++;
        switch (character) {
        case '"':
            fputs("\\\"", output);
            break;
        case '\\':
            fputs("\\\\", output);
            break;
        case '\b':
            fputs("\\b", output);
            break;
        case '\f':
            fputs("\\f", output);
            break;
        case '\n':
            fputs("\\n", output);
            break;
        case '\r':
            fputs("\\r", output);
            break;
        case '\t':
            fputs("\\t", output);
            break;
        default:
            if (character < 0x20U)
                fprintf(output, "\\u%04x", (unsigned int)character);
            else
                fputc((int)character, output);
            break;
        }
    }
    fputc('"', output);
}

static MIDIEndpointRef find_exact_destination(const char *expected_name,
                                               unsigned int *match_count) {
    MIDIEndpointRef matched = (MIDIEndpointRef)0;
    *match_count = 0U;
    CFStringRef expected = CFStringCreateWithCString(
        kCFAllocatorDefault, expected_name, kCFStringEncodingUTF8);
    if (expected == NULL)
        return matched;

    const ItemCount count = MIDIGetNumberOfDestinations();
    for (ItemCount index = 0; index < count; ++index) {
        const MIDIEndpointRef endpoint = MIDIGetDestination(index);
        CFStringRef display_name = NULL;
        const OSStatus status = MIDIObjectGetStringProperty(
            endpoint, kMIDIPropertyDisplayName, &display_name);
        if (status == noErr && display_name != NULL) {
            if (CFStringCompare(display_name, expected, 0) == kCFCompareEqualTo) {
                matched = endpoint;
                ++(*match_count);
            }
            CFRelease(display_name);
        }
    }
    CFRelease(expected);
    return matched;
}

static OSStatus send_message(MIDIPortRef port, MIDIEndpointRef destination,
                             const uint8_t bytes[3]) {
    union {
        MIDIPacketList packet_list;
        uint8_t storage[PACKET_BUFFER_BYTES];
    } buffer;
    MIDIPacketList *packet_list = &buffer.packet_list;
    MIDIPacket *packet = MIDIPacketListInit(packet_list);
    packet = MIDIPacketListAdd(
        packet_list, sizeof(buffer.storage), packet, 0, 3, bytes);
    if (packet == NULL)
        return (OSStatus)-1;
    return MIDISend(port, destination, packet_list);
}

static void send_cleanup(MIDIPortRef port, MIDIEndpointRef destination) {
    for (uint8_t channel = 0U; channel < 16U; ++channel) {
        const uint8_t sustain_off[3] = {
            (uint8_t)(0xb0U | channel), 64U, 0U,
        };
        const uint8_t all_notes_off[3] = {
            (uint8_t)(0xb0U | channel), 123U, 0U,
        };
        const uint8_t pitch_center[3] = {
            (uint8_t)(0xe0U | channel), 0U, 64U,
        };
        (void)send_message(port, destination, sustain_off);
        (void)send_message(port, destination, all_notes_off);
        (void)send_message(port, destination, pitch_center);
    }
}

static void sleep_seconds(double seconds) {
    const long milliseconds = (long)ceil(seconds * 1000.0);
    if (milliseconds > 0L)
        Pa_Sleep(milliseconds);
}

static int capture_callback(const void *input_buffer, void *output_buffer,
                            unsigned long frame_count,
                            const PaStreamCallbackTimeInfo *time_info,
                            PaStreamCallbackFlags status_flags,
                            void *user_data) {
    (void)output_buffer;
    CaptureState *capture = (CaptureState *)user_data;
    if (capture->block_count >= capture->block_capacity
            || capture->sample_count + frame_count > capture->sample_capacity) {
        capture->capacity_exceeded = true;
        return paAbort;
    }

    BlockTimestamp *block = &capture->blocks[capture->block_count++];
    block->start_frame = capture->sample_count;
    block->frame_count = frame_count;
    block->adc_time = time_info->inputBufferAdcTime;
    block->status_flags = (unsigned long)status_flags;
    capture->callback_status_flags |= (unsigned long)status_flags;

    float *destination = capture->samples + capture->sample_count;
    if (input_buffer == NULL) {
        memset(destination, 0, frame_count * sizeof(*destination));
        ++capture->null_input_count;
    } else {
        const float *input = (const float *)input_buffer;
        for (unsigned long frame = 0UL; frame < frame_count; ++frame)
            destination[frame] =
                input[frame * INPUT_CHANNELS + SELECTED_INPUT_CHANNEL];
    }
    capture->sample_count += frame_count;
    return paContinue;
}

static int fail_pa(const char *operation, PaError error) {
    fprintf(stderr, "%s failed: %s\n", operation, Pa_GetErrorText(error));
    return 1;
}

static void progress(const char *stage) {
    fprintf(stderr, "{\"stage\":\"%s\"}\n", stage);
    fflush(stderr);
}

int main(int argc, char **argv) {
    const char *device_name = NULL;
    const char *destination_name = NULL;
    const char *raw_path = NULL;
    const char *timing_path = NULL;
    unsigned long trials = 8UL;
    unsigned long note = 69UL;
    unsigned long velocity = 127UL;
    double settle_seconds = 9.5;
    double interval_seconds = 9.5;
    double note_duration_seconds = 0.10;
    double tail_seconds = 0.50;

    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--device") == 0 && index + 1 < argc)
            device_name = argv[++index];
        else if (strcmp(argv[index], "--destination") == 0
                && index + 1 < argc)
            destination_name = argv[++index];
        else if (strcmp(argv[index], "--raw") == 0 && index + 1 < argc)
            raw_path = argv[++index];
        else if (strcmp(argv[index], "--timing") == 0 && index + 1 < argc)
            timing_path = argv[++index];
        else if (strcmp(argv[index], "--trials") == 0 && index + 1 < argc) {
            if (!parse_unsigned(argv[++index], 1UL, MAX_TRIALS, &trials)) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[index], "--settle-seconds") == 0
                && index + 1 < argc) {
            if (!parse_double(argv[++index], 0.25, 30.0, &settle_seconds)) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[index], "--interval-seconds") == 0
                && index + 1 < argc) {
            if (!parse_double(argv[++index], 0.25, 30.0, &interval_seconds)) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[index], "--note-duration-seconds") == 0
                && index + 1 < argc) {
            if (!parse_double(
                    argv[++index], 0.01, 2.0, &note_duration_seconds)) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[index], "--tail-seconds") == 0
                && index + 1 < argc) {
            if (!parse_double(argv[++index], 0.10, 10.0, &tail_seconds)) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[index], "--note") == 0 && index + 1 < argc) {
            if (!parse_unsigned(argv[++index], 0UL, 127UL, &note)) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[index], "--velocity") == 0
                && index + 1 < argc) {
            if (!parse_unsigned(argv[++index], 1UL, 127UL, &velocity)) {
                usage(argv[0]);
                return 2;
            }
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (device_name == NULL || destination_name == NULL || raw_path == NULL
            || interval_seconds <= note_duration_seconds) {
        usage(argv[0]);
        return 2;
    }

    const double requested_duration = settle_seconds
        + (double)(trials - 1UL) * interval_seconds
        + note_duration_seconds + tail_seconds + 1.0;
    const uint64_t sample_capacity =
        (uint64_t)ceil(requested_duration * SAMPLE_RATE);
    const size_t block_capacity =
        (size_t)(sample_capacity / FRAMES_PER_BUFFER) + 4096U;
    CaptureState capture;
    memset(&capture, 0, sizeof(capture));
    capture.samples = (float *)calloc(
        (size_t)sample_capacity, sizeof(*capture.samples));
    capture.blocks = (BlockTimestamp *)calloc(
        block_capacity, sizeof(*capture.blocks));
    capture.sample_capacity = sample_capacity;
    capture.block_capacity = block_capacity;
    if (capture.samples == NULL || capture.blocks == NULL) {
        fprintf(stderr, "Could not allocate capture buffers\n");
        free(capture.samples);
        free(capture.blocks);
        return 3;
    }
    progress("buffers-allocated");

    PaError pa_error = Pa_Initialize();
    if (pa_error != paNoError) {
        free(capture.samples);
        free(capture.blocks);
        return fail_pa("Pa_Initialize", pa_error);
    }
    progress("portaudio-initialized");

    PaDeviceIndex matched_device = paNoDevice;
    int device_matches = 0;
    const PaDeviceIndex device_count = Pa_GetDeviceCount();
    if (device_count < 0) {
        Pa_Terminate();
        free(capture.samples);
        free(capture.blocks);
        return fail_pa("Pa_GetDeviceCount", device_count);
    }
    for (PaDeviceIndex index = 0; index < device_count; ++index) {
        const PaDeviceInfo *info = Pa_GetDeviceInfo(index);
        if (info != NULL && info->maxInputChannels >= INPUT_CHANNELS
                && strcmp(info->name, device_name) == 0) {
            matched_device = index;
            ++device_matches;
        }
    }
    if (device_matches != 1 || matched_device == paNoDevice) {
        fprintf(stderr,
                "Expected exactly one %s input with at least %d channels; "
                "found %d\n",
                device_name, INPUT_CHANNELS, device_matches);
        Pa_Terminate();
        free(capture.samples);
        free(capture.blocks);
        return 4;
    }
    progress("audio-device-found");

    unsigned int destination_matches = 0U;
    const MIDIEndpointRef destination = find_exact_destination(
        destination_name, &destination_matches);
    if (destination_matches != 1U || destination == (MIDIEndpointRef)0) {
        fprintf(stderr,
                "Expected exactly one CoreMIDI destination named '%s'; "
                "found %u\n",
                destination_name, destination_matches);
        Pa_Terminate();
        free(capture.samples);
        free(capture.blocks);
        return 5;
    }
    progress("midi-destination-found");

    MIDIClientRef midi_client = (MIDIClientRef)0;
    MIDIPortRef output_port = (MIDIPortRef)0;
    OSStatus midi_status = MIDIClientCreate(
        CFSTR("Icy Beauty latency measurement"), NULL, NULL, &midi_client);
    if (midi_status == noErr)
        midi_status = MIDIOutputPortCreate(
            midi_client, CFSTR("Icy Beauty latency output"), &output_port);
    if (midi_status != noErr) {
        fprintf(stderr,
                "CoreMIDI output initialization failed: %d\n",
                (int)midi_status);
        if (midi_client != (MIDIClientRef)0)
            MIDIClientDispose(midi_client);
        Pa_Terminate();
        free(capture.samples);
        free(capture.blocks);
        return 6;
    }
    progress("midi-output-opened");

    const PaDeviceInfo *device = Pa_GetDeviceInfo(matched_device);
    PaStreamParameters input;
    memset(&input, 0, sizeof(input));
    input.device = matched_device;
    input.channelCount = INPUT_CHANNELS;
    input.sampleFormat = paFloat32;
    input.suggestedLatency = device->defaultLowInputLatency;

    PaStream *stream = NULL;
    pa_error = Pa_OpenStream(
        &stream, &input, NULL, SAMPLE_RATE, FRAMES_PER_BUFFER, paClipOff,
        capture_callback, &capture);
    if (pa_error != paNoError) {
        MIDIPortDispose(output_port);
        MIDIClientDispose(midi_client);
        Pa_Terminate();
        free(capture.samples);
        free(capture.blocks);
        return fail_pa("Pa_OpenStream", pa_error);
    }
    progress("audio-stream-opened");
    pa_error = Pa_StartStream(stream);
    if (pa_error != paNoError) {
        Pa_CloseStream(stream);
        MIDIPortDispose(output_port);
        MIDIClientDispose(midi_client);
        Pa_Terminate();
        free(capture.samples);
        free(capture.blocks);
        return fail_pa("Pa_StartStream", pa_error);
    }
    progress("audio-stream-started");

    const PaStreamInfo *stream_info = Pa_GetStreamInfo(stream);
    const double input_latency =
        stream_info != NULL ? stream_info->inputLatency : 0.0;
    fprintf(
        stderr,
        "{\"ready\":true,\"device\":\"%s\",\"sampleRate\":%d,"
        "\"inputLatencySeconds\":%.9f}\n",
        device_name, SAMPLE_RATE, input_latency);
    fflush(stderr);
    TrialTiming trial_timing[MAX_TRIALS];
    memset(trial_timing, 0, sizeof(trial_timing));
    send_cleanup(output_port, destination);
    const double first_deadline =
        Pa_GetStreamTime(stream) + settle_seconds;
    sleep_seconds(settle_seconds);
    bool midi_passed = true;
    for (unsigned long index = 0UL; index < trials; ++index) {
        TrialTiming *timing = &trial_timing[index];
        timing->scheduled_time =
            first_deadline + (double)index * interval_seconds;
        timing->note = (uint8_t)note;
        timing->velocity = (uint8_t)velocity;

        const uint8_t note_on[3] = {
            0x90U, (uint8_t)note, (uint8_t)velocity,
        };
        timing->send_before_time = Pa_GetStreamTime(stream);
        midi_status = send_message(output_port, destination, note_on);
        timing->send_after_time = Pa_GetStreamTime(stream);
        if (midi_status != noErr) {
            midi_passed = false;
            break;
        }

        sleep_seconds(note_duration_seconds);
        const uint8_t note_off[3] = {0x80U, (uint8_t)note, 0U};
        timing->note_off_before_time = Pa_GetStreamTime(stream);
        midi_status = send_message(output_port, destination, note_off);
        timing->note_off_after_time = Pa_GetStreamTime(stream);
        if (midi_status != noErr) {
            midi_passed = false;
            break;
        }
        if (index + 1UL < trials)
            sleep_seconds(interval_seconds - note_duration_seconds);
    }

    if (midi_passed)
        sleep_seconds(tail_seconds);
    send_cleanup(output_port, destination);
    progress("midi-trials-complete");

    const PaError stop_error = Pa_StopStream(stream);
    progress("audio-stream-stopped");
    const PaError close_error = Pa_CloseStream(stream);
    MIDIPortDispose(output_port);
    MIDIClientDispose(midi_client);
    const PaError terminate_error = Pa_Terminate();

    bool raw_passed = false;
    FILE *raw = fopen(raw_path, "wb");
    if (raw != NULL) {
        const size_t written = fwrite(
            capture.samples, sizeof(*capture.samples),
            (size_t)capture.sample_count, raw);
        raw_passed = written == (size_t)capture.sample_count
            && fflush(raw) == 0 && fclose(raw) == 0;
    }

    const bool passed = midi_passed && raw_passed
        && stop_error == paNoError && close_error == paNoError
        && terminate_error == paNoError && !capture.capacity_exceeded
        && capture.callback_status_flags == 0UL
        && capture.null_input_count == 0UL && capture.block_count > 0U
        && capture.sample_count > 0U;

    FILE *timing_output = stdout;
    if (timing_path != NULL) {
        timing_output = fopen(timing_path, "w");
        if (timing_output == NULL) {
            fprintf(stderr, "Could not write timing JSON to %s\n", timing_path);
            free(capture.samples);
            free(capture.blocks);
            return 8;
        }
    }
    fputs("{\"passed\":", timing_output);
    fputs(passed ? "true" : "false", timing_output);
    fputs(",\"device\":", timing_output);
    print_json_string(timing_output, device_name);
    fputs(",\"destination\":", timing_output);
    print_json_string(timing_output, destination_name);
    fprintf(
        timing_output,
        ",\"deviceMatches\":%d,\"destinationMatches\":%u,"
        "\"sampleRate\":%d,\"deviceInputChannels\":%d,"
        "\"selectedInput\":%d,\"sampleFormat\":\"f32le\","
        "\"framesPerBufferRequested\":%d,"
        "\"streamInputLatencySeconds\":%.9f,"
        "\"sampleCount\":%llu,\"blockCount\":%zu,"
        "\"callbackStatusFlags\":%lu,\"nullInputCount\":%lu,"
        "\"capacityExceeded\":%s,\"midiPassed\":%s,\"rawPassed\":%s,"
        "\"trials\":[",
        device_matches, destination_matches, SAMPLE_RATE, INPUT_CHANNELS,
        SELECTED_INPUT_CHANNEL + 1, FRAMES_PER_BUFFER, input_latency,
        (unsigned long long)capture.sample_count, capture.block_count,
        capture.callback_status_flags, capture.null_input_count,
        capture.capacity_exceeded ? "true" : "false",
        midi_passed ? "true" : "false", raw_passed ? "true" : "false");
    for (unsigned long index = 0UL; index < trials; ++index) {
        const TrialTiming *timing = &trial_timing[index];
        if (index > 0UL)
            fputc(',', timing_output);
        fprintf(
            timing_output,
            "{\"index\":%lu,\"note\":%u,\"velocity\":%u,"
            "\"scheduledTime\":%.9f,\"sendBeforeTime\":%.9f,"
            "\"sendAfterTime\":%.9f,\"noteOffBeforeTime\":%.9f,"
            "\"noteOffAfterTime\":%.9f}",
            index + 1UL, (unsigned int)timing->note,
            (unsigned int)timing->velocity, timing->scheduled_time,
            timing->send_before_time, timing->send_after_time,
            timing->note_off_before_time, timing->note_off_after_time);
    }
    fputs("],\"blocks\":[", timing_output);
    for (size_t index = 0U; index < capture.block_count; ++index) {
        const BlockTimestamp *block = &capture.blocks[index];
        if (index > 0U)
            fputc(',', timing_output);
        fprintf(
            timing_output,
            "{\"startFrame\":%llu,\"frameCount\":%lu,"
            "\"adcTime\":%.9f,\"statusFlags\":%lu}",
            (unsigned long long)block->start_frame, block->frame_count,
            block->adc_time, block->status_flags);
    }
    fputs("]}\n", timing_output);
    const bool timing_passed = fflush(timing_output) == 0
        && (timing_path == NULL || fclose(timing_output) == 0);

    free(capture.samples);
    free(capture.blocks);
    if (!raw_passed)
        fprintf(stderr, "Could not write raw capture to %s\n", raw_path);
    if (!midi_passed)
        fprintf(stderr, "CoreMIDI send failed: %d\n", (int)midi_status);
    if (capture.callback_status_flags != 0UL)
        fprintf(stderr, "PortAudio callback flags: %lu\n",
                capture.callback_status_flags);
    if (!timing_passed)
        fprintf(stderr, "Could not finish timing JSON\n");
    return passed && timing_passed ? 0 : 7;
}
