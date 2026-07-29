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

const Voice* findMidiVoice(const IcyBeautyAlgorithm* algorithm,
                           uint8_t channel, uint8_t note) {
    for (uint8_t index = 0; index < algorithm->voiceCount; ++index) {
        const Voice& voice = algorithm->dtc->voices[index];
        if (voice.source == kVoiceMidi && voice.channel == channel &&
            voice.note == note) {
            return &voice;
        }
    }
    return NULL;
}

void makeEnduranceChord(uint32_t generation, uint8_t notes[kMaxVoices]) {
    static const uint8_t kIntervals[kMaxVoices] = {
        0, 3, 7, 10, 14, 17, 21, 24,
    };
    const uint8_t root = static_cast<uint8_t>(36 + generation % 48);
    for (uint8_t index = 0; index < kMaxVoices; ++index)
        notes[index] = root + kIntervals[index];
}

double renderSoundControlDifference(const _NT_factory* factory,
                                    SoundParameter control) {
    const int frames = 64;
    HostInstance low(factory, 4);
    HostInstance high(factory, 4);
    low.values[kParamOutputMode] = 1;
    high.values[kParamOutputMode] = 1;
    low.values[low.synth()->soundParameter(control)] = 0;
    high.values[high.synth()->soundParameter(control)] = 100;

    static const uint8_t kNotes[] = {57, 64, 69};
    for (std::size_t index = 0; index < sizeof(kNotes); ++index) {
        factory->midiMessage(low.algorithm, 0x90, kNotes[index], 100);
        factory->midiMessage(high.algorithm, 0x90, kNotes[index], 100);
    }

    std::vector<float> lowBusses(kNT_lastBus * frames, 0.0f);
    std::vector<float> highBusses(kNT_lastBus * frames, 0.0f);
    const int totalBlocks = control == kSoundRelease ? 320 : 192;
    double difference = 0.0;
    for (int block = 0; block < totalBlocks; ++block) {
        if (control == kSoundRelease && block == 48) {
            for (std::size_t index = 0; index < sizeof(kNotes); ++index) {
                factory->midiMessage(low.algorithm, 0x80, kNotes[index], 0);
                factory->midiMessage(high.algorithm, 0x80, kNotes[index], 0);
            }
        }
        std::fill(lowBusses.begin(), lowBusses.end(), 0.0f);
        std::fill(highBusses.begin(), highBusses.end(), 0.0f);
        factory->step(low.algorithm, lowBusses.data(), frames / 4);
        factory->step(high.algorithm, highBusses.data(), frames / 4);
        const float* lowOutput = lowBusses.data() + 12 * frames;
        const float* highOutput = highBusses.data() + 12 * frames;
        for (int frame = 0; frame < frames; ++frame) {
            if (!std::isfinite(lowOutput[frame]) ||
                !std::isfinite(highOutput[frame]))
                return -1.0;
            difference += std::fabs(lowOutput[frame] - highOutput[frame]);
        }
    }
    return difference / (totalBlocks * frames);
}

struct ControlRender {
    std::vector<float> low;
    std::vector<float> high;
};

ControlRender renderControlExtremes(const _NT_factory* factory,
                                    SoundParameter control,
                                    int totalBlocks, int noteOffBlock) {
    const int frames = 64;
    HostInstance low(factory, 1);
    HostInstance high(factory, 1);
    low.values[kParamOutputMode] = 1;
    high.values[kParamOutputMode] = 1;
    for (int index = 0; index < kNumSoundParameters; ++index) {
        const SoundParameter parameter =
            static_cast<SoundParameter>(index);
        low.values[low.synth()->soundParameter(parameter)] = 50;
        high.values[high.synth()->soundParameter(parameter)] = 50;
    }

    low.values[low.synth()->soundParameter(kSoundMotion)] = 0;
    high.values[high.synth()->soundParameter(kSoundMotion)] = 0;
    low.values[low.synth()->soundParameter(kSoundGrain)] = 0;
    high.values[high.synth()->soundParameter(kSoundGrain)] = 0;
    low.values[low.synth()->soundParameter(kSoundResonance)] = 0;
    high.values[high.synth()->soundParameter(kSoundResonance)] = 0;
    low.values[low.synth()->soundParameter(control)] = 0;
    high.values[high.synth()->soundParameter(control)] = 100;

    factory->midiMessage(low.algorithm, 0x90, 57, 100);
    factory->midiMessage(high.algorithm, 0x90, 57, 100);

    std::vector<float> lowBusses(kNT_lastBus * frames, 0.0f);
    std::vector<float> highBusses(kNT_lastBus * frames, 0.0f);
    ControlRender rendered;
    rendered.low.reserve(totalBlocks * frames);
    rendered.high.reserve(totalBlocks * frames);
    for (int block = 0; block < totalBlocks; ++block) {
        if (block == noteOffBlock) {
            factory->midiMessage(low.algorithm, 0x80, 57, 0);
            factory->midiMessage(high.algorithm, 0x80, 57, 0);
        }
        std::fill(lowBusses.begin(), lowBusses.end(), 0.0f);
        std::fill(highBusses.begin(), highBusses.end(), 0.0f);
        factory->step(low.algorithm, lowBusses.data(), frames / 4);
        factory->step(high.algorithm, highBusses.data(), frames / 4);
        const float* lowOutput = lowBusses.data() + 12 * frames;
        const float* highOutput = highBusses.data() + 12 * frames;
        rendered.low.insert(rendered.low.end(), lowOutput,
                            lowOutput + frames);
        rendered.high.insert(rendered.high.end(), highOutput,
                             highOutput + frames);
    }
    return rendered;
}

