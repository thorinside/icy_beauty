#include <distingnt/api.h>

#include <math.h>
#include <new>
#include <stdint.h>

namespace {

enum {
    kMaxVoices = 16,
    kDefaultVoices = 8,
    kMaxGateGroups = 6,
    kDefaultGateGroups = 4,
    kMaxCvPerGate = 11,
    kNumCommonParameters = 3,
    kParametersPerGroup = 3,
    kNumSoundParameters = 5,
    kMaxParameters = kNumCommonParameters +
                     kMaxGateGroups * kParametersPerGroup +
                     kNumSoundParameters +
                     kMaxGateGroups * kMaxCvPerGate,
};

static const float kOutputPeakVolts = 5.0f;
static const float kOutputLimiterKneeFraction = 0.9f;
static const float kSafeContribution = 0.0001f;
static const float kFastReleaseSeconds = 0.005f;

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

enum Parameter {
    kParamOutput,
    kParamOutputMode,
    kParamMidiChannel,
    kParamGroupFirst,
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
    uint32_t pendingBasePhaseIncrement;
    float envelope;
    float velocity;
    float toneState;
    float resonatorLow;
    float resonatorBand;
    float pendingVelocity;
    uint8_t note;
    uint8_t channel;
    uint8_t polyAftertouch;
    uint8_t source;
    uint8_t cvGroup;
    uint8_t cvOrdinal;
    uint8_t pendingNote;
    uint8_t pendingChannel;
    uint8_t pendingSource;
    uint8_t pendingCvGroup;
    uint8_t pendingCvOrdinal;
    bool gate;
    bool keyHeld;
    bool fastRelease;
    bool pendingStart;
    bool pendingKeyHeld;
};

struct GateGroupState {
    bool high;
    uint8_t appliedCount;
    int16_t gateBus;
};

struct IcyBeautyDtc {
    Voice voices[kMaxVoices];
    GateGroupState groups[kMaxGateGroups];
    float pitchBendScale[16];
    uint32_t nextAge;
    uint8_t modWheel[16];
    uint8_t channelPressure[16];
    bool sustainPedal[16];
};

static const char* const kMidiChannels[] = {
    "Omni", "1", "2", "3", "4", "5", "6", "7", "8",
    "9", "10", "11", "12", "13", "14", "15", "16",
};

static const char* const kOffOn[] = {
    "Off", "On",
};

static const char* const kGateInputNames[kMaxGateGroups] = {
    "Gate input 1", "Gate input 2", "Gate input 3",
    "Gate input 4", "Gate input 5", "Gate input 6",
};

static const char* const kGateCountNames[kMaxGateGroups] = {
    "Gate 1 CV count", "Gate 2 CV count", "Gate 3 CV count",
    "Gate 4 CV count", "Gate 5 CV count", "Gate 6 CV count",
};

static const char* const kSampleHoldNames[kMaxGateGroups] = {
    "Gate 1 sample & hold", "Gate 2 sample & hold",
    "Gate 3 sample & hold", "Gate 4 sample & hold",
    "Gate 5 sample & hold", "Gate 6 sample & hold",
};

#define STRINGIFY_DETAIL(value) #value
#define STRINGIFY(value) STRINGIFY_DETAIL(value)
#define GATE_PITCH_ROUTING_NAMES(group)                                  \
    {                                                                   \
        "Gate " STRINGIFY(group) " pitch +1",                          \
        "Gate " STRINGIFY(group) " pitch +2",                          \
        "Gate " STRINGIFY(group) " pitch +3",                          \
        "Gate " STRINGIFY(group) " pitch +4",                          \
        "Gate " STRINGIFY(group) " pitch +5",                          \
        "Gate " STRINGIFY(group) " pitch +6",                          \
        "Gate " STRINGIFY(group) " pitch +7",                          \
        "Gate " STRINGIFY(group) " pitch +8",                          \
        "Gate " STRINGIFY(group) " pitch +9",                          \
        "Gate " STRINGIFY(group) " pitch +10",                         \
        "Gate " STRINGIFY(group) " pitch +11",                         \
    }

static const char* const
    kPitchRoutingNames[kMaxGateGroups][kMaxCvPerGate] = {
        GATE_PITCH_ROUTING_NAMES(1), GATE_PITCH_ROUTING_NAMES(2),
        GATE_PITCH_ROUTING_NAMES(3), GATE_PITCH_ROUTING_NAMES(4),
        GATE_PITCH_ROUTING_NAMES(5), GATE_PITCH_ROUTING_NAMES(6),
};

#undef GATE_PITCH_ROUTING_NAMES
#undef STRINGIFY
#undef STRINGIFY_DETAIL

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

int groupGateParameter(uint8_t group) {
    return kParamGroupFirst + group * kParametersPerGroup;
}

int groupCountParameter(uint8_t group) {
    return groupGateParameter(group) + 1;
}

int groupSampleHoldParameter(uint8_t group) {
    return groupGateParameter(group) + 2;
}

struct IcyBeautyAlgorithm : public _NT_algorithm {
    IcyBeautyAlgorithm(IcyBeautyDtc* dtcMemory, uint8_t configuredVoices,
                       uint8_t configuredGroups)
        : dtc(dtcMemory), voiceCount(configuredVoices),
          gateGroupCount(configuredGroups),
          pitchRoutingCount(configuredVoices < kMaxCvPerGate
                                ? configuredVoices
                                : static_cast<uint8_t>(kMaxCvPerGate)),
          updatingParameter(false) {
        for (int i = 0; i < kNumCommonParameters; ++i)
            copyParameter(parameterDefs[i], kCommonParameters[i]);

        uint8_t cvPageCount = 0;
        for (uint8_t group = 0; group < gateGroupCount; ++group) {
            setCvInput(parameterDefs[groupGateParameter(group)],
                       kGateInputNames[group]);
            setCount(parameterDefs[groupCountParameter(group)],
                     kGateCountNames[group],
                     voiceCount < kMaxCvPerGate
                         ? voiceCount
                         : static_cast<uint8_t>(kMaxCvPerGate));
            setSampleHold(parameterDefs[groupSampleHoldParameter(group)],
                          kSampleHoldNames[group]);
            cvPageParams[cvPageCount++] = groupGateParameter(group);
            cvPageParams[cvPageCount++] = groupCountParameter(group);
            cvPageParams[cvPageCount++] =
                groupSampleHoldParameter(group);
        }

        soundParameterFirst =
            kNumCommonParameters + gateGroupCount * kParametersPerGroup;
        for (uint8_t control = 0; control < kNumSoundParameters; ++control) {
            copyParameter(parameterDefs[soundParameterFirst + control],
                          kSoundParameters[control]);
            soundPageParams[control] = soundParameterFirst + control;
        }

        pitchRoutingParameterFirst =
            soundParameterFirst + kNumSoundParameters;
        for (uint8_t group = 0; group < gateGroupCount; ++group) {
            for (uint8_t ordinal = 0; ordinal < pitchRoutingCount;
                 ++ordinal) {
                setCvInput(
                    parameterDefs[pitchRoutingParameter(group, ordinal)],
                    kPitchRoutingNames[group][ordinal]);
            }
        }

        setupPageParams[0] = kParamMidiChannel;
        routingPageParams[0] = kParamOutput;
        routingPageParams[1] = kParamOutputMode;

        setPage(pageDefs[0], "Setup", setupPageParams, 1);
        setPage(pageDefs[1], "Sound", soundPageParams,
                kNumSoundParameters);
        setPage(pageDefs[2], "CV/Gate", cvPageParams, cvPageCount);
        setPage(pageDefs[3], "Routing", routingPageParams, 2);
        pagesDef.numPages = 4;
        pagesDef.pages = pageDefs;

        parameters = parameterDefs;
        parameterPages = &pagesDef;
    }
    ~IcyBeautyAlgorithm() {}

