#include "adapters/filesystem/gpu_recording_files.h"
#include "application/gpu_replay.h"
#include "domain/gpu/command_processor.h"
#include "domain/gpu/gpu_memory.h"
#include "domain/gpu/pica_state.h"
#include "domain/gpu/shader_interpreter.h"
#include "domain/video/yuv_conversion.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <set>
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
    "       hexlith-oracle-run --shader-cases-from <recording> <directory>\n"
    "       hexlith-oracle-run --precision <directory>\n"
    "\n"
    "Runs every case file the directory holds and writes its results there, named\n"
    "as the Azahar oracle names them with .hexlith before the extension:\n"
    "\n"
    "  y2r-cases.bin   ->  y2r-results.hexlith.bin\n"
    "  pica-cases.bin  ->  pica-results.hexlith.bin\n"
    "  shader-cases.bin  ->  shader-results.hexlith.bin\n"
    "\n"
    "The case formats are described in third_party/azahar/README.md.\n"
    "\n"
    "With --shader-cases-from, it instead writes shader-cases.bin into the directory\n"
    "from a GPU recording: one case for each distinct state a draw ran a vertex or\n"
    "geometry shader in, with the program, descriptors and uniforms the title gave it\n"
    "and inputs drawn at random.\n"
    "\n"
    "With --precision, it runs the vertex shader cases of shader-cases.bin in float32\n"
    "and again rounded to float24 (ADR 0018), and prints how far o0, the position in\n"
    "every program of the reference title, moves on a screen 400 pixels wide.\n";

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

template<std::size_t N>
void TakeVectors(std::istream& in, std::array<gpu::Vec4, N>& vectors) {
    for (gpu::Vec4& vector : vectors) {
        for (float& component : vector) {
            component = std::bit_cast<float>(Take<std::uint32_t>(in));
        }
    }
}

template<std::size_t N>
void PutVectors(std::ostream& out, const std::array<gpu::Vec4, N>& vectors) {
    for (const gpu::Vec4& vector : vectors) {
        for (const float component : vector) {
            Put(out, std::bit_cast<std::uint32_t>(component));
        }
    }
}

// What a shader case says beyond its unit and registers.
struct ShaderCase {
    bool geometry = false;
    std::uint32_t entry = 0;
    std::uint32_t output_mask = 0;
};

// A case's header and unit, as third_party/azahar/README.md lays them out.
ShaderCase ReadShaderUnit(std::istream& in, gpu::ShaderUnitState& unit) {
    ShaderCase header;
    header.geometry = Take<std::uint32_t>(in) != 0;
    header.entry = Take<std::uint32_t>(in);
    header.output_mask = Take<std::uint32_t>(in);
    for (std::uint32_t& word : unit.code) {
        word = Take<std::uint32_t>(in);
    }
    for (std::uint32_t& word : unit.descriptors) {
        word = Take<std::uint32_t>(in);
    }
    TakeVectors(in, unit.float_uniforms);
    unit.bool_uniforms = static_cast<std::uint16_t>(Take<std::uint32_t>(in));
    for (std::uint32_t& word : unit.int_uniforms) {
        word = Take<std::uint32_t>(in);
    }
    return header;
}

void ReadRegisters(std::istream& in, gpu::ShaderRegisters& registers) {
    TakeVectors(in, registers.inputs);
    TakeVectors(in, registers.temporaries);
    TakeVectors(in, registers.outputs[0]);
    TakeVectors(in, registers.outputs[1]);
    registers.output_bank = Take<std::uint32_t>(in) != 0 ? 1 : 0;
    for (std::int32_t& address : registers.address) {
        address = static_cast<std::int32_t>(Take<std::uint32_t>(in));
    }
    for (auto&& condition : registers.conditions) {
        condition = Take<std::uint32_t>(in) != 0;
    }
}

void WriteRegisters(std::ostream& out, const gpu::ShaderRegisters& registers) {
    PutVectors(out, registers.inputs);
    PutVectors(out, registers.temporaries);
    PutVectors(out, registers.outputs[0]);
    PutVectors(out, registers.outputs[1]);
    Put(out, static_cast<std::uint32_t>(registers.output_bank));
    for (const std::int32_t address : registers.address) {
        Put(out, static_cast<std::uint32_t>(address));
    }
    for (const bool condition : registers.conditions) {
        Put(out, static_cast<std::uint32_t>(condition));
    }
}

void WritePrimitives(std::ostream& out, const gpu::GeometryEmitter& emitter) {
    Put(out, static_cast<std::uint32_t>(emitter.Primitives().size()));
    for (const gpu::GeometryEmitter::Primitive& primitive : emitter.Primitives()) {
        Put(out, static_cast<std::uint32_t>(primitive.reversed));
        for (const gpu::ShaderRegisters::Bank& vertex : primitive.vertices) {
            PutVectors(out, vertex);
        }
    }
}

