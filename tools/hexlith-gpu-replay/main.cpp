#include "adapters/filesystem/gpu_recording_files.h"
#include "adapters/filesystem/png_file.h"
#include "application/gpu_render.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>

// hexlith-gpu-replay draws what a GPU recording's screens show in chosen
// frames, with the reference renderer, and writes each screen as a PNG.
//
// A development tool, not part of the runtime. It reads the recording it is
// named and writes into the directory it is named, which belongs in the
// workspace: the pictures are the game's.
namespace {

namespace fs = std::filesystem;
namespace application = hexlith::application;
namespace gpu = hexlith::domain::gpu;

constexpr int Success = 0;
constexpr int Failure = 1;
constexpr int Usage = 2;

constexpr std::string_view UsageText =
    "usage: hexlith-gpu-replay [--sampling hardware|azahar] --frames <n,n,...> <recording> <directory>\n"
    "\n"
    "Replays the GPU recording in <recording> through the reference renderer, with its\n"
    "memory fills and display transfers, and writes what each screen shows in each of\n"
    "the frames as frame-<n>-top.png and frame-<n>-bottom.png in <directory>.\n"
    "\n"
    "Textures are sampled as their registers say, or with --sampling azahar the nearest\n"
    "texel of the largest level, as Azahar's software renderer does (ADR 0019).\n";

std::optional<std::set<std::uint64_t>> ParseFrames(std::string_view text) {
    std::set<std::uint64_t> frames;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view item = text.substr(0, comma);
        constexpr std::uint64_t Decimal = 10;
        std::uint64_t frame = 0;
        if (item.empty()) {
            return std::nullopt;
        }
        for (const char digit : item) {
            if (digit < '0' || digit > '9') {
                return std::nullopt;
            }
            frame = (frame * Decimal) + static_cast<std::uint64_t>(digit - '0');
        }
        frames.insert(frame);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    }
    return frames.empty() ? std::nullopt : std::optional{frames};
}

struct Arguments {
    gpu::Sampling sampling = gpu::Sampling::Hardware;
    std::set<std::uint64_t> frames;
    fs::path recording;
    fs::path output;
};

std::optional<Arguments> Parse(std::span<char* const> arguments) {
    Arguments parsed;
    std::vector<std::string_view> positional;
    for (std::size_t at = 1; at < arguments.size(); ++at) {
        const std::string_view argument{arguments[at]};
        const bool has_value = at + 1 < arguments.size();
        if (argument == "--frames" && has_value) {
            const auto frames = ParseFrames(arguments[++at]);
            if (!frames) {
                return std::nullopt;
            }
            parsed.frames = *frames;
        } else if (argument == "--sampling" && has_value) {
            const std::string_view sampling{arguments[++at]};
            if (sampling == "azahar") {
                parsed.sampling = gpu::Sampling::AzaharSoftware;
            } else if (sampling != "hardware") {
                return std::nullopt;
            }
        } else {
            positional.push_back(argument);
        }
    }
    if (positional.size() != 2 || parsed.frames.empty()) {
        return std::nullopt;
    }
    parsed.recording = positional[0];
    parsed.output = positional[1];
    return parsed;
}

int Main(std::span<char* const> arguments) {
    const std::optional<Arguments> parsed = Parse(arguments);
    if (!parsed) {
        std::cerr << UsageText;
        return Usage;
    }
    fs::create_directories(parsed->output);
    hexlith::adapters::filesystem::GpuRecordingFiles source{parsed->recording};
    const auto write = [&parsed](const application::RenderedFrame& frame) {
        for (const auto& [name, picture] :
             {std::pair{"top", &frame.top}, std::pair{"bottom", &frame.bottom}}) {
            const fs::path path =
                parsed->output / std::format("frame-{}-{}.png", frame.frame, name);
            const hexlith::core::Status written =
                hexlith::adapters::filesystem::WritePng(path, *picture);
            if (!written) {
                return written;
            }
            std::cout << path.string() << '\n';
        }
        return hexlith::core::Status{};
    };
    auto memory = std::make_unique<gpu::GpuMemory>();
    application::ReferenceGpuBackend backend(*memory, parsed->sampling);
    const hexlith::core::Result<application::GpuRenderSummary> rendered =
        application::RenderGpuRecording(source, *memory, backend, parsed->frames, write);
    if (!rendered) {
        std::cerr << "hexlith-gpu-replay: " << rendered.Failure().Message() << '\n';
        return Failure;
    }
    const application::GpuRenderSummary& summary = rendered.Value();
    const hexlith::ports::GpuBackendStatistics statistics = backend.Statistics();
    std::cout << std::format("{}: {} lists, {} draws, {} fills, {} transfers{}\n",
                             backend.Describe(),
                             statistics.lists,
                             statistics.draws,
                             summary.fills,
                             summary.transfers,
                             summary.unsupported || statistics.unsupported
                                 ? "; something asked for was not implemented"
                                 : "");
    return Success;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return Main(std::span<char* const>{argv, static_cast<std::size_t>(argc)});
    } catch (const std::exception& thrown) {
        std::cerr << "hexlith-gpu-replay: " << thrown.what() << '\n';
        return Failure;
    }
}