bool isFiniteAndAudible(const std::vector<float>& audio) {
    float peak = 0.0f;
    for (std::size_t index = 0; index < audio.size(); ++index) {
        if (!std::isfinite(audio[index]))
            return false;
        peak = std::max(peak, std::fabs(audio[index]));
    }
    return peak > 0.1f;
}

double normalizedDifference(const std::vector<float>& audio, int order) {
    const std::size_t start = 24000;
    double signalEnergy = 0.0;
    double differenceEnergy = 0.0;
    for (std::size_t index = start + order; index < audio.size(); ++index) {
        const double sample = audio[index];
        signalEnergy += sample * sample;
        if (order == 1) {
            const double difference = sample - audio[index - 1];
            differenceEnergy += difference * difference;
        } else {
            const double difference =
                sample - 2.0 * audio[index - 1] + audio[index - 2];
            differenceEnergy += difference * difference;
        }
    }
    return signalEnergy > 0.0
               ? std::sqrt(differenceEnergy / signalEnergy)
               : 0.0;
}

double componentMagnitude(const std::vector<float>& audio,
                          double frequency) {
    const std::size_t start = 24000;
    const std::size_t count = 48000;
    const double twoPi = 6.28318530717958647692;
    double real = 0.0;
    double imaginary = 0.0;
    for (std::size_t index = 0; index < count; ++index) {
        const double phase =
            twoPi * frequency * index / NT_globals.sampleRate;
        real += audio[start + index] * std::cos(phase);
        imaginary -= audio[start + index] * std::sin(phase);
    }
    return 2.0 * std::sqrt(real * real + imaginary * imaginary) / count;
}

double tailRms(const std::vector<float>& audio) {
    const std::size_t start = 24000;
    double energy = 0.0;
    for (std::size_t index = start; index < audio.size(); ++index)
        energy += audio[index] * audio[index];
    return std::sqrt(energy / (audio.size() - start));
}

double motionPhaseExcursion(const _NT_factory* factory, int motion) {
    const int frames = 64;
    HostInstance host(factory, 1);
    host.values[kParamOutputMode] = 1;
    host.values[host.synth()->soundParameter(kSoundMotion)] = motion;
    host.values[host.synth()->soundParameter(kSoundGrain)] = 0;
    host.values[host.synth()->soundParameter(kSoundResonance)] = 0;
    factory->midiMessage(host.algorithm, 0x90, 57, 100);

    std::vector<float> busses(kNT_lastBus * frames, 0.0f);
    uint32_t minimumDelta = 0xffffffffU;
    uint32_t maximumDelta = 0;
    for (int block = 0; block < 1500; ++block) {
        const uint32_t phaseBefore =
            host.synth()->dtc->voices[0].fundamentalPhase;
        factory->step(host.algorithm, busses.data(), frames / 4);
        const uint32_t phaseDelta =
            host.synth()->dtc->voices[0].fundamentalPhase - phaseBefore;
        minimumDelta = std::min(minimumDelta, phaseDelta);
        maximumDelta = std::max(maximumDelta, phaseDelta);
    }
    return static_cast<double>(maximumDelta - minimumDelta);
}

