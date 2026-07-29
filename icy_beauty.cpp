#include <distingnt/api.h>

#include <math.h>
#include <new>
#include <stdint.h>

namespace {

enum {
    kMaxVoices = 8,
    kDefaultVoices = 4,
    kNumCommonParameters = 3,
    kMaxParameters = kNumCommonParameters + 1 + kMaxVoices,
};

enum VoiceSource {
    kVoiceUnused,
    kVoiceMidi,
    kVoiceCv,
};

struct Voice {
    uint32_t fundamentalPhase;
    uint32_t glassPhase;
    uint32_t phaseIncrement;
    uint32_t age;
    float envelope;
    float velocity;
    uint8_t note;
    uint8_t source;
    bool gate;
};

struct IcyBeautyDtc {
    Voice voices[kMaxVoices];
    uint32_t nextAge;
    bool cvGateHigh;
};

enum Parameter {
    kParamOutput,
    kParamOutputMode,
    kParamMidiChannel,
    kParamGate,
    kParamPitchFirst,
};

static const char* const kMidiChannels[] = {
    "Omni", "1", "2", "3", "4", "5", "6", "7", "8",
    "9", "10", "11", "12", "13", "14", "15", "16",
};

static const char* const kPitchNames[kMaxVoices] = {
    "Pitch 1", "Pitch 2", "Pitch 3", "Pitch 4",
    "Pitch 5", "Pitch 6", "Pitch 7", "Pitch 8",
};

static const _NT_parameter kCommonParameters[] = {
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("Output", 1, 13)
    { .name = "MIDI channel", .min = 0, .max = 16, .def = 0,
      .unit = kNT_unitEnum, .scaling = 0, .enumStrings = kMidiChannels },
};

struct IcyBeautyAlgorithm : public _NT_algorithm {
    IcyBeautyAlgorithm(IcyBeautyDtc* dtcMemory, uint8_t configuredVoices)
        : dtc(dtcMemory), voiceCount(configuredVoices) {
        for (int i = 0; i < kNumCommonParameters; ++i) {
            parameterDefs[i].name = kCommonParameters[i].name;
            parameterDefs[i].min = kCommonParameters[i].min;
            parameterDefs[i].max = kCommonParameters[i].max;
            parameterDefs[i].def = kCommonParameters[i].def;
            parameterDefs[i].unit = kCommonParameters[i].unit;
            parameterDefs[i].scaling = kCommonParameters[i].scaling;
            parameterDefs[i].enumStrings = kCommonParameters[i].enumStrings;
        }

        setCvInput(parameterDefs[kParamGate], "Gate", 1);
        for (uint8_t voice = 0; voice < voiceCount; ++voice) {
            setCvInput(parameterDefs[kParamPitchFirst + voice],
                       kPitchNames[voice], voice + 2);
        }

        setupPageParams[0] = kParamMidiChannel;
        cvPageParams[0] = kParamGate;
        for (uint8_t voice = 0; voice < voiceCount; ++voice)
            cvPageParams[voice + 1] = kParamPitchFirst + voice;
        routingPageParams[0] = kParamOutput;
        routingPageParams[1] = kParamOutputMode;

        setPage(pageDefs[0], "Setup", setupPageParams, 1);
        setPage(pageDefs[1], "CV/Gate", cvPageParams, voiceCount + 1);
        setPage(pageDefs[2], "Routing", routingPageParams, 2);
        pagesDef.numPages = 3;
        pagesDef.pages = pageDefs;

        parameters = parameterDefs;
        parameterPages = &pagesDef;
    }
    ~IcyBeautyAlgorithm() {}

    static void setCvInput(_NT_parameter& parameter, const char* name,
                           int16_t defaultBus) {
        parameter.name = name;
        parameter.min = 1;
        parameter.max = kNT_lastBus;
        parameter.def = defaultBus;
        parameter.unit = kNT_unitCvInput;
        parameter.scaling = 0;
        parameter.enumStrings = NULL;
    }

