#include <distingnt/api.h>

#include <math.h>
#include <new>
#include <stdint.h>

namespace {

enum {
    kMaxVoices = 8,
    kDefaultVoices = 4,
    kNumCommonParameters = 3,
    kNumSoundParameters = 5,
    kMaxParameters =
        kNumCommonParameters + 1 + kMaxVoices + kNumSoundParameters,
};

static const float kOutputPeakVolts = 5.0f;
static const float kOutputLimiterKneeFraction = 0.9f;

enum VoiceSource {
    kVoiceUnused,
    kVoiceMidi,
    kVoiceCv,
};

enum SoundParameter {
    kSoundTone,
    kSoundMotion,
    kSoundGrain,
    kSoundResonance,
    kSoundRelease,
};

struct Voice {
    uint32_t fundamentalPhase;
    uint32_t glassPhase;
    uint32_t shimmerPhase;
    uint32_t motionPhase;
    uint32_t basePhaseIncrement;
    uint32_t phaseIncrement;
    uint32_t noiseState;
    uint32_t age;
    float envelope;
    float velocity;
    float toneState;
    float resonatorLow;
    float resonatorBand;
    uint8_t note;
    uint8_t channel;
    uint8_t polyAftertouch;
    uint8_t source;
    bool gate;
    bool keyHeld;
};

struct IcyBeautyDtc {
    Voice voices[kMaxVoices];
    float pitchBendScale[16];
    uint32_t nextAge;
    uint8_t modWheel[16];
    uint8_t channelPressure[16];
    bool sustainPedal[16];
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

static const _NT_parameter kSoundParameters[kNumSoundParameters] = {
    { .name = "Tone", .min = 0, .max = 100, .def = 50,
      .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
    { .name = "Motion", .min = 0, .max = 100, .def = 50,
      .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
    { .name = "Grain", .min = 0, .max = 100, .def = 50,
      .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
    { .name = "Resonance", .min = 0, .max = 100, .def = 50,
      .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
    { .name = "Release", .min = 0, .max = 100, .def = 65,
      .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
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

        soundParameterFirst = kParamPitchFirst + voiceCount;
        for (uint8_t control = 0; control < kNumSoundParameters; ++control) {
            const _NT_parameter& source = kSoundParameters[control];
            _NT_parameter& destination =
                parameterDefs[soundParameterFirst + control];
            destination.name = source.name;
            destination.min = source.min;
            destination.max = source.max;
            destination.def = source.def;
            destination.unit = source.unit;
            destination.scaling = source.scaling;
            destination.enumStrings = source.enumStrings;
            soundPageParams[control] = soundParameterFirst + control;
        }

        setupPageParams[0] = kParamMidiChannel;
        cvPageParams[0] = kParamGate;
        for (uint8_t voice = 0; voice < voiceCount; ++voice)
            cvPageParams[voice + 1] = kParamPitchFirst + voice;
        routingPageParams[0] = kParamOutput;
        routingPageParams[1] = kParamOutputMode;

        setPage(pageDefs[0], "Setup", setupPageParams, 1);
        setPage(pageDefs[1], "Sound", soundPageParams,
                kNumSoundParameters);
        setPage(pageDefs[2], "CV/Gate", cvPageParams, voiceCount + 1);
        setPage(pageDefs[3], "Routing", routingPageParams, 2);
        pagesDef.numPages = 4;
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

    int soundParameter(SoundParameter control) const {
        return soundParameterFirst + static_cast<int>(control);
    }

    float soundValue(SoundParameter control) const {
        return v[soundParameter(control)] * 0.01f;
    }

    IcyBeautyDtc* dtc;
    uint8_t voiceCount;
    uint8_t soundParameterFirst;
    _NT_parameter parameterDefs[kMaxParameters];
    _NT_parameterPages pagesDef;
    _NT_parameterPage pageDefs[4];
    uint8_t setupPageParams[1];
    uint8_t soundPageParams[kNumSoundParameters];
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
    requirements.numParameters =
        kNumCommonParameters + 1 + voiceCount + kNumSoundParameters;
    requirements.sram = sizeof(IcyBeautyAlgorithm);
    requirements.dram = 0;
    requirements.dtc = sizeof(IcyBeautyDtc);
    requirements.itc = 0;
}

void clearVoice(Voice& voice) {
    voice.fundamentalPhase = 0;
    voice.glassPhase = 0x40000000U;
    voice.shimmerPhase = 0x80000000U;
    voice.motionPhase = 0;
    voice.basePhaseIncrement = 0;
    voice.phaseIncrement = 0;
    voice.noiseState = 1;
    voice.age = 0;
    voice.envelope = 0.0f;
    voice.velocity = 0.0f;
    voice.toneState = 0.0f;
    voice.resonatorLow = 0.0f;
    voice.resonatorBand = 0.0f;
    voice.note = 0;
    voice.channel = 0;
    voice.polyAftertouch = 0;
    voice.source = kVoiceUnused;
    voice.gate = false;
    voice.keyHeld = false;
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
    volatile uint8_t* const modWheel = dtc->modWheel;
    volatile uint8_t* const channelPressure = dtc->channelPressure;
    volatile bool* const sustainPedal = dtc->sustainPedal;
    for (int channel = 0; channel < 16; ++channel) {
        dtc->pitchBendScale[channel] = 1.0f;
        modWheel[channel] = 0;
        channelPressure[channel] = 0;
        sustainPedal[channel] = false;
    }
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

float pitchBendScale(uint16_t bend) {
    const float normalized = bend >= 8192U
                                 ? (bend - 8192U) * (1.0f / 8191.0f)
                                 : (static_cast<int32_t>(bend) - 8192) *
                                       (1.0f / 8192.0f);
    return exp2f(normalized * (2.0f / 12.0f));
}

void startVoice(IcyBeautyDtc* dtc, Voice& voice, uint8_t source,
                uint8_t channel, uint8_t note,
                uint32_t basePhaseIncrement, float velocity) {
    voice.fundamentalPhase = 0;
    voice.glassPhase = 0x40000000U;
    voice.shimmerPhase = 0x80000000U;
    voice.basePhaseIncrement = basePhaseIncrement;
    voice.phaseIncrement = source == kVoiceMidi
                               ? static_cast<uint32_t>(
                                     basePhaseIncrement *
                                     dtc->pitchBendScale[channel])
                               : basePhaseIncrement;
    voice.age = ++dtc->nextAge;
    voice.motionPhase = 0x9e3779b9U * voice.age;
    voice.noiseState = 0x6d2b79f5U ^ (voice.age * 0x85ebca6bU) ^ note;
    if (voice.noiseState == 0)
        voice.noiseState = 1;
    voice.envelope = 0.0f;
    voice.velocity = velocity;
    voice.toneState = 0.0f;
    voice.resonatorLow = 0.0f;
    voice.resonatorBand = 0.0f;
    voice.note = note;
    voice.channel = channel;
    voice.polyAftertouch = 0;
    voice.source = source;
    voice.gate = true;
    voice.keyHeld = source == kVoiceMidi;
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

Voice* oldestReleasedMidiVoice(IcyBeautyAlgorithm* algorithm) {
    Voice* oldest = NULL;
    for (uint8_t index = 0; index < algorithm->voiceCount; ++index) {
        Voice& voice = algorithm->dtc->voices[index];
        if (voice.source != kVoiceMidi || voice.keyHeld)
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

    selected = oldestReleasedMidiVoice(algorithm);
    if (selected != NULL)
        return selected;

    selected = oldestVoiceMatching(algorithm, kVoiceCv, false, false);
    if (selected != NULL)
        return selected;

    return oldestVoiceMatching(algorithm, 0xffU, true, true);
}

void setPitchBend(IcyBeautyAlgorithm* algorithm, uint8_t channel,
                  uint16_t bend) {
    const float scale = pitchBendScale(bend);
    algorithm->dtc->pitchBendScale[channel] = scale;
    for (uint8_t index = 0; index < algorithm->voiceCount; ++index) {
        Voice& voice = algorithm->dtc->voices[index];
        if (voice.source == kVoiceMidi && voice.channel == channel) {
            voice.phaseIncrement = static_cast<uint32_t>(
                voice.basePhaseIncrement * scale);
        }
    }
}

void setSustainPedal(IcyBeautyAlgorithm* algorithm, uint8_t channel,
                     bool down) {
    const bool wasDown = algorithm->dtc->sustainPedal[channel];
    algorithm->dtc->sustainPedal[channel] = down;
    if (!wasDown || down)
        return;

    for (uint8_t index = 0; index < algorithm->voiceCount; ++index) {
        Voice& voice = algorithm->dtc->voices[index];
        if (voice.source == kVoiceMidi && voice.channel == channel &&
            !voice.keyHeld) {
            voice.gate = false;
        }
    }
}

void setPolyAftertouch(IcyBeautyAlgorithm* algorithm, uint8_t channel,
                       uint8_t note, uint8_t pressure) {
    for (uint8_t index = 0; index < algorithm->voiceCount; ++index) {
        Voice& voice = algorithm->dtc->voices[index];
        if (voice.source == kVoiceMidi && voice.channel == channel &&
            voice.note == note) {
            voice.polyAftertouch = pressure;
        }
    }
}

float aftertouchAmount(const IcyBeautyDtc* dtc, const Voice& voice) {
    const uint8_t channelPressure = dtc->channelPressure[voice.channel];
    const uint8_t pressure = voice.polyAftertouch > channelPressure
                                 ? voice.polyAftertouch
                                 : channelPressure;
    return pressure * (1.0f / 127.0f);
}

float resonanceWithAftertouch(float resonance, float pressure) {
    return resonance + (1.0f - resonance) * pressure;
}

float motionWithAftertouch(float motion, float pressure) {
    return motion + (1.0f - motion) * (0.35f * pressure);
}

void midiMessage(_NT_algorithm* self, uint8_t status, uint8_t data1,
                 uint8_t data2) {
    IcyBeautyAlgorithm* algorithm = static_cast<IcyBeautyAlgorithm*>(self);
    if (!acceptsMidiChannel(algorithm, status))
        return;

    const uint8_t message = status & 0xf0U;
    const uint8_t channel = status & 0x0fU;
    if (message == 0x90U && data2 != 0) {
        Voice* voice = selectMidiVoice(algorithm);
        startVoice(algorithm->dtc, *voice, kVoiceMidi, channel, data1,
                   phaseIncrementForNote(data1),
                   data2 * (1.0f / 127.0f));
    } else if (message == 0x80U ||
               (message == 0x90U && data2 == 0)) {
        for (uint8_t index = 0; index < algorithm->voiceCount; ++index) {
            Voice& voice = algorithm->dtc->voices[index];
            if (voice.source == kVoiceMidi && voice.channel == channel &&
                voice.note == data1) {
                voice.keyHeld = false;
                if (!algorithm->dtc->sustainPedal[channel])
                    voice.gate = false;
            }
        }
    } else if (message == 0xa0U) {
        setPolyAftertouch(algorithm, channel, data1,
                          data2 & 0x7fU);
    } else if (message == 0xb0U) {
        if (data1 == 1U)
            algorithm->dtc->modWheel[channel] = data2 & 0x7fU;
        else if (data1 == 64U)
            setSustainPedal(algorithm, channel, data2 >= 64U);
    } else if (message == 0xd0U) {
        algorithm->dtc->channelPressure[channel] = data1 & 0x7fU;
    } else if (message == 0xe0U) {
        const uint16_t bend = static_cast<uint16_t>(data1 & 0x7fU) |
                              (static_cast<uint16_t>(data2 & 0x7fU) << 7);
        setPitchBend(algorithm, channel, bend);
    }
}

float triangle(uint32_t phase) {
    const float saw = static_cast<float>(phase >> 8) *
                          (1.0f / 8388608.0f) -
                      1.0f;
    return 1.0f - 2.0f * fabsf(saw);
}

float nextNoise(Voice& voice) {
    uint32_t state = voice.noiseState;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    voice.noiseState = state;
    return static_cast<float>(state >> 8) * (1.0f / 8388608.0f) - 1.0f;
}

float softLimitedOutputVolts(float normalizedSample) {
    const float magnitude = normalizedSample < 0.0f
                                ? -normalizedSample
                                : normalizedSample;
    float limitedMagnitude = magnitude;
    if (magnitude > kOutputLimiterKneeFraction) {
        const float excess = magnitude - kOutputLimiterKneeFraction;
        limitedMagnitude =
            kOutputLimiterKneeFraction +
            (1.0f - kOutputLimiterKneeFraction) * excess /
                (1.0f + excess);
    }
    const float volts = kOutputPeakVolts * limitedMagnitude;
    return normalizedSample < 0.0f ? -volts : volts;
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
        startVoice(algorithm->dtc, voice, kVoiceCv, 0, 0,
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
            voice.basePhaseIncrement =
                phaseIncrementForPitchCv(pitchInputs[index][0]);
            voice.phaseIncrement = voice.basePhaseIncrement;
        }
    }

    const float tone = algorithm->soundValue(kSoundTone);
    const float motion = algorithm->soundValue(kSoundMotion);
    const float grain = algorithm->soundValue(kSoundGrain);
    const float resonance = algorithm->soundValue(kSoundResonance);
    const float release = algorithm->soundValue(kSoundRelease);
    const float sampleRate = NT_globals.sampleRate == 0
                                 ? 48000.0f
                                 : static_cast<float>(NT_globals.sampleRate);
    const float toneCoefficient = 0.025f + 0.55f * tone * tone;
    const float fundamentalLevel = 0.84f - 0.18f * tone;
    const float glassLevel = 0.035f + 0.095f * tone;
    const float shimmerLevel = 0.28f + 0.54f * tone;
    const float grainDepth = grain * grain;
    const float releaseSeconds = 0.08f + 12.0f * release * release;
    const float releaseRate =
        13.815510558f / (sampleRate * releaseSeconds);
    float resonanceAmount[kMaxVoices];
    float resonatorCoefficient[kMaxVoices];
    float resonatorDamping[kMaxVoices];
    float motionDepth[kMaxVoices];
    uint32_t motionIncrement[kMaxVoices];
    for (uint8_t index = 0; index < algorithm->voiceCount; ++index) {
        const Voice& voice = dtc->voices[index];
        const float cyclesPerSample =
            voice.phaseIncrement * (1.0f / 4294967296.0f);
        float coefficient =
            6.0f * cyclesPerSample * (2.01f + 0.7f * tone);
        if (coefficient > 0.55f)
            coefficient = 0.55f;
        resonatorCoefficient[index] = coefficient;

        float effectiveMotion = motion;
        float effectiveResonance = resonance;
        if (voice.source == kVoiceMidi) {
            const float pressure = aftertouchAmount(dtc, voice);
            effectiveMotion = motionWithAftertouch(effectiveMotion,
                                                   pressure);
            effectiveResonance = resonanceWithAftertouch(
                effectiveResonance, pressure);
            const float wheel =
                dtc->modWheel[voice.channel] * (1.0f / 127.0f);
            effectiveMotion += (1.0f - effectiveMotion) * wheel;
        }
        resonanceAmount[index] = effectiveResonance;
        resonatorDamping[index] = 1.3f - 0.95f * effectiveResonance;
        motionDepth[index] = effectiveMotion * 0.0075f;
        motionIncrement[index] = phaseIncrementForFrequency(
            0.08f + 0.92f * effectiveMotion);
    }

    static const float kVoiceGain[kMaxVoices] = {
        0.63f, 0.445f, 0.364f, 0.315f, 0.281f, 0.257f, 0.238f, 0.223f,
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
            voice.motionPhase += motionIncrement[index];
            const float motionSignal = triangle(voice.motionPhase);
            const float noise = nextNoise(voice);
            float frequencyScale =
                1.0f + motionDepth[index] * motionSignal +
                0.0015f * grainDepth * noise;
            if (frequencyScale < 0.98f)
                frequencyScale = 0.98f;
            if (frequencyScale > 1.02f)
                frequencyScale = 1.02f;
            const uint32_t modulatedIncrement = static_cast<uint32_t>(
                voice.phaseIncrement * frequencyScale);
            voice.fundamentalPhase += modulatedIncrement;
            voice.glassPhase += modulatedIncrement * 2U +
                                modulatedIncrement / 127U;
            voice.shimmerPhase += modulatedIncrement * 2U +
                                  modulatedIncrement / 2U +
                                  modulatedIncrement / 127U;

            const float target = voice.gate ? 1.0f : 0.0f;
            const float envelopeRate = voice.gate ? 0.0015f : releaseRate;
            voice.envelope += envelopeRate * (target - voice.envelope);

            const float velocityBrightness = 0.06f * voice.velocity;
            const float raw =
                (fundamentalLevel - 0.1f * velocityBrightness) *
                    triangle(voice.fundamentalPhase) +
                (glassLevel + 0.25f * velocityBrightness) *
                    triangle(voice.glassPhase) +
                (shimmerLevel + velocityBrightness) *
                    triangle(voice.shimmerPhase) +
                0.12f * grainDepth * noise;
            const float voiceToneCoefficient =
                toneCoefficient + 0.08f * voice.velocity;
            voice.toneState +=
                voiceToneCoefficient * (raw - voice.toneState);

            voice.resonatorLow +=
                resonatorCoefficient[index] * voice.resonatorBand;
            const float resonatorHigh =
                voice.toneState - voice.resonatorLow -
                resonatorDamping[index] * voice.resonatorBand;
            voice.resonatorBand +=
                resonatorCoefficient[index] * resonatorHigh;
            const float signal =
                (1.0f - 0.2f * resonanceAmount[index]) *
                    voice.toneState +
                0.35f * resonanceAmount[index] * voice.resonatorBand;
            mixed += voice.velocity * voice.envelope * signal;

            if (!voice.gate && voice.envelope < 0.000001f)
                clearVoice(voice);
        }

        const float sample = softLimitedOutputVolts(voiceGain * mixed);
        output[frame] = replace ? sample : output[frame] + sample;
    }
}

static const _NT_factory kFactory = {
    .guid = NT_MULTICHAR('T', 'h', 'I', 'b'),
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
