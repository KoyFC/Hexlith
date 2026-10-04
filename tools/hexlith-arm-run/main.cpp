#include "domain/arm/cpu_state.h"
#include "domain/arm/interpreter/interpreter.h"
#include "domain/memory/paged_memory.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <format>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// hexlith-arm-run executes one instruction, or a few, from a state it is given
// and prints the state it leaves.
//
// It exists to be compared with another implementation of the architecture.
// scripts/compare_arm_interpreter.py feeds it hundreds of thousands of cases
// and runs each one in Unicorn as well, which is how docs/arm-interpreter.md can
// say the interpreter was verified rather than merely tested. It is the
// interpreter's counterpart of hexlith-arm-dump.
//
// A development tool, not part of the runtime. It reads cases from standard
// input and never looks for the workspace.
namespace {

namespace arm = hexlith::domain::arm;
namespace memory = hexlith::domain::memory;

constexpr int Success = 0;
constexpr int Failure = 1;

constexpr std::string_view Usage =
    "usage: hexlith-arm-run [steps]\n"
    "\n"
    "Reads one case per line from standard input and prints one result per line.\n"
    "A case is space-separated key=value fields, every number hexadecimal behind\n"
    "0x:\n"
    "\n"
    "  r0 .. r15     the general registers, r15 being where execution starts\n"
    "  cpsr          the flags, Q, GE and T, in their CPSR positions\n"
    "  fpscr         FPSCR\n"
    "  s0 .. s31     the single precision registers\n"
    "  code          the bytes to put at r15, as hexadecimal digit pairs\n"
    "\n"
    "Fields left out are zero. Two regions are mapped writable and filled with a\n"
    "pattern both sides of a comparison can compute: a page of code at 0x00100000\n"
    "and 64 KiB of data at 0x00200000. Byte n of each holds the top byte of\n"
    "n times 0x9E3779B1, counted from the address.\n"
    "\n"
    "A result has the same fields, plus stop and detail, and mem, which lists every\n"
    "byte of the two regions that no longer holds the pattern as address:value.\n"
    "Each case runs the given number of instructions, one by default.\n";

struct Region {
    std::uint32_t base = 0;
    std::uint32_t size = 0;
};

constexpr Region CodeRegion{.base = 0x00100000, .size = 0x1000};
constexpr Region DataRegion{.base = 0x00200000, .size = 0x10000};

// The byte both sides fill unwritten memory with: the top byte of a
// multiplicative hash of the address, so that neighbouring bytes differ and a
// misplaced load reads something recognisably wrong.
std::uint8_t Pattern(std::uint32_t address) {
    constexpr std::uint32_t GoldenRatio = 0x9E3779B1;
    constexpr unsigned TopByte = 24;
    return static_cast<std::uint8_t>((address * GoldenRatio) >> TopByte);
}

std::optional<std::uint64_t> ParseHex(std::string_view text) {
    constexpr unsigned Hexadecimal = 16;
    constexpr unsigned LetterValue = 10;
    if (!text.starts_with("0x") || text.size() == 2) {
        return std::nullopt;
    }
    text.remove_prefix(2);
    std::uint64_t value = 0;
    for (const char character : text) {
        unsigned digit = 0;
        if (character >= '0' && character <= '9') {
            digit = static_cast<unsigned>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            digit = LetterValue + static_cast<unsigned>(character - 'a');
        } else if (character >= 'A' && character <= 'F') {
            digit = LetterValue + static_cast<unsigned>(character - 'A');
        } else {
            return std::nullopt;
        }
        value = (value * Hexadecimal) + digit;
    }
    return value;
}

std::optional<std::uint64_t> ParseDecimal(std::string_view text) {
    constexpr std::uint64_t Decimal = 10;
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        value = (value * Decimal) + static_cast<std::uint64_t>(character - '0');
    }
    return value;
}

std::optional<std::vector<std::uint8_t>> ParseBytes(std::string_view text) {
    if (text.size() % 2 != 0) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes;
    for (std::size_t index = 0; index < text.size(); index += 2) {
        const std::optional<std::uint64_t> byte =
            ParseHex(std::format("0x{}", text.substr(index, 2)));
        if (!byte.has_value()) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<std::uint8_t>(byte.value()));
    }
    return bytes;
}

std::map<std::string, std::string, std::less<>> Fields(std::string_view line) {
    std::map<std::string, std::string, std::less<>> fields;
    while (!line.empty()) {
        const std::size_t space = line.find(' ');
        const std::string_view field = line.substr(0, space);
        const std::size_t equals = field.find('=');
        if (equals != std::string_view::npos) {
            fields.emplace(std::string{field.substr(0, equals)},
                           std::string{field.substr(equals + 1)});
        }
        if (space == std::string_view::npos) {
            break;
        }
        line.remove_prefix(space + 1);
    }
    return fields;
}

std::vector<std::uint8_t> MakePattern(Region region) {
    std::vector<std::uint8_t> bytes(region.size);
    for (std::uint32_t offset = 0; offset < region.size; ++offset) {
        bytes.at(offset) = Pattern(region.base + offset);
    }
    return bytes;
}

const std::vector<std::uint8_t>& PatternOf(Region region) {
    static const std::vector<std::uint8_t> CodePattern = MakePattern(CodeRegion);
    static const std::vector<std::uint8_t> DataPattern = MakePattern(DataRegion);
    return region.base == CodeRegion.base ? CodePattern : DataPattern;
}