std::vector<float> renderMidiVelocity(const _NT_factory* factory,
                                      uint8_t velocity) {
    const int frames = 64;
    const int totalBlocks = 1500;
    HostInstance host(factory, 1);
    host.values[kParamOutputMode] = 1;
    host.values[host.synth()->soundParameter(kSoundMotion)] = 0;
    host.values[host.synth()->soundParameter(kSoundGrain)] = 0;
    host.values[host.synth()->soundParameter(kSoundResonance)] = 0;
    factory->midiMessage(host.algorithm, 0x93, 57, velocity);

    std::vector<float> busses(kNT_lastBus * frames, 0.0f);
    std::vector<float> rendered;
    rendered.reserve(totalBlocks * frames);
    for (int block = 0; block < totalBlocks; ++block) {
        factory->step(host.algorithm, busses.data(), frames / 4);
        const float* output = busses.data() + 12 * frames;
        rendered.insert(rendered.end(), output, output + frames);
    }
    return rendered;
}

double steadyRms(const std::vector<float>& audio) {
    const std::size_t start = 24000;
    double energy = 0.0;
    for (std::size_t index = start; index < audio.size(); ++index)
        energy += audio[index] * audio[index];
    return std::sqrt(energy / (audio.size() - start));
}

uint32_t pitchBendPhaseDelta(const _NT_factory* factory, uint16_t bend) {
    const int frames = 64;
    HostInstance host(factory, 1);
    host.values[kParamOutputMode] = 1;
    host.values[host.synth()->soundParameter(kSoundMotion)] = 0;
    host.values[host.synth()->soundParameter(kSoundGrain)] = 0;
    factory->midiMessage(host.algorithm, 0xe4,
                         static_cast<uint8_t>(bend & 0x7fU),
                         static_cast<uint8_t>((bend >> 7) & 0x7fU));
    factory->midiMessage(host.algorithm, 0x94, 57, 100);

    std::vector<float> busses(kNT_lastBus * frames, 0.0f);
    const uint32_t phaseBefore =
        host.synth()->dtc->voices[0].fundamentalPhase;
    factory->step(host.algorithm, busses.data(), frames / 4);
    return host.synth()->dtc->voices[0].fundamentalPhase - phaseBefore;
}

double midiWheelMotionExcursion(const _NT_factory* factory,
                                uint8_t wheel) {
    const int frames = 64;
    HostInstance host(factory, 1);
    host.values[kParamOutputMode] = 1;
    host.values[host.synth()->soundParameter(kSoundMotion)] = 0;
    host.values[host.synth()->soundParameter(kSoundGrain)] = 0;
    host.values[host.synth()->soundParameter(kSoundResonance)] = 0;
    factory->midiMessage(host.algorithm, 0xb5, 1, wheel);
    factory->midiMessage(host.algorithm, 0x95, 57, 100);

    std::vector<float> busses(kNT_lastBus * frames, 0.0f);
    uint32_t minimumDelta = 0xffffffffU;
    uint32_t maximumDelta = 0;
    for (int block = 0; block < 1500; ++block) {
        const uint32_t phaseBefore =
            host.synth()->dtc->voices[0].fundamentalPhase;
        factory->step(host.algorithm, busses.data(), frames / 4);
        const uint32_t phaseDelta =
            host.synth()->dtc->voices[0].fundamentalPhase - phaseBefore;
        minimumDelta = std::min(minimumDelta, phaseDelta);
        maximumDelta = std::max(maximumDelta, phaseDelta);
    }
    return static_cast<double>(maximumDelta - minimumDelta);
}

struct AftertouchRender {
    std::vector<float> audio;
    double motionExcursion;
};

