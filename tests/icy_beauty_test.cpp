#include <distingnt/api.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

_NT_algorithm* gActiveAlgorithm = NULL;
int16_t* gActiveValues = NULL;
uint32_t gActiveValueCount = 0;
uint32_t gDefinitionUpdates = 0;

}  // namespace

extern "C" int32_t NT_algorithmIndex(const _NT_algorithm* algorithm) {
    return algorithm == gActiveAlgorithm ? 0 : -1;
}

extern "C" void NT_setParameterFromAudio(uint32_t algorithmIndex,
                                          uint32_t parameter,
                                          int16_t value) {
    if (algorithmIndex == 0 && parameter < gActiveValueCount)
        gActiveValues[parameter] = value;
}

extern "C" uint32_t NT_parameterOffset(void) {
    return 0;
}

extern "C" void NT_updateParameterDefinition(uint32_t algorithmIndex,
                                              uint32_t parameterIndex) {
    if (algorithmIndex == 0 && parameterIndex < gActiveValueCount)
        ++gDefinitionUpdates;
}

namespace {

int fail(const char* message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

std::vector<std::max_align_t> allocateAligned(std::size_t byteCount) {
    const std::size_t count =
        (byteCount + sizeof(std::max_align_t) - 1) /
        sizeof(std::max_align_t);
    return std::vector<std::max_align_t>(count == 0 ? 1 : count);
}

struct HostInstance {
    HostInstance(const _NT_factory* instanceFactory, int32_t voices = 8,
                 int32_t gateGroups = 4)
        : factory(instanceFactory), requirements(), algorithm(NULL) {
        specification[0] = voices;
        specification[1] = gateGroups;
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
             parameter < requirements.numParameters; ++parameter)
            values[parameter] = algorithm->parameters[parameter].def;
        algorithm->v = values.data();
        algorithm->vIncludingCommon = values.data();
        activate();
    }

    HostInstance(const HostInstance&) = delete;
    HostInstance& operator=(const HostInstance&) = delete;

    void activate() {
        gActiveAlgorithm = algorithm;
        gActiveValues = values.data();
        gActiveValueCount = static_cast<uint32_t>(values.size());
    }

    void setParameter(int parameter, int16_t value) {
        activate();
        values[parameter] = value;
        if (factory->parameterChanged != NULL)
            factory->parameterChanged(algorithm, parameter);
    }

    void render(std::vector<float>& busses, int frames) {
        activate();
        factory->step(algorithm, busses.data(), frames / 4);
    }

    IcyBeautyAlgorithm* synth() {
        return static_cast<IcyBeautyAlgorithm*>(algorithm);
    }

    const IcyBeautyAlgorithm* synth() const {
        return static_cast<const IcyBeautyAlgorithm*>(algorithm);
    }

    const _NT_factory* factory;
    int32_t specification[2];
    _NT_algorithmRequirements requirements;
    std::vector<std::max_align_t> sram;
    std::vector<std::max_align_t> dtc;
    _NT_algorithm* algorithm;
    std::vector<int16_t> values;
};

std::vector<float> makeBusses(int frames) {
    return std::vector<float>(kNT_lastBus * frames, 0.0f);
}

void fillBus(std::vector<float>& busses, int frames, int bus, float value) {
    float* first = busses.data() + (bus - 1) * frames;
    std::fill(first, first + frames, value);
}

void renderBlocks(HostInstance& host, std::vector<float>& busses,
                  int frames, int blocks) {
    for (int block = 0; block < blocks; ++block)
        host.render(busses, frames);
}

int countVoices(const HostInstance& host, uint8_t source) {
    int count = 0;
    for (uint8_t index = 0; index < host.synth()->voiceCount; ++index) {
        if (host.synth()->dtc->voices[index].source == source)
            ++count;
    }
    return count;
}

int countMidiIdentity(const HostInstance& host, uint8_t channel,
                      uint8_t note) {
    int count = 0;
    for (uint8_t index = 0; index < host.synth()->voiceCount; ++index) {
        const Voice& voice = host.synth()->dtc->voices[index];
        if (voice.source == kVoiceMidi && voice.channel == channel &&
            voice.note == note)
            ++count;
    }
    return count;
}

Voice* cvVoice(HostInstance& host, uint8_t group, uint8_t ordinal) {
    return findCvVoice(host.synth(), group, ordinal);
}

int semitoneOffsetParameter(const HostInstance& host) {
    return static_cast<int>(host.requirements.numParameters) - 2;
}

int octaveOffsetParameter(const HostInstance& host) {
    return semitoneOffsetParameter(host) + 1;
}

bool declaresCvInput(const HostInstance& host, int bus) {
    for (uint32_t parameter = 0;
         parameter < host.requirements.numParameters; ++parameter) {
        if (host.algorithm->parameters[parameter].unit == kNT_unitCvInput &&
            host.values[parameter] == bus)
            return true;
    }
    return false;
}

void renderUsingDeclaredInputs(HostInstance& host,
                               const std::vector<float>& sourceBusses,
                               int frames) {
    std::vector<float> routedBusses = sourceBusses;
    for (int bus = 1; bus <= 12; ++bus) {
        if (!declaresCvInput(host, bus))
            fillBus(routedBusses, frames, bus, 0.0f);
    }
    host.render(routedBusses, frames);
}

bool allVoicesUnused(const HostInstance& host) {
    for (uint8_t index = 0; index < host.synth()->voiceCount; ++index) {
        if (host.synth()->dtc->voices[index].source != kVoiceUnused)
            return false;
    }
    return true;
}

double frequencyForPhaseIncrement(uint32_t increment) {
    return increment * (48000.0 / 4294967296.0);
}

bool allFiniteAndBounded(const std::vector<float>& busses, int frames,
                         float* peak) {
    const float* output = busses.data() + 12 * frames;
    for (int frame = 0; frame < frames; ++frame) {
        if (!std::isfinite(output[frame]) ||
            std::fabs(output[frame]) >= kOutputPeakVolts)
            return false;
        *peak = std::max(*peak, std::fabs(output[frame]));
    }
    return true;
}

int testFactoryAndSurface(const _NT_factory* factory) {
    if (factory->numSpecifications != 2)
        return fail("the factory must expose Voices and Gate groups");
    const _NT_specification& voices = factory->specifications[0];
    const _NT_specification& groups = factory->specifications[1];
    if (std::strcmp(voices.name, "Voices") != 0 ||
        voices.min != 1 || voices.max != 16 || voices.def != 8)
        return fail("Voices must expose 1..16 with default 8");
    if (std::strcmp(groups.name, "Gate groups") != 0 ||
        groups.min != 0 || groups.max != 6 || groups.def != 4)
        return fail("Gate groups must expose 0..6 with default 4");

    for (int voiceChoice = 1; voiceChoice <= kMaxVoices;
         ++voiceChoice) {
        for (int groupCount = 0; groupCount <= kMaxGateGroups;
             ++groupCount) {
            HostInstance host(factory, voiceChoice, groupCount);
            const int pitchRoutingCount =
                std::min(voiceChoice, static_cast<int>(kMaxCvPerGate));
            const uint32_t expectedParameters =
                kNumCommonParameters +
                groupCount * kParametersPerGroup +
                kNumSoundParameters + groupCount * pitchRoutingCount + 2;
            if (host.requirements.numParameters != expectedParameters ||
                host.requirements.dram != 0 ||
                host.requirements.dtc != sizeof(IcyBeautyDtc) ||
                host.synth()->voiceCount != voiceChoice ||
                host.synth()->gateGroupCount != groupCount)
                return fail("construction requirements are not fixed and specification-driven");

            const _NT_parameterPages* pages =
                host.algorithm->parameterPages;
            if (pages == NULL || pages->numPages != 4 ||
                pages->pages[0].numParams != 3 ||
                pages->pages[2].numParams !=
                    groupCount * kParametersPerGroup)
                return fail("parameter pages do not match Setup and Gate groups");

            const int semitones = semitoneOffsetParameter(host);
            const int octaves = octaveOffsetParameter(host);
            if (pages->pages[0].params[0] != kParamMidiChannel ||
                pages->pages[0].params[1] != semitones ||
                pages->pages[0].params[2] != octaves ||
                std::strcmp(host.algorithm->parameters[semitones].name,
                            "Semitones") != 0 ||
                host.algorithm->parameters[semitones].min != -11 ||
                host.algorithm->parameters[semitones].max != 11 ||
                host.algorithm->parameters[semitones].def != 0 ||
                host.algorithm->parameters[semitones].unit !=
                    kNT_unitSemitones ||
                std::strcmp(host.algorithm->parameters[octaves].name,
                            "Octaves") != 0 ||
                host.algorithm->parameters[octaves].min != -4 ||
                host.algorithm->parameters[octaves].max != 4 ||
                host.algorithm->parameters[octaves].def != 0 ||
                host.algorithm->parameters[octaves].unit != kNT_unitNone)
                return fail("Setup pitch offsets are missing or malformed");

            for (int group = 0; group < groupCount; ++group) {
                const int gate = groupGateParameter(group);
                const int count = groupCountParameter(group);
                const int sampleHold =
                    groupSampleHoldParameter(group);
                if (host.values[gate] != 0 ||
                    host.values[count] != 0 ||
                    host.values[sampleHold] != 0 ||
                    host.algorithm->parameters[gate].min != 0 ||
                    host.algorithm->parameters[gate].unit !=
                        kNT_unitCvInput ||
                    host.algorithm->parameters[count].min != 0 ||
                    host.algorithm->parameters[count].max !=
                        std::min(voiceChoice,
                                 static_cast<int>(kMaxCvPerGate)) ||
                    host.algorithm->parameters[sampleHold].unit !=
                        kNT_unitEnum)
                    return fail("a gate group is not safe and disconnected by default");
                for (int ordinal = 0; ordinal < pitchRoutingCount;
                     ++ordinal) {
                    const int routing = host.synth()->pitchRoutingParameter(
                        static_cast<uint8_t>(group),
                        static_cast<uint8_t>(ordinal));
                    if (host.values[routing] != 0 ||
                        host.algorithm->parameters[routing].unit !=
                            kNT_unitCvInput)
                        return fail("a derived pitch bus is not safely disconnected by default");
                }
            }
        }
    }
    std::puts(
        "PASS: fixed 16-voice memory supports Voices 1..16 and Gate groups "
        "0..6 with disconnected group controls");
    return 0;
}

int testCountBounds(const _NT_factory* factory) {
    HostInstance host(factory, 8, 4);
    const int count0 = groupCountParameter(0);
    const int count1 = groupCountParameter(1);
    const int count2 = groupCountParameter(2);
    const int gate0 = groupGateParameter(0);

    host.setParameter(count0, 6);
    if (host.values[count0] != 6 ||
        host.algorithm->parameters[count1].max != 2)
        return fail("Count did not reserve the remaining Voices");
    host.setParameter(count1, 9);
    if (host.values[count1] != 2 || host.values[count0] != 6)
        return fail("only the edited Count should be clamped");
    host.setParameter(count0, 7);
    if (host.values[count0] != 6 || host.values[count1] != 2)
        return fail("an over-allocated edit changed another group");
    host.setParameter(count2, -4);
    if (host.values[count2] != 0)
        return fail("negative Count was not tolerated and clamped");

    host.setParameter(gate0, kNT_lastBus - 1);
    if (host.values[count0] != 1 ||
        host.algorithm->parameters[count0].max != 1 ||
        host.values[count1] != 2)
        return fail("following-bus room did not safely bound the paired Count");
    host.setParameter(gate0, 0);
    if (host.algorithm->parameters[count0].max != 6)
        return fail("None gate should not impose a following-bus limit");

    HostInstance wide(factory, 16, 2);
    const int wideCount0 = groupCountParameter(0);
    const int wideCount1 = groupCountParameter(1);
    if (wide.algorithm->parameters[wideCount0].max != 11)
        return fail("Count nominal maximum must remain 11");
    wide.setParameter(wideCount0, 11);
    wide.setParameter(wideCount1, 11);
    if (wide.values[wideCount0] != 11 ||
        wide.values[wideCount1] != 5)
        return fail("Counts were not bounded by Voices 16");
    if (gDefinitionUpdates == 0)
        return fail("dynamic Count definition changes were not published");

    std::puts(
        "PASS: Count edits are tolerant, clamp only themselves, and remain "
        "bounded by Voices, 11 CVs, and following buses");
    return 0;
}

int testDefaultMidiOwnership(const _NT_factory* factory) {
    HostInstance defaultHost(factory, 8, 4);
    for (uint8_t note = 60; note < 68; ++note)
        factory->midiMessage(defaultHost.algorithm, 0x90, note, 100);
    if (countVoices(defaultHost, kVoiceMidi) != 8 ||
        countVoices(defaultHost, kVoiceCv) != 0)
        return fail("zero default Counts did not leave all Voices to MIDI");

    HostInstance midiOnly(factory, 16, 0);
    for (uint8_t note = 48; note < 64; ++note)
        factory->midiMessage(midiOnly.algorithm, 0x90, note, 100);
    if (countVoices(midiOnly, kVoiceMidi) != 16)
        return fail("Gate groups 0 did not provide a 16-voice MIDI synth");

    HostInstance allCv(factory, 4, 1);
    allCv.setParameter(groupCountParameter(0), 4);
    factory->midiMessage(allCv.algorithm, 0x90, 60, 100);
    if (countVoices(allCv, kVoiceMidi) != 0)
        return fail("MIDI Note On was not ignored when CV owns every Voice");

    HostInstance split(factory, 4, 1);
    split.setParameter(groupGateParameter(0), 1);
    split.setParameter(groupCountParameter(0), 2);
    const int frames = 64;
    std::vector<float> busses = makeBusses(frames);
    fillBus(busses, frames, 1, 5.0f);
    fillBus(busses, frames, 2, 0.0f);
    fillBus(busses, frames, 3, 0.5f);
    split.render(busses, frames);
    factory->midiMessage(split.algorithm, 0x90, 60, 100);
    factory->midiMessage(split.algorithm, 0x90, 64, 100);
    factory->midiMessage(split.algorithm, 0x90, 67, 100);
    if (countVoices(split, kVoiceCv) != 2 ||
        cvVoice(split, 0, 0) == NULL ||
        cvVoice(split, 0, 1) == NULL ||
        countVoices(split, kVoiceMidi) != 2)
        return fail("MIDI note events crossed into the CV partition");

    std::puts(
        "PASS: default and zero-group instances are all MIDI; CV and MIDI "
        "partitions never cross-steal");
    return 0;
}

int testGateGroups(const _NT_factory* factory) {
    const int frames = 64;
    HostInstance host(factory, 8, 2);
    host.setParameter(groupGateParameter(0), 1);
    host.setParameter(groupCountParameter(0), 2);
    host.setParameter(groupGateParameter(1), 4);
    host.setParameter(groupCountParameter(1), 2);

    std::vector<float> busses = makeBusses(frames);
    fillBus(busses, frames, 1, 5.0f);
    fillBus(busses, frames, 2, 0.0f);
    fillBus(busses, frames, 3, 0.5f);
    fillBus(busses, frames, 4, 0.0f);
    fillBus(busses, frames, 5, 1.0f);
    fillBus(busses, frames, 6, 1.5f);
    host.render(busses, frames);
    if (cvVoice(host, 0, 0) == NULL ||
        cvVoice(host, 0, 1) == NULL ||
        cvVoice(host, 1, 0) != NULL)
        return fail("group 1 rising edge affected the wrong group");

    fillBus(busses, frames, 1, 0.75f);
    host.render(busses, frames);
    if (!host.synth()->dtc->groups[0].high ||
        !cvVoice(host, 0, 0)->gate)
        return fail("gate hysteresis did not retain state in the middle band");

    fillBus(busses, frames, 1, 0.49f);
    fillBus(busses, frames, 4, 5.0f);
    host.render(busses, frames);
    if (host.synth()->dtc->groups[0].high ||
        cvVoice(host, 0, 0)->gate ||
        cvVoice(host, 1, 0) == NULL ||
        !cvVoice(host, 1, 0)->gate)
        return fail("independent falling and rising edges were not isolated");

    host.setParameter(groupCountParameter(1), 3);
    host.render(busses, frames);
    if (cvVoice(host, 1, 2) != NULL)
        return fail("a Count increase started a voice while Gate was held");
    fillBus(busses, frames, 4, 0.0f);
    host.render(busses, frames);
    fillBus(busses, frames, 4, 5.0f);
    host.render(busses, frames);
    if (cvVoice(host, 1, 2) == NULL ||
        !cvVoice(host, 1, 2)->gate)
        return fail("a newly added voice did not wait for the next rising Gate");

    host.setParameter(groupCountParameter(1), 2);
    Voice* removed = cvVoice(host, 1, 2);
    if (removed == NULL || removed->gate || !removed->fastRelease)
        return fail("Count decrease did not immediately fade the removed voice");

    host.setParameter(groupCountParameter(1), 0);
    host.setParameter(groupCountParameter(1), 2);
    host.render(busses, frames);
    if (cvVoice(host, 1, 0) != NULL &&
        cvVoice(host, 1, 0)->gate)
        return fail("re-enabled Count retriggered under a held Gate");
    fillBus(busses, frames, 4, 0.0f);
    host.render(busses, frames);
    fillBus(busses, frames, 4, 5.0f);
    host.render(busses, frames);
    if (cvVoice(host, 1, 0) == NULL ||
        !cvVoice(host, 1, 0)->gate)
        return fail("re-enabled Count missed the next rising Gate");

    HostInstance boundary(factory, 4, 2);
    boundary.setParameter(groupGateParameter(0), 1);
    boundary.setParameter(groupCountParameter(0), 1);
    boundary.setParameter(groupGateParameter(1), 3);
    boundary.setParameter(groupCountParameter(1), 1);
    std::vector<float> boundaryBusses = makeBusses(frames);
    fillBus(boundaryBusses, frames, 1, 5.0f);
    fillBus(boundaryBusses, frames, 2, 0.0f);
    fillBus(boundaryBusses, frames, 3, 5.0f);
    fillBus(boundaryBusses, frames, 4, 0.5f);
    boundary.render(boundaryBusses, frames);
    Voice* retained = cvVoice(boundary, 1, 0);
    if (retained == NULL)
        return fail("boundary test did not start the retained group voice");
    const uint32_t retainedAge = retained->age;
    boundary.setParameter(groupCountParameter(0), 2);
    fillBus(boundaryBusses, frames, 1, 0.0f);
    boundary.render(boundaryBusses, frames);
    fillBus(boundaryBusses, frames, 1, 5.0f);
    boundary.render(boundaryBusses, frames);
    retained = cvVoice(boundary, 1, 0);
    if (retained == NULL || !retained->gate ||
        retained->age != retainedAge ||
        cvVoice(boundary, 0, 1) != NULL)
        return fail("a boundary shift stole or restarted a retained group voice");

    std::puts(
        "PASS: groups have independent hysteretic gates and live Count "
        "changes obey next-edge, fade-out, and retained-voice rules");
    return 0;
}

int testSampleAndHold(const _NT_factory* factory) {
    const int frames = 64;
    HostInstance host(factory, 2, 1);
    host.setParameter(groupGateParameter(0), 1);
    host.setParameter(groupCountParameter(0), 1);
    std::vector<float> busses = makeBusses(frames);
    fillBus(busses, frames, 1, 5.0f);
    fillBus(busses, frames, 2, 0.0f);
    host.render(busses, frames);
    const uint32_t zeroVolts = cvVoice(host, 0, 0)->phaseIncrement;
    fillBus(busses, frames, 2, 1.0f);
    host.render(busses, frames);
    const uint32_t tracked = cvVoice(host, 0, 0)->phaseIncrement;
    if (tracked == zeroVolts ||
        tracked != phaseIncrementForPitchCv(1.0f))
        return fail("Sample & hold Off did not track pitch continuously");

    host.setParameter(groupSampleHoldParameter(0), 1);
    fillBus(busses, frames, 1, 0.0f);
    host.render(busses, frames);
    fillBus(busses, frames, 2, 0.25f);
    fillBus(busses, frames, 1, 5.0f);
    host.render(busses, frames);
    const uint32_t sampled = cvVoice(host, 0, 0)->phaseIncrement;
    fillBus(busses, frames, 2, 1.25f);
    host.render(busses, frames);
    if (cvVoice(host, 0, 0)->phaseIncrement != sampled)
        return fail("Sample & hold On changed pitch while Gate was held");

    fillBus(busses, frames, 1, 0.0f);
    host.render(busses, frames);
    fillBus(busses, frames, 1, 5.0f);
    host.render(busses, frames);
    if (cvVoice(host, 0, 0)->phaseIncrement !=
        phaseIncrementForPitchCv(1.25f))
        return fail("Sample & hold retrigger did not resample pitch");

    std::puts(
        "PASS: Sample & hold defaults Off, tracks continuously when Off, "
        "and resamples every retrigger when On");
    return 0;
}

int testCvRetriggerDeclick(const _NT_factory* factory) {
    const int frames = 4;
    float maximumDiscontinuity = 0.0f;
    for (int sampleHold = 0; sampleHold <= 1; ++sampleHold) {
        HostInstance host(factory, 1, 1);
        host.setParameter(groupGateParameter(0), 1);
        host.setParameter(groupCountParameter(0), 1);
        host.setParameter(groupSampleHoldParameter(0), sampleHold);
        host.setParameter(host.synth()->soundParameter(kSoundMotion), 0);
        host.setParameter(host.synth()->soundParameter(kSoundGrain), 0);
        host.setParameter(host.synth()->soundParameter(kSoundResonance), 0);
        host.setParameter(host.synth()->soundParameter(kSoundRelease), 100);

        std::vector<float> busses = makeBusses(frames);
        fillBus(busses, frames, 1, 5.0f);
        fillBus(busses, frames, 2, 0.0f);
        renderBlocks(host, busses, frames, 300);

        fillBus(busses, frames, 1, 0.0f);
        float beforeRetrigger = 0.0f;
        for (int block = 0; block < 256; ++block) {
            std::fill(busses.begin() + 12 * frames,
                      busses.begin() + 13 * frames, 0.0f);
            host.render(busses, frames);
            beforeRetrigger = busses[13 * frames - 1];
            if (std::fabs(beforeRetrigger) > 0.2f)
                break;
        }
        if (std::fabs(beforeRetrigger) <= 0.2f)
            return fail(
                "CV retrigger fixture did not reach an audible release tail");

        fillBus(busses, frames, 1, 5.0f);
        std::fill(busses.begin() + 12 * frames,
                  busses.begin() + 13 * frames, 0.0f);
        host.render(busses, frames);
        const float afterRetrigger = busses[12 * frames];
        const float discontinuity =
            std::fabs(afterRetrigger - beforeRetrigger);
        maximumDiscontinuity =
            std::max(maximumDiscontinuity, discontinuity);
        if (discontinuity >= 0.15f)
            return fail(
                "CV retrigger reset a releasing voice and introduced a click-sized discontinuity");

        const Voice* voice = cvVoice(host, 0, 0);
        if (voice == NULL || !voice->gate || voice->pendingStart ||
            voice->fastRelease || voice->envelope <= kSafeContribution)
            return fail(
                "CV retrigger did not resume its release tail immediately");
    }

    std::printf(
        "PASS: fast CV retriggers resume the release tail without latency "
        "or a click-sized discontinuity in either S&H mode (%.3f V max)\n",
        maximumDiscontinuity);
    return 0;
}

int testPitchCvCalibration() {
    static const double kZeroVoltFrequencyHz = 130.8127826502993;
    static const float kTestVolts[] = {
        -5.0f, -2.0f, -1.0f, 0.0f, 0.5f, 1.0f, 2.0f, 4.0f, 7.0f,
    };

    for (std::size_t index = 0; index < ARRAY_SIZE(kTestVolts); ++index) {
        const double expected =
            kZeroVoltFrequencyHz * std::pow(2.0, kTestVolts[index]);
        const double actual = frequencyForPhaseIncrement(
            phaseIncrementForPitchCv(kTestVolts[index]));
        const double centsError =
            1200.0 * std::log2(actual / expected);
        if (std::fabs(centsError) > 0.01) {
            std::fprintf(stderr,
                         "FAIL: %.2f V produced %.9f Hz instead of %.9f Hz "
                         "(%.6f cents)\n",
                         kTestVolts[index], actual, expected, centsError);
            return 1;
        }
    }

    const double zeroVolts = frequencyForPhaseIncrement(
        phaseIncrementForPitchCv(0.0f));
    const double oneVolt = frequencyForPhaseIncrement(
        phaseIncrementForPitchCv(1.0f));
    const double minusOneVolt = frequencyForPhaseIncrement(
        phaseIncrementForPitchCv(-1.0f));
    if (std::fabs(oneVolt / zeroVolts - 2.0) > 0.000001 ||
        std::fabs(zeroVolts / minusOneVolt - 2.0) > 0.000001 ||
        phaseIncrementForPitchCv(0.0f) != phaseIncrementForNote(48))
        return fail("Pitch CV does not preserve exact 1 V/octave octave ratios");

    if (phaseIncrementForPitchCv(-9.0f) !=
            phaseIncrementForPitchCv(-8.0f) ||
        phaseIncrementForPitchCv(9.0f) !=
            phaseIncrementForPitchCv(8.0f) ||
        frequencyForPhaseIncrement(phaseIncrementForPitchCv(8.0f)) >=
            48000.0 * 0.5)
        return fail("Pitch CV safety clamping is not bounded below Nyquist");

    std::puts(
        "PASS: Pitch CV tracks 1 V/oct from -5 V through +7 V within "
        "0.01 cent, with 0 V at MIDI note 48 and safe endpoint clamping");
    return 0;
}

int testPitchOffsets(const _NT_factory* factory) {
    HostInstance midi(factory, 1, 0);
    factory->midiMessage(midi.algorithm, 0x90, 48, 100);
    Voice& midiVoice = midi.synth()->dtc->voices[0];
    const double midiBase = frequencyForPhaseIncrement(
        midiVoice.phaseIncrement);
    midi.setParameter(semitoneOffsetParameter(midi), 7);
    midi.setParameter(octaveOffsetParameter(midi), 1);
    const double midiExpected = midiBase * std::pow(2.0, 19.0 / 12.0);
    const double midiCents = 1200.0 * std::log2(
        frequencyForPhaseIncrement(midiVoice.phaseIncrement) /
        midiExpected);
    if (std::fabs(midiCents) > 0.01)
        return fail("Setup pitch offsets did not transpose a held MIDI voice");

    const int frames = 64;
    HostInstance cv(factory, 1, 1);
    cv.setParameter(groupGateParameter(0), 1);
    cv.setParameter(groupCountParameter(0), 1);
    cv.setParameter(groupSampleHoldParameter(0), 1);
    std::vector<float> busses = makeBusses(frames);
    fillBus(busses, frames, 1, 5.0f);
    fillBus(busses, frames, 2, 0.0f);
    cv.render(busses, frames);
    Voice* cvPitch = cvVoice(cv, 0, 0);
    const double cvBase = frequencyForPhaseIncrement(
        cvPitch->phaseIncrement);

    cv.setParameter(semitoneOffsetParameter(cv), -5);
    cv.setParameter(octaveOffsetParameter(cv), -1);
    const double cvExpected = cvBase * std::pow(2.0, -17.0 / 12.0);
    const double cvCents = 1200.0 * std::log2(
        frequencyForPhaseIncrement(cvPitch->phaseIncrement) / cvExpected);
    if (std::fabs(cvCents) > 0.01)
        return fail("Setup pitch offsets did not transpose held S&H CV");

    const uint32_t offsetHeld = cvPitch->phaseIncrement;
    fillBus(busses, frames, 2, 1.0f);
    cv.render(busses, frames);
    if (cvPitch->phaseIncrement != offsetHeld)
        return fail("S&H did not hold input CV after a Setup offset change");

    fillBus(busses, frames, 1, 0.0f);
    cv.render(busses, frames);
    fillBus(busses, frames, 1, 5.0f);
    cv.render(busses, frames);
    const double retriggerExpected =
        cvBase * std::pow(2.0, 1.0 - 17.0 / 12.0);
    const double retriggerCents = 1200.0 * std::log2(
        frequencyForPhaseIncrement(cvPitch->phaseIncrement) /
        retriggerExpected);
    if (std::fabs(retriggerCents) > 0.01)
        return fail("S&H retrigger did not combine input CV and Setup offsets");

    std::puts(
        "PASS: Semitones and Octaves transpose held MIDI and CV voices "
        "while S&H continues to hold only its input CV");
    return 0;
}

int testTargetPitchRoutingAndMidiStop(const _NT_factory* factory) {
    int result = 0;
    const int frames = 64;
    HostInstance pitch(factory, 4, 2);
    pitch.setParameter(groupGateParameter(0), 9);
    pitch.setParameter(groupCountParameter(0), 3);
    std::vector<float> pitchBusses = makeBusses(frames);
    fillBus(pitchBusses, frames, 9, 5.0f);
    fillBus(pitchBusses, frames, 10, 0.0f);
    fillBus(pitchBusses, frames, 11, 0.5f);
    fillBus(pitchBusses, frames, 12, 1.0f);
    renderUsingDeclaredInputs(pitch, pitchBusses, frames);
    const float expectedPitchVolts[3] = {0.0f, 0.5f, 1.0f};
    for (uint8_t ordinal = 0; ordinal < 3; ++ordinal) {
        Voice* voice = cvVoice(pitch, 0, ordinal);
        const int pitchBus = 10 + ordinal;
        if (!declaresCvInput(pitch, pitchBus) || voice == NULL ||
            voice->phaseIncrement !=
                phaseIncrementForPitchCv(expectedPitchVolts[ordinal])) {
            std::fprintf(
                stderr,
                "FAIL: Gate Input 9 Count 3 did not declare and play "
                "independent pitches from Inputs 10, 11, and 12\n");
            result = 1;
            break;
        }
    }

    Voice* voice = cvVoice(pitch, 0, 0);
    const uint32_t zeroVoltIncrement = voice == NULL ? 0 : voice->phaseIncrement;
    fillBus(pitchBusses, frames, 10, 1.0f);
    renderUsingDeclaredInputs(pitch, pitchBusses, frames);
    voice = cvVoice(pitch, 0, 0);
    if (voice == NULL || voice->phaseIncrement == zeroVoltIncrement ||
        voice->phaseIncrement != phaseIncrementForPitchCv(1.0f)) {
        std::fprintf(
            stderr,
            "FAIL: a declared pitch bus did not track while its gate was "
            "held\n");
        result = 1;
    }

    pitch.setParameter(groupCountParameter(0), 1);
    if (declaresCvInput(pitch, 11) || declaresCvInput(pitch, 12)) {
        std::fprintf(stderr,
                     "FAIL: reducing Count did not remove inactive pitch "
                     "dependencies from host routing\n");
        result = 1;
    }

    HostInstance stop(factory, 4, 1);
    stop.setParameter(groupGateParameter(0), 1);
    stop.setParameter(groupCountParameter(0), 1);
    std::vector<float> stopBusses = makeBusses(frames);
    fillBus(stopBusses, frames, 1, 5.0f);
    fillBus(stopBusses, frames, 2, 0.0f);
    stop.render(stopBusses, frames);
    factory->midiMessage(stop.algorithm, 0x90, 60, 100);
    factory->midiMessage(stop.algorithm, 0x90, 64, 100);
    if (factory->midiRealtime == NULL) {
        std::fprintf(stderr,
                     "FAIL: MIDI Stop has no callback to reset all voices\n");
        result = 1;
    } else {
        factory->midiRealtime(stop.algorithm, 0xfcU);
        if (!allVoicesUnused(stop) ||
            !stop.synth()->dtc->groups[0].high) {
            std::fprintf(stderr,
                         "FAIL: MIDI Stop did not reset every voice and wait "
                         "for a fresh CV gate edge\n");
            result = 1;
        }
        stop.render(stopBusses, frames);
        if (!allVoicesUnused(stop)) {
            std::fprintf(stderr,
                         "FAIL: a held CV gate immediately defeated MIDI "
                         "Stop\n");
            result = 1;
        }
        fillBus(stopBusses, frames, 1, 0.0f);
        stop.render(stopBusses, frames);
        fillBus(stopBusses, frames, 1, 5.0f);
        stop.render(stopBusses, frames);
        if (cvVoice(stop, 0, 0) == NULL) {
            std::fprintf(stderr,
                         "FAIL: CV did not resume on a fresh gate edge after "
                         "MIDI Stop\n");
            result = 1;
        }
    }

    factory->midiMessage(stop.algorithm, 0xb0U, 123U, 0U);
    if (!allVoicesUnused(stop)) {
        std::fprintf(stderr,
                     "FAIL: MIDI All Notes Off did not reset every voice\n");
        result = 1;
    }

    factory->midiMessage(stop.algorithm, 0x90U, 67U, 100U);
    factory->midiMessage(stop.algorithm, 0xb0U, 120U, 0U);
    if (!allVoicesUnused(stop)) {
        std::fprintf(stderr,
                     "FAIL: MIDI All Sound Off did not reset every voice\n");
        result = 1;
    }

    factory->midiMessage(stop.algorithm, 0x90U, 69U, 100U);
    factory->midiRealtime(stop.algorithm, 0xffU);
    if (!allVoicesUnused(stop)) {
        std::fprintf(stderr,
                     "FAIL: MIDI System Reset did not reset every voice\n");
        result = 1;
    }

    if (result == 0) {
        std::puts(
            "PASS: Count-driven pitch busses are declared to host routing, "
            "and MIDI stop/panic resets every voice");
    }
    return result;
}

int testSixInputPairs(const _NT_factory* factory) {
    const int frames = 64;
    HostInstance host(factory, 16, 6);
    std::vector<float> busses = makeBusses(frames);
    for (uint8_t group = 0; group < 6; ++group) {
        const int gateBus = 1 + group * 2;
        host.setParameter(groupGateParameter(group), gateBus);
        host.setParameter(groupCountParameter(group), 1);
        fillBus(busses, frames, gateBus, 5.0f);
        fillBus(busses, frames, gateBus + 1, group * 0.1f);
    }
    host.render(busses, frames);
    for (uint8_t group = 0; group < 6; ++group) {
        Voice* voice = cvVoice(host, group, 0);
        if (voice == NULL || !voice->gate)
            return fail("six gate/pitch pairs did not trigger independently");
    }
    if (host.synth()->midiStart() != 6)
        return fail("six single-CV groups did not leave the MIDI suffix");
    for (uint8_t note = 48; note < 58; ++note)
        factory->midiMessage(host.algorithm, 0x90, note, 100);
    float peak = 0.0f;
    for (int block = 0; block < 32; ++block) {
        std::fill(busses.begin() + 12 * frames,
                  busses.begin() + 13 * frames, 0.0f);
        host.render(busses, frames);
        if (!allFiniteAndBounded(busses, frames, &peak))
            return fail("mixed six-group/ten-MIDI render was not finite and bounded");
    }
    if (countVoices(host, kVoiceCv) != 6 ||
        countVoices(host, kVoiceMidi) != 10 || peak < 0.1f)
        return fail("mixed Voices 16 ownership did not remain fully playable");

    std::puts(
        "PASS: six independent gate/pitch pairs fit the twelve physical "
        "inputs and coexist with ten MIDI voices");
    return 0;
}

int testMidiReuse(const _NT_factory* factory) {
    const int frames = 64;
    HostInstance priority(factory, 4, 0);
    for (uint8_t note = 60; note < 64; ++note)
        factory->midiMessage(priority.algorithm, 0x90, note, 100);
    std::vector<float> busses = makeBusses(frames);
    renderBlocks(priority, busses, frames, 8);

    factory->midiMessage(priority.algorithm, 0x80, 61, 0);
    factory->midiMessage(priority.algorithm, 0xb0, 64, 127);
    factory->midiMessage(priority.algorithm, 0x80, 62, 0);
    factory->midiMessage(priority.algorithm, 0x90, 80, 100);
    if (countMidiIdentity(priority, 0, 61) != 0 ||
        countMidiIdentity(priority, 0, 80) != 1 ||
        countMidiIdentity(priority, 0, 62) != 1)
        return fail("MIDI did not reuse the oldest released key first");
    factory->midiMessage(priority.algorithm, 0x90, 81, 100);
    if (countMidiIdentity(priority, 0, 62) != 0 ||
        countMidiIdentity(priority, 0, 81) != 1)
        return fail("MIDI did not reuse the oldest sustain-held key second");
    factory->midiMessage(priority.algorithm, 0x90, 82, 100);
    if (countMidiIdentity(priority, 0, 60) != 0 ||
        countMidiIdentity(priority, 0, 82) != 1)
        return fail("MIDI did not reuse the oldest physically held key last");

    HostInstance sameNote(factory, 1, 0);
    factory->midiMessage(sameNote.algorithm, 0x93, 60, 100);
    std::vector<float> oneVoiceBusses = makeBusses(frames);
    renderBlocks(sameNote, oneVoiceBusses, frames, 8);
    factory->midiMessage(sameNote.algorithm, 0x93, 60, 120);
    if (countMidiIdentity(sameNote, 3, 60) != 1 ||
        !sameNote.synth()->dtc->voices[0].pendingStart)
        return fail("same-note retrigger duplicated instead of reusing its voice");

    int transitionFrames = 0;
    while (sameNote.synth()->dtc->voices[0].pendingStart &&
           transitionFrames < 480) {
        std::fill(oneVoiceBusses.begin(), oneVoiceBusses.end(), 0.0f);
        sameNote.render(oneVoiceBusses, frames);
        transitionFrames += frames;
    }
    if (sameNote.synth()->dtc->voices[0].pendingStart ||
        transitionFrames > 480 ||
        sameNote.synth()->dtc->voices[0].envelope > 0.2f)
        return fail("same-note declick transition exceeded 10 ms or restarted hot");

    HostInstance oldNoteOff(factory, 1, 0);
    factory->midiMessage(oldNoteOff.algorithm, 0x90, 60, 100);
    std::vector<float> oldNoteBusses = makeBusses(frames);
    renderBlocks(oldNoteOff, oldNoteBusses, frames, 8);
    factory->midiMessage(oldNoteOff.algorithm, 0x90, 64, 100);
    factory->midiMessage(oldNoteOff.algorithm, 0x80, 60, 0);
    renderBlocks(oldNoteOff, oldNoteBusses, frames, 8);
    const Voice& replacement = oldNoteOff.synth()->dtc->voices[0];
    if (replacement.note != 64 || !replacement.gate ||
        !replacement.keyHeld)
        return fail("a stolen note's later Note Off affected its replacement");

    std::puts(
        "PASS: MIDI reuse order is deterministic; same-note and stolen-note "
        "handoffs are identity-safe and complete within 10 ms");
    return 0;
}

int testOutputAndRelease(const _NT_factory* factory) {
    const int frames = 64;
    HostInstance host(factory, 16, 0);
    for (uint8_t note = 48; note < 64; ++note)
        factory->midiMessage(host.algorithm, 0x90, note, 110);
    std::vector<float> busses = makeBusses(frames);
    float peak = 0.0f;
    for (int block = 0; block < 64; ++block) {
        std::fill(busses.begin(), busses.end(), 0.0f);
        host.render(busses, frames);
        if (!allFiniteAndBounded(busses, frames, &peak))
            return fail("16-voice render was non-finite or reached the +/-5 V ceiling");
    }
    if (peak < 0.1f)
        return fail("16-voice render was unexpectedly silent");

    host.setParameter(host.synth()->soundParameter(kSoundRelease), 0);
    for (uint8_t note = 48; note < 64; ++note)
        factory->midiMessage(host.algorithm, 0x80, note, 0);
    for (int block = 0; block < 100; ++block) {
        std::fill(busses.begin(), busses.end(), 0.0f);
        host.render(busses, frames);
    }
    if (countVoices(host, kVoiceMidi) != 0)
        return fail("released MIDI voices did not finish and become reusable");

    std::printf(
        "PASS: 16-voice rendering is finite and audible inside the +/-5 V "
        "ceiling (%.3f V peak), then fully releases\n",
        peak);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2 ||
        (argc == 2 && std::strcmp(argv[1], "--endurance") != 0))
        return fail("usage: icy_beauty_test [--endurance]");
    if (pluginEntry(kNT_selector_version, 0) != kNT_apiVersionCurrent)
        return fail("plugin reports the wrong disting NT API version");
    if (pluginEntry(kNT_selector_numFactories, 0) != 1)
        return fail("plugin must export exactly one synth factory");

