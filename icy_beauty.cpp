#include <distingnt/api.h>

#include <math.h>
#include <new>
#include <stdint.h>

namespace {

struct IcyBeautyDtc {
    uint32_t fundamentalPhase;
    uint32_t glassPhase;
    uint32_t phaseIncrement;
    float envelope;
    float velocity;
    uint8_t note;
    bool gate;
};

struct IcyBeautyAlgorithm : public _NT_algorithm {
    explicit IcyBeautyAlgorithm(IcyBeautyDtc* dtcMemory) : dtc(dtcMemory) {}
    ~IcyBeautyAlgorithm() {}

    IcyBeautyDtc* dtc;
};

enum Parameter {
    kParamOutput,
    kParamOutputMode,
    kParamMidiChannel,
};

static const char* const kMidiChannels[] = {
    "Omni", "1", "2", "3", "4", "5", "6", "7", "8",
    "9", "10", "11", "12", "13", "14", "15", "16",
};

static const _NT_parameter kParameters[] = {
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("Output", 1, 13)
    { .name = "MIDI channel", .min = 0, .max = 16, .def = 0,
      .unit = kNT_unitEnum, .scaling = 0, .enumStrings = kMidiChannels },
};

static const uint8_t kSetupParameters[] = {kParamMidiChannel};
static const uint8_t kRoutingParameters[] = {kParamOutput, kParamOutputMode};

static const _NT_parameterPage kPages[] = {
    { .name = "Setup", .numParams = ARRAY_SIZE(kSetupParameters), .group = 0,
      .unused = {0, 0}, .params = kSetupParameters },
    { .name = "Routing", .numParams = ARRAY_SIZE(kRoutingParameters), .group = 0,
      .unused = {0, 0}, .params = kRoutingParameters },
};

static const _NT_parameterPages kParameterPages = {
    .numPages = ARRAY_SIZE(kPages),
    .pages = kPages,
};

void calculateRequirements(_NT_algorithmRequirements& requirements,
                           const int32_t*) {
    requirements.numParameters = ARRAY_SIZE(kParameters);
    requirements.sram = sizeof(IcyBeautyAlgorithm);
    requirements.dram = 0;
    requirements.dtc = sizeof(IcyBeautyDtc);
    requirements.itc = 0;
}

_NT_algorithm* construct(const _NT_algorithmMemoryPtrs& memory,
                         const _NT_algorithmRequirements&,
                         const int32_t*) {
    IcyBeautyDtc* dtc = reinterpret_cast<IcyBeautyDtc*>(memory.dtc);
    IcyBeautyAlgorithm* algorithm =
        new (memory.sram) IcyBeautyAlgorithm(dtc);

    algorithm->parameters = kParameters;
    algorithm->parameterPages = &kParameterPages;

    dtc->fundamentalPhase = 0;
    dtc->glassPhase = 0x40000000U;
    dtc->phaseIncrement = 0;
    dtc->envelope = 0.0f;
    dtc->velocity = 0.0f;
    dtc->note = 0;
    dtc->gate = false;

    return algorithm;
}

bool acceptsMidiChannel(const IcyBeautyAlgorithm* algorithm, uint8_t status) {
    const int selectedChannel = algorithm->v[kParamMidiChannel];
    return selectedChannel == 0 ||
           (status & 0x0fU) == static_cast<uint8_t>(selectedChannel - 1);
}

uint32_t phaseIncrementForNote(uint8_t note) {
    const float semitones = (static_cast<int>(note) - 69) / 12.0f;
    const float frequency = 440.0f * exp2f(semitones);
    const uint32_t sampleRate = NT_globals.sampleRate == 0
                                    ? 48000U
                                    : NT_globals.sampleRate;
    return static_cast<uint32_t>(
        frequency * (4294967296.0 / static_cast<double>(sampleRate)));
}

void midiMessage(_NT_algorithm* self, uint8_t status, uint8_t data1,
                 uint8_t data2) {
    IcyBeautyAlgorithm* algorithm = static_cast<IcyBeautyAlgorithm*>(self);
    IcyBeautyDtc* dtc = algorithm->dtc;

    if (!acceptsMidiChannel(algorithm, status))
        return;

    const uint8_t message = status & 0xf0U;
    if (message == 0x90U && data2 != 0) {
        dtc->note = data1;
        dtc->phaseIncrement = phaseIncrementForNote(data1);
        dtc->fundamentalPhase = 0;
        dtc->glassPhase = 0x40000000U;
        dtc->velocity = data2 * (1.0f / 127.0f);
        dtc->gate = true;
    } else if ((message == 0x80U ||
                (message == 0x90U && data2 == 0)) &&
               data1 == dtc->note) {
        dtc->gate = false;
    }
}

float triangle(uint32_t phase) {
    const float saw = static_cast<float>(phase >> 8) *
                          (1.0f / 8388608.0f) -
                      1.0f;
    return 1.0f - 2.0f * fabsf(saw);
}

void step(_NT_algorithm* self, float* busFrames, int numFramesBy4) {
    IcyBeautyAlgorithm* algorithm = static_cast<IcyBeautyAlgorithm*>(self);
    IcyBeautyDtc* dtc = algorithm->dtc;
    const int numFrames = numFramesBy4 * 4;
    float* output = busFrames +
                    (algorithm->v[kParamOutput] - 1) * numFrames;
    const bool replace = algorithm->v[kParamOutputMode] != 0;

    for (int frame = 0; frame < numFrames; ++frame) {
        dtc->fundamentalPhase += dtc->phaseIncrement;
        dtc->glassPhase += dtc->phaseIncrement * 2U +
                           dtc->phaseIncrement / 127U;

        const float target = dtc->gate ? 1.0f : 0.0f;
        const float envelopeRate = dtc->gate ? 0.0045f : 0.00018f;
        dtc->envelope += envelopeRate * (target - dtc->envelope);

        const float voice = 0.72f * triangle(dtc->fundamentalPhase) +
                            0.28f * triangle(dtc->glassPhase);
        const float sample = 3.5f * dtc->velocity * dtc->envelope * voice;
        output[frame] = replace ? sample : output[frame] + sample;
    }
}

static const _NT_factory kFactory = {
    .guid = NT_MULTICHAR('N', 's', 'I', 'b'),
    .name = "Icy Beauty",
    .description = "A haunting, delicate software synthesizer",
    .numSpecifications = 0,
    .specifications = NULL,
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
