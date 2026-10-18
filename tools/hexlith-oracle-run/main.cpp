#include "domain/video/yuv_conversion.h"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
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
    "  y2r-cases.bin  ->  y2r-results.hexlith.bin\n"
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
