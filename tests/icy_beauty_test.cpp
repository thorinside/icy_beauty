#include <distingnt/api.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

const _NT_globals NT_globals = {
    48000,
    64,
    NULL,
    0,
    0,
    0,
};

#include "../icy_beauty.cpp"

namespace {

int fail(const char* message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

std::vector<std::max_align_t> allocateAligned(std::size_t byteCount) {
    const std::size_t count =
        (byteCount + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
    return std::vector<std::max_align_t>(count == 0 ? 1 : count);
}

struct HostInstance {
    HostInstance(const _NT_factory* instanceFactory, int32_t voices)
        : factory(instanceFactory), requirements(), algorithm(NULL) {
        specification[0] = voices;
        factory->calculateRequirements(requirements, specification);
        sram = allocateAligned(requirements.sram);
        dtc = allocateAligned(requirements.dtc);
        _NT_algorithmMemoryPtrs memory = {
            reinterpret_cast<uint8_t*>(sram.data()),
            NULL,
            reinterpret_cast<uint8_t*>(dtc.data()),
            NULL,
        };
        algorithm = factory->construct(memory, requirements, specification);
        values.resize(requirements.numParameters);
        for (uint32_t parameter = 0;
             parameter < requirements.numParameters; ++parameter) {
            values[parameter] = algorithm->parameters[parameter].def;
        }
        algorithm->v = values.data();
        algorithm->vIncludingCommon = values.data();
    }

    HostInstance(const HostInstance&) = delete;
    HostInstance& operator=(const HostInstance&) = delete;

    IcyBeautyAlgorithm* synth() {
        return static_cast<IcyBeautyAlgorithm*>(algorithm);
    }

    const _NT_factory* factory;
    int32_t specification[1];
    _NT_algorithmRequirements requirements;
    std::vector<std::max_align_t> sram;
    std::vector<std::max_align_t> dtc;
    _NT_algorithm* algorithm;
    std::vector<int16_t> values;
};

void setCvInputs(std::vector<float>& busses, int frames, int voices,
                 float gateVoltage, const float* pitches) {
    std::fill(busses.begin(), busses.end(), 0.0f);
    std::fill(busses.begin(), busses.begin() + frames, gateVoltage);
    for (int voice = 0; voice < voices; ++voice) {
        float* pitchBus = busses.data() + (voice + 1) * frames;
        std::fill(pitchBus, pitchBus + frames, pitches[voice]);
    }
}

bool hasHeldMidiNote(const IcyBeautyAlgorithm* algorithm, uint8_t note) {
    for (uint8_t index = 0; index < algorithm->voiceCount; ++index) {
        const Voice& voice = algorithm->dtc->voices[index];
        if (voice.source == kVoiceMidi && voice.gate && voice.note == note)
            return true;
    }
    return false;
}

int heldMidiVoiceCount(const IcyBeautyAlgorithm* algorithm) {
    int count = 0;
    for (uint8_t index = 0; index < algorithm->voiceCount; ++index) {
        const Voice& voice = algorithm->dtc->voices[index];
        if (voice.source == kVoiceMidi && voice.gate)
            ++count;
    }
    return count;
}

void makeEnduranceChord(uint32_t generation, uint8_t notes[kMaxVoices]) {
    static const uint8_t kIntervals[kMaxVoices] = {
        0, 3, 7, 10, 14, 17, 21, 24,
    };
    const uint8_t root = static_cast<uint8_t>(36 + generation % 48);
    for (uint8_t index = 0; index < kMaxVoices; ++index)
        notes[index] = root + kIntervals[index];
}

void sendInitialEnduranceChord(const _NT_factory* factory,
                               _NT_algorithm* algorithm,
                               uint8_t notes[kMaxVoices]) {
    makeEnduranceChord(0, notes);
    for (uint8_t index = 0; index < kMaxVoices; ++index) {
        const uint8_t velocity = static_cast<uint8_t>(48 + index * 10);
        factory->midiMessage(algorithm, 0x90, notes[index], velocity);
    }
}

void sendDenseMidiActivity(const _NT_factory* factory,
                           _NT_algorithm* algorithm, uint32_t generation,
                           uint8_t notes[kMaxVoices]) {
    factory->midiMessage(algorithm, 0xb0, 64, 127);
    for (uint8_t index = 0; index < kMaxVoices; ++index)
        factory->midiMessage(algorithm, 0x80, notes[index], 0);

    factory->midiMessage(
        algorithm, 0xb0, 1, static_cast<uint8_t>((generation * 29) & 0x7fU));
    static const uint16_t kBendValues[] = {0, 8192, 16383, 8192};
    const uint16_t bend = kBendValues[generation % 4];
    factory->midiMessage(algorithm, 0xe0,
                         static_cast<uint8_t>(bend & 0x7fU),
                         static_cast<uint8_t>((bend >> 7) & 0x7fU));
    factory->midiMessage(
        algorithm, 0xd0, static_cast<uint8_t>((generation * 37) & 0x7fU), 0);

    uint8_t nextNotes[kMaxVoices];
    makeEnduranceChord(generation, nextNotes);
    for (uint8_t index = 0; index < kMaxVoices; ++index) {
        const uint8_t velocity = static_cast<uint8_t>(32 +
            ((generation * 17 + index * 11) % 96));
        const uint8_t pressure = static_cast<uint8_t>(
            (generation * 23 + index * 13) & 0x7fU);
        factory->midiMessage(algorithm, 0x90, nextNotes[index], velocity);
        factory->midiMessage(algorithm, 0xa0, nextNotes[index], pressure);
        notes[index] = nextNotes[index];
    }
    factory->midiMessage(algorithm, 0xb0, 64, 0);
}

int runDenseMidiEndurance(const _NT_factory* factory) {
    const int frames = 64;
    const uint32_t sampleRate = NT_globals.sampleRate;
    const uint32_t durationMinutes = 30;
    const uint32_t totalBlocks =
        durationMinutes * 60U * sampleRate / frames;
    const uint32_t blocksPerSecond = sampleRate / frames;
    const uint32_t activityIntervalBlocks = blocksPerSecond / 2U;

    HostInstance endurance(factory, kMaxVoices);
    endurance.values[kParamOutputMode] = 1;
    std::vector<float> busses(kNT_lastBus * frames, 0.0f);
    uint8_t notes[kMaxVoices];
    sendInitialEnduranceChord(factory, endurance.algorithm, notes);

    double oneSecondEnergy = 0.0;
    uint32_t generation = 1;
    const std::clock_t started = std::clock();
    for (uint32_t block = 0; block < totalBlocks; ++block) {
        factory->step(endurance.algorithm, busses.data(), frames / 4);
        const float* output = busses.data() + 12 * frames;
        for (int frame = 0; frame < frames; ++frame) {
            if (!std::isfinite(output[frame]))
                return fail("dense MIDI endurance produced a non-finite sample");
            oneSecondEnergy += std::fabs(output[frame]);
        }

        if ((block + 1) % blocksPerSecond == 0) {
            if (oneSecondEnergy < 1.0)
                return fail("dense MIDI endurance detected a silent one-second window");
            oneSecondEnergy = 0.0;
        }
        if ((block + 1) % activityIntervalBlocks == 0 &&
            block + 1 < totalBlocks) {
            sendDenseMidiActivity(factory, endurance.algorithm,
                                  generation++, notes);
        }
    }

    factory->midiMessage(endurance.algorithm, 0xb0, 64, 0);
    for (uint8_t index = 0; index < kMaxVoices; ++index)
        factory->midiMessage(endurance.algorithm, 0x80, notes[index], 0);

    const uint32_t releaseBlocks = 2U * sampleRate / frames;
    for (uint32_t block = 0; block < releaseBlocks; ++block) {
        factory->step(endurance.algorithm, busses.data(), frames / 4);
        const float* output = busses.data() + 12 * frames;
        for (int frame = 0; frame < frames; ++frame) {
            if (!std::isfinite(output[frame]))
                return fail("post-endurance release produced a non-finite sample");
        }
    }
    for (uint8_t index = 0; index < kMaxVoices; ++index) {
        const Voice& voice = endurance.synth()->dtc->voices[index];
        if (voice.gate || voice.source != kVoiceUnused)
            return fail("dense MIDI endurance left a stuck voice");
    }

    const double elapsedSeconds =
        static_cast<double>(std::clock() - started) / CLOCKS_PER_SEC;
    std::printf(
        "PASS: simulated %u minutes of eight-voice dense MIDI in %.2f seconds "
        "(%u activity cycles) with finite audio, no silent one-second windows, "
        "and no stuck voices\n",
        durationMinutes, elapsedSeconds, generation);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const bool runEndurance =
        argc == 2 && std::strcmp(argv[1], "--endurance") == 0;
    if (argc > 1 && !runEndurance)
        return fail("usage: icy_beauty_test [--endurance]");
    if (pluginEntry(kNT_selector_version, 0) != kNT_apiVersionCurrent)
        return fail("plugin reports the wrong disting NT API version");
    if (pluginEntry(kNT_selector_numFactories, 0) != 1)
        return fail("plugin must export exactly one synth factory");

    const _NT_factory* factory = reinterpret_cast<const _NT_factory*>(
        pluginEntry(kNT_selector_factoryInfo, 0));
    if (factory == NULL)
        return fail("plugin factory is unavailable");
    if (std::strcmp(factory->name, "Icy Beauty") != 0)
        return fail("plugin factory has the wrong product name");
    if ((factory->tags & kNT_tagInstrument) == 0)
        return fail("plugin factory is not tagged as an instrument");
    if (factory->calculateRequirements == NULL || factory->construct == NULL ||
        factory->step == NULL || factory->midiMessage == NULL)
        return fail("plugin is missing a required synth callback");
    if (factory->numSpecifications != 1 ||
        std::strcmp(factory->specifications[0].name, "Voices") != 0 ||
        factory->specifications[0].min != 1 ||
        factory->specifications[0].max != 8 ||
        factory->specifications[0].def != 4)
        return fail("voice-count specification does not expose voices 1 through 8");

    int32_t oneVoiceSpec[] = {1};
    int32_t eightVoiceSpec[] = {8};
    _NT_algorithmRequirements oneVoiceRequirements = {};
    _NT_algorithmRequirements eightVoiceRequirements = {};
    factory->calculateRequirements(oneVoiceRequirements, oneVoiceSpec);
    factory->calculateRequirements(eightVoiceRequirements, eightVoiceSpec);
    if (oneVoiceRequirements.numParameters != 5 ||
        eightVoiceRequirements.numParameters != 12 ||
        eightVoiceRequirements.sram == 0 || eightVoiceRequirements.dtc == 0)
        return fail("voice count does not determine the CV pitch parameter count");

    HostInstance midi(factory, 4);
    if (midi.algorithm == NULL || midi.algorithm->parameters == NULL ||
        midi.algorithm->parameterPages == NULL)
        return fail("plugin instance did not expose its host parameter surface");
    if (midi.values[kParamOutput] != 13)
        return fail("fresh-load audio is not routed to Output 1");
    if (midi.values[kParamMidiChannel] != 0)
        return fail("fresh-load MIDI selection is not Omni");
    if (std::strcmp(midi.algorithm->parameters[kParamGate].name, "Gate") != 0 ||
        midi.algorithm->parameters[kParamGate].unit != kNT_unitCvInput ||
        midi.values[kParamGate] != 1)
        return fail("CV Gate is not exposed on Input 1 by default");
    for (int voice = 0; voice < 4; ++voice) {
        const int parameter = kParamPitchFirst + voice;
        if (std::strcmp(midi.algorithm->parameters[parameter].name,
                        kPitchNames[voice]) != 0 ||
            midi.algorithm->parameters[parameter].unit != kNT_unitCvInput ||
            midi.values[parameter] != voice + 2)
            return fail("pitch CV inputs are not presented sequentially");
    }
    if (midi.algorithm->parameterPages->pages[1].numParams != 5)
        return fail("CV/Gate page does not follow the configured voice count");

    const int frames = 64;
    std::vector<float> busses(kNT_lastBus * frames, 0.0f);
    factory->midiMessage(midi.algorithm, 0x99, 69, 112);

    float soundingPeak = 0.0f;
    for (int block = 0; block < 32; ++block) {
        std::fill(busses.begin(), busses.end(), 0.0f);
        factory->step(midi.algorithm, busses.data(), frames / 4);
        const float* output1 = busses.data() + 12 * frames;
        for (int frame = 0; frame < frames; ++frame) {
            if (!std::isfinite(output1[frame]))
                return fail("MIDI note-on rendering produced a non-finite sample");
            soundingPeak = std::max(soundingPeak, std::fabs(output1[frame]));
        }
    }
    if (soundingPeak < 0.1f)
        return fail("Omni MIDI note-on did not render audible synth output");

    float* output1 = busses.data() + 12 * frames;
    std::fill(output1, output1 + frames, 20.0f);
    factory->step(midi.algorithm, busses.data(), frames / 4);
    if (*std::min_element(output1, output1 + frames) < 15.0f)
        return fail("add output mode did not preserve existing bus audio");

    midi.values[kParamOutputMode] = 1;
    std::fill(output1, output1 + frames, 20.0f);
    factory->step(midi.algorithm, busses.data(), frames / 4);
    if (*std::max_element(output1, output1 + frames) > 5.0f)
        return fail("replace output mode did not replace existing bus audio");

    factory->midiMessage(midi.algorithm, 0x89, 69, 0);
    float releasePeak = 0.0f;
    for (int block = 0; block < 800; ++block) {
        std::fill(busses.begin(), busses.end(), 0.0f);
        factory->step(midi.algorithm, busses.data(), frames / 4);
        if (block == 799) {
            const float* releasedOutput = busses.data() + 12 * frames;
            for (int frame = 0; frame < frames; ++frame) {
                releasePeak = std::max(releasePeak,
                                       std::fabs(releasedOutput[frame]));
            }
        }
    }
    if (releasePeak > 0.01f)
        return fail("MIDI note-off did not release the synth voice");

    HostInstance cv(factory, 3);
    const float pitches[3] = {0.0f, 0.5f, 1.0f};
    float cvPeak = 0.0f;
    for (int block = 0; block < 32; ++block) {
        setCvInputs(busses, frames, 3, 5.0f, pitches);
        factory->step(cv.algorithm, busses.data(), frames / 4);
        const float* cvOutput = busses.data() + 12 * frames;
        for (int frame = 0; frame < frames; ++frame) {
            if (!std::isfinite(cvOutput[frame]))
                return fail("CV/gate rendering produced a non-finite sample");
            cvPeak = std::max(cvPeak, std::fabs(cvOutput[frame]));
        }
    }
    if (cvPeak < 0.1f)
        return fail("CV Gate did not render audible polyphonic synth output");
    for (int voice = 0; voice < 3; ++voice) {
        const Voice& state = cv.synth()->dtc->voices[voice];
        if (state.source != kVoiceCv || !state.gate ||
            state.phaseIncrement != phaseIncrementForPitchCv(pitches[voice]))
            return fail("a sequential pitch CV input did not control its active voice");
    }

    setCvInputs(busses, frames, 3, 0.0f, pitches);
    factory->step(cv.algorithm, busses.data(), frames / 4);
    for (int voice = 0; voice < 3; ++voice) {
        if (cv.synth()->dtc->voices[voice].gate)
            return fail("CV Gate low did not release every CV voice");
    }

    HostInstance priority(factory, 2);
    const float priorityPitches[2] = {0.0f, 0.25f};
    setCvInputs(busses, frames, 2, 5.0f, priorityPitches);
    factory->step(priority.algorithm, busses.data(), frames / 4);
    factory->midiMessage(priority.algorithm, 0x90, 69, 100);
    setCvInputs(busses, frames, 2, 0.0f, priorityPitches);
    factory->step(priority.algorithm, busses.data(), frames / 4);
    setCvInputs(busses, frames, 2, 5.0f, priorityPitches);
    factory->step(priority.algorithm, busses.data(), frames / 4);
    if (!hasHeldMidiNote(priority.synth(), 69))
        return fail("a CV Gate retrigger displaced a held MIDI note");

    HostInstance replacement(factory, 8);
    for (uint8_t note = 60; note < 68; ++note)
        factory->midiMessage(replacement.algorithm, 0x90, note, 100);
    factory->midiMessage(replacement.algorithm, 0x80, 67, 0);
    factory->midiMessage(replacement.algorithm, 0x90, 80, 100);
    if (heldMidiVoiceCount(replacement.synth()) != 8 ||
        !hasHeldMidiNote(replacement.synth(), 60) ||
        hasHeldMidiNote(replacement.synth(), 67) ||
        !hasHeldMidiNote(replacement.synth(), 80))
        return fail("MIDI replacement did not choose the released voice first");

    float eightVoicePeak = 0.0f;
    for (int block = 0; block < 32; ++block) {
        std::fill(busses.begin(), busses.end(), 0.0f);
        factory->step(replacement.algorithm, busses.data(), frames / 4);
        const float* eightVoiceOutput = busses.data() + 12 * frames;
        for (int frame = 0; frame < frames; ++frame) {
            if (!std::isfinite(eightVoiceOutput[frame]))
                return fail("eight-voice MIDI rendering produced a non-finite sample");
            eightVoicePeak = std::max(eightVoicePeak,
                                      std::fabs(eightVoiceOutput[frame]));
        }
    }
    if (eightVoicePeak < 0.1f)
        return fail("eight occupied MIDI voices did not render audible output");

    HostInstance heldReplacement(factory, 8);
    for (uint8_t note = 60; note < 68; ++note)
        factory->midiMessage(heldReplacement.algorithm, 0x90, note, 100);
    factory->midiMessage(heldReplacement.algorithm, 0x90, 80, 100);
    if (heldMidiVoiceCount(heldReplacement.synth()) != 8 ||
        hasHeldMidiNote(heldReplacement.synth(), 60) ||
        !hasHeldMidiNote(heldReplacement.synth(), 61) ||
        !hasHeldMidiNote(heldReplacement.synth(), 80))
        return fail("MIDI replacement did not choose the oldest held voice");

    std::puts("PASS: Icy Beauty renders MIDI and voice-count-driven poly CV/gate audio");
    if (runEndurance)
        return runDenseMidiEndurance(factory);
    return 0;
}
