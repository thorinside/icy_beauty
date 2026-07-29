#include <distingnt/api.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

const int kSampleRate = 48000;
const int kFramesPerBlock = 64;
const int kOutputBus = 12;

std::vector<std::max_align_t> allocateAligned(std::size_t byteCount) {
    const std::size_t count =
        (byteCount + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
    return std::vector<std::max_align_t>(count == 0 ? 1 : count);
}

struct HostInstance {
    explicit HostInstance(const _NT_factory* instanceFactory)
        : factory(instanceFactory), requirements(), algorithm(NULL) {
        specification[0] = 4;
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

void writeUint16(std::FILE* output, uint16_t value) {
    const uint8_t bytes[2] = {
        static_cast<uint8_t>(value & 0xffU),
        static_cast<uint8_t>((value >> 8U) & 0xffU),
    };
    std::fwrite(bytes, sizeof(bytes), 1, output);
}

void writeUint32(std::FILE* output, uint32_t value) {
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(value & 0xffU),
        static_cast<uint8_t>((value >> 8U) & 0xffU),
        static_cast<uint8_t>((value >> 16U) & 0xffU),
        static_cast<uint8_t>((value >> 24U) & 0xffU),
    };
    std::fwrite(bytes, sizeof(bytes), 1, output);
}

bool writePcm16Wav(const char* path, const std::vector<float>& samples) {
    std::FILE* output = std::fopen(path, "wb");
    if (output == NULL)
        return false;

    const uint32_t dataBytes =
        static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    std::fwrite("RIFF", 4, 1, output);
    writeUint32(output, 36U + dataBytes);
    std::fwrite("WAVE", 4, 1, output);
    std::fwrite("fmt ", 4, 1, output);
    writeUint32(output, 16U);
    writeUint16(output, 1U);
    writeUint16(output, 1U);
    writeUint32(output, kSampleRate);
    writeUint32(output, kSampleRate * sizeof(int16_t));
    writeUint16(output, sizeof(int16_t));
    writeUint16(output, 16U);
    std::fwrite("data", 4, 1, output);
    writeUint32(output, dataBytes);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const float clamped =
            std::max(-1.0f, std::min(0.999969f, samples[index]));
        const int16_t quantized =
            static_cast<int16_t>(std::lround(clamped * 32768.0f));
        writeUint16(output, static_cast<uint16_t>(quantized));
    }
    const bool success = std::fclose(output) == 0;
    return success;
}

bool parseControl(const char* text, int16_t& value) {
    char* end = NULL;
    const long parsed = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || parsed < 0 || parsed > 100)
        return false;
    value = static_cast<int16_t>(parsed);
    return true;
}

int renderPhrase(const _NT_factory* factory, const char* outputPath,
                 const int16_t controls[kNumSoundParameters]) {
    HostInstance host(factory);
    if (host.algorithm == NULL)
        return 1;
    host.values[kParamOutputMode] = 1;
    for (int control = 0; control < kNumSoundParameters; ++control) {
        host.values[host.synth()->soundParameter(
            static_cast<SoundParameter>(control))] = controls[control];
    }

    // The block-aligned schedule reproduces the root phrase inferred from the
    // exposed attacks in model/cononical.wav. Each gate is 500 ms so that the
    // initial spectrum and the release can both be compared reproducibly.
    static const uint32_t kNoteOnBlocks[] = {
        375U,   // 0.500 s
        2171U,  // +2.395 s
        3079U,  // +1.211 s
        4871U,  // +2.389 s
    };
    static const uint8_t kNotes[] = {50, 54, 47, 49};
    const uint32_t gateBlocks = 375U;
    const uint32_t totalBlocks = 6375U;  // 8.5 seconds

    std::vector<float> audio;
    audio.reserve(totalBlocks * kFramesPerBlock);
    std::vector<float> busses(kNT_lastBus * kFramesPerBlock, 0.0f);
    for (uint32_t block = 0; block < totalBlocks; ++block) {
        for (std::size_t event = 0;
             event < sizeof(kNotes) / sizeof(kNotes[0]); ++event) {
            if (block == kNoteOnBlocks[event])
                factory->midiMessage(host.algorithm, 0x90, kNotes[event], 100);
            if (block == kNoteOnBlocks[event] + gateBlocks)
                factory->midiMessage(host.algorithm, 0x80, kNotes[event], 0);
        }
        std::fill(busses.begin(), busses.end(), 0.0f);
        factory->step(host.algorithm, busses.data(), kFramesPerBlock / 4);
        const float* output =
            busses.data() + kOutputBus * kFramesPerBlock;
        for (int frame = 0; frame < kFramesPerBlock; ++frame) {
            if (!std::isfinite(output[frame]))
                return 1;
            audio.push_back(output[frame]);
        }
    }

    if (!writePcm16Wav(outputPath, audio))
        return 1;
    std::printf(
        "PASS: rendered D3 -> F#3 -> B2 -> C#3 to %s "
        "(Tone %d, Motion %d, Grain %d, Resonance %d, Release %d)\n",
        outputPath, controls[0], controls[1], controls[2], controls[3],
        controls[4]);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 7) {
        std::fprintf(
            stderr,
            "usage: render_reference_phrase OUTPUT.wav "
            "[TONE MOTION GRAIN RESONANCE RELEASE]\n");
        return 2;
    }
    int16_t controls[kNumSoundParameters] = {
        kSoundParameters[kSoundTone].def,
        kSoundParameters[kSoundMotion].def,
        kSoundParameters[kSoundGrain].def,
        kSoundParameters[kSoundResonance].def,
        kSoundParameters[kSoundRelease].def,
    };
    if (argc == 7) {
        for (int control = 0; control < kNumSoundParameters; ++control) {
            if (!parseControl(argv[control + 2], controls[control])) {
                std::fprintf(stderr, "sound controls must be integers 0..100\n");
                return 2;
            }
        }
    }

    if (pluginEntry(kNT_selector_version, 0) != kNT_apiVersionCurrent ||
        pluginEntry(kNT_selector_numFactories, 0) != 1) {
        std::fprintf(stderr, "plugin contract is unavailable\n");
        return 1;
    }
    const _NT_factory* factory = reinterpret_cast<const _NT_factory*>(
        pluginEntry(kNT_selector_factoryInfo, 0));
    if (factory == NULL) {
        std::fprintf(stderr, "plugin factory is unavailable\n");
        return 1;
    }
    return renderPhrase(factory, argv[1], controls);
}
