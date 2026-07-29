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

}  // namespace

int main() {
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

    _NT_algorithmRequirements requirements = {};
    factory->calculateRequirements(requirements, NULL);
    if (requirements.numParameters != 3 || requirements.sram == 0 ||
        requirements.dtc == 0)
        return fail("plugin reports invalid instance requirements");

    std::vector<std::max_align_t> sram = allocateAligned(requirements.sram);
    std::vector<std::max_align_t> dtc = allocateAligned(requirements.dtc);
    _NT_algorithmMemoryPtrs memory = {
        reinterpret_cast<uint8_t*>(sram.data()),
        NULL,
        reinterpret_cast<uint8_t*>(dtc.data()),
        NULL,
    };
    _NT_algorithm* algorithm =
        factory->construct(memory, requirements, NULL);
    if (algorithm == NULL || algorithm->parameters == NULL ||
        algorithm->parameterPages == NULL)
        return fail("plugin instance did not expose its host parameter surface");

    std::vector<int16_t> values(requirements.numParameters);
    for (uint32_t parameter = 0; parameter < requirements.numParameters;
         ++parameter) {
        values[parameter] = algorithm->parameters[parameter].def;
    }
    algorithm->v = values.data();
    algorithm->vIncludingCommon = values.data();

    if (values[0] != 13)
        return fail("fresh-load audio is not routed to Output 1");
    if (values[2] != 0)
        return fail("fresh-load MIDI selection is not Omni");

    const int frames = 64;
    std::vector<float> busses(kNT_lastBus * frames, 0.0f);
    factory->midiMessage(algorithm, 0x99, 69, 112);

    float soundingPeak = 0.0f;
    for (int block = 0; block < 32; ++block) {
        std::fill(busses.begin(), busses.end(), 0.0f);
        factory->step(algorithm, busses.data(), frames / 4);
        const float* output1 = busses.data() + 12 * frames;
        for (int frame = 0; frame < frames; ++frame) {
            if (!std::isfinite(output1[frame]))
                return fail("note-on rendering produced a non-finite sample");
            soundingPeak = std::max(soundingPeak, std::fabs(output1[frame]));
        }
    }
    if (soundingPeak < 0.1f)
        return fail("Omni MIDI note-on did not render audible synth output");

    float* output1 = busses.data() + 12 * frames;
    std::fill(output1, output1 + frames, 20.0f);
    factory->step(algorithm, busses.data(), frames / 4);
    if (*std::min_element(output1, output1 + frames) < 15.0f)
        return fail("add output mode did not preserve existing bus audio");

    values[1] = 1;
    std::fill(output1, output1 + frames, 20.0f);
    factory->step(algorithm, busses.data(), frames / 4);
    if (*std::max_element(output1, output1 + frames) > 5.0f)
        return fail("replace output mode did not replace existing bus audio");

    factory->midiMessage(algorithm, 0x89, 69, 0);
    float releasePeak = 0.0f;
    for (int block = 0; block < 800; ++block) {
        std::fill(busses.begin(), busses.end(), 0.0f);
        factory->step(algorithm, busses.data(), frames / 4);
        if (block == 799) {
            const float* output1 = busses.data() + 12 * frames;
            for (int frame = 0; frame < frames; ++frame)
                releasePeak = std::max(releasePeak, std::fabs(output1[frame]));
        }
    }
    if (releasePeak > 0.01f)
        return fail("MIDI note-off did not release the synth voice");

    std::puts("PASS: Icy Beauty exports a disting NT instrument and renders MIDI audio");
    return 0;
}