// shader-cases.bin and its results.
bool RunShaders(const fs::path& directory) {
    std::ifstream in(directory / "shader-cases.bin", std::ios::binary);
    if (!in) {
        return true;
    }
    std::ofstream out(directory / "shader-results.hexlith.bin", std::ios::binary);
    auto unit = std::make_unique<gpu::ShaderUnitState>();
    const auto cases = Take<std::uint32_t>(in);
    for (std::uint32_t index = 0; index < cases && in; ++index) {
        const ShaderCase header = ReadShaderUnit(in, *unit);
        gpu::ShaderRegisters registers;
        ReadRegisters(in, registers);
        gpu::GeometryEmitter emitter{header.output_mask};
        static_cast<void>(gpu::RunShader(
            *unit,
            registers,
            {.entry = header.entry, .emitter = header.geometry ? &emitter : nullptr}));
        WriteRegisters(out, registers);
        WritePrimitives(out, emitter);
    }
    return static_cast<bool>(in) && static_cast<bool>(out);
}

// Writes a case for each distinct shader state the recording's draws run.
class ShaderCaseWriter {
public:
    explicit ShaderCaseWriter(std::ostream& out) : out_(&out) {
        Put(*out_, std::uint32_t{0});
    }

    void Draw(const gpu::PicaState& state, bool /*indexed*/) {
        Take(state);
    }

    void ImmediateVertex(const gpu::PicaState& state) {
        Take(state);
    }

    void RestartPrimitive(const gpu::PicaState& /*state*/) {}

    void Finalize() {}

    // Puts the count of cases in front of them.
    void Close() {
        out_->seekp(0);
        Put(*out_, cases_);
    }

    [[nodiscard]] std::uint32_t Cases() const {
        return cases_;
    }

private:
    // GPUREG_xSH_ENTRYPOINT's offset, bits 0 to 15; GPUREG_GEOSTAGE_CONFIG
    // says a geometry shader is in use with 2 in bits 0 and 1.
    static constexpr std::uint16_t EntryOffset = gpu::reg::shader::EntryPoint;
    static constexpr std::uint32_t LowHalf = 0xFFFF;
    static constexpr std::uint32_t GeometryInUse = 2;
    static constexpr std::uint32_t GeometryUseMask = 3;

    void Take(const gpu::PicaState& state) {
        Write(state.VertexShader(),
              false,
              state.Register(gpu::reg::VertexShaderBase + EntryOffset) & LowHalf,
              0);
        if ((state.Register(gpu::reg::GeometryStageConfig) & GeometryUseMask) == GeometryInUse) {
            Write(state.GeometryShader(),
                  true,
                  state.Register(gpu::reg::GeometryShaderBase + EntryOffset) & LowHalf,
                  state.Register(gpu::reg::GeometryShaderBase + gpu::reg::shader::OutputMask) &
                      LowHalf);
        }
    }

    void Write(const gpu::ShaderUnitState& unit,
               bool geometry,
               std::uint32_t entry,
               std::uint32_t mask) {
        std::uint64_t key = HashWords(unit.code, HashWords(unit.descriptors));
        key = HashVectors(unit.float_uniforms) ^ (key * HashPrime);
        key = HashWords(unit.int_uniforms, key) ^ unit.bool_uniforms ^
              (std::uint64_t{entry} << 32U) ^ (geometry ? 1U : 0U);
        if (!seen_.insert(key).second) {
            return;
        }
        ++cases_;
        Put(*out_, static_cast<std::uint32_t>(geometry));
        Put(*out_, entry);
        Put(*out_, mask);
        for (const std::uint32_t word : unit.code) {
            Put(*out_, word);
        }
        for (const std::uint32_t word : unit.descriptors) {
            Put(*out_, word);
        }
        PutVectors(*out_, unit.float_uniforms);
        Put(*out_, static_cast<std::uint32_t>(unit.bool_uniforms));
        for (const std::uint32_t word : unit.int_uniforms) {
            Put(*out_, word);
        }
        // Inputs at random, everything else as a run starts it: zero.
        std::array<gpu::Vec4, gpu::ShaderRegisters::Count> inputs{};
        constexpr float Spread = 2.0F;
        std::uniform_real_distribution<float> spread{-Spread, Spread};
        for (gpu::Vec4& input : inputs) {
            for (float& component : input) {
                component = spread(random_);
            }
        }
        PutVectors(*out_, inputs);
        constexpr std::size_t ZeroWords = (3 * gpu::ShaderRegisters::Count * 4) + 1 + 3 + 2;
        for (std::size_t word = 0; word < ZeroWords; ++word) {
            Put(*out_, std::uint32_t{0});
        }
    }

