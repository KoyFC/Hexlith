#include "domain/linker/cro_linker.h"

#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// hexlith-cro-link links CRO modules at addresses given on the command line and
// prints every segment and every linked word, one record per line.
//
// It exists to be compared with memory. scripts/compare_cro_link.py takes the
// addresses from an Azahar log, runs this, and checks the records against a dump
// of the same modules out of the emulator, which is how docs/cro-linking.md can
// say the linker was checked against a console's loader rather than against its
// own tests.
//
// A development tool, not part of the runtime. It reads the directory it is named
// and never looks for the workspace.
namespace {

namespace core = hexlith::core;
namespace ctr = hexlith::domain::ctr;
namespace linker = hexlith::domain::linker;

constexpr int Success = 0;
constexpr int Failure = 1;

constexpr std::string_view Usage =
    "usage: hexlith-cro-link <romfs> <module>@<address>,<data>,<bss>...\n"
    "\n"
    "Links the static module, then each module named, in the order given, at the\n"
    "three addresses LoadCRO was given for it: the mapping, the data buffer and the\n"
    "BSS buffer, in hexadecimal. The modules are read from <romfs>/<module>.cro.\n"
    "\n"
    "Prints a 'segment' record for each segment of each module, and a 'word'\n"
    "record for each word the loader writes, with its address and value.\n";

constexpr std::string_view StaticModuleFile = "static.crs";
constexpr std::string_view ModuleExtension = ".cro";

void Report(std::string_view message) {
    std::cerr << "hexlith-cro-link: " << message << '\n';
}

// A hexadecimal number, with or without 0x. Written out so that it reads a
// string_view without taking its data pointer.
std::optional<std::uint32_t> ParseHex(std::string_view text) {
    constexpr std::uint64_t Hexadecimal = 16;
    constexpr std::uint64_t LetterValue = 10;
    constexpr std::uint64_t Largest = 0xFFFFFFFF;

    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char digit : text) {
        std::uint64_t place = 0;
        if (digit >= '0' && digit <= '9') {
            place = static_cast<std::uint64_t>(digit - '0');
        } else if (digit >= 'a' && digit <= 'f') {
            place = static_cast<std::uint64_t>(digit - 'a') + LetterValue;
        } else if (digit >= 'A' && digit <= 'F') {
            place = static_cast<std::uint64_t>(digit - 'A') + LetterValue;
        } else {
            return std::nullopt;
        }
        value = (value * Hexadecimal) + place;
        if (value > Largest) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint32_t>(value);
}

struct Request {
    std::string name;
    linker::Placement placement;
};

std::optional<Request> ParseRequest(std::string_view text) {
    const std::size_t at = text.find('@');
    const std::size_t first = text.find(',', at);
    const std::size_t second = text.find(',', first + 1);
    if (at == std::string_view::npos || first == std::string_view::npos ||
        second == std::string_view::npos) {
        return std::nullopt;
    }
    const std::optional<std::uint32_t> module = ParseHex(text.substr(at + 1, first - at - 1));
    const std::optional<std::uint32_t> data = ParseHex(text.substr(first + 1, second - first - 1));
    const std::optional<std::uint32_t> bss = ParseHex(text.substr(second + 1));
    if (!module || !data || !bss) {
        return std::nullopt;
    }
    return Request{.name = std::string{text.substr(0, at)},
                   .placement = linker::Placement{
                       .module_address = *module, .data_address = *data, .bss_address = *bss}};
}

std::vector<std::uint8_t> ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

// The files and the modules read from them. A deque, because the linker keeps
// pointers to the modules and they must not move as more are added.
struct Loaded {
    std::deque<std::vector<std::uint8_t>> files;
    std::deque<linker::CroModule> modules;

    const linker::CroModule* Read(const std::filesystem::path& path) {
        files.push_back(ReadWholeFile(path));
        const core::Result<linker::CroModule> module =
            linker::ReadCroModule(core::ByteSpan{files.back()});
        if (!module) {
            Report(std::format("{}: {}", path.string(), module.Failure().Message()));
            return nullptr;
        }
        modules.push_back(module.Value());
        return &modules.back();
    }
};

void PrintWords(const linker::LoadedModule& loaded,
                const std::vector<linker::LinkedWord>& words,
                std::string_view kind,
                const std::vector<linker::LoadedModule>& all) {
    for (std::size_t index = 0; index < words.size(); ++index) {
        const linker::LinkedWord& word = words.at(index);
        if (!word.value.has_value()) {
            continue;
        }
        std::string resolution;
        if (kind == "external") {
            const linker::Resolution& resolved = loaded.resolutions.at(index);
            resolution = resolved.resolved
                             ? std::format(" from={}", all.at(resolved.exporter).Name())
                             : std::string{" from=unresolved"};
        }
        std::cout << std::format(
            "word {} kind={} segment={} offset={:#010x} address={:#010x} value={:#010x}{}\n",
            loaded.Name(),
            kind,
            word.target.SegmentIndex(),
            word.target.OffsetInSegment(),
            word.address,
            word.value.value_or(0),
            resolution);
    }
}

void Print(const std::vector<linker::LoadedModule>& all) {
    for (const linker::LoadedModule& loaded : all) {
        const ctr::CroHeader& header = loaded.module->header;
        for (std::size_t index = 0; index < header.segments.size(); ++index) {
            const ctr::CroSegment& segment = header.segments.at(index);
            std::cout << std::format(
                "segment {} index={} type={} file-offset={:#010x} size={:#010x} address={:#010x}\n",
                loaded.Name(),
                index,
                segment.type_known ? ctr::Describe(segment.type) : "unknown",
                segment.offset,
                segment.size,
                loaded.segment_addresses.at(index));
        }
        PrintWords(loaded, loaded.internal, "internal", all);
        PrintWords(loaded, loaded.external, "external", all);
    }
}

int Main(std::span<char* const> arguments) {
    if (arguments.size() < 2 || std::string_view{arguments[1]} == "--help") {
        std::cerr << Usage;
        return arguments.size() < 2 ? Failure : Success;
    }

    const std::filesystem::path romfs{arguments[1]};
    Loaded loaded;
    const linker::CroModule* static_module = loaded.Read(romfs / StaticModuleFile);
    if (static_module == nullptr) {
        return Failure;
    }
    core::Result<linker::Linker> linking = linker::Linker::FromStaticModule(*static_module);
    if (!linking) {
        Report(linking.Failure().Message());
        return Failure;
    }

    for (const char* argument : arguments.subspan(2)) {
        const std::optional<Request> request = ParseRequest(argument);
        if (!request) {
            Report(std::format("'{}' is not <module>@<address>,<data>,<bss>", argument));
            return Failure;
        }
        const linker::CroModule* module =
            loaded.Read(romfs / (request->name + std::string{ModuleExtension}));
        if (module == nullptr) {
            return Failure;
        }
        const core::Result<std::size_t> index = linking.Value().Load(*module, request->placement);
        if (!index) {
            Report(index.Failure().Message());
            return Failure;
        }
    }

    Print(linking.Value().Modules());
    return Success;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return Main(std::span<char* const>{argv, static_cast<std::size_t>(argc)});
    } catch (const std::exception& thrown) {
        Report(thrown.what());
        return Failure;
    } catch (...) {
        Report("something was thrown that is not an exception");
        return Failure;
    }
}