    const _NT_factory* factory = reinterpret_cast<const _NT_factory*>(
        pluginEntry(kNT_selector_factoryInfo, 0));
    if (factory == NULL ||
        std::strcmp(factory->name, "Icy Beauty") != 0 ||
        factory->guid != NT_MULTICHAR('T', 'h', 'I', 'b') ||
        (factory->tags & kNT_tagInstrument) == 0)
        return fail("factory identity is incorrect");
    if (factory->calculateRequirements == NULL ||
        factory->construct == NULL ||
        factory->parameterChanged == NULL ||
        factory->step == NULL ||
        factory->midiRealtime == NULL ||
        factory->midiMessage == NULL)
        return fail("factory is missing a required callback");

    if (testFactoryAndSurface(factory) != 0)
        return 1;
    if (testCountBounds(factory) != 0)
        return 1;
    if (testDefaultMidiOwnership(factory) != 0)
        return 1;
    if (testGateGroups(factory) != 0)
        return 1;
    if (testSampleAndHold(factory) != 0)
        return 1;
    if (testCvRetriggerDeclick(factory) != 0)
        return 1;
    if (testPitchCvCalibration() != 0)
        return 1;
    if (testPitchOffsets(factory) != 0)
        return 1;
    if (testTargetPitchRoutingAndMidiStop(factory) != 0)
        return 1;
    if (testSixInputPairs(factory) != 0)
        return 1;
    if (testMidiReuse(factory) != 0)
        return 1;
    if (testOutputAndRelease(factory) != 0)
        return 1;

    std::puts("PASS: Icy Beauty flexible poly CV/gate contract");
    return 0;
}