    static void copyParameter(_NT_parameter& destination,
                              const _NT_parameter& source) {
        destination.name = source.name;
        destination.min = source.min;
        destination.max = source.max;
        destination.def = source.def;
        destination.unit = source.unit;
        destination.scaling = source.scaling;
        destination.enumStrings = source.enumStrings;
    }

    static void setCvInput(_NT_parameter& parameter, const char* name) {
        parameter.name = name;
        parameter.min = 0;
        parameter.max = kNT_lastBus;
        parameter.def = 0;
        parameter.unit = kNT_unitCvInput;
        parameter.scaling = 0;
        parameter.enumStrings = NULL;
    }

    static void setCount(_NT_parameter& parameter, const char* name,
                         int16_t maximum) {
        parameter.name = name;
        parameter.min = 0;
        parameter.max = maximum;
        parameter.def = 0;
        parameter.unit = kNT_unitNone;
        parameter.scaling = 0;
        parameter.enumStrings = NULL;
    }

    static void setSampleHold(_NT_parameter& parameter, const char* name) {
        parameter.name = name;
        parameter.min = 0;
        parameter.max = 1;
        parameter.def = 0;
        parameter.unit = kNT_unitEnum;
        parameter.scaling = 0;
        parameter.enumStrings = kOffOn;
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

    int pitchRoutingParameter(uint8_t group, uint8_t ordinal) const {
        return pitchRoutingParameterFirst +
               group * pitchRoutingCount + ordinal;
    }

    float soundValue(SoundParameter control) const {
        return v[soundParameter(control)] * 0.01f;
    }

    uint8_t groupCount(uint8_t group) const {
        int value = v[groupCountParameter(group)];
        if (value < 0)
            value = 0;
        if (value > kMaxCvPerGate)
            value = kMaxCvPerGate;
        return static_cast<uint8_t>(value);
    }

    uint8_t groupStart(uint8_t group) const {
        uint8_t start = 0;
        for (uint8_t previous = 0; previous < group; ++previous)
            start += groupCount(previous);
        return start;
    }

    uint8_t midiStart() const {
        uint8_t start = 0;
        for (uint8_t group = 0; group < gateGroupCount; ++group)
            start += groupCount(group);
        return start > voiceCount ? voiceCount : start;
    }

    int countMaximum(uint8_t group) const {
        int usedByOtherGroups = 0;
        for (uint8_t other = 0; other < gateGroupCount; ++other) {
            if (other != group)
                usedByOtherGroups += groupCount(other);
        }
        int maximum = static_cast<int>(voiceCount) - usedByOtherGroups;
        if (maximum < 0)
            maximum = 0;
        if (maximum > kMaxCvPerGate)
            maximum = kMaxCvPerGate;

        const int gateBus = v[groupGateParameter(group)];
        if (gateBus > 0) {
            int busRoom = kNT_lastBus - gateBus;
            if (busRoom < 0)
                busRoom = 0;
            if (maximum > busRoom)
                maximum = busRoom;
        }
        return maximum;
    }

    IcyBeautyDtc* dtc;
    uint8_t voiceCount;
    uint8_t gateGroupCount;
    uint8_t soundParameterFirst;
    uint8_t pitchRoutingParameterFirst;
    uint8_t pitchRoutingCount;
    bool updatingParameter;
    _NT_parameter parameterDefs[kMaxParameters];
    _NT_parameterPages pagesDef;
    _NT_parameterPage pageDefs[4];
    uint8_t setupPageParams[1];
    uint8_t soundPageParams[kNumSoundParameters];
    uint8_t cvPageParams[kMaxGateGroups * kParametersPerGroup];
    uint8_t routingPageParams[2];
};

static const _NT_specification kSpecifications[] = {
    { .name = "Voices", .min = 1, .max = kMaxVoices,
      .def = kDefaultVoices, .type = kNT_typeGeneric },
    { .name = "Gate groups", .min = 0, .max = kMaxGateGroups,
      .def = kDefaultGateGroups, .type = kNT_typeGeneric },
};

uint8_t boundedSpecification(const int32_t* specifications, int index,
                             int minimum, int maximum, int defaultValue) {
    int32_t value = specifications == NULL
                        ? static_cast<int32_t>(defaultValue)
                        : specifications[index];
    if (value < minimum)
        value = minimum;
    if (value > maximum)
        value = maximum;
    return static_cast<uint8_t>(value);
}

uint8_t voiceCountFromSpecifications(const int32_t* specifications) {
    return boundedSpecification(specifications, 0, 1, kMaxVoices,
                                kDefaultVoices);
}

uint8_t gateGroupCountFromSpecifications(const int32_t* specifications) {
    return boundedSpecification(specifications, 1, 0, kMaxGateGroups,
                                kDefaultGateGroups);
}

void calculateRequirements(_NT_algorithmRequirements& requirements,
                           const int32_t* specifications) {
    const uint8_t voiceCount =
        voiceCountFromSpecifications(specifications);
    const uint8_t gateGroups =
        gateGroupCountFromSpecifications(specifications);
    const uint8_t pitchRoutingCount =
        voiceCount < kMaxCvPerGate
            ? voiceCount
            : static_cast<uint8_t>(kMaxCvPerGate);
    requirements.numParameters =
        kNumCommonParameters + gateGroups * kParametersPerGroup +
        kNumSoundParameters + gateGroups * pitchRoutingCount;
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
    voice.pendingBasePhaseIncrement = 0;
    voice.envelope = 0.0f;
    voice.velocity = 0.0f;
    voice.toneState = 0.0f;
    voice.resonatorLow = 0.0f;
    voice.resonatorBand = 0.0f;
    voice.pendingVelocity = 0.0f;
    voice.note = 0;
    voice.channel = 0;
    voice.polyAftertouch = 0;
    voice.source = kVoiceUnused;
    voice.cvGroup = 0xffU;
    voice.cvOrdinal = 0xffU;
    voice.pendingNote = 0;
    voice.pendingChannel = 0;
    voice.pendingSource = kVoiceUnused;
    voice.pendingCvGroup = 0xffU;
    voice.pendingCvOrdinal = 0xffU;
    voice.gate = false;
    voice.keyHeld = false;
    voice.fastRelease = false;
    voice.pendingStart = false;
    voice.pendingKeyHeld = false;
}

_NT_algorithm* construct(const _NT_algorithmMemoryPtrs& memory,
                         const _NT_algorithmRequirements&,
                         const int32_t* specifications) {
    IcyBeautyDtc* dtc = reinterpret_cast<IcyBeautyDtc*>(memory.dtc);
    const uint8_t voiceCount =
        voiceCountFromSpecifications(specifications);
    const uint8_t gateGroups =
        gateGroupCountFromSpecifications(specifications);
    IcyBeautyAlgorithm* algorithm =
        new (memory.sram) IcyBeautyAlgorithm(dtc, voiceCount, gateGroups);

    for (int voice = 0; voice < kMaxVoices; ++voice)
        clearVoice(dtc->voices[voice]);
    for (int group = 0; group < kMaxGateGroups; ++group) {
        dtc->groups[group].high = false;
        dtc->groups[group].appliedCount = 0;
        dtc->groups[group].gateBus = 0;
    }
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
                uint32_t basePhaseIncrement, float velocity,
                uint8_t cvGroup = 0xffU, uint8_t cvOrdinal = 0xffU,
                bool keyHeld = true, bool gate = true) {
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
    voice.cvGroup = cvGroup;
    voice.cvOrdinal = cvOrdinal;
    voice.gate = gate;
    voice.keyHeld = source == kVoiceMidi && keyHeld;
    voice.fastRelease = false;
    voice.pendingStart = false;
    voice.pendingSource = kVoiceUnused;
}

void queueVoiceStart(Voice& voice, uint8_t source, uint8_t channel,
                     uint8_t note, uint32_t basePhaseIncrement,
                     float velocity, uint8_t cvGroup, uint8_t cvOrdinal,
                     bool keyHeld) {
    voice.source = source;
    voice.channel = channel;
    voice.note = note;
    voice.cvGroup = cvGroup;
    voice.cvOrdinal = cvOrdinal;
    voice.keyHeld = source == kVoiceMidi && keyHeld;
    voice.gate = false;
    voice.fastRelease = true;
    voice.pendingStart = true;
    voice.pendingSource = source;
    voice.pendingChannel = channel;
    voice.pendingNote = note;
    voice.pendingBasePhaseIncrement = basePhaseIncrement;
    voice.pendingVelocity = velocity;
    voice.pendingCvGroup = cvGroup;
    voice.pendingCvOrdinal = cvOrdinal;
    voice.pendingKeyHeld = keyHeld;
}

void fastReleaseVoice(Voice& voice) {
    voice.gate = false;
    voice.fastRelease = true;
    voice.pendingStart = false;
    voice.pendingSource = kVoiceUnused;
}

Voice* findCvVoice(IcyBeautyAlgorithm* algorithm, uint8_t group,
                   uint8_t ordinal) {
    for (uint8_t index = 0; index < algorithm->voiceCount; ++index) {
        Voice& voice = algorithm->dtc->voices[index];
        if (voice.source == kVoiceCv && voice.cvGroup == group &&
            voice.cvOrdinal == ordinal)
            return &voice;
    }
    return NULL;
}

Voice* findMidiNote(IcyBeautyAlgorithm* algorithm, uint8_t channel,
                    uint8_t note) {
    const uint8_t first = algorithm->midiStart();
    for (uint8_t index = first; index < algorithm->voiceCount; ++index) {
        Voice& voice = algorithm->dtc->voices[index];
        if (voice.source == kVoiceMidi && voice.channel == channel &&
            voice.note == note)
            return &voice;
    }
    return NULL;
}

Voice* oldestMidiMatching(IcyBeautyAlgorithm* algorithm,
                          bool requireKeyHeld, bool keyHeld,
                          bool requireGate, bool gate) {
    Voice* oldest = NULL;
    const uint8_t first = algorithm->midiStart();
    for (uint8_t index = first; index < algorithm->voiceCount; ++index) {
        Voice& voice = algorithm->dtc->voices[index];
        if (voice.source != kVoiceMidi)
            continue;
        if (requireKeyHeld && voice.keyHeld != keyHeld)
            continue;
        if (requireGate && voice.gate != gate)
            continue;
        if (oldest == NULL || voice.age < oldest->age)
            oldest = &voice;
    }
    return oldest;
}

Voice* selectMidiVoice(IcyBeautyAlgorithm* algorithm) {
    const uint8_t first = algorithm->midiStart();
    for (uint8_t index = first; index < algorithm->voiceCount; ++index) {
        Voice& voice = algorithm->dtc->voices[index];
        if (voice.source == kVoiceUnused ||
            (!voice.gate && !voice.pendingStart &&
             voice.envelope <= kSafeContribution)) {
            clearVoice(voice);
            return &voice;
        }
    }

    Voice* selected =
        oldestMidiMatching(algorithm, true, false, true, false);
    if (selected != NULL)
        return selected;

    selected = oldestMidiMatching(algorithm, true, false, true, true);
    if (selected != NULL)
        return selected;

    return oldestMidiMatching(algorithm, true, true, false, false);
}

void beginMidiNote(IcyBeautyAlgorithm* algorithm, Voice& voice,
                   uint8_t channel, uint8_t note, uint8_t velocity) {
    const uint32_t increment = phaseIncrementForNote(note);
    const float level = velocity * (1.0f / 127.0f);
    if (voice.source == kVoiceUnused ||
        voice.envelope <= kSafeContribution) {
        startVoice(algorithm->dtc, voice, kVoiceMidi, channel, note,
                   increment, level);
    } else {
        voice.age = ++algorithm->dtc->nextAge;
        queueVoiceStart(voice, kVoiceMidi, channel, note, increment,
                        level, 0xffU, 0xffU, true);
    }
}

void setPitchBend(IcyBeautyAlgorithm* algorithm, uint8_t channel,
                  uint16_t bend) {
    const float scale = pitchBendScale(bend);
    algorithm->dtc->pitchBendScale[channel] = scale;
    for (uint8_t index = algorithm->midiStart();
         index < algorithm->voiceCount; ++index) {
        Voice& voice = algorithm->dtc->voices[index];
        if (voice.source == kVoiceMidi && voice.channel == channel &&
            !voice.pendingStart) {
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

    for (uint8_t index = algorithm->midiStart();
         index < algorithm->voiceCount; ++index) {
        Voice& voice = algorithm->dtc->voices[index];
        if (voice.source != kVoiceMidi || voice.channel != channel ||
            voice.keyHeld)
            continue;
        if (voice.pendingStart)
            voice.pendingStart = false;
        voice.gate = false;
    }
}

void resetAllVoices(IcyBeautyAlgorithm* algorithm) {
    IcyBeautyDtc* dtc = algorithm->dtc;
    for (uint8_t index = 0; index < algorithm->voiceCount; ++index)
        clearVoice(dtc->voices[index]);

    for (uint8_t group = 0; group < algorithm->gateGroupCount; ++group) {
        GateGroupState& state = dtc->groups[group];
        state.gateBus = algorithm->v[groupGateParameter(group)];
        state.appliedCount = algorithm->groupCount(group);
        // A connected gate may still be high when Stop arrives. Treat it as
        // already high so silence is retained until a fresh low-to-high edge.
        state.high = state.gateBus > 0;
    }

    volatile uint8_t* const modWheel = dtc->modWheel;
    volatile uint8_t* const channelPressure = dtc->channelPressure;
    volatile bool* const sustainPedal = dtc->sustainPedal;
    for (uint8_t channel = 0; channel < 16; ++channel) {
        dtc->pitchBendScale[channel] = 1.0f;
        modWheel[channel] = 0;
        channelPressure[channel] = 0;
        sustainPedal[channel] = false;
    }
    dtc->nextAge = 0;
}

void setPolyAftertouch(IcyBeautyAlgorithm* algorithm, uint8_t channel,
                       uint8_t note, uint8_t pressure) {
    for (uint8_t index = algorithm->midiStart();
         index < algorithm->voiceCount; ++index) {
        Voice& voice = algorithm->dtc->voices[index];
        if (voice.source == kVoiceMidi && voice.channel == channel &&
            voice.note == note)
            voice.polyAftertouch = pressure;
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
        Voice* voice = findMidiNote(algorithm, channel, data1);
        if (voice == NULL)
            voice = selectMidiVoice(algorithm);
        if (voice != NULL)
            beginMidiNote(algorithm, *voice, channel, data1, data2);
    } else if (message == 0x80U ||
               (message == 0x90U && data2 == 0)) {
        for (uint8_t index = algorithm->midiStart();
             index < algorithm->voiceCount; ++index) {
            Voice& voice = algorithm->dtc->voices[index];
            if (voice.source != kVoiceMidi ||
                voice.channel != channel || voice.note != data1)
                continue;
            voice.keyHeld = false;
            voice.pendingKeyHeld = false;
            if (!algorithm->dtc->sustainPedal[channel]) {
                if (voice.pendingStart)
                    voice.pendingStart = false;
                voice.gate = false;
            }
        }
    } else if (message == 0xa0U) {
        setPolyAftertouch(algorithm, channel, data1,
                          data2 & 0x7fU);
    } else if (message == 0xb0U) {
        if (data1 == 120U || data1 == 123U)
            resetAllVoices(algorithm);
        else if (data1 == 1U)
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

void midiRealtime(_NT_algorithm* self, uint8_t byte) {
    if (byte == 0xfcU || byte == 0xffU)
        resetAllVoices(static_cast<IcyBeautyAlgorithm*>(self));
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

void releaseCvGroup(IcyBeautyAlgorithm* algorithm, uint8_t group,
                    bool fast) {
    for (uint8_t index = 0; index < algorithm->voiceCount; ++index) {
        Voice& voice = algorithm->dtc->voices[index];
        if (voice.source != kVoiceCv || voice.cvGroup != group)
            continue;
        if (fast)
            fastReleaseVoice(voice);
        else
            voice.gate = false;
    }
}

void releaseRemovedCvVoices(IcyBeautyAlgorithm* algorithm, uint8_t group,
                            uint8_t retainedCount) {
    for (uint8_t index = 0; index < algorithm->voiceCount; ++index) {
        Voice& voice = algorithm->dtc->voices[index];
        if (voice.source == kVoiceCv && voice.cvGroup == group &&
            voice.cvOrdinal >= retainedCount)
            fastReleaseVoice(voice);
    }
}

void reconcileConfiguration(IcyBeautyAlgorithm* algorithm) {
    IcyBeautyDtc* dtc = algorithm->dtc;
    for (uint8_t group = 0; group < algorithm->gateGroupCount; ++group) {
        GateGroupState& state = dtc->groups[group];
        const uint8_t count = algorithm->groupCount(group);
        const int16_t gateBus =
            algorithm->v[groupGateParameter(group)];
        if (gateBus != state.gateBus) {
            releaseCvGroup(algorithm, group, true);
            state.high = false;
            state.gateBus = gateBus;
        }
        if (count < state.appliedCount)
            releaseRemovedCvVoices(algorithm, group, count);
        state.appliedCount = count;
    }

    const uint8_t firstMidi = algorithm->midiStart();
    for (uint8_t index = 0; index < firstMidi; ++index) {
        Voice& voice = dtc->voices[index];
        if (voice.source == kVoiceMidi)
            fastReleaseVoice(voice);
    }
}

void correctParameter(IcyBeautyAlgorithm* algorithm, int parameter,
                      int16_t value) {
    if (algorithm->v[parameter] == value)
        return;
    const int32_t algorithmIndex = NT_algorithmIndex(algorithm);
    if (algorithmIndex >= 0) {
        NT_setParameterFromAudio(
            static_cast<uint32_t>(algorithmIndex),
            static_cast<uint32_t>(parameter) + NT_parameterOffset(),
            value);
    }
}

void syncPitchRoutingParameters(IcyBeautyAlgorithm* algorithm) {
    for (uint8_t group = 0; group < algorithm->gateGroupCount; ++group) {
        const int gateBus = algorithm->v[groupGateParameter(group)];
        const uint8_t count = algorithm->groupCount(group);
        for (uint8_t ordinal = 0;
             ordinal < algorithm->pitchRoutingCount; ++ordinal) {
            int16_t pitchBus = 0;
            if (gateBus > 0 && ordinal < count &&
                gateBus + ordinal + 1 <= kNT_lastBus) {
                pitchBus = static_cast<int16_t>(gateBus + ordinal + 1);
            }
            correctParameter(
                algorithm,
                algorithm->pitchRoutingParameter(group, ordinal),
                pitchBus);
        }
    }
}

void refreshCountDefinitions(IcyBeautyAlgorithm* algorithm) {
    const int32_t algorithmIndex = NT_algorithmIndex(algorithm);
    for (uint8_t group = 0; group < algorithm->gateGroupCount; ++group) {
        const int parameter = groupCountParameter(group);
        const int16_t maximum =
            static_cast<int16_t>(algorithm->countMaximum(group));
        if (algorithm->parameterDefs[parameter].max == maximum)
            continue;
        algorithm->parameterDefs[parameter].max = maximum;
        if (algorithmIndex >= 0) {
            NT_updateParameterDefinition(
                static_cast<uint32_t>(algorithmIndex),
                static_cast<uint32_t>(parameter));
        }
    }
}

void parameterChanged(_NT_algorithm* self, int parameter) {
    IcyBeautyAlgorithm* algorithm = static_cast<IcyBeautyAlgorithm*>(self);
    if (algorithm->updatingParameter)
        return;
    algorithm->updatingParameter = true;

    for (uint8_t group = 0; group < algorithm->gateGroupCount; ++group) {
        const int countParameter = groupCountParameter(group);
        const int gateParameter = groupGateParameter(group);
        if (parameter != countParameter && parameter != gateParameter)
            continue;
        int value = algorithm->v[countParameter];
        const int maximum = algorithm->countMaximum(group);
        if (value < 0)
            value = 0;
        if (value > maximum)
            value = maximum;
        correctParameter(algorithm, countParameter,
                         static_cast<int16_t>(value));
        break;
    }

    refreshCountDefinitions(algorithm);
    syncPitchRoutingParameters(algorithm);
    reconcileConfiguration(algorithm);
    algorithm->updatingParameter = false;
}

float busValue(const float* busFrames, int numFrames, int bus, int frame) {
    if (bus <= 0 || bus > kNT_lastBus)
        return 0.0f;
    return busFrames[(bus - 1) * numFrames + frame];
}

void startCvVoice(IcyBeautyAlgorithm* algorithm, uint8_t group,
                  uint8_t ordinal, float pitchVolts) {
    Voice* voice = findCvVoice(algorithm, group, ordinal);
    const uint32_t increment = phaseIncrementForPitchCv(pitchVolts);
    if (voice != NULL) {
        startVoice(algorithm->dtc, *voice, kVoiceCv, 0, 0,
                   increment, 0.8f, group, ordinal, false, true);
        return;
    }

    const uint8_t slot =
        static_cast<uint8_t>(algorithm->groupStart(group) + ordinal);
    if (slot >= algorithm->voiceCount)
        return;
    voice = &algorithm->dtc->voices[slot];
    if (voice->source == kVoiceCv &&
        voice->cvGroup < algorithm->gateGroupCount &&
        voice->cvOrdinal <
            algorithm->groupCount(voice->cvGroup) &&
        (voice->gate || voice->envelope > kSafeContribution)) {
        // A live Count edit can move a retained logical voice across a new
        // partition boundary. Preserve that voice rather than stealing it;
        // the newly allocated ordinal remains quiet until a later rising
        // edge finds the slot safely available.
        return;
    }
    if (voice->source == kVoiceUnused ||
        voice->envelope <= kSafeContribution) {
        startVoice(algorithm->dtc, *voice, kVoiceCv, 0, 0,
                   increment, 0.8f, group, ordinal, false, true);
    } else {
        queueVoiceStart(*voice, kVoiceCv, 0, 0, increment, 0.8f,
                        group, ordinal, false);
    }
}

void triggerCvGroup(IcyBeautyAlgorithm* algorithm, uint8_t group,
                    const float* busFrames, int numFrames, int frame) {
    const int gateBus = algorithm->v[groupGateParameter(group)];
    const uint8_t count = algorithm->groupCount(group);
    for (uint8_t ordinal = 0; ordinal < count; ++ordinal) {
        const int pitchBus = gateBus + ordinal + 1;
        startCvVoice(algorithm, group, ordinal,
                     busValue(busFrames, numFrames, pitchBus, frame));
    }
}

void trackCvGroup(IcyBeautyAlgorithm* algorithm, uint8_t group,
                  const float* busFrames, int numFrames, int frame) {
    if (!algorithm->dtc->groups[group].high ||
        algorithm->v[groupSampleHoldParameter(group)] != 0)
        return;
    const int gateBus = algorithm->v[groupGateParameter(group)];
    const uint8_t count = algorithm->groupCount(group);
    for (uint8_t ordinal = 0; ordinal < count; ++ordinal) {
        Voice* voice = findCvVoice(algorithm, group, ordinal);
        if (voice == NULL || !voice->gate || voice->pendingStart)
            continue;
        const int pitchBus = gateBus + ordinal + 1;
        voice->basePhaseIncrement = phaseIncrementForPitchCv(
            busValue(busFrames, numFrames, pitchBus, frame));
        voice->phaseIncrement = voice->basePhaseIncrement;
    }
}

void processCvGates(IcyBeautyAlgorithm* algorithm,
                    const float* busFrames, int numFrames, int frame) {
    for (uint8_t group = 0; group < algorithm->gateGroupCount; ++group) {
        GateGroupState& state = algorithm->dtc->groups[group];
        const int gateBus = algorithm->v[groupGateParameter(group)];
        if (gateBus <= 0) {
            if (state.high)
                releaseCvGroup(algorithm, group, false);
            state.high = false;
            continue;
        }

        const float gate = busValue(busFrames, numFrames, gateBus, frame);
        if (!state.high && gate > 1.0f) {
            state.high = true;
            triggerCvGroup(algorithm, group, busFrames, numFrames, frame);
        } else if (state.high && gate < 0.5f) {
            state.high = false;
            releaseCvGroup(algorithm, group, false);
        }
        trackCvGroup(algorithm, group, busFrames, numFrames, frame);
    }
}

bool pendingStartIsValid(const IcyBeautyAlgorithm* algorithm,
                         const Voice& voice, uint8_t index) {
    if (!voice.pendingStart)
        return false;
    if (voice.pendingSource == kVoiceMidi) {
        if (index < algorithm->midiStart())
            return false;
        return voice.pendingKeyHeld ||
               algorithm->dtc->sustainPedal[voice.pendingChannel];
    }
    if (voice.pendingSource == kVoiceCv) {
        const uint8_t group = voice.pendingCvGroup;
        return group < algorithm->gateGroupCount &&
               voice.pendingCvOrdinal < algorithm->groupCount(group) &&
               algorithm->dtc->groups[group].high;
    }
    return false;
}

void finishFastRelease(IcyBeautyAlgorithm* algorithm, Voice& voice,
                       uint8_t index) {
    if (!pendingStartIsValid(algorithm, voice, index)) {
        clearVoice(voice);
        return;
    }

    const uint8_t source = voice.pendingSource;
    const uint8_t channel = voice.pendingChannel;
    const uint8_t note = voice.pendingNote;
    const uint32_t increment = voice.pendingBasePhaseIncrement;
    const float velocity = voice.pendingVelocity;
    const uint8_t group = voice.pendingCvGroup;
    const uint8_t ordinal = voice.pendingCvOrdinal;
    const bool keyHeld = voice.pendingKeyHeld;
    const bool gate = source == kVoiceCv || keyHeld ||
                      algorithm->dtc->sustainPedal[channel];
    startVoice(algorithm->dtc, voice, source, channel, note, increment,
               velocity, group, ordinal, keyHeld, gate);
}

void step(_NT_algorithm* self, float* busFrames, int numFramesBy4) {
    IcyBeautyAlgorithm* algorithm = static_cast<IcyBeautyAlgorithm*>(self);
    IcyBeautyDtc* dtc = algorithm->dtc;
    const int numFrames = numFramesBy4 * 4;
    if (numFrames <= 0)
        return;
    reconcileConfiguration(algorithm);

    float* output = busFrames +
                    (algorithm->v[kParamOutput] - 1) * numFrames;
    const bool replace = algorithm->v[kParamOutputMode] != 0;

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
    float fastReleaseRate =
        9.210340372f / (sampleRate * kFastReleaseSeconds);
    if (fastReleaseRate > 1.0f)
        fastReleaseRate = 1.0f;

    static const float kVoiceGain[kMaxVoices] = {
        0.630000f, 0.445477f, 0.363731f, 0.315000f,
        0.281745f, 0.257196f, 0.238110f, 0.222739f,
        0.210000f, 0.199223f, 0.189948f, 0.181865f,
        0.174723f, 0.168372f, 0.162666f, 0.157500f,
    };
    const float voiceGain = kVoiceGain[algorithm->voiceCount - 1];

    for (int frame = 0; frame < numFrames; ++frame) {
        processCvGates(algorithm, busFrames, numFrames, frame);

        float mixed = 0.0f;
        for (uint8_t index = 0; index < algorithm->voiceCount; ++index) {
            Voice& voice = dtc->voices[index];
            if (voice.source == kVoiceUnused)
                continue;

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
                effectiveMotion +=
                    (1.0f - effectiveMotion) * wheel;
            }
            const float cyclesPerSample =
                voice.phaseIncrement * (1.0f / 4294967296.0f);
            float resonatorCoefficient =
                6.0f * cyclesPerSample * (2.01f + 0.7f * tone);
            if (resonatorCoefficient > 0.55f)
                resonatorCoefficient = 0.55f;
            const float resonatorDamping =
                1.3f - 0.95f * effectiveResonance;
            const float motionDepth = effectiveMotion * 0.0075f;
            const uint32_t motionIncrement = phaseIncrementForFrequency(
                0.08f + 0.92f * effectiveMotion);

            voice.motionPhase += motionIncrement;
            const float motionSignal = triangle(voice.motionPhase);
            const float noise = nextNoise(voice);
            float frequencyScale =
                1.0f + motionDepth * motionSignal +
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

            if (voice.fastRelease) {
                voice.envelope +=
                    fastReleaseRate * (0.0f - voice.envelope);
            } else {
                const float target = voice.gate ? 1.0f : 0.0f;
                const float envelopeRate =
                    voice.gate ? 0.0015f : releaseRate;
                voice.envelope +=
                    envelopeRate * (target - voice.envelope);
            }

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
                resonatorCoefficient * voice.resonatorBand;
            const float resonatorHigh =
                voice.toneState - voice.resonatorLow -
                resonatorDamping * voice.resonatorBand;
            voice.resonatorBand +=
                resonatorCoefficient * resonatorHigh;
            const float signal =
                (1.0f - 0.2f * effectiveResonance) *
                    voice.toneState +
                0.35f * effectiveResonance * voice.resonatorBand;
            mixed += voice.velocity * voice.envelope * signal;

            if (voice.fastRelease &&
                voice.envelope <= kSafeContribution) {
                finishFastRelease(algorithm, voice, index);
            } else if (!voice.gate && !voice.pendingStart &&
                       voice.envelope < 0.000001f) {
                clearVoice(voice);
            }
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
    .parameterChanged = parameterChanged,
    .step = step,
    .draw = NULL,
    .midiRealtime = midiRealtime,
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
