#include "domain/gpu/command_processor.h"
#include "domain/gpu/gpu_memory.h"
#include "domain/gpu/pica_state.h"
#include "domain/video/yuv_conversion.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// hexlith-oracle-run runs a directory of cases through Hexlith's own code and
// writes the results beside them, the way the oracle hooks of
// third_party/azahar/oracle.patch run the same cases through Azahar's. The
// comparison scripts in scripts/ generate the cases, run both, and compare.
//
// A development tool, not part of the runtime. It never looks for the
// workspace.
namespace {

namespace fs = std::filesystem;
namespace gpu = hexlith::domain::gpu;
namespace video = hexlith::domain::video;

constexpr int Success = 0;
constexpr int Failure = 1;
constexpr int Usage = 2;

constexpr std::string_view UsageText =
    "usage: hexlith-oracle-run <directory>\n"
    "\n"
    "Runs every case file the directory holds and writes its results there, named\n"
    "as the Azahar oracle names them with .hexlith before the extension:\n"
    "\n"
    "  y2r-cases.bin   ->  y2r-results.hexlith.bin\n"
    "  pica-cases.bin  ->  pica-results.hexlith.bin\n"
    "\n"
    "The case formats are described in third_party/azahar/README.md.\n";

template<typename T>
T Take(std::istream& in) {
    T value{};
    in.read(reinterpret_cast<char*>(&value), sizeof(value));
    return value;
}

std::vector<std::uint8_t> TakeBytes(std::istream& in) {
    std::vector<std::uint8_t> bytes(Take<std::uint32_t>(in));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

// The samples of a plane as the conversion takes them: the low byte of each
// halfword in a wide format.
std::vector<std::uint8_t> Samples(const std::vector<std::uint8_t>& bytes, std::size_t stride) {
    std::vector<std::uint8_t> samples;
    samples.reserve(bytes.size() / stride);
    for (std::size_t at = 0; at < bytes.size(); at += stride) {
        samples.push_back(bytes[at]);
    }
    return samples;
}

bool RunYuvToRgb(const fs::path& directory) {
    std::ifstream in(directory / "y2r-cases.bin", std::ios::binary);
    if (!in) {
        return true;
    }
    std::ofstream out(directory / "y2r-results.hexlith.bin", std::ios::binary);
    const auto count = Take<std::uint32_t>(in);
    for (std::uint32_t index = 0; index < count && in; ++index) {
        video::YuvConversion conversion;
        conversion.input = static_cast<video::YuvFormat>(Take<std::uint8_t>(in));
        conversion.output = static_cast<video::RgbFormat>(Take<std::uint8_t>(in));
        conversion.rotation = static_cast<video::Rotation>(Take<std::uint8_t>(in));
        conversion.layout = static_cast<video::OutputLayout>(Take<std::uint8_t>(in));
        conversion.width = Take<std::uint16_t>(in);
        conversion.lines = Take<std::uint16_t>(in);
        for (std::int16_t& coefficient : conversion.coefficients) {
            coefficient = Take<std::int16_t>(in);
        }
        conversion.alpha = Take<std::uint8_t>(in);
        static_cast<void>(Take<std::uint8_t>(in));
        static_cast<void>(Take<std::uint16_t>(in));

        const std::size_t stride = video::BytesPerSample(conversion.input);
        const std::vector<std::uint8_t> y = Samples(TakeBytes(in), stride);
        const std::vector<std::uint8_t> u = Samples(TakeBytes(in), stride);
        const std::vector<std::uint8_t> v = Samples(TakeBytes(in), stride);
        const std::vector<std::uint8_t> interleaved = TakeBytes(in);
        const std::vector<std::uint8_t> result = video::ConvertYuvToRgb(
            conversion, video::YuvPlanes{.y = y, .u = u, .v = v, .interleaved = interleaved});

        const auto length = static_cast<std::uint32_t>(result.size());
        out.write(reinterpret_cast<const char*>(&length), sizeof(length));
        out.write(reinterpret_cast<const char*>(result.data()),
                  static_cast<std::streamsize>(result.size()));
    }
    return static_cast<bool>(in) && static_cast<bool>(out);
}

// FNV-1a over bytes, as the Azahar oracle hashes the same state.
constexpr std::uint64_t HashStart = 0xCBF29CE484222325ULL;
constexpr std::uint64_t HashPrime = 0x100000001B3ULL;

std::uint64_t Hash(std::span<const std::uint8_t> bytes, std::uint64_t hash = HashStart) {
    for (const std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= HashPrime;
    }
    return hash;
}

template<typename T>
std::uint64_t HashObject(const T& value, std::uint64_t hash = HashStart) {
    return Hash({reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)}, hash);
}

// Azahar keeps 0300h internal registers; the comparison covers those.
constexpr std::size_t ComparedRegisters = 0x300;

std::uint64_t HashRegisters(const gpu::PicaState& state) {
    return Hash({reinterpret_cast<const std::uint8_t*>(state.Registers().data()),
                 ComparedRegisters * sizeof(std::uint32_t)});
}

template<std::size_t N>
std::uint64_t HashVectors(const std::array<gpu::Vec4, N>& vectors) {
    std::uint64_t hash = HashStart;
    for (const gpu::Vec4& vector : vectors) {
        hash = HashObject(vector, hash);
    }
    return hash;
}

template<std::size_t N>
std::uint64_t HashWords(const std::array<std::uint32_t, N>& words, std::uint64_t hash = HashStart) {
    return Hash({reinterpret_cast<const std::uint8_t*>(words.data()), N * sizeof(std::uint32_t)},
                hash);
}

// The sixteen section hashes, in the order docs/pica-commands.md lists.
std::vector<std::uint64_t> HashState(const gpu::PicaState& state) {
    std::vector<std::uint64_t> hashes{HashRegisters(state)};
    for (const gpu::ShaderUnitState* unit : {&state.VertexShader(), &state.GeometryShader()}) {
        hashes.push_back(HashWords(unit->code));
        hashes.push_back(HashWords(unit->descriptors));
        hashes.push_back(HashVectors(unit->float_uniforms));
        hashes.push_back(HashObject(static_cast<std::uint32_t>(unit->bool_uniforms)));
        hashes.push_back(HashWords(unit->int_uniforms));
    }
    hashes.push_back(HashVectors(state.FixedAttributeValues()));
    std::uint64_t lighting = HashStart;
    for (const auto& table : state.LightingLutData()) {
        lighting = HashWords(table, lighting);
    }
    hashes.push_back(lighting);
    hashes.push_back(HashWords(state.FogLut()));
    // Noise, colour map and alpha map have 128 entries, colour and colour
    // difference 256.
    std::uint64_t proctex = HashStart;
    for (const auto& [table, entries] : {std::pair{0, 128},
                                         std::pair{2, 128},
                                         std::pair{3, 128},
                                         std::pair{4, 256},
                                         std::pair{5, 256}}) {
        const auto& words = state.ProcTexLuts().at(static_cast<std::size_t>(table));
        proctex = Hash({reinterpret_cast<const std::uint8_t*>(words.data()),
                        static_cast<std::size_t>(entries) * sizeof(std::uint32_t)},
                       proctex);
    }
    hashes.push_back(proctex);
    hashes.push_back(HashVectors(state.ImmediateVertex()));
    return hashes;
}

// The registers at an event, with the data ports of the FIFOs left out as the
// Azahar oracle leaves them out: Azahar stores the last of several values
// written to one before it handles the first.
std::uint64_t HashEventRegisters(const gpu::PicaState& state) {
    std::array<std::uint32_t, ComparedRegisters> copy{};
    std::copy_n(state.Registers().begin(), ComparedRegisters, copy.begin());
    static constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 10> Ports = {
        {{0x0B0, 0x0B8},
         {0x0E8, 0x0F0},
         {0x1C8, 0x1D0},
         {0x233, 0x236},
         {0x291, 0x299},
         {0x29C, 0x2A4},
         {0x2A6, 0x2AE},
         {0x2C1, 0x2C9},
         {0x2CC, 0x2D4},
         {0x2D6, 0x2DE}}};
    for (const auto& [first, end] : Ports) {
        for (std::uint32_t id = first; id < end; ++id) {
            copy.at(id) = 0;
        }
    }
    return HashWords(copy);
}

// The kinds of event, as the Azahar oracle's patch numbers them.
enum class EventKind : std::uint8_t {
    DrawArrays = 1,
    DrawElements = 2,
    ImmediateVertex = 3,
    PrimitiveRestart = 4,
    Finalize = 5,
};

// Records what a list asks for, as the Azahar oracle's hooks do.
struct OracleExecutor {
    struct Event {
        EventKind kind = EventKind::DrawArrays;
        std::uint64_t registers = 0;
        std::uint64_t extra = 0;
    };