// Refills both regions with the pattern, which is cheaper than mapping them
// again for every case.
bool ResetRegions(memory::PagedMemory& space) {
    return space.Load(CodeRegion.base, PatternOf(CodeRegion)) &&
           space.Load(DataRegion.base, PatternOf(DataRegion));
}

// Reads the case into a state and a memory. Returns an error message, or
// nothing when the case was well formed.
std::optional<std::string>
ReadCase(std::string_view line, arm::CpuState& state, memory::PagedMemory& space) {
    const auto fields = Fields(line);
    const auto number = [&fields](const std::string& key) -> std::optional<std::uint64_t> {
        const auto found = fields.find(key);
        if (found == fields.end()) {
            return std::uint64_t{0};
        }
        return ParseHex(found->second);
    };

    for (std::size_t index = 0; index < arm::RegisterCount; ++index) {
        const std::optional<std::uint64_t> value = number(std::format("r{}", index));
        if (!value.has_value()) {
            return std::format("r{} is not a number", index);
        }
        state.registers.at(index) = static_cast<std::uint32_t>(value.value());
    }
    for (std::size_t index = 0; index < arm::SingleRegisterCount; ++index) {
        const std::optional<std::uint64_t> value = number(std::format("s{}", index));
        if (!value.has_value()) {
            return std::format("s{} is not a number", index);
        }
        state.float_registers.at(index) = static_cast<std::uint32_t>(value.value());
    }

    const std::optional<std::uint64_t> cpsr = number("cpsr");
    const std::optional<std::uint64_t> fpscr = number("fpscr");
    if (!cpsr.has_value() || !fpscr.has_value()) {
        return std::string{"cpsr or fpscr is not a number"};
    }
    constexpr std::uint32_t AllUserBytes = 0b1100;
    arm::WriteCpsrFromUserMode(state, static_cast<std::uint32_t>(cpsr.value()), AllUserBytes);
    state.thumb = ((cpsr.value() >> arm::cpsr::ThumbBit) & 1U) != 0;
    state.fpscr = static_cast<std::uint32_t>(fpscr.value());

    if (!ResetRegions(space)) {
        return std::string{"cannot fill the regions"};
    }
    const auto code = fields.find("code");
    if (code != fields.end()) {
        const std::optional<std::vector<std::uint8_t>> bytes = ParseBytes(code->second);
        if (!bytes.has_value() ||
            !space.Load(state.registers.at(arm::ProgramCounter), bytes.value())) {
            return std::string{"the code is not hexadecimal or does not fit"};
        }
    }
    return std::nullopt;
}

std::string Result(const arm::CpuState& state, const memory::PagedMemory& space, arm::Stop stop) {
    // The reason is printed as one word, since a field value holds no spaces.
    std::string reason{arm::Describe(stop.reason)};
    std::ranges::replace(reason, ' ', '-');
    std::string line = std::format("stop={} detail=0x{:X}", reason, stop.detail);
    for (std::size_t index = 0; index < arm::RegisterCount; ++index) {
        line.append(std::format(" r{}=0x{:X}", index, state.registers.at(index)));
    }
    line.append(std::format(" cpsr=0x{:X} fpscr=0x{:X}", arm::PackCpsr(state), state.fpscr));
    for (std::size_t index = 0; index < arm::SingleRegisterCount; ++index) {
        line.append(std::format(" s{}=0x{:X}", index, state.float_registers.at(index)));
    }

    std::string changed;
    for (const Region region : {CodeRegion, DataRegion}) {
        std::vector<std::uint8_t> now(region.size);
        if (!space.Copy(region.base, now)) {
            continue;
        }
        const std::vector<std::uint8_t>& before = PatternOf(region);
        for (std::uint32_t offset = 0; offset < region.size; ++offset) {
            if (now.at(offset) != before.at(offset)) {
                changed.append(std::format("{}0x{:X}:0x{:X}",
                                           changed.empty() ? "" : ",",
                                           region.base + offset,
                                           now.at(offset)));
            }
        }
    }
    line.append(std::format(" mem={}\n", changed.empty() ? "-" : changed));
    return line;
}

int Main(std::span<char* const> arguments) {
    std::uint64_t steps = 1;
    if (arguments.size() > 1) {
        const std::string_view argument{arguments[1]};
        if (argument == "--help") {
            std::cout << Usage;
            return Success;
        }
        const std::optional<std::uint64_t> parsed = ParseDecimal(argument);
        if (!parsed.has_value()) {
            std::cerr << Usage;
            return Failure;
        }
        steps = parsed.value();
    }

    memory::PagedMemory space;
    if (!space.Map(CodeRegion.base, CodeRegion.size, true) ||
        !space.Map(DataRegion.base, DataRegion.size, true)) {
        std::cerr << "hexlith-arm-run: cannot map the regions\n";
        return Failure;
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        arm::CpuState state;
        if (const std::optional<std::string> error = ReadCase(line, state, space);
            error.has_value()) {
            std::cerr << std::format("hexlith-arm-run: {}\n", error.value());
            return Failure;
        }
        const arm::Stop stop = arm::Run(state, space, steps);
        std::cout << Result(state, space, stop);
    }
    return Success;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return Main(std::span<char* const>{argv, static_cast<std::size_t>(argc)});
    } catch (const std::exception& thrown) {
        std::cerr << std::format("hexlith-arm-run: {}\n", thrown.what());
        return Failure;
    } catch (...) {
        std::cerr << "hexlith-arm-run: something was thrown that is not an exception\n";
        return Failure;
    }
}