AftertouchRender renderAftertouch(const _NT_factory* factory,
                                  bool polyphonic, uint8_t pressure) {
    const int frames = 64;
    const int totalBlocks = 1500;
    HostInstance host(factory, 1);
    host.values[kParamOutputMode] = 1;
    host.values[host.synth()->soundParameter(kSoundMotion)] = 0;
    host.values[host.synth()->soundParameter(kSoundGrain)] = 0;
    host.values[host.synth()->soundParameter(kSoundResonance)] = 0;
    factory->midiMessage(host.algorithm, 0x96, 57, 100);
    if (polyphonic)
        factory->midiMessage(host.algorithm, 0xa6, 57, pressure);
    else
        factory->midiMessage(host.algorithm, 0xd6, pressure, 0);

    std::vector<float> busses(kNT_lastBus * frames, 0.0f);
    AftertouchRender rendered;
    rendered.audio.reserve(totalBlocks * frames);
    uint32_t minimumDelta = 0xffffffffU;
    uint32_t maximumDelta = 0;
    for (int block = 0; block < totalBlocks; ++block) {
        std::fill(busses.begin(), busses.end(), 0.0f);
        const uint32_t phaseBefore =
            host.synth()->dtc->voices[0].fundamentalPhase;
        factory->step(host.algorithm, busses.data(), frames / 4);
        const uint32_t phaseDelta =
            host.synth()->dtc->voices[0].fundamentalPhase - phaseBefore;
        minimumDelta = std::min(minimumDelta, phaseDelta);
        maximumDelta = std::max(maximumDelta, phaseDelta);
        const float* output = busses.data() + 12 * frames;
        rendered.audio.insert(rendered.audio.end(), output,
                              output + frames);
    }
    rendered.motionExcursion =
        static_cast<double>(maximumDelta - minimumDelta);
    return rendered;
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

    for (int configuredVoices = 1; configuredVoices <= kMaxVoices;
         ++configuredVoices) {
        HostInstance cvSurface(factory, configuredVoices);
        if (cvSurface.requirements.numParameters !=
                static_cast<uint32_t>(kNumCommonParameters + 1 +
                                      configuredVoices +
                                      kNumSoundParameters) ||
            cvSurface.requirements.sram == 0 ||
            cvSurface.requirements.dtc == 0 ||
            cvSurface.synth()->voiceCount != configuredVoices)
            return fail("voice count does not determine the focused parameter count");

        const _NT_parameterPages* cvPages =
            cvSurface.algorithm->parameterPages;
        if (cvPages == NULL || cvPages->numPages != 4 ||
            std::strcmp(cvPages->pages[2].name, "CV/Gate") != 0 ||
            cvPages->pages[2].numParams != configuredVoices + 1 ||
            cvPages->pages[2].params[0] != kParamGate)
            return fail("CV/Gate page does not follow the configured voice count");

        int cvInputCount = 0;
        for (uint32_t parameter = 0;
             parameter < cvSurface.requirements.numParameters; ++parameter) {
            if (cvSurface.algorithm->parameters[parameter].unit ==
                kNT_unitCvInput)
                ++cvInputCount;
        }
        if (cvInputCount != configuredVoices + 1 ||
            cvSurface.algorithm->parameters[kParamGate].unit !=
                kNT_unitCvInput ||
            cvSurface.values[kParamGate] != 1)
            return fail("CV surface must contain only Gate and configured pitches");

        for (int voice = 0; voice < configuredVoices; ++voice) {
            const int parameter = kParamPitchFirst + voice;
            if (cvPages->pages[2].params[voice + 1] != parameter ||
                std::strcmp(cvSurface.algorithm->parameters[parameter].name,
                            kPitchNames[voice]) != 0 ||
                cvSurface.algorithm->parameters[parameter].unit !=
                    kNT_unitCvInput ||
                cvSurface.values[parameter] != voice + 2)
                return fail("pitch CV inputs are not presented sequentially");
        }

        for (int control = 0; control < kNumSoundParameters; ++control) {
            const int parameter = cvSurface.synth()->soundParameter(
                static_cast<SoundParameter>(control));
            if (cvPages->pages[1].params[control] != parameter ||
                cvSurface.algorithm->parameters[parameter].unit !=
                    kNT_unitPercent)
                return fail("sound controls must remain ordinary host parameters");
        }
    }
    std::puts(
        "PASS: voice counts 1..8 expose only Gate plus sequential Pitch CV "
        "inputs; sound controls remain host-mappable parameters");

    HostInstance midi(factory, 4);
    if (midi.algorithm == NULL || midi.algorithm->parameters == NULL ||
        midi.algorithm->parameterPages == NULL)
        return fail("plugin instance did not expose its host parameter surface");
    if (midi.values[kParamOutput] != 13)
        return fail("fresh-load audio is not routed to Output 1");
    if (midi.values[kParamMidiChannel] != 0)
        return fail("fresh-load MIDI selection is not Omni");
    if (std::strcmp(midi.algorithm->parameters[kParamGate].name, "Gate") != 0)
        return fail("CV Gate has the wrong host parameter name");
    const _NT_parameterPages* pages = midi.algorithm->parameterPages;
    if (pages->numPages != 4 ||
        std::strcmp(pages->pages[0].name, "Setup") != 0 ||
        pages->pages[0].numParams != 1 ||
        std::strcmp(pages->pages[1].name, "Sound") != 0 ||
        pages->pages[1].numParams != kNumSoundParameters ||
        std::strcmp(pages->pages[2].name, "CV/Gate") != 0 ||
        pages->pages[2].numParams != 5 ||
        std::strcmp(pages->pages[3].name, "Routing") != 0 ||
        pages->pages[3].numParams != 2)
        return fail("parameter pages do not expose one focused synth surface");
    for (int control = 0; control < kNumSoundParameters; ++control) {
        const int parameter = midi.synth()->soundParameter(
            static_cast<SoundParameter>(control));
        const _NT_parameter& definition = midi.algorithm->parameters[parameter];
        if (std::strcmp(definition.name, kSoundParameters[control].name) != 0 ||
            definition.min != 0 || definition.max != 100 ||
            definition.unit != kNT_unitPercent ||
            midi.values[parameter] != kSoundParameters[control].def)
            return fail("focused sound control definition is incorrect");
    }
    if (midi.values[midi.synth()->soundParameter(kSoundTone)] != 50 ||
        midi.values[midi.synth()->soundParameter(kSoundMotion)] != 50 ||
        midi.values[midi.synth()->soundParameter(kSoundGrain)] != 50 ||
        midi.values[midi.synth()->soundParameter(kSoundResonance)] != 50 ||
        midi.values[midi.synth()->soundParameter(kSoundRelease)] != 65)
        return fail("fresh-load sound controls are not centered with a medium-long release");

    {
        const int freshFrames = 64;
        HostInstance freshLoad(factory, 4);
        if (freshLoad.values[kParamOutput] != 13 ||
            freshLoad.values[kParamMidiChannel] != 0)
            return fail("fresh-load routing or MIDI defaults changed");

        static const uint8_t kFreshNotes[] = {57, 64, 69};
        for (std::size_t index = 0; index < sizeof(kFreshNotes); ++index)
            factory->midiMessage(freshLoad.algorithm, 0x99,
                                 kFreshNotes[index], 100);

        std::vector<float> freshBusses(
            kNT_lastBus * freshFrames, 0.0f);
        float freshPeak = 0.0f;
        uint32_t minimumPhaseDelta = 0xffffffffU;
        uint32_t maximumPhaseDelta = 0;
        const int soundingBlocks = NT_globals.sampleRate / freshFrames;
        for (int block = 0; block < soundingBlocks; ++block) {
            std::fill(freshBusses.begin(), freshBusses.end(), 0.0f);
            const uint32_t phaseBefore =
                freshLoad.synth()->dtc->voices[0].fundamentalPhase;
            factory->step(freshLoad.algorithm, freshBusses.data(),
                          freshFrames / 4);
            const uint32_t phaseDelta =
                freshLoad.synth()->dtc->voices[0].fundamentalPhase -
                phaseBefore;
            minimumPhaseDelta = std::min(minimumPhaseDelta, phaseDelta);
            maximumPhaseDelta = std::max(maximumPhaseDelta, phaseDelta);
            const float* output1 =
                freshBusses.data() + 12 * freshFrames;
            for (int frame = 0; frame < freshFrames; ++frame) {
                if (!std::isfinite(output1[frame]))
                    return fail("fresh-load patch produced non-finite audio");
                freshPeak = std::max(freshPeak, std::fabs(output1[frame]));
            }
        }
        if (freshPeak < 0.1f || maximumPhaseDelta == minimumPhaseDelta)
            return fail("fresh-load patch was not audible and animated");

        for (std::size_t index = 0; index < sizeof(kFreshNotes); ++index)
            factory->midiMessage(freshLoad.algorithm, 0x89,
                                 kFreshNotes[index], 0);
        const int releaseBlocks = NT_globals.sampleRate / (4 * freshFrames);
        float endingTailPeak = 0.0f;
        for (int block = 0; block < releaseBlocks; ++block) {
            std::fill(freshBusses.begin(), freshBusses.end(), 0.0f);
            factory->step(freshLoad.algorithm, freshBusses.data(),
                          freshFrames / 4);
            if (block + 8 < releaseBlocks)
                continue;
            const float* output1 =
                freshBusses.data() + 12 * freshFrames;
            for (int frame = 0; frame < freshFrames; ++frame) {
                if (!std::isfinite(output1[frame]))
                    return fail("fresh-load release produced non-finite audio");
                endingTailPeak = std::max(endingTailPeak,
                                          std::fabs(output1[frame]));
            }
        }
        if (endingTailPeak < 0.001f)
            return fail("fresh-load release did not sustain an audible tail");

        std::printf(
            "PASS: fresh load renders centered icy controls through Output 1 "
            "from Omni MIDI with animated audio and a %.3f tail peak after "
            "250 ms of release\n",
            endingTailPeak);
    }

    for (int control = 0; control < kNumSoundParameters; ++control) {
        const double difference = renderSoundControlDifference(
            factory, static_cast<SoundParameter>(control));
        if (difference < 0.0005)
            return fail("a focused sound control did not change rendered audio");
    }

    const int semanticBlocks = 1500;
    const ControlRender tone = renderControlExtremes(
        factory, kSoundTone, semanticBlocks, -1);
    const ControlRender motion = renderControlExtremes(
        factory, kSoundMotion, semanticBlocks, -1);
    const ControlRender grain = renderControlExtremes(
        factory, kSoundGrain, semanticBlocks, -1);
    const ControlRender resonance = renderControlExtremes(
        factory, kSoundResonance, semanticBlocks, -1);
    const ControlRender release = renderControlExtremes(
        factory, kSoundRelease, semanticBlocks, 100);
    const ControlRender* const semanticRenders[] = {
        &tone, &motion, &grain, &resonance, &release,
    };
    for (int control = 0; control < kNumSoundParameters; ++control) {
        if (!isFiniteAndAudible(semanticRenders[control]->low) ||
            !isFiniteAndAudible(semanticRenders[control]->high))
            return fail("a sound-control extreme was silent or non-finite");
    }

    const double toneBrightnessRatio =
        normalizedDifference(tone.high, 1) /
        normalizedDifference(tone.low, 1);
    if (toneBrightnessRatio < 1.35)
        return fail("Tone did not move from dark toward glassy brightness");

    const double lowMotionExcursion = motionPhaseExcursion(factory, 0);
    const double highMotionExcursion = motionPhaseExcursion(factory, 100);
    const double audibleMotionExcursion =
        phaseIncrementForNote(57) * 64.0 * 0.005;
    if (lowMotionExcursion > 1.0 ||
        highMotionExcursion < audibleMotionExcursion)
        return fail("Motion did not add animated pitch instability");

    const double grainRoughnessRatio =
        normalizedDifference(grain.high, 2) /
        normalizedDifference(grain.low, 2);
    if (grainRoughnessRatio < 5.0)
        return fail("Grain did not add a clearly stronger noisy texture");

    const double lowResonanceRatio =
        componentMagnitude(resonance.low, 440.0) /
        componentMagnitude(resonance.low, 220.0);
    const double highResonanceRatio =
        componentMagnitude(resonance.high, 440.0) /
        componentMagnitude(resonance.high, 220.0);
    if (highResonanceRatio < lowResonanceRatio * 1.4)
        return fail("Resonance did not strengthen the tuned upper spectrum");

    const double shortReleaseTail = tailRms(release.low);
    const double longReleaseTail = tailRms(release.high);
    if (shortReleaseTail > 0.001 || longReleaseTail < 0.05)
        return fail("Release did not range from short to haunting tails");

    std::printf(
        "PASS: control semantics: Tone %.2fx brighter, Motion %.0f phase "
        "excursion, Grain %.2fx rougher, Resonance %.2fx stronger harmonic, "
        "Release %.3f long-tail RMS\n",
        toneBrightnessRatio, highMotionExcursion, grainRoughnessRatio,
        highResonanceRatio / lowResonanceRatio, longReleaseTail);

    const std::vector<float> lowVelocity =
        renderMidiVelocity(factory, 32);
    const std::vector<float> highVelocity =
        renderMidiVelocity(factory, 112);
    if (!isFiniteAndAudible(lowVelocity) ||
        !isFiniteAndAudible(highVelocity))
        return fail("velocity response produced silent or non-finite audio");
    const double velocityLoudnessRatio =
        steadyRms(highVelocity) / steadyRms(lowVelocity);
    const double velocityBrightnessRatio =
        normalizedDifference(highVelocity, 1) /
        normalizedDifference(lowVelocity, 1);
    if (velocityLoudnessRatio < 2.5)
        return fail("higher MIDI velocity did not clearly increase loudness");
    if (velocityBrightnessRatio < 1.03 ||
        velocityBrightnessRatio >= velocityLoudnessRatio)
        return fail("velocity brightness increase was absent or not subtle");

    const double bendDown = pitchBendPhaseDelta(factory, 0);
    const double bendCenter = pitchBendPhaseDelta(factory, 8192);
    const double bendUp = pitchBendPhaseDelta(factory, 16383);
    const double expectedBendDown = std::exp2(-2.0 / 12.0);
    const double expectedBendUp = std::exp2(2.0 / 12.0);
    if (std::fabs(bendDown / bendCenter - expectedBendDown) > 0.0001 ||
        std::fabs(bendUp / bendCenter - expectedBendUp) > 0.0001)
        return fail("full-scale pitch bend did not reach fixed +/-2 semitones");

    const double wheelLowMotion = midiWheelMotionExcursion(factory, 0);
    const double wheelHighMotion = midiWheelMotionExcursion(factory, 127);
    const double wheelAudibleMotion =
        phaseIncrementForNote(57) * 64.0 * 0.005;
    if (wheelLowMotion > 1.0 || wheelHighMotion < wheelAudibleMotion)
        return fail("modulation wheel did not audibly increase Motion");

    const int frames = 64;
    std::vector<float> busses(kNT_lastBus * frames, 0.0f);
    HostInstance sustain(factory, 2);
    sustain.values[kParamOutputMode] = 1;
    factory->midiMessage(sustain.algorithm, 0x92, 60, 100);
    factory->midiMessage(sustain.algorithm, 0xb2, 64, 127);
    factory->midiMessage(sustain.algorithm, 0x82, 60, 0);
    const Voice* sustained = findMidiVoice(sustain.synth(), 2, 60);
    if (sustained == NULL || sustained->keyHeld || !sustained->gate)
        return fail("sustain pedal down did not hold a released key");
    float sustainPeak = 0.0f;
    for (int block = 0; block < 64; ++block) {
        std::fill(busses.begin(), busses.end(), 0.0f);
        factory->step(sustain.algorithm, busses.data(), frames / 4);
        const float* output = busses.data() + 12 * frames;
        for (int frame = 0; frame < frames; ++frame)
            sustainPeak = std::max(sustainPeak, std::fabs(output[frame]));
    }
    if (sustainPeak < 0.1f)
        return fail("sustain pedal did not keep released-key audio sounding");
    factory->midiMessage(sustain.algorithm, 0x92, 64, 100);
    factory->midiMessage(sustain.algorithm, 0xb2, 64, 0);
    sustained = findMidiVoice(sustain.synth(), 2, 60);
    const Voice* heldThroughPedalUp =
        findMidiVoice(sustain.synth(), 2, 64);
    if (sustained == NULL || sustained->gate ||
        heldThroughPedalUp == NULL || !heldThroughPedalUp->keyHeld ||
        !heldThroughPedalUp->gate)
        return fail("sustain pedal up did not release only unheld keys");
    factory->midiMessage(sustain.algorithm, 0x82, 64, 0);
    if (heldThroughPedalUp->gate)
        return fail("held key did not release normally after sustain pedal up");

    std::printf(
        "PASS: MIDI expression: velocity %.2fx louder/%.2fx brighter, "
        "pitch bend %.4fx..%.4fx, mod-wheel Motion %.0f excursion, sustain "
        "holds and releases keys conventionally\n",
        velocityLoudnessRatio, velocityBrightnessRatio,
        bendDown / bendCenter, bendUp / bendCenter, wheelHighMotion);

    const AftertouchRender polyPressureLow =
        renderAftertouch(factory, true, 0);
    const AftertouchRender polyPressureHigh =
        renderAftertouch(factory, true, 127);
    if (!isFiniteAndAudible(polyPressureLow.audio) ||
        !isFiniteAndAudible(polyPressureHigh.audio))
        return fail("polyphonic aftertouch produced silent or non-finite audio");
    const double lowPressureResonanceRatio =
        componentMagnitude(polyPressureLow.audio, 440.0) /
        componentMagnitude(polyPressureLow.audio, 220.0);
    const double highPressureResonanceRatio =
        componentMagnitude(polyPressureHigh.audio, 440.0) /
        componentMagnitude(polyPressureHigh.audio, 220.0);
    const double aftertouchMotionThreshold =
        phaseIncrementForNote(57) * 64.0 * 0.003;
    if (highPressureResonanceRatio < lowPressureResonanceRatio * 1.25)
        return fail("aftertouch did not audibly increase Resonance");
    if (polyPressureLow.motionExcursion > 1.0 ||
        polyPressureHigh.motionExcursion < aftertouchMotionThreshold)
        return fail("aftertouch did not audibly increase Motion");

    HostInstance polyPressure(factory, 2);
    polyPressure.values[polyPressure.synth()->soundParameter(kSoundMotion)] = 0;
    polyPressure.values[
        polyPressure.synth()->soundParameter(kSoundResonance)] = 0;
    factory->midiMessage(polyPressure.algorithm, 0x98, 57, 100);
    factory->midiMessage(polyPressure.algorithm, 0x98, 64, 100);
    factory->midiMessage(polyPressure.algorithm, 0xa8, 57, 127);
    const Voice* pressedNote = findMidiVoice(polyPressure.synth(), 8, 57);
    const Voice* unpressedNote = findMidiVoice(polyPressure.synth(), 8, 64);
    if (pressedNote == NULL || unpressedNote == NULL ||
        aftertouchAmount(polyPressure.synth()->dtc, *pressedNote) < 0.99f ||
        aftertouchAmount(polyPressure.synth()->dtc, *unpressedNote) != 0.0f)
        return fail("polyphonic aftertouch did not remain note-independent");
    const float resonanceLift =
        resonanceWithAftertouch(0.5f, 1.0f) - 0.5f;
    const float motionLift =
        motionWithAftertouch(0.5f, 1.0f) - 0.5f;
    if (resonanceLift <= motionLift)
        return fail("aftertouch Motion mapping was not weaker than Resonance");

    HostInstance channelPressure(factory, 2);
    factory->midiMessage(channelPressure.algorithm, 0x9a, 57, 100);
    factory->midiMessage(channelPressure.algorithm, 0x9a, 64, 100);
    factory->midiMessage(channelPressure.algorithm, 0xda, 127, 0);
    const Voice* firstChannelNote =
        findMidiVoice(channelPressure.synth(), 10, 57);
    const Voice* secondChannelNote =
        findMidiVoice(channelPressure.synth(), 10, 64);
    if (firstChannelNote == NULL || secondChannelNote == NULL ||
        aftertouchAmount(channelPressure.synth()->dtc,
                         *firstChannelNote) < 0.99f ||
        aftertouchAmount(channelPressure.synth()->dtc,
                         *secondChannelNote) < 0.99f)
        return fail("channel pressure did not apply across the held chord");
    const AftertouchRender channelPressureLow =
        renderAftertouch(factory, false, 0);
    const AftertouchRender channelPressureHigh =
        renderAftertouch(factory, false, 127);
    if (!isFiniteAndAudible(channelPressureLow.audio) ||
        !isFiniteAndAudible(channelPressureHigh.audio))
        return fail("channel pressure produced silent or non-finite audio");
    const double lowChannelResonanceRatio =
        componentMagnitude(channelPressureLow.audio, 440.0) /
        componentMagnitude(channelPressureLow.audio, 220.0);
    const double highChannelResonanceRatio =
        componentMagnitude(channelPressureHigh.audio, 440.0) /
        componentMagnitude(channelPressureHigh.audio, 220.0);
    if (highChannelResonanceRatio < lowChannelResonanceRatio * 1.25 ||
        channelPressureLow.motionExcursion > 1.0 ||
        channelPressureHigh.motionExcursion < aftertouchMotionThreshold)
        return fail("channel pressure did not apply the aftertouch mapping");

    std::printf(
        "PASS: aftertouch: Resonance %.2fx stronger harmonic, polyphonic "
        "pressure remains per-note, channel pressure spans the chord, and "
        "Motion rises by the smaller %.2f normalized amount\n",
        highPressureResonanceRatio / lowPressureResonanceRatio,
        motionLift);

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

    HostInstance sustainedReplacement(factory, 8);
    for (uint8_t note = 60; note < 68; ++note)
        factory->midiMessage(sustainedReplacement.algorithm,
                             0x90, note, 100);
    factory->midiMessage(sustainedReplacement.algorithm, 0xb0, 64, 127);
    factory->midiMessage(sustainedReplacement.algorithm, 0x80, 61, 0);
    factory->midiMessage(sustainedReplacement.algorithm, 0x80, 65, 0);
    factory->midiMessage(sustainedReplacement.algorithm, 0x90, 80, 100);
    if (heldMidiVoiceCount(sustainedReplacement.synth()) != 8 ||
        hasHeldMidiNote(sustainedReplacement.synth(), 61) ||
        !hasHeldMidiNote(sustainedReplacement.synth(), 65) ||
        !hasHeldMidiNote(sustainedReplacement.synth(), 80))
        return fail("MIDI replacement did not choose the oldest unheld key");

    HostInstance heldReplacement(factory, 8);
    for (uint8_t note = 60; note < 68; ++note)
        factory->midiMessage(heldReplacement.algorithm, 0x90, note, 100);
    factory->midiMessage(heldReplacement.algorithm, 0x90, 80, 100);
    if (heldMidiVoiceCount(heldReplacement.synth()) != 8 ||
        hasHeldMidiNote(heldReplacement.synth(), 60) ||
        !hasHeldMidiNote(heldReplacement.synth(), 61) ||
        !hasHeldMidiNote(heldReplacement.synth(), 80))
        return fail("MIDI replacement did not choose the oldest held voice");

    std::puts(
        "PASS: Icy Beauty renders one focused five-control MIDI/CV synth path");
    if (runEndurance)
        return runDenseMidiEndurance(factory);
    return 0;
}