    void Draw(const gpu::PicaState& state, bool indexed) {
        events.push_back({.kind = indexed ? EventKind::DrawElements : EventKind::DrawArrays,
                          .registers = HashEventRegisters(state),
                          .extra = 0});
    }

    void ImmediateVertex(const gpu::PicaState& state) {
        events.push_back({.kind = EventKind::ImmediateVertex,
                          .registers = HashEventRegisters(state),
                          .extra = HashVectors(state.ImmediateVertex())});
    }

    void RestartPrimitive(const gpu::PicaState& state) {
        events.push_back({.kind = EventKind::PrimitiveRestart,
                          .registers = HashEventRegisters(state),
                          .extra = 0});
    }

    void Finalize() {
        events.push_back(
            {.kind = EventKind::Finalize, .registers = HashEventRegisters(*watched), .extra = 0});
    }

    // The state Finalize reports, which is handed no state of its own.
    const gpu::PicaState* watched = nullptr;
    std::vector<Event> events;
};

template<typename T>
void Put(std::ostream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

// The interrupt setup the Azahar oracle leaves in its registers before each
// case: GPUREG_IRQ_CMP(0), IRQ_MASK and IRQ_AUTOSTOP as Nintendo's code
// sets them.
void SetUpInterrupts(gpu::PicaState& state) {
    constexpr std::uint32_t AllBytes = 0xF;
    static_cast<void>(
        state.Write(gpu::reg::IrqCompare, gpu::InterruptSetup::NintendoCompare, AllBytes));
    static_cast<void>(state.Write(gpu::reg::IrqMask, gpu::InterruptSetup::NintendoMask, AllBytes));
    static_cast<void>(state.Write(gpu::reg::IrqAutostop, 1, AllBytes));
}

bool RunPica(const fs::path& directory) {
    std::ifstream in(directory / "pica-cases.bin", std::ios::binary);
    if (!in) {
        return true;
    }
    std::ofstream out(directory / "pica-results.hexlith.bin", std::ios::binary);
    auto memory = std::make_unique<gpu::GpuMemory>();
    const auto cases = Take<std::uint32_t>(in);
    for (std::uint32_t index = 0; index < cases && in; ++index) {
        gpu::PicaState state;
        SetUpInterrupts(state);
        OracleExecutor executor;
        executor.watched = &state;
        gpu::CommandProcessor processor{state, *memory, executor};
        const auto steps = Take<std::uint32_t>(in);
        for (std::uint32_t step = 0; step < steps && in; ++step) {
            const auto blocks = Take<std::uint32_t>(in);
            for (std::uint32_t block = 0; block < blocks; ++block) {
                const auto address = Take<std::uint32_t>(in);
                static_cast<void>(memory->Write(address, TakeBytes(in)));
            }
            executor.events.clear();
            static_cast<void>(processor.Run(TakeBytes(in)));
            Put(out, static_cast<std::uint32_t>(executor.events.size()));
            for (const OracleExecutor::Event& event : executor.events) {
                Put(out, static_cast<std::uint8_t>(event.kind));
                Put(out, event.registers);
                Put(out, event.extra);
            }
            for (const std::uint64_t hash : HashState(state)) {
                Put(out, hash);
            }
            out.write(reinterpret_cast<const char*>(state.Registers().data()),
                      static_cast<std::streamsize>(ComparedRegisters * sizeof(std::uint32_t)));
        }
    }
    return static_cast<bool>(in) && static_cast<bool>(out);
}

int Main(std::span<char* const> arguments) {
    if (arguments.size() != 2 || std::string_view{arguments[1]} == "--help") {
        std::cerr << UsageText;
        return Usage;
    }
    const fs::path directory{arguments[1]};
    if (!RunYuvToRgb(directory)) {
        std::cerr << "hexlith-oracle-run: the Y2R cases could not be read or answered\n";
        return Failure;
    }
    if (!RunPica(directory)) {
        std::cerr << "hexlith-oracle-run: the PICA cases could not be read or answered\n";
        return Failure;
    }
    return Success;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return Main(std::span<char* const>{argv, static_cast<std::size_t>(argc)});
    } catch (const std::exception& thrown) {
        std::cerr << "hexlith-oracle-run: " << thrown.what() << '\n';
        return Failure;
    }
}