    static void setPage(_NT_parameterPage& page, const char* name,
                        const uint8_t* pageParameters, uint8_t count) {
        page.name = name;
        page.numParams = count;
        page.group = 0;
        page.unused[0] = 0;
        page.unused[1] = 0;
        page.params = pageParameters;
    }

    IcyBeautyDtc* dtc;
    uint8_t voiceCount;
    _NT_parameter parameterDefs[kMaxParameters];
    _NT_parameterPages pagesDef;
    _NT_parameterPage pageDefs[3];
    uint8_t setupPageParams[1];
    uint8_t cvPageParams[1 + kMaxVoices];
    uint8_t routingPageParams[2];
};

static const _NT_specification kSpecifications[] = {
    { .name = "Voices", .min = 1, .max = kMaxVoices,
      .def = kDefaultVoices, .type = kNT_typeGeneric },
};

uint8_t voiceCountFromSpecifications(const int32_t* specifications) {
    int32_t count = specifications == NULL
                        ? static_cast<int32_t>(kDefaultVoices)
                        : specifications[0];
    if (count < 1)
        count = 1;
    if (count > kMaxVoices)
        count = kMaxVoices;
    return static_cast<uint8_t>(count);
}

void calculateRequirements(_NT_algorithmRequirements& requirements,
                           const int32_t* specifications) {
    const uint8_t voiceCount =
        voiceCountFromSpecifications(specifications);
    requirements.numParameters = kNumCommonParameters + 1 + voiceCount;
    requirements.sram = sizeof(IcyBeautyAlgorithm);
    requirements.dram = 0;
    requirements.dtc = sizeof(IcyBeautyDtc);
    requirements.itc = 0;
}

void clearVoice(Voice& voice) {
    voice.fundamentalPhase = 0;
    voice.glassPhase = 0x40000000U;
    voice.phaseIncrement = 0;
    voice.age = 0;
    voice.envelope = 0.0f;
    voice.velocity = 0.0f;
    voice.note = 0;
    voice.source = kVoiceUnused;
    voice.gate = false;
}

_NT_algorithm* construct(const _NT_algorithmMemoryPtrs& memory,
                         const _NT_algorithmRequirements&,
                         const int32_t* specifications) {
    IcyBeautyDtc* dtc = reinterpret_cast<IcyBeautyDtc*>(memory.dtc);
    const uint8_t voiceCount =
        voiceCountFromSpecifications(specifications);
    IcyBeautyAlgorithm* algorithm =
        new (memory.sram) IcyBeautyAlgorithm(dtc, voiceCount);

    for (int voice = 0; voice < kMaxVoices; ++voice)
        clearVoice(dtc->voices[voice]);
    dtc->nextAge = 0;
    dtc->cvGateHigh = false;

    return algorithm;
}

bool acceptsMidiChannel(const IcyBeautyAlgorithm* algorithm, uint8_t status) {
    const int selectedChannel = algorithm->v[kParamMidiChannel];
    return selectedChannel == 0 ||
           (status & 0x0fU) == static_cast<uint8_t>(selectedChannel - 1);
}

uint32_t phaseIncrementForFrequency(float frequency) {
    const uint32_t sampleRate = NT_globals.sampleRate == 0
                                    ? 48000U
                                    : NT_globals.sampleRate;
    const float maximumFrequency = sampleRate * 0.49f;
    if (frequency > maximumFrequency)
        frequency = maximumFrequency;
    if (frequency < 0.0f)
        frequency = 0.0f;
    return static_cast<uint32_t>(
        frequency * (4294967296.0 / static_cast<double>(sampleRate)));
}

uint32_t phaseIncrementForNote(uint8_t note) {
    const float semitones = (static_cast<int>(note) - 69) / 12.0f;
    return phaseIncrementForFrequency(440.0f * exp2f(semitones));
}

uint32_t phaseIncrementForPitchCv(float volts) {
    if (volts < -8.0f)
        volts = -8.0f;
    if (volts > 8.0f)
        volts = 8.0f;
    const float zeroVoltFrequency =
        440.0f * exp2f((48 - 69) / 12.0f);
    return phaseIncrementForFrequency(zeroVoltFrequency * exp2f(volts));
}

void startVoice(IcyBeautyDtc* dtc, Voice& voice, uint8_t source,
                uint8_t note, uint32_t phaseIncrement, float velocity) {
    voice.fundamentalPhase = 0;
    voice.glassPhase = 0x40000000U;
    voice.phaseIncrement = phaseIncrement;
    voice.age = ++dtc->nextAge;
    voice.envelope = 0.0f;
    voice.velocity = velocity;
    voice.note = note;
    voice.source = source;
    voice.gate = true;
}

Voice* oldestVoiceMatching(IcyBeautyAlgorithm* algorithm, uint8_t source,
                           bool requireGate, bool gateValue) {
    Voice* oldest = NULL;
    for (uint8_t index = 0; index < algorithm->voiceCount; ++index) {
        Voice& voice = algorithm->dtc->voices[index];
        if (source != 0xffU && voice.source != source)
            continue;
        if (requireGate && voice.gate != gateValue)
            continue;
        if (oldest == NULL || voice.age < oldest->age)
            oldest = &voice;
    }
    return oldest;
}

Voice* selectMidiVoice(IcyBeautyAlgorithm* algorithm) {
    Voice* selected = oldestVoiceMatching(algorithm, 0xffU, true, false);
    if (selected != NULL)
        return selected;

    selected = oldestVoiceMatching(algorithm, kVoiceCv, false, false);
    if (selected != NULL)
        return selected;

    return oldestVoiceMatching(algorithm, 0xffU, true, true);
}

void midiMessage(_NT_algorithm* self, uint8_t status, uint8_t data1,
                 uint8_t data2) {
    IcyBeautyAlgorithm* algorithm = static_cast<IcyBeautyAlgorithm*>(self);
    if (!acceptsMidiChannel(algorithm, status))
        return;

    const uint8_t message = status & 0xf0U;
    if (message == 0x90U && data2 != 0) {
        Voice* voice = selectMidiVoice(algorithm);
        startVoice(algorithm->dtc, *voice, kVoiceMidi, data1,
                   phaseIncrementForNote(data1),
                   data2 * (1.0f / 127.0f));
    } else if (message == 0x80U ||
               (message == 0x90U && data2 == 0)) {
        for (uint8_t index = 0; index < algorithm->voiceCount; ++index) {
            Voice& voice = algorithm->dtc->voices[index];
            if (voice.source == kVoiceMidi && voice.note == data1)
                voice.gate = false;
        }
    }
}

float triangle(uint32_t phase) {
    const float saw = static_cast<float>(phase >> 8) *
                          (1.0f / 8388608.0f) -
                      1.0f;
    return 1.0f - 2.0f * fabsf(saw);
}

void releaseCvVoices(IcyBeautyAlgorithm* algorithm) {
    for (uint8_t index = 0; index < algorithm->voiceCount; ++index) {
        Voice& voice = algorithm->dtc->voices[index];
        if (voice.source == kVoiceCv)
            voice.gate = false;
    }
}

void triggerCvVoices(IcyBeautyAlgorithm* algorithm,
                     const float* const pitchInputs[kMaxVoices], int frame) {
    for (uint8_t index = 0; index < algorithm->voiceCount; ++index) {
        Voice& voice = algorithm->dtc->voices[index];
        if (voice.source == kVoiceMidi && voice.gate)
            continue;
        startVoice(algorithm->dtc, voice, kVoiceCv, 0,
                   phaseIncrementForPitchCv(pitchInputs[index][frame]),
                   0.8f);
    }
}

void step(_NT_algorithm* self, float* busFrames, int numFramesBy4) {
    IcyBeautyAlgorithm* algorithm = static_cast<IcyBeautyAlgorithm*>(self);
    IcyBeautyDtc* dtc = algorithm->dtc;
    const int numFrames = numFramesBy4 * 4;
    float* output = busFrames +
                    (algorithm->v[kParamOutput] - 1) * numFrames;
    const bool replace = algorithm->v[kParamOutputMode] != 0;
    const float* gateInput = busFrames +
                             (algorithm->v[kParamGate] - 1) * numFrames;
    const float* pitchInputs[kMaxVoices];
    for (uint8_t index = 0; index < algorithm->voiceCount; ++index) {
        pitchInputs[index] =
            busFrames +
            (algorithm->v[kParamPitchFirst + index] - 1) * numFrames;
        Voice& voice = dtc->voices[index];
        if (voice.source == kVoiceCv && voice.gate) {
            voice.phaseIncrement =
                phaseIncrementForPitchCv(pitchInputs[index][0]);
        }
    }

    static const float kVoiceGain[kMaxVoices] = {
        3.5f, 2.47f, 2.02f, 1.75f, 1.56f, 1.43f, 1.32f, 1.24f,
    };
    const float voiceGain = kVoiceGain[algorithm->voiceCount - 1];

    for (int frame = 0; frame < numFrames; ++frame) {
        const bool cvGateHigh = gateInput[frame] > 1.0f;
        if (cvGateHigh && !dtc->cvGateHigh)
            triggerCvVoices(algorithm, pitchInputs, frame);
        else if (!cvGateHigh && dtc->cvGateHigh)
            releaseCvVoices(algorithm);
        dtc->cvGateHigh = cvGateHigh;

        float mixed = 0.0f;
        for (uint8_t index = 0; index < algorithm->voiceCount; ++index) {
            Voice& voice = dtc->voices[index];
            voice.fundamentalPhase += voice.phaseIncrement;
            voice.glassPhase += voice.phaseIncrement * 2U +
                                voice.phaseIncrement / 127U;

            const float target = voice.gate ? 1.0f : 0.0f;
            const float envelopeRate = voice.gate ? 0.0045f : 0.00018f;
            voice.envelope += envelopeRate * (target - voice.envelope);

            const float signal =
                0.72f * triangle(voice.fundamentalPhase) +
                0.28f * triangle(voice.glassPhase);
            mixed += voice.velocity * voice.envelope * signal;

            if (!voice.gate && voice.envelope < 0.000001f)
                clearVoice(voice);
        }

        const float sample = voiceGain * mixed;
        output[frame] = replace ? sample : output[frame] + sample;
    }
}

static const _NT_factory kFactory = {
    .guid = NT_MULTICHAR('N', 's', 'I', 'b'),
    .name = "Icy Beauty",
    .description = "A haunting, delicate software synthesizer",
    .numSpecifications = ARRAY_SIZE(kSpecifications),
    .specifications = kSpecifications,
    .calculateStaticRequirements = NULL,
    .initialise = NULL,
    .calculateRequirements = calculateRequirements,
    .construct = construct,
    .parameterChanged = NULL,
    .step = step,
    .draw = NULL,
    .midiRealtime = NULL,
    .midiMessage = midiMessage,
    .tags = kNT_tagInstrument,
    .hasCustomUi = NULL,
    .customUi = NULL,
    .setupUi = NULL,
    .serialise = NULL,
    .deserialise = NULL,
    .midiSysEx = NULL,
    .parameterUiPrefix = NULL,
    .parameterString = NULL,
};

}  // namespace

uintptr_t pluginEntry(_NT_selector selector, uint32_t data) {
    switch (selector) {
    case kNT_selector_version:
        return kNT_apiVersionCurrent;
    case kNT_selector_numFactories:
        return 1;
    case kNT_selector_factoryInfo:
        return reinterpret_cast<uintptr_t>(data == 0 ? &kFactory : NULL);
    }
    return 0;
}
