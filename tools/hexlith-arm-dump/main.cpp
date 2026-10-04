#include "domain/arm/decode_arm.h"
#include "domain/arm/decode_thumb.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// hexlith-arm-dump prints what the ARM decoder makes of a run of words, one
// record per line.
//
// It exists to be compared with another disassembler. scripts/compare-arm-
// decoder.py runs it over the reference image and over random words and checks
// every record against Capstone, which is how docs/arm-decoder.md can say the
// decoder was verified rather than merely tested. It is also the quickest way to
// look at a stretch of the image by hand.
//
// A development tool, not part of the runtime. It reads a file the caller names
// and never looks for the workspace, so it cannot be pointed at a cartridge by
// accident.
namespace {

namespace arm = hexlith::domain::arm;

constexpr int Success = 0;
constexpr int Failure = 1;

constexpr std::string_view Usage =
    "usage: hexlith-arm-dump [--thumb] <file> [offset] [length] [address]\n"
    "       hexlith-arm-dump [--thumb] -\n"
    "\n"
    "Prints one record per instruction. Offsets and lengths are decimal or\n"
    "0x-prefixed. The address is where the first instruction is taken to live,\n"
    "which is what branch targets are counted from. With - the words are read as\n"
    "hexadecimal from standard input instead of from a file, one per line in ARM\n"
    "and one halfword pair per line in Thumb.\n"
    "\n"
    "With --thumb the input is decoded as Thumb, where an instruction is two bytes\n"
    "unless it is a branch with link, which is four.\n";

// A decimal number, or a hexadecimal one behind 0x. Written out so that it reads
// a string_view without taking its data pointer.
std::optional<std::uint64_t> ParseNumber(std::string_view text) {
    constexpr std::uint64_t Decimal = 10;
    constexpr std::uint64_t Hexadecimal = 16;
    constexpr std::uint64_t LetterValue = 10;

    std::uint64_t base = Decimal;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        base = Hexadecimal;
    }
    if (text.empty()) {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    for (const char character : text) {
        std::uint64_t digit = 0;
        if (character >= '0' && character <= '9') {
            digit = static_cast<std::uint64_t>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            digit = LetterValue + static_cast<std::uint64_t>(character - 'a');
        } else if (character >= 'A' && character <= 'F') {
            digit = LetterValue + static_cast<std::uint64_t>(character - 'A');
        } else {
            return std::nullopt;
        }
        if (digit >= base) {
            return std::nullopt;
        }
        value = (value * base) + digit;
    }
    return value;
}

std::string_view ConditionText(arm::Condition condition) {
    switch (condition) {
    case arm::Condition::Always:
        return "al";
    case arm::Condition::Unconditional:
        return "uncond";
    default:
        break;
    }
    return arm::ConditionSuffix(condition);
}

void Field(std::string& line, std::string_view key, std::string_view value) {
    line.append(std::format(" {}={}", key, value));
}

void RegisterField(std::string& line, std::string_view key, arm::RegisterIndex index) {
    if (index != arm::NoRegister) {
        Field(line, key, arm::RegisterName(index));
    }
}

// Whether the operation has an addressing mode, which is what the pre-indexing,
// direction and write-back fields describe. A branch has none of them and
// printing them beside one would suggest it did.
bool UsesAddressing(arm::Operation operation) {
    switch (operation) {
    case arm::Operation::LoadWord:
    case arm::Operation::LoadByte:
    case arm::Operation::StoreWord:
    case arm::Operation::StoreByte:
    case arm::Operation::LoadWordUnprivileged:
    case arm::Operation::LoadByteUnprivileged:
    case arm::Operation::StoreWordUnprivileged:
    case arm::Operation::StoreByteUnprivileged:
    case arm::Operation::LoadHalf:
    case arm::Operation::StoreHalf:
    case arm::Operation::LoadSignedByte:
    case arm::Operation::LoadSignedHalf:
    case arm::Operation::LoadDouble:
    case arm::Operation::StoreDouble:
    case arm::Operation::LoadMultiple:
    case arm::Operation::StoreMultiple:
    case arm::Operation::LoadCoprocessor:
    case arm::Operation::StoreCoprocessor:
    case arm::Operation::Preload:
    case arm::Operation::StoreReturnState:
    case arm::Operation::ReturnFromException:
        return true;
    default:
        return false;
    }
}

std::string Record(std::uint32_t address, const arm::Instruction& decoded) {
    std::string line = decoded.width == 2
                           ? std::format("0x{:08X} encoding=0x{:04X}", address, decoded.encoding)
                           : std::format("0x{:08X} encoding=0x{:08X}", address, decoded.encoding);
    Field(line, "op", arm::Mnemonic(decoded.operation));
    Field(line, "cond", ConditionText(decoded.condition));

    if (decoded.operation == arm::Operation::Undefined) {
        line.push_back('\n');
        return line;
    }

    if (decoded.updates_flags) {
        Field(line, "flags", "s");
    }

    RegisterField(line, "rd", decoded.destination);
    RegisterField(line, "rdhi", decoded.destination_high);
    RegisterField(line, "rn", decoded.first);
    RegisterField(line, "rm", decoded.second);
    RegisterField(line, "rs", decoded.third);

    switch (decoded.operand) {
    case arm::OperandForm::Immediate:
        Field(line, "imm", std::format("0x{:X}", decoded.immediate));
        break;
    case arm::OperandForm::ShiftedRegister:
        if (decoded.shift_register != arm::NoRegister) {
            Field(line,
                  "shift",
                  std::format("{}:{}",
                              arm::ShiftName(decoded.shift),
                              arm::RegisterName(decoded.shift_register)));
        } else {
            Field(line,
                  "shift",
                  std::format("{}#{}", arm::ShiftName(decoded.shift), decoded.shift_amount));
        }
        break;
    case arm::OperandForm::Register:
    case arm::OperandForm::None:
        break;
    }

    if (decoded.registers != 0) {
        Field(line, "regs", std::format("0x{:04X}", decoded.registers));
    }
    if (arm::HasKnownTarget(decoded)) {
        Field(line, "target", std::format("0x{:08X}", arm::BranchTarget(decoded, address)));
    }
    if (const std::optional<std::uint32_t> pool =
            arm::ProgramCounterRelativeAddress(decoded, address);
        pool.has_value()) {
        Field(line, "reads", std::format("0x{:08X}", pool.value()));
    }

    if (UsesAddressing(decoded.operation)) {
        Field(line, "index", decoded.preindexed ? "pre" : "post");
        Field(line, "offset", decoded.add_offset ? "add" : "sub");
        if (decoded.writes_back) {
            Field(line, "wb", "yes");
        }
    }
    if (decoded.coprocessor != 0) {
        Field(line, "cp", std::format("{}", decoded.coprocessor));
        Field(line, "cpop1", std::format("{}", decoded.coprocessor_opcode1));
        Field(line, "cpop2", std::format("{}", decoded.coprocessor_opcode2));
    }

    line.push_back('\n');
    return line;
}

// One instruction decoded at an address, in whichever encoding the caller asked
// for. In Thumb the second halfword is only read when the first one needs it, and
// the width the decoder reports says how far the next instruction is.
arm::Instruction Decode(std::uint32_t word, bool thumb) {
    if (!thumb) {
        return arm::DecodeArm(word);
    }
    return arm::DecodeThumb(static_cast<std::uint16_t>(word),
                            static_cast<std::uint16_t>(word >> 16U));
}

int DumpStandardInput(bool thumb) {
    std::string text;
    std::uint32_t address = 0;
    while (std::cin >> text) {
        const std::optional<std::uint64_t> word = ParseNumber(text);
        if (!word.has_value()) {
            std::cerr << std::format("hexlith-arm-dump: '{}' is not a word\n", text);
            return Failure;
        }
        const arm::Instruction decoded = Decode(static_cast<std::uint32_t>(word.value()), thumb);
        std::cout << Record(address, decoded);

        // One line is one word either way, so the addresses advance by four even
        // in Thumb, where a line holds a pair of halfwords.
        address += 4;
    }
    return Success;
}

int Main(std::span<char* const> arguments_in) {
    std::vector<std::string_view> arguments{arguments_in.begin() + 1, arguments_in.end()};

    bool thumb = false;
    if (!arguments.empty() && arguments.front() == "--thumb") {
        thumb = true;
        arguments.erase(arguments.begin());
    }

    if (arguments.empty() || arguments.front() == "--help") {
        std::cout << Usage;
        return arguments.empty() ? Failure : Success;
    }

    if (arguments.front() == "-") {
        return DumpStandardInput(thumb);
    }

    std::ifstream file{std::string{arguments.front()}, std::ios::binary};
    if (!file) {
        std::cerr << std::format("hexlith-arm-dump: cannot read {}\n", arguments.front());
        return Failure;
    }

    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file),
                                          std::istreambuf_iterator<char>()};

    std::uint64_t offset = 0;
    std::uint64_t length = bytes.size();
    std::uint64_t address = 0;
    const std::array<std::uint64_t*, 3> numbers = {&offset, &length, &address};

    for (std::size_t index = 1; index < arguments.size() && index <= numbers.size(); ++index) {
        const std::optional<std::uint64_t> value = ParseNumber(arguments[index]);
        if (!value.has_value()) {
            std::cerr << std::format("hexlith-arm-dump: '{}' is not a number\n", arguments[index]);
            return Failure;
        }
        *numbers.at(index - 1) = value.value();
    }

    if (offset > bytes.size()) {
        std::cerr << "hexlith-arm-dump: the offset is past the end of the file\n";
        return Failure;
    }
    length = std::min(length, bytes.size() - offset);

    const std::uint64_t smallest = thumb ? 2 : 4;

    std::string out;
    for (std::uint64_t at = 0; at + smallest <= length;) {
        const auto byte = static_cast<std::size_t>(offset + at);

        // Four bytes are read even in Thumb, because a branch with link needs the
        // halfword after it. Near the end of the range the missing bytes read as
        // zero, which decodes as no pair at all rather than as an invented one.
        std::uint32_t word = 0;
        for (unsigned index = 0; index < 4; ++index) {
            if (byte + index < bytes.size() && at + index < length) {
                word |= static_cast<std::uint32_t>(bytes[byte + index]) << (8U * index);
            }
        }

        const arm::Instruction decoded = Decode(word, thumb);
        out.append(Record(static_cast<std::uint32_t>(address + at), decoded));

        // A Thumb instruction is two bytes unless it is a branch with link, so
        // the decoder's answer is what says where the next one starts. Walking an
        // instruction stream is the only way to know, which is the whole problem
        // the analysis stage exists to solve.
        at += decoded.width;

        // Written in blocks so that a sweep of a megabyte does not build a
        // string of a hundred megabytes first.
        if (out.size() > (1U << 16U)) {
            std::cout << out;
            out.clear();
        }
    }
    std::cout << out;

    return Success;
}

}  // namespace

int main(int argc, char** argv) {
    // Nothing below here throws as a matter of course. A std::bad_alloc on a
    // file of several megabytes is the case this catches, and it deserves a
    // sentence rather than a terminate.
    try {
        return Main(std::span<char* const>{argv, static_cast<std::size_t>(argc)});
    } catch (const std::exception& thrown) {
        std::cerr << std::format("hexlith-arm-dump: {}\n", thrown.what());
        return Failure;
    } catch (...) {
        std::cerr << "hexlith-arm-dump: something was thrown that is not an exception\n";
        return Failure;
    }
}