    std::ostream* out_;
    std::uint32_t cases_ = 0;
    std::set<std::uint64_t> seen_;
    // A fixed seed, so that the same recording always gives the same cases.
    static constexpr std::uint32_t Seed = 1;
    std::mt19937 random_{Seed};  // NOLINT(bugprone-random-generator-seed): repeatable on purpose
};

int WriteShaderCases(const fs::path& recording, const fs::path& directory) {
    fs::create_directories(directory);
    std::ofstream out(directory / "shader-cases.bin", std::ios::binary);
    hexlith::adapters::filesystem::GpuRecordingFiles source{recording};
    auto memory = std::make_unique<gpu::GpuMemory>();
    gpu::PicaState state;
    ShaderCaseWriter writer{out};
    gpu::CommandProcessor processor{state, *memory, writer};
    hexlith::application::GpuReplayVisitor visitor;
    visitor.list = [&processor](const gpu::RecordedList& /*list*/,
                                std::span<const std::uint8_t> bytes) {
        const hexlith::core::Result<gpu::CommandListRun> run = processor.Run(bytes);
        return run ? hexlith::core::Status{} : hexlith::core::Status{run.Failure()};
    };
    const hexlith::core::Status replayed =
        hexlith::application::ReplayGpuRecording(source, *memory, visitor);
    if (!replayed) {
        std::cerr << "hexlith-oracle-run: " << replayed.Failure().Message() << '\n';
        return Failure;
    }
    writer.Close();
    std::cout << writer.Cases() << " shader cases\n";
    return out ? Success : Failure;
}

// How far float24 arithmetic would move each vertex shader case's position.
int MeasurePrecision(const fs::path& directory) {
    std::ifstream in(directory / "shader-cases.bin", std::ios::binary);
    if (!in) {
        std::cerr << "hexlith-oracle-run: no shader-cases.bin\n";
        return Failure;
    }
    // Half the width of the widest screen, in pixels: a unit of normalised
    // device coordinates spans that many.
    constexpr double HalfWidth = 200.0;
    auto unit = std::make_unique<gpu::ShaderUnitState>();
    std::vector<double> moves;
    const auto cases = Take<std::uint32_t>(in);
    for (std::uint32_t index = 0; index < cases && in; ++index) {
        const ShaderCase header = ReadShaderUnit(in, *unit);
        gpu::ShaderRegisters start;
        ReadRegisters(in, start);
        const bool geometry = header.geometry;
        const std::uint32_t entry = header.entry;
        if (geometry) {
            continue;
        }
        std::array<gpu::Vec4, 2> positions{};
        for (const gpu::ShaderPrecision precision :
             {gpu::ShaderPrecision::Float32, gpu::ShaderPrecision::Float24}) {
            gpu::ShaderRegisters registers = start;
            static_cast<void>(
                gpu::RunShader(*unit, registers, {.entry = entry, .precision = precision}));
            positions.at(precision == gpu::ShaderPrecision::Float32 ? 0 : 1) =
                registers.outputs[0][0];
        }
        const auto ndc = [](const gpu::Vec4& position, std::size_t axis) {
            return static_cast<double>(position.at(axis)) / static_cast<double>(position.at(3));
        };
        // Only a vertex in front of the eye and inside the view lands on the
        // screen.
        if (!(positions[0][3] > 0.0F) || std::abs(ndc(positions[0], 0)) > 1.0 ||
            std::abs(ndc(positions[0], 1)) > 1.0) {
            continue;
        }
        const double move = std::max(std::abs(ndc(positions[0], 0) - ndc(positions[1], 0)),
                                     std::abs(ndc(positions[0], 1) - ndc(positions[1], 1))) *
                            HalfWidth;
        if (std::isfinite(move)) {
            moves.push_back(move);
        }
    }
    std::ranges::sort(moves);
    if (moves.empty()) {
        std::cout << "no vertex landed on the screen\n";
        return Success;
    }
    constexpr double Median = 0.5;
    constexpr double Rare = 0.999;
    const auto at = [&moves](double fraction) {
        return moves.at(static_cast<std::size_t>(fraction * static_cast<double>(moves.size() - 1)));
    };
    std::cout << std::format(
        "{} vertices on the screen; float24 moves them by a median of {:.6f} pixels, "
        "{:.6f} at the 99.9th percentile, {:.6f} at most\n",
        moves.size(),
        at(Median),
        at(Rare),
        moves.back());
    return Success;
}

int Main(std::span<char* const> arguments) {
    if (arguments.size() == 3 && std::string_view{arguments[1]} == "--precision") {
        return MeasurePrecision(fs::path{arguments[2]});
    }
    if (arguments.size() == 4 && std::string_view{arguments[1]} == "--shader-cases-from") {
        return WriteShaderCases(fs::path{arguments[2]}, fs::path{arguments[3]});
    }
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
    if (!RunShaders(directory)) {
        std::cerr << "hexlith-oracle-run: the shader cases could not be read or answered\n";
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
